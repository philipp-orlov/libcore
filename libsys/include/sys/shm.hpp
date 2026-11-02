// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Anonymous shared memory that can be handed to another process and can never be resized under its mapping.
// Linux: a memfd sealed against shrinking and growing (a peer that shrank the file would make the reader die with
// SIGBUS). The descriptor stays open so it can be passed with SCM_RIGHTS. macOS/Windows: not implemented yet
// (POSIX shm with an immediate unlink / a named file mapping); the functions throw Unimplemented there.
// Failures throw base::Error with errno in detail().
#pragma once

#include <cstddef>
#include <cstdint>

#include "sys/fd.hpp"

namespace sys {

class SharedMemory {
public:
    // Seals every SharedMemory carries; adopt() requires shrink and grow.
    static constexpr int kSealsMask = 0x0001 | 0x0002;   // F_SEAL_SHRINK | F_SEAL_GROW (Linux values)

    SharedMemory() = default;
    ~SharedMemory() { unmap(); }
    SharedMemory(SharedMemory&&) noexcept;
    SharedMemory& operator=(SharedMemory&&) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // `label` only names the file in /proc/<pid>/fd and maps. Zero-filled, mapped read-write.
    static SharedMemory create(const char* label, std::size_t bytes);
    // Maps memory another process created with create(). Refuses a descriptor that is not sealed against resizing, has
    // no size, or is larger than `maxBytes` (what a peer may make this process map).
    static SharedMemory adopt(Fd fd, std::size_t maxBytes = std::size_t{1} << 32);

    std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    int fd() const noexcept { return fd_.get(); }
    Fd releaseFd() noexcept { return std::move(fd_); }
    explicit operator bool() const noexcept { return data_ != nullptr; }

private:
    static SharedMemory map(Fd fd, std::size_t bytes);
    void unmap() noexcept;

    Fd fd_;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace sys
