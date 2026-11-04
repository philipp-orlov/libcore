// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Counts calls to the global operator new (any thread) while an AllocScope is alive. This header REPLACES the global
// operator new/delete, so include it in exactly one translation unit of an executable.
//
//   bench::AllocScope scope;
//   ... steady-state work ...
//   CHECK_EQ(scope.calls(), 0);
//
// Allocations made through malloc, mmap or placement new are not counted: pair with a pool's mapCalls for mmap.
#pragma once

#include <atomic>
#include <cstdlib>
#include <new>

namespace bench {
inline std::atomic<unsigned long long> g_newCalls{0};
inline std::atomic<bool> g_counting{false};
struct AllocScope {
    AllocScope() { g_newCalls = 0; g_counting = true; }
    ~AllocScope() { g_counting = false; }
    unsigned long long calls() const { return g_newCalls.load(); }
};
}  // namespace bench

void* operator new(std::size_t n) {
    if (bench::g_counting.load(std::memory_order_relaxed))
        bench::g_newCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
// std::stable_sort's temporary buffer and friends ask for the nothrow form, and delete it through the forms below.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (bench::g_counting.load(std::memory_order_relaxed))
        bench::g_newCalls.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// Compatibility for the tests written against the first spelling.
namespace tst {
using bench::AllocScope;
}
