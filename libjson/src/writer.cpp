// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "json/writer.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
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
    static const char kHex[] = "0123456789abcdef";
    put('"');
    std::size_t run = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        put(s.substr(run, i - run));
        run = i + 1;
        switch (c) {
            case '"': put("\\\""); break;
            case '\\': put("\\\\"); break;
            case '\n': put("\\n"); break;
            case '\r': put("\\r"); break;
            case '\t': put("\\t"); break;
            case '\b': put("\\b"); break;
            case '\f': put("\\f"); break;
            default: {
                const char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
                put(std::string_view(esc, 6));
            }
        }
    }
    put(s.substr(run));
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
    // Older libstdc++ has no floating-point to_chars: %.17g, then undo a locale's decimal comma.
    int n0 = std::snprintf(tmp, sizeof tmp, "%.17g", v);
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
