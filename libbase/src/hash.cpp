// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "base/hash.hpp"

#include <cstring>

namespace base {

uint64_t hashBytes64(const void* data, std::size_t n) noexcept {
    const char* p = static_cast<const char*>(data);
    constexpr uint64_t k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    auto load64 = [](const char* q) {
        uint64_t w;
        std::memcpy(&w, q, 8);
        return w;
    };
    auto load32 = [](const char* q) {
        uint32_t w;
        std::memcpy(&w, q, 4);
        return uint64_t(w);
    };
    uint64_t h = k2 ^ (n * k1);
    uint64_t tail;
    if (n >= 8) {
        const char* const last = p + n - 8;
        while (p < last) {
            h = (h ^ (load64(p) * k1)) * k2;
            h ^= h >> 29;
            p += 8;
        }
        tail = load64(last);
    } else if (n >= 4) {
        tail = load32(p) | (load32(p + n - 4) << 32);
    } else if (n > 0) {
        tail = uint64_t(static_cast<unsigned char>(p[0])) | uint64_t(static_cast<unsigned char>(p[n / 2])) << 8 |
               uint64_t(static_cast<unsigned char>(p[n - 1])) << 16;
    } else {
        tail = 0;
    }
    h = (h ^ (tail * k1)) * k2;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

namespace {
inline uint32_t rotl(uint32_t v, int b) noexcept { return (v << b) | (v >> (32 - b)); }
}  // namespace

void Sha1::reset() noexcept {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    bytes_ = 0;
    fill_ = 0;
}

void Sha1::block(const uint8_t* p) noexcept {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t{p[i * 4]} << 24) | (uint32_t{p[i * 4 + 1]} << 16) | (uint32_t{p[i * 4 + 2]} << 8) |
               uint32_t{p[i * 4 + 3]};
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_ += size;
    if (fill_ != 0) {
        const std::size_t take = size < 64 - fill_ ? size : 64 - fill_;
        std::memcpy(buf_ + fill_, p, take);
        fill_ += static_cast<uint32_t>(take);
        p += take;
        size -= take;
        if (fill_ < 64)
            return;
        block(buf_);
        fill_ = 0;
    }
    while (size >= 64) {
        block(p);
        p += 64;
        size -= 64;
    }
    if (size) {
        std::memcpy(buf_, p, size);
        fill_ = static_cast<uint32_t>(size);
    }
}

void Sha1::finish(uint8_t out[20]) noexcept {
    const uint64_t bits = bytes_ * 8;
    uint8_t pad[72] = {0x80};
    const std::size_t padLen = (fill_ < 56 ? 56 - fill_ : 120 - fill_);
    for (int i = 0; i < 8; ++i)
        pad[padLen + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(pad, padLen + 8);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
}

void Sha1::digest(const void* data, std::size_t size, uint8_t out[20]) noexcept {
    Sha1 s;
    s.update(data, size);
    s.finish(out);
}

}  // namespace base
