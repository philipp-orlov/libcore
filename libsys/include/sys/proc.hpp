// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace sys {

// Resident set size of this process in bytes (0 if unavailable). Used by the soak tests to prove "no growth".
std::size_t residentBytes() noexcept;

// Peak resident set size so far.
std::size_t peakResidentBytes() noexcept;

// Monotonic and wall clocks in nanoseconds.
std::uint64_t monotonicNs() noexcept;
std::uint64_t wallNs() noexcept;

// glibc: caps malloc arenas and fixes the mmap/trim thresholds so a long-running process does not creep.
// No-ops elsewhere. Call once, early in main(), before threads start.
struct AllocatorTuning {
    int maxArenas = 4;                          // M_ARENA_MAX
    std::size_t mmapThresholdBytes = 1u << 20;  // M_MMAP_THRESHOLD (fixing it turns off the dynamic adjustment)
    std::size_t trimThresholdBytes = 8u << 20;  // M_TRIM_THRESHOLD
};
void tuneAllocator(const AllocatorTuning& tuning = {}) noexcept;

// Returns unused heap memory to the OS (glibc malloc_trim(0)); a no-op elsewhere.
void trimHeap() noexcept;

}  // namespace sys
