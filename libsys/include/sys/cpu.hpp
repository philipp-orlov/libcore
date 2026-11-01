// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <vector>

namespace sys {

struct CpuTopology {
    unsigned logical = 1;      // online hardware threads
    unsigned physical = 1;     // distinct cores (SMT siblings counted once)
    unsigned performance = 1;  // cores at the top cpu_capacity (big cores on big.LITTLE); == physical on symmetric CPUs
    unsigned numaNodes = 1;
    std::vector<unsigned> cpuToNode;  // size == highest online cpu id + 1; 0 when unknown
};

// Computed once on first use, then constant.
const CpuTopology& cpuTopology();

// Restricts the calling thread to one logical CPU. False when unsupported (macOS) or refused.
bool pinThisThread(unsigned cpu) noexcept;

// Bytes of the unified/last-level cache that one core sees, or 0 when unknown.
std::size_t l2CacheBytes() noexcept;

struct CpuFeatures {
    bool sse42 = false, avx2 = false, avx512f = false;
    bool armCrc32 = false, armLse = false, armFp16 = false, armDotProd = false;
};
const CpuFeatures& cpuFeatures() noexcept;

}  // namespace sys
