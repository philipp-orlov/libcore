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

// CRC32C (Castagnoli) of `size` bytes, continuing from `crc` (the CRC of the preceding bytes, 0 to start).
// Uses the CPU's CRC instructions (SSE4.2 on x86-64, the ARMv8 CRC extension) when the running CPU has them,
// decided once at the first call; otherwise a table-driven loop. Results are identical.
uint32_t crc32c(const void* data, std::size_t size, uint32_t crc = 0) noexcept;
uint32_t crc32cPortable(const void* data, std::size_t size, uint32_t crc = 0) noexcept;
// "sse4.2", "armv8-crc" or "portable": the kernel crc32c() picks on this machine.
const char* crc32cBackend() noexcept;

}  // namespace base
