// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/proc.hpp"

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
