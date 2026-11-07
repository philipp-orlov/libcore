// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/class_table.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/status.hpp"
#include "base/platform.hpp"

namespace mem {

namespace {
std::size_t ceilPow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}
inline unsigned log2Floor(std::size_t v) noexcept {
    return static_cast<unsigned>(8 * sizeof(unsigned long long) - 1 - base::countLeadingZeros(v));
}
}  // namespace

ClassTable ClassTable::pow2(std::size_t minBytes, std::size_t maxBytes) {
    ClassTable t;
    t.shape_ = Shape::Pow2;
    std::size_t lo = std::max<std::size_t>(64, ceilPow2(minBytes));
    const std::size_t hi = ceilPow2(maxBytes);
    if (lo > hi)
        throw base::Error(base::Code::InvalidArgument, "ClassTable::pow2: min > max");
    t.base_ = lo;
    for (std::size_t s = lo; s <= hi; s <<= 1) {
        if (t.count_ == kMaxClasses)
            throw base::Error(base::Code::InvalidArgument, "ClassTable: too many classes");
        t.sizes_[t.count_++] = s;
    }
    return t;
}

ClassTable ClassTable::quarterSteps(std::size_t minBytes, std::size_t maxBytes) {
    ClassTable t;
    t.shape_ = Shape::Quarter;
    // Classes are 2^k * (4 + j) / 4 for j in 0..3, k >= 8 (so every size is a multiple of 64).
    std::size_t lo = std::max<std::size_t>(256, minBytes);
    std::size_t k = 8;
    while ((std::size_t{1} << (k + 1)) <= lo)
        ++k;
    // Find the first grid point >= lo.
    std::size_t octave = std::size_t{1} << k, j = 0;
    while (octave + j * (octave / 4) < lo) {
        if (++j == 4) {
            j = 0;
            octave <<= 1;
        }
    }
    t.base_ = octave + j * (octave / 4);
    for (;;) {
        const std::size_t s = octave + j * (octave / 4);
        if (s >= maxBytes) {
            if (t.count_ == kMaxClasses)
                throw base::Error(base::Code::InvalidArgument, "ClassTable: too many classes");
            t.sizes_[t.count_++] = s;
            break;
        }
        if (t.count_ == kMaxClasses)
            throw base::Error(base::Code::InvalidArgument, "ClassTable: too many classes");
        t.sizes_[t.count_++] = s;
        if (++j == 4) {
            j = 0;
            octave <<= 1;
        }
    }
    return t;
}

ClassTable ClassTable::custom(const std::size_t* sizes, std::size_t count) {
    ClassTable t;
    t.shape_ = Shape::Custom;
    if (count == 0 || count > kMaxClasses)
        throw base::Error(base::Code::InvalidArgument, "ClassTable::custom: bad class count");
    for (std::size_t i = 0; i < count; ++i) {
        if (sizes[i] == 0 || sizes[i] % 64 != 0 || (i > 0 && sizes[i] <= sizes[i - 1]))
            throw base::Error(base::Code::InvalidArgument, "ClassTable::custom: sizes must ascend and be multiples of 64");
        t.sizes_[i] = sizes[i];
    }
    t.count_ = count;
    return t;
}

ClassTable ClassTable::custom(std::initializer_list<std::size_t> sizes) { return custom(sizes.begin(), sizes.size()); }

std::size_t ClassTable::classOf(std::size_t bytes) const noexcept {
    if (bytes > maxBytes())
        return npos;
    if (bytes <= sizes_[0])
        return 0;
    switch (shape_) {
        case Shape::Pow2: {
            const unsigned lg = log2Floor(bytes - 1) + 1;  // ceil(log2(bytes))
            return lg - log2Floor(base_);
        }
        case Shape::Quarter: {
            // Binary search is branch-light and exact; the grid has <= 128 entries.
            const std::size_t* it = std::lower_bound(sizes_.data(), sizes_.data() + count_, bytes);
            return static_cast<std::size_t>(it - sizes_.data());
        }
        case Shape::Custom:
        default: {
            const std::size_t* it = std::lower_bound(sizes_.data(), sizes_.data() + count_, bytes);
            return static_cast<std::size_t>(it - sizes_.data());
        }
    }
}

}  // namespace mem
