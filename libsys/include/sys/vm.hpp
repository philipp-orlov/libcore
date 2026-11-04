// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Virtual memory: anonymous mappings that never touch the process heap, and the calls that give pages back to the
// OS while keeping the address range. Everything in libmem sits on these.
#pragma once

#include <cstddef>

namespace sys {

struct VmRegion {
    void* base = nullptr;
    std::size_t bytes = 0;
};

// System page size (cached after the first call).
std::size_t pageSize() noexcept;

// Rounds up to a multiple of pageSize().
std::size_t roundUpToPage(std::size_t bytes) noexcept;

// Anonymous read-write mapping, zero filled, page aligned. `bytes` is rounded up to whole pages. Throws
// base::Error(ResourceExhausted, detail = errno) when the OS refuses; tryVmMap() returns an empty region instead.
VmRegion vmMap(std::size_t bytes);
VmRegion tryVmMap(std::size_t bytes) noexcept;

// Like vmMap, with the start aligned to `alignment` (a power of two, a multiple of the page size). Over-maps and
// trims, so no address space is wasted afterwards.
VmRegion vmMapAligned(std::size_t bytes, std::size_t alignment);
VmRegion tryVmMapAligned(std::size_t bytes, std::size_t alignment) noexcept;

// Unmaps the region returned by vmMap (the full range).
void vmUnmap(VmRegion region) noexcept;
void vmUnmap(void* base, std::size_t bytes) noexcept;

// Gives the whole pages inside [p, p+bytes) back to the OS; the address range stays valid and reads as zero
// afterwards (Linux) or as unspecified old/zero bytes (Windows MEM_RESET, macOS). Partial pages at either end are
// left alone, so neighbours that share them are never clobbered.
void vmDiscard(void* p, std::size_t bytes) noexcept;

// Reserve address space with no access, commit later. vmReserve throws like vmMap.
VmRegion vmReserve(std::size_t bytes);
void vmCommit(void* p, std::size_t bytes);

// Ask for transparent huge pages on a large region (advice only; no failure if unsupported).
void vmAdviseHuge(void* p, std::size_t bytes) noexcept;

// Faults every page of [p, p+bytes) in now, so first touches on a hot path cost no page fault and resident memory is
// flat from the start (shared rings, arenas). Linux 5.14+: MADV_POPULATE_WRITE; otherwise each page is read once
// (never written), which is safe on memory another thread or process is already using.
void vmPrefault(void* p, std::size_t bytes) noexcept;

// For a code generator: flip whole pages between writable and executable, never both at once. vmProtectExecutable
// also makes the instruction caches see what was written. Both return false when the OS refuses (a hardened kernel
// policy, for one); the mapping keeps its old protection. `p` must be page aligned.
bool vmProtectExecutable(void* p, std::size_t bytes) noexcept;
bool vmProtectWritable(void* p, std::size_t bytes) noexcept;

// Pin pages in RAM (mlock). Returns false when the limit is exceeded or unsupported.
bool vmLock(void* p, std::size_t bytes) noexcept;
void vmUnlock(void* p, std::size_t bytes) noexcept;

}  // namespace sys
