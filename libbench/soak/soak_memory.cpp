// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Soak test of the memory and scheduling stack: every thread mixes SizeClassPool buffers (small and large), Arena
// rounds, CachingAllocator blocks with drifting sizes, ObjectPool objects, logger lines and thread-pool tasks.
// Fails when RSS or any pool gauge keeps growing after warm-up.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>

#include "bench/soak.hpp"
#include "lg/logger.hpp"
#include "mem/arena.hpp"
#include "mem/caching_allocator.hpp"
#include "mem/object_pool.hpp"
#include "mem/size_class_pool.hpp"
#include "sched/thread_pool.hpp"
#include "sys/proc.hpp"

namespace {
struct Session {
    char data[200];
    std::uint64_t id;
};
}  // namespace

int main() {
    sys::tuneAllocator();
    mem::SizeClassPool pool;
    mem::ObjectPool<Session> sessions;
    mem::CachingOptions co;
    co.maxCachedBytes = 64u << 20;
    co.maxBlocks = 1 << 16;
    mem::CachingAllocator caching(mem::hostVmBacking(), co);
    std::mutex cachingMu;
    sched::ThreadPool workers({2, 1024});
    lg::LoggerOptions lo;
    lo.level = lg::Level::Info;
    lg::Logger log(lo);
    log.addSink(lg::makeCallbackSink([](lg::Level, const char*, std::size_t) {}));
    std::atomic<std::uint64_t> taskCount{0};

    bench::SoakOptions so;
    so.duration = bench::soakSeconds(std::chrono::seconds(20));
    so.threads = 4;
    so.gauges.push_back({"pool_mapped_bytes", [&] { return pool.stats().mappedBytes; }});
    so.gauges.push_back({"pool_blocks_in_use", [&] { return pool.stats().blocksInUse; }});
    so.gauges.push_back({"caching_segment_bytes", [&] { std::lock_guard<std::mutex> g(cachingMu); return caching.stats().segmentBytes; }});
    so.gauges.push_back({"sessions_in_use", [&] { return sessions.stats().blocksInUse; }});

    const bench::SoakResult r = bench::runSoak(so, [&](unsigned t, const std::atomic<bool>& stop) -> std::uint64_t {
        std::mt19937_64 rng(1234 + t);
        mem::Arena arena({&pool, 64 << 10, 4});
        std::vector<mem::SizeClassPool::Block> held;
        std::vector<mem::CachingAllocator::Block> cheld;
        std::vector<Session*> sheld;
        std::uint64_t it = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++it;
            // Slowly drifting size distribution: the workload that makes naive caches grow.
            const std::size_t drift = 1 + (it / 200000) % 7;
            switch (rng() % 8) {
                case 0: case 1: {
                    if (held.size() < 48) {
                        auto b = pool.tryAcquire(1 + rng() % (drift * 20000));
                        if (b.data) { std::memset(b.data, 1, std::min<std::size_t>(b.capacity, 256)); held.push_back(b); }
                    } else {
                        const std::size_t k = rng() % held.size();
                        pool.release(held[k]);
                        held[k] = held.back();
                        held.pop_back();
                    }
                    break;
                }
                case 2: {   // occasional large buffer
                    auto b = pool.tryAcquire((1 + rng() % 16) << 20);
                    if (b.data) { static_cast<char*>(b.data)[0] = 1; pool.release(b); }
                    break;
                }
                case 3: {   // an arena "statement"
                    mem::Arena::Scope scope(arena);
                    for (int i = 0; i < 20; ++i) arena.allocate(1 + rng() % 2000);
                    break;
                }
                case 4: {
                    std::lock_guard<std::mutex> g(cachingMu);
                    if (cheld.size() < 40) {
                        auto b = caching.allocate((1 + rng() % (drift * 600)) * 512 + rng() % 512);
                        if (b) cheld.push_back(b);
                    } else {
                        const std::size_t k = rng() % cheld.size();
                        caching.free(cheld[k]);
                        cheld[k] = cheld.back();
                        cheld.pop_back();
                    }
                    break;
                }
                case 5: {
                    if (sheld.size() < 64) {
                        if (Session* s = sessions.acquire()) sheld.push_back(s);
                    } else {
                        const std::size_t k = rng() % sheld.size();
                        sessions.release(sheld[k]);
                        sheld[k] = sheld.back();
                        sheld.pop_back();
                    }
                    break;
                }
                case 6:
                    log.write(lg::Level::Info, "thread %u iteration %llu drift %zu", t, (unsigned long long)it, drift);
                    break;
                default:
                    workers.tryEnqueue([&taskCount] { ++taskCount; });
                    break;
            }
        }
        for (auto& b : held) pool.release(b);
        {
            std::lock_guard<std::mutex> g(cachingMu);
            for (auto& b : cheld) caching.free(b);
        }
        for (Session* s : sheld) sessions.release(s);
        return it;
    });
    workers.shutdown();
    log.flush();
    std::fputs(r.report().c_str(), stdout);

    // Criteria: RSS may not grow by more than 2 MiB per minute after warm-up, and no gauge's windowed maximum may
    // grow by more than 15 % from the first to the last third of the measured period.
    bool ok = true;
    const bench::SoakSeries* rss = r.find("rss_bytes");
    if (rss->slopePerMinute > 2.0 * 1024 * 1024) {
        std::printf("FAIL: RSS grows %.1f KiB/min\n", rss->slopePerMinute / 1024);
        ok = false;
    }
    for (const char* gauge : {"rss_bytes", "pool_mapped_bytes", "caching_segment_bytes"}) {
        const bench::SoakSeries* s = r.find(gauge);
        if (s->windowGrowthPercent > 15.0) {
            std::printf("FAIL: %s windowed maximum grows %.1f %%\n", gauge, s->windowGrowthPercent);
            ok = false;
        }
    }
    const auto st = pool.stats();
    if (st.blocksInUse != 0 || sessions.stats().blocksInUse != 0 || st.misuse != 0) {
        std::printf("FAIL: leak or misuse (pool in use %llu, sessions %llu, misuse %llu)\n", (unsigned long long)st.blocksInUse,
                    (unsigned long long)sessions.stats().blocksInUse, (unsigned long long)st.misuse);
        ok = false;
    }
    std::printf("tasks run: %llu, log lines written/dropped: %llu/%llu\n", (unsigned long long)taskCount.load(),
                (unsigned long long)log.writtenLines(), (unsigned long long)log.droppedLines());
    std::printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
