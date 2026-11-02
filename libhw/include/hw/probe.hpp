// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What machine is this? One cheap, side-effect-free snapshot used to pick a memory kind (unified memory on a Jetson
// means host and GPU share RAM), a default memory budget, and a metrics provider. Reads /proc and /sys only; never
// loads a driver or runs a program.
#pragma once

#include <string>
#include <vector>

namespace hw {

struct Probe {
    unsigned logicalCpus = 1, physicalCpus = 1, performanceCpus = 1, numaNodes = 1;
    double memTotalMb = 0, memAvailableMb = 0;
    bool tegra = false;              // NVIDIA Jetson / Tegra SoC
    bool unifiedMemory = false;      // CPU and GPU share one RAM (true on Tegra)
    unsigned nvidiaGpus = 0;         // discrete GPUs the kernel driver knows (/proc/driver/nvidia/gpus)
    std::string model;               // device-tree model or DMI product name; may be empty
    std::vector<std::string> thermalZones;   // /sys/class/thermal/*/type
};

Probe probe();

// 75 % of the memory a GPU workload may plan on: available RAM on unified-memory machines, else 0 (the caller asks the
// driver). Used as a default budget for caches.
double defaultCacheBudgetMb(const Probe& p, double fraction = 0.25) noexcept;

}  // namespace hw
