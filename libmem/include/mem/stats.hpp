// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mem {

// Plain snapshot, the same fields for every pool.
struct Stats {
    std::uint64_t mappedBytes = 0;      // address space currently mapped for blocks
    std::uint64_t retainedBytes = 0;    // mapped but idle (free list, idle large blocks)
    std::uint64_t blocksInUse = 0;
    std::uint64_t peakBlocksInUse = 0;
    std::uint64_t acquires = 0;
    std::uint64_t releases = 0;
    std::uint64_t mapCalls = 0;
    std::uint64_t unmapCalls = 0;
    std::uint64_t borrowed = 0;         // served from a larger class
    std::uint64_t refused = 0;          // sealed or over quota
    std::uint64_t misuse = 0;

    // Writes {"mappedBytes":N,...} into `out`, NUL-terminated and truncated to `cap`; returns the length that
    // would have been written (like snprintf). Allocation free.
    std::size_t toJson(char* out, std::size_t cap) const noexcept;
    // Prometheus text exposition, metric names prefixed with `prefix` (e.g. "http_pool_").
    std::size_t toPrometheus(char* out, std::size_t cap, const char* prefix) const noexcept;
};

// The live counters; padded so threads that update different pools do not share a line.
struct alignas(64) Counters {
    std::atomic<std::uint64_t> mappedBytes{0};
    std::atomic<std::uint64_t> retainedBytes{0};
    std::atomic<std::uint64_t> blocksInUse{0};
    std::atomic<std::uint64_t> peakBlocksInUse{0};
    std::atomic<std::uint64_t> acquires{0};
    std::atomic<std::uint64_t> releases{0};
    std::atomic<std::uint64_t> mapCalls{0};
    std::atomic<std::uint64_t> unmapCalls{0};
    std::atomic<std::uint64_t> borrowed{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> misuse{0};

    void noteAcquire() noexcept {
        acquires.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now = blocksInUse.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t peak = peakBlocksInUse.load(std::memory_order_relaxed);
        while (now > peak && !peakBlocksInUse.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
        }
    }
    void noteRelease() noexcept {
        releases.fetch_add(1, std::memory_order_relaxed);
        blocksInUse.fetch_sub(1, std::memory_order_relaxed);
    }
    Stats snapshot() const noexcept {
        Stats s;
        s.mappedBytes = mappedBytes.load(std::memory_order_relaxed);
        s.retainedBytes = retainedBytes.load(std::memory_order_relaxed);
        s.blocksInUse = blocksInUse.load(std::memory_order_relaxed);
        s.peakBlocksInUse = peakBlocksInUse.load(std::memory_order_relaxed);
        s.acquires = acquires.load(std::memory_order_relaxed);
        s.releases = releases.load(std::memory_order_relaxed);
        s.mapCalls = mapCalls.load(std::memory_order_relaxed);
        s.unmapCalls = unmapCalls.load(std::memory_order_relaxed);
        s.borrowed = borrowed.load(std::memory_order_relaxed);
        s.refused = refused.load(std::memory_order_relaxed);
        s.misuse = misuse.load(std::memory_order_relaxed);
        return s;
    }
};

}  // namespace mem
