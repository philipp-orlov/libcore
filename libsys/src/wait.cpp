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
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <synchapi.h>

#include <algorithm>
#pragma comment(lib, "Synchronization.lib")
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

#elif defined(_WIN32)   // NOT BUILT OR RUN on this machine: WaitOnAddress (Windows 8+, needs Synchronization.lib).

WaitResult waitOnAddress(std::atomic<std::uint32_t>& word, std::uint32_t expected, std::chrono::nanoseconds timeout) noexcept {
    if (word.load(std::memory_order_acquire) != expected)
        return WaitResult::Changed;
    const DWORD ms = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(std::min<std::int64_t>(timeout.count() / 1000000, 0xFFFFFFFELL));
    std::uint32_t compare = expected;
    if (::WaitOnAddress(&word, &compare, sizeof compare, ms))
        return WaitResult::Woken;
    return ::GetLastError() == ERROR_TIMEOUT ? WaitResult::TimedOut : WaitResult::Woken;
}
void wakeOne(std::atomic<std::uint32_t>& word) noexcept { ::WakeByAddressSingle(&word); }
void wakeAll(std::atomic<std::uint32_t>& word) noexcept { ::WakeByAddressAll(&word); }

#elif defined(__APPLE__)   // NOT BUILT OR RUN on this machine: os_sync_wait_on_address (macOS 14.4+).

#include <os/os_sync_wait_on_address.h>

WaitResult waitOnAddress(std::atomic<std::uint32_t>& word, std::uint32_t expected, std::chrono::nanoseconds timeout) noexcept {
    if (word.load(std::memory_order_acquire) != expected)
        return WaitResult::Changed;
    int rc;
    if (timeout.count() < 0)
        rc = os_sync_wait_on_address(&word, expected, sizeof expected, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
    else
        rc = os_sync_wait_on_address_with_timeout(&word, expected, sizeof expected, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                  OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(timeout.count()));
    if (rc >= 0)
        return WaitResult::Woken;
    return errno == ETIMEDOUT ? WaitResult::TimedOut : WaitResult::Woken;
}
void wakeOne(std::atomic<std::uint32_t>& word) noexcept { os_sync_wake_by_address_any(&word, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED); }
void wakeAll(std::atomic<std::uint32_t>& word) noexcept { os_sync_wake_by_address_all(&word, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED); }

#else  // Portable fallback: poll.

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
