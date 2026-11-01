// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <thread>

#include "mini_test.hpp"
#include "sys/cpu.hpp"

TEST(topologyIsConsistent) {
    const sys::CpuTopology& t = sys::cpuTopology();
    CHECK(t.logical >= 1);
    CHECK(t.physical >= 1 && t.physical <= t.logical);
    CHECK(t.performance >= 1 && t.performance <= t.physical);
    CHECK(t.numaNodes >= 1);
    CHECK(!t.cpuToNode.empty());
    CHECK(t.logical <= std::thread::hardware_concurrency() + 1000);
    std::fprintf(stderr, "  logical=%u physical=%u performance=%u numa=%u l2=%zu\n", t.logical, t.physical, t.performance,
                 t.numaNodes, sys::l2CacheBytes());
}

TEST(pinningWorksOnLinux) {
#if defined(__linux__)
    std::thread th([] { CHECK(sys::pinThisThread(0)); });
    th.join();
#endif
}

TEST(featuresAreStable) {
    const sys::CpuFeatures& a = sys::cpuFeatures();
    const sys::CpuFeatures& b = sys::cpuFeatures();
    CHECK(&a == &b);
}

int main() { return tst::runAll("cpu"); }
