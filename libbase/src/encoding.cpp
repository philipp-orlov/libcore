// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "base/encoding.hpp"

#include "base/ascii.hpp"

#include <array>

namespace base {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::uint8_t kBad = 0xFF;

constexpr std::array<std::uint8_t, 256> makeReverse() {
    std::array<std::uint8_t, 256> t{};
    for (auto& v : t)
        v = kBad;
    for (int i = 0; i < 64; ++i)
        t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::uint8_t>(i);
    return t;
}
constexpr std::array<std::uint8_t, 256> kReverse = makeReverse();

}  // namespace

std::size_t base64Encode(const void* data, std::size_t len, char* out) noexcept {
    const auto* in = static_cast<const std::uint8_t*>(data);
    char* o = out;
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t n = (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8) | in[i + 2];
        *o++ = kAlphabet[(n >> 18) & 0x3F];
        *o++ = kAlphabet[(n >> 12) & 0x3F];
        *o++ = kAlphabet[(n >> 6) & 0x3F];
        *o++ = kAlphabet[n & 0x3F];
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const std::uint32_t n = std::uint32_t{in[i]} << 16;
        *o++ = kAlphabet[(n >> 18) & 0x3F];
        *o++ = kAlphabet[(n >> 12) & 0x3F];
        *o++ = '=';
        *o++ = '=';
    } else if (rem == 2) {
        const std::uint32_t n = (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8);
        *o++ = kAlphabet[(n >> 18) & 0x3F];
        *o++ = kAlphabet[(n >> 12) & 0x3F];
        *o++ = kAlphabet[(n >> 6) & 0x3F];
        *o++ = '=';
    }
    return static_cast<std::size_t>(o - out);
}

std::size_t base64Decode(std::string_view in, void* out) noexcept {
    auto* o = static_cast<std::uint8_t*>(out);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : in) {
        const std::uint8_t v = kReverse[static_cast<unsigned char>(c)];
        if (v == kBad)
            continue;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *o++ = static_cast<std::uint8_t>((buffer >> bits) & 0xFF);
        }
    }
    return static_cast<std::size_t>(o - static_cast<std::uint8_t*>(out));
}

bool base64DecodeStrict(std::string_view in, void* out, std::size_t* written) noexcept {
    if (in.size() % 4 != 0)
        return false;
    auto* o = static_cast<std::uint8_t*>(out);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::uint32_t n = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=') {
                // Padding is legal only in the last quantum's final one or two positions.
                if (i + 4 != in.size() || k < 2)
                    return false;
                ++pad;
                n <<= 6;
                continue;
            }
            if (pad != 0)
                return false;  // data after padding
            const std::uint8_t v = kReverse[static_cast<unsigned char>(c)];
            if (v == kBad)
                return false;
            n = (n << 6) | v;
        }
        *o++ = static_cast<std::uint8_t>(n >> 16);
        if (pad < 2)
            *o++ = static_cast<std::uint8_t>(n >> 8);
        if (pad < 1)
            *o++ = static_cast<std::uint8_t>(n);
    }
    *written = static_cast<std::size_t>(o - static_cast<std::uint8_t*>(out));
    return true;
}

std::string base64Encode(const void* data, std::size_t len) {
    std::string s(base64Size(len), '\0');
    base64Encode(data, len, s.data());
    return s;
}

std::string base64Decode(std::string_view in) {
    std::string s(base64MaxDecodedSize(in.size()), '\0');
    s.resize(base64Decode(in, s.data()));
    return s;
}

void hexEncode(const void* data, std::size_t len, char* out) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* in = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 15];
    }
}

bool hexDecode(std::string_view in, void* out, std::size_t* written) noexcept {
    if (in.size() % 2 != 0)
        return false;
    auto* o = static_cast<std::uint8_t*>(out);
    for (std::size_t i = 0; i < in.size(); i += 2) {
        const int hi = hexValue(in[i]), lo = hexValue(in[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        *o++ = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    *written = in.size() / 2;
    return true;
}

std::string hexEncode(const void* data, std::size_t len) {
    std::string s(len * 2, '\0');
    hexEncode(data, len, s.data());
    return s;
}

}  // namespace base
