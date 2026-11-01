// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <thread>

#include "mini_test.hpp"
#include "sys/wait.hpp"

using namespace std::chrono_literals;

TEST(changedWhenValueDiffers) {
    std::atomic<std::uint32_t> w{5};
    CHECK(sys::waitOnAddress(w, 4, 1s) == sys::WaitResult::Changed);
}

TEST(timesOut) {
    std::atomic<std::uint32_t> w{0};
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(sys::waitOnAddress(w, 0, 30ms) == sys::WaitResult::TimedOut);
    CHECK(std::chrono::steady_clock::now() - t0 >= 25ms);
}

TEST(wakeAllReleasesEveryWaiter) {
    std::atomic<std::uint32_t> w{0};
    std::atomic<int> done{0};
    std::thread t[4];
    for (auto& x : t)
        x = std::thread([&] {
            while (w.load(std::memory_order_acquire) == 0)
                sys::waitOnAddress(w, 0);
            ++done;
        });
    std::this_thread::sleep_for(50ms);
    w.store(1, std::memory_order_release);
    sys::wakeAll(w);
    for (auto& x : t)
        x.join();
    CHECK_EQ(done.load(), 4);
}

TEST(pingPong) {
    std::atomic<std::uint32_t> turn{0};
    constexpr int rounds = 20000;
    std::thread other([&] {
        for (int i = 0; i < rounds; ++i) {
            while (turn.load(std::memory_order_acquire) != 1)
                sys::waitOnAddress(turn, 0);
            turn.store(0, std::memory_order_release);
            sys::wakeOne(turn);
        }
    });
    for (int i = 0; i < rounds; ++i) {
        turn.store(1, std::memory_order_release);
        sys::wakeOne(turn);
        while (turn.load(std::memory_order_acquire) != 0)
            sys::waitOnAddress(turn, 1);
    }
    other.join();
    CHECK_EQ(turn.load(), 0u);
}

int main() { return tst::runAll("wait"); }
