// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "base/status.hpp"
#include "mini_test.hpp"
#include "sys/process.hpp"

using namespace std::chrono_literals;
using sys::Process;

TEST(readsLinesAndEof) {
    Process p = Process::spawn({{"/bin/sh", "-c", "printf 'one\\ntwo\\nlast'"}});
    std::string line;
    CHECK(p.readLine(line) == Process::ReadResult::Line);
    CHECK_EQ(line, std::string("one"));
    CHECK(p.readLine(line) == Process::ReadResult::Line);
    CHECK_EQ(line, std::string("two"));
    CHECK(p.readLine(line) == Process::ReadResult::Line);  // unterminated last line
    CHECK_EQ(line, std::string("last"));
    CHECK(p.readLine(line) == Process::ReadResult::Eof);
    CHECK_EQ(p.wait(), 0);
}

TEST(noShellInterpretation) {
    // The argument is data, not shell syntax.
    Process p = Process::spawn({{"/bin/echo", "$(touch /tmp/never_created_by_test); `id`; a|b"}});
    std::string line;
    CHECK(p.readLine(line) == Process::ReadResult::Line);
    CHECK_EQ(line, std::string("$(touch /tmp/never_created_by_test); `id`; a|b"));
    CHECK(access("/tmp/never_created_by_test", F_OK) != 0);
}

TEST(exitCodeAndMergedStderr) {
    Process p = Process::spawn({{"/bin/sh", "-c", "echo err 1>&2; exit 3"}, true});
    std::string line;
    CHECK(p.readLine(line) == Process::ReadResult::Line);
    CHECK_EQ(line, std::string("err"));
    CHECK_EQ(p.wait(), 3);
}

TEST(missingProgramThrows) {
    bool threw = false;
    try {
        Process::spawn({{"/definitely/not/here"}});
    } catch (const base::Error& e) {
        threw = e.status().detail() == static_cast<std::uint32_t>(ENOENT);
    }
    CHECK(threw);
}

TEST(timeoutReturnsWithoutLosingTheChild) {
    Process p = Process::spawn({{"/bin/sh", "-c", "sleep 0.3; echo late"}});
    std::string line;
    CHECK(p.readLine(line, 30ms) == Process::ReadResult::Timeout);
    CHECK(p.readLine(line, 5000ms) == Process::ReadResult::Line);
    CHECK_EQ(line, std::string("late"));
}

TEST(interruptUnblocksAReaderOnAnotherThreadAndKeepsDescriptorsOpen) {
    Process p = Process::spawn({{"/bin/sh", "-c", "echo first; sleep 30"}});
    std::string line;
    CHECK(p.readLine(line) == Process::ReadResult::Line);
    Process::ReadResult result = Process::ReadResult::Line;
    std::thread reader([&] {
        std::string l;
        result = p.readLine(l);
    });
    std::this_thread::sleep_for(100ms);
    const auto t0 = std::chrono::steady_clock::now();
    p.interrupt();
    reader.join();
    CHECK(result == Process::ReadResult::Interrupted);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(p.running());
    // Sticky.
    CHECK(p.readLine(line, 10ms) == Process::ReadResult::Interrupted);
}

TEST(terminateReapsTheWholeGroupAndLeavesNoZombie) {
    pid_t child;
    {
        Process p = Process::spawn({{"/bin/sh", "-c", "sleep 60 & sleep 60; wait"}});
        child = static_cast<pid_t>(p.pid());
        std::this_thread::sleep_for(100ms);
        const auto t0 = std::chrono::steady_clock::now();
        p.terminate();
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        CHECK(!p.running());
    }
    // Already reaped: waitpid on it must fail with ECHILD, and the group must be empty.
    CHECK(::waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD);
    // Orphaned grandchildren are reparented to init and linger as zombies until it reaps them; give it a moment.
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        gone = ::kill(-child, 0) == -1 && errno == ESRCH;
        if (!gone)
            std::this_thread::sleep_for(10ms);
    }
    CHECK(gone);
}

TEST(destructorTerminates) {
    pid_t child;
    {
        Process p = Process::spawn({{"/bin/sleep", "60"}});
        child = static_cast<pid_t>(p.pid());
    }
    CHECK(::waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

TEST(moveTransfersOwnership) {
    Process a = Process::spawn({{"/bin/echo", "x"}});
    const long pid = a.pid();
    Process b = std::move(a);
    CHECK(!a.running());
    CHECK_EQ(b.pid(), pid);
    std::string line;
    CHECK(b.readLine(line) == Process::ReadResult::Line);
}

int main() { return tst::runAll("process"); }
