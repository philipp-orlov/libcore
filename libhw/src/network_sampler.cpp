// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "hw/network_sampler.hpp"

#include <sstream>
#include <string_view>
#include <vector>

#include "hw/procfs.hpp"

namespace hw {
namespace {

}  // namespace

std::unordered_map<std::string, ProcNetDevCounters> parseProcNetDev(const std::string& content)
{
    std::unordered_map<std::string, ProcNetDevCounters> result;
    std::istringstream lines(content);
    std::string line;
    int lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (lineNumber <= 2)
            continue;  // "Inter-|   Receive ..." / " face |bytes packets ..."

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string name = line.substr(0, colon);
        size_t start = name.find_first_not_of(' ');
        if (start == std::string::npos)
            continue;
        name = name.substr(start);

        std::istringstream fields(line.substr(colon + 1));
        std::vector<uint64_t> values;
        uint64_t value = 0;
        while (fields >> value) values.push_back(value);
        // rxBytes(0) rxPackets rxErrs rxDrop rxFifo rxFrame rxCompressed
        // rxMulticast(7) txBytes(8) ...
        if (values.size() < 9)
            continue;

        ProcNetDevCounters counters;
        counters.rxBytes = values[0];
        counters.txBytes = values[8];
        result[name] = counters;
    }
    return result;
}

ProcNetDevCounters sumNonLoopback(const std::unordered_map<std::string, ProcNetDevCounters>& byInterface)
{
    ProcNetDevCounters total;
    for (auto& [name, counters] : byInterface) {
        if (name == "lo")
            continue;
        total.rxBytes += counters.rxBytes;
        total.txBytes += counters.txBytes;
    }
    return total;
}

// Sum of every non-loopback interface straight from the text: no map, no strings, no allocation.
static ProcNetDevCounters sumProcNetDev(std::string_view content)
{
    ProcNetDevCounters total;
    Scanner lines(content);
    lines.skipLine();
    lines.skipLine();   // the two header lines
    while (!lines.atEnd()) {
        const std::string_view line = lines.line();
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        size_t start = line.find_first_not_of(' ');
        if (start == std::string_view::npos || start >= colon)
            continue;
        if (line.substr(start, colon - start) == "lo")
            continue;
        Scanner fields(line.substr(colon + 1));
        std::uint64_t values[9];
        int n = 0;
        for (; n < 9; ++n) {
            const std::string_view token = fields.token();
            if (token.empty())
                break;
            values[n] = toUnsigned(token);
        }
        if (n < 9)
            continue;
        total.rxBytes += values[0];
        total.txBytes += values[8];
    }
    return total;
}

NetworkSample NetworkSampler::sample()
{
    NetworkSample out;
    char buffer[16384];
    const long n = readTextFile("/proc/net/dev", buffer, sizeof buffer);
    ProcNetDevCounters counters = n > 0 ? sumProcNetDev(std::string_view(buffer, static_cast<size_t>(n))) : ProcNetDevCounters{};
    const auto now = std::chrono::steady_clock::now();

    if (havePrevious_) {
        const double elapsedSeconds = std::chrono::duration<double>(now - previousTime_).count();
        if (elapsedSeconds > 0 && counters.rxBytes >= previous_.rxBytes &&
            counters.txBytes >= previous_.txBytes) {
            out.rxBytesPerSec = static_cast<double>(counters.rxBytes - previous_.rxBytes) / elapsedSeconds;
            out.txBytesPerSec = static_cast<double>(counters.txBytes - previous_.txBytes) / elapsedSeconds;
        }
    }

    previous_ = counters;
    previousTime_ = now;
    havePrevious_ = true;
    return out;
}

}  // namespace hw
