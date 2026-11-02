// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/shm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "base/status.hpp"
#include "sys/errno.hpp"

namespace sys {

namespace {
[[noreturn]] void fail(base::Code code, const char* what, int e) {
    throw base::Error(code, e ? errnoMessage(what, e) : std::string(what), static_cast<std::uint32_t>(e));
}
}  // namespace

SharedMemory::SharedMemory(SharedMemory&& o) noexcept : fd_(std::move(o.fd_)), data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

SharedMemory& SharedMemory::operator=(SharedMemory&& o) noexcept {
    if (this != &o) {
        unmap();
        fd_ = std::move(o.fd_);
        data_ = std::exchange(o.data_, nullptr);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

SharedMemory SharedMemory::map(Fd fd, std::size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (p == MAP_FAILED)
        fail(base::Code::ResourceExhausted, "mmap", errno);
    SharedMemory m;
    m.fd_ = std::move(fd);
    m.data_ = static_cast<std::uint8_t*>(p);
    m.size_ = bytes;
    return m;
}

void SharedMemory::unmap() noexcept {
    if (data_)
        ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
    fd_.reset();
}

#if defined(__linux__)

SharedMemory SharedMemory::create(const char* label, std::size_t bytes) {
    if (bytes == 0)
        fail(base::Code::InvalidArgument, "empty shared memory", 0);
    Fd fd(::memfd_create(label, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd)
        fail(base::Code::Internal, "memfd_create", errno);
    if (::ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0)
        fail(base::Code::ResourceExhausted, "ftruncate", errno);
    if (::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
        fail(base::Code::Internal, "F_ADD_SEALS", errno);
    return map(std::move(fd), bytes);
}

SharedMemory SharedMemory::adopt(Fd fd, std::size_t maxBytes) {
    if (!fd)
        fail(base::Code::InvalidArgument, "no descriptor", 0);
    const int seals = ::fcntl(fd.get(), F_GET_SEALS);
    if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW))
        fail(base::Code::FailedPrecondition, "shared memory is not sealed against resizing", 0);
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || st.st_size <= 0)
        fail(base::Code::InvalidArgument, "shared memory has no size", 0);
    const std::size_t bytes = static_cast<std::size_t>(st.st_size);
    if (bytes > maxBytes)
        fail(base::Code::OutOfRange, "shared memory is larger than allowed", 0);
    return map(std::move(fd), bytes);
}

#else

SharedMemory SharedMemory::create(const char*, std::size_t) { fail(base::Code::Unimplemented, "sys::SharedMemory needs memfd (Linux)", 0); }
SharedMemory SharedMemory::adopt(Fd, std::size_t) { fail(base::Code::Unimplemented, "sys::SharedMemory needs memfd (Linux)", 0); }

#endif

}  // namespace sys
