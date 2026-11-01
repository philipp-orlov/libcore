// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace base {

std::uint64_t xxh3_64(const void* data, std::size_t size) noexcept;
const char* xxh3Backend() noexcept;

class Xxh3State
{
public:
    Xxh3State() noexcept;
    Xxh3State(const Xxh3State&) = delete;
    Xxh3State& operator=(const Xxh3State&) = delete;
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    std::uint64_t digest() const noexcept;

private:
    std::array<std::uint64_t, 8> accumulators_{};
    std::array<std::byte, 256> buffer_{};
    std::size_t bufferedBytes_ = 0;
    std::size_t stripesInBlock_ = 0;
    std::uint64_t length_ = 0;
};

}  // namespace base
