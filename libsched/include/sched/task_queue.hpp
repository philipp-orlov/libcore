// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "base/inline_function.hpp"

namespace sched {

// Bounded many-producer queue drained by one reactor thread: a normal lane of `capacity` slots plus 4 control slots
// that stay available when the normal lane is full. Moved from libhttp; the callable type is a parameter, the
// default being the heap-free inline function.
template <class Fn = base::InlineFunction<void(), 48>>
class BasicTaskQueue
{
public:
    explicit BasicTaskQueue(size_t capacity) : tasks_(capacity)
    {
        if (!capacity)
            throw std::invalid_argument("reactor task capacity must be positive");
    }

    bool tryPush(Fn callback, bool control = false)
    {
        if (!callback)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return false;

        if (control) {
            if (controlCount_ == controls_.size())
                return false;

            controls_[(controlHead_ + controlCount_) % controls_.size()] = std::move(callback);
            ++controlCount_;
        } else {
            if (count_ == tasks_.size()) {
                ++rejectedTasks_;
                return false;
            }

            tasks_[(head_ + count_) % tasks_.size()] = std::move(callback);
            ++count_;
        }
        pending_.store(count_ + controlCount_);
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    bool closed() const noexcept { return closed_.load(); }

    // Lock-free; a loop checks it every round, almost always finding nothing.
    // Sequentially consistent on purpose: it pairs with IoLoop raising its
    // `sleeping` flag before the final check (store-then-load on both sides).
    bool hasPending() const noexcept { return pending_.load() != 0; }

    size_t failedTasks() const noexcept { return failedTasks_.load(); }
    // Tasks refused because the queue was full.
    size_t rejectedTasks() const noexcept { return rejectedTasks_.load(); }
    size_t pendingTasks() const noexcept { return pending_.load(); }

    void drain()
    {
        drain(false);
        drain(true);
    }

private:
    // Runs what was queued when it started, taking tasks out a batch at a
    // time under one lock; each one's slot is free again before it runs, so
    // a task may queue another even on a full queue.
    void drain(bool control)
    {
        static constexpr size_t kBatch = 8;
        size_t remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining = control ? controlCount_ : count_;
        }
        while (remaining) {
            std::array<Fn, kBatch> batch;
            const size_t taken = std::min(remaining, kBatch);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (size_t index = 0; index < taken; ++index) {
                    if (control) {
                        batch[index] = std::move(controls_[controlHead_]);
                        controlHead_ = (controlHead_ + 1) % controls_.size();
                        --controlCount_;
                    } else {
                        batch[index] = std::move(tasks_[head_]);
                        head_ = (head_ + 1) % tasks_.size();
                        --count_;
                    }
                }
                pending_.store(count_ + controlCount_);
            }
            remaining -= taken;
            for (size_t index = 0; index < taken; ++index) {
                try {
                    batch[index]();
                } catch (...) {
                    ++failedTasks_;
                }
                batch[index] = Fn();
            }
        }
    }

    mutable std::mutex mutex_;
    std::vector<Fn> tasks_;
    std::array<Fn, 4> controls_;
    size_t head_ = 0;
    size_t count_ = 0;
    size_t controlHead_ = 0;
    size_t controlCount_ = 0;
    std::atomic<bool> closed_{false};
    std::atomic<size_t> pending_{0};
    std::atomic<size_t> failedTasks_{0};
    std::atomic<size_t> rejectedTasks_{0};
};

using TaskQueue = BasicTaskQueue<>;

}  // namespace sched
