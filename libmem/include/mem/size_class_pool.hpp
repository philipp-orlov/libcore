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
    void trim() noexcept;                           // age out idle large blocks now
    Stats stats() const noexcept;

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

    Block acquireLarge(std::size_t cls) noexcept;
    void releaseLarge(Block block) noexcept;
    std::size_t findLarge(const void* p) const noexcept;    // mu_ held; returns slot or npos
    std::uint32_t insertLarge(const LargeEntry& e) noexcept;// mu_ held; returns the slot
    void eraseLarge(std::size_t slot) noexcept;             // mu_ held
    void ageIdle(std::uint64_t nowNs) noexcept;             // takes mu_, performs syscalls after dropping it
    std::size_t indexHome(const void* p) const noexcept;

    PoolOptions options_;
    ClassTable table_;
    std::size_t firstLargeClass_;
    std::unique_ptr<SlabPool[]> small_;   // never constructed with the vector/heap in steady state
    std::size_t smallCount_ = 0;
    void* smallStorage_ = nullptr;

    // Large blocks.
    std::mutex mu_;
    LargeEntry* entries_ = nullptr;
    IndexEntry* index_ = nullptr;
    std::uint32_t* freeSlots_ = nullptr;   // stack of unused entry slots
    std::size_t freeSlotCount_ = 0, indexMask_ = 0, largeLive_ = 0;
    std::size_t entriesBytes_ = 0, indexBytes_ = 0, freeSlotsBytes_ = 0;
    std::uint32_t idleHead_[ClassTable::kMaxClasses] = {};
    std::size_t idleBytes_ = 0;
    std::atomic<std::uint64_t> lastAgeNs_{0};

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
