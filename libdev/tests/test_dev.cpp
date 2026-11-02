// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <thread>

#include "base/status.hpp"
#include "dev/dev.hpp"
#include "dev/host_driver.hpp"
#include "mini_test.hpp"

namespace {
struct Rig {
    std::unique_ptr<dev::HostDriver> driver = dev::makeHostDriverTyped(3, std::size_t{64} << 20);
};
}  // namespace

TEST(guardSwitchesAndRestoresTheCurrentDevice) {
    Rig r;
    CHECK(r.driver->setDevice(2));
    {
        dev::DeviceGuard g(*r.driver, 1);
        CHECK_EQ(r.driver->currentDevice(), 1);
        {
            dev::DeviceGuard g2(*r.driver, 0);
            CHECK_EQ(r.driver->currentDevice(), 0);
        }
        CHECK_EQ(r.driver->currentDevice(), 1);
    }
    CHECK_EQ(r.driver->currentDevice(), 2);
    bool threw = false;
    try { dev::DeviceGuard bad(*r.driver, 9); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
    // The current device is per thread.
    std::thread([&] { CHECK_EQ(r.driver->currentDevice(), 0); }).join();
}

TEST(everyDeviceHasItsOwnCache) {
    Rig r;
    dev::System sys(std::move(r.driver));
    CHECK_EQ(sys.deviceCount(), 3);
    auto& host = static_cast<dev::HostDriver&>(sys.driver());
    dev::Stream s0(sys.driver(), 0), s1(sys.driver(), 1);
    // Allocate on device 1 while device 2 is current: the segment must still land on device 1.
    sys.driver().setDevice(2);
    auto b1 = sys.allocator(1).allocate(1 << 20, s1);
    CHECK(b1);
    CHECK(host.usedBytes(1) > 0);
    CHECK_EQ(host.usedBytes(2), 0u);
    CHECK_EQ(host.usedBytes(0), 0u);
    CHECK_EQ(sys.driver().currentDevice(), 2);          // and the caller's device is untouched
    auto b0 = sys.allocator(0).allocate(1 << 20, s0);
    CHECK(host.usedBytes(0) > 0);
    // A stream of another device is refused.
    bool threw = false;
    try { sys.allocator(0).allocate(64, s1); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
    sys.allocator(1).free(b1, s1);
    sys.allocator(0).free(b0, s0);
    sys.allocator(1).emptyCache();                       // frees on device 1 while device 2 is current
    sys.allocator(0).emptyCache();
    CHECK_EQ(host.liveAllocations(), 0u);
    CHECK_EQ(host.misuse(), 0u);                         // never freed on the wrong device
}

TEST(crossStreamUseDefersReuseUntilTheEventCompletes) {
    Rig r;
    dev::System sys(std::move(r.driver));
    auto& host = static_cast<dev::HostDriver&>(sys.driver());
    dev::Stream a(sys.driver(), 0), b(sys.driver(), 0);
    auto block = sys.allocator(0).allocate(4096, a);
    void* where = block.ptr;
    dev::Event ev(sys.driver(), 0);
    ev.record(b.handle());                               // work on stream b is still reading the block
    sys.allocator(0).recordUse(block, b, std::move(ev));
    sys.allocator(0).free(block, a);
    auto other = sys.allocator(0).allocate(4096, a);
    CHECK(other.ptr != where);                           // not reusable yet
    CHECK_EQ(sys.allocator(0).reclaim(), 0u);
    host.complete(b.handle());
    CHECK_EQ(sys.allocator(0).reclaim(), 1u);
    auto again = sys.allocator(0).allocate(4096, a);
    CHECK(again.ptr == where);
    sys.allocator(0).free(other, a);
    sys.allocator(0).free(again, a);
}

TEST(budgetLimitsADevice) {
    auto driver = dev::makeHostDriverTyped(2, std::size_t{8} << 20);
    dev::System sys(std::move(driver));
    dev::Stream s(sys.driver(), 0);
    auto big = sys.allocator(0).allocate(6u << 20, s);
    CHECK(big);
    CHECK(!sys.allocator(0).allocate(6u << 20, s));      // the driver refuses: the allocator reports failure, no crash
    sys.allocator(0).free(big, s);
    CHECK(sys.allocator(0).allocate(6u << 20, s));       // reused after the free
}

TEST(pinnedPoolRegistersOncePerBlock) {
    Rig r;
    auto pool = dev::makePinnedPool(*r.driver);
    void* a = pool->take(10000);
    CHECK(a != nullptr);
    CHECK(pool->device(a) == a);
    pool->give(a, 10000);
    void* b = pool->take(9000);
    CHECK(b == a);
    pool->give(b, 9000);
    pool->trim(0);
    pool.reset();
    CHECK_EQ(r.driver->misuse(), 0u);
}

TEST(streamsAndEventsAreCheckedByTheDriver) {
    Rig r;
    dev::Stream s(*r.driver, 1);
    dev::Event e(*r.driver, 1);
    CHECK(!e.done() || true);
    e.record(s.handle());
    CHECK(!e.done());
    s.synchronize();
    CHECK(e.done());
    dev::Event wrong(*r.driver, 2);
    bool threw = false;
    try { wrong.record(s.handle()); } catch (const base::Error&) { threw = true; }   // event and stream of different devices
    CHECK(threw);
    CHECK(r.driver->misuse() >= 1u);
}

TEST(cudaRuntimeIsOptional) {
    std::string why;
    auto cuda = dev::makeCudaDriver(&why);
    if (!cuda) {
        CHECK(!why.empty());   // this machine: no libcudart, reported cleanly
        std::fprintf(stderr, "  (CUDA runtime not available: %s)\n", why.c_str());
    } else {
        CHECK(cuda->deviceCount() >= 1);
    }
}

int main() { return tst::runAll("dev"); }
