// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/wait.hpp"

#include <cerrno>
#include <climits>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#else
#include <thread>
#endif

namespace sys {

#if defined(__linux__)

WaitResult waitOnAddress(std::atomic<std::uint32_t>& word, std::uint32_t expected,
                         std::chrono::nanoseconds timeout) noexcept {
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t), "futex needs a plain 32-bit word");
    timespec ts{};
    timespec* pts = nullptr;
    if (timeout.count() >= 0) {
        ts.tv_sec = static_cast<time_t>(timeout.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(timeout.count() % 1000000000);
        pts = &ts;
    }
    // Not FUTEX_PRIVATE: the word may live in memory other processes map.
    const long rc = ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAIT, expected, pts, nullptr, 0);
    if (rc == 0)
        return WaitResult::Woken;
    switch (errno) {
        case EAGAIN: return WaitResult::Changed;
        case ETIMEDOUT: return WaitResult::TimedOut;
        default: return WaitResult::Woken;  // EINTR: a spurious wake, the caller re-checks anyway
    }
}

void wakeOne(std::atomic<std::uint32_t>& word) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void wakeAll(std::atomic<std::uint32_t>& word) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

#else  // Portable fallback until the Windows (WaitOnAddress) and macOS (os_sync_wait_on_address) ports land: poll.

WaitResult waitOnAddress(std::atomic<std::uint32_t>& word, std::uint32_t expected,
                         std::chrono::nanoseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + (timeout.count() < 0 ? std::chrono::nanoseconds::max() / 2 : timeout);
    if (word.load(std::memory_order_acquire) != expected)
        return WaitResult::Changed;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (word.load(std::memory_order_acquire) != expected)
            return WaitResult::Woken;
    }
    return WaitResult::TimedOut;
}
void wakeOne(std::atomic<std::uint32_t>&) noexcept {}
void wakeAll(std::atomic<std::uint32_t>&) noexcept {}

#endif

}  // namespace sys
