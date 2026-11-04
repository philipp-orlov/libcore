// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "base/number.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// BASE_FP_CHARCONV=0 forces the fallback (its tests do).
#ifndef BASE_FP_CHARCONV
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#define BASE_FP_CHARCONV 1
#else
#define BASE_FP_CHARCONV 0
#endif
#endif

#if !BASE_FP_CHARCONV
#include "base/classic_locale.hpp"
#endif

namespace base {

namespace {

// d.ddde[+-]xx -> digits and point position.
ShortestDigits fromScientific(const char* p, const char* end) noexcept {
    ShortestDigits out;
    for (; p < end && *p != 'e'; ++p)
        if (*p != '.' && out.count < static_cast<int>(sizeof out.digits))
            out.digits[out.count++] = *p;
    while (out.count > 1 && out.digits[out.count - 1] == '0')
        --out.count;
    int exponent = 0;
    if (p < end) {
        ++p;
        const bool negative = p < end && *p == '-';
        if (p < end && (*p == '-' || *p == '+'))
            ++p;
        for (; p < end; ++p)
            exponent = exponent * 10 + (*p - '0');
        if (negative)
            exponent = -exponent;
    }
    out.pointPosition = exponent + 1;
    return out;
}

#if BASE_FP_CHARCONV
template <class T>
ShortestDigits digitsOf(T value) noexcept {
    char buffer[kMaxShortest];
    const auto r = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    return fromScientific(buffer, r.ptr);
}
#else
inline double readBack(const char* text, double) noexcept { return std::strtod(text, nullptr); }
inline float readBack(const char* text, float) noexcept { return std::strtof(text, nullptr); }

template <class T>
ShortestDigits digitsOf(T value) noexcept {
    ClassicLocale classic;
    char buffer[kMaxShortest + 8];
    int length = 0;
    for (int precision = 0; precision < 17; ++precision) {
        length = std::snprintf(buffer, sizeof buffer, "%.*e", precision, static_cast<double>(value));
        if (readBack(buffer, value) == value)
            break;
    }
    return fromScientific(buffer, buffer + length);
}
#endif

// How much of `text` is a number, with `sign` already skipped.
std::size_t tokenLength(std::string_view text) noexcept {
    const auto lower = [&](std::size_t i) { return i < text.size() ? char(text[i] | 0x20) : '\0'; };
    const auto digit = [&](std::size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
    const auto word = [&](const char* w) {
        std::size_t i = 0;
        for (; w[i]; ++i)
            if (lower(i) != w[i])
                return std::size_t{0};
        return i;
    };
    if (const std::size_t n = word("infinity"))
        return n;
    if (const std::size_t n = word("inf"))
        return n;
    if (const std::size_t n = word("nan"))
        return n;
    std::size_t i = 0;
    while (digit(i))
        ++i;
    std::size_t mantissaDigits = i;
    if (i < text.size() && text[i] == '.') {
        const std::size_t point = i++;
        while (digit(i))
            ++i;
        mantissaDigits += i - point - 1;
        if (i == point + 1 && point == 0)
            return 0;
    }
    if (mantissaDigits == 0)
        return 0;
    if (lower(i) == 'e') {
        std::size_t e = i + 1;
        if (e < text.size() && (text[e] == '+' || text[e] == '-'))
            ++e;
        if (digit(e)) {
            while (digit(e))
                ++e;
            i = e;
        }
    }
    return i;
}

}  // namespace

ShortestDigits shortestDigits(double value) noexcept { return digitsOf(value); }
ShortestDigits shortestDigits(float value) noexcept { return digitsOf(value); }

std::size_t formatShortest(double value, char* out) noexcept {
#if BASE_FP_CHARCONV
    return static_cast<std::size_t>(std::to_chars(out, out + kMaxShortest, value).ptr - out);
#else
    // The fewest %g digits that read back as the same double, as to_chars would choose (5e-324, not %.17g's
    // 4.9406564584124654e-324).
    ClassicLocale classic;
    char buffer[kMaxShortest + 8];
    int length = 0;
    for (int digits = 1; digits <= 17; ++digits) {
        length = std::snprintf(buffer, sizeof buffer, "%.*g", digits, value);
        if (std::strtod(buffer, nullptr) == value)
            break;
    }
    std::memcpy(out, buffer, static_cast<std::size_t>(length));
    return static_cast<std::size_t>(length);
#endif
}

ParsedDouble parseDouble(std::string_view text) {
    ParsedDouble out;
    const std::size_t signBytes = !text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    const std::size_t length = tokenLength(text.substr(signBytes));
    if (length == 0)
        return out;
    out.consumed = signBytes + length;
#if BASE_FP_CHARCONV
    // from_chars takes '-' but not '+'.
    const char* first = text.data() + (text[0] == '+' ? 1 : 0);
    const char* last = text.data() + out.consumed;
    const auto r = std::from_chars(first, last, out.value);
    if (r.ec == std::errc() && r.ptr == last)
        return out;
#endif
    // The fallback, and the out-of-range numbers from_chars reports without a value.
    char small[128];
    std::string large;
    const char* token = small;
    if (out.consumed < sizeof small) {
        std::memcpy(small, text.data(), out.consumed);
        small[out.consumed] = '\0';
    } else {
        large.assign(text.data(), out.consumed);
        token = large.c_str();
    }
#if !BASE_FP_CHARCONV
    ClassicLocale classic;
#endif
    errno = 0;
    out.value = std::strtod(token, nullptr);
    // ERANGE also flags a subnormal result, which from_chars accepts; only an underflow to zero and an overflow count.
    out.outOfRange = errno == ERANGE && (out.value == 0 || std::isinf(out.value));
    return out;
}

}  // namespace base
