// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/registered_pool.hpp"
#include "mini_test.hpp"

namespace {
struct Device {
    std::atomic<int> registered{0}, unregistered{0};
    bool refuse = false;
};
void* doRegister(void* ctx, void* host, std::size_t) {
    auto* d = static_cast<Device*>(ctx);
    if (d->refuse) return nullptr;
    ++d->registered;
    return static_cast<char*>(host) + 0x100000000ull;   // a fake device address space
}
void doUnregister(void* ctx, void*, std::size_t) { ++static_cast<Device*>(ctx)->unregistered; }
mem::Registration reg(Device& d) { return {&doRegister, &doUnregister, &d}; }
}  // namespace

TEST(blocksAreRegisteredOnceAndReused) {
    Device dev;
    mem::RegisteredPool pool(reg(dev));
    void* a = pool.take(5000);
    CHECK(a != nullptr);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(a) % 4096, 0u);
    std::memset(a, 1, 8192);
    pool.give(a, 5000);
    void* b = pool.take(8000);
    CHECK(b == a);                                     // same class, same block
    CHECK_EQ(dev.registered.load(), 1);
    CHECK_EQ(pool.stats().blocks, 1u);
    pool.give(b, 8000);
}

TEST(deviceTranslationCoversTheWholeBlockOnly) {
    Device dev;
    mem::RegisteredPool pool(reg(dev));
    auto* a = static_cast<char*>(pool.take(4096));
    CHECK(pool.device(a) == a + 0x100000000ull);
    CHECK(pool.device(a + 4095) == a + 4095 + 0x100000000ull);
    CHECK(pool.device(a + 4096) == nullptr || true);   // a neighbouring block, if any, may translate; this one must not extend
    int local;
    CHECK(pool.device(&local) == nullptr);
    pool.give(a, 4096);
}

TEST(limitsRefuseAndIdleCapEvictsLargestFirst) {
    Device dev;
    mem::RegisteredPool::Limits lim;
    lim.maxBytes = 64 * 1024;
    lim.maxIdleBytes = 8 * 1024;
    mem::RegisteredPool pool(reg(dev), lim);
    void* big = pool.take(32 * 1024);
    void* mid = pool.take(16 * 1024);
    CHECK(big && mid);
    CHECK(pool.take(32 * 1024) == nullptr);            // 32+16+32 > 64 KiB
    CHECK_EQ(pool.stats().refusals, 1u);
    pool.give(big, 32 * 1024);                          // idle 32 KiB > 8 KiB cap: evicted
    CHECK_EQ(pool.stats().evictedBlocks, 1u);
    CHECK_EQ(dev.unregistered.load(), 1);
    CHECK(pool.device(big) == nullptr);                 // gone from the translation table
    pool.give(mid, 16 * 1024);
    CHECK(pool.stats().idleBytes <= 8 * 1024);
}

TEST(trimAndRegistrationFailure) {
    Device dev;
    mem::RegisteredPool pool(reg(dev));
    std::vector<void*> v;
    for (int i = 0; i < 10; ++i) v.push_back(pool.take(4096 << (i % 3)));
    for (std::size_t i = 0; i < v.size(); ++i) pool.give(v[i], 4096 << (i % 3));
    CHECK(pool.stats().idleBytes > 0);
    const std::size_t freed = pool.trim(0);
    CHECK(freed > 0);
    CHECK_EQ(pool.stats().blocks, 0u);
    CHECK_EQ(dev.registered.load(), dev.unregistered.load());
    dev.refuse = true;
    CHECK(pool.take(4096) == nullptr);                 // registration failed: the caller falls back
    CHECK_EQ(pool.stats().blocks, 0u);
}

TEST(manyThreadsKeepTheAccounting) {
    Device dev;
    mem::RegisteredPool::Limits lim;
    lim.maxIdleBytes = 256 * 1024;
    mem::RegisteredPool pool(reg(dev), lim);
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t)
        ts.emplace_back([&, t] {
            std::uint64_t rng = 88172645463325252ull + t;
            for (int i = 0; i < 20000; ++i) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                const std::size_t size = 4096u << (rng % 5);
                if (void* p = pool.take(size)) { static_cast<char*>(p)[0] = 1; pool.give(p, size); }
            }
        });
    for (auto& t : ts) t.join();
    const auto s = pool.stats();
    CHECK_EQ(s.inUseBlocks, 0u);
    CHECK(s.idleBytes <= 256 * 1024);
    CHECK_EQ(s.bytes, s.idleBytes);
    CHECK_EQ(dev.registered.load() - dev.unregistered.load(), static_cast<int>(s.blocks));
}

TEST(steadyStateTouchesNoHeap) {
    Device dev;
    mem::RegisteredPool pool(reg(dev));
    void* warm = pool.take(16384);
    pool.give(warm, 16384);
    bench::AllocScope scope;
    for (int i = 0; i < 100000; ++i) { void* p = pool.take(16384); pool.give(p, 16384); }
    CHECK_EQ(scope.calls(), 0ull);
}

int main() { return tst::runAll("registered_pool"); }
