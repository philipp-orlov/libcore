// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/slab_pool.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include "base/platform.hpp"
#include "base/status.hpp"
#include "sys/vm.hpp"

namespace mem {

namespace {
constexpr std::uint64_t kSlabMagic = 0x534c41424d454d31ull;  // "SLABMEM1"
constexpr std::size_t kStealBatch = 16;
constexpr unsigned kSlots = 64;       // thread-local cache slots (pool generation % kSlots)
constexpr unsigned kMaxMag = 32;      // upper bound of options.threadCacheBlocks

std::atomic<unsigned> g_nextThreadId{0};
unsigned threadId() noexcept {
    static thread_local const unsigned id = g_nextThreadId.fetch_add(1, std::memory_order_relaxed);
    return id;
}

std::atomic<std::uint64_t> g_nextGeneration{1};

// Guards slot registration, retirement and pool destruction. Taken on a thread's first use of a pool, when a slot is
// taken over, when a thread exits, when stats are read and when a pool is destroyed: never on the fast path.
std::mutex& cacheMutex() {
    static std::mutex* m = new std::mutex;  // leaked on purpose: thread-exit hooks may run during static destruction
    return *m;
}

constexpr bool isPow2(std::size_t v) { return v && !(v & (v - 1)); }
constexpr std::size_t bitmapWordsFor(std::size_t blocks) { return (blocks + 63) / 64; }
}  // namespace

struct SlabPool::Slab {
    std::uint64_t magic;
    std::uint32_t blockBytes;
    std::uint32_t blockCount;
    std::uint32_t carved;     // blocks handed out at least once; guarded by the owning shard's mutex
    std::uint32_t pad;
    Slab* nextReserve;
    std::atomic<std::uint64_t> bits[1];  // allocated flags, bitmapWords entries (the header is sized for them)
};

// One per (thread, pool). Written only by its thread, except `dead` (pool destructor) and the list links (cacheMutex).
struct alignas(64) SlabPool::ThreadSlot {
    SlabPool* pool = nullptr;
    std::uint64_t gen = 0;
    std::atomic<bool> dead{false};
    std::atomic<std::uint64_t> acquires{0}, releases{0};
    std::uint64_t syncedAcquires = 0, syncedReleases = 0;
    unsigned count = 0;
    void* items[kMaxMag];
    ThreadSlot* next = nullptr;
    ThreadSlot* prev = nullptr;
};

struct SlabPool::ThreadCacheAccess {
    // cacheMutex held. Folds a slot's counters into its pool, returns its cached blocks, unlinks it.
    static void retire(ThreadSlot& s) noexcept {
        SlabPool* p = s.pool;
        if (p && !s.dead.load(std::memory_order_acquire)) {
            const std::uint64_t acq = s.acquires.load(std::memory_order_relaxed), rel = s.releases.load(std::memory_order_relaxed);
            p->retiredAcquires_.fetch_add(acq, std::memory_order_relaxed);
            p->retiredReleases_.fetch_add(rel, std::memory_order_relaxed);
            p->retiredInUse_.fetch_add(static_cast<std::int64_t>(acq) - static_cast<std::int64_t>(rel), std::memory_order_relaxed);
            if (s.count) {
                void* first = nullptr;
                void* last = s.items[0];
                for (unsigned i = 0; i < s.count; ++i) {
                    *static_cast<void**>(s.items[i]) = first;
                    first = s.items[i];
                }
                p->pushToShard(first, last, s.count);
            }
            if (s.prev) s.prev->next = s.next; else p->slotHead_ = s.next;
            if (s.next) s.next->prev = s.prev;
        }
        s.pool = nullptr;
        s.gen = 0;
        s.count = 0;
        s.acquires.store(0, std::memory_order_relaxed);
        s.releases.store(0, std::memory_order_relaxed);
        s.syncedAcquires = s.syncedReleases = 0;
        s.dead.store(false, std::memory_order_relaxed);
        s.next = s.prev = nullptr;
    }
};

namespace {
thread_local bool t_cacheGone = false;   // see below
}

namespace {
struct ThreadCache {
    SlabPool::ThreadSlot slots[kSlots];
    ~ThreadCache() {
        std::lock_guard<std::mutex> g(cacheMutex());
        t_cacheGone = true;
        for (SlabPool::ThreadSlot& s : slots)
            if (s.pool)
                SlabPool::ThreadCacheAccess::retire(s);
    }
};
thread_local ThreadCache t_cache;
// Set when this thread's cache has been torn down. Other thread-exit destructors (a thread_local holding a pooled
// buffer, a task object destroyed late) may still release blocks after that; they must not re-bind a slot that
// nothing would ever retire. Plain bool: constant-initialised, trivially destructible, valid to the end of the thread.
}  // namespace

SlabPool::SlabPool(const SlabPoolOptions& o)
    : blockBytes_(o.blockBytes),
      slabBytes_(o.slabBytes),
      blocksPerSlab_([&] {
          std::size_t blocks = o.slabBytes / o.blockBytes;
          for (;;) {
              const std::size_t header = (offsetof(Slab, bits) + 8 * bitmapWordsFor(blocks) + 63) & ~std::size_t{63};
              if (header + blocks * o.blockBytes <= o.slabBytes)
                  return blocks;
              --blocks;
          }
      }()),
      firstBlockOffset_((offsetof(Slab, bits) + 8 * bitmapWordsFor(blocksPerSlab_) + 63) & ~std::size_t{63}),
      bitmapWords_(bitmapWordsFor(blocksPerSlab_)),
      shardCount_(o.shards ? o.shards : 1),
      maxSlabs_(o.maxSlabs ? o.maxSlabs : 1),
      maxBytes_(o.maxBytes),
      scrub_(o.scrubOnRelease),
      verify_(o.verifyBlocks),
      hooks_(o.hooks),
      name_(o.name),
      mapped_(o.sharedMapped ? o.sharedMapped : &ownMapped_),
      cacheBlocks_(std::min<unsigned>(o.threadCacheBlocks, kMaxMag)),
      gen_(g_nextGeneration.fetch_add(1, std::memory_order_relaxed)) {
    if (o.blockBytes < 64 || o.blockBytes % 64 != 0)
        throw base::Error(base::Code::InvalidArgument, "SlabPool: blockBytes must be a multiple of 64");
    if (!isPow2(o.slabBytes) || o.slabBytes < o.blockBytes * 16 || o.slabBytes < sys::pageSize())
        throw base::Error(base::Code::InvalidArgument, "SlabPool: slabBytes must be a power of two >= 16 blocks");
    shards_.reset(new Shard[shardCount_]);
    // The slab registry: virtual memory only; untouched pages cost nothing until slabs are registered.
    std::size_t cap = 16;
    while (cap < maxSlabs_ * 2)
        cap <<= 1;
    const sys::VmRegion r = sys::vmMap(cap * sizeof(std::atomic<std::uintptr_t>));
    registry_ = static_cast<std::atomic<std::uintptr_t>*>(r.base);  // zero filled: all empty
    registryMask_ = cap - 1;
    registryBytes_ = r.bytes;
}

SlabPool::~SlabPool() {
    {
        // Threads that still hold a slot for this pool will find it dead and drop their cached pointers.
        std::lock_guard<std::mutex> g(cacheMutex());
        for (ThreadSlot* s = slotHead_; s; s = s->next)
            s->dead.store(true, std::memory_order_release);
        slotHead_ = nullptr;
    }
    for (std::size_t i = 0; i <= registryMask_; ++i) {
        const std::uintptr_t base = registry_[i].load(std::memory_order_relaxed);
        if (base) {
            sys::vmUnmap(reinterpret_cast<void*>(base), slabBytes_);
            counters_.unmapCalls.fetch_add(1, std::memory_order_relaxed);
            counters_.mappedBytes.fetch_sub(slabBytes_, std::memory_order_relaxed);
            mapped_->fetch_sub(slabBytes_, std::memory_order_relaxed);
        }
    }
    sys::vmUnmap(registry_, registryBytes_);
}

unsigned SlabPool::shardIndex() const noexcept { return threadId() % shardCount_; }

SlabPool::Slab* SlabPool::slabOf(const void* p) const noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(p) & ~(slabBytes_ - 1);
    std::size_t i = static_cast<std::size_t>((base / slabBytes_) * 0x9E3779B97F4A7C15ull >> 20) & registryMask_;
    for (;;) {
        const std::uintptr_t v = registry_[i].load(std::memory_order_acquire);
        if (v == base)
            return reinterpret_cast<Slab*>(base);
        if (v == 0)
            return nullptr;
        i = (i + 1) & registryMask_;
    }
}

bool SlabPool::owns(const void* p) const noexcept { return slabOf(p) != nullptr; }

bool SlabPool::registryInsert(Slab* s) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(s);
    std::size_t i = static_cast<std::size_t>((base / slabBytes_) * 0x9E3779B97F4A7C15ull >> 20) & registryMask_;
    while (registry_[i].load(std::memory_order_relaxed) != 0)
        i = (i + 1) & registryMask_;
    registry_[i].store(base, std::memory_order_release);
    return true;
}

SlabPool::Slab* SlabPool::mapSlab() noexcept {
    std::lock_guard<std::mutex> grow(growMu_);
    if (sealed_.load(std::memory_order_acquire))
        return nullptr;
    if (slabs_.load(std::memory_order_relaxed) >= maxSlabs_) {
        reportExhausted(hooks_, name_, slabBytes_);
        return nullptr;
    }
    if (maxBytes_ != 0 && mapped_->load(std::memory_order_relaxed) + slabBytes_ > maxBytes_) {
        reportExhausted(hooks_, name_, slabBytes_);
        return nullptr;
    }
    const sys::VmRegion r = sys::tryVmMapAligned(slabBytes_, slabBytes_);
    if (!r.base)
        return nullptr;
    counters_.mapCalls.fetch_add(1, std::memory_order_relaxed);
    counters_.mappedBytes.fetch_add(slabBytes_, std::memory_order_relaxed);
    mapped_->fetch_add(slabBytes_, std::memory_order_relaxed);
    auto* s = static_cast<Slab*>(r.base);  // pages are zero: bitmap clear, carved == 0
    s->magic = kSlabMagic;
    s->blockBytes = static_cast<std::uint32_t>(blockBytes_);
    s->blockCount = static_cast<std::uint32_t>(blocksPerSlab_);
    s->nextReserve = nullptr;
    registryInsert(s);
    slabs_.fetch_add(1, std::memory_order_relaxed);
    return s;
}

void* SlabPool::takeFromCarve(Shard& sh) noexcept {
    Slab* s = sh.carve;
    if (!s || s->carved >= s->blockCount) {
        if (!sh.reserve)
            return nullptr;
        s = sh.carve = sh.reserve;
        sh.reserve = s->nextReserve;
        s->nextReserve = nullptr;
    }
    unsigned char* base = reinterpret_cast<unsigned char*>(s) + firstBlockOffset_;
    return base + std::size_t{s->carved++} * blockBytes_;
}

void* SlabPool::steal(unsigned self, bool blocking) noexcept {
    for (unsigned k = 1; k < shardCount_; ++k) {
        Shard& other = shards_[(self + k) % shardCount_];
        std::unique_lock<std::mutex> lock(other.mu, std::defer_lock);
        if (blocking)
            lock.lock();
        else if (!lock.try_lock())
            continue;
        if (other.freeCount == 0)
            continue;
        void* first = other.freeHead;
        void* last = first;
        std::size_t taken = 1;
        while (taken < kStealBatch) {
            void* next = *static_cast<void**>(last);
            if (!next)
                break;
            last = next;
            ++taken;
        }
        other.freeHead = *static_cast<void**>(last);
        other.freeCount -= taken;
        lock.unlock();
        if (taken > 1) {
            void* extra = *static_cast<void**>(first);
            pushToShard(extra, last, taken - 1);
        }
        return first;
    }
    return nullptr;
}

void SlabPool::pushToShard(void* first, void* last, std::size_t count) noexcept {
    Shard& sh = shards_[shardIndex()];
    std::lock_guard<std::mutex> g(sh.mu);
    *static_cast<void**>(last) = sh.freeHead;
    sh.freeHead = first;
    sh.freeCount += count;
}

std::size_t SlabPool::takeBatch(unsigned shard, void** out, std::size_t want) noexcept {
    Shard& sh = shards_[shard];
    std::lock_guard<std::mutex> g(sh.mu);
    std::size_t n = 0;
    while (n < want && sh.freeHead) {
        void* p = sh.freeHead;
        sh.freeHead = *static_cast<void**>(p);
        --sh.freeCount;
        out[n++] = p;
    }
    while (n < want) {
        void* p = takeFromCarve(sh);
        if (!p)
            break;
        out[n++] = p;
    }
    return n;
}

std::size_t SlabPool::refillBatch(void** out, std::size_t want) noexcept {
    const unsigned self = shardIndex();
    std::size_t n = takeBatch(self, out, want);
    if (n)
        return n;
    if (void* p = steal(self, false)) {
        out[0] = p;
        return 1;
    }
    // Before mapping more memory, look properly (waiting for locks) at what other shards have idle: a producer on one
    // thread and a consumer on another must not make the pool grow because a try_lock lost a race.
    if (void* p = steal(self, true)) {
        out[0] = p;
        return 1;
    }
    Slab* fresh = mapSlab();   // syscalls run with no shard lock held
    if (!fresh) {
        counters_.refused.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    {
        Shard& sh = shards_[self];
        std::lock_guard<std::mutex> g(sh.mu);
        fresh->nextReserve = sh.reserve;
        sh.reserve = fresh;
    }
    return takeBatch(self, out, want);
}

bool SlabPool::markAllocated(void* p) noexcept {
    Slab* s = reinterpret_cast<Slab*>(reinterpret_cast<std::uintptr_t>(p) & ~(slabBytes_ - 1));
    const std::size_t idx = (static_cast<unsigned char*>(p) - reinterpret_cast<unsigned char*>(s) - firstBlockOffset_) / blockBytes_;
    const std::uint64_t bit = std::uint64_t{1} << (idx & 63);
    const std::uint64_t old = s->bits[idx >> 6].fetch_or(bit, std::memory_order_acq_rel);
    if (old & bit) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(hooks_, "slab free list handed out a block that is already allocated (corruption)");
        return false;
    }
    return true;
}

bool SlabPool::clearAllocated(void* p) noexcept {
    Slab* s = slabOf(p);
    if (!s) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(hooks_, "release of a pointer this pool does not own");
        return false;
    }
    const auto off = static_cast<std::size_t>(static_cast<unsigned char*>(p) - reinterpret_cast<unsigned char*>(s));
    if (off < firstBlockOffset_ || (off - firstBlockOffset_) % blockBytes_ != 0) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(hooks_, "release of a pointer that is not at the start of a block");
        return false;
    }
    const std::size_t idx = (off - firstBlockOffset_) / blockBytes_;
    if (idx >= blocksPerSlab_) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(hooks_, "release of a pointer past the last block of its slab");
        return false;
    }
    const std::uint64_t bit = std::uint64_t{1} << (idx & 63);
    const std::uint64_t old = s->bits[idx >> 6].fetch_and(~bit, std::memory_order_acq_rel);
    if (!(old & bit)) {
        counters_.misuse.fetch_add(1, std::memory_order_relaxed);
        reportMisuse(hooks_, "double free of a pooled block");
        return false;
    }
    return true;
}

SlabPool::ThreadSlot& SlabPool::bind() noexcept {
    ThreadSlot& s = t_cache.slots[gen_ % kSlots];
    if (BASE_LIKELY(s.gen == gen_ && s.pool == this))
        return s;
    std::lock_guard<std::mutex> g(cacheMutex());
    ThreadCacheAccess::retire(s);   // a different (or dead) pool held this slot
    s.pool = this;
    s.gen = gen_;
    s.next = slotHead_;
    s.prev = nullptr;
    if (slotHead_)
        slotHead_->prev = &s;
    slotHead_ = &s;
    return s;
}

void SlabPool::sampleInUse(ThreadSlot& s) noexcept {
    const std::uint64_t acq = s.acquires.load(std::memory_order_relaxed), rel = s.releases.load(std::memory_order_relaxed);
    const std::int64_t delta = static_cast<std::int64_t>(acq - s.syncedAcquires) - static_cast<std::int64_t>(rel - s.syncedReleases);
    s.syncedAcquires = acq;
    s.syncedReleases = rel;
    const std::int64_t now = syncedInUse_.fetch_add(delta, std::memory_order_relaxed) + delta;
    std::uint64_t peak = counters_.peakBlocksInUse.load(std::memory_order_relaxed);
    while (now > 0 && static_cast<std::uint64_t>(now) > peak &&
           !counters_.peakBlocksInUse.compare_exchange_weak(peak, static_cast<std::uint64_t>(now), std::memory_order_relaxed)) {
    }
}

void* SlabPool::tryAcquire() noexcept {
    if (BASE_UNLIKELY(t_cacheGone)) {   // late in thread teardown: straight to the shared lists, counted globally
        void* p = nullptr;
        if (refillBatch(&p, 1) == 0)
            return nullptr;
        if (verify_)
            markAllocated(p);
        retiredAcquires_.fetch_add(1, std::memory_order_relaxed);
        retiredInUse_.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    ThreadSlot& s = bind();
    if (s.count == 0) {
        sampleInUse(s);
        s.count = static_cast<unsigned>(refillBatch(s.items, std::max<unsigned>(1, cacheBlocks_ / 2)));
        if (s.count == 0)
            return nullptr;
    }
    void* p = s.items[--s.count];
    if (verify_)
        markAllocated(p);
    s.acquires.store(s.acquires.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
#ifndef NDEBUG
    std::memset(p, 0xCD, blockBytes_);
#endif
    return p;
}

bool SlabPool::release(void* p) noexcept {
    if (verify_ && !clearAllocated(p))
        return false;
    if (scrub_)
        std::memset(p, 0, blockBytes_);
#ifndef NDEBUG
    else
        std::memset(p, 0xDD, blockBytes_);
#endif
    if (BASE_UNLIKELY(t_cacheGone)) {
        *static_cast<void**>(p) = nullptr;
        pushToShard(p, p, 1);
        retiredReleases_.fetch_add(1, std::memory_order_relaxed);
        retiredInUse_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    ThreadSlot& s = bind();
    s.releases.store(s.releases.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const unsigned cap = std::max<unsigned>(1, cacheBlocks_);
    if (s.count >= cap) {
        // Hand the newest half back to the shared free list in one locked operation.
        sampleInUse(s);
        const unsigned give = std::max<unsigned>(1, cap / 2);
        void* first = nullptr;
        void* last = nullptr;
        for (unsigned i = 0; i < give; ++i) {
            void* b = s.items[--s.count];
            if (!last)
                last = b;
            *static_cast<void**>(b) = first;
            first = b;
        }
        pushToShard(first, last, give);
    }
    s.items[s.count++] = p;
    return true;
}

Stats SlabPool::stats() const noexcept {
    Stats st = counters_.snapshot();
    std::uint64_t acq = retiredAcquires_.load(std::memory_order_relaxed), rel = retiredReleases_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(cacheMutex());
        for (const ThreadSlot* s = slotHead_; s; s = s->next) {
            acq += s->acquires.load(std::memory_order_relaxed);
            rel += s->releases.load(std::memory_order_relaxed);
        }
    }
    st.acquires = acq;
    st.releases = rel;
    st.blocksInUse = acq >= rel ? acq - rel : 0;
    st.peakBlocksInUse = std::max(st.peakBlocksInUse, st.blocksInUse);
    const std::uint64_t leased = st.blocksInUse * blockBytes_;
    st.retainedBytes = st.mappedBytes > leased ? st.mappedBytes - leased : 0;
    return st;
}

bool SlabPool::prewarm(std::size_t blocks) noexcept {
    std::size_t have = 0;
    for (unsigned i = 0; i < shardCount_; ++i) {
        std::lock_guard<std::mutex> g(shards_[i].mu);
        have += shards_[i].freeCount;
        if (shards_[i].carve)
            have += shards_[i].carve->blockCount - shards_[i].carve->carved;
        for (Slab* r = shards_[i].reserve; r; r = r->nextReserve)
            have += r->blockCount;
    }
    Shard& sh = shards_[0];
    while (have < blocks) {
        Slab* fresh = mapSlab();
        if (!fresh)
            return false;
        std::lock_guard<std::mutex> g(sh.mu);
        fresh->nextReserve = sh.reserve;
        sh.reserve = fresh;
        have += fresh->blockCount;
    }
    return true;
}

}  // namespace mem
