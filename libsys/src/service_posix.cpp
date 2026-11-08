// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Other systems' service managers (systemd, launchd) are configured by files that belong to the program's packaging, not
// driven from the program: the management calls say so.

#include <unistd.h>

#include <string>

#include "base/status.hpp"
#include "sys/service.hpp"

namespace sys::service {

namespace {
[[noreturn]] void unsupported() {
    throw base::Error(base::Code::Unimplemented, "installing services is for the Windows service manager; use systemd or launchd here");
}
}  // namespace

void install(const Spec&) { unsupported(); }
void remove(const std::string&) { unsupported(); }
void start(const std::string&, std::chrono::milliseconds) { unsupported(); }
void stop(const std::string&, std::chrono::milliseconds) { unsupported(); }
State state(const std::string&) { unsupported(); }

bool isElevated() noexcept { return ::geteuid() == 0; }

std::string executablePath() {
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : std::string();
}

}  // namespace sys::service
