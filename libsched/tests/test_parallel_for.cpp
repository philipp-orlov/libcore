// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mini_test.hpp"
#include "sched/parallel_for.hpp"

TEST(coversTheRangeExactlyOnce) {
    sched::ThreadPool pool({3, 64});
    for (std::size_t n : {0u, 1u, 2u, 7u, 100u, 12345u}) {
        for (std::size_t grain : {1u, 5u, 64u, 100000u}) {
            std::vector<std::atomic<int>> hits(n);
            sched::parallelFor(pool, 0, n, grain, [&](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) hits[i].fetch_add(1);
            });
            for (std::size_t i = 0; i < n; ++i) CHECK_EQ(hits[i].load(), 1);
        }
    }
    std::vector<std::atomic<int>> offset(50);
    sched::parallelFor(pool, 1000, 1050, 1, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) offset[i - 1000].fetch_add(1);
    });
    for (auto& h : offset) CHECK_EQ(h.load(), 1);
}

TEST(resultsAreIdenticalForAnyPoolSize) {
    // The split depends only on the pool size, and the sum of per-piece sums is independent of it.
    std::vector<double> v(100000);
    std::iota(v.begin(), v.end(), 0.0);
    double reference = 0;
    for (double x : v) reference += x;
    for (unsigned threads : {1u, 2u, 7u}) {
        sched::ThreadPool pool({threads, 64});
        std::atomic<long long> total{0};
        sched::parallelFor(pool, 0, v.size(), 1000, [&](std::size_t lo, std::size_t hi) {
            long long s = 0;
            for (std::size_t i = lo; i < hi; ++i) s += static_cast<long long>(v[i]);
            total += s;
        });
        CHECK_EQ(static_cast<double>(total.load()), reference);
    }
}

TEST(nestedCallRunsSeriallyWithoutDeadlock) {
    sched::ThreadPool pool({2, 64});
    std::atomic<int> inner{0};
    sched::parallelFor(pool, 0, 8, 1, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i)
            sched::parallelFor(pool, 0, 10, 1, [&](std::size_t a, std::size_t b) { inner += static_cast<int>(b - a); });
    });
    CHECK_EQ(inner.load(), 80);
}

TEST(exceptionIsRethrownAndRegionFlagIsReset) {
    sched::ThreadPool pool({3, 64});
    bool caught = false;
    try {
        sched::parallelFor(pool, 0, 1000, 10, [&](std::size_t lo, std::size_t) {
            if (lo >= 500) throw std::runtime_error("piece failed");
        });
    } catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);
    // The next call must still run in parallel (the flag was not left set): check via worker participation.
    std::atomic<int> workerPieces{0};
    sched::parallelFor(pool, 0, 4000, 1, [&](std::size_t, std::size_t) {
        if (sched::ThreadPool::onWorkerThread()) ++workerPieces;
    });
    CHECK(workerPieces.load() > 0);
}

TEST(rejectedPiecesRunOnTheCaller) {
    sched::ThreadPool pool({1, 2});
    std::atomic<bool> release{false}, started{false};
    pool.tryEnqueue([&] { started = true; while (!release) std::this_thread::yield(); });
    while (!started) std::this_thread::yield();
    pool.tryEnqueue([] {}); pool.tryEnqueue([] {});            // queue full
    std::atomic<int> done{0};
    sched::parallelFor(pool, 0, 100, 1, [&](std::size_t lo, std::size_t hi) { done += static_cast<int>(hi - lo); });
    CHECK_EQ(done.load(), 100);
    release = true;
}

TEST(noHeapPerCall) {
    sched::ThreadPool pool({3, 64});
    std::atomic<int> sink{0};
    sched::parallelFor(pool, 0, 100, 1, [&](std::size_t, std::size_t) { ++sink; });
    tst::AllocScope scope;
    for (int i = 0; i < 20000; ++i)
        sched::parallelFor(pool, 0, 1000, 10, [&](std::size_t lo, std::size_t hi) { sink += static_cast<int>(hi - lo); });
    CHECK_EQ(scope.calls(), 0ull);
}

int main() { return tst::runAll("parallel_for"); }
