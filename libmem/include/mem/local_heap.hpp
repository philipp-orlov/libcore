// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A heap for one owner at a time: an interpreter's cells, a parser's nodes, anything that allocates and frees small
// objects by the million under one lock or on one thread. No atomics, so a pop is a few instructions, and built so
// that a process that runs for months does not shred:
//
//   * Segments of 2 MiB (aligned to their size) are cut into 64 KiB blocks; a block serves one size class, so objects
//     of one size stay together, and a pointer finds its block with a mask. Slots are carved on demand, so a block
//     touches no page it does not use.
//   * Each block has its own free list and the class fills one block at a time. Allocation therefore concentrates on
//     the blocks already in use, while blocks nothing is allocated from drain, and a block that drains goes back to
//     the heap at once, for any class to take. A free list threaded across blocks never lets go like that.
//   * deallocate never makes a system call. trim() is where memory goes back to the OS: pages of idle blocks beyond a
//     small reserve, and segments with nothing live in them. It costs the number of partly free segments, not the
//     number of objects.
//   * A request above kMaxSmall goes to a SizeClassPool, or straight to the OS past that pool's largest class.
//
// The caller passes the size back to deallocate: it knows it, and a header per object would cost more than the rest of
// this file saves. Not thread safe; hold a lock, or use one heap per thread. Blocks may move between threads with the
// usual happens-before of that lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#include "base/platform.hpp"
#include "mem/size_class_pool.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MEM_LOCAL_HEAP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MEM_LOCAL_HEAP_ASAN 1
#endif
#endif
#if defined(MEM_LOCAL_HEAP_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace mem {

class LocalHeap {
public:
    static constexpr std::size_t kGranule = 16;
    static constexpr std::size_t kMaxSmall = 8192;
    static constexpr std::size_t kBlockBytes = std::size_t{64} << 10;
    static constexpr std::size_t kSegmentBytes = std::size_t{2} << 20;

    struct Options {
        SizeClassPool* large = nullptr;     // serves requests above kMaxSmall; null = SizeClassPool::global()
        std::size_t minIdleBlocks = 16;     // idle blocks whose pages trim() always leaves resident
        unsigned idleFraction = 8;          // ... or 1/this of the blocks in use, whichever is more
    };

    struct Stats {
        std::uint64_t mappedBytes = 0;      // address space of the segments
        std::uint64_t committedBytes = 0;   // blocks in use, plus idle blocks whose pages are still resident
        std::uint64_t largeBytes = 0;       // requested from the large pool and not yet returned
        std::uint64_t blocksInUse = 0;
        std::uint64_t idleBlocks = 0;       // resident and free for any class
        std::uint64_t segments = 0;
    };

    LocalHeap() : LocalHeap(Options{}) {}
    explicit LocalHeap(Options options);
    ~LocalHeap();
    LocalHeap(const LocalHeap&) = delete;
    LocalHeap& operator=(const LocalHeap&) = delete;

    // 16-byte aligned. Throws std::bad_alloc when the OS or the large pool refuses.
    BASE_ALWAYS_INLINE void* allocate(std::size_t bytes) {
        if (BASE_UNLIKELY(bytes - 1 >= kMaxSmall))  // 0 wraps around, and is handled as a large request
            return allocateLarge(bytes);
        Class& cls = classes_[classOf(bytes)];
        Block* block = cls.current;
        if (Slot* slot = block->free) {
            block->free = slot->next;
            ++block->live;
            unpoison(slot, block->slotBytes);
            return slot;
        }
        if (block->bump != block->end) {
            void* slot = block->bump;
            block->bump += block->slotBytes;
            ++block->live;
            return slot;
        }
        return allocateSlow(cls);
    }

    // `bytes` is what was passed to allocate. Never makes a system call for a small request.
    BASE_ALWAYS_INLINE void deallocate(void* p, std::size_t bytes) noexcept {
        if (BASE_UNLIKELY(bytes - 1 >= kMaxSmall)) {
            deallocateLarge(p, bytes);
            return;
        }
        Block* block = blockOf(p);
        const bool wasFull = !block->free && block->bump == block->end;
        auto* slot = static_cast<Slot*>(p);
        slot->next = block->free;
        block->free = slot;
        poison(static_cast<char*>(p) + sizeof(Slot), block->slotBytes - sizeof(Slot));
        --block->live;
        if (BASE_UNLIKELY(wasFull || block->live == 0))
            blockChanged(block, wasFull);
    }

    // Gives memory back to the OS; see above. Call at a point where a few system calls do not matter.
    void trim() noexcept;
    Stats stats() const noexcept;
    // The usable size of a request: what a slot really holds.
    static std::size_t usableSize(std::size_t bytes) noexcept { return bytes && bytes <= kMaxSmall ? slotBytesOf(classOf(bytes)) : bytes; }

private:
    struct Slot {
        Slot* next;
    };
    struct Segment;
    struct Block {
        Slot* free = nullptr;
        char* bump = nullptr;      // slots not carved yet: [bump, end)
        char* end = nullptr;
        Block* prev = nullptr;     // partial list of the class
        Block* next = nullptr;
        std::uint32_t live = 0;
        std::uint32_t slotBytes = 0;
        std::uint32_t cls = 0;
    };
    static_assert(sizeof(Block) <= 64, "a block's header must stay on one cache line");
    static constexpr std::size_t kBlockHeader = 64;
    static constexpr std::size_t kClassCount = 48;  // 32 at 16-byte steps up to 512, then four per doubling

    struct Class {
        Block* current;            // never null: an empty sentinel when the class has no block
        Block* partial = nullptr;  // other blocks with a free slot, most recently freed into first
    };

    static BASE_ALWAYS_INLINE unsigned highestBit(std::size_t n) noexcept {
#if defined(_MSC_VER)
        unsigned long index = 0;
        _BitScanReverse64(&index, n);
        return static_cast<unsigned>(index);
#else
        return 63u - static_cast<unsigned>(__builtin_clzll(n));
#endif
    }
    static BASE_ALWAYS_INLINE std::size_t classOf(std::size_t bytes) noexcept {
        if (BASE_LIKELY(bytes <= 512))
            return (bytes + kGranule - 1) / kGranule - 1;
        const std::size_t n = bytes - 1;
        const unsigned msb = highestBit(n);
        return 32 + (msb - 9) * 4 + ((n >> (msb - 2)) & 3);
    }
    static constexpr std::size_t slotBytesOf(std::size_t cls) noexcept {
        if (cls < 32)
            return (cls + 1) * kGranule;
        const std::size_t step = cls - 32;
        return (5 + step % 4) << (9 + step / 4 - 2);
    }
    static BASE_ALWAYS_INLINE Block* blockOf(void* p) noexcept {
        return reinterpret_cast<Block*>(reinterpret_cast<std::uintptr_t>(p) & ~(std::uintptr_t{kBlockBytes} - 1));
    }
    static BASE_ALWAYS_INLINE void poison(void* p, std::size_t bytes) noexcept {
#if defined(MEM_LOCAL_HEAP_ASAN)
        __asan_poison_memory_region(p, bytes);
#else
        (void)p, (void)bytes;
#endif
    }
    static BASE_ALWAYS_INLINE void unpoison(void* p, std::size_t bytes) noexcept {
#if defined(MEM_LOCAL_HEAP_ASAN)
        __asan_unpoison_memory_region(p, bytes);
#else
        (void)p, (void)bytes;
#endif
    }

    void* allocateSlow(Class& cls);
    void* allocateLarge(std::size_t bytes);
    void deallocateLarge(void* p, std::size_t bytes) noexcept;
    void blockChanged(Block* block, bool wasFull) noexcept;
    Block* takeBlock(std::size_t cls);
    void releaseBlock(Block* block) noexcept;
    void unlinkPartial(Class& cls, Block* block) noexcept;
    void pushPartial(Class& cls, Block* block) noexcept;
    Segment* mapSegment();
    void unmapSegment(Segment* segment) noexcept;

    Class classes_[kClassCount];
    Options options_;
    SizeClassPool* large_;
    Segment* segments_ = nullptr;      // every segment
    Segment* withFree_ = nullptr;      // the segments that have a free block
    std::uint64_t segmentCount_ = 0;
    std::uint64_t blocksInUse_ = 0;
    std::uint64_t idleResident_ = 0;
    std::uint64_t largeBytes_ = 0;
};

}  // namespace mem
