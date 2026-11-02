// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "hw/log.hpp"

#include <atomic>
#include <cstdio>

namespace hw {

namespace {
std::atomic<WarnHook> g_hook{nullptr};
}

void setWarnHook(WarnHook hook) noexcept { g_hook.store(hook, std::memory_order_release); }

void warn(const char* message) noexcept {
    if (WarnHook hook = g_hook.load(std::memory_order_acquire))
        hook(message);
    else
        std::fprintf(stderr, "hw: %s\n", message);
}

}  // namespace hw
