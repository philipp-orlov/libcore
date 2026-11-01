// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/vm.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "base/status.hpp"
#include "sys/errno.hpp"

namespace sys {

std::size_t pageSize() noexcept {
    static const std::size_t size = [] {
        const long v = ::sysconf(_SC_PAGESIZE);
        return v > 0 ? static_cast<std::size_t>(v) : std::size_t{4096};
    }();
    return size;
}

std::size_t roundUpToPage(std::size_t bytes) noexcept {
    const std::size_t p = pageSize();
    return (bytes + p - 1) & ~(p - 1);
}

VmRegion tryVmMap(std::size_t bytes) noexcept {
    if (bytes == 0)
        return {};
    const std::size_t rounded = roundUpToPage(bytes);
    if (rounded < bytes)
        return {};  // overflow
    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return {};
    return {p, rounded};
}

VmRegion vmMap(std::size_t bytes) {
    VmRegion r = tryVmMap(bytes);
    if (!r.base && bytes != 0) {
        const int e = errno;
        throw base::Error(base::Code::ResourceExhausted, errnoMessage("mmap", e), static_cast<std::uint32_t>(e));
    }
    return r;
}

VmRegion tryVmMapAligned(std::size_t bytes, std::size_t alignment) noexcept {
    if (bytes == 0)
        return {};
    const std::size_t page = pageSize();
    if (alignment <= page)
        return tryVmMap(bytes);
    const std::size_t want = roundUpToPage(bytes);
    if (want + alignment < want)
        return {};
    VmRegion over = tryVmMap(want + alignment);
    if (!over.base)
        return {};
    const auto start = reinterpret_cast<std::uintptr_t>(over.base);
    const std::uintptr_t aligned = (start + alignment - 1) & ~(alignment - 1);
    const std::size_t head = aligned - start;
    const std::size_t tail = over.bytes - head - want;
    if (head)
        ::munmap(over.base, head);
    if (tail)
        ::munmap(reinterpret_cast<void*>(aligned + want), tail);
    return {reinterpret_cast<void*>(aligned), want};
}

VmRegion vmMapAligned(std::size_t bytes, std::size_t alignment) {
    VmRegion r = tryVmMapAligned(bytes, alignment);
    if (!r.base && bytes != 0)
        throw base::Error(base::Code::ResourceExhausted, "aligned mmap failed", static_cast<std::uint32_t>(ENOMEM));
    return r;
}

void vmUnmap(void* base, std::size_t bytes) noexcept {
    if (base)
        ::munmap(base, bytes);
}

void vmUnmap(VmRegion region) noexcept { vmUnmap(region.base, region.bytes); }

void vmDiscard(void* p, std::size_t bytes) noexcept {
    // Whole system pages inside the range only: madvise() refuses an unaligned start, and widening the range would
    // throw away the bytes of neighbours that share the partial pages.
    const std::uintptr_t sp = pageSize();
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t first = (at + sp - 1) & ~(sp - 1);
    const std::uintptr_t last = (at + bytes) & ~(sp - 1);
    if (last <= first)
        return;
    (void)::madvise(reinterpret_cast<void*>(first), last - first, MADV_DONTNEED);
}

VmRegion vmReserve(std::size_t bytes) {
    const std::size_t rounded = roundUpToPage(bytes);
    void* p = ::mmap(nullptr, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        const int e = errno;
        throw base::Error(base::Code::ResourceExhausted, errnoMessage("mmap(reserve)", e), static_cast<std::uint32_t>(e));
    }
    return {p, rounded};
}

void vmCommit(void* p, std::size_t bytes) {
    if (::mprotect(p, roundUpToPage(bytes), PROT_READ | PROT_WRITE) != 0) {
        const int e = errno;
        throw base::Error(base::Code::ResourceExhausted, errnoMessage("mprotect(commit)", e), static_cast<std::uint32_t>(e));
    }
}

void vmAdviseHuge(void* p, std::size_t bytes) noexcept {
#ifdef MADV_HUGEPAGE
    (void)::madvise(p, bytes, MADV_HUGEPAGE);
#else
    (void)p;
    (void)bytes;
#endif
}

bool vmLock(void* p, std::size_t bytes) noexcept { return ::mlock(p, bytes) == 0; }
void vmUnlock(void* p, std::size_t bytes) noexcept { (void)::munlock(p, bytes); }

}  // namespace sys
