// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/object_pool.hpp"
#include "mini_test.hpp"

namespace {
std::atomic<int> g_alive{0};
struct Session {
    int id;
    std::string peer;   // non-trivial member: proves the destructor runs
    char scratch[300];
    Session(int i, const char* p) : id(i), peer(p) { ++g_alive; }
    ~Session() { --g_alive; }
};
struct Thrower {
    Thrower() { throw std::runtime_error("no"); }
};
}  // namespace

TEST(constructsAndDestroysInPlace) {
    mem::ObjectPool<Session> pool;
    Session* a = pool.acquire(1, "10.0.0.1:443");
    Session* b = pool.acquire(2, "10.0.0.2:443");
    CHECK(a && b && a != b);
    CHECK_EQ(a->peer, std::string("10.0.0.1:443"));
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(a) % 64, 0u);
    CHECK_EQ(g_alive.load(), 2);
    pool.release(a);
    pool.release(b);
    CHECK_EQ(g_alive.load(), 0);
    pool.release(nullptr);
}

TEST(aThrowingConstructorGivesTheSlotBack) {
    mem::ObjectPool<Thrower> pool;
    for (int i = 0; i < 3; ++i) {
        bool threw = false;
        try { pool.acquire(); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(connectDisconnectAMillionTimesWithoutTouchingTheHeap) {
    mem::ObjectPool<Session> pool;
    pool.prewarm(256);
    std::vector<Session*> live;
    live.reserve(128);
    {
        tst::AllocScope scope;
        for (int i = 0; i < 1000000; ++i) {
            live.push_back(pool.acquire(i, "peer"));   // "peer" fits the std::string small buffer
            if (live.size() == 100) {
                for (Session* s : live) pool.release(s);
                live.clear();
            }
        }
        for (Session* s : live) pool.release(s);
        CHECK_EQ(scope.calls(), 0ull);
    }
    CHECK_EQ(g_alive.load(), 0);
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(limitReturnsNullptr) {
    mem::ObjectPool<Session>::Options o;
    o.slabBytes = 64 << 10;
    o.maxBytes = 64 << 10;
    o.threadCacheBlocks = 0;
    mem::ObjectPool<Session> pool(o);
    std::vector<Session*> all;
    while (Session* s = pool.acquire(0, "x"))
        all.push_back(s);
    CHECK(!all.empty());
    for (Session* s : all) pool.release(s);
}

int main() { return tst::runAll("object_pool"); }
