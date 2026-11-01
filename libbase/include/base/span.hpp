// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// base::Span<T>: a pointer and a count (the C++17 subset of std::span the libraries use).
#pragma once

#include <cstddef>
#include <type_traits>

namespace base {

template <class T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}
    template <std::size_t N>
    constexpr Span(T (&array)[N]) noexcept : data_(array), size_(N) {}
    // From std::vector, std::array, std::string or anything else contiguous with data() and size().
    template <class C, class = std::enable_if_t<std::is_convertible_v<decltype(std::declval<C&>().data()), T*>>>
    constexpr Span(C& container) noexcept : data_(container.data()), size_(container.size()) {}
    template <class C, class = std::enable_if_t<std::is_convertible_v<decltype(std::declval<const C&>().data()), T*> &&
                                                std::is_const_v<T>>>
    constexpr Span(const C& container) noexcept : data_(container.data()), size_(container.size()) {}

    constexpr T* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr std::size_t sizeBytes() const noexcept { return size_ * sizeof(T); }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr T& operator[](std::size_t i) const noexcept { return data_[i]; }
    constexpr T* begin() const noexcept { return data_; }
    constexpr T* end() const noexcept { return data_ + size_; }
    constexpr T& front() const noexcept { return data_[0]; }
    constexpr T& back() const noexcept { return data_[size_ - 1]; }

    // Clamped like Slice::subspan: an offset past the end gives an empty span.
    constexpr Span subspan(std::size_t off, std::size_t n = ~std::size_t{0}) const noexcept {
        if (off > size_)
            off = size_;
        const std::size_t left = size_ - off;
        return Span(data_ + off, n < left ? n : left);
    }
    constexpr Span first(std::size_t n) const noexcept { return subspan(0, n); }
    constexpr Span last(std::size_t n) const noexcept { return subspan(n < size_ ? size_ - n : 0); }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace base
