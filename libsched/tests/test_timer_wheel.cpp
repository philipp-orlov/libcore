// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <vector>

#include "mini_test.hpp"
#include "sched/timer_wheel.hpp"

using sched::TimerWheel;
using Clock = TimerWheel::Clock;
using ms = std::chrono::milliseconds;

TEST(neverFiresEarlyAndFiresOnceDue) {
    const auto t0 = Clock::now();
    TimerWheel w(64, t0);
    int fired = 0;
    w.tryAdd(t0 + ms(30), [&] { ++fired; });
    w.tryAdd(t0 + ms(5), [&] { fired += 10; });
    TimerWheel::Callback cb;
    CHECK(!w.popDue(t0, cb));
    CHECK(!w.popDue(t0 + ms(3), cb));
    CHECK(w.popDue(t0 + ms(6), cb));
    cb();
    CHECK_EQ(fired, 10);
    CHECK(!w.popDue(t0 + ms(29), cb));
    CHECK(w.popDue(t0 + ms(31), cb));
    cb();
    CHECK_EQ(fired, 11);
    CHECK(w.empty());
    CHECK_EQ(w.waitMilliseconds(t0), -1);
}

TEST(longDelaysCascadeThroughTheLevels) {
    const auto t0 = Clock::now();
    TimerWheel w(16, t0);
    for (long delay : {300L, 70000L, 20000000L}) {   // level 1, level 2, level 3
        w.tryAdd(t0 + ms(delay), [] {});
    }
    TimerWheel::Callback cb;
    int popped = 0;
    for (long t = 0; t <= 20002000; t += (t < 100000 ? 7 : 997)) {
        while (w.popDue(t0 + ms(t), cb)) {
            ++popped;
            if (popped == 1) CHECK(t >= 300);
            if (popped == 2) CHECK(t >= 70000);
            if (popped == 3) CHECK(t >= 20000000);
        }
    }
    CHECK_EQ(popped, 3);
}

TEST(aVeryLongSleepRebasesInsteadOfWalking) {
    const auto t0 = Clock::now();
    TimerWheel w(8, t0);
    w.tryAdd(t0 + ms(10), [] {});
    w.tryAdd(t0 + std::chrono::hours(5), [] {});
    TimerWheel::Callback cb;
    CHECK(w.popDue(t0 + std::chrono::hours(1), cb));   // jumped over millions of ticks
    CHECK(!w.popDue(t0 + std::chrono::hours(1), cb));
    CHECK(w.popDue(t0 + std::chrono::hours(5) + ms(1), cb));
}

TEST(staleIdsAndCapacity) {
    const auto t0 = Clock::now();
    TimerWheel w(1, t0);
    const auto first = w.tryAdd(t0, [] {});
    CHECK_EQ(w.tryAdd(t0, [] {}), 0u);   // full
    w.cancel(first);
    const auto second = w.tryAdd(t0 + ms(5), [] {});
    CHECK(first != second);
    w.cancel(first);                       // stale
    CHECK(!w.refresh(first, t0));
    CHECK(w.refresh(second, t0 + ms(50)));
    TimerWheel::Callback cb;
    CHECK(!w.popDue(t0 + ms(10), cb));
    CHECK(w.popDue(t0 + ms(51), cb));
}

TEST(randomOperationsMatchAReferenceModel) {
    constexpr std::size_t kCap = 512;
    const auto base = Clock::now();
    TimerWheel w(kCap, base);
    std::multimap<std::int64_t, std::uint64_t> ref;
    std::map<std::uint64_t, std::multimap<std::int64_t, std::uint64_t>::iterator> byId;
    std::mt19937_64 rng(7);
    std::int64_t clock = 0;   // ms, only moves forward
    for (int step = 0; step < 300000; ++step) {
        const auto op = rng() % 10;
        if (op < 4 && byId.size() < kCap) {
            const std::int64_t when = clock + static_cast<std::int64_t>(rng() % 70000);
            const auto id = w.tryAdd(base + ms(when), [] {});
            CHECK(id != 0);
            byId[id] = ref.insert({when, id});
        } else if (op < 6 && !byId.empty()) {
            auto it = byId.begin();
            std::advance(it, rng() % byId.size());
            w.cancel(it->first);
            ref.erase(it->second);
            byId.erase(it);
        } else if (op < 8 && !byId.empty()) {
            auto it = byId.begin();
            std::advance(it, rng() % byId.size());
            const std::int64_t when = clock + static_cast<std::int64_t>(rng() % 70000);
            CHECK(w.refresh(it->first, base + ms(when)));
            const auto id = it->first;
            ref.erase(it->second);
            it->second = ref.insert({when, id});
        } else {
            clock += static_cast<std::int64_t>(rng() % (rng() % 50 == 0 ? 90000 : 40));
            const std::size_t due = std::distance(ref.begin(), ref.upper_bound(clock));
            TimerWheel::Callback cb;
            std::size_t popped = 0;
            while (w.popDue(base + ms(clock), cb)) ++popped;
            CHECK_EQ(popped, due);
            for (auto it = ref.begin(); it != ref.upper_bound(clock);) { byId.erase(it->second); it = ref.erase(it); }
            const int wait = w.waitMilliseconds(base + ms(clock));
            if (ref.empty()) CHECK_EQ(wait, -1);
            else {
                CHECK(wait >= 0);
                if (clock + wait > ref.begin()->first) std::fprintf(stderr, "clock %lld wait %d earliest %lld\n", (long long)clock, wait, (long long)ref.begin()->first);
                CHECK(clock + wait <= ref.begin()->first);   // never sleeps past the earliest timer
            }
        }
    }
}

int main() { return tst::runAll("timer_wheel"); }
