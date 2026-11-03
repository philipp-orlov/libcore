// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "json/writer.hpp"

#include "json/escape.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

void Writer::put(char c) noexcept {
    if (buf_ && len_ < cap_)
        buf_[len_] = c;
    ++len_;
}

void Writer::put(std::string_view s) noexcept {
    if (buf_ && len_ < cap_)
        std::memcpy(buf_ + len_, s.data(), std::min(s.size(), cap_ - len_));
    len_ += s.size();
}

void Writer::beforeValue() {
    if (depth_ == 0)
        return;
    const std::uint64_t bit = std::uint64_t{1} << (depth_ - 1);
    if (isObject_ & bit) {
        if (!afterKey_) {
            good_ = false;   // a value in an object needs a key first
            return;
        }
        afterKey_ = false;
    } else {
        if (hasItems_ & bit)
            put(',');
        hasItems_ |= bit;
    }
}

Writer& Writer::beginObject() {
    beforeValue();
    if (depth_ >= kMaxDepth) { good_ = false; return *this; }
    put('{');
    ++depth_;
    isObject_ |= std::uint64_t{1} << (depth_ - 1);
    hasItems_ &= ~(std::uint64_t{1} << (depth_ - 1));
    return *this;
}

Writer& Writer::endObject() {
    if (depth_ == 0 || !(isObject_ & (std::uint64_t{1} << (depth_ - 1))) || afterKey_) { good_ = false; return *this; }
    put('}');
    --depth_;
    return *this;
}

Writer& Writer::beginArray() {
    beforeValue();
    if (depth_ >= kMaxDepth) { good_ = false; return *this; }
    put('[');
    ++depth_;
    isObject_ &= ~(std::uint64_t{1} << (depth_ - 1));
    hasItems_ &= ~(std::uint64_t{1} << (depth_ - 1));
    return *this;
}

Writer& Writer::endArray() {
    if (depth_ == 0 || (isObject_ & (std::uint64_t{1} << (depth_ - 1)))) { good_ = false; return *this; }
    put(']');
    --depth_;
    return *this;
}

void Writer::escaped(std::string_view s) {
    struct Sink {
        Writer& w;
        void put(const char* p, std::size_t n) noexcept { w.put(std::string_view(p, n)); }
    } sink{*this};
    put('"');
    escapeTo(sink, s);
    put('"');
}

Writer& Writer::key(std::string_view name) {
    if (depth_ == 0 || !(isObject_ & (std::uint64_t{1} << (depth_ - 1))) || afterKey_) { good_ = false; return *this; }
    const std::uint64_t bit = std::uint64_t{1} << (depth_ - 1);
    if (hasItems_ & bit)
        put(',');
    hasItems_ |= bit;
    escaped(name);
    put(':');
    afterKey_ = true;
    return *this;
}

Writer& Writer::null() { beforeValue(); put("null"); return *this; }
Writer& Writer::value(bool v) { beforeValue(); put(v ? "true" : "false"); return *this; }

Writer& Writer::value(std::int64_t v) {
    beforeValue();
    char tmp[24];
    const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
    put(std::string_view(tmp, static_cast<std::size_t>(r.ptr - tmp)));
    return *this;
}

Writer& Writer::value(std::uint64_t v) {
    beforeValue();
    char tmp[24];
    const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
    put(std::string_view(tmp, static_cast<std::size_t>(r.ptr - tmp)));
    return *this;
}

Writer& Writer::value(double v) {
    beforeValue();
    if (!std::isfinite(v)) {
        put("null");
        return *this;
    }
    char tmp[40];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);   // shortest round-trip, locale independent
    std::size_t n = static_cast<std::size_t>(r.ptr - tmp);
#else
    // Older libstdc++ (gcc 9, 10) has no floating-point to_chars: the fewest %g digits that read back as the same
    // double, as to_chars would choose (5e-324, not %.17g's 4.9406564584124654e-324), then undo a locale's decimal
    // comma. snprintf and strtod agree on the locale, and neither allocates.
    int n0 = 0;
    for (int digits = 1; digits <= 17; ++digits) {
        n0 = std::snprintf(tmp, sizeof tmp, "%.*g", digits, v);
        if (std::strtod(tmp, nullptr) == v)
            break;
    }
    std::size_t n = static_cast<std::size_t>(n0);
    for (std::size_t i = 0; i < n; ++i)
        if (tmp[i] == ',') tmp[i] = '.';
#endif
    // JSON has no "1e+22" ambiguity but does need digits on both sides of a point; to_chars never emits ".5".
    put(std::string_view(tmp, n));
    return *this;
}

Writer& Writer::value(std::string_view s) { beforeValue(); escaped(s); return *this; }
Writer& Writer::raw(std::string_view encoded) { beforeValue(); put(encoded); return *this; }

}  // namespace json
