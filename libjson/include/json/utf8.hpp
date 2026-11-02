// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The validator lives in libbase; this keeps the old include path and name.
#include "base/utf8.hpp"

namespace json {
using base::isValidUtf8;
}
