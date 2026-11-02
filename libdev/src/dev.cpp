// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "dev/dev.hpp"

#include <algorithm>
#include <atomic>

#include "base/status.hpp"

namespace dev {

namespace {
std::atomic<std::uint32_t> g_nextStreamId{1};
}

DeviceGuard::DeviceGuard(Driver& driver, int device) : driver_(driver), previous_(driver.currentDevice()), switched_(false) {
    if (previous_ != device) {
        if (!driver_.setDevice(device))
            throw base::Error(base::Code::InvalidArgument, "dev: no such device");
        switched_ = true;
    }
}
DeviceGuard::~DeviceGuard() {
    if (switched_)
        driver_.setDevice(previous_);
}

Event::Event(Driver& driver, int device) : driver_(&driver), device_(device) {
    DeviceGuard guard(driver, device);
    handle_ = driver.eventCreate();
    if (!handle_)
        throw base::Error(base::Code::ResourceExhausted, "dev: cannot create an event");
}
Event::~Event() {
    if (handle_) {
        DeviceGuard guard(*driver_, device_);
        driver_->eventDestroy(handle_);
    }
}
Event::Event(Event&& o) noexcept : driver_(o.driver_), device_(o.device_), handle_(o.handle_) { o.handle_ = nullptr; }
Event& Event::operator=(Event&& o) noexcept {
    if (this != &o) {
        if (handle_) {
            DeviceGuard guard(*driver_, device_);
            driver_->eventDestroy(handle_);
        }
        driver_ = o.driver_;
        device_ = o.device_;
        handle_ = o.handle_;
        o.handle_ = nullptr;
    }
    return *this;
}
void Event::record(StreamHandle stream) {
    DeviceGuard guard(*driver_, device_);
    if (!driver_->eventRecord(handle_, stream))
        throw base::Error(base::Code::Internal, "dev: eventRecord failed");
}
bool Event::done() const {
    DeviceGuard guard(*driver_, device_);
    return driver_->eventQuery(handle_);
}

Stream::Stream(Driver& driver, int device) : driver_(&driver), device_(device), id_(g_nextStreamId.fetch_add(1)) {
    DeviceGuard guard(driver, device);
    handle_ = driver.streamCreate();
    if (!handle_)
        throw base::Error(base::Code::ResourceExhausted, "dev: cannot create a stream");
}
Stream::~Stream() {
    if (handle_) {
        DeviceGuard guard(*driver_, device_);
        driver_->streamDestroy(handle_);
    }
}
Stream::Stream(Stream&& o) noexcept : driver_(o.driver_), device_(o.device_), handle_(o.handle_), id_(o.id_) { o.handle_ = nullptr; }
void Stream::synchronize() {
    DeviceGuard guard(*driver_, device_);
    driver_->streamSynchronize(handle_);
}

// ---- DeviceAllocator ----

mem::Segment DeviceAllocator::allocSegment(void* ctx, std::size_t bytes) {
    auto* self = static_cast<DeviceAllocator*>(ctx);
    DeviceGuard guard(self->driver_, self->device_);   // the caller may have another device current
    void* p = self->driver_.malloc(bytes);
    return {p, p ? bytes : 0};
}
void DeviceAllocator::freeSegment(void* ctx, mem::Segment s) {
    auto* self = static_cast<DeviceAllocator*>(ctx);
    DeviceGuard guard(self->driver_, self->device_);
    self->driver_.free(s.base);
}

DeviceAllocator::DeviceAllocator(Driver& driver, int device, Options options) : driver_(driver), device_(device) {
    mem::CachingOptions co;
    co.maxCachedBytes = options.maxCachedBytes;
    co.budgetBytes = options.budgetBytes;
    co.maxStreams = 32;
    co.maxBlocks = 1 << 16;
    cache_ = std::make_unique<mem::CachingAllocator>(mem::BackingOps{&allocSegment, &freeSegment, this}, co);
}
DeviceAllocator::~DeviceAllocator() = default;

mem::CachingAllocator::Block DeviceAllocator::allocate(std::size_t bytes, const Stream& stream) {
    if (stream.device() != device_)
        throw base::Error(base::Code::InvalidArgument, "dev: stream belongs to another device");
    std::lock_guard<std::mutex> g(mutex_);
    return cache_->allocate(bytes, stream.id());
}
void DeviceAllocator::free(mem::CachingAllocator::Block block, const Stream& lastUse) {
    (void)lastUse;   // the cache returns the block to the pool of the stream that allocated it, deferred if recordUse() was called
    std::lock_guard<std::mutex> g(mutex_);
    cache_->free(block);
}
void DeviceAllocator::recordUse(mem::CachingAllocator::Block block, const Stream& other, Event event) {
    std::lock_guard<std::mutex> g(mutex_);
    cache_->recordUse(block, other.id());
    pending_.push_back({other.id(), std::move(event)});
}

namespace {
struct ReclaimCtx {
    std::vector<std::pair<std::uint32_t, bool>> streams;   // stream id -> all of its pending events done
};
bool streamDone(mem::CachingAllocator::StreamId id, void* ctx) {
    for (auto& [stream, done] : static_cast<ReclaimCtx*>(ctx)->streams)
        if (stream == id)
            return done;
    return true;
}
}  // namespace

std::size_t DeviceAllocator::reclaim() {
    std::lock_guard<std::mutex> g(mutex_);
    ReclaimCtx ctx;
    for (const Pending& p : pending_) {
        const bool done = p.event.done();
        auto it = std::find_if(ctx.streams.begin(), ctx.streams.end(), [&](auto& s) { return s.first == p.stream; });
        if (it == ctx.streams.end())
            ctx.streams.push_back({p.stream, done});
        else
            it->second = it->second && done;
    }
    const std::size_t freed = cache_->reclaimDeferred(&streamDone, &ctx);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.event.done(); }), pending_.end());
    return freed;
}
std::size_t DeviceAllocator::emptyCache() {
    std::lock_guard<std::mutex> g(mutex_);
    return cache_->emptyCache();
}
mem::CachingAllocator::CacheStats DeviceAllocator::stats() {
    std::lock_guard<std::mutex> g(mutex_);
    return cache_->stats();
}

// ---- pinned registration, system ----

std::unique_ptr<mem::RegisteredPool> makePinnedPool(Driver& driver, mem::RegisteredPool::Limits limits) {
    mem::Registration reg;
    reg.ctx = &driver;
    reg.registerMemory = [](void* ctx, void* host, std::size_t bytes) -> void* {
        return static_cast<Driver*>(ctx)->hostRegister(host, bytes) ? host : nullptr;   // mapped host memory: device address == host address
    };
    reg.unregisterMemory = [](void* ctx, void* host, std::size_t) { static_cast<Driver*>(ctx)->hostUnregister(host); };
    return std::make_unique<mem::RegisteredPool>(reg, limits);
}

System::System(std::unique_ptr<Driver> driver) : driver_(std::move(driver)) {
    const int n = driver_->deviceCount();
    for (int d = 0; d < n; ++d)
        allocators_.push_back(std::make_unique<DeviceAllocator>(*driver_, d));
}

DeviceAllocator& System::allocator(int device) {
    if (device < 0 || device >= deviceCount())
        throw base::Error(base::Code::OutOfRange, "dev: no such device");
    return *allocators_[static_cast<std::size_t>(device)];
}

}  // namespace dev
