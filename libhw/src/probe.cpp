// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "hw/probe.hpp"

#include <dirent.h>

#include <cstring>

#include "hw/procfs.hpp"
#include "sys/cpu.hpp"

namespace hw {

namespace {
bool exists(const char* path) { return ::access(path, F_OK) == 0; }

double meminfoMb(std::string_view text, std::string_view key) {
    Scanner lines(text);
    while (!lines.atEnd()) {
        const std::string_view line = lines.line();
        if (line.rfind(key, 0) == 0) {
            Scanner f(line.substr(key.size()));
            return static_cast<double>(toUnsigned(f.token())) / 1024.0;
        }
    }
    return 0;
}
}  // namespace

Probe probe() {
    Probe p;
    const sys::CpuTopology& t = sys::cpuTopology();
    p.logicalCpus = t.logical;
    p.physicalCpus = t.physical;
    p.performanceCpus = t.performance;
    p.numaNodes = t.numaNodes;

    char buffer[8192];
    if (const long n = readTextFile("/proc/meminfo", buffer, sizeof buffer); n > 0) {
        const std::string_view text(buffer, static_cast<std::size_t>(n));
        p.memTotalMb = meminfoMb(text, "MemTotal:");
        p.memAvailableMb = meminfoMb(text, "MemAvailable:");
    }
    if (const long n = readTextFile("/proc/device-tree/model", buffer, sizeof buffer); n > 0)
        p.model.assign(buffer, ::strnlen(buffer, static_cast<std::size_t>(n)));
    else if (const long m = readTextFile("/sys/devices/virtual/dmi/id/product_name", buffer, sizeof buffer); m > 0) {
        p.model.assign(buffer, static_cast<std::size_t>(m));
        while (!p.model.empty() && (p.model.back() == '\n' || p.model.back() == ' '))
            p.model.pop_back();
    }
    p.tegra = exists("/etc/nv_tegra_release") || p.model.find("NVIDIA Jetson") != std::string::npos ||
              p.model.find("Tegra") != std::string::npos;
    p.unifiedMemory = p.tegra;

    if (DIR* d = ::opendir("/proc/driver/nvidia/gpus")) {
        while (const dirent* e = ::readdir(d))
            if (e->d_name[0] != '.')
                ++p.nvidiaGpus;
        ::closedir(d);
    }
    if (DIR* d = ::opendir("/sys/class/thermal")) {
        while (const dirent* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "thermal_zone", 12) != 0)
                continue;
            char path[300];
            std::snprintf(path, sizeof path, "/sys/class/thermal/%s/type", e->d_name);
            const long n = readTextFile(path, buffer, sizeof buffer);
            if (n > 0) {
                std::string type(buffer, static_cast<std::size_t>(n));
                while (!type.empty() && (type.back() == '\n' || type.back() == ' '))
                    type.pop_back();
                p.thermalZones.push_back(std::move(type));
            }
        }
        ::closedir(d);
    }
    return p;
}

double defaultCacheBudgetMb(const Probe& p, double fraction) noexcept {
    return p.unifiedMemory ? p.memAvailableMb * fraction : 0.0;
}

}  // namespace hw
