// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/page_class_allocator.hpp"

#include <algorithm>
#include <iterator>

#include "sys/vm.hpp"

namespace mem {

namespace {
std::size_t classOf(std::size_t bytes) {
    std::size_t cls = 0;
    while ((PageClassAllocator::kPage << cls) < bytes)
        ++cls;
    return cls;
}
}  // namespace

PageClassAllocator::PageClassAllocator(std::uint64_t bytes)
    : bytes_((std::max<std::uint64_t>(bytes, kPage) + kPage - 1) / kPage * kPage) {
    const sys::VmRegion r = sys::vmMap(static_cast<std::size_t>(bytes_ / kPage) * sizeof(Entry));
    pages_ = static_cast<Entry*>(r.base);   // zero filled: every entry is kFree
    tableBytes_ = r.bytes;
}

PageClassAllocator::~PageClassAllocator() { sys::vmUnmap(pages_, tableBytes_); }

bool PageClassAllocator::allocate(std::size_t bytes, std::uint64_t& offset) {
    if (bytes == 0)
        bytes = 1;
    if (bytes > bytes_)
        return false;
    const std::size_t cls = classOf(bytes);
    if (cls >= kClasses)
        return false;
    const std::uint64_t size = std::uint64_t{kPage} << cls;
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t at;
    if (head_[cls]) {
        const std::uint32_t index = head_[cls] - 1;
        head_[cls] = pages_[index].next;
        at = std::uint64_t{index} * kPage;
    } else if (bump_ + size <= bytes_) {
        at = bump_;
        bump_ += size;
    } else if (inUse_.load(std::memory_order_relaxed) == 0 && bump_ != 0) {
        // Nothing is Held or Sent (both counted under this mutex), so every carved block is free and none of the
        // cached classes is worth keeping: start over, and a burst of another size finds the whole area. A block
        // released twice is refused as before, since its entry reads kFree.
        std::fill(std::begin(head_), std::end(head_), 0u);
        bump_ = size;
        at = 0;
    } else {
        return false;
    }
    Entry& entry = pages_[static_cast<std::size_t>(at / kPage)];
    entry.cls = static_cast<std::uint8_t>(cls);
    entry.state = kHeld;
    entry.next = 0;
    inUse_.fetch_add(size, std::memory_order_relaxed);
    offset = at;
    return true;
}

bool PageClassAllocator::markSent(std::uint64_t offset) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset % kPage != 0 || offset >= bytes_)
        return false;
    Entry& entry = pages_[static_cast<std::size_t>(offset / kPage)];
    if (entry.state != kHeld)
        return false;
    entry.state = kSent;
    return true;
}

bool PageClassAllocator::release(std::uint64_t offset, std::uint8_t expected) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset % kPage != 0 || offset >= bytes_)
        return false;
    const std::size_t index = static_cast<std::size_t>(offset / kPage);
    Entry& entry = pages_[index];
    if (entry.state != expected)
        return false;
    entry.state = kFree;
    entry.next = head_[entry.cls];
    head_[entry.cls] = static_cast<std::uint32_t>(index + 1);
    inUse_.fetch_sub(std::uint64_t{kPage} << entry.cls, std::memory_order_relaxed);
    return true;
}

void PageClassAllocator::releaseHeld(std::uint64_t offset) noexcept { release(offset, kHeld); }
bool PageClassAllocator::releaseSent(std::uint64_t offset) noexcept { return release(offset, kSent); }

}  // namespace mem
