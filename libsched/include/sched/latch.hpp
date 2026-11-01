// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include "sys/wait.hpp"

namespace sched {

// A countdown latch that lives on the stack and parks on a futex-style word.
class Latch {
public:
    explicit Latch(std::uint32_t count) noexcept : count_(count) {}
    void countDown() noexcept {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            sys::wakeAll(count_);
    }
    void wait() noexcept {
        for (;;) {
            const std::uint32_t c = count_.load(std::memory_order_acquire);
            if (c == 0)
                return;
            sys::waitOnAddress(count_, c);
        }
    }
private:
    std::atomic<std::uint32_t> count_;
};

}  // namespace sched
