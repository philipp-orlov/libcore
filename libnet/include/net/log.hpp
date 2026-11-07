// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// libnet reports through one replaceable hook instead of depending on a logger. Default: stderr.
namespace net {

enum class LogLevel { Warn, Error };
using LogHook = void (*)(LogLevel level, const char* message);
void setLogHook(LogHook hook) noexcept;
#if defined(__GNUC__) || defined(__clang__)
void log(LogLevel level, const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));
#else
void log(LogLevel level, const char* format, ...) noexcept;
#endif

}  // namespace net

#define NET_LOG_WARN(...) ::net::log(::net::LogLevel::Warn, __VA_ARGS__)
#define NET_LOG_ERROR(...) ::net::log(::net::LogLevel::Error, __VA_ARGS__)
