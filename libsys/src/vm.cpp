// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/vm.hpp"

#include <cerrno>
#include <cstdint>

#ifdef _WIN32
// NOT BUILT OR RUN on this machine: the Windows implementations are at the end of this file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "base/status.hpp"
#include "sys/errno.hpp"

namespace sys {

#ifndef _WIN32
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

void vmPrefault(void* p, std::size_t bytes) noexcept {
    if (!p || !bytes)
        return;
#if defined(__linux__)
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
    const std::uintptr_t page = pageSize();
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(p) & ~(page - 1);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(p) + bytes;
    if (::madvise(reinterpret_cast<void*>(begin), end - begin, MADV_POPULATE_WRITE) == 0)
        return;
#endif
    const auto* bytesIn = static_cast<const volatile unsigned char*>(p);
    for (std::size_t off = 0; off < bytes; off += pageSize())
        (void)bytesIn[off];
    (void)bytesIn[bytes - 1];
}

bool vmLock(void* p, std::size_t bytes) noexcept { return ::mlock(p, bytes) == 0; }
void vmUnlock(void* p, std::size_t bytes) noexcept { (void)::munlock(p, bytes); }

bool vmProtectExecutable(void* p, std::size_t bytes) noexcept {
    if (::mprotect(p, bytes, PROT_READ | PROT_EXEC) != 0)
        return false;
#if defined(__GNUC__)
    // arm64 has separate instruction and data caches; on x86 the builtin compiles to nothing.
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + bytes);
#endif
    return true;
}

bool vmProtectWritable(void* p, std::size_t bytes) noexcept {
    return ::mprotect(p, bytes, PROT_READ | PROT_WRITE) == 0;
}

#else  // ---- Windows ----

std::size_t pageSize() noexcept {
    static const std::size_t size = [] { SYSTEM_INFO si; ::GetSystemInfo(&si); return static_cast<std::size_t>(si.dwPageSize ? si.dwPageSize : 4096); }();
    return size;
}
std::size_t roundUpToPage(std::size_t bytes) noexcept { const std::size_t p = pageSize(); return (bytes + p - 1) & ~(p - 1); }

VmRegion tryVmMap(std::size_t bytes) noexcept {
    if (bytes == 0) return {};
    const std::size_t rounded = roundUpToPage(bytes);
    void* p = ::VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p ? VmRegion{p, rounded} : VmRegion{};
}
VmRegion vmMap(std::size_t bytes) {
    VmRegion r = tryVmMap(bytes);
    if (!r.base && bytes != 0)
        throw base::Error(base::Code::ResourceExhausted, "VirtualAlloc failed", static_cast<std::uint32_t>(::GetLastError()));
    return r;
}
VmRegion tryVmMapAligned(std::size_t bytes, std::size_t alignment) noexcept {
    if (bytes == 0) return {};
    const std::size_t want = roundUpToPage(bytes);
    if (alignment <= 65536) return tryVmMap(bytes);   // VirtualAlloc already aligns to the 64 KiB allocation granularity
    // Reserve a larger range to find an aligned address, release it, and map exactly there (retry on a race).
    for (int attempt = 0; attempt < 8; ++attempt) {
        void* probe = ::VirtualAlloc(nullptr, want + alignment, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return {};
        const auto start = reinterpret_cast<std::uintptr_t>(probe);
        const std::uintptr_t aligned = (start + alignment - 1) & ~(alignment - 1);
        ::VirtualFree(probe, 0, MEM_RELEASE);
        void* p = ::VirtualAlloc(reinterpret_cast<void*>(aligned), want, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return {p, want};
    }
    return {};
}
VmRegion vmMapAligned(std::size_t bytes, std::size_t alignment) {
    VmRegion r = tryVmMapAligned(bytes, alignment);
    if (!r.base && bytes != 0)
        throw base::Error(base::Code::ResourceExhausted, "aligned VirtualAlloc failed");
    return r;
}
void vmUnmap(void* base, std::size_t) noexcept { if (base) ::VirtualFree(base, 0, MEM_RELEASE); }
void vmUnmap(VmRegion region) noexcept { vmUnmap(region.base, region.bytes); }
void vmDiscard(void* p, std::size_t bytes) noexcept {
    const std::uintptr_t sp = pageSize();
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t first = (at + sp - 1) & ~(sp - 1);
    const std::uintptr_t last = (at + bytes) & ~(sp - 1);
    if (last <= first) return;
    (void)::VirtualAlloc(reinterpret_cast<void*>(first), last - first, MEM_RESET, PAGE_READWRITE);
}
VmRegion vmReserve(std::size_t bytes) {
    const std::size_t rounded = roundUpToPage(bytes);
    void* p = ::VirtualAlloc(nullptr, rounded, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) throw base::Error(base::Code::ResourceExhausted, "VirtualAlloc(reserve) failed", static_cast<std::uint32_t>(::GetLastError()));
    return {p, rounded};
}
void vmCommit(void* p, std::size_t bytes) {
    if (!::VirtualAlloc(p, roundUpToPage(bytes), MEM_COMMIT, PAGE_READWRITE))
        throw base::Error(base::Code::ResourceExhausted, "VirtualAlloc(commit) failed", static_cast<std::uint32_t>(::GetLastError()));
}
void vmAdviseHuge(void*, std::size_t) noexcept {}
void vmPrefault(void* p, std::size_t bytes) noexcept {
    if (!p || !bytes)
        return;
    const auto* bytesIn = static_cast<const volatile unsigned char*>(p);
    for (std::size_t off = 0; off < bytes; off += pageSize())
        (void)bytesIn[off];
    (void)bytesIn[bytes - 1];
}
bool vmLock(void* p, std::size_t bytes) noexcept { return ::VirtualLock(p, bytes) != 0; }
void vmUnlock(void* p, std::size_t bytes) noexcept { (void)::VirtualUnlock(p, bytes); }

bool vmProtectExecutable(void* p, std::size_t bytes) noexcept {
    DWORD previous = 0;
    if (!VirtualProtect(p, bytes, PAGE_EXECUTE_READ, &previous))
        return false;
    FlushInstructionCache(GetCurrentProcess(), p, bytes);
    return true;
}

bool vmProtectWritable(void* p, std::size_t bytes) noexcept {
    DWORD previous = 0;
    return VirtualProtect(p, bytes, PAGE_READWRITE, &previous) != 0;
}

#endif

}  // namespace sys
