// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Fixed worker pool over a bounded lock-free MPMC ring (Vyukov). Tasks are base::InlineFunction (no heap when the
// capture fits; a larger capture is a compile-time error). Idle workers spin for `spin`, then park on a futex-style
// word (sys::waitOnAddress) with an eventcount, so an enqueue into a busy pool costs no system call.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/inline_function.hpp"
#include "base/platform.hpp"

namespace sched {

using Task = base::InlineFunction<void(), 48>;

struct ThreadPoolOptions {
    ThreadPoolOptions() = default;
    ThreadPoolOptions(unsigned threadCount, std::size_t capacity = 1024) : threads(threadCount), queueCapacity(capacity) {}
    unsigned threads = 0;                      // 0 = sys::cpuTopology().performance
    std::size_t queueCapacity = 1024;          // rounded up to a power of two (at least 2)
    std::chrono::microseconds spin{0};         // poll this long after a task before parking
    std::string namePrefix = "worker";         // thread names "<prefix>-<n>" (Linux: truncated to 15 characters)
    std::vector<unsigned> cpus;                // if not empty, worker i is pinned to cpus[i % size]
};

class ThreadPool {
public:
    explicit ThreadPool(ThreadPoolOptions options = {});
    ~ThreadPool();   // shutdown()
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // False when the queue is full or the pool is stopping; the task is neither run nor consumed in that case
    // (the argument is moved only on success).
    template <class F>
    bool tryEnqueue(F&& f) {
        static_assert(sizeof(std::decay_t<F>) <= 48,
                      "task capture too large for the inline task slot: capture a pointer to a pooled context instead");
        Task t(static_cast<F&&>(f));
        return push(t);
    }
    // For callers that hold an arbitrarily large callable: boxes it on the heap (one allocation) and counts it.
    bool tryEnqueueBoxed(std::function<void()> f);

    // Drains the queue, then joins. Call from the owning thread, never from a worker. Idempotent.
    void shutdown();

    unsigned threads() const noexcept { return static_cast<unsigned>(workers_.size()); }
    std::size_t capacity() const noexcept { return mask_ + 1; }
    std::size_t pendingTasks() const noexcept;
    std::uint64_t executedTasks() const noexcept { return executed_.load(std::memory_order_relaxed); }
    std::uint64_t failedTasks() const noexcept { return failed_.load(std::memory_order_relaxed); }
    std::uint64_t rejectedTasks() const noexcept { return rejected_.load(std::memory_order_relaxed); }
    std::uint64_t boxedTasks() const noexcept { return boxed_.load(std::memory_order_relaxed); }

    // True on a worker thread of any ThreadPool.
    static bool onWorkerThread() noexcept;

private:
    struct Cell {
        std::atomic<std::size_t> seq;
        Task task;
    };
    bool push(Task& task) noexcept;
    bool pop(Task& out) noexcept;
    void workerLoop(unsigned index);
    void notifyOne() noexcept;

    ThreadPoolOptions options_;
    std::size_t mask_ = 0;
    std::unique_ptr<Cell[]> cells_;
    alignas(base::kCacheLine) std::atomic<std::size_t> enqueuePos_{0};
    alignas(base::kCacheLine) std::atomic<std::size_t> dequeuePos_{0};
    alignas(base::kCacheLine) std::atomic<std::uint32_t> wakeWord_{0};
    std::atomic<std::uint32_t> sleepers_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> executed_{0}, failed_{0}, rejected_{0}, boxed_{0};
    std::vector<std::thread> workers_;
};

}  // namespace sched
