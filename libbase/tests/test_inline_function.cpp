// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>

#include "base/inline_function.hpp"
#include "mini_test.hpp"

namespace {
std::atomic<long> g_news{0};
}
void* operator new(std::size_t n) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
struct Counted {
    static inline int alive = 0;
    Counted() { ++alive; }
    Counted(const Counted&) { ++alive; }
    Counted(Counted&&) noexcept { ++alive; }
    ~Counted() { --alive; }
    int operator()() const { return 42; }
};
}  // namespace

TEST(callsMovesAndDestroysWithoutAllocating) {
    const long before = g_news.load();
    int hits = 0;
    int* counter = &hits;
    base::InlineFunction<void()> f = [counter] { ++*counter; };
    f();
    base::InlineFunction<void()> g = std::move(f);
    CHECK(!f);
    g();
    CHECK_EQ(hits, 2);
    base::InlineFunction<int(int, int)> add = [](int a, int b) { return a + b; };
    CHECK_EQ(add(2, 3), 5);
    CHECK_EQ(g_news.load(), before);
}

TEST(lifetimesAreBalanced) {
    {
        base::InlineFunction<int()> f = Counted{};
        CHECK_EQ(Counted::alive, 1);
        base::InlineFunction<int()> g = std::move(f);
        CHECK_EQ(Counted::alive, 1);
        CHECK_EQ(g(), 42);
        g = nullptr;
        CHECK_EQ(Counted::alive, 0);
    }
    CHECK_EQ(Counted::alive, 0);
}

TEST(movesOnlyCallablesAreAccepted) {
    auto owned = std::make_unique<int>(7);
    int* raw = owned.get();
    base::InlineFunction<int()> f = [p = std::move(owned)] { return *p; };
    CHECK_EQ(f(), 7);
    (void)raw;
}

int main() { return tst::runAll("inline_function"); }
