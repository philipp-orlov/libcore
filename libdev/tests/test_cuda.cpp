// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The libdev rules on a real CUDA runtime. Exits 77 (ctest: skipped) when libcudart or a device is missing, so it is
// registered everywhere and only does work on a GPU machine. What it proves there:
//   * DeviceGuard switches and restores the real per-thread current device;
//   * a DeviceAllocator segment for device d is allocated on d whatever device is current (cudaMemGetInfo of d drops);
//   * a block used by a second stream is not reused before that stream's event completes;
//   * steady-state allocate/free on one stream reaches the driver only during warm-up;
//   * pinned host memory is registered once per block;
//   * with two or more devices: peer access is queried and enabled where the hardware allows.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "base/status.hpp"
#include "dev/dev.hpp"
#include "mini_test.hpp"

namespace {
std::unique_ptr<dev::Driver> g_driver;

std::size_t freeBytesOf(dev::Driver& d, int device) {
    dev::DeviceGuard g(d, device);
    std::size_t freeBytes = 0, total = 0;
    CHECK(d.memInfo(&freeBytes, &total));
    return freeBytes;
}
}  // namespace

TEST(guardOnRealDevices) {
    dev::Driver& d = *g_driver;
    const int n = d.deviceCount();
    const int start = d.currentDevice();
    {
        dev::DeviceGuard g(d, n - 1);
        CHECK_EQ(d.currentDevice(), n - 1);
    }
    CHECK_EQ(d.currentDevice(), start);
}

TEST(segmentsLandOnTheirDevice) {
    dev::Driver& d = *g_driver;
    const int n = d.deviceCount();
    const int target = n - 1, other = 0;
    d.setDevice(other);
    dev::DeviceAllocator alloc(d, target);
    dev::Stream s(d, target);
    const std::size_t before = freeBytesOf(d, target);
    auto block = alloc.allocate(std::size_t{256} << 20, s);
    CHECK(block);
    CHECK_EQ(d.currentDevice(), other);
    CHECK(freeBytesOf(d, target) + (std::size_t{200} << 20) <= before);   // the memory came from `target`
    alloc.free(block, s);
    alloc.emptyCache();
    s.synchronize();
}

TEST(crossStreamReuseWaitsForTheEvent) {
    dev::Driver& d = *g_driver;
    dev::DeviceAllocator alloc(d, 0);
    dev::Stream a(d, 0), b(d, 0);
    auto block = alloc.allocate(1 << 20, a);
    void* where = block.ptr;
    dev::Event ev(d, 0);
    ev.record(b.handle());
    alloc.recordUse(block, b, std::move(ev));
    alloc.free(block, a);
    b.synchronize();                      // the event has completed now
    CHECK_EQ(alloc.reclaim(), 1u);
    auto again = alloc.allocate(1 << 20, a);
    CHECK(again.ptr == where);
    alloc.free(again, a);
}

TEST(steadyStateDoesNotReachTheDriver) {
    dev::Driver& d = *g_driver;
    dev::DeviceAllocator alloc(d, 0);
    dev::Stream s(d, 0);
    std::vector<mem::CachingAllocator::Block> held(64);
    std::uint32_t seed = 7;
    auto round = [&] {
        for (auto& b : held) {
            seed = seed * 1664525u + 1013904223u;
            b = alloc.allocate(std::size_t{4096} + (seed >> 12) % (std::size_t{8} << 20), s);
            CHECK(b);
        }
        for (auto& b : held)
            alloc.free(b, s);
    };
    for (int i = 0; i < 20; ++i)
        round();
    const auto warm = alloc.stats();
    for (int i = 0; i < 2000; ++i)
        round();
    const auto after = alloc.stats();
    std::fprintf(stderr, "  steady: %llu segments, %llu MiB segment bytes, backing allocs %llu -> %llu\n",
                 static_cast<unsigned long long>(after.segments),
                 static_cast<unsigned long long>(after.segmentBytes >> 20),
                 static_cast<unsigned long long>(warm.backingAllocs),
                 static_cast<unsigned long long>(after.backingAllocs));
    CHECK(after.backingAllocs <= warm.backingAllocs + 4);   // a drifting mix may still settle a little
    CHECK_EQ(after.misuse, 0u);
    alloc.emptyCache();
}

TEST(pinnedHostMemory) {
    dev::Driver& d = *g_driver;
    auto pool = dev::makePinnedPool(d);
    void* a = pool->take(std::size_t{4} << 20);
    CHECK(a != nullptr);
    pool->give(a, std::size_t{4} << 20);
    void* b = pool->take(std::size_t{3} << 20);
    CHECK(b == a);
    pool->give(b, std::size_t{3} << 20);
}

TEST(peerAccessWhereAvailable) {
    dev::Driver& d = *g_driver;
    const int n = d.deviceCount();
    if (n < 2) {
        std::fprintf(stderr, "  (one device: peer access not exercised)\n");
        return;
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j) {
                const bool can = d.canAccessPeer(i, j);
                std::fprintf(stderr, "  peer %d -> %d: %s\n", i, j, can ? "yes" : "no");
                if (can) {
                    dev::DeviceGuard g(d, i);
                    CHECK(d.enablePeerAccess(j));
                }
            }
}

int main() {
    std::string why;
    g_driver = dev::makeCudaDriver(&why);
    if (!g_driver) {
        std::fprintf(stderr, "dev_cuda: skipped (%s)\n", why.c_str());
        return 77;
    }
    std::fprintf(stderr, "dev_cuda: %d device(s) via %s\n", g_driver->deviceCount(), g_driver->name());
    const int rc = tst::runAll("dev_cuda");
    g_driver.reset();
    return rc;
}
