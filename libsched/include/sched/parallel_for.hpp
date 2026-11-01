// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// parallelFor(pool, begin, end, grain, fn): fn(lo, hi) is called on disjoint sub-ranges covering [begin, end).
//   * the range is split once into min(workers + 1, ceil(n / grain)) pieces (at most 64); the caller runs the first
//     piece itself, so a pool of N workers gives N + 1 way parallelism;
//   * no std::function, no future, no heap: pieces live on the caller's stack and are tracked by a Latch;
//   * a piece the queue rejects runs on the caller;
//   * the first exception thrown by any piece is rethrown on the caller after all pieces finished;
//   * a call made from inside a parallel region (or from a pool worker) runs serially, so nested use cannot
//     deadlock, and the region flag is reset by a guard even when a piece throws.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>

#include "sched/latch.hpp"
#include "sched/thread_pool.hpp"

namespace sched {

namespace detail {
inline bool& insideParallelRegion() noexcept {
    static thread_local bool flag = false;
    return flag;
}
struct RegionGuard {
    RegionGuard() noexcept : previous(insideParallelRegion()) { insideParallelRegion() = true; }
    ~RegionGuard() { insideParallelRegion() = previous; }
    bool previous;
};
struct ErrorSlot {
    std::atomic<bool> taken{false};
    std::exception_ptr error;
    void capture() noexcept {
        if (!taken.exchange(true, std::memory_order_acq_rel))
            error = std::current_exception();
    }
};
}  // namespace detail

template <class F>
void parallelFor(ThreadPool& pool, std::size_t begin, std::size_t end, std::size_t grain, F&& fn) {
    if (end <= begin)
        return;
    const std::size_t n = end - begin;
    if (grain == 0)
        grain = 1;
    const std::size_t maxPieces = std::min<std::size_t>(64, pool.threads() + 1);
    const std::size_t pieces = std::min(maxPieces, (n + grain - 1) / grain);
    if (pieces <= 1 || detail::insideParallelRegion() || ThreadPool::onWorkerThread()) {
        detail::RegionGuard guard;
        fn(begin, end);
        return;
    }

    struct Piece {
        std::size_t lo, hi;
    };
    struct Context {
        F* fn;
        Latch* latch;
        detail::ErrorSlot* error;
        Piece range[64];
    };
    Latch latch(static_cast<std::uint32_t>(pieces - 1));
    detail::ErrorSlot error;
    Context ctx;
    ctx.fn = &fn;
    ctx.latch = &latch;
    ctx.error = &error;
    // Even split; the first `n % pieces` pieces get one extra element.
    std::size_t at = begin;
    for (std::size_t i = 0; i < pieces; ++i) {
        const std::size_t len = n / pieces + (i < n % pieces ? 1 : 0);
        ctx.range[i] = {at, at + len};
        at += len;
    }

    auto runPiece = [](Context* c, std::size_t i) noexcept {
        detail::RegionGuard guard;
        try {
            (*c->fn)(c->range[i].lo, c->range[i].hi);
        } catch (...) {
            c->error->capture();
        }
    };
    for (std::size_t i = 1; i < pieces; ++i) {
        Context* c = &ctx;
        const bool queued = pool.tryEnqueue([c, i, runPiece] {
            runPiece(c, i);
            c->latch->countDown();
        });
        if (!queued) {
            runPiece(&ctx, i);   // queue full: do it here
            latch.countDown();
        }
    }
    runPiece(&ctx, 0);
    latch.wait();
    if (error.error)
        std::rethrow_exception(error.error);
}

}  // namespace sched
