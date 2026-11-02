// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "net/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace net {

namespace { std::atomic<LogHook> g_hook{nullptr}; }

void setLogHook(LogHook hook) noexcept { g_hook.store(hook, std::memory_order_release); }

void log(LogLevel level, const char* format, ...) noexcept {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (LogHook hook = g_hook.load(std::memory_order_acquire))
        hook(level, buffer);
    else
        std::fprintf(stderr, "net: %s: %s\n", level == LogLevel::Error ? "error" : "warning", buffer);
}

}  // namespace net
