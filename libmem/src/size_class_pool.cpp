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
// Radix map over 48-bit addresses: granule = largest power of two <= the smallest large class (12..16 bits), so two
// live blocks never start in one granule; leaves cover 16 bits, the top level the rest (at most 20 bits: 8 MiB of
// virtual memory, touched only where blocks are).
constexpr unsigned kLeafBits = 16;
constexpr std::size_t kRadixLeaf = std::size_t{1} << kLeafBits;
constexpr unsigned kAddressBits = 48;
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
    // Stripes: a power of two no larger than `shards` (max 16). Each stripe can hold every large block (the live
    // cap is enforced across stripes), so a skewed address hash never fails an insert; the tables are virtual
    // memory and only the touched pages cost RSS.
    unsigned want = options_.shards ? options_.shards : 1;
    if (want > 16)
        want = 16;
    while (stripeCount_ * 2 <= want)
        stripeCount_ *= 2;
    stripeMask_ = stripeCount_ - 1;
    const std::size_t maxLarge = options_.maxLargeBlocks ? options_.maxLargeBlocks : 1;
    entriesPerStripe_ = maxLarge;
    std::size_t cap = 16;
    while (cap < maxLarge * 2)
        cap <<= 1;
    indexMask_ = cap - 1;
    radixShift_ = 12;
    if (firstLargeClass_ < table_.count())
        while (radixShift_ < 16 && (std::size_t{1} << (radixShift_ + 1)) <= table_.bytesOf(firstLargeClass_))
            ++radixShift_;
    radixTop_ = std::size_t{1} << (kAddressBits - kLeafBits - radixShift_);
    const sys::VmRegion rt = sys::vmMap(radixTop_ * sizeof(std::atomic<std::atomic<std::uint8_t>*>));
    radix_ = static_cast<std::atomic<std::atomic<std::uint8_t>*>*>(rt.base);   // zero == no leaf
    radixBytes_ = rt.bytes;
    const sys::VmRegion sr = sys::vmMap(stripeCount_ * sizeof(LargeStripe));
    stripes_ = static_cast<LargeStripe*>(sr.base);
    stripeBytes_ = sr.bytes;
    for (std::size_t k = 0; k < stripeCount_; ++k) {
        LargeStripe* st = new (stripes_ + k) LargeStripe();
        const sys::VmRegion e = sys::vmMap(maxLarge * sizeof(LargeEntry));
        const sys::VmRegion ix = sys::vmMap(cap * sizeof(IndexEntry));
        const sys::VmRegion fs = sys::vmMap(maxLarge * sizeof(std::uint32_t));
        st->entries = static_cast<LargeEntry*>(e.base);   // zero filled == all empty
        st->index = static_cast<IndexEntry*>(ix.base);
        st->freeSlots = static_cast<std::uint32_t*>(fs.base);
        entriesBytes_ = e.bytes;
        indexBytes_ = ix.bytes;
        freeSlotsBytes_ = fs.bytes;
        for (std::size_t i = 0; i < maxLarge; ++i)
            st->freeSlots[i] = static_cast<std::uint32_t>(maxLarge - 1 - i);   // pops 0,1,2...
        st->freeSlotCount = maxLarge;
    }
}

SizeClassPool::~SizeClassPool() {
    for (std::size_t k = 0; k < stripeCount_; ++k) {
        LargeStripe& st = stripes_[k];
        for (std::size_t i = 0; i < entriesPerStripe_; ++i)
            if (st.entries[i].ptr)
                sys::vmUnmap(st.entries[i].ptr, st.entries[i].bytes);
        sys::vmUnmap(st.entries, entriesBytes_);
        sys::vmUnmap(st.index, indexBytes_);
        sys::vmUnmap(st.freeSlots, freeSlotsBytes_);
        st.~LargeStripe();
    }
    sys::vmUnmap(stripes_, stripeBytes_);
    for (std::size_t t = 0; t < radixTop_; ++t)
        if (auto* leaf = radix_[t].load(std::memory_order_relaxed))
            sys::vmUnmap(leaf, kRadixLeaf);
    sys::vmUnmap(radix_, radixBytes_);
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

namespace {
std::atomic<unsigned> g_nextHome{0};
thread_local unsigned t_home = ~0u;                     // trivially destructible: safe during thread teardown

unsigned homeOfThisThread() noexcept {
    if (t_home == ~0u)
        t_home = g_nextHome.fetch_add(1, std::memory_order_relaxed);
    return t_home;
}

inline bool radixCovers(const void* p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) >> kAddressBits) == 0;
}
}  // namespace

std::size_t SizeClassPool::indexHome(const void* p) const noexcept {
    return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(p) >> 12) * 0x9E3779B97F4A7C15ull >> 20) & indexMask_;
}

namespace {
inline std::size_t hashStripe(const void* p, std::size_t mask) noexcept {
    return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(p) >> 12) * 0xC2B2AE3D27D4EB4Full >> 59) & mask;
}
}  // namespace

std::size_t SizeClassPool::placeStripe(const void* p) const noexcept {
    return radixCovers(p) ? (homeOfThisThread() & stripeMask_) : hashStripe(p, stripeMask_);
}

std::size_t SizeClassPool::stripeOf(const void* p) const noexcept {
    if (!radixCovers(p))
        return hashStripe(p, stripeMask_);
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    const std::atomic<std::uint8_t>* leaf = radix_[a >> (radixShift_ + kLeafBits)].load(std::memory_order_acquire);
    if (!leaf)
        return kNone;
    const std::uint8_t v = leaf[(a >> radixShift_) & (kRadixLeaf - 1)].load(std::memory_order_acquire);
    return v ? std::size_t{v} - 1u : kNone;
}

void SizeClassPool::setOwner(const void* p, std::size_t stripe) noexcept {
    if (!radixCovers(p))
        return;
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    std::atomic<std::atomic<std::uint8_t>*>& top = radix_[a >> (radixShift_ + kLeafBits)];
    std::atomic<std::uint8_t>* leaf = top.load(std::memory_order_acquire);
    if (!leaf) {
        const sys::VmRegion r = sys::tryVmMap(kRadixLeaf);   // zero == no owner; kept until the pool dies
        if (!r.base)
            return;   // stripeOf then reports "not owned": the caller checks the map succeeded first (see acquireLarge)
        auto* fresh = static_cast<std::atomic<std::uint8_t>*>(r.base);
        if (top.compare_exchange_strong(leaf, fresh, std::memory_order_acq_rel))
            leaf = fresh;
        else
            sys::vmUnmap(r.base, r.bytes);   // another thread installed one first
    }
    leaf[(a >> radixShift_) & (kRadixLeaf - 1)].store(static_cast<std::uint8_t>(stripe == kNone ? 0 : stripe + 1),
                                                      std::memory_order_release);
}

std::size_t SizeClassPool::findLarge(const LargeStripe& st, const void* p) const noexcept {
    for (std::size_t i = indexHome(p);; i = (i + 1) & indexMask_) {
        if (st.index[i].ptr == p)
            return st.index[i].slot;
        if (st.index[i].ptr == nullptr)
            return kNone;
    }
}

std::uint32_t SizeClassPool::insertLarge(LargeStripe& st, const LargeEntry& e) noexcept {
    const std::uint32_t slot = st.freeSlots[--st.freeSlotCount];
    st.entries[slot] = e;
    std::size_t i = indexHome(e.ptr);
    while (st.index[i].ptr != nullptr)
        i = (i + 1) & indexMask_;
    st.index[i] = {e.ptr, slot};
    return slot;
}

void SizeClassPool::eraseLarge(LargeStripe& st, std::size_t slot) noexcept {
    void* const ptr = st.entries[slot].ptr;
    std::size_t hole = indexHome(ptr);
    while (st.index[hole].ptr != ptr)
        hole = (hole + 1) & indexMask_;
    for (std::size_t j = hole;;) {
        j = (j + 1) & indexMask_;
        if (st.index[j].ptr == nullptr)
            break;
        const std::size_t home = indexHome(st.index[j].ptr);
        const bool stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (stays)
            continue;
        st.index[hole] = st.index[j];
        hole = j;
    }
    st.index[hole] = IndexEntry{};
    setOwner(ptr, kNone);   // before the caller unmaps, so a new mapping at this address starts unowned
    st.entries[slot] = LargeEntry{};
    st.freeSlots[st.freeSlotCount++] = static_cast<std::uint32_t>(slot);
}

bool SizeClassPool::popIdle(LargeStripe& st, std::size_t cls, Block& out) noexcept {
    const std::uint32_t ref = st.idleHead[cls];
    if (!ref)
        return false;
    LargeEntry& e = st.entries[ref - 1];
    st.idleHead[cls] = e.nextIdle;
    e.nextIdle = 0;
    e.inUse = true;
    st.idleCount[cls].store(st.idleCount[cls].load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
    if (options_.maxIdlePerLargeClass != 0)
        idleCount_[cls].fetch_sub(1, std::memory_order_relaxed);
    idleBytes_.fetch_sub(e.bytes, std::memory_order_relaxed);
    st.counters.noteAcquire();
    out = {e.ptr, table_.bytesOf(cls)};
    return true;
}

SizeClassPool::Block SizeClassPool::acquireLarge(std::size_t cls) noexcept {
    const std::size_t capacity = table_.bytesOf(cls);
    // 1. Idle block: the hinted stripe first (locked unconditionally: it is where this thread's own frees went), then
    //    any other stripe whose idle count says it has one.
    const std::size_t home = homeOfThisThread() & stripeMask_;
    for (std::size_t n = 0; n < stripeCount_; ++n) {
        const std::size_t k = (home + n) & stripeMask_;
        LargeStripe& st = stripes_[k];
        if (n != 0 && st.idleCount[cls].load(std::memory_order_relaxed) == 0)
            continue;
        Block b;
        std::lock_guard<std::mutex> g(st.mu);
        if (popIdle(st, cls, b))
            return b;
    }
    // 2. New mapping. The live-count and quota are reserved with atomics, the syscall runs with no lock held.
    const std::size_t pageBytes = sys::roundUpToPage(capacity);
    if (sealed_.load(std::memory_order_acquire)) {
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    if (largeLive_.fetch_add(1, std::memory_order_relaxed) >= options_.maxLargeBlocks) {
        largeLive_.fetch_sub(1, std::memory_order_relaxed);
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const std::uint64_t before = mapped_.fetch_add(pageBytes, std::memory_order_relaxed);
    if (options_.maxBytes != 0 && before + pageBytes > options_.maxBytes) {
        mapped_.fetch_sub(pageBytes, std::memory_order_relaxed);
        largeLive_.fetch_sub(1, std::memory_order_relaxed);
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const sys::VmRegion r = sys::tryVmMap(capacity);
    if (!r.base) {
        mapped_.fetch_sub(pageBytes, std::memory_order_relaxed);
        largeLive_.fetch_sub(1, std::memory_order_relaxed);
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
    const std::size_t k = placeStripe(r.base);
    if (radixCovers(r.base)) {
        setOwner(r.base, k);
        if (stripeOf(r.base) != k) {   // no memory for a radix leaf: give the block back and refuse
            sys::vmUnmap(r);
            counters_.mapCalls.fetch_sub(1, std::memory_order_relaxed);
            counters_.mappedBytes.fetch_sub(r.bytes, std::memory_order_relaxed);
            mapped_.fetch_sub(pageBytes, std::memory_order_relaxed);
            largeLive_.fetch_sub(1, std::memory_order_relaxed);
            counters_.refused.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
    }
    LargeStripe& st = stripes_[k];
    std::lock_guard<std::mutex> g(st.mu);
    insertLarge(st, e);
    st.counters.noteAcquire();
    return {r.base, capacity};
}

void SizeClassPool::releaseLarge(Block block) noexcept {
    sys::VmRegion toUnmap{};
    bool misuse = false;
    const char* why = nullptr;
    const std::size_t k = stripeOf(block.data);
    if (k == kNone) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(options_.hooks, "release of a large block this pool does not own");
        return;
    }
    LargeStripe& st = stripes_[k];
    const std::uint64_t now = sys::monotonicNs();   // one clock read serves the idle stamp and the ageing check
    {
        std::lock_guard<std::mutex> g(st.mu);
        const std::size_t i = findLarge(st, block.data);
        if (i == kNone) {
            misuse = true;
            why = "release of a large block this pool does not own";
        } else if (!st.entries[i].inUse) {
            misuse = true;
            why = "double free of a large block";
        } else if (table_.bytesOf(st.entries[i].cls) != block.capacity) {
            misuse = true;
            why = "release of a large block with the wrong capacity";
        } else {
            LargeEntry& e = st.entries[i];
            if (options_.scrubOnRelease)
                std::memset(e.ptr, 0, block.capacity);
            st.counters.noteRelease();
            // Reserve room under the retention caps; undo if over.
            bool keep = idleBytes_.fetch_add(e.bytes, std::memory_order_relaxed) + e.bytes <= options_.retainLargeBytes;
            if (keep && options_.maxIdlePerLargeClass != 0) {   // the all-stripe class count exists only for this cap
                const std::uint32_t was = idleCount_[e.cls].fetch_add(1, std::memory_order_relaxed);
                if (was >= options_.maxIdlePerLargeClass) {
                    idleCount_[e.cls].fetch_sub(1, std::memory_order_relaxed);
                    keep = false;
                }
            }
            if (keep) {
                e.inUse = false;
                e.discarded = false;
                e.idleSinceNs = now;
                e.nextIdle = st.idleHead[e.cls];
                st.idleHead[e.cls] = static_cast<std::uint32_t>(i + 1);
                st.idleCount[e.cls].store(st.idleCount[e.cls].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            } else {
                idleBytes_.fetch_sub(e.bytes, std::memory_order_relaxed);
                // Over the retention cap: unmap this block (after the lock is dropped).
                toUnmap = {e.ptr, e.bytes};
                counters_.mappedBytes.fetch_sub(e.bytes, std::memory_order_relaxed);
                counters_.unmapCalls.fetch_add(1, std::memory_order_relaxed);
                mapped_.fetch_sub(e.bytes, std::memory_order_relaxed);
                largeLive_.fetch_sub(1, std::memory_order_relaxed);
                eraseLarge(st, i);
            }
        }
    }
    if (misuse) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(options_.hooks, why);
        return;
    }
    if (toUnmap.base)
        sys::vmUnmap(toUnmap);
    // Age idle blocks at most every 50 ms, from whichever thread releases.
    std::uint64_t last = lastAgeNs_.load(std::memory_order_relaxed);
    if (now - last > 50'000'000ull && lastAgeNs_.compare_exchange_strong(last, now, std::memory_order_relaxed))
        ageIdle(now);
}

void SizeClassPool::ageIdle(std::uint64_t nowNs) noexcept {
    // Idle blocks older than the limit lose their pages. A block being aged is first taken off its idle list, so no
    // other thread can acquire it while madvise runs without the lock; it goes back afterwards. The idle counters
    // are left alone while a block is aged: it is still idle, merely unavailable for a moment.
    const std::uint64_t limit = static_cast<std::uint64_t>(options_.releaseIdleAfter.count()) * 1'000'000ull;
    for (std::size_t s = 0; s < stripeCount_; ++s) {
        LargeStripe& st = stripes_[s];
        for (;;) {
            std::uint32_t taken[kMaxPending];
            std::size_t count = 0;
            {
                std::lock_guard<std::mutex> g(st.mu);
                for (std::size_t c = firstLargeClass_; c < table_.count() && count < kMaxPending; ++c) {
                    std::uint32_t* link = &st.idleHead[c];
                    while (*link && count < kMaxPending) {
                        LargeEntry& e = st.entries[*link - 1];
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
                break;
            for (std::size_t k = 0; k < count; ++k)
                sys::vmDiscard(st.entries[taken[k]].ptr, st.entries[taken[k]].bytes);   // exclusively ours while aging
            std::lock_guard<std::mutex> g(st.mu);
            for (std::size_t k = 0; k < count; ++k) {
                LargeEntry& e = st.entries[taken[k]];
                e.aging = false;
                e.discarded = true;
                e.nextIdle = st.idleHead[e.cls];
                st.idleHead[e.cls] = taken[k] + 1;
            }
            if (count < kMaxPending)
                break;
        }
    }
}

void SizeClassPool::trim() noexcept { ageIdle(~std::uint64_t{0}); }

void SizeClassPool::releaseIdleLarge() noexcept {
    for (std::size_t s = 0; s < stripeCount_; ++s) {
        LargeStripe& st = stripes_[s];
        for (;;) {
            sys::VmRegion batch[kMaxPending];
            std::size_t n = 0;
            {
                std::lock_guard<std::mutex> g(st.mu);
                for (std::size_t c = firstLargeClass_; c < table_.count() && n < kMaxPending; ++c) {
                    while (st.idleHead[c] && n < kMaxPending) {
                        const std::uint32_t slot = st.idleHead[c] - 1;
                        LargeEntry& e = st.entries[slot];
                        st.idleHead[c] = e.nextIdle;
                        batch[n++] = {e.ptr, e.bytes};
                        st.idleCount[c].store(st.idleCount[c].load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
                        if (options_.maxIdlePerLargeClass != 0)
                            idleCount_[c].fetch_sub(1, std::memory_order_relaxed);
                        idleBytes_.fetch_sub(e.bytes, std::memory_order_relaxed);
                        counters_.mappedBytes.fetch_sub(e.bytes, std::memory_order_relaxed);
                        counters_.unmapCalls.fetch_add(1, std::memory_order_relaxed);
                        mapped_.fetch_sub(e.bytes, std::memory_order_relaxed);
                        largeLive_.fetch_sub(1, std::memory_order_relaxed);
                        eraseLarge(st, slot);
                    }
                }
            }
            for (std::size_t i = 0; i < n; ++i)
                sys::vmUnmap(batch[i]);
            if (n < kMaxPending)
                break;
        }
    }
}

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
    Stats total = counters_.snapshot();   // large mappings, borrow/refuse/misuse counts
    total.retainedBytes = idleBytes_.load(std::memory_order_relaxed);
    for (std::size_t k = 0; k < stripeCount_; ++k) {   // large acquires/releases live per stripe
        const Stats t = stripes_[k].counters.snapshot();
        total.blocksInUse += t.blocksInUse;
        total.peakBlocksInUse += t.peakBlocksInUse;   // sum of stripe peaks: an upper bound of the true peak
        total.acquires += t.acquires;
        total.releases += t.releases;
    }
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
