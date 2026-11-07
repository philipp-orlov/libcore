// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// An owned file descriptor, closed when destroyed. Move-only. (Moved from libbufs.)
#pragma once

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <utility>

namespace sys {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    int release() { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0)
#if defined(_WIN32)
            ::_close(fd_);
#else
            ::close(fd_);
#endif
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace sys
