// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alloc_counter.hpp"
#include "lg/logger.hpp"
#include "mem/size_class_pool.hpp"
#include "mini_test.hpp"

using namespace std::chrono_literals;

namespace {
struct Capture {
    std::mutex mu;
    std::string text;
    std::atomic<std::uint64_t> lines{0};
    std::unique_ptr<lg::Sink> sink() {
        return lg::makeCallbackSink([this](lg::Level, const char* d, std::size_t n) {
            std::lock_guard<std::mutex> g(mu);
            text.append(d, n);
            lines.fetch_add(1, std::memory_order_relaxed);
        });
    }
};
lg::LoggerOptions plain() {
    lg::LoggerOptions o;
    o.timestamps = false;
    o.level = lg::Level::Trace;
    return o;
}
std::string tmpPath(const char* name) { return std::string("/tmp/liblog_test_") + name + "_" + std::to_string(::getpid()); }
}  // namespace

TEST(formatsLevelsAndKeepsOrderOfOneThread) {
    Capture cap;
    lg::Logger log(plain());
    log.addSink(cap.sink());
    log.write(lg::Level::Info, "hello %d", 1);
    log.write(lg::Level::Error, "bad %s", "thing");
    log.write(lg::Level::Trace, "t");
    log.flush();
    CHECK_EQ(cap.text, std::string("[INFO ] hello 1\n[ERROR] bad thing\n[TRACE] t\n"));
    log.setLevel(lg::Level::Warn);
    log.write(lg::Level::Info, "filtered");
    log.write(lg::Level::Warn, "kept");
    log.flush();
    CHECK(cap.text.find("filtered") == std::string::npos);
    CHECK(cap.text.find("kept") != std::string::npos);
}

TEST(timestampPrefix) {
    Capture cap;
    lg::LoggerOptions o = plain();
    o.timestamps = true;
    lg::Logger log(o);
    log.addSink(cap.sink());
    log.write(lg::Level::Info, "x");
    log.flush();
    // 2026-11-01 12:34:56.789 [INFO ] x
    CHECK(cap.text.size() > 30);
    CHECK_EQ(cap.text[4], '-');
    CHECK_EQ(cap.text[13], ':');
    CHECK_EQ(cap.text[19], '.');
    CHECK_EQ(cap.text.substr(24, 8), std::string("[INFO ] "));
}

TEST(controlCharactersCannotForgeLines) {
    Capture cap;
    lg::Logger log(plain());
    log.addSink(cap.sink());
    log.write(lg::Level::Info, "a\nb\r\tc\x01\x7f%s", "\n[ERROR] forged");
    log.flush();
    CHECK_EQ(cap.text, std::string("[INFO ] a\\nb\\r\\tc\\x01\\x7f\\n[ERROR] forged\n"));
    CHECK_EQ(std::count(cap.text.begin(), cap.text.end(), '\n'), 1);
}

TEST(longMessagesAreCutAndCounted) {
    Capture cap;
    lg::Logger log(plain());
    log.addSink(cap.sink());
    log.write(lg::Level::Info, "%s", std::string(5000, 'x').c_str());
    log.flush();
    CHECK_EQ(log.truncatedLines(), 1u);
    CHECK(cap.text.find("...\n") != std::string::npos);
    CHECK(cap.text.size() < 300);
}

TEST(everyLineIsAccountedForUnderContention) {
    Capture cap;
    lg::Logger log(plain());
    log.addSink(cap.sink());
    constexpr int kThreads = 8, kEach = 100000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i)
                log.write(lg::Level::Info, "t%d line %d", t, i);
        });
    for (auto& t : ts) t.join();
    log.flush();
    const std::uint64_t lines = static_cast<std::uint64_t>(std::count(cap.text.begin(), cap.text.end(), '\n'));
    CHECK_EQ(lines + log.droppedLines(), std::uint64_t{kThreads} * kEach);
    CHECK_EQ(log.writtenLines(), lines);
    // Per-thread order is preserved among the lines that made it.
    for (int t = 0; t < kThreads; ++t) {
        long last = -1;
        std::istringstream in(cap.text);
        std::string line;
        const std::string tag = "t" + std::to_string(t) + " line ";
        while (std::getline(in, line)) {
            const auto at = line.find(tag);
            if (at == std::string::npos) continue;
            const long n = std::stol(line.substr(at + tag.size()));
            CHECK(n > last);
            last = n;
        }
    }
    std::fprintf(stderr, "  delivered %llu of %d, dropped %llu\n", (unsigned long long)lines, kThreads * kEach, (unsigned long long)log.droppedLines());
}

TEST(flushWaitsForEverythingProducedBeforeTheCall) {
    Capture cap;
    lg::Logger log(plain());
    log.addSink(cap.sink());
    std::atomic<bool> stop{false};
    std::thread noise([&] { while (!stop) { log.write(lg::Level::Info, "noise"); } });
    int failures = 0;
    for (int i = 0; i < 300; ++i) {
        const std::uint64_t droppedBefore = log.droppedLines();
        log.write(lg::Level::Warn, "marker %d", i);
        const std::uint64_t droppedAfter = log.droppedLines();
        log.flush();
        if (droppedAfter != droppedBefore) continue;   // the marker itself was dropped (ring full): nothing to wait for
        std::lock_guard<std::mutex> g(cap.mu);
        if (cap.text.find("marker " + std::to_string(i) + "\n") == std::string::npos) ++failures;
    }
    stop = true;
    noise.join();
    CHECK_EQ(failures, 0);
}

TEST(shutdownDrainsEverythingQueued) {
    std::atomic<bool> gate{false};
    std::atomic<int> got{0};
    {
        lg::Logger log(plain());
        log.addSink(lg::makeCallbackSink([&](lg::Level, const char*, std::size_t) { while (!gate.load()) std::this_thread::sleep_for(1ms); ++got; }));
        for (int i = 0; i < 40; ++i) log.write(lg::Level::Info, "queued %d", i);
        std::thread opener([&] { std::this_thread::sleep_for(100ms); gate = true; });
        log.shutdown();                  // must wait for all 40 even though the sink was blocked
        opener.join();
        CHECK_EQ(got.load(), 40);
        log.write(lg::Level::Info, "after");   // refused, counted
        CHECK(log.droppedLines() >= 1u);
    }
}

TEST(fileSinkModeNoFollowRotateReopen) {
    const std::string path = tmpPath("file");
    ::unlink(path.c_str());
    {
        lg::FileSinkOptions fo;
        fo.path = path;
        fo.mode = 0600;
        fo.rotateBytes = 2000;
        fo.keep = 2;
        std::string err;
        auto sink = lg::makeFileSink(fo, &err);
        CHECK(sink != nullptr);
        lg::Logger log(plain());
        log.addSink(std::move(sink));
        struct stat st {};
        CHECK_EQ(::stat(path.c_str(), &st), 0);
        CHECK_EQ(st.st_mode & 0777, 0600u);
        for (int i = 0; i < 200; ++i) log.write(lg::Level::Info, "line number %d with some padding to fill the file", i);
        log.flush();
        CHECK_EQ(::stat((path + ".1").c_str(), &st), 0);      // rotated
        CHECK(::stat((path + ".3").c_str(), &st) != 0);       // keep = 2
        // logrotate-style rename, then reopen: new lines land in a fresh file at the old path.
        ::rename(path.c_str(), (path + ".moved").c_str());
        log.reopenSinks();
        log.write(lg::Level::Info, "after reopen");
        log.flush();
        std::ifstream in(path);
        std::string all((std::istreambuf_iterator<char>(in)), {});
        CHECK(all.find("after reopen") != std::string::npos);
    }
    // O_NOFOLLOW: a symlink at the log path is refused.
    const std::string link = path + ".link";
    ::unlink(link.c_str());
    CHECK_EQ(::symlink(path.c_str(), link.c_str()), 0);
    lg::FileSinkOptions fo2;
    fo2.path = link;
    std::string err;
    CHECK(lg::makeFileSink(fo2, &err) == nullptr);
    CHECK(!err.empty());
    for (const char* suffix : {"", ".1", ".2", ".moved", ".link"}) ::unlink((path + suffix).c_str());
}

TEST(producersDoNotAllocateAfterTheirFirstLine) {
    lg::LoggerOptions o = plain();
    lg::Logger log(o);
    log.addSink(lg::makeCallbackSink([](lg::Level, const char*, std::size_t) {}));
    log.write(lg::Level::Info, "warm up");
    log.flush();
    tst::AllocScope scope;
    for (int i = 0; i < 200000; ++i) {
        log.write(lg::Level::Info, "request %d took %d us", i, i * 3);
        if ((i & 31) == 31) std::this_thread::yield();
    }
    log.flush();
    CHECK_EQ(scope.calls(), 0ull);   // counts the consumer thread too
}

TEST(ringsAreReturnedWhenThreadsExit) {
    auto& pool = mem::SizeClassPool::global();
    const auto before = pool.stats().blocksInUse;
    {
        Capture cap;
        lg::Logger log(plain());
        log.addSink(cap.sink());
        for (int round = 0; round < 100; ++round) {
            std::thread([&] { log.write(lg::Level::Info, "short lived"); }).join();
        }
        log.flush();
        // Retired empty rings are freed by the consumer on its next pass.
        for (int i = 0; i < 100 && pool.stats().blocksInUse > before + 2; ++i) std::this_thread::sleep_for(10ms);
        CHECK(pool.stats().blocksInUse <= before + 2);
    }
    CHECK_EQ(pool.stats().blocksInUse, before);
}

TEST(parseLevel) {
    CHECK(lg::parseLevel("WARNING", lg::Level::Info) == lg::Level::Warn);
    CHECK(lg::parseLevel("Debug", lg::Level::Info) == lg::Level::Debug);
    CHECK(lg::parseLevel("none", lg::Level::Info) == lg::Level::Off);
    CHECK(lg::parseLevel("nonsense", lg::Level::Error) == lg::Level::Error);
    CHECK(lg::parseLevel(nullptr, lg::Level::Error) == lg::Level::Error);
}

int main() { return tst::runAll("logger"); }
