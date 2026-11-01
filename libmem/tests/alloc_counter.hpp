// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Counts calls to the global operator new while a scope is active. Include in exactly one translation unit of a
// test executable.
#pragma once

#include <atomic>
#include <cstdlib>
#include <new>

namespace tst {
inline std::atomic<unsigned long long> g_newCalls{0};
inline std::atomic<bool> g_counting{false};
struct AllocScope {
    AllocScope() { g_newCalls = 0; g_counting = true; }
    ~AllocScope() { g_counting = false; }
    unsigned long long calls() const { return g_newCalls.load(); }
};
}  // namespace tst

void* operator new(std::size_t n) {
    if (tst::g_counting.load(std::memory_order_relaxed))
        tst::g_newCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
