// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Fixed-size blocks carved from large aligned slabs.
//
//  * Slabs are mapped with sys::vmMapAligned, aligned to their own size, so the slab of any pointer is found with a
//    mask. A fixed-size lock-free hash of slab bases (allocated once, outside the heap) says which masked addresses
//    really are slabs, so a foreign pointer is rejected without being dereferenced.
//  * Every slab header carries a bitmap of allocated blocks: a double free or a pointer into the middle of a block
//    is detected in all builds, never corrupts the free list, and is reported through Hooks.
//  * The free list is threaded through the first 8 bytes of free blocks; per-shard locks, thread -> shard by a
//    thread_local round-robin id; an empty shard steals a batch from a sibling before it maps another slab.
//  * Nothing here allocates from the process heap after construction, and no syscall runs under a lock.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "mem/hooks.hpp"
#include "mem/stats.hpp"

namespace mem {

struct SlabPoolOptions {
    std::size_t blockBytes = 256;          // multiple of 64
    std::size_t slabBytes = 256u << 10;    // power of two, >= 16 * blockBytes
    unsigned shards = 16;
    std::size_t maxSlabs = 1024;           // registry capacity: a hard cap on slabs of this pool
    std::size_t maxBytes = 0;              // 0 = unlimited; counted on `shared` (or on the pool's own counters)
    unsigned threadCacheBlocks = 8;        // blocks cached per thread (0 = off, max 32): the lock-free fast path
    bool verifyBlocks = true;              // bitmap + registry checks on every acquire/release (double free, foreign pointer)
    bool scrubOnRelease = false;           // zero released blocks (tenant isolation)
    Hooks hooks{};
    std::atomic<std::uint64_t>* sharedMapped = nullptr;  // mapped-bytes quota counter shared with a parent pool
    const char* name = "slab";
};

class SlabPool {
public:
    explicit SlabPool(const SlabPoolOptions& options);
    ~SlabPool();
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    // Returns a block of blockBytes() bytes, 64-byte aligned, or nullptr when sealed/over quota/out of memory.
    void* tryAcquire() noexcept;
    // Returns false (and reports misuse) for a pointer this pool does not own, a pointer not at a block start, or
    // a block that is not currently allocated; the pointer is left alone in that case.
    bool release(void* p) noexcept;

    // No new slabs from now on (existing free blocks keep being served).
    void seal() noexcept { sealed_.store(true, std::memory_order_release); }
    bool sealed() const noexcept { return sealed_.load(std::memory_order_acquire); }
    // Maps slabs until at least `blocks` blocks are free. Returns false if it could not.
    bool prewarm(std::size_t blocks) noexcept;

    std::size_t blockBytes() const noexcept { return blockBytes_; }
    std::size_t blocksPerSlab() const noexcept { return blocksPerSlab_; }
    bool owns(const void* p) const noexcept;
    // Exact for every counter except peakBlocksInUse, which is sampled when thread caches refill or flush (it is
    // within threads x threadCacheBlocks of the true peak).
    Stats stats() const noexcept;
    std::size_t slabCount() const noexcept { return slabs_.load(std::memory_order_relaxed); }

public:
    struct ThreadSlot;                  // implementation detail of the per-thread cache
    struct ThreadCacheAccess;
private:
    struct Slab;
    struct alignas(64) Shard {
        std::mutex mu;
        void* freeHead = nullptr;
        std::size_t freeCount = 0;
        Slab* carve = nullptr;     // slab being handed out front to back
        Slab* reserve = nullptr;   // fully fresh slabs that arrived while carve still had room
    };

    Slab* slabOf(const void* p) const noexcept;
    bool registryInsert(Slab* s) noexcept;
    Slab* mapSlab() noexcept;
    void* takeFromCarve(Shard& sh) noexcept;       // sh.mu held
    void* steal(unsigned self) noexcept;
    std::size_t takeBatch(unsigned shard, void** out, std::size_t want) noexcept;
    std::size_t refillBatch(void** out, std::size_t want) noexcept;   // free list, carve, steal, then a new slab
    void pushToShard(void* first, void* last, std::size_t count) noexcept;
    bool markAllocated(void* p) noexcept;          // sets the block's bit; false = corruption
    bool clearAllocated(void* p) noexcept;         // validates the pointer and clears the bit; false = misuse
    void sampleInUse(ThreadSlot& slot) noexcept;   // folds the slot's counters into the peak sample
    ThreadSlot& bind() noexcept;
    unsigned shardIndex() const noexcept;

    const std::size_t blockBytes_, slabBytes_, blocksPerSlab_, firstBlockOffset_, bitmapWords_;
    const unsigned shardCount_;
    const std::size_t maxSlabs_, maxBytes_;
    const bool scrub_;
    const bool verify_;
    const Hooks hooks_;
    const char* const name_;
    std::unique_ptr<Shard[]> shards_;

    std::atomic<std::uintptr_t>* registry_ = nullptr;  // set in the constructor, never changes   // open-addressing hash of slab bases
    std::size_t registryMask_ = 0, registryBytes_ = 0;
    std::mutex growMu_;                                 // serialises slab creation + registry insertion
    std::atomic<std::size_t> slabs_{0};
    std::atomic<bool> sealed_{false};

    // Slow-path counters (shared across threads) and the per-thread cache bookkeeping.
    Counters counters_;
    std::atomic<std::uint64_t> ownMapped_{0};
    std::atomic<std::uint64_t>* mapped_;
    const unsigned cacheBlocks_;
    const std::uint64_t gen_;                // unique for the life of the process; a stale thread slot never matches
    ThreadSlot* slotHead_ = nullptr;         // registered thread slots, guarded by the global cache mutex
    std::atomic<std::int64_t> retiredInUse_{0};     // acquires - releases of threads that have exited
    std::atomic<std::uint64_t> retiredAcquires_{0}, retiredReleases_{0};
    std::atomic<std::int64_t> syncedInUse_{0};      // in-use as of the last refill/flush, for the peak sample
};

}  // namespace mem
