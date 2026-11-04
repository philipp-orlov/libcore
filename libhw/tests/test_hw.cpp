// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "hw/tegrastats_provider.hpp"
#include <thread>

#include "bench/alloc_budget.hpp"
#include "hw/network_sampler.hpp"
#include "hw/probe.hpp"
#include "hw/process_sampler.hpp"
#include "hw/procfs.hpp"
#include "mini_test.hpp"

TEST(procParsersHandleTrickyInput) {
    hw::ProcPidStat st;
    CHECK(hw::parseProcPidStat("42 (my (weird) name) S 1 2 3 4 5 6 7 8 9 10 111 222 13 14", st));
    CHECK_EQ(st.pid, 42);
    CHECK_EQ(st.name, std::string("my (weird) name"));
    CHECK_EQ(st.utimeTicks, 111u);
    CHECK_EQ(st.stimeTicks, 222u);
    CHECK(!hw::parseProcPidStat("42 (x) S 1 2", st));
    CHECK(!hw::parseProcPidStat("garbage", st));
    CHECK_EQ(hw::parseVmRssKb("Name:\tx\nVmRSS:\t   2048 kB\nThreads: 1\n"), 2048.0);
    CHECK_EQ(hw::parseVmRssKb("Name:\tx\n"), 0.0);
    CHECK_EQ(hw::cpuPercentFromTicks(100, 200, 1.0, 100), 100.0);
    CHECK_EQ(hw::cpuPercentFromTicks(200, 100, 1.0, 100), 0.0);   // pid reuse
}

TEST(netDevParsing) {
    const std::string text =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
        "  eth0: 5000 50 0 0 0 0 0 0 7000 70 0 0 0 0 0 0\n";
    const auto by = hw::parseProcNetDev(text);
    CHECK_EQ(by.size(), 2u);
    const auto total = hw::sumNonLoopback(by);
    CHECK_EQ(total.rxBytes, 5000u);
    CHECK_EQ(total.txBytes, 7000u);
}

TEST(realProcSamplingWorksAndSeesThisProcess) {
    hw::ProcessSampler s;
    std::vector<hw::ProcessSample> out;
    s.sample(5, out);
    // Burn a little CPU so this process shows up with a rate.
    volatile double x = 0;
    for (int i = 0; i < 20000000; ++i) x += i;
    s.sample(100000, out);
    bool found = false;
    for (const auto& p : out)
        if (p.pid == ::getpid()) { found = true; CHECK(p.memoryMb > 0); }
    CHECK(found);
    for (std::size_t i = 1; i < out.size(); ++i) CHECK(out[i - 1].cpuPercent >= out[i].cpuPercent);
    hw::NetworkSampler n;
    (void)n.sample();
    (void)n.sample();
}

TEST(samplingAllocatesNothingAfterTheFirstTick) {
    hw::ProcessSampler procs;
    hw::NetworkSampler net;
    std::vector<hw::ProcessSample> out;
    for (int i = 0; i < 3; ++i) { procs.sample(10, out); (void)net.sample(); }   // warm up tables and vector
    bench::AllocScope scope;
    for (int i = 0; i < 300; ++i) { procs.sample(10, out); (void)net.sample(); }
    // The only allocations left are a process name longer than the string's 15-byte inline buffer (the kernel reports
    // up to 23 characters for kworker threads) landing in an output slot that has not held one before: a handful at
    // most, never one per tick or per process.
    CHECK(scope.calls() <= 4);
}

TEST(probeReportsThisMachine) {
    const hw::Probe p = hw::probe();
    CHECK(p.logicalCpus >= 1);
    CHECK(p.memTotalMb > 100);
    CHECK(p.memAvailableMb > 0 && p.memAvailableMb <= p.memTotalMb);
    CHECK_EQ(p.unifiedMemory, p.tegra);
    CHECK(hw::defaultCacheBudgetMb(p) >= 0);
    hw::Probe jetson;
    jetson.unifiedMemory = true;
    jetson.memAvailableMb = 4000;
    CHECK_EQ(hw::defaultCacheBudgetMb(jetson, 0.25), 1000.0);
}

TEST(tegrastatsIsStartedAgainWhenItEnds) {
    std::atomic<int> streams{0};
    std::atomic<int> samples{0};
    const std::string line = "RAM 4461/15524MB CPU [0%@1984] GR3D_FREQ 0% GPU@40C";
    hw::TegrastatsProvider provider(1000, [&]() -> std::unique_ptr<hw::LineStream> {
        ++streams;
        return std::make_unique<hw::VectorLineStream>(std::vector<std::string>{line});
    });
    CHECK(provider.start([&](const hw::Sample&) { ++samples; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // the first restart waits 1 s
    provider.stop();
    CHECK(streams.load() >= 2);
    CHECK(samples.load() >= 2);
}

int main() { return tst::runAll("hw"); }
