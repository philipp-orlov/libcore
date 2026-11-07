// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/errno.hpp"

#include <cstring>

namespace sys {

namespace {
// GNU strerror_r returns a char*, XSI returns an int; overload on the result type.
[[maybe_unused]] const char* pick(const char* result, const char*) { return result; }
[[maybe_unused]] const char* pick(int result, const char* buffer) { return result == 0 ? buffer : "unknown error"; }
}  // namespace

std::string errnoMessage(int e) {
    char buffer[128] = {};
#if defined(_WIN32)
    if (::strerror_s(buffer, sizeof buffer, e) != 0)
        return "unknown error";
    return std::string(buffer);
#else
    return std::string(pick(::strerror_r(e, buffer, sizeof buffer), buffer));
#endif
}

std::string errnoMessage(const char* what, int e) {
    std::string s(what);
    s += ": ";
    s += errnoMessage(e);
    return s;
}

}  // namespace sys
