// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// UTF-8 validation (RFC 3629: no overlongs, no surrogates, nothing above U+10FFFF) and code point conversion.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace base {

inline bool isValidUtf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        // Eight ASCII bytes at a time: most text is ASCII most of the time.
        while (i + 8 <= size) {
            std::uint64_t word;
            std::memcpy(&word, bytes + i, sizeof(word));
            if (word & 0x8080808080808080ull)
                break;
            i += 8;
        }
        if (i >= size)
            break;
        const unsigned char b0 = bytes[i];
        if (b0 < 0x80) {
            ++i;
            continue;
        }
        std::size_t need;
        std::uint32_t cp, minimum;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            need = 1, cp = b0 & 0x1F, minimum = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            need = 2, cp = b0 & 0x0F, minimum = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            need = 3, cp = b0 & 0x07, minimum = 0x10000;
        } else {
            return false;
        }
        if (i + need >= size)
            return false;  // truncated sequence
        for (std::size_t k = 1; k <= need; ++k) {
            const unsigned char b = bytes[i + k];
            if ((b & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += need + 1;
    }
    return true;
}

// Appends the encoding of `cp` at `out` (room for 4 bytes); returns the byte count, or 0 for an invalid code point
// (surrogate or > U+10FFFF).
inline std::size_t encodeCodepoint(std::uint32_t cp, char* out) noexcept {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF)
        return 0;
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

}  // namespace base
