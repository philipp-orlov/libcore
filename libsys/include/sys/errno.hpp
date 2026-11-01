// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace sys {

// Thread-safe text for an errno value (strerror_r / FormatMessage), never the shared buffer of std::strerror.
std::string errnoMessage(int e);

// "<what>: <errnoMessage(e)>"
std::string errnoMessage(const char* what, int e);

}  // namespace sys
