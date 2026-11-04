// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A pool of byte buffers in size classes.
//   small classes (<= smallLimit): carved from slabs by one SlabPool per class (see slab_pool.hpp)
//   large classes: one mapping per block; idle blocks are kept warm up to retainLargeBytes, their pages go back to
//                  the OS after releaseIdleAfter (the address range is kept), and the oldest are unmapped beyond
//                  the cap. All syscalls happen with no lock held.
// Misuse (double free, foreign pointer, wrong capacity) is reported through Hooks and never crashes release builds.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "mem/class_table.hpp"
#include "mem/hooks.hpp"
#include "mem/slab_pool.hpp"
#include "mem/stats.hpp"

namespace mem {

struct PoolOptions {
    ClassTable classes = ClassTable::pow2(256, std::size_t{64} << 20);
    std::size_t smallLimit = std::size_t{64} << 10;       // classes up to this size are slab-carved
    std::size_t slabBytes = std::size_t{256} << 10;       // >= 16 x the largest small class
    unsigned shards = 16;
    unsigned threadCacheBlocks = 8;                        // per thread and small class; 0 = off (max 32)
    std::size_t maxSlabsPerClass = 1024;
    std::size_t maxLargeBlocks = 4096;                     // live + idle large mappings, hard cap
    std::size_t maxBytes = 0;                              // 0 = unlimited quota on mapped bytes
    std::size_t retainLargeBytes = std::size_t{64} << 20;  // idle large blocks kept mapped
    std::size_t maxIdlePerLargeClass = 0;                  // 0 = no per-class limit; else idle blocks kept per large class
    std::chrono::milliseconds releaseIdleAfter{250};       // idle large block drops its pages after this
    unsigned borrowClasses = 0;                            // may lend from up to N larger classes when sealed/at quota
    bool verifyBlocks = true;                              // false: skip the per-block double-free/foreign-pointer checks (small classes)
    bool scrubOnRelease = false;                           // zero blocks when released
    Hooks hooks{};
    const char* name = "pool";
};

class SizeClassPool {
public:
    struct Block {
        void* data = nullptr;
        std::size_t capacity = 0;
        explicit operator bool() const noexcept { return data != nullptr; }
    };

    explicit SizeClassPool(PoolOptions options = {});
    ~SizeClassPool();
    SizeClassPool(const SizeClassPool&) = delete;
    SizeClassPool& operator=(const SizeClassPool&) = delete;

    Block acquire(std::size_t minBytes);           // throws std::bad_alloc
    Block tryAcquire(std::size_t minBytes) noexcept;
    void release(Block block) noexcept;

    void prewarm(std::size_t cls, std::size_t blocks) noexcept;
    void seal() noexcept;                           // no new mappings; borrow-or-refuse from now on
    void trim() noexcept;                           // age out idle large blocks now (pages go back, mappings stay)
    void releaseIdleLarge() noexcept;               // unmap every idle large block; small slabs stay
    Stats stats() const noexcept;
    // Bytes held by idle large blocks only (the small classes' free blocks are not counted).
    std::uint64_t idleLargeBytes() const noexcept { return idleBytes_.load(std::memory_order_relaxed); }

    const ClassTable& classes() const noexcept { return table_; }

    // Process-lifetime default pool, intentionally never destroyed.
    static SizeClassPool& global();

private:
    // Entries live in a fixed array and never move, so idle lists can link them by slot number. A second
    // open-addressing table (ptr -> slot, backward-shift deletion) answers "which entry owns this pointer".
    struct LargeEntry {
        void* ptr = nullptr;
        std::size_t bytes = 0;        // mapped size (page rounded class size)
        std::size_t cls = 0;
        std::uint64_t idleSinceNs = 0;
        std::uint32_t nextIdle = 0;   // slot + 1 of the next idle entry of the same class, 0 = none
        bool inUse = false;
        bool aging = false;           // taken off the idle list while its pages are being returned to the OS
        bool discarded = false;       // pages already returned to the OS
    };
    struct IndexEntry {
        void* ptr = nullptr;
        std::uint32_t slot = 0;
    };

    // Large blocks are striped. Each thread has a home stripe (round robin, like the shards of the pools this one
    // replaced); a block is mapped into, and returned to, the stripe of the thread that mapped it, and a lock-free
    // radix map (64 KiB granules -> stripe) tells release which stripe that is. acquire takes from the home stripe
    // and only locks another stripe when its idle count for the class is non-zero. Addresses above 48 bits fall back
    // to a hash of the address.
    struct alignas(64) LargeStripe {
        std::mutex mu;
        LargeEntry* entries = nullptr;
        IndexEntry* index = nullptr;
        std::uint32_t* freeSlots = nullptr;   // stack of returned entry slots
        std::size_t freeSlotCount = 0;
        std::uint32_t freshSlots = 0;         // slots below this have been used at least once
        std::uint32_t idleHead[ClassTable::kMaxClasses] = {};
        std::atomic<std::uint32_t> idleCount[ClassTable::kMaxClasses] = {};   // written under mu, read without it
        Counters counters;                    // acquires / releases / in use / peak of this stripe
    };

    Block acquireLarge(std::size_t cls) noexcept;
    void releaseLarge(Block block) noexcept;
    std::size_t stripeOf(const void* p) const noexcept;        // stripe that owns a block (radix or hash)
    std::size_t placeStripe(const void* p) const noexcept;     // stripe for a new mapping by this thread
    void setOwner(const void* p, std::size_t stripe) noexcept; // radix map write (0 = none)
    bool popIdle(LargeStripe& st, std::size_t cls, Block& out) noexcept;   // st.mu held
    std::size_t findLarge(const LargeStripe& st, const void* p) const noexcept;    // st.mu held; slot or npos
    std::uint32_t insertLarge(LargeStripe& st, const LargeEntry& e) noexcept;      // st.mu held; returns the slot
    void eraseLarge(LargeStripe& st, std::size_t slot) noexcept;                   // st.mu held
    void ageIdle(std::uint64_t nowNs) noexcept;             // takes stripe locks, performs syscalls after dropping them
    std::size_t indexHome(const void* p) const noexcept;

    PoolOptions options_;
    ClassTable table_;
    std::size_t firstLargeClass_;
    std::unique_ptr<SlabPool[]> small_;   // never constructed with the vector/heap in steady state
    std::size_t smallCount_ = 0;
    void* smallStorage_ = nullptr;

    // Large blocks.
    LargeStripe* stripes_ = nullptr;      // off-heap (vmMap), stripeCount_ entries
    std::atomic<std::atomic<std::uint8_t>*>* radix_ = nullptr;   // [1 << 16] leaves of [1 << 16] stripe+1 bytes
    std::size_t radixBytes_ = 0, radixTop_ = 0;
    unsigned radixShift_ = 12;
    std::size_t stripeCount_ = 1, stripeMask_ = 0, stripeBytes_ = 0;
    std::size_t entriesPerStripe_ = 0, indexMask_ = 0;
    std::size_t entriesBytes_ = 0, indexBytes_ = 0, freeSlotsBytes_ = 0;   // per stripe
    alignas(64) std::atomic<std::size_t> largeLive_{0};
    std::atomic<std::size_t> idleBytes_{0};                                     // == counters_.retainedBytes
    std::atomic<std::uint32_t> idleCount_[ClassTable::kMaxClasses] = {};       // all stripes; for maxIdlePerLargeClass
    alignas(64) std::atomic<std::uint64_t> lastAgeNs_{0};

    Counters counters_;
    std::atomic<std::uint64_t> mapped_{0};
    std::atomic<bool> sealed_{false};
};

// Move-only lease: returns itself to the pool in the destructor.
class Buffer {
public:
    Buffer() = default;
    Buffer(SizeClassPool& pool, std::size_t minBytes) : pool_(&pool), block_(pool.acquire(minBytes)) {}
    Buffer(Buffer&& o) noexcept : pool_(o.pool_), block_(o.block_) { o.pool_ = nullptr; o.block_ = {}; }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) {
            reset();
            pool_ = o.pool_;
            block_ = o.block_;
            o.pool_ = nullptr;
            o.block_ = {};
        }
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { reset(); }

    std::uint8_t* data() const noexcept { return static_cast<std::uint8_t*>(block_.data); }
    std::size_t capacity() const noexcept { return block_.capacity; }
    explicit operator bool() const noexcept { return block_.data != nullptr; }
    void reset() noexcept {
        if (pool_ && block_.data)
            pool_->release(block_);
        pool_ = nullptr;
        block_ = {};
    }

private:
    SizeClassPool* pool_ = nullptr;
    SizeClassPool::Block block_{};
};

}  // namespace mem
