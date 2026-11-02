// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Devices, streams, events, pinned registration and a per-device caching allocator, written once against dev::Driver.
// Rules this layer enforces (BUG-H9): the current device is switched by a guard around every runtime call, a block is
// returned on the stream that last used it, and every device has its own allocator, so two GPUs never share a cache.
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "dev/driver.hpp"
#include "mem/caching_allocator.hpp"
#include "mem/registered_pool.hpp"

namespace dev {

// Makes `device` current for this thread and restores the previous one.
class DeviceGuard {
public:
    DeviceGuard(Driver& driver, int device);
    ~DeviceGuard();
    DeviceGuard(const DeviceGuard&) = delete;
    DeviceGuard& operator=(const DeviceGuard&) = delete;

private:
    Driver& driver_;
    int previous_;
    bool switched_;
};

class Event {
public:
    Event(Driver& driver, int device);
    ~Event();
    Event(Event&&) noexcept;
    Event& operator=(Event&&) noexcept;
    Event(const Event&) = delete;
    void record(StreamHandle stream);
    bool done() const;
    int device() const noexcept { return device_; }

private:
    Driver* driver_;
    int device_;
    EventHandle handle_;
};

class Stream {
public:
    Stream(Driver& driver, int device);
    ~Stream();
    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) = delete;
    Stream(const Stream&) = delete;
    int device() const noexcept { return device_; }
    StreamHandle handle() const noexcept { return handle_; }
    std::uint32_t id() const noexcept { return id_; }
    void synchronize();

private:
    Driver* driver_;
    int device_;
    StreamHandle handle_;
    std::uint32_t id_;
};

// One caching allocator per device over that device's memory.
class DeviceAllocator {
public:
    struct Options {
        std::size_t maxCachedBytes = 0;
        std::size_t budgetBytes = 0;
    };
    DeviceAllocator(Driver& driver, int device, Options options);
    DeviceAllocator(Driver& driver, int device) : DeviceAllocator(driver, device, Options{}) {}
    ~DeviceAllocator();
    DeviceAllocator(const DeviceAllocator&) = delete;
    DeviceAllocator& operator=(const DeviceAllocator&) = delete;

    mem::CachingAllocator::Block allocate(std::size_t bytes, const Stream& stream);
    // Returns the block for reuse on the stream that last used it.
    void free(mem::CachingAllocator::Block block, const Stream& lastUse);
    // `other` also read or wrote the block: the block is not reusable until an event on `other` completes.
    void recordUse(mem::CachingAllocator::Block block, const Stream& other, Event event);
    std::size_t reclaim();                 // frees blocks whose cross-stream events completed
    std::size_t emptyCache();
    mem::CachingAllocator::CacheStats stats();
    int device() const noexcept { return device_; }

private:
    static mem::Segment allocSegment(void* ctx, std::size_t bytes);
    static void freeSegment(void* ctx, mem::Segment s);
    struct Pending {
        std::uint32_t stream;
        Event event;
    };
    Driver& driver_;
    int device_;
    std::mutex mutex_;
    std::unique_ptr<mem::CachingAllocator> cache_;
    std::vector<Pending> pending_;
};

// Pinned (page-locked) host memory registered once and reused: mem::RegisteredPool wired to hostRegister.
std::unique_ptr<mem::RegisteredPool> makePinnedPool(Driver& driver, mem::RegisteredPool::Limits limits = {});

// A process-wide view of all devices: a driver plus an allocator per device.
class System {
public:
    explicit System(std::unique_ptr<Driver> driver);
    Driver& driver() noexcept { return *driver_; }
    int deviceCount() const noexcept { return static_cast<int>(allocators_.size()); }
    DeviceAllocator& allocator(int device);   // throws base::Error(OutOfRange)
private:
    std::unique_ptr<Driver> driver_;
    std::vector<std::unique_ptr<DeviceAllocator>> allocators_;
};

}  // namespace dev
