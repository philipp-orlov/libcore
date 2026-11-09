// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// UTF-8 <-> UTF-16, for the languages and formats whose strings are specified in code units.
//   * Nothing is ever refused: a stray byte in UTF-8 decodes to U+FFFD and decoding carries on.
//   * A lone surrogate is representable in such a string, so it must survive a round trip: it encodes as the three
//     bytes of its code unit (WTF-8), which is not valid UTF-8. Use isValidUtf8 on the result where that matters.
// `Out` is any basic_string: the caller picks its allocator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace base {

inline constexpr std::uint32_t kFirstSurrogate = 0xD800, kFirstLowSurrogate = 0xDC00, kLastSurrogate = 0xDFFF;
inline constexpr std::uint32_t kReplacementChar = 0xFFFD;

constexpr bool isHighSurrogate(std::uint32_t unit) noexcept { return unit >= kFirstSurrogate && unit < kFirstLowSurrogate; }
constexpr bool isLowSurrogate(std::uint32_t unit) noexcept { return unit >= kFirstLowSurrogate && unit <= kLastSurrogate; }
constexpr std::uint32_t joinSurrogates(std::uint32_t high, std::uint32_t low) noexcept {
    return 0x10000 + ((high - kFirstSurrogate) << 10) + (low - kFirstLowSurrogate);
}

// Appends `cp` as one code unit, or as a surrogate pair above the first plane.
template <class Out>
void appendCodePointUtf16(Out& out, std::uint32_t cp) {
    using C = typename Out::value_type;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out.push_back(C(kFirstSurrogate + (cp >> 10)));
        out.push_back(C(kFirstLowSurrogate + (cp & 0x3FF)));
    } else {
        out.push_back(C(cp));
    }
}

template <class Out>
void appendUtf8FromUtf16(std::u16string_view in, Out& out) {
    out.reserve(out.size() + in.size());
    using C = typename Out::value_type;
    for (std::size_t i = 0; i < in.size(); ++i) {
        // A run of ASCII, which is most text, copied in one go.
        if (in[i] < 0x80) {
            std::size_t end = i + 1;
            while (end < in.size() && in[end] < 0x80)
                ++end;
            const std::size_t at = out.size();
            out.resize(at + (end - i));
            C* to = &out[at];
            for (std::size_t k = i; k < end; ++k)
                *to++ = C(in[k]);
            i = end - 1;
            continue;
        }
        std::uint32_t cp = in[i];
        if (isHighSurrogate(cp) && i + 1 < in.size() && isLowSurrogate(in[i + 1]))
            cp = joinSurrogates(cp, in[++i]);
        if (cp < 0x80) {
            out.push_back(C(cp));
        } else if (cp < 0x800) {
            out.push_back(C(0xC0 | (cp >> 6)));
            out.push_back(C(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(C(0xE0 | (cp >> 12)));
            out.push_back(C(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(C(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(C(0xF0 | (cp >> 18)));
            out.push_back(C(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(C(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(C(0x80 | (cp & 0x3F)));
        }
    }
}

template <class Out>
void appendUtf16FromUtf8(std::string_view in, Out& out) {
    out.reserve(out.size() + in.size());
    using C16 = typename Out::value_type;
    for (std::size_t i = 0; i < in.size();) {
        // A run of ASCII, which is most text, copied in one go.
        if (static_cast<std::uint8_t>(in[i]) < 0x80) {
            std::size_t end = i + 1;
            while (end < in.size() && static_cast<std::uint8_t>(in[end]) < 0x80)
                ++end;
            const std::size_t at = out.size();
            out.resize(at + (end - i));
            C16* to = &out[at];
            for (std::size_t k = i; k < end; ++k)
                *to++ = C16(static_cast<std::uint8_t>(in[k]));
            i = end;
            continue;
        }
        const auto c = static_cast<std::uint8_t>(in[i]);
        std::uint32_t cp = c;
        std::size_t extra = 0;
        if (c >= 0xF8) {
            cp = kReplacementChar;  // not a lead byte
        } else if (c >= 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if (c >= 0x80) {
            cp = kReplacementChar;  // a stray continuation byte
        }
        if (i + extra >= in.size()) {  // a truncated sequence swallows nothing but its lead byte
            extra = 0;
            cp = kReplacementChar;
        }
        for (std::size_t k = 1; k <= extra; ++k)
            cp = (cp << 6) | (static_cast<std::uint8_t>(in[i + k]) & 0x3Fu);
        i += extra + 1;
        appendCodePointUtf16(out, cp);
    }
}

}  // namespace base
