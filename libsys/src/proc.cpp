// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/proc.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Exported by kernel32 (as K32GetProcessMemoryInfo) from Windows 7 on; this keeps psapi.dll out of the imports.
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>

namespace sys {

std::size_t residentBytes() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof counters))
        return 0;
    return static_cast<std::size_t>(counters.WorkingSetSize);
}

std::size_t peakResidentBytes() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof counters))
        return 0;
    return static_cast<std::size_t>(counters.PeakWorkingSetSize);
}

std::uint64_t monotonicNs() noexcept {
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split so that ticks * 1e9 cannot overflow after weeks of uptime.
    return (ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency;
}

std::uint64_t wallNs() noexcept {
    FILETIME time;
    ::GetSystemTimePreciseAsFileTime(&time);
    const std::uint64_t hundredNs = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    constexpr std::uint64_t kEpochDifference = 116444736000000000ull;  // 1601 to 1970, in 100 ns
    return (hundredNs - kEpochDifference) * 100ull;
}

// There is no glibc arena to tune or trim here.
void tuneAllocator(const AllocatorTuning&) noexcept {}
void trimHeap() noexcept {}

}  // namespace sys

#else  // POSIX


#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <fcntl.h>

#include <cstdio>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace sys {

std::size_t residentBytes() noexcept {
#if defined(__linux__)
    // /proc/self/statm: size resident shared ... in pages. Read with stdio-free syscalls so it never allocates.
    char buf[128];
    const int fd = ::open("/proc/self/statm", 0 /*O_RDONLY*/);
    if (fd < 0)
        return 0;
    const ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    unsigned long long size = 0, resident = 0;
    if (std::sscanf(buf, "%llu %llu", &size, &resident) != 2)
        return 0;
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

std::size_t peakResidentBytes() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);          // bytes
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;  // kilobytes
#endif
}

std::uint64_t monotonicNs() noexcept {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t wallNs() noexcept {
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

void tuneAllocator(const AllocatorTuning& t) noexcept {
#if defined(__GLIBC__)
    mallopt(M_ARENA_MAX, t.maxArenas);
    mallopt(M_MMAP_THRESHOLD, static_cast<int>(t.mmapThresholdBytes));
    mallopt(M_TRIM_THRESHOLD, static_cast<int>(t.trimThresholdBytes));
#else
    (void)t;
#endif
}

void trimHeap() noexcept {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

}  // namespace sys

#endif  // _WIN32
