// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mini_test.hpp"
#include "sched/timer_heap.hpp"

using sched::TimerHeap;
using Clock = TimerHeap::Clock;

TEST(firesInOrderAndOnlyWhenDue) {
    TimerHeap q(16);
    const auto t0 = Clock::now();
    std::vector<int> order;
    q.tryAdd(t0 + std::chrono::milliseconds(30), [&] { order.push_back(3); });
    q.tryAdd(t0 + std::chrono::milliseconds(10), [&] { order.push_back(1); });
    q.tryAdd(t0 + std::chrono::milliseconds(20), [&] { order.push_back(2); });
    TimerHeap::Callback cb;
    CHECK(!q.popDue(t0, cb));
    CHECK(q.waitMilliseconds(t0) >= 9);
    while (q.popDue(t0 + std::chrono::milliseconds(25), cb)) cb();
    CHECK_EQ(order.size(), 2u);
    CHECK(order[0] == 1 && order[1] == 2);
    CHECK(q.popDue(t0 + std::chrono::milliseconds(31), cb));
    cb();
    CHECK(q.empty());
    CHECK_EQ(q.waitMilliseconds(t0), -1);
}

TEST(staleIdsNeverTouchANewTimerInTheSameSlot) {
    TimerHeap q(1);
    const auto t0 = Clock::now();
    const auto first = q.tryAdd(t0, [] {});
    q.cancel(first);
    int fired = 0;
    const auto second = q.tryAdd(t0, [&] { ++fired; });
    CHECK(first != second);
    q.cancel(first);                                    // stale: must not cancel the second timer
    CHECK(!q.refresh(first, t0));
    TimerHeap::Callback cb;
    CHECK(q.popDue(t0, cb));
    cb();
    CHECK_EQ(fired, 1);
    CHECK_EQ(q.tryAdd(t0, [] {}) != 0, true);           // slot is free again
    CHECK_EQ(q.tryAdd(t0, [] {}), 0u);                  // capacity 1
}

TEST(randomOperationsMatchAReferenceMultimap) {
    constexpr std::size_t kCap = 512;
    TimerHeap q(kCap);
    std::multimap<std::int64_t, std::uint64_t> ref;      // when -> id
    std::map<std::uint64_t, std::multimap<std::int64_t, std::uint64_t>::iterator> byId;
    const auto base = Clock::now();
    std::mt19937_64 rng(42);
    for (int step = 0; step < 300000; ++step) {
        const auto op = rng() % 10;
        if (op < 4 && byId.size() < kCap) {
            const std::int64_t when = static_cast<std::int64_t>(rng() % 100000);
            const std::uint64_t id = q.tryAdd(base + std::chrono::microseconds(when), [] {});
            CHECK(id != 0);
            byId[id] = ref.insert({when, id});
        } else if (op < 6 && !byId.empty()) {
            auto it = byId.begin();
            std::advance(it, rng() % byId.size());
            q.cancel(it->first);
            ref.erase(it->second);
            byId.erase(it);
        } else if (op < 8 && !byId.empty()) {
            auto it = byId.begin();
            std::advance(it, rng() % byId.size());
            const std::int64_t when = static_cast<std::int64_t>(rng() % 100000);
            CHECK(q.refresh(it->first, base + std::chrono::microseconds(when)));
            const auto id = it->first;
            ref.erase(it->second);
            it->second = ref.insert({when, id});
        } else {
            const std::int64_t now = static_cast<std::int64_t>(rng() % 100000);
            // Pop everything due; each popped timer must be one the reference also considers due, and nothing due
            // may be left behind.
            TimerHeap::Callback cb;
            std::size_t popped = 0;
            const std::size_t due = std::distance(ref.begin(), ref.upper_bound(now));
            while (q.popDue(base + std::chrono::microseconds(now), cb)) ++popped;
            CHECK_EQ(popped, due);
            for (auto it = ref.begin(); it != ref.upper_bound(now);) { byId.erase(it->second); it = ref.erase(it); }
        }
    }
}

int main() { return tst::runAll("timer_heap"); }
