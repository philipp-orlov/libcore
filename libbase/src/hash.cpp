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

namespace {

constexpr uint32_t kSha256Round[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotateRight(uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::reset() noexcept {
    static constexpr uint32_t kInitial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInitial, sizeof h_);
    bytes_ = 0;
    fill_ = 0;
}

void Sha256::block(const uint8_t* p) noexcept {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choose + kSha256Round[i] + w[i];
        const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
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

void Sha256::finish(uint8_t out[32]) noexcept {
    const uint64_t bits = bytes_ * 8;
    uint8_t pad[72] = {0x80};
    const std::size_t padLen = (fill_ < 56 ? 56 - fill_ : 120 - fill_);
    for (int i = 0; i < 8; ++i)
        pad[padLen + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(pad, padLen + 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
}

void Sha256::digest(const void* data, std::size_t size, uint8_t out[32]) noexcept {
    Sha256 s;
    s.update(data, size);
    s.finish(out);
}

}  // namespace base
