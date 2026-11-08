// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The parts of sys/service.hpp that are the same everywhere: the foreground host and the small helpers. The Windows service
// manager is service_windows.cpp; other systems have service_posix.cpp.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

#include "base/status.hpp"
#include "service_internal.hpp"
#include "sys/service.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

namespace sys::service {

std::string quoteArgument(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos)
        return argument;
    // Backslashes count only in front of a quote (or the closing one), where each doubles and the quote gets one more.
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

const char* stateName(State state) noexcept {
    switch (state) {
        case State::NotInstalled: return "not installed";
        case State::Stopped: return "stopped";
        case State::Starting: return "starting";
        case State::Stopping: return "stopping";
        case State::Running: return "running";
        case State::Paused: return "paused";
        case State::Unknown: break;
    }
    return "unknown";
}

namespace detail {

namespace {

BasicControl* gForeground = nullptr;

#if defined(_WIN32)

BOOL WINAPI onConsoleEvent(DWORD) {
    // Ctrl-C, Ctrl-Break, closing the console window, logoff and shutdown. Windows ends the process a few seconds after
    // this returns for the last three, which is time enough for a program that stops promptly.
    if (gForeground)
        gForeground->requestStop();
    return TRUE;
}

#else

std::atomic<bool> gSignalled{false};

void onSignal(int) { gSignalled.store(true); }

#endif

}  // namespace

int runInForeground(const std::string& name, const std::function<int(Control&)>& body) {
    BasicControl control;
    gForeground = &control;
#if defined(_WIN32)
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    gSignalled.store(false);
    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    struct sigaction previousInterrupt {}, previousTerminate {};
    sigaction(SIGINT, &action, &previousInterrupt);
    sigaction(SIGTERM, &action, &previousTerminate);
    // A signal handler may only set a flag; this thread turns the flag into the call.
    std::atomic<bool> finished{false};
    std::thread watcher([&] {
        while (!finished.load()) {
            if (gSignalled.exchange(false))
                control.requestStop();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
#endif
    int code = 1;
    try {
        code = body(control);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        code = 1;
    }
#if defined(_WIN32)
    SetConsoleCtrlHandler(onConsoleEvent, FALSE);
#else
    finished.store(true);
    watcher.join();
    sigaction(SIGINT, &previousInterrupt, nullptr);
    sigaction(SIGTERM, &previousTerminate, nullptr);
#endif
    gForeground = nullptr;
    return code;
}

}  // namespace detail

int run(const std::string& name, bool asService, const std::function<int(Control&)>& body) {
    if (asService) {
#if defined(_WIN32)
        return detail::runUnderServiceManager(name, body);
#else
        throw base::Error(base::Code::Unimplemented, "running as a service is for the Windows service manager");
#endif
    }
    return detail::runInForeground(name, body);
}

}  // namespace sys::service
