// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstring>

#include "base/hash.hpp"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define BASE_CRC_X86 1
#elif defined(__aarch64__) && defined(__linux__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#define BASE_CRC_ARM 1
#elif defined(__aarch64__) && defined(__APPLE__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define BASE_CRC_ARM_STATIC 1
#endif

namespace base {
namespace {

constexpr uint32_t kPolynomial = 0x82f63b78u;  // 0x1edc6f41 reflected

constexpr std::array<uint32_t, 256> makeTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1u) ? (value >> 1) ^ kPolynomial : value >> 1;
        table[i] = value;
    }
    return table;
}
constexpr std::array<uint32_t, 256> kTable = makeTable();

using Kernel = uint32_t (*)(const void*, std::size_t, uint32_t) noexcept;

#if defined(BASE_CRC_X86)
__attribute__((target("sse4.2"))) uint32_t crcSse42(const void* data, std::size_t size, uint32_t crc) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t state = ~crc;
    while (size >= 8) {
        uint64_t word;
        std::memcpy(&word, bytes, 8);
        state = _mm_crc32_u64(state, word);
        bytes += 8;
        size -= 8;
    }
    uint32_t tail = static_cast<uint32_t>(state);
    while (size-- > 0)
        tail = _mm_crc32_u8(tail, *bytes++);
    return ~tail;
}
#endif

#if defined(BASE_CRC_ARM)
#if defined(__clang__)
#define BASE_TARGET_CRC __attribute__((target("crc")))
#else
#define BASE_TARGET_CRC __attribute__((target("arch=armv8-a+crc")))
#endif
BASE_TARGET_CRC uint32_t crcArm(const void* data, std::size_t size, uint32_t crc) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t state = ~crc;
    while (size >= 8) {
        uint64_t word;
        std::memcpy(&word, bytes, 8);
        state = __crc32cd(state, word);
        bytes += 8;
        size -= 8;
    }
    while (size-- > 0)
        state = __crc32cb(state, *bytes++);
    return ~state;
}
#endif

#if defined(BASE_CRC_ARM_STATIC)
uint32_t crcArmStatic(const void* data, std::size_t size, uint32_t crc) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t state = ~crc;
    while (size >= 8) {
        uint64_t word;
        std::memcpy(&word, bytes, 8);
        state = __crc32cd(state, word);
        bytes += 8;
        size -= 8;
    }
    while (size-- > 0)
        state = __crc32cb(state, *bytes++);
    return ~state;
}
#endif

struct Choice {
    Kernel kernel;
    const char* name;
};

Choice choose() noexcept {
#if defined(BASE_CRC_X86)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2"))
        return {crcSse42, "sse4.2"};
#elif defined(BASE_CRC_ARM)
    if (getauxval(AT_HWCAP) & HWCAP_CRC32)
        return {crcArm, "armv8-crc"};
#elif defined(BASE_CRC_ARM_STATIC)
    return {crcArmStatic, "armv8-crc"};
#endif
    return {crc32cPortable, "portable"};
}

const Choice& choice() noexcept {
    static const Choice chosen = choose();
    return chosen;
}

}  // namespace

uint32_t crc32cPortable(const void* data, std::size_t size, uint32_t crc) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t state = ~crc;
    for (std::size_t i = 0; i < size; ++i)
        state = kTable[(state ^ bytes[i]) & 0xffu] ^ (state >> 8);
    return ~state;
}

uint32_t crc32c(const void* data, std::size_t size, uint32_t crc) noexcept { return choice().kernel(data, size, crc); }

const char* crc32cBackend() noexcept { return choice().name; }

}  // namespace base
