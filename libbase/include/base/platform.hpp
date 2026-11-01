// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Compiler, OS and CPU detection plus the few macros every layer above uses.
#pragma once

#include <cstddef>

#if defined(_WIN32)
#define BASE_OS_WINDOWS 1
#elif defined(__APPLE__)
#define BASE_OS_MACOS 1
#elif defined(__linux__)
#define BASE_OS_LINUX 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define BASE_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define BASE_ARCH_ARM64 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define BASE_LIKELY(x) __builtin_expect(!!(x), 1)
#define BASE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BASE_NOINLINE __attribute__((noinline))
#define BASE_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define BASE_LIKELY(x) (x)
#define BASE_UNLIKELY(x) (x)
#define BASE_NOINLINE __declspec(noinline)
#define BASE_ALWAYS_INLINE __forceinline
#endif

#if !defined(__BYTE_ORDER__) && !defined(_WIN32)
#error "base: cannot determine the byte order"
#endif

namespace base {

// Cache line size used to pad shared counters. Apple's arm64 cores use 128-byte lines.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// A hint inside spin loops: x86 `pause`, ARM `yield`.
inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

}  // namespace base
