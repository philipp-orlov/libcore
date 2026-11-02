// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Page-aligned power-of-two blocks that are registered once with a device (cudaHostRegister, an RDMA memory region,
// Metal no-copy buffers) and reused. Moved from librpc's PinnedPool and made allocation-free: blocks come from
// sys::vmMap, the block table and the free lists are reserved up front, so a long-running endpoint never touches the
// heap here. The registration calls and the unmapping run outside the lock.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mem {

struct Registration {
    // Returns the block's device address; nullptr = it could not be registered (take() then returns nullptr).
    void* (*registerMemory)(void* ctx, void* host, std::size_t bytes) = nullptr;
    void (*unregisterMemory)(void* ctx, void* host, std::size_t bytes) = nullptr;
    void* ctx = nullptr;
    explicit operator bool() const noexcept { return registerMemory != nullptr; }
};

class RegisteredPool {
public:
    struct Limits {
        std::size_t maxBytes = 0;       // registered bytes, 0 = none
        std::size_t maxIdleBytes = 0;   // registered bytes kept for reuse, 0 = none
        std::size_t maxBlocks = 4096;   // block table capacity (a hard cap on blocks)
    };
    struct Stats {
        std::size_t blocks = 0, bytes = 0, inUseBlocks = 0, idleBytes = 0, peakBytes = 0;
        std::uint64_t refusals = 0, evictedBlocks = 0;
    };

    RegisteredPool() : RegisteredPool(Registration{}, Limits{}) {}
    explicit RegisteredPool(Registration registration) : RegisteredPool(registration, Limits{}) {}
    RegisteredPool(Registration registration, Limits limits);
    ~RegisteredPool();
    RegisteredPool(const RegisteredPool&) = delete;
    RegisteredPool& operator=(const RegisteredPool&) = delete;

    // A block of at least `bytes` (rounded up to a power of two >= 4 KiB), or nullptr (over the limit, the table is
    // full, out of memory, or registration failed).
    void* take(std::size_t bytes);
    // `bytes` must be the size passed to take() for this block.
    void give(void* data, std::size_t bytes);
    // Device address of the byte at `host` if it lies inside a registered block, else nullptr.
    const void* device(const void* host) const;
    // Unregisters and frees idle blocks, largest first, until at most keepIdleBytes of them remain; returns bytes freed.
    std::size_t trim(std::size_t keepIdleBytes = 0);
    Stats stats() const;

private:
    static constexpr std::size_t kMinShift = 12, kClasses = 40;
    struct Info {
        std::uintptr_t host;
        std::size_t bytes;
        void* device;
    };
    struct Retired {
        Info info;
    };
    std::size_t evictLocked(std::size_t keep, Info* out, std::size_t cap);   // returns count; mutex_ held
    void retire(const Info* blocks, std::size_t n);

    Registration registration_;
    Limits limits_;
    mutable std::mutex mutex_;
    void* freeHead_[kClasses] = {};
    Info* all_ = nullptr;                 // sorted by host address
    std::size_t count_ = 0, tableBytes_ = 0;
    std::atomic<std::size_t> inUse_{0};
    std::size_t registeredBytes_ = 0, reservedBytes_ = 0, idleBytes_ = 0, peakBytes_ = 0;
    std::uint64_t refusals_ = 0, evicted_ = 0;
};

}  // namespace mem
