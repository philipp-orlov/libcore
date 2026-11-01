// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Slice / WritableSlice: non-owning byte ranges, the currency of key/value APIs. Behaviour matches the
// libnosql originals (byte-wise ordering, shorter-is-less on a common prefix, clamped subspan).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "base/status.hpp"

namespace base {

class Slice {
public:
    constexpr Slice() noexcept = default;
    constexpr Slice(const void* p, std::size_t n) noexcept : data_(static_cast<const std::byte*>(p)), size_(n) {}
    Slice(std::string_view s) noexcept : data_(reinterpret_cast<const std::byte*>(s.data())), size_(s.size()) {}
    Slice(const std::string& s) noexcept : Slice(std::string_view(s)) {}
    Slice(const char* s) noexcept : Slice(std::string_view(s ? s : "")) {}

    // View the object representation of a trivially copyable value: Slice::ref(id).
    template <class T>
    static Slice ref(const T& v) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "Slice::ref needs a trivially copyable type");
        return Slice(&v, sizeof(T));
    }

    constexpr const std::byte* data() const noexcept { return data_; }
    const char* chars() const noexcept { return reinterpret_cast<const char*>(data_); }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr explicit operator bool() const noexcept { return data_ != nullptr; }

    std::string_view view() const noexcept { return {chars(), size_}; }
    std::string string() const { return {chars(), size_}; }

    // Copies the bytes out as T. The sizes must match exactly, otherwise base::Error(InvalidArgument).
    template <class T>
    T as() const {
        static_assert(std::is_trivially_copyable_v<T>, "Slice::as needs a trivially copyable type");
        if (size_ != sizeof(T))
            throw Error(Code::InvalidArgument, "slice size does not match requested type");
        T v{};
        std::memcpy(&v, data_, sizeof(T));
        return v;
    }

    constexpr Slice subspan(std::size_t off, std::size_t n = ~std::size_t{0}) const noexcept {
        if (off > size_)
            off = size_;
        const std::size_t left = size_ - off;
        return Slice(data_ + off, n < left ? n : left);
    }

    int compare(Slice o) const noexcept {
        const std::size_t n = size_ < o.size_ ? size_ : o.size_;
        if (n) {
            const int c = std::memcmp(data_, o.data_, n);
            if (c)
                return c;
        }
        return size_ < o.size_ ? -1 : (size_ > o.size_ ? 1 : 0);
    }
    bool startsWith(Slice p) const noexcept {
        return size_ >= p.size_ && (p.size_ == 0 || std::memcmp(data_, p.data_, p.size_) == 0);
    }
    bool endsWith(Slice p) const noexcept {
        return size_ >= p.size_ && (p.size_ == 0 || std::memcmp(data_ + size_ - p.size_, p.data_, p.size_) == 0);
    }

    friend bool operator==(Slice a, Slice b) noexcept {
        return a.size_ == b.size_ && (a.size_ == 0 || std::memcmp(a.data_, b.data_, a.size_) == 0);
    }
    friend bool operator!=(Slice a, Slice b) noexcept { return !(a == b); }
    friend bool operator<(Slice a, Slice b) noexcept { return a.compare(b) < 0; }
    friend bool operator<=(Slice a, Slice b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>(Slice a, Slice b) noexcept { return a.compare(b) > 0; }
    friend bool operator>=(Slice a, Slice b) noexcept { return a.compare(b) >= 0; }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

class WritableSlice {
public:
    constexpr WritableSlice() noexcept = default;
    constexpr WritableSlice(void* p, std::size_t n) noexcept : data_(static_cast<std::byte*>(p)), size_(n) {}

    constexpr std::byte* data() const noexcept { return data_; }
    char* chars() const noexcept { return reinterpret_cast<char*>(data_); }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

    // Copies min(n, size()) bytes.
    void assign(const void* src, std::size_t n) const noexcept { std::memcpy(data_, src, n < size_ ? n : size_); }
    void assign(Slice s) const noexcept { assign(s.data(), s.size()); }

    operator Slice() const noexcept { return Slice(data_, size_); }

private:
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace base
