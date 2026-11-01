// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/size_class_pool.hpp"
#include "mini_test.hpp"
#include "sys/proc.hpp"

using namespace std::chrono_literals;
using mem::PoolOptions;
using mem::SizeClassPool;

namespace {
std::atomic<int> g_misuse{0};
void countMisuse(void*, const char*) { ++g_misuse; }

PoolOptions opts() {
    PoolOptions o;
    o.classes = mem::ClassTable::pow2(256, 8u << 20);
    o.smallLimit = 16 << 10;
    o.slabBytes = 256 << 10;
    o.shards = 4;
    o.retainLargeBytes = 16 << 20;
    o.releaseIdleAfter = 20ms;
    o.hooks.onMisuse = countMisuse;
    o.hooks.abortOnMisuse = false;
    return o;
}
}  // namespace

TEST(capacitiesAndAlignment) {
    SizeClassPool pool(opts());
    for (std::size_t n : {1u, 200u, 256u, 257u, 5000u, 16384u, 16385u, 100000u, 1u << 20, 8u << 20}) {
        auto b = pool.acquire(n);
        CHECK(b.data != nullptr);
        CHECK(b.capacity >= n);
        CHECK(b.capacity < 2 * std::max<std::size_t>(n, 256));
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(b.data) % 64, 0u);
        if (b.capacity > (16 << 10))
            CHECK_EQ(reinterpret_cast<std::uintptr_t>(b.data) % 4096, 0u);   // large blocks are page aligned
        std::memset(b.data, 0x11, b.capacity);   // the whole capacity is writable
        pool.release(b);
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    CHECK(!pool.tryAcquire((8u << 20) + 1).data);
    bool threw = false;
    try { pool.acquire((8u << 20) + 1); } catch (const std::bad_alloc&) { threw = true; }
    CHECK(threw);
}

TEST(largeBlocksAreReusedNotRemapped) {
    SizeClassPool pool(opts());
    auto a = pool.acquire(1 << 20);
    void* addr = a.data;
    pool.release(a);
    const auto maps = pool.stats().mapCalls;
    auto b = pool.acquire(900 << 10);
    CHECK(b.data == addr);
    CHECK_EQ(pool.stats().mapCalls, maps);
    pool.release(b);
}

TEST(retentionCapUnmapsTheExcess) {
    PoolOptions o = opts();
    o.retainLargeBytes = 4 << 20;
    SizeClassPool pool(o);
    std::vector<SizeClassPool::Block> blocks;
    for (int i = 0; i < 8; ++i)
        blocks.push_back(pool.acquire(2 << 20));
    for (auto& b : blocks)
        pool.release(b);
    const auto s = pool.stats();
    CHECK(s.retainedBytes <= (4u << 20));
    CHECK(s.unmapCalls >= 6u);
    CHECK(s.mappedBytes <= (4u << 20) + 4096);
}

TEST(idleLargeBlocksGivePagesBackButKeepTheMapping) {
    SizeClassPool pool(opts());
    auto b = pool.acquire(8 << 20);
    std::memset(b.data, 1, b.capacity);
    void* addr = b.data;
    const std::size_t resident = sys::residentBytes();
    pool.release(b);
    std::this_thread::sleep_for(60ms);
    pool.trim();
    CHECK(sys::residentBytes() + (6u << 20) < resident);
    auto again = pool.acquire(8 << 20);
    CHECK(again.data == addr);                               // same mapping
    CHECK_EQ(static_cast<unsigned char*>(again.data)[4096], 0);   // and it reads as zero again
    pool.release(again);
}

TEST(misuseIsReportedNotFatal) {
    SizeClassPool pool(opts());
    g_misuse = 0;
    auto small = pool.acquire(300);
    auto big = pool.acquire(1 << 20);
    pool.release(small);
    pool.release(small);                                      // double free (slab)
    pool.release(big);
    pool.release(big);                                        // double free (large)
    pool.release({big.data, big.capacity / 2});               // wrong capacity for a large block
    int local;
    pool.release({&local, 512});                              // foreign pointer
    pool.release({&local, 12345});                            // capacity not a class
    CHECK_EQ(g_misuse.load(), 5);
    CHECK_EQ(pool.stats().misuse, 5u);
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(sealBorrowsFromLargerClassesOrRefuses) {
    PoolOptions o = opts();
    o.borrowClasses = 2;
    SizeClassPool pool(o);
    pool.prewarm(pool.classes().classOf(1024), 1);            // one 1 KiB... slab holds many
    pool.prewarm(pool.classes().classOf(4096), 4);
    pool.seal();
    // 4096-class blocks already exist inside a mapped slab; 2048 asks borrow from it when 2048 runs dry.
    std::vector<SizeClassPool::Block> got;
    for (int i = 0; i < 100000; ++i) {
        auto b = pool.tryAcquire(2048);
        if (!b.data)
            break;
        got.push_back(b);
    }
    CHECK(pool.stats().borrowed > 0u);
    CHECK(pool.stats().refused > 0u);
    for (auto& b : got)
        pool.release(b);                                      // borrowed blocks go home to their own class
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    CHECK(!pool.tryAcquire(1 << 20).data);                    // sealed: no new large mapping either
}

TEST(quotaStopsGrowth) {
    PoolOptions o = opts();
    o.maxBytes = 3 << 20;
    SizeClassPool pool(o);
    std::vector<SizeClassPool::Block> held;
    while (auto b = pool.tryAcquire(1 << 20))
        held.push_back(b);
    CHECK_EQ(held.size(), 3u);
    for (auto& b : held)
        pool.release(b);
}

TEST(bufferLeaseReturnsItself) {
    SizeClassPool pool(opts());
    {
        mem::Buffer a(pool, 5000);
        mem::Buffer b(std::move(a));
        CHECK(!a);
        CHECK(b.capacity() >= 5000);
        CHECK_EQ(pool.stats().blocksInUse, 1u);
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(statsFormatters) {
    SizeClassPool pool(opts());
    auto b = pool.acquire(100);
    char buf[1024];
    const std::size_t n = pool.stats().toJson(buf, sizeof buf);
    CHECK(n < sizeof buf);
    CHECK(std::strstr(buf, "\"blocksInUse\":1") != nullptr);
    const std::size_t m = pool.stats().toPrometheus(buf, sizeof buf, "x_");
    CHECK(m < sizeof buf);
    CHECK(std::strstr(buf, "x_blocks_in_use 1") != nullptr);
    CHECK(pool.stats().toJson(nullptr, 0) == n);              // size query
    pool.release(b);
}

TEST(randomSizesFromManyThreadsKeepInvariants) {
    PoolOptions o = opts();
    SizeClassPool pool(o);
    constexpr int kThreads = 8;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::vector<std::pair<SizeClassPool::Block, unsigned char>> held;
            std::uint64_t rng = 0x2545F4914F6CDD1Dull * (t + 1);
            while (std::chrono::steady_clock::now() < deadline) {
                for (int i = 0; i < 200; ++i) {
                    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                    if (held.size() < 32 && (rng & 3) != 0) {
                        const std::size_t size = 1 + (rng >> 8) % (rng & 64 ? 40000 : 3000000);
                        auto b = pool.tryAcquire(size);
                        if (!b.data) { ++failures; continue; }
                        const unsigned char tag = static_cast<unsigned char>(t * 16 + 1);
                        std::memset(b.data, tag, std::min<std::size_t>(b.capacity, 4096));
                        held.push_back({b, tag});
                    } else if (!held.empty()) {
                        const std::size_t k = rng % held.size();
                        auto [b, tag] = held[k];
                        for (std::size_t x = 0; x < std::min<std::size_t>(b.capacity, 4096); x += 61)
                            if (static_cast<unsigned char*>(b.data)[x] != tag) { ++failures; break; }
                        held[k] = held.back();
                        held.pop_back();
                        pool.release(b);
                    }
                }
            }
            for (auto& [b, tag] : held) pool.release(b);
        });
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
    const auto s = pool.stats();
    CHECK_EQ(s.blocksInUse, 0u);
    CHECK_EQ(s.misuse, 0u);
    CHECK(s.mappedBytes <= (1u << 30));
}

TEST(steadyStateAllocatesNothingAndMapsNothing) {
    SizeClassPool pool(opts());
    for (std::size_t n : {300u, 4000u, 70000u, 3000000u})
        pool.prewarm(pool.classes().classOf(n), 8);
    const auto before = pool.stats();
    {
        tst::AllocScope scope;
        for (int i = 0; i < 200000; ++i)
            for (std::size_t n : {300u, 4000u, 70000u, 3000000u}) {
                auto b = pool.acquire(n);
                static_cast<char*>(b.data)[0] = 1;
                pool.release(b);
            }
        CHECK_EQ(scope.calls(), 0ull);
    }
    const auto after = pool.stats();
    CHECK_EQ(after.mapCalls, before.mapCalls);
    CHECK_EQ(after.unmapCalls, before.unmapCalls);
}

int main() { return tst::runAll("size_class_pool"); }
