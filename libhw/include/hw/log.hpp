// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libhw reports odd input ("unrecognized line, skipped") through one replaceable hook so that it does not depend on
// any logger; the application routes it into its own (liblog, libhttp's logger, syslog). Default: stderr.
#pragma once

namespace hw {

using WarnHook = void (*)(const char* message);
void setWarnHook(WarnHook hook) noexcept;   // null restores the default
void warn(const char* message) noexcept;

}  // namespace hw
