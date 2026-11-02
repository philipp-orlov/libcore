// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/slab_pool.hpp"
#include "mini_test.hpp"

using mem::SlabPool;
using mem::SlabPoolOptions;

namespace {
std::atomic<int> g_misuse{0};
void countMisuse(void*, const char*) { ++g_misuse; }

SlabPoolOptions opts(std::size_t block = 256) {
    SlabPoolOptions o;
    o.blockBytes = block;
    o.slabBytes = 64 << 10;
    o.shards = 4;
    o.maxSlabs = 64;
    o.hooks.onMisuse = countMisuse;
    o.hooks.abortOnMisuse = false;
    return o;
}
}  // namespace

TEST(blocksAreAlignedDistinctAndReusable) {
    SlabPool pool(opts());
    std::set<void*> seen;
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* p = pool.tryAcquire();
        CHECK(p != nullptr);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, 0u);
        CHECK(seen.insert(p).second);
        std::memset(p, 0xAB, 256);
        blocks.push_back(p);
    }
    CHECK_EQ(pool.stats().blocksInUse, 1000u);
    for (void* p : blocks)
        CHECK(pool.release(p));
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    const auto mapsBefore = pool.stats().mapCalls;
    for (int i = 0; i < 1000; ++i)
        CHECK(pool.tryAcquire() != nullptr);   // all served from the free lists
    CHECK_EQ(pool.stats().mapCalls, mapsBefore);
}

TEST(misuseIsDetectedAndContained) {
    SlabPool pool(opts());
    g_misuse = 0;
    void* p = pool.tryAcquire();
    CHECK(pool.release(p));
    CHECK(!pool.release(p));                                  // double free
    CHECK_EQ(g_misuse.load(), 1);
    int onStack = 0;
    CHECK(!pool.release(&onStack));                           // foreign pointer
    CHECK(!pool.release(nullptr));                            // null is foreign too
    void* q = pool.tryAcquire();
    CHECK(!pool.release(static_cast<char*>(q) + 8));          // inside a block
    CHECK_EQ(g_misuse.load(), 4);
    CHECK(pool.release(q));
    // The free list is intact: a double free did not push the block twice.
    void* a = pool.tryAcquire();
    void* b = pool.tryAcquire();
    CHECK(a != b);
    CHECK_EQ(pool.stats().misuse, 4u);
}

TEST(quotaAndSeal) {
    SlabPoolOptions o = opts();
    o.maxBytes = 2 * o.slabBytes;
    SlabPool pool(o);
    std::vector<void*> blocks;
    while (void* p = pool.tryAcquire())
        blocks.push_back(p);
    CHECK(blocks.size() > pool.blocksPerSlab());
    CHECK(blocks.size() <= 2 * pool.blocksPerSlab());
    CHECK(pool.stats().refused >= 1u);
    for (void* p : blocks)
        pool.release(p);

    SlabPool sealed(opts());
    CHECK(sealed.prewarm(100));
    sealed.seal();
    const auto maps = sealed.stats().mapCalls;
    std::vector<void*> got;
    for (std::size_t i = 0; i < sealed.blocksPerSlab() * 2; ++i)
        if (void* p = sealed.tryAcquire())
            got.push_back(p);
    CHECK_EQ(sealed.stats().mapCalls, maps);                  // no new slab after seal
    CHECK(got.size() >= 100 && got.size() <= sealed.blocksPerSlab() * 2);
}

TEST(manyThreadsNeverShareABlock) {
    SlabPool pool(opts(128));
    constexpr int kThreads = 8, kRounds = 200000;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::vector<unsigned char*> held;
            std::uint64_t rng = 0x9E3779B97F4A7C15ull * (t + 1);
            for (int i = 0; i < kRounds; ++i) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                if (held.size() < 64 && (rng & 3) != 0) {
                    auto* p = static_cast<unsigned char*>(pool.tryAcquire());
                    if (!p) { ++failures; continue; }
                    std::memset(p, t + 1, 128);
                    held.push_back(p);
                } else if (!held.empty()) {
                    const std::size_t k = rng % held.size();
                    unsigned char* p = held[k];
                    for (int b = 0; b < 128; ++b)
                        if (p[b] != t + 1) { ++failures; break; }   // someone else wrote into our block
                    held[k] = held.back();
                    held.pop_back();
                    if (!pool.release(p)) ++failures;
                }
            }
            for (auto* p : held) pool.release(p);
        });
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    CHECK_EQ(pool.stats().misuse, 0u);
    // The pool needed only what the peak required.
    CHECK(pool.slabCount() * pool.blocksPerSlab() <= (kThreads * 64 + pool.blocksPerSlab() * 2 * 4));
}

TEST(steadyStateAllocatesNothing) {
    SlabPool pool(opts());
    CHECK(pool.prewarm(512));
    std::vector<void*> held;
    held.reserve(64);
    {
        tst::AllocScope scope;
        for (int i = 0; i < 1000000; ++i) {
            held.push_back(pool.tryAcquire());
            if (held.size() == 64) {
                for (void* p : held) pool.release(p);
                held.clear();
            }
        }
        for (void* p : held) pool.release(p);
        CHECK_EQ(scope.calls(), 0ull);
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(threadExitReturnsCachedBlocksToThePool) {
    SlabPool pool(opts());
    std::thread([&] {
        std::vector<void*> v;
        for (int i = 0; i < 5; ++i) v.push_back(pool.tryAcquire());
        for (void* p : v) pool.release(p);   // all end up in this thread's cache
    }).join();
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    CHECK_EQ(pool.stats().acquires, 5u);
    const auto maps = pool.stats().mapCalls;
    std::thread([&] {
        for (int i = 0; i < 5; ++i) CHECK(pool.tryAcquire() != nullptr);
    }).join();
    CHECK_EQ(pool.stats().mapCalls, maps);   // the blocks came back through the shared free list, no new slab
}

TEST(poolsComeAndGoWhileThreadsKeepStaleCaches) {
    // Same thread, many short-lived pools: each pool's slot in the thread cache is taken over by the next one.
    for (int i = 0; i < 300; ++i) {
        SlabPool pool(opts());
        void* a = pool.tryAcquire();
        void* b = pool.tryAcquire();
        pool.release(a);
        (void)b;   // deliberately leaked into the destroyed pool
    }
    // A long-lived worker keeps its slot (and one cached block) for a pool that is destroyed under it.
    std::atomic<bool> used{false}, go{false};
    auto pool = std::make_unique<SlabPool>(opts());
    std::thread t([&, raw = pool.get()] {
        void* b = raw->tryAcquire();
        raw->release(b);                     // cached in this thread's slot
        used.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!used.load(std::memory_order_acquire)) std::this_thread::yield();
    pool.reset();                            // t no longer touches the pool, but its slot still points at it
    go.store(true, std::memory_order_release);
    t.join();                                // t's exit hook must see the pool is dead and drop the pointers
    CHECK(true);
}

TEST(releasesDuringThreadTeardownDoNotRebindTheCache) {
    // A thread_local whose destructor releases a block runs after (or before) the pool's own thread cache is torn down,
    // depending on construction order; either order must be safe, and the pool must stay consistent afterwards.
    SlabPool pool(opts());
    struct Late {
        SlabPool* pool;
        void* block = nullptr;
        ~Late() { if (block) pool->release(block); }
    };
    for (int round = 0; round < 50; ++round) {
        std::thread([&] {
            static thread_local Late late{&pool};      // constructed on first use, destroyed in reverse order of construction
            late.pool = &pool;
            void* first = pool.tryAcquire();            // creates this thread's cache slot after `late`
            late.block = pool.tryAcquire();
            pool.release(first);
        }).join();
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
    CHECK_EQ(pool.stats().misuse, 0u);
}

TEST(producerAndConsumerOnDifferentThreadsDoNotGrowThePool) {
    SlabPool pool(opts());
    constexpr int kBlocks = 20000;
    std::vector<void*> queue(kBlocks);
    std::atomic<int> produced{0}, consumed{0};
    std::thread consumer([&] {
        for (int i = 0; i < kBlocks; ++i) {
            while (produced.load(std::memory_order_acquire) <= i) std::this_thread::yield();
            CHECK(pool.release(queue[i]));
            consumed.store(i + 1, std::memory_order_release);
        }
    });
    for (int i = 0; i < kBlocks; ++i) {
        while (i - consumed.load(std::memory_order_acquire) > 200) std::this_thread::yield();   // at most ~200 blocks in flight
        queue[i] = pool.tryAcquire();
        CHECK(queue[i] != nullptr);
        produced.store(i + 1, std::memory_order_release);
    }
    consumer.join();
    // 200 in flight + two threads' magazines + a margin: a few slabs, not one per refill.
    CHECK(pool.slabCount() <= 4);
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

int main() { return tst::runAll("slab_pool"); }
