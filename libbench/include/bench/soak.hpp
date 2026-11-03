// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Soak runner: runs a workload on N threads for a fixed time, samples the process RSS (and any extra gauges) once
// per interval, and fits a straight line through the samples taken after the warm-up. A process that "runs for
// months" must show a slope of about zero; this turns that into a number a test can assert on.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bench {

struct SoakOptions {
    std::chrono::seconds duration{10};
    std::chrono::milliseconds sampleEvery{250};
    double warmupFraction = 0.4;           // samples in the first 40 % are ignored for the slope
    unsigned threads = 4;
    // Extra gauges sampled alongside RSS (name, function returning bytes or a count).
    std::vector<std::pair<std::string, std::function<std::uint64_t()>>> gauges;
};

struct SoakSeries {
    std::string name;
    std::vector<double> samples;           // one per interval
    double slopePerMinute = 0;             // least squares over the post-warm-up samples, in the gauge's unit per minute
    double first = 0, last = 0, peak = 0;  // post-warm-up
    // The post-warm-up samples are cut into three equal windows. A gauge that merely swings (a cache following a
    // drifting working set) has the same maximum in each window; one that leaks has a larger maximum in each.
    double windowMax[3] = {0, 0, 0};
    double windowGrowthPercent = 0;        // (windowMax[2] - windowMax[0]) / windowMax[0] * 100
};

struct SoakResult {
    std::uint64_t iterations = 0;
    double seconds = 0;
    std::vector<SoakSeries> series;        // series[0] is RSS in bytes
    const SoakSeries* find(const std::string& name) const;
    std::string report() const;
};

// `work(threadIndex, stop)` should loop until `stop` becomes true, returning the number of iterations it performed.
SoakResult runSoak(const SoakOptions& options, const std::function<std::uint64_t(unsigned thread, const std::atomic<bool>& stop)>& work);

// Environment override so CI can lengthen the same test: CORE_SOAK_SECONDS.
std::chrono::seconds soakSeconds(std::chrono::seconds fallback);

}  // namespace bench
