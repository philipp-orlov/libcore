// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Child processes without a shell. Replaces popen() / std::system() (command injection, no way to unblock a
// reader, no reaping guarantee).
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace sys {

class Process {
public:
    struct Options {
        std::vector<std::string> argv;  // argv[0] is the program; looked up in PATH when it has no '/'
        bool mergeStderr = false;       // child's stderr goes to the same pipe as stdout
        bool newProcessGroup = true;    // so terminate() can signal everything the child spawned
    };

    // Throws base::Error(Internal, detail = errno) when the child cannot be started (includes "program not found").
    static Process spawn(const Options& options);

    Process() = default;
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();  // terminate() + reap: never leaves a zombie

    enum class ReadResult { Line, Timeout, Eof, Interrupted };
    // Reads one '\n'-terminated line (the newline is stripped; a final unterminated line is returned too).
    // timeout < 0 waits forever. Only one thread may call readLine at a time.
    ReadResult readLine(std::string& out, std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    // Reads up to `size` bytes of the child's stdout (blocking up to `timeout`). Returns the byte count, 0 at end of
    // stream, -1 on timeout, -2 when interrupt() was called. Do not mix with readLine() on the same Process.
    long readSome(void* buffer, std::size_t size, std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    // Makes a readLine() running on another thread return Interrupted without closing any descriptor.
    // Sticky: later readLine() calls return Interrupted until the Process is destroyed. Thread-safe.
    void interrupt() noexcept;

    // SIGTERM to the child's group, SIGKILL after `grace`, then reap. Safe to call twice.
    void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(2000)) noexcept;

    // Waits for exit; returns the exit code, or 128 + signal. -1 if already reaped by terminate().
    int wait() noexcept;

    bool running() const noexcept { return pid_ > 0; }
    long pid() const noexcept { return pid_; }

private:
    void closeAll() noexcept;
    long pid_ = -1;
    int outFd_ = -1, wakeRead_ = -1, wakeWrite_ = -1;
    bool groupLeader_ = false;
    int exitCode_ = -1;
    std::atomic<bool> interrupted_{false};
    std::string pending_;
};

}  // namespace sys
