// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace mem {

#ifdef NDEBUG
inline constexpr bool kDebugBuild = false;
#else
inline constexpr bool kDebugBuild = true;
#endif

// Callbacks a pool uses to report what it cannot handle itself. Both may be null. They can be called from any
// thread, with no pool lock held, and must not call back into the pool that is reporting.
struct Hooks {
    void (*onMisuse)(void* ctx, const char* what) = nullptr;       // double free, foreign pointer, wrong capacity
    void (*onExhausted)(void* ctx, const char* pool, std::size_t bytes) = nullptr;
    void* ctx = nullptr;
    // After onMisuse returns, abort(). On by default in debug builds so misuse is found by tests; release builds
    // count it, leave the block out of circulation and carry on.
    bool abortOnMisuse = kDebugBuild;
};

void reportMisuse(const Hooks& hooks, const char* what) noexcept;
void reportExhausted(const Hooks& hooks, const char* pool, std::size_t bytes) noexcept;

}  // namespace mem
