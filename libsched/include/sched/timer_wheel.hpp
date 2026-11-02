// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Hierarchical timing wheel with the interface of TimerHeap (tryAdd / refresh / cancel / popDue / waitMilliseconds /
// empty): 1 ms ticks, 4 levels of 256 slots (reach about 49 days; later timers sit in the top level and are
// re-examined when it comes round). Add, cancel and refresh are O(1) regardless of the number of timers, which is
// what a loop with 100 k+ connection timeouts wants. Fixed capacity, no allocation after construction. Timers fire
// with 1 ms granularity (a timer due at 10.4 ms fires from tick 11).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "base/inline_function.hpp"

namespace sched {

class TimerWheel {
public:
    using Clock = std::chrono::steady_clock;
    using Callback = base::InlineFunction<void(), 48>;

    static Clock::time_point after(std::chrono::milliseconds delay) noexcept {
        const auto now = Clock::now();
        if (delay.count() <= 0)
            return now;
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
        return delay >= maximum ? Clock::time_point::max() : now + delay;
    }

    // `origin` is tick 0; ticks are measured from it (default: now). Tests pin it to get exact millisecond arithmetic.
    explicit TimerWheel(std::size_t capacity, Clock::time_point origin = Clock::now());

    std::uint64_t tryAdd(Clock::time_point when, Callback callback);
    bool refresh(std::uint64_t id, Clock::time_point when);
    void cancel(std::uint64_t id);
    bool popDue(Clock::time_point now, Callback& callback);
    int waitMilliseconds(Clock::time_point now) const;
    std::size_t capacity() const noexcept { return slots_.size(); }
    bool empty() const noexcept { return armed_.load() == 0; }

private:
    static constexpr unsigned kLevels = 4, kBits = 8, kSlots = 1u << kBits;
    static constexpr std::uint32_t kNil = ~std::uint32_t{0};
    struct Slot {
        std::uint64_t tick = 0;
        Callback callback;
        std::uint32_t prev = kNil, next = kNil;
        std::uint32_t generation = 0;
        std::uint16_t bucket = 0;      // level * 256 + slot, or kDueBucket
        bool active = false;
    };
    static constexpr std::uint16_t kDueBucket = kLevels * kSlots;

    std::uint64_t tickOf(Clock::time_point t) const noexcept;
    void link(std::uint32_t index, std::uint16_t bucket);
    void unlink(std::uint32_t index);
    void place(std::uint32_t index);
    void advanceTo(std::uint64_t tick);
    std::size_t find(std::uint64_t id) const noexcept;

    mutable std::mutex mutex_;
    std::atomic<std::size_t> armed_{0};
    Clock::time_point origin_;
    std::uint64_t now_ = 0;                       // the last tick that has been processed
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::array<std::uint32_t, kLevels * kSlots + 1> heads_;   // last entry: the due list
    std::array<std::uint32_t, kLevels * kSlots + 1> tails_;
    std::size_t dueCount_ = 0;
};

}  // namespace sched
