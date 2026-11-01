// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sched/thread_pool.hpp"

#include <algorithm>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "sys/cpu.hpp"
#include "sys/wait.hpp"

namespace sched {

namespace {
thread_local bool t_onWorker = false;
std::size_t ceilPow2(std::size_t v) {
    std::size_t p = 2;
    while (p < v)
        p <<= 1;
    return p;
}
}  // namespace

bool ThreadPool::onWorkerThread() noexcept { return t_onWorker; }

ThreadPool::ThreadPool(ThreadPoolOptions options) : options_(std::move(options)) {
    const std::size_t cap = ceilPow2(options_.queueCapacity);
    mask_ = cap - 1;
    cells_.reset(new Cell[cap]);
    for (std::size_t i = 0; i < cap; ++i)
        cells_[i].seq.store(i, std::memory_order_relaxed);
    unsigned n = options_.threads ? options_.threads : sys::cpuTopology().performance;
    n = std::max(1u, n);
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        workers_.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::push(Task& task) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells_[pos & mask_];
        const std::size_t seq = cell.seq.load(std::memory_order_acquire);
        const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
        if (diff == 0) {
            if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.task = std::move(task);
                cell.seq.store(pos + 1, std::memory_order_release);
                break;
            }
        } else if (diff < 0) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;   // full
        } else {
            pos = enqueuePos_.load(std::memory_order_relaxed);
        }
    }
    notifyOne();
    return true;
}

bool ThreadPool::pop(Task& out) noexcept {
    std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells_[pos & mask_];
        const std::size_t seq = cell.seq.load(std::memory_order_acquire);
        const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
        if (diff == 0) {
            if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                out = std::move(cell.task);
                cell.seq.store(pos + mask_ + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;   // empty
        } else {
            pos = dequeuePos_.load(std::memory_order_relaxed);
        }
    }
}

void ThreadPool::notifyOne() noexcept {
    // Pairs with the sleeper's "announce, then re-check the queue": a worker that announced itself before our push
    // is seen here; one that announces after finds our task when it re-checks.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_relaxed) != 0) {
        wakeWord_.fetch_add(1, std::memory_order_release);
        sys::wakeOne(wakeWord_);
    }
}

std::size_t ThreadPool::pendingTasks() const noexcept {
    const std::size_t e = enqueuePos_.load(std::memory_order_relaxed), d = dequeuePos_.load(std::memory_order_relaxed);
    return e >= d ? e - d : 0;
}

bool ThreadPool::tryEnqueueBoxed(std::function<void()> f) {
    struct Boxed {
        std::function<void()> fn;
    };
    auto* box = new Boxed{std::move(f)};
    boxed_.fetch_add(1, std::memory_order_relaxed);
    Task t([box] {
        std::unique_ptr<Boxed> owner(box);
        owner->fn();
    });
    if (push(t))
        return true;
    delete box;   // the task object was not consumed; its capture is just a pointer
    return false;
}

void ThreadPool::workerLoop(unsigned index) {
    t_onWorker = true;
#if defined(__linux__)
    {
        std::string name = options_.namePrefix + "-" + std::to_string(index);
        name.resize(std::min<std::size_t>(name.size(), 15));
        pthread_setname_np(pthread_self(), name.c_str());
    }
#endif
    if (!options_.cpus.empty())
        sys::pinThisThread(options_.cpus[index % options_.cpus.size()]);

    Task task;
    for (;;) {
        if (pop(task)) {
            try {
                task();
            } catch (...) {
                failed_.fetch_add(1, std::memory_order_relaxed);
            }
            task = Task();   // release captures now
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Spin, then park.
        if (options_.spin.count() > 0) {
            const auto until = std::chrono::steady_clock::now() + options_.spin;
            bool got = false;
            while (std::chrono::steady_clock::now() < until) {
                if (pendingTasks() != 0) { got = true; break; }
                base::cpuRelax();
            }
            if (got)
                continue;
        }
        const std::uint32_t ticket = wakeWord_.load(std::memory_order_acquire);
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        if (pop(task)) {
            sleepers_.fetch_sub(1, std::memory_order_relaxed);
            try {
                task();
            } catch (...) {
                failed_.fetch_add(1, std::memory_order_relaxed);
            }
            task = Task();
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (stopping_.load(std::memory_order_acquire)) {
            sleepers_.fetch_sub(1, std::memory_order_relaxed);
            if (pendingTasks() == 0)
                return;
            continue;
        }
        sys::waitOnAddress(wakeWord_, ticket);
        sleepers_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void ThreadPool::shutdown() {
    if (workers_.empty())
        return;
    stopping_.store(true, std::memory_order_release);
    wakeWord_.fetch_add(1, std::memory_order_release);
    sys::wakeAll(wakeWord_);
    for (std::thread& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
}

}  // namespace sched
