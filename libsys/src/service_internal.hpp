// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#include "sys/service.hpp"

namespace sys::service::detail {

/// The Control both hosts use: remembers the stop function, and calls it once, however early the request came.
class BasicControl final : public Control {
public:
    void onStop(std::function<void()> stop) override {
        std::function<void()> now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = std::move(stop);
            if (requested_.load() && !called_) {
                called_ = true;
                now = stop_;
            }
        }
        if (now)
            now();
    }

    void running() override {
        if (runningHook)
            runningHook();
    }

    bool stopRequested() const override { return requested_.load(); }

    /// From the thread that received the request (never a signal handler).
    void requestStop() {
        std::function<void()> now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_.store(true);
            if (stop_ && !called_) {
                called_ = true;
                now = stop_;
            }
        }
        if (now)
            now();
    }

    std::function<void()> runningHook;

private:
    std::mutex mutex_;
    std::function<void()> stop_;
    std::atomic<bool> requested_{false};
    bool called_ = false;
};

/// Runs the body in the foreground; Ctrl-C and SIGTERM (Windows: also Ctrl-Break, closing the console, logoff and shutdown)
/// request a stop.
int runInForeground(const std::string& name, const std::function<int(Control&)>& body);

/// Runs the body as the service `name` of the Windows service control manager. Windows only.
int runUnderServiceManager(const std::string& name, const std::function<int(Control&)>& body);

}  // namespace sys::service::detail
