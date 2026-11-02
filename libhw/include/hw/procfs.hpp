// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Allocation-free reading of /proc and /sys text files: open/read/close into a caller's reusable buffer (no
// ifstream, no ostringstream), plus small token helpers. A sampler that runs once a second for months must not
// allocate per process per tick.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hw {

// Reads up to cap-1 bytes of `path`; NUL terminates. Returns the length, or -1 if the file cannot be opened.
inline long readTextFile(const char* path, char* buffer, std::size_t cap) noexcept {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    std::size_t total = 0;
    while (total + 1 < cap) {
        const ssize_t n = ::read(fd, buffer + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    buffer[total] = '\0';
    return static_cast<long>(total);
}

// Cursor over text: whitespace-separated tokens and lines, no copies.
class Scanner {
public:
    explicit Scanner(std::string_view text) noexcept : text_(text) {}
    bool atEnd() const noexcept { return pos_ >= text_.size(); }
    // Next token on any line (blanks and newlines separate tokens).
    std::string_view token() noexcept {
        while (pos_ < text_.size() && isBlank(text_[pos_]))
            ++pos_;
        const std::size_t start = pos_;
        while (pos_ < text_.size() && !isBlank(text_[pos_]))
            ++pos_;
        return text_.substr(start, pos_ - start);
    }
    // Next token without leaving the current line; empty at the end of the line.
    std::string_view lineToken() noexcept {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t'))
            ++pos_;
        const std::size_t start = pos_;
        while (pos_ < text_.size() && !isBlank(text_[pos_]))
            ++pos_;
        return text_.substr(start, pos_ - start);
    }
    std::string_view line() noexcept {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] != '\n')
            ++pos_;
        const std::string_view out = text_.substr(start, pos_ - start);
        if (pos_ < text_.size())
            ++pos_;
        return out;
    }
    void skipLine() noexcept { (void)line(); }

private:
    static bool isBlank(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    std::string_view text_;
    std::size_t pos_ = 0;
};

inline std::uint64_t toUnsigned(std::string_view s) noexcept {
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            break;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

}  // namespace hw
