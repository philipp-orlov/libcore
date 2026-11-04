// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Floating point <-> text, independent of the process locale and free of heap traffic on every common path. Built on
// <charconv> where the standard library has it for floating point (libstdc++ 11 and later), and on snprintf/strtod in
// the "C" locale where it does not (GCC 9 on JetPack 5), with identical results.
#pragma once

#include <cstddef>
#include <string_view>

namespace base {

// The shortest decimal digits that read back as the same number: value == 0.d1d2d3... * 10^pointPosition. This is the
// form language specifications define their number spelling on, so each can lay it out as it says.
struct ShortestDigits {
    char digits[18];     // not terminated; no trailing zeros
    int count = 0;
    int pointPosition = 0;
};
// `value` must be finite and greater than zero.
ShortestDigits shortestDigits(double value) noexcept;
ShortestDigits shortestDigits(float value) noexcept;

// Writes the shortest round-trip text in std::to_chars's plain form ("1e+22", "0.1"), at most kMaxShortest characters,
// not terminated. Returns the length. `value` must be finite.
inline constexpr std::size_t kMaxShortest = 32;
std::size_t formatShortest(double value, char* out) noexcept;

// The longest decimal number at the start of `text`: an optional sign, digits with an optional point, an optional
// exponent, or inf/infinity/nan. No leading blanks and no hex floats, whatever strtod would do with them.
struct ParsedDouble {
    double value = 0;          // an out-of-range number reads as strtod reads it: +-infinity, or +-0
    std::size_t consumed = 0;  // 0: nothing there that reads as a number
    bool outOfRange = false;
};
ParsedDouble parseDouble(std::string_view text);

}  // namespace base
