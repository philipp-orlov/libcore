// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/size_class_pool.hpp"

#include <cstring>
#include <new>

#include "base/status.hpp"
#include "sys/proc.hpp"
#include "sys/vm.hpp"

namespace mem {

namespace {
constexpr std::size_t kNone = ~std::size_t{0};
constexpr std::size_t kMaxPending = 64;
}  // namespace

SizeClassPool::SizeClassPool(PoolOptions options) : options_(std::move(options)), table_(options_.classes) {
    firstLargeClass_ = table_.count();
    for (std::size_t c = 0; c < table_.count(); ++c)
        if (table_.bytesOf(c) > options_.smallLimit) {
            firstLargeClass_ = c;
            break;
        }
    smallCount_ = firstLargeClass_;
    if (smallCount_) {
        // SlabPool is neither movable nor copyable: construct in place in one raw block (setup time only).
        smallStorage_ = ::operator new(sizeof(SlabPool) * smallCount_);
        auto* arr = static_cast<SlabPool*>(smallStorage_);
        std::size_t built = 0;
        try {
            for (; built < smallCount_; ++built) {
                SlabPoolOptions so;
                so.blockBytes = table_.bytesOf(built);
                so.slabBytes = options_.slabBytes;
                while (so.slabBytes < so.blockBytes * 16)
                    so.slabBytes <<= 1;
                so.shards = options_.shards;
                so.maxSlabs = options_.maxSlabsPerClass;
                so.maxBytes = options_.maxBytes;
                so.scrubOnRelease = options_.scrubOnRelease;
                so.verifyBlocks = options_.verifyBlocks;
                so.hooks = options_.hooks;
                so.threadCacheBlocks = options_.threadCacheBlocks;
                so.sharedMapped = &mapped_;
                so.name = options_.name;
                new (arr + built) SlabPool(so);
            }
        } catch (...) {
            for (std::size_t i = 0; i < built; ++i)
                arr[i].~SlabPool();
            ::operator delete(smallStorage_);
            throw;
        }
    }
    const std::size_t maxLarge = options_.maxLargeBlocks ? options_.maxLargeBlocks : 1;
    std::size_t cap = 16;
    while (cap < maxLarge * 2)
        cap <<= 1;
    const sys::VmRegion e = sys::vmMap(maxLarge * sizeof(LargeEntry));
    const sys::VmRegion ix = sys::vmMap(cap * sizeof(IndexEntry));
    const sys::VmRegion fs = sys::vmMap(maxLarge * sizeof(std::uint32_t));
    entries_ = static_cast<LargeEntry*>(e.base);   // zero filled == all empty
    index_ = static_cast<IndexEntry*>(ix.base);
    freeSlots_ = static_cast<std::uint32_t*>(fs.base);
    entriesBytes_ = e.bytes;
    indexBytes_ = ix.bytes;
    freeSlotsBytes_ = fs.bytes;
    indexMask_ = cap - 1;
    for (std::size_t i = 0; i < maxLarge; ++i)
        freeSlots_[i] = static_cast<std::uint32_t>(maxLarge - 1 - i);   // pops 0,1,2...
    freeSlotCount_ = maxLarge;
}

SizeClassPool::~SizeClassPool() {
    const std::size_t maxLarge = options_.maxLargeBlocks ? options_.maxLargeBlocks : 1;
    for (std::size_t i = 0; i < maxLarge; ++i)
        if (entries_[i].ptr)
            sys::vmUnmap(entries_[i].ptr, entries_[i].bytes);
    sys::vmUnmap(entries_, entriesBytes_);
    sys::vmUnmap(index_, indexBytes_);
    sys::vmUnmap(freeSlots_, freeSlotsBytes_);
    auto* arr = static_cast<SlabPool*>(smallStorage_);
    for (std::size_t i = 0; i < smallCount_; ++i)
        arr[i].~SlabPool();
    ::operator delete(smallStorage_);
}

SizeClassPool& SizeClassPool::global() {
    static SizeClassPool* pool = new SizeClassPool();  // never destroyed: blocks may outlive static destruction
    return *pool;
}

// ---- large blocks ----

std::size_t SizeClassPool::indexHome(const void* p) const noexcept {
    return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(p) >> 12) * 0x9E3779B97F4A7C15ull >> 20) & indexMask_;
}

std::size_t SizeClassPool::findLarge(const void* p) const noexcept {
    for (std::size_t i = indexHome(p);; i = (i + 1) & indexMask_) {
        if (index_[i].ptr == p)
            return index_[i].slot;
        if (index_[i].ptr == nullptr)
            return kNone;
    }
}

std::uint32_t SizeClassPool::insertLarge(const LargeEntry& e) noexcept {
    const std::uint32_t slot = freeSlots_[--freeSlotCount_];
    entries_[slot] = e;
    std::size_t i = indexHome(e.ptr);
    while (index_[i].ptr != nullptr)
        i = (i + 1) & indexMask_;
    index_[i] = {e.ptr, slot};
    return slot;
}

void SizeClassPool::eraseLarge(std::size_t slot) noexcept {
    void* const ptr = entries_[slot].ptr;
    std::size_t hole = indexHome(ptr);
    while (index_[hole].ptr != ptr)
        hole = (hole + 1) & indexMask_;
    for (std::size_t j = hole;;) {
        j = (j + 1) & indexMask_;
        if (index_[j].ptr == nullptr)
            break;
        const std::size_t home = indexHome(index_[j].ptr);
        const bool stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (stays)
            continue;
        index_[hole] = index_[j];
        hole = j;
    }
    index_[hole] = IndexEntry{};
    entries_[slot] = LargeEntry{};
    freeSlots_[freeSlotCount_++] = static_cast<std::uint32_t>(slot);
}

SizeClassPool::Block SizeClassPool::acquireLarge(std::size_t cls) noexcept {
    const std::size_t capacity = table_.bytesOf(cls);
    {
        std::lock_guard<std::mutex> g(mu_);
        const std::uint32_t ref = idleHead_[cls];
        if (ref) {
            LargeEntry& e = entries_[ref - 1];
            idleHead_[cls] = e.nextIdle;
            e.nextIdle = 0;
            e.inUse = true;
            idleBytes_ -= e.bytes;
            counters_.retainedBytes.fetch_sub(e.bytes, std::memory_order_relaxed);
            counters_.noteAcquire();
            return {e.ptr, capacity};
        }
        if (sealed_.load(std::memory_order_acquire) || largeLive_ >= options_.maxLargeBlocks ||
            (options_.maxBytes != 0 && mapped_.load(std::memory_order_relaxed) + sys::roundUpToPage(capacity) > options_.maxBytes)) {
            counters_.refused.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        ++largeLive_;   // reserve the slot while we map without the lock
        mapped_.fetch_add(sys::roundUpToPage(capacity), std::memory_order_relaxed);
    }
    const sys::VmRegion r = sys::tryVmMap(capacity);   // syscall outside the lock
    std::lock_guard<std::mutex> g(mu_);
    if (!r.base) {
        --largeLive_;
        mapped_.fetch_sub(sys::roundUpToPage(capacity), std::memory_order_relaxed);
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    counters_.mapCalls.fetch_add(1, std::memory_order_relaxed);
    counters_.mappedBytes.fetch_add(r.bytes, std::memory_order_relaxed);
    LargeEntry e;
    e.ptr = r.base;
    e.bytes = r.bytes;
    e.cls = cls;
    e.inUse = true;
    insertLarge(e);
    counters_.noteAcquire();
    return {r.base, capacity};
}

void SizeClassPool::releaseLarge(Block block) noexcept {
    sys::VmRegion toUnmap[kMaxPending];
    std::size_t pending = 0;
    bool misuse = false;
    const char* why = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        const std::size_t i = findLarge(block.data);
        if (i == kNone) {
            misuse = true;
            why = "release of a large block this pool does not own";
        } else if (!entries_[i].inUse) {
            misuse = true;
            why = "double free of a large block";
        } else if (table_.bytesOf(entries_[i].cls) != block.capacity) {
            misuse = true;
            why = "release of a large block with the wrong capacity";
        } else {
            LargeEntry& e = entries_[i];
            if (options_.scrubOnRelease)
                std::memset(e.ptr, 0, block.capacity);
            counters_.noteRelease();
            if (idleBytes_ + e.bytes <= options_.retainLargeBytes) {
                e.inUse = false;
                e.discarded = false;
                e.idleSinceNs = sys::monotonicNs();
                e.nextIdle = idleHead_[e.cls];
                idleHead_[e.cls] = static_cast<std::uint32_t>(i + 1);
                idleBytes_ += e.bytes;
                counters_.retainedBytes.fetch_add(e.bytes, std::memory_order_relaxed);
            } else {
                // Over the retention cap: unmap this block (after the lock is dropped).
                toUnmap[pending++] = {e.ptr, e.bytes};
                counters_.mappedBytes.fetch_sub(e.bytes, std::memory_order_relaxed);
                counters_.unmapCalls.fetch_add(1, std::memory_order_relaxed);
                mapped_.fetch_sub(e.bytes, std::memory_order_relaxed);
                --largeLive_;
                eraseLarge(i);
            }
        }
    }
    if (misuse) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(options_.hooks, why);
        return;
    }
    for (std::size_t k = 0; k < pending; ++k)
        sys::vmUnmap(toUnmap[k]);
    // Age idle blocks at most every 50 ms, from whichever thread releases.
    const std::uint64_t now = sys::monotonicNs();
    std::uint64_t last = lastAgeNs_.load(std::memory_order_relaxed);
    if (now - last > 50'000'000ull && lastAgeNs_.compare_exchange_strong(last, now, std::memory_order_relaxed))
        ageIdle(now);
}

void SizeClassPool::ageIdle(std::uint64_t nowNs) noexcept {
    // Idle blocks older than the limit lose their pages. A block being aged is first taken off its idle list, so no
    // other thread can acquire it while madvise runs without the lock; it goes back afterwards.
    std::uint32_t taken[kMaxPending];
    std::size_t count = 0;
    const std::uint64_t limit = static_cast<std::uint64_t>(options_.releaseIdleAfter.count()) * 1'000'000ull;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (std::size_t c = firstLargeClass_; c < table_.count() && count < kMaxPending; ++c) {
            std::uint32_t* link = &idleHead_[c];
            while (*link && count < kMaxPending) {
                LargeEntry& e = entries_[*link - 1];
                if (!e.discarded && nowNs - e.idleSinceNs >= limit) {
                    taken[count++] = *link - 1;
                    *link = e.nextIdle;
                    e.nextIdle = 0;
                    e.aging = true;
                } else {
                    link = &e.nextIdle;
                }
            }
        }
    }
    if (count == 0)
        return;
    for (std::size_t k = 0; k < count; ++k)
        sys::vmDiscard(entries_[taken[k]].ptr, entries_[taken[k]].bytes);   // entry is exclusively ours while aging
    std::lock_guard<std::mutex> g(mu_);
    for (std::size_t k = 0; k < count; ++k) {
        LargeEntry& e = entries_[taken[k]];
        e.aging = false;
        e.discarded = true;
        e.nextIdle = idleHead_[e.cls];
        idleHead_[e.cls] = taken[k] + 1;
    }
}

void SizeClassPool::trim() noexcept { ageIdle(~std::uint64_t{0}); }

// ---- public ----

SizeClassPool::Block SizeClassPool::tryAcquire(std::size_t minBytes) noexcept {
    const std::size_t cls = table_.classOf(minBytes == 0 ? 1 : minBytes);
    if (cls == ClassTable::npos) {
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const std::size_t limit = std::min<std::size_t>(table_.count() - 1, cls + options_.borrowClasses);
    for (std::size_t c = cls; c <= limit; ++c) {
        Block b;
        if (c < firstLargeClass_) {
            auto* arr = static_cast<SlabPool*>(smallStorage_);
            void* p = arr[c].tryAcquire();
            if (p)
                b = {p, table_.bytesOf(c)};
        } else {
            b = acquireLarge(c);
        }
        if (b.data) {
            if (c != cls)
                counters_.borrowed.fetch_add(1, std::memory_order_relaxed);
            return b;
        }
    }
    return {};
}

SizeClassPool::Block SizeClassPool::acquire(std::size_t minBytes) {
    Block b = tryAcquire(minBytes);
    if (!b.data) {
        reportExhausted(options_.hooks, options_.name, minBytes);
        throw std::bad_alloc();
    }
    return b;
}

void SizeClassPool::release(Block block) noexcept {
    if (!block.data)
        return;
    const std::size_t cls = table_.classOf(block.capacity);
    if (cls == ClassTable::npos || table_.bytesOf(cls) != block.capacity) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(options_.hooks, "release with a capacity that is not one of the pool's class sizes");
        return;
    }
    if (cls < firstLargeClass_)
        static_cast<SlabPool*>(smallStorage_)[cls].release(block.data);
    else
        releaseLarge(block);
}

void SizeClassPool::prewarm(std::size_t cls, std::size_t blocks) noexcept {
    if (cls >= table_.count())
        return;
    if (cls < firstLargeClass_) {
        static_cast<SlabPool*>(smallStorage_)[cls].prewarm(blocks);
        return;
    }
    Block held[64];
    std::size_t n = 0;
    for (; n < blocks && n < 64; ++n) {
        held[n] = acquireLarge(cls);
        if (!held[n].data)
            break;
    }
    for (std::size_t i = 0; i < n; ++i)
        releaseLarge(held[i]);
}

Stats SizeClassPool::stats() const noexcept {
    Stats total = counters_.snapshot();   // large blocks + borrow counts
    const auto* arr = static_cast<const SlabPool*>(smallStorage_);
    for (std::size_t c = 0; c < smallCount_; ++c) {
        const Stats t = arr[c].stats();
        total.mappedBytes += t.mappedBytes;
        total.retainedBytes += t.retainedBytes;
        total.blocksInUse += t.blocksInUse;
        total.peakBlocksInUse += t.peakBlocksInUse;
        total.acquires += t.acquires;
        total.releases += t.releases;
        total.mapCalls += t.mapCalls;
        total.unmapCalls += t.unmapCalls;
        total.refused += t.refused;
        total.misuse += t.misuse;
    }
    return total;
}

void SizeClassPool::seal() noexcept {
    sealed_.store(true, std::memory_order_release);
    for (std::size_t c = 0; c < smallCount_; ++c)
        static_cast<SlabPool*>(smallStorage_)[c].seal();
}

}  // namespace mem
