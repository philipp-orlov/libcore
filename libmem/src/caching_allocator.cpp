// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/caching_allocator.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include "base/status.hpp"
#include "sys/vm.hpp"
#include "base/platform.hpp"

namespace mem {

namespace {
constexpr std::uint32_t kNil = ~std::uint32_t{0};
constexpr std::size_t kLargeBins = 4096;   // last bin collects everything bigger

Segment hostAlloc(void*, std::size_t bytes) {
    const sys::VmRegion r = sys::tryVmMapAligned(bytes, std::size_t{2} << 20);
    return {r.base, r.bytes};
}
void hostFree(void*, Segment s) { sys::vmUnmap(s.base, s.bytes); }
}  // namespace

BackingOps hostVmBacking() noexcept { return {&hostAlloc, &hostFree, nullptr}; }

struct CachingAllocator::Impl {
    struct Node {
        char* ptr;
        std::size_t size;
        std::uint32_t prev, next;          // address order inside the segment
        std::uint32_t binPrev, binNext;    // free-bin list
        std::uint32_t segment;
        std::uint32_t pool;                // pool index = stream slot * 2 + large
        std::uint32_t generation;
        std::uint32_t crossUse;            // outstanding recordUse() registrations
        std::uint32_t crossStreams;        // bitmask of other stream slots (<= 32)
        bool allocated;
        bool deferred;
        bool inUse;                        // node slot is live
    };
    struct SegmentRec {
        char* base;
        std::size_t bytes;
        std::uint32_t head;                // first node in address order
        std::uint64_t lastTick;
        bool inUse;
    };
    struct Pool {
        std::uint32_t* heads;              // per bin
        std::uint64_t* bitmap;             // non-empty bins
        std::size_t bins, words;
    };

    BackingOps backing;
    CachingOptions opt;
    Node* nodes = nullptr;
    std::uint32_t* freeNodes = nullptr;
    std::size_t freeNodeCount = 0;
    SegmentRec* segs = nullptr;
    std::uint32_t* freeSegs = nullptr;
    std::size_t freeSegCount = 0;
    std::size_t smallBins = 0;
    Pool* pools = nullptr;                 // [stream slot * 2 + large], reserved up front
    StreamId* streamIds = nullptr;
    unsigned streamCount = 0;
    std::uint32_t deferredHead = kNil;     // chain through Node::binNext
    void* arena = nullptr;
    std::size_t arenaBytes = 0;
    std::uint64_t tick = 0;
    CacheStats st;

    // ---- setup ----
    explicit Impl(BackingOps b, CachingOptions o) : backing(b), opt(o) {
        if (!b.allocate || !b.release)
            throw base::Error(base::Code::InvalidArgument, "CachingAllocator: backing needs allocate and release");
        if (opt.maxStreams == 0 || opt.maxStreams > 32)
            throw base::Error(base::Code::InvalidArgument, "CachingAllocator: maxStreams must be 1..32");
        smallBins = opt.smallLimit / opt.smallGranule + 1;
        const std::size_t poolCount = std::size_t{opt.maxStreams} * 2;
        std::size_t total = 0;
        auto reserve = [&](std::size_t bytes) { const std::size_t at = total; total += (bytes + 63) & ~std::size_t{63}; return at; };
        const std::size_t oNodes = reserve(opt.maxBlocks * sizeof(Node));
        const std::size_t oFreeNodes = reserve(opt.maxBlocks * sizeof(std::uint32_t));
        const std::size_t oSegs = reserve(opt.maxSegments * sizeof(SegmentRec));
        const std::size_t oFreeSegs = reserve(opt.maxSegments * sizeof(std::uint32_t));
        const std::size_t oPools = reserve(poolCount * sizeof(Pool));
        const std::size_t oStreams = reserve(opt.maxStreams * sizeof(StreamId));
        std::size_t oBins[64], oMaps[64];
        for (std::size_t p = 0; p < poolCount; ++p) {
            const std::size_t bins = (p & 1) ? kLargeBins : smallBins;
            oBins[p] = reserve(bins * sizeof(std::uint32_t));
            oMaps[p] = reserve(((bins + 63) / 64) * sizeof(std::uint64_t));
        }
        const sys::VmRegion r = sys::vmMap(total);   // zero filled, committed lazily
        arena = r.base;
        arenaBytes = r.bytes;
        char* base = static_cast<char*>(r.base);
        nodes = reinterpret_cast<Node*>(base + oNodes);
        freeNodes = reinterpret_cast<std::uint32_t*>(base + oFreeNodes);
        segs = reinterpret_cast<SegmentRec*>(base + oSegs);
        freeSegs = reinterpret_cast<std::uint32_t*>(base + oFreeSegs);
        pools = reinterpret_cast<Pool*>(base + oPools);
        streamIds = reinterpret_cast<StreamId*>(base + oStreams);
        for (std::size_t i = 0; i < opt.maxBlocks; ++i)
            freeNodes[i] = static_cast<std::uint32_t>(opt.maxBlocks - 1 - i);
        freeNodeCount = opt.maxBlocks;
        for (std::size_t i = 0; i < opt.maxSegments; ++i)
            freeSegs[i] = static_cast<std::uint32_t>(opt.maxSegments - 1 - i);
        freeSegCount = opt.maxSegments;
        for (std::size_t p = 0; p < poolCount; ++p) {
            const std::size_t bins = (p & 1) ? kLargeBins : smallBins;
            pools[p].heads = reinterpret_cast<std::uint32_t*>(base + oBins[p]);
            pools[p].bitmap = reinterpret_cast<std::uint64_t*>(base + oMaps[p]);
            pools[p].bins = bins;
            pools[p].words = (bins + 63) / 64;
            std::fill(pools[p].heads, pools[p].heads + bins, kNil);
        }
    }

    ~Impl() {
        for (std::size_t i = 0; i < opt.maxSegments; ++i)
            if (segs[i].inUse)
                backing.release(backing.ctx, {segs[i].base, segs[i].bytes});
        sys::vmUnmap(arena, arenaBytes);
    }

    // ---- bins ----
    std::size_t roundSize(std::size_t bytes, bool& large) const {
        if (bytes == 0)
            bytes = 1;
        large = bytes > opt.smallLimit;
        const std::size_t g = large ? opt.largeGranule : opt.smallGranule;
        return (bytes + g - 1) / g * g;
    }
    std::size_t binOf(const Pool& pool, std::size_t size, bool large) const {
        const std::size_t b = size / (large ? opt.largeGranule : opt.smallGranule);
        return std::min(b, pool.bins - 1);
    }
    void binInsert(Node& n, std::uint32_t idx) {
        Pool& pool = pools[n.pool];
        const bool large = n.pool & 1;
        const std::size_t b = binOf(pool, n.size, large);
        n.binPrev = kNil;
        n.binNext = pool.heads[b];
        if (n.binNext != kNil)
            nodes[n.binNext].binPrev = idx;
        pool.heads[b] = idx;
        pool.bitmap[b >> 6] |= std::uint64_t{1} << (b & 63);
    }
    void binRemove(Node& n, std::uint32_t idx) {
        Pool& pool = pools[n.pool];
        const std::size_t b = binOf(pool, n.size, n.pool & 1);
        if (n.binPrev != kNil) nodes[n.binPrev].binNext = n.binNext; else pool.heads[b] = n.binNext;
        if (n.binNext != kNil) nodes[n.binNext].binPrev = n.binPrev;
        if (pool.heads[b] == kNil)
            pool.bitmap[b >> 6] &= ~(std::uint64_t{1} << (b & 63));
        n.binPrev = n.binNext = kNil;
        (void)idx;
    }
    // Best fit: the smallest free block with size >= need. Bins are exact for sizes below the last bin.
    std::uint32_t findFit(std::uint32_t poolIndex, std::size_t need) {
        Pool& pool = pools[poolIndex];
        const bool large = poolIndex & 1;
        std::size_t b = binOf(pool, need, large);
        // The bin of `need` itself may hold blocks of exactly `need` (small/large exact bins) -- or, for the last
        // overflow bin, blocks of various sizes: scan that one for the smallest fit.
        for (std::size_t w = b >> 6; w < pool.words; ++w) {
            std::uint64_t bits = pool.bitmap[w];
            if (w == (b >> 6))
                bits &= ~std::uint64_t{0} << (b & 63);
            while (bits) {
                const std::size_t bin = w * 64 + static_cast<std::size_t>(base::countTrailingZeros(bits));
                // A bin above need's bin holds only blocks that fit, and they differ by less than a granule.
                if (bin != b && bin != pool.bins - 1)
                    return pool.heads[bin];
                std::uint32_t best = kNil;
                for (std::uint32_t i = pool.heads[bin]; i != kNil; i = nodes[i].binNext)
                    if (nodes[i].size >= need && (best == kNil || nodes[i].size < nodes[best].size))
                        best = i;
                if (best != kNil)
                    return best;
                bits &= bits - 1;
            }
        }
        return kNil;
    }

    // ---- nodes / segments ----
    std::uint32_t newNode() {
        if (freeNodeCount == 0)
            return kNil;
        const std::uint32_t i = freeNodes[--freeNodeCount];
        const std::uint32_t gen = nodes[i].generation + 1;
        nodes[i] = Node{};
        nodes[i].generation = gen;
        nodes[i].inUse = true;
        nodes[i].prev = nodes[i].next = nodes[i].binPrev = nodes[i].binNext = kNil;
        return i;
    }
    void deleteNode(std::uint32_t i) {
        nodes[i].inUse = false;
        nodes[i].allocated = false;
        freeNodes[freeNodeCount++] = i;
    }

    unsigned streamSlot(StreamId id, bool create) {
        for (unsigned i = 0; i < streamCount; ++i)
            if (streamIds[i] == id)
                return i;
        if (!create || streamCount == opt.maxStreams)
            return ~0u;
        streamIds[streamCount] = id;
        return streamCount++;
    }

    bool newSegment(std::uint32_t poolIndex, std::size_t need) {
        const bool large = poolIndex & 1;
        const std::size_t bytes = large ? need : std::max(need, opt.smallSegment);
        if (opt.budgetBytes && st.segmentBytes + bytes > opt.budgetBytes)
            return false;
        if (freeSegCount == 0 || freeNodeCount == 0)
            return false;
        const Segment s = backing.allocate(backing.ctx, bytes);
        if (!s.base)
            return false;
        const std::uint32_t si = freeSegs[--freeSegCount];
        const std::uint32_t ni = newNode();
        segs[si] = {static_cast<char*>(s.base), s.bytes, ni, ++tick, true};
        Node& n = nodes[ni];
        n.ptr = static_cast<char*>(s.base);
        n.size = s.bytes;
        n.segment = si;
        n.pool = poolIndex;
        st.segmentBytes += s.bytes;
        st.cachedBytes += s.bytes;
        st.peakSegmentBytes = std::max(st.peakSegmentBytes, st.segmentBytes);
        ++st.segments;
        ++st.backingAllocs;
        binInsert(n, ni);
        return true;
    }

    std::size_t releaseSegment(std::uint32_t si) {
        SegmentRec& s = segs[si];
        const std::uint32_t ni = s.head;
        binRemove(nodes[ni], ni);
        const std::size_t bytes = s.bytes;
        st.cachedBytes -= bytes;
        st.segmentBytes -= bytes;
        --st.segments;
        ++st.backingFrees;
        deleteNode(ni);
        const Segment seg{s.base, s.bytes};
        s.inUse = false;
        freeSegs[freeSegCount++] = si;
        backing.release(backing.ctx, seg);
        return bytes;
    }

    // A segment is idle when its only block is free and spans the whole segment.
    std::size_t releaseIdle(std::size_t target, bool all) {
        std::size_t released = 0;
        while (all || released < target) {
            std::uint32_t oldest = kNil;
            for (std::uint32_t i = 0; i < opt.maxSegments; ++i) {
                const SegmentRec& s = segs[i];
                if (!s.inUse)
                    continue;
                const Node& h = nodes[s.head];
                if (h.allocated || h.size != s.bytes || h.deferred)
                    continue;
                if (oldest == kNil || s.lastTick < segs[oldest].lastTick)
                    oldest = i;
            }
            if (oldest == kNil)
                break;
            released += releaseSegment(oldest);
        }
        return released;
    }

    // ---- API ----
    Block allocate(std::size_t bytes, StreamId stream) {
        bool large;
        const std::size_t need = roundSize(bytes, large);
        const unsigned slot = streamSlot(stream, true);
        if (slot == ~0u) {
            ++st.refused;
            reportExhausted(opt.hooks, "caching-allocator streams", 0);
            return {};
        }
        const std::uint32_t poolIndex = slot * 2 + (large ? 1u : 0u);
        std::uint32_t fit = findFit(poolIndex, need);
        if (fit == kNil) {
            if (!newSegment(poolIndex, need)) {
                releaseIdle(0, true);                 // give idle segments back, then try once more
                if (!newSegment(poolIndex, need)) {
                    ++st.refused;
                    reportExhausted(opt.hooks, "caching-allocator", bytes);
                    return {};
                }
            }
            fit = findFit(poolIndex, need);
            if (fit == kNil) {
                ++st.refused;
                return {};
            }
        }
        Node& n = nodes[fit];
        binRemove(n, fit);
        const std::size_t remainder = n.size - need;
        const std::size_t minSplit = large ? opt.splitRemainderMin : opt.smallGranule;
        if (remainder >= minSplit && freeNodeCount > 0) {
            const std::uint32_t ri = newNode();
            Node& r = nodes[ri];
            Node& cur = nodes[fit];
            r.ptr = cur.ptr + need;
            r.size = remainder;
            r.segment = cur.segment;
            r.pool = cur.pool;
            r.prev = fit;
            r.next = cur.next;
            if (cur.next != kNil)
                nodes[cur.next].prev = ri;
            cur.next = ri;
            cur.size = need;
            binInsert(r, ri);
            ++st.splits;
        }
        Node& out = nodes[fit];
        out.allocated = true;
        ++out.generation;   // every hand-out gets a new generation, so a stale handle to the same block is rejected
        st.cachedBytes -= out.size;
        st.allocatedBytes += out.size;
        st.peakAllocatedBytes = std::max(st.peakAllocatedBytes, st.allocatedBytes);
        ++st.blocksInUse;
        ++st.allocations;
        segs[out.segment].lastTick = ++tick;
        return {out.ptr, out.size, fit, out.generation};
    }

    bool valid(const Block& b) const {
        return b.ptr && b.node < opt.maxBlocks && nodes[b.node].inUse && nodes[b.node].allocated &&
               nodes[b.node].generation == b.generation && nodes[b.node].ptr == b.ptr;
    }

    void release(std::uint32_t idx) {
        Node* n = &nodes[idx];
        n->allocated = false;
        n->deferred = false;
        st.allocatedBytes -= n->size;
        st.cachedBytes += n->size;
        --st.blocksInUse;
        segs[n->segment].lastTick = ++tick;
        // Coalesce with free neighbours.
        if (n->prev != kNil && !nodes[n->prev].allocated && !nodes[n->prev].deferred) {
            const std::uint32_t pi = n->prev;
            binRemove(nodes[pi], pi);
            nodes[pi].size += n->size;
            nodes[pi].next = n->next;
            if (n->next != kNil)
                nodes[n->next].prev = pi;
            deleteNode(idx);
            idx = pi;
            n = &nodes[idx];
            ++st.merges;
        }
        if (n->next != kNil && !nodes[n->next].allocated && !nodes[n->next].deferred) {
            const std::uint32_t ni = n->next;
            binRemove(nodes[ni], ni);
            n->size += nodes[ni].size;
            n->next = nodes[ni].next;
            if (nodes[ni].next != kNil)
                nodes[nodes[ni].next].prev = idx;
            deleteNode(ni);
            ++st.merges;
        }
        binInsert(*n, idx);
        if (opt.maxCachedBytes && st.cachedBytes > opt.maxCachedBytes)
            releaseIdle(st.cachedBytes - opt.maxCachedBytes, false);
    }
};

CachingAllocator::CachingAllocator(BackingOps backing, CachingOptions options) : impl_(new Impl(backing, options)) {}
CachingAllocator::~CachingAllocator() { delete impl_; }

CachingAllocator::Block CachingAllocator::allocate(std::size_t bytes, StreamId stream) noexcept {
    return impl_->allocate(bytes, stream);
}

void CachingAllocator::free(Block block) noexcept {
    Impl& m = *impl_;
    if (!m.valid(block)) {
        ++m.st.misuse;
        reportMisuse(m.opt.hooks, "CachingAllocator::free of an invalid or already freed block");
        return;
    }
    ++m.st.frees;
    Impl::Node& n = m.nodes[block.node];
    if (n.crossUse > 0) {
        // Still in flight on another stream: keep it allocated-looking and chain it for reclaimDeferred().
        n.deferred = true;
        n.binNext = m.deferredHead;
        m.deferredHead = block.node;
        ++m.st.deferred;
        n.allocated = false;
        m.st.allocatedBytes -= n.size;
        m.st.cachedBytes += n.size;
        --m.st.blocksInUse;
        return;
    }
    m.release(block.node);
}

void CachingAllocator::recordUse(Block block, StreamId other) noexcept {
    Impl& m = *impl_;
    if (!m.valid(block)) {
        ++m.st.misuse;
        reportMisuse(m.opt.hooks, "CachingAllocator::recordUse on an invalid block");
        return;
    }
    const unsigned slot = m.streamSlot(other, true);
    if (slot == ~0u)
        return;
    Impl::Node& n = m.nodes[block.node];
    if (slot == n.pool / 2)
        return;   // the owning stream already orders its own uses
    if (!(n.crossStreams & (1u << slot))) {
        n.crossStreams |= 1u << slot;
        ++n.crossUse;
    }
}

std::size_t CachingAllocator::reclaimDeferred(bool (*isDone)(StreamId, void*), void* ctx) noexcept {
    Impl& m = *impl_;
    std::size_t freed = 0;
    std::uint32_t* link = &m.deferredHead;
    while (*link != kNil) {
        const std::uint32_t idx = *link;
        Impl::Node& n = m.nodes[idx];
        bool done = true;
        for (unsigned s = 0; s < m.streamCount && done; ++s)
            if ((n.crossStreams & (1u << s)) && !isDone(m.streamIds[s], ctx))
                done = false;
        if (!done) {
            link = &n.binNext;
            continue;
        }
        *link = n.binNext;
        n.binNext = kNil;
        n.crossUse = 0;
        n.crossStreams = 0;
        // The accounting for a deferred block was already moved to "cached" in free(); undo it so release() can redo it.
        n.allocated = true;
        m.st.cachedBytes -= n.size;
        m.st.allocatedBytes += n.size;
        ++m.st.blocksInUse;
        m.release(idx);
        --m.st.deferred;
        ++freed;
    }
    return freed;
}

std::size_t CachingAllocator::emptyCache() noexcept { return impl_->releaseIdle(0, true); }

CachingAllocator::CacheStats CachingAllocator::stats() const noexcept { return impl_->st; }

void CachingAllocator::resetPeaks() noexcept {
    impl_->st.peakAllocatedBytes = impl_->st.allocatedBytes;
    impl_->st.peakSegmentBytes = impl_->st.segmentBytes;
}

}  // namespace mem
