// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/registered_pool.hpp"

#include <algorithm>
#include <cstring>

#include "sys/vm.hpp"

namespace mem {

namespace {
std::size_t classOf(std::size_t bytes, std::size_t minShift) {
    std::size_t shift = minShift;
    while ((std::size_t{1} << shift) < bytes)
        ++shift;
    return shift - minShift;
}
}  // namespace

RegisteredPool::RegisteredPool(Registration registration, Limits limits) : registration_(registration), limits_(limits) {
    if (limits_.maxBlocks == 0)
        limits_.maxBlocks = 1;
    const sys::VmRegion r = sys::vmMap(limits_.maxBlocks * sizeof(Info));
    all_ = static_cast<Info*>(r.base);
    tableBytes_ = r.bytes;
}

RegisteredPool::~RegisteredPool() {
    for (std::size_t i = 0; i < count_; ++i) {
        void* host = reinterpret_cast<void*>(all_[i].host);
        if (all_[i].device && registration_.unregisterMemory)
            registration_.unregisterMemory(registration_.ctx, host, all_[i].bytes);
        sys::vmUnmap(host, all_[i].bytes);
    }
    sys::vmUnmap(all_, tableBytes_);
}

const void* RegisteredPool::device(const void* host) const {
    const auto address = reinterpret_cast<std::uintptr_t>(host);
    std::lock_guard<std::mutex> lock(mutex_);
    const Info* begin = all_;
    const Info* end = all_ + count_;
    const Info* after = std::upper_bound(begin, end, address, [](std::uintptr_t a, const Info& b) { return a < b.host; });
    if (after == begin)
        return nullptr;
    const Info& block = *(after - 1);
    if (address - block.host >= block.bytes || !block.device)
        return nullptr;
    return static_cast<const std::uint8_t*>(block.device) + (address - block.host);
}

void* RegisteredPool::take(std::size_t bytes) {
    if (bytes == 0)
        bytes = 1;
    const std::size_t cls = classOf(bytes, kMinShift);
    if (cls >= kClasses)
        return nullptr;
    const std::size_t size = std::size_t{1} << (cls + kMinShift);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (void* p = freeHead_[cls]) {
            freeHead_[cls] = *static_cast<void**>(p);
            idleBytes_ -= size;
            inUse_.fetch_add(1, std::memory_order_relaxed);
            return p;
        }
        // Growing: the budget is claimed before anything is mapped or registered, and given back on failure.
        if ((limits_.maxBytes && registeredBytes_ + reservedBytes_ + size > limits_.maxBytes) || count_ + 1 > limits_.maxBlocks) {
            ++refusals_;
            return nullptr;
        }
        reservedBytes_ += size;
    }
    void* p = sys::tryVmMap(size).base;
    void* device = nullptr;
    if (p && registration_) {
        device = registration_.registerMemory(registration_.ctx, p, size);
        if (!device) {
            sys::vmUnmap(p, size);
            p = nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reservedBytes_ -= size;
    if (!p)
        return nullptr;
    registeredBytes_ += size;
    peakBytes_ = std::max(peakBytes_, registeredBytes_);
    const Info block{reinterpret_cast<std::uintptr_t>(p), size, device};
    Info* at = std::upper_bound(all_, all_ + count_, block.host, [](std::uintptr_t a, const Info& b) { return a < b.host; });
    std::memmove(at + 1, at, static_cast<std::size_t>(all_ + count_ - at) * sizeof(Info));
    *at = block;
    ++count_;
    inUse_.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void RegisteredPool::give(void* data, std::size_t bytes) {
    if (bytes == 0)
        bytes = 1;
    const std::size_t cls = classOf(bytes, kMinShift);
    constexpr std::size_t kMaxEvict = 64;
    Info evicted[kMaxEvict];
    std::size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        *static_cast<void**>(data) = freeHead_[cls];
        freeHead_[cls] = data;
        idleBytes_ += std::size_t{1} << (cls + kMinShift);
        if (limits_.maxIdleBytes && idleBytes_ > limits_.maxIdleBytes)
            n = evictLocked(limits_.maxIdleBytes, evicted, kMaxEvict);
    }
    inUse_.fetch_sub(1, std::memory_order_relaxed);
    retire(evicted, n);
}

std::size_t RegisteredPool::evictLocked(std::size_t keep, Info* out, std::size_t cap) {
    std::size_t n = 0;
    for (std::size_t cls = kClasses; cls-- > 0 && idleBytes_ > keep;) {
        while (freeHead_[cls] && idleBytes_ > keep && n < cap) {
            const std::size_t size = std::size_t{1} << (cls + kMinShift);
            void* p = freeHead_[cls];
            freeHead_[cls] = *static_cast<void**>(p);
            const auto host = reinterpret_cast<std::uintptr_t>(p);
            Info* at = std::lower_bound(all_, all_ + count_, host, [](const Info& b, std::uintptr_t a) { return b.host < a; });
            out[n++] = *at;
            std::memmove(at, at + 1, static_cast<std::size_t>(all_ + count_ - at - 1) * sizeof(Info));   // out of the table first
            --count_;
            idleBytes_ -= size;
            registeredBytes_ -= size;
            ++evicted_;
        }
    }
    return n;
}

void RegisteredPool::retire(const Info* blocks, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        void* host = reinterpret_cast<void*>(blocks[i].host);
        if (blocks[i].device && registration_.unregisterMemory)
            registration_.unregisterMemory(registration_.ctx, host, blocks[i].bytes);
        sys::vmUnmap(host, blocks[i].bytes);
    }
}

std::size_t RegisteredPool::trim(std::size_t keepIdleBytes) {
    std::size_t freed = 0;
    for (;;) {
        constexpr std::size_t kMaxEvict = 64;
        Info evicted[kMaxEvict];
        std::size_t n;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            n = evictLocked(keepIdleBytes, evicted, kMaxEvict);
        }
        for (std::size_t i = 0; i < n; ++i)
            freed += evicted[i].bytes;
        retire(evicted, n);
        if (n < kMaxEvict)
            return freed;
    }
}

RegisteredPool::Stats RegisteredPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out;
    out.blocks = count_;
    out.bytes = registeredBytes_;
    out.inUseBlocks = inUse_.load(std::memory_order_relaxed);
    out.idleBytes = idleBytes_;
    out.peakBytes = peakBytes_;
    out.refusals = refusals_;
    out.evictedBlocks = evicted_;
    return out;
}

}  // namespace mem
