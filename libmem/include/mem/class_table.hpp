// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>

namespace mem {

// Maps a request size to the size class that serves it. Built once; lookups never allocate.
class ClassTable {
public:
    static constexpr std::size_t kMaxClasses = 128;
    static constexpr std::size_t npos = ~std::size_t{0};

    // 256, 512, 1024 ... (every power of two between min and max, both rounded up to powers of two).
    static ClassTable pow2(std::size_t minBytes, std::size_t maxBytes);
    // Four classes per octave: 1,1.25,1.5,1.75 x 2^k (min/max rounded up to a multiple of 64 and the grid).
    static ClassTable quarterSteps(std::size_t minBytes, std::size_t maxBytes);
    // Explicit ascending sizes (each a multiple of 64).
    static ClassTable custom(std::initializer_list<std::size_t> sizes);
    static ClassTable custom(const std::size_t* sizes, std::size_t count);

    std::size_t count() const noexcept { return count_; }
    std::size_t bytesOf(std::size_t cls) const noexcept { return sizes_[cls]; }
    std::size_t maxBytes() const noexcept { return count_ ? sizes_[count_ - 1] : 0; }
    // Smallest class whose size is >= bytes, or npos when bytes exceeds the largest class.
    std::size_t classOf(std::size_t bytes) const noexcept;

private:
    enum class Shape { Pow2, Quarter, Custom };
    Shape shape_ = Shape::Custom;
    std::size_t count_ = 0;
    std::size_t base_ = 0;  // Pow2/Quarter: size of class 0
    std::array<std::size_t, kMaxClasses> sizes_{};
};

}  // namespace mem
