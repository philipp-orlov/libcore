// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Streaming JSON writer into a caller buffer: no tree, no allocation. Run it once with a null buffer to learn the
// size, then again into a buffer of that size (two-pass), or give it a big enough buffer and check truncated().
// Numbers are locale independent; NaN and infinities are written as null.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace json {

class Writer {
public:
    static constexpr unsigned kMaxDepth = 64;

    Writer(char* buffer, std::size_t capacity) noexcept : buf_(buffer), cap_(capacity) {}

    Writer& beginObject();
    Writer& endObject();
    Writer& beginArray();
    Writer& endArray();
    Writer& key(std::string_view name);       // inside an object, before the value
    Writer& null();
    Writer& value(bool v);
    Writer& value(std::int64_t v);
    Writer& value(std::uint64_t v);
    Writer& value(int v) { return value(static_cast<std::int64_t>(v)); }
    Writer& value(unsigned v) { return value(static_cast<std::uint64_t>(v)); }
    Writer& value(double v);
    Writer& value(std::string_view s);        // quoted and escaped
    Writer& value(const char* s) { return s ? value(std::string_view(s)) : null(); }
    Writer& raw(std::string_view encodedJson); // an already encoded value (e.g. a stored sample), copied as is

    // Length the document has (or would have, if the buffer was too small). Never counts a terminating NUL.
    std::size_t size() const noexcept { return len_; }
    bool truncated() const noexcept { return buf_ == nullptr ? false : len_ > cap_; }
    // False after a structural mistake: value without key in an object, unbalanced end, nesting past kMaxDepth.
    bool good() const noexcept { return good_; }
    std::string_view view() const noexcept { return {buf_, len_ < cap_ ? len_ : cap_}; }

private:
    void put(char c) noexcept;
    void put(std::string_view s) noexcept;
    void beforeValue();
    void escaped(std::string_view s);

    char* buf_;
    std::size_t cap_;
    std::size_t len_ = 0;
    bool good_ = true;
    unsigned depth_ = 0;
    std::uint64_t isObject_ = 0;   // bit per level
    std::uint64_t hasItems_ = 0;
    bool afterKey_ = false;
};

// Convenience for cold paths: runs the callable (a function taking Writer&) twice and returns the text.
template <class F>
std::string toString(F&& body) {
    Writer count(nullptr, 0);
    body(count);
    std::string out(count.size(), '\0');
    Writer w(out.data(), out.size());
    body(w);
    return out;
}

}  // namespace json
