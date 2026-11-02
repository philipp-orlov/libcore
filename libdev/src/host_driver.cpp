// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "dev/host_driver.hpp"

namespace dev {

namespace {

thread_local int t_device = 0;

class Host final : public HostDriver {
public:
    Host(int devices, std::size_t perDevice) : devices_(devices), cap_(perDevice), used_(static_cast<std::size_t>(devices), 0) {}
    ~Host() override {
        for (auto& [p, info] : allocations_)
            std::free(p);
    }
    const char* name() const noexcept override { return "host"; }
    int deviceCount() override { return devices_; }
    bool setDevice(int device) override {
        if (device < 0 || device >= devices_)
            return false;
        t_device = device;
        return true;
    }
    int currentDevice() override { return t_device; }
    void* malloc(std::size_t bytes) override {
        std::lock_guard<std::mutex> g(mu_);
        if (cap_ && used_[t_device] + bytes > cap_)
            return nullptr;
        void* p = std::aligned_alloc(256, (bytes + 255) & ~std::size_t{255});
        if (!p)
            return nullptr;
        allocations_[p] = {t_device, bytes};
        used_[t_device] += bytes;
        return p;
    }
    void free(void* p) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = allocations_.find(p);
        if (it == allocations_.end() || it->second.device != t_device) {
            ++misuse_;   // unknown pointer, or freed while another device is current
            if (it == allocations_.end())
                return;
        }
        used_[it->second.device] -= it->second.bytes;
        allocations_.erase(it);
        std::free(p);
    }
    bool memInfo(std::size_t* freeBytes, std::size_t* totalBytes) override {
        std::lock_guard<std::mutex> g(mu_);
        const std::size_t total = cap_ ? cap_ : (std::size_t{1} << 40);
        if (totalBytes) *totalBytes = total;
        if (freeBytes) *freeBytes = total - used_[t_device];
        return true;
    }
    bool hostRegister(void* p, std::size_t) override {
        std::lock_guard<std::mutex> g(mu_);
        return registered_.insert(p).second;
    }
    bool hostUnregister(void* p) override {
        std::lock_guard<std::mutex> g(mu_);
        if (registered_.erase(p) == 0) { ++misuse_; return false; }
        return true;
    }
    StreamHandle streamCreate() override {
        std::lock_guard<std::mutex> g(mu_);
        auto* s = new Stream{t_device, 0};
        streams_.insert(s);
        return s;
    }
    void streamDestroy(StreamHandle h) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* s = static_cast<Stream*>(h);
        if (!streams_.erase(s)) { ++misuse_; return; }
        delete s;
    }
    bool streamSynchronize(StreamHandle h) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* s = static_cast<Stream*>(h);
        if (!streams_.count(s)) { ++misuse_; return false; }
        s->completed = s->recorded;
        return true;
    }
    EventHandle eventCreate() override {
        std::lock_guard<std::mutex> g(mu_);
        auto* e = new Event{t_device, nullptr, 0};
        events_.insert(e);
        return e;
    }
    bool eventRecord(EventHandle eh, StreamHandle sh) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* e = static_cast<Event*>(eh);
        auto* s = static_cast<Stream*>(sh);
        if (!events_.count(e) || !streams_.count(s) || e->device != s->device) { ++misuse_; return false; }
        e->stream = s;
        e->ticket = ++s->recorded;
        return true;
    }
    bool eventQuery(EventHandle eh) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* e = static_cast<Event*>(eh);
        if (!events_.count(e)) { ++misuse_; return false; }
        return e->stream == nullptr || e->ticket <= e->stream->completed;
    }
    void eventDestroy(EventHandle eh) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* e = static_cast<Event*>(eh);
        if (!events_.erase(e)) { ++misuse_; return; }
        delete e;
    }
    bool canAccessPeer(int device, int peer) override { return device != peer && device >= 0 && peer >= 0 && device < devices_ && peer < devices_; }
    bool enablePeerAccess(int peer) override { return canAccessPeer(t_device, peer); }

    void complete(StreamHandle sh) override {
        std::lock_guard<std::mutex> g(mu_);
        auto* s = static_cast<Stream*>(sh);
        if (streams_.count(s)) s->completed = s->recorded;
    }
    std::size_t usedBytes(int device) override { std::lock_guard<std::mutex> g(mu_); return used_[device]; }
    std::size_t liveAllocations() override { std::lock_guard<std::mutex> g(mu_); return allocations_.size(); }
    std::uint64_t misuse() override { std::lock_guard<std::mutex> g(mu_); return misuse_; }

private:
    struct Stream { int device; std::uint64_t recorded; std::uint64_t completed = 0; };
    struct Event { int device; Stream* stream = nullptr; std::uint64_t ticket = 0; };
    int devices_;
    std::size_t cap_;
    std::mutex mu_;
    std::vector<std::size_t> used_;
    struct Info { int device; std::size_t bytes; };
    std::map<void*, Info> allocations_;
    std::set<void*> registered_;
    std::set<Stream*> streams_;
    std::set<Event*> events_;
    std::uint64_t misuse_ = 0;
};

}  // namespace

std::unique_ptr<HostDriver> makeHostDriverTyped(int devices, std::size_t bytesPerDevice) {
    return std::make_unique<Host>(devices, bytesPerDevice);
}
std::unique_ptr<Driver> makeHostDriver(int devices, std::size_t bytesPerDevice) { return makeHostDriverTyped(devices, bytesPerDevice); }

}  // namespace dev
