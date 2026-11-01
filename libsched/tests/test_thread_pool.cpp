// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "alloc_counter.hpp"
#include "mini_test.hpp"
#include "sched/thread_pool.hpp"

using namespace std::chrono_literals;

TEST(everySubmittedTaskRunsExactlyOnce) {
    sched::ThreadPool pool({4, 1024});
    std::atomic<std::uint64_t> sum{0}, count{0};
    constexpr int kSubmitters = 16, kEach = 60000;
    std::vector<std::thread> ts;
    for (int s = 0; s < kSubmitters; ++s)
        ts.emplace_back([&] {
            for (int i = 1; i <= kEach; ++i) {
                while (!pool.tryEnqueue([&, i] { sum += i; ++count; }))
                    std::this_thread::yield();
            }
        });
    for (auto& t : ts) t.join();
    pool.shutdown();   // drains
    CHECK_EQ(count.load(), std::uint64_t{kSubmitters} * kEach);
    CHECK_EQ(sum.load(), std::uint64_t{kSubmitters} * (std::uint64_t{kEach} * (kEach + 1) / 2));
    CHECK_EQ(pool.executedTasks(), std::uint64_t{kSubmitters} * kEach);
}

TEST(fullQueueRejectsWithoutRunning) {
    sched::ThreadPool pool({1, 4});
    std::atomic<bool> release{false};
    std::atomic<int> started{0}, ran{0};
    CHECK(pool.tryEnqueue([&] { ++started; while (!release) std::this_thread::yield(); ++ran; }));
    while (started == 0) std::this_thread::yield();           // the single worker is now busy
    int accepted = 0;
    for (int i = 0; i < 20; ++i)
        if (pool.tryEnqueue([&] { ++ran; })) ++accepted;
    CHECK_EQ(accepted, 4);                                    // capacity 4
    CHECK_EQ(pool.rejectedTasks(), 16u);
    release = true;
    pool.shutdown();
    CHECK_EQ(ran.load(), 1 + 4);                              // rejected tasks never ran
    CHECK(!pool.tryEnqueue([] {}));                           // stopped
}

TEST(throwingTaskIsCountedAndWorkerSurvives) {
    sched::ThreadPool pool({1, 16});
    std::atomic<int> after{0};
    pool.tryEnqueue([] { throw std::runtime_error("boom"); });
    pool.tryEnqueue([&] { ++after; });
    pool.shutdown();
    CHECK_EQ(pool.failedTasks(), 1u);
    CHECK_EQ(after.load(), 1);
}

TEST(shutdownDrainsQueuedTasks) {
    sched::ThreadPool pool({2, 4096});
    std::atomic<int> n{0};
    for (int i = 0; i < 3000; ++i)
        while (!pool.tryEnqueue([&] { std::this_thread::sleep_for(10us); ++n; })) std::this_thread::yield();
    pool.shutdown();
    CHECK_EQ(n.load(), 3000);
    pool.shutdown();   // idempotent
}

TEST(idleWorkersParkAndWakeOnEnqueue) {
    sched::ThreadPool pool({2, 64});
    std::this_thread::sleep_for(50ms);                        // let both park
    std::atomic<int> n{0};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        pool.tryEnqueue([&] { ++n; });
        while (n.load() != i + 1) std::this_thread::yield();  // each task must be picked up by a parked worker
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}

TEST(boxedTasksAreCounted) {
    sched::ThreadPool pool({1, 16});
    std::atomic<int> n{0};
    char big[256] = {1};
    CHECK(pool.tryEnqueueBoxed([&, big] { n += big[0]; }));
    pool.shutdown();
    CHECK_EQ(n.load(), 1);
    CHECK_EQ(pool.boxedTasks(), 1u);
}

TEST(workerNamesAndAffinity) {
    sched::ThreadPoolOptions o;
    o.threads = 2;
    o.namePrefix = "svc";
    o.cpus = {0};
    sched::ThreadPool pool(o);
    std::atomic<bool> ok{false};
    pool.tryEnqueue([&] {
        char name[32] = {};
        pthread_getname_np(pthread_self(), name, sizeof name);
        ok = std::string(name).rfind("svc-", 0) == 0 && sched::ThreadPool::onWorkerThread();
    });
    pool.shutdown();
    CHECK(ok.load());
}

TEST(steadyStateRunsWithoutHeapAllocations) {
    sched::ThreadPool pool({2, 256});
    std::atomic<int> n{0};
    for (int i = 0; i < 1000; ++i) { while (!pool.tryEnqueue([&] { ++n; })) std::this_thread::yield(); }
    while (n.load() < 1000) std::this_thread::yield();
    {
        tst::AllocScope scope;
        for (int i = 0; i < 200000; ++i) { while (!pool.tryEnqueue([&] { ++n; })) std::this_thread::yield(); }
        while (n.load() < 201000) std::this_thread::yield();
        CHECK_EQ(scope.calls(), 0ull);
    }
}

int main() { return tst::runAll("thread_pool"); }
