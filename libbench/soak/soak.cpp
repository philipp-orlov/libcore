// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "bench/soak.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

#include "sys/proc.hpp"

namespace bench {

const SoakSeries* SoakResult::find(const std::string& name) const {
    for (const SoakSeries& s : series)
        if (s.name == name)
            return &s;
    return nullptr;
}

std::string SoakResult::report() const {
    std::ostringstream out;
    char line[256];
    std::snprintf(line, sizeof line, "soak: %.1f s, %llu iterations (%.2f M/s)\n", seconds, (unsigned long long)iterations,
                  iterations / seconds / 1e6);
    out << line;
    for (const SoakSeries& s : series) {
        std::snprintf(line, sizeof line, "  %-22s first %.0f  last %.0f  peak %.0f  slope %+.1f/min  window maxima %.0f %.0f %.0f (%+.1f%%)\n",
                      s.name.c_str(), s.first, s.last, s.peak, s.slopePerMinute, s.windowMax[0], s.windowMax[1], s.windowMax[2],
                      s.windowGrowthPercent);
        out << line;
    }
    return out.str();
}

std::chrono::seconds soakSeconds(std::chrono::seconds fallback) {
    if (const char* v = std::getenv("CORE_SOAK_SECONDS")) {
        const long n = std::strtol(v, nullptr, 10);
        if (n > 0)
            return std::chrono::seconds(n);
    }
    return fallback;
}

static void fit(SoakSeries& s, double warmup, double intervalSeconds) {
    const std::size_t n = s.samples.size();
    const std::size_t from = static_cast<std::size_t>(n * warmup);
    if (n - from < 3)
        return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double m = static_cast<double>(n - from);
    s.peak = 0;
    for (std::size_t i = from; i < n; ++i) {
        const double x = static_cast<double>(i - from) * intervalSeconds / 60.0;   // minutes
        const double y = s.samples[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
        s.peak = std::max(s.peak, y);
    }
    const double denom = m * sxx - sx * sx;
    s.slopePerMinute = denom != 0 ? (m * sxy - sx * sy) / denom : 0;
    s.first = s.samples[from];
    s.last = s.samples[n - 1];
    const std::size_t span = n - from;
    for (std::size_t i = from; i < n; ++i) {
        const std::size_t w = std::min<std::size_t>(2, (i - from) * 3 / span);
        s.windowMax[w] = std::max(s.windowMax[w], s.samples[i]);
    }
    const double before = std::max(s.windowMax[0], s.windowMax[1]);
    s.windowGrowthPercent = before > 0 ? (s.windowMax[2] - before) / before * 100.0 : 0;
}

SoakResult runSoak(const SoakOptions& o, const std::function<std::uint64_t(unsigned, const std::atomic<bool>&)>& work) {
    SoakResult result;
    result.series.push_back({"rss_bytes", {}});
    for (const auto& g : o.gauges)
        result.series.push_back({g.first, {}});
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> iterations{0};
    std::vector<std::thread> threads;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < o.threads; ++t)
        threads.emplace_back([&, t] { iterations += work(t, stop); });
    while (std::chrono::steady_clock::now() - start < o.duration) {
        std::this_thread::sleep_for(o.sampleEvery);
        result.series[0].samples.push_back(static_cast<double>(sys::residentBytes()));
        for (std::size_t i = 0; i < o.gauges.size(); ++i)
            result.series[i + 1].samples.push_back(static_cast<double>(o.gauges[i].second()));
    }
    stop.store(true);
    for (auto& t : threads)
        t.join();
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.iterations = iterations.load();
    const double interval = std::chrono::duration<double>(o.sampleEvery).count();
    for (SoakSeries& s : result.series)
        fit(s, o.warmupFraction, interval);
    return result;
}

}  // namespace bench
