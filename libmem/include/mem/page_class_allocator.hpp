// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Allocator over a fixed region that something else owns -- a sealed shared-memory file, a registered device buffer.
// It manages offsets only (the memory itself is never touched, which is the point: the peer reads it), so the side
// table that says which pages are free, held by the application or sent to the peer lives in its own storage.
// Blocks are power-of-two multiples of the page size, carved front to back; a freed block goes to the free list of
// its class; when nothing at all is in use the whole region is handed out afresh. Moved from librpc's ClientArena.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mem {

class PageClassAllocator {
public:
    static constexpr std::size_t kPage = 4096;

    explicit PageClassAllocator(std::uint64_t bytes);
    ~PageClassAllocator();
    PageClassAllocator(const PageClassAllocator&) = delete;
    PageClassAllocator& operator=(const PageClassAllocator&) = delete;

    std::uint64_t bytes() const noexcept { return bytes_; }
    // A block of at least `bytes`; false when none is free (never blocks).
    bool allocate(std::size_t bytes, std::uint64_t& offset);
    bool markSent(std::uint64_t offset) noexcept;       // Held -> Sent; false if not Held
    void releaseHeld(std::uint64_t offset) noexcept;    // Held -> free
    bool releaseSent(std::uint64_t offset) noexcept;    // Sent -> free; false if not Sent (a misbehaving peer)
    std::uint64_t inUse() const noexcept { return inUse_.load(std::memory_order_relaxed); }   // bytes Held or Sent

private:
    enum : std::uint8_t { kFree = 0, kHeld = 1, kSent = 2 };
    struct Entry {
        std::uint32_t next = 0;   // free list: index + 1 of the next free block of the class, 0 = end
        std::uint8_t cls = 0;
        std::uint8_t state = kFree;
    };
    static constexpr std::size_t kClasses = 53;

    bool release(std::uint64_t offset, std::uint8_t expected) noexcept;

    std::uint64_t bytes_;
    std::mutex mutex_;
    Entry* pages_ = nullptr;           // one per page, from sys::vmMap
    std::size_t tableBytes_ = 0;
    std::uint32_t head_[kClasses] = {};
    std::uint64_t bump_ = 0;
    std::atomic<std::uint64_t> inUse_{0};
};

}  // namespace mem
