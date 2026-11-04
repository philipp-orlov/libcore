// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Non-cryptographic hashes and checksums shared by every library. XXH3-64 is in base/xxh3.hpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace base {

// FNV-1a, 32 bit. This is how libbufs turns "ns.Type" into a type id and librpc turns "ns.Service.Method" into a
// method id: both are on the wire, so the result for a given name must never change.
constexpr uint32_t fnv1a32(std::string_view text) noexcept {
    uint32_t hash = 2166136261u;
    for (char c : text) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

// Fast 64-bit hash for short keys (identifiers, column names, symbols); lifted unchanged from libsql's flat_hash.
// Not for untrusted input that must resist collision attacks, and not stable across versions: never persist it.
uint64_t hashBytes64(const void* data, std::size_t size) noexcept;

// Mixes a pointer into a table index. The low bits of an aligned pointer never vary, so they are dropped first.
inline std::size_t hashPointer(const void* p) noexcept {
    uint64_t v = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(p)) >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return static_cast<std::size_t>(v ^ (v >> 29));
}

// Smallest power of two >= n, and at least `minimum` (itself a power of two): the size of an open-addressed table.
constexpr std::size_t tableSizeFor(std::size_t n, std::size_t minimum = 8) noexcept {
    std::size_t size = minimum;
    while (size < n)
        size *= 2;
    return size;
}

// SHA-1 (RFC 3174). Kept only because RFC 6455 mandates it for the WebSocket handshake; not for security.
class Sha1 {
public:
    Sha1() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void finish(uint8_t out[20]) noexcept;  // the object must be reset() before reuse
    static void digest(const void* data, std::size_t size, uint8_t out[20]) noexcept;

private:
    void block(const uint8_t* p) noexcept;
    uint32_t h_[5];
    uint8_t buf_[64];
    uint64_t bytes_;
    uint32_t fill_;
};

// CRC32C (Castagnoli) of `size` bytes, continuing from `crc` (the CRC of the preceding bytes, 0 to start).
// Uses the CPU's CRC instructions (SSE4.2 on x86-64, the ARMv8 CRC extension) when the running CPU has them,
// decided once at the first call; otherwise a table-driven loop. Results are identical.
uint32_t crc32c(const void* data, std::size_t size, uint32_t crc = 0) noexcept;
uint32_t crc32cPortable(const void* data, std::size_t size, uint32_t crc = 0) noexcept;
// "sse4.2", "armv8-crc" or "portable": the kernel crc32c() picks on this machine.
const char* crc32cBackend() noexcept;

}  // namespace base
