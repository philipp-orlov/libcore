// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>
#include <vector>

#include "alloc_counter.hpp"
#include "mem/caching_allocator.hpp"
#include "mini_test.hpp"

using mem::CachingAllocator;
using mem::CachingOptions;

namespace {
std::atomic<int> g_misuse{0};
void countMisuse(void*, const char*) { ++g_misuse; }
CachingOptions base() {
    CachingOptions o;
    o.maxBlocks = 4096;
    o.maxSegments = 256;
    o.hooks.onMisuse = countMisuse;
    o.hooks.abortOnMisuse = false;
    return o;
}
constexpr std::size_t MiB = 1u << 20;
}  // namespace

TEST(roundingAndPools) {
    CachingAllocator a(mem::hostVmBacking(), base());
    auto small = a.allocate(1000);
    CHECK_EQ(small.size, 1024u);                    // 512 B granule
    auto large = a.allocate(3 * MiB + 1);
    CHECK_EQ(large.size, 4 * MiB);                  // 2 MiB granule
    std::memset(small.ptr, 1, small.size);
    std::memset(large.ptr, 2, large.size);
    a.free(small);
    a.free(large);
    CHECK_EQ(a.stats().allocatedBytes, 0u);
}

TEST(smallBlocksShareASegmentAndCoalesceBack) {
    CachingAllocator a(mem::hostVmBacking(), base());
    std::vector<CachingAllocator::Block> v;
    for (int i = 0; i < 100; ++i) v.push_back(a.allocate(10000));
    CHECK_EQ(a.stats().segments, 1u);               // 100 x 10 KiB fit in one 2 MiB segment
    for (auto& b : v) a.free(b);
    const auto s = a.stats();
    CHECK_EQ(s.allocatedBytes, 0u);
    CHECK_EQ(s.cachedBytes, s.segmentBytes);
    CHECK(s.merges >= 99u);
    // Everything merged back into one block: a request for almost the whole segment fits without a new segment.
    auto big = a.allocate(MiB);
    auto big2 = a.allocate(MiB);
    CHECK(big && big2);
    CHECK_EQ(a.stats().segments, 1u);
    CHECK_EQ(a.stats().backingAllocs, 1u);
}

TEST(bestFitPicksTheSmallestHole) {
    CachingAllocator a(mem::hostVmBacking(), base());
    auto b1 = a.allocate(8192);
    auto sep1 = a.allocate(512);
    auto b2 = a.allocate(2048);
    auto sep2 = a.allocate(512);
    auto b3 = a.allocate(4096);
    auto sep3 = a.allocate(512);
    a.free(b1); a.free(b2); a.free(b3);              // holes of 8192, 2048, 4096 separated by live blocks
    auto fit = a.allocate(2000);
    CHECK(fit.ptr == b2.ptr);                        // the 2048 hole, not the 8192 one
    auto fit2 = a.allocate(3000);
    CHECK(fit2.ptr == b3.ptr);                       // the 4096 hole
    for (auto b : {fit, fit2, sep1, sep2, sep3}) a.free(b);
}

TEST(streamsDoNotShareFreedBlocks) {
    CachingAllocator a(mem::hostVmBacking(), base());
    auto x = a.allocate(1 << 16, 1);
    void* where = x.ptr;
    a.free(x);
    auto y = a.allocate(1 << 16, 2);                 // another stream must not get stream 1's block
    CHECK(y.ptr != where);
    auto z = a.allocate(1 << 16, 1);
    CHECK(z.ptr == where);                           // its own stream does
    a.free(y); a.free(z);
}

namespace {
bool g_done[4];
bool isDone(CachingAllocator::StreamId s, void*) { return g_done[s]; }
}  // namespace

TEST(crossStreamUseDefersTheFree) {
    CachingAllocator a(mem::hostVmBacking(), base());
    auto x = a.allocate(1 << 16, 1);
    void* where = x.ptr;
    a.recordUse(x, 2);
    a.free(x);                                       // stream 2 may still be reading it
    auto y = a.allocate(1 << 16, 1);
    CHECK(y.ptr != where);
    g_done[2] = false;
    CHECK_EQ(a.reclaimDeferred(&isDone, nullptr), 0u);
    g_done[2] = true;
    CHECK_EQ(a.reclaimDeferred(&isDone, nullptr), 1u);
    auto z = a.allocate(1 << 16, 1);
    CHECK(z.ptr == where);
    a.free(y); a.free(z);
    CHECK_EQ(a.stats().allocatedBytes, 0u);
}

TEST(misuseIsReportedNotFatal) {
    CachingAllocator a(mem::hostVmBacking(), base());
    g_misuse = 0;
    auto x = a.allocate(1000);
    a.free(x);
    a.free(x);                                       // double free
    CachingAllocator::Block forged{x.ptr, x.size, 12345, 1};
    a.free(forged);
    a.free({});
    CHECK_EQ(g_misuse.load(), 3);
    auto y = a.allocate(1000);                       // the old handle must stay dead even though the node is reused
    a.free(x);
    CHECK_EQ(g_misuse.load(), 4);
    a.free(y);
}

TEST(budgetFreesIdleSegmentsThenRefuses) {
    CachingOptions o = base();
    o.budgetBytes = 8 * MiB;
    CachingAllocator a(mem::hostVmBacking(), o);
    auto b1 = a.allocate(4 * MiB);
    auto b2 = a.allocate(4 * MiB);
    CHECK(b1 && b2);
    CHECK(!a.allocate(2 * MiB));                     // at the budget with nothing idle
    CHECK_EQ(a.stats().refused, 1u);
    a.free(b1);
    auto b3 = a.allocate(2 * MiB);                   // reuses part of b1's cached block
    CHECK(b3);
    a.free(b2); a.free(b3);
    CHECK(a.allocate(8 * MiB));                      // idle segments are released, then the 8 MiB request fits
    CHECK(a.stats().segmentBytes <= 8 * MiB);
}

TEST(retentionCapReleasesOldestIdleSegments) {
    CachingOptions o = base();
    o.maxCachedBytes = 6 * MiB;
    CachingAllocator a(mem::hostVmBacking(), o);
    std::vector<CachingAllocator::Block> v;
    for (int i = 0; i < 10; ++i) v.push_back(a.allocate(4 * MiB));
    for (auto& b : v) a.free(b);
    const auto s = a.stats();
    CHECK(s.cachedBytes <= 6 * MiB);
    CHECK(s.backingFrees >= 8u);
    CHECK_EQ(s.allocatedBytes, 0u);
    CHECK_EQ(s.segmentBytes, s.cachedBytes);
}

TEST(emptyCacheKeepsLiveBlocks) {
    CachingAllocator a(mem::hostVmBacking(), base());
    auto live = a.allocate(3 * MiB);
    auto gone = a.allocate(5 * MiB);
    std::memset(live.ptr, 7, live.size);
    a.free(gone);
    CHECK(a.emptyCache() >= 6 * MiB);
    CHECK_EQ(static_cast<unsigned char*>(live.ptr)[100], 7);
    CHECK_EQ(a.stats().segmentBytes, 4 * MiB);
    a.free(live);
}

TEST(noHeapAfterConstruction) {
    CachingAllocator a(mem::hostVmBacking(), base());
    std::vector<CachingAllocator::Block> v;
    v.reserve(64);
    tst::AllocScope scope;
    std::mt19937_64 rng(3);
    for (int i = 0; i < 200000; ++i) {
        if (v.size() < 50 && (rng() & 1)) {
            auto b = a.allocate(1 + rng() % (3 * MiB));
            if (b) v.push_back(b);
        } else if (!v.empty()) {
            const std::size_t k = rng() % v.size();
            a.free(v[k]);
            v[k] = v.back();
            v.pop_back();
        }
    }
    for (auto& b : v) a.free(b);
    CHECK_EQ(scope.calls(), 0ull);
}

// The workload that makes exact-size free lists grow without bound: tensor shapes that drift over time.
TEST(driftingShapesDoNotGrowTheCache) {
    CachingOptions o = base();
    o.maxBlocks = 1 << 16;
    o.maxSegments = 4096;
    CachingAllocator a(mem::hostVmBacking(), o);
    std::mt19937_64 rng(11);
    std::vector<CachingAllocator::Block> live;
    constexpr int kPhases = 6, kOps = 150000;
    std::uint64_t worstRatioPct = 0;
    for (int phase = 0; phase < kPhases; ++phase) {
        // Each phase draws from a different size distribution (sizes in 512-byte units, never repeating exactly).
        const std::size_t lo = 1 + phase * 7, span = 200 + phase * 311;
        a.resetPeaks();
        for (int i = 0; i < kOps; ++i) {
            if (live.size() < 120 && (rng() % 100) < 52) {
                const std::size_t bytes = (lo + rng() % span) * 512 + rng() % 512;
                auto b = a.allocate(bytes);
                if (b) live.push_back(b);
            } else if (!live.empty()) {
                const std::size_t k = rng() % live.size();
                a.free(live[k]);
                live[k] = live.back();
                live.pop_back();
            }
        }
        const auto s = a.stats();
        worstRatioPct = std::max<std::uint64_t>(worstRatioPct, s.segmentBytes * 100 / std::max<std::uint64_t>(1, s.peakAllocatedBytes));
        std::fprintf(stderr, "  phase %d: peakAllocated %llu KiB, segments %llu KiB (%llu%%), segs=%llu\n", phase,
                     (unsigned long long)(s.peakAllocatedBytes >> 10), (unsigned long long)(s.segmentBytes >> 10),
                     (unsigned long long)(s.segmentBytes * 100 / std::max<std::uint64_t>(1, s.peakAllocatedBytes)),
                     (unsigned long long)s.segments);
    }
    for (auto& b : live) a.free(b);
    live.clear();
    a.emptyCache();
    CHECK_EQ(a.stats().segmentBytes, 0u);
    CHECK(worstRatioPct <= 200);   // measured 114-123 %; the old exact-size lists grow without bound on this workload
}

int main() { return tst::runAll("caching_allocator"); }
