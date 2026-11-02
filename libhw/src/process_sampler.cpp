// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "hw/process_sampler.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "hw/procfs.hpp"

namespace hw {
namespace {

std::size_t slotOf(int pid, std::size_t mask) noexcept { return (static_cast<std::uint32_t>(pid) * 2654435761u) & mask; }

}  // namespace

bool parseProcPidStat(std::string_view content, ProcPidStat& out)
{
    const size_t open = content.find('(');
    const size_t close = content.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return false;
    out.pid = std::atoi(std::string(content.substr(0, open)).c_str());
    out.name.assign(content.substr(open + 1, close - open - 1));
    // After the name: state(3) ppid(4) ... utime(14) stime(15): the 12th and 13th tokens counting from "state".
    Scanner fields(content.substr(std::min(close + 2, content.size())));
    std::string_view token;
    for (int index = 0; index < 13; ++index) {
        token = fields.token();
        if (token.empty())
            return false;
        if (index == 11)
            out.utimeTicks = toUnsigned(token);
        else if (index == 12)
            out.stimeTicks = toUnsigned(token);
    }
    return true;
}

double parseVmRssKb(std::string_view statusContent)
{
    Scanner lines(statusContent);
    while (!lines.atEnd()) {
        const std::string_view line = lines.line();
        if (line.rfind("VmRSS:", 0) == 0) {
            Scanner fields(line.substr(6));
            return static_cast<double>(toUnsigned(fields.token()));
        }
    }
    return 0.0;
}

double cpuPercentFromTicks(uint64_t previousTicks, uint64_t currentTicks, double elapsedSeconds, long ticksPerSecond)
{
    if (elapsedSeconds <= 0.0 || currentTicks < previousTicks || ticksPerSecond <= 0)
        return 0.0;
    uint64_t deltaTicks = currentTicks - previousTicks;
    return 100.0 * static_cast<double>(deltaTicks) / (static_cast<double>(ticksPerSecond) * elapsedSeconds);
}

std::vector<ProcessSample> ProcessSampler::sample(size_t topN)
{
    std::vector<ProcessSample> out;
    sample(topN, out);
    return out;
}

void ProcessSampler::sample(size_t topN, std::vector<ProcessSample>& out)
{
    const auto now = std::chrono::steady_clock::now();
    const double elapsedSeconds = havePrevious_ ? std::chrono::duration<double>(now - previousTime_).count() : 0.0;
    static const long ticksPerSecond = sysconf(_SC_CLK_TCK);

    candidates_.clear();
    if (candidates_.capacity() < 2048)
        candidates_.reserve(2048);   // grows (rarely) only past this many processes
    if (current_.empty())
        current_.assign(1u << 13, Entry{});
    else
        std::fill(current_.begin(), current_.end(), Entry{});
    currentCount_ = 0;

    char path[40];
    char buffer[2048];
    DIR* dir = ::opendir("/proc");
    if (dir) {
        while (const dirent* entry = ::readdir(dir)) {
            const char* name = entry->d_name;
            if (name[0] < '1' || name[0] > '9')
                continue;
            int pid = 0;
            bool digits = true;
            for (const char* c = name; *c; ++c) {
                if (*c < '0' || *c > '9') { digits = false; break; }
                pid = pid * 10 + (*c - '0');
            }
            if (!digits)
                continue;
            std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
            const long n = readTextFile(path, buffer, sizeof buffer);
            if (n <= 0)
                continue;   // exited between listing and reading
            const std::string_view content(buffer, static_cast<size_t>(n));
            const size_t open = content.find('(');
            const size_t close = content.rfind(')');
            if (open == std::string_view::npos || close == std::string_view::npos || close < open)
                continue;
            Scanner fields(content.substr(std::min(close + 2, content.size())));
            std::uint64_t utime = 0, stime = 0;
            bool complete = true;
            for (int index = 0; index < 13; ++index) {
                const std::string_view token = fields.token();
                if (token.empty()) { complete = false; break; }
                if (index == 11) utime = toUnsigned(token);
                if (index == 12) stime = toUnsigned(token);
            }
            if (!complete)
                continue;
            const std::uint64_t ticks = utime + stime;

            // Remember this reading (grow the table, rarely, when it is more than half full).
            if ((currentCount_ + 1) * 2 > current_.size()) {
                std::vector<Entry> bigger(current_.size() * 2);
                for (const Entry& e : current_)
                    if (e.pid) {
                        std::size_t i = slotOf(e.pid, bigger.size() - 1);
                        while (bigger[i].pid) i = (i + 1) & (bigger.size() - 1);
                        bigger[i] = e;
                    }
                current_.swap(bigger);
            }
            const std::size_t mask = current_.size() - 1;
            std::size_t i = slotOf(pid, mask);
            while (current_[i].pid) i = (i + 1) & mask;
            current_[i] = {pid, ticks};
            ++currentCount_;

            double cpu = 0.0;
            if (havePrevious_ && !previous_.empty()) {
                const std::size_t pmask = previous_.size() - 1;
                for (std::size_t j = slotOf(pid, pmask); previous_[j].pid; j = (j + 1) & pmask)
                    if (previous_[j].pid == pid) {
                        cpu = cpuPercentFromTicks(previous_[j].ticks, ticks, elapsedSeconds, ticksPerSecond);
                        break;
                    }
            }
            Candidate c;
            c.pid = pid;
            c.cpuPercent = cpu;
            const size_t nameLen = std::min<size_t>(close - open - 1, sizeof c.name - 1);
            std::memcpy(c.name, content.data() + open + 1, nameLen);
            c.name[nameLen] = '\0';
            candidates_.push_back(c);
        }
        ::closedir(dir);
    }

    previous_.swap(current_);
    previousCount_ = currentCount_;
    previousTime_ = now;
    havePrevious_ = true;

    const size_t keep = std::min(topN, candidates_.size());
    std::partial_sort(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(keep), candidates_.end(),
                      [](const Candidate& a, const Candidate& b) { return a.cpuPercent > b.cpuPercent; });
    out.resize(keep);
    for (size_t k = 0; k < keep; ++k) {
        ProcessSample& p = out[k];
        p.pid = candidates_[k].pid;
        p.name.assign(candidates_[k].name);
        p.cpuPercent = candidates_[k].cpuPercent;
        // Resident size is read only for the processes that are reported.
        std::snprintf(path, sizeof path, "/proc/%d/status", p.pid);
        const long n = readTextFile(path, buffer, sizeof buffer);
        p.memoryMb = n > 0 ? parseVmRssKb(std::string_view(buffer, static_cast<size_t>(n))) / 1024.0 : 0.0;
    }
}

}  // namespace hw
