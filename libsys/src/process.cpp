// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <thread>
#include <utility>

#include "base/status.hpp"
#include "sys/errno.hpp"

extern char** environ;

namespace sys {

namespace {
[[noreturn]] void fail(const char* what, int e) {
    throw base::Error(base::Code::Internal, errnoMessage(what, e), static_cast<std::uint32_t>(e));
}
int makePipe(int fds[2], bool nonBlock) {
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC | (nonBlock ? O_NONBLOCK : 0));
#else
    if (::pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; ++i) {
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (nonBlock)
            ::fcntl(fds[i], F_SETFL, ::fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
#endif
}
}  // namespace

Process Process::spawn(const Options& options) {
    if (options.argv.empty())
        throw base::Error(base::Code::InvalidArgument, "Process::spawn: empty argv");
    int out[2] = {-1, -1}, wake[2] = {-1, -1};
    if (makePipe(out, false) != 0)
        fail("pipe", errno);
    if (makePipe(wake, true) != 0) {
        const int e = errno;
        ::close(out[0]);
        ::close(out[1]);
        fail("pipe", e);
    }

    // Report exec failure (program not found) as an error instead of a child that exits 127: use a close-on-exec
    // status pipe that the child's exec closes on success. posix_spawn already reports ENOENT synchronously on
    // glibc >= 2.24 and macOS, so no extra pipe is needed here.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    if (options.mergeStderr)
        posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags = 0;
    if (options.newProcessGroup) {
        flags |= POSIX_SPAWN_SETPGROUP;
        posix_spawnattr_setpgroup(&attributes, 0);
    }
    sigset_t none;
    sigemptyset(&none);
    flags |= POSIX_SPAWN_SETSIGMASK;
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setflags(&attributes, flags);

    std::vector<char*> argv;
    argv.reserve(options.argv.size() + 1);
    for (const std::string& a : options.argv)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(out[1]);
    if (rc != 0) {
        ::close(out[0]);
        ::close(wake[0]);
        ::close(wake[1]);
        fail(("posix_spawn " + options.argv[0]).c_str(), rc);
    }
    Process p;
    p.pid_ = pid;
    p.outFd_ = out[0];
    p.wakeRead_ = wake[0];
    p.wakeWrite_ = wake[1];
    p.groupLeader_ = options.newProcessGroup;
    return p;
}

Process::Process(Process&& o) noexcept
    : pid_(o.pid_), outFd_(o.outFd_), wakeRead_(o.wakeRead_), wakeWrite_(o.wakeWrite_), groupLeader_(o.groupLeader_),
      exitCode_(o.exitCode_), interrupted_(o.interrupted_.load()), pending_(std::move(o.pending_)) {
    o.pid_ = -1;
    o.outFd_ = o.wakeRead_ = o.wakeWrite_ = -1;
}

Process& Process::operator=(Process&& o) noexcept {
    if (this != &o) {
        terminate();
        closeAll();
        pid_ = o.pid_;
        outFd_ = o.outFd_;
        wakeRead_ = o.wakeRead_;
        wakeWrite_ = o.wakeWrite_;
        groupLeader_ = o.groupLeader_;
        exitCode_ = o.exitCode_;
        interrupted_.store(o.interrupted_.load());
        pending_ = std::move(o.pending_);
        o.pid_ = -1;
        o.outFd_ = o.wakeRead_ = o.wakeWrite_ = -1;
    }
    return *this;
}

Process::~Process() {
    terminate();
    closeAll();
}

void Process::closeAll() noexcept {
    for (int* fd : {&outFd_, &wakeRead_, &wakeWrite_})
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
}

Process::ReadResult Process::readLine(std::string& out, std::chrono::milliseconds timeout) {
    if (outFd_ < 0)
        return ReadResult::Eof;
    const auto deadline = std::chrono::steady_clock::now() + (timeout.count() < 0 ? std::chrono::hours(24 * 365) : timeout);
    for (;;) {
        const std::size_t newline = pending_.find('\n');
        if (newline != std::string::npos) {
            out.assign(pending_, 0, newline);
            pending_.erase(0, newline + 1);
            return ReadResult::Line;
        }
        if (interrupted_.load(std::memory_order_acquire))
            return ReadResult::Interrupted;

        int waitMs = -1;
        if (timeout.count() >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                return ReadResult::Timeout;
            waitMs = static_cast<int>(left.count() > 1000000 ? 1000000 : left.count());
        }
        pollfd fds[2] = {{outFd_, POLLIN, 0}, {wakeRead_, POLLIN, 0}};
        const int ready = ::poll(fds, 2, waitMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return ReadResult::Eof;
        }
        if (ready == 0)
            continue;  // loop re-checks the deadline
        if (fds[1].revents & POLLIN)
            return ReadResult::Interrupted;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[4096];
            const ssize_t n = ::read(outFd_, buffer, sizeof buffer);
            if (n > 0) {
                pending_.append(buffer, static_cast<std::size_t>(n));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                if (pending_.empty())
                    return ReadResult::Eof;
                out.swap(pending_);
                pending_.clear();
                return ReadResult::Line;  // last line without a newline
            }
        }
    }
}

void Process::interrupt() noexcept {
    interrupted_.store(true, std::memory_order_release);
    if (wakeWrite_ >= 0) {
        const char byte = 1;
        const ssize_t ignored = ::write(wakeWrite_, &byte, 1);  // a full pipe is already readable
        (void)ignored;
    }
}

void Process::terminate(std::chrono::milliseconds grace) noexcept {
    if (pid_ <= 0)
        return;
    const pid_t target = groupLeader_ ? -pid_ : pid_;
    ::kill(target, SIGTERM);
    int status = 0;
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (!reaped && std::chrono::steady_clock::now() < deadline) {
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || (r < 0 && errno != EINTR)) {
            reaped = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (!reaped) {
        ::kill(target, SIGKILL);
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    pid_ = -1;
}

int Process::wait() noexcept {
    if (pid_ <= 0)
        return exitCode_;
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0) {
        if (errno != EINTR) {
            pid_ = -1;
            return -1;
        }
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    pid_ = -1;
    return exitCode_;
}

}  // namespace sys
