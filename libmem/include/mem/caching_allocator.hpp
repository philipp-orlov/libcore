// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Best-fit caching allocator for expensive, coarse backing memory (cudaMalloc, mmap, Metal heaps) with block
// splitting and neighbour coalescing -- the design PyTorch's CUDA allocator made popular, with the host-heap
// churn removed:
//   * block headers, segment records and the free index are fixed arrays reserved once (virtual memory), so the
//     allocator never calls malloc/new after construction;
//   * the free index is a set of bins (size / granule) plus a bitmap of non-empty bins: best fit in O(words);
//   * a retention cap (maxCachedBytes) releases fully idle segments, oldest first; a hard budget makes allocate()
//     free idle segments and retry before it fails;
//   * one pool per stream (blocks are only reused by the stream that freed them) and a deferred-free list for
//     blocks used by other streams until their events are done (reclaimDeferred).
// Not thread safe by itself: wrap in a mutex (cortex does) or use one per device/thread.
#pragma once

#include <cstddef>
#include <cstdint>

#include "mem/hooks.hpp"
#include "mem/stats.hpp"

namespace mem {

struct Segment {
    void* base = nullptr;
    std::size_t bytes = 0;
};

// What the allocator needs from its backing store. Plain function pointers so the core is not a template.
struct BackingOps {
    Segment (*allocate)(void* ctx, std::size_t bytes) = nullptr;   // {nullptr,0} on failure
    void (*release)(void* ctx, Segment segment) = nullptr;
    void* ctx = nullptr;
};

// Backing from anonymous virtual memory (2 MiB aligned segments).
BackingOps hostVmBacking() noexcept;

struct CachingOptions {
    std::size_t smallLimit = std::size_t{1} << 20;      // requests <= this use the small pool (512 B granule, 2 MiB segments)
    std::size_t smallGranule = 512;
    std::size_t smallSegment = std::size_t{2} << 20;
    std::size_t largeGranule = std::size_t{2} << 20;    // larger requests round up to this (CUDA allocation granularity)
    std::size_t splitRemainderMin = std::size_t{1} << 20;   // a large block is split only if the rest is at least this
    std::size_t maxCachedBytes = 0;                     // retention cap on idle bytes, 0 = unlimited
    std::size_t budgetBytes = 0;                        // hard cap on bytes taken from the backing, 0 = unlimited
    unsigned maxStreams = 8;
    std::size_t maxBlocks = std::size_t{1} << 17;       // block headers reserved up front
    std::size_t maxSegments = 4096;
    Hooks hooks{};
};

class CachingAllocator {
public:
    using StreamId = std::uint32_t;
    struct Block {
        void* ptr = nullptr;
        std::size_t size = 0;            // usable bytes (>= the request)
        std::uint32_t node = 0;
        std::uint32_t generation = 0;
        explicit operator bool() const noexcept { return ptr != nullptr; }
    };

    CachingAllocator(BackingOps backing, CachingOptions options = {});
    ~CachingAllocator();
    CachingAllocator(const CachingAllocator&) = delete;
    CachingAllocator& operator=(const CachingAllocator&) = delete;

    // {nullptr,0} when the budget is hit even after releasing idle segments, or the backing fails.
    Block allocate(std::size_t bytes, StreamId stream = 0) noexcept;
    // The block becomes reusable by `stream` at once. Misuse (stale handle, double free) is reported, not fatal.
    void free(Block block) noexcept;
    // Marks the block as also used on `other`; free() then defers it until reclaimDeferred() says `other` is done.
    void recordUse(Block block, StreamId other) noexcept;
    // `isDone(stream, ctx)` tells whether all work queued on that stream so far has completed. Returns blocks freed.
    std::size_t reclaimDeferred(bool (*isDone)(StreamId stream, void* ctx), void* ctx) noexcept;

    // Returns every fully idle segment to the backing; live blocks are never touched. Returns bytes released.
    std::size_t emptyCache() noexcept;

    struct CacheStats {
        std::uint64_t allocatedBytes = 0;     // handed out and not yet freed
        std::uint64_t cachedBytes = 0;        // held in free blocks
        std::uint64_t segmentBytes = 0;       // allocated + cached == taken from the backing
        std::uint64_t peakAllocatedBytes = 0;
        std::uint64_t peakSegmentBytes = 0;
        std::uint64_t segments = 0;
        std::uint64_t blocksInUse = 0;
        std::uint64_t allocations = 0, frees = 0, backingAllocs = 0, backingFrees = 0, splits = 0, merges = 0;
        std::uint64_t refused = 0, misuse = 0, deferred = 0;
    };
    CacheStats stats() const noexcept;
    // Re-arms the peaks (call between phases when you want per-phase peaks).
    void resetPeaks() noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace mem
