// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Park a thread on a 32-bit word until another thread or process changes it. Works on words in memory shared
// between processes (the Linux futex is not PRIVATE).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace sys {

enum class WaitResult {
    Woken,      // returned after a wake (or spuriously: always re-check the condition)
    Changed,    // the word was already different from `expected`
    TimedOut,
};

// Blocks while word == expected, at most `timeout` (negative = forever).
WaitResult waitOnAddress(std::atomic<std::uint32_t>& word, std::uint32_t expected,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1)) noexcept;
void wakeOne(std::atomic<std::uint32_t>& word) noexcept;
void wakeAll(std::atomic<std::uint32_t>& word) noexcept;

}  // namespace sys
