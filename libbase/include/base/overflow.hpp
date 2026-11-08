// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Checked 64-bit signed arithmetic: the result is stored in *result (wrapped when it does not fit) and the return value says
// whether it overflowed. GCC and Clang have builtins for this; MSVC has none for signed integers.
#pragma once

#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace base {

inline bool addOverflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, result);
#else
    *result = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    return ((a ^ *result) & (b ^ *result)) < 0;
#endif
}

inline bool subOverflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, result);
#else
    *result = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
    return ((a ^ b) & (a ^ *result)) < 0;
#endif
}

inline bool mulOverflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, result);
#else
    *result = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    const std::int64_t high = __mulh(a, b);  // the upper 64 bits of the 128-bit product (x64 and ARM64)
    return high != (*result >> 63);
#endif
}

}  // namespace base
