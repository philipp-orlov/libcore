// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/cpu.hpp"

#include <thread>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/auxv.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#endif

namespace sys {

#if defined(__linux__)
namespace {

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return false;
    char buf[512];
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return true;
}

// "0-3,5,7-8" -> ids
std::vector<unsigned> parseCpuList(const std::string& text) {
    std::vector<unsigned> ids;
    std::size_t i = 0;
    while (i < text.size()) {
        char* end = nullptr;
        const unsigned long a = std::strtoul(text.c_str() + i, &end, 10);
        if (end == text.c_str() + i)
            break;
        unsigned long b = a;
        i = static_cast<std::size_t>(end - text.c_str());
        if (i < text.size() && text[i] == '-') {
            b = std::strtoul(text.c_str() + i + 1, &end, 10);
            i = static_cast<std::size_t>(end - text.c_str());
        }
        for (unsigned long v = a; v <= b && v < 4096; ++v)
            ids.push_back(static_cast<unsigned>(v));
        if (i < text.size() && text[i] == ',')
            ++i;
    }
    return ids;
}

CpuTopology detectTopology() {
    CpuTopology t;
    std::string text;
    std::vector<unsigned> online;
    if (readFile("/sys/devices/system/cpu/online", text))
        online = parseCpuList(text);
    if (online.empty()) {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; ++i)
            online.push_back(i);
    }
    t.logical = static_cast<unsigned>(online.size());

    std::set<std::pair<long, long>> cores;  // (package, core)
    unsigned long topCapacity = 0;
    std::vector<unsigned long> capacities(online.size(), 0);
    for (std::size_t k = 0; k < online.size(); ++k) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(online[k]);
        long pkg = 0, core = static_cast<long>(online[k]);
        if (readFile(base + "/topology/physical_package_id", text))
            pkg = std::atol(text.c_str());
        if (readFile(base + "/topology/core_id", text))
            core = std::atol(text.c_str());
        cores.insert({pkg, core});
        if (readFile(base + "/cpu_capacity", text)) {
            capacities[k] = std::strtoul(text.c_str(), nullptr, 10);
            topCapacity = std::max(topCapacity, capacities[k]);
        }
    }
    t.physical = static_cast<unsigned>(cores.empty() ? online.size() : cores.size());
    if (topCapacity == 0) {
        t.performance = t.physical;
    } else {
        // Cores within ~5 % of the best capacity are performance cores (little cores are usually below 80 %).
        std::set<std::pair<long, long>> perf;
        for (std::size_t k = 0; k < online.size(); ++k) {
            if (capacities[k] * 100 >= topCapacity * 95) {
                long pkg = 0, core = static_cast<long>(online[k]);
                const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(online[k]);
                if (readFile(base + "/topology/physical_package_id", text)) pkg = std::atol(text.c_str());
                if (readFile(base + "/topology/core_id", text)) core = std::atol(text.c_str());
                perf.insert({pkg, core});
            }
        }
        t.performance = static_cast<unsigned>(std::max<std::size_t>(1, perf.size()));
    }

    // NUMA
    unsigned maxCpu = 0;
    for (unsigned c : online)
        maxCpu = std::max(maxCpu, c);
    t.cpuToNode.assign(maxCpu + 1, 0);
    unsigned nodes = 0;
    if (DIR* d = ::opendir("/sys/devices/system/node")) {
        while (const dirent* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "node", 4) != 0 || e->d_name[4] < '0' || e->d_name[4] > '9')
                continue;
            const unsigned node = static_cast<unsigned>(std::atoi(e->d_name + 4));
            if (readFile(std::string("/sys/devices/system/node/") + e->d_name + "/cpulist", text))
                for (unsigned c : parseCpuList(text))
                    if (c <= maxCpu)
                        t.cpuToNode[c] = node;
            ++nodes;
        }
        ::closedir(d);
    }
    t.numaNodes = std::max(1u, nodes);
    return t;
}

}  // namespace
#endif

const CpuTopology& cpuTopology() {
#if defined(__linux__)
    static const CpuTopology topology = detectTopology();
#else
    static const CpuTopology topology = [] {
        CpuTopology t;
        t.logical = t.physical = t.performance = std::max(1u, std::thread::hardware_concurrency());
        t.cpuToNode.assign(t.logical, 0);
        return t;
    }();
#endif
    return topology;
}

bool pinThisThread(unsigned cpu) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpu >= CPU_SETSIZE)
        return false;
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

std::size_t l2CacheBytes() noexcept {
#if defined(__linux__)
    std::string text;
    for (int index = 0; index < 8; ++index) {
        const std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(index);
        if (!readFile(base + "/level", text) || std::atoi(text.c_str()) != 2)
            continue;
        if (!readFile(base + "/size", text))
            return 0;
        char* end = nullptr;
        const unsigned long v = std::strtoul(text.c_str(), &end, 10);
        if (end && (*end == 'K' || *end == 'k'))
            return v * 1024;
        if (end && *end == 'M')
            return v * 1024 * 1024;
        return v;
    }
#endif
    return 0;
}

const CpuFeatures& cpuFeatures() noexcept {
    static const CpuFeatures features = [] {
        CpuFeatures f;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
        __builtin_cpu_init();
        f.sse42 = __builtin_cpu_supports("sse4.2");
        f.avx2 = __builtin_cpu_supports("avx2");
        f.avx512f = __builtin_cpu_supports("avx512f");
#elif defined(__aarch64__) && defined(__linux__)
        const unsigned long hwcap = ::getauxval(AT_HWCAP);
        f.armCrc32 = (hwcap & (1ul << 7)) != 0;    // HWCAP_CRC32
        f.armLse = (hwcap & (1ul << 8)) != 0;      // HWCAP_ATOMICS
        f.armFp16 = (hwcap & (1ul << 9)) != 0;     // HWCAP_FPHP
        f.armDotProd = (hwcap & (1ul << 20)) != 0; // HWCAP_ASIMDDP
#endif
        return f;
    }();
    return features;
}

}  // namespace sys
