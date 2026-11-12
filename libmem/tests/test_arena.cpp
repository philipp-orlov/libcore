// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <functional>
#include <memory_resource>
#include <string>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/arena.hpp"
#include "mini_test.hpp"

using mem::Arena;
using mem::SizeClassPool;

namespace {
mem::PoolOptions poolOpts() {
    mem::PoolOptions o;
    o.classes = mem::ClassTable::pow2(256, 8u << 20);
    o.smallLimit = 16 << 10;
    o.hooks.abortOnMisuse = false;
    return o;
}
}  // namespace

TEST(alignmentAndZeroSize) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 8192, 2});
    for (std::size_t align : {1u, 2u, 8u, 16u, 64u, 256u, 4096u}) {
        a.allocate(3);
        void* p = a.allocate(10, align);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % align, 0u);
    }
    CHECK(a.allocate(0) != nullptr);
    CHECK(a.tryAllocate(8, 3) == nullptr);          // not a power of two
    CHECK(a.tryAllocate(8, 8192) == nullptr);       // too large an alignment
}

TEST(pointersStayValidAcrossChunkGrowthAndRewind) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 4096, 2});
    auto* first = a.allocateArray<int>(100);
    for (int i = 0; i < 100; ++i) first[i] = i;
    const Arena::Mark m = a.mark();
    std::vector<int*> later;
    for (int i = 0; i < 50; ++i) {
        later.push_back(a.allocateArray<int>(500));      // spans many chunks
        std::fill(later.back(), later.back() + 500, i);
    }
    CHECK(a.chunkCount() > 3);
    for (int i = 0; i < 100; ++i) CHECK_EQ(first[i], i);   // earlier data untouched
    a.rewind(m);
    for (int i = 0; i < 100; ++i) CHECK_EQ(first[i], i);
    auto* again = a.allocateArray<int>(500);
    CHECK(again != nullptr);
}

TEST(scopeRewindsOnExit) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 4096, 2});
    a.allocate(100);
    const std::size_t before = a.usedBytes();
    {
        Arena::Scope s(a);
        a.allocate(10000);
        CHECK(a.usedBytes() > before);
    }
    CHECK_EQ(a.usedBytes(), before);
}

TEST(resetRetainsExactlyRetainChunksAndReturnsTheRest) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 4096, 3});
    for (int i = 0; i < 20; ++i) a.allocate(3000);
    CHECK(a.chunkCount() >= 20);
    a.reset();
    CHECK_EQ(a.usedBytes(), 0u);
    CHECK_EQ(a.chunkCount(), 3u);                          // 3 spare, 0 live
    CHECK_EQ(pool.stats().blocksInUse, 3u);                // the other 17+ went back to the pool
    // The next round is served from the spares: the pool is not touched.
    const auto acquires = pool.stats().acquires;
    a.allocate(3000); a.allocate(3000); a.allocate(3000);
    CHECK_EQ(pool.stats().acquires, acquires);
}

TEST(oversizeRequestsGetADedicatedChunk) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 4096, 2});
    void* big = a.allocate(1 << 20);
    std::memset(big, 1, 1 << 20);
    a.reset();
    CHECK_EQ(a.chunkCount(), 0u);                          // an oversize chunk is not worth keeping
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(destructorReturnsEverything) {
    SizeClassPool pool(poolOpts());
    {
        Arena a({&pool, 4096, 4});
        for (int i = 0; i < 50; ++i) a.allocate(1000);
        a.reset();
        for (int i = 0; i < 10; ++i) a.allocate(1000);
    }
    CHECK_EQ(pool.stats().blocksInUse, 0u);
}

TEST(pmrContainersUseTheArenaNotTheHeap) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 1 << 16, 2});
    a.allocate(1);   // make sure the first chunk exists
    {
        tst::AllocScope scope;
        std::pmr::vector<int> v(a.resource());
        for (int i = 0; i < 1000; ++i) v.push_back(i);
        std::pmr::string s("a fairly long string that does not fit the small string optimisation buffer", a.resource());
        CHECK_EQ(scope.calls(), 0ull);
        CHECK_EQ(v[999], 999);
    }
}

TEST(rewindWithPointersHeldInOuterFrames) {
    // Mirrors a b-tree split: outer frames keep raw pointers while inner frames allocate and rewind.
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 4096, 2});
    struct Node { int depth; Node* parent; char pad[200]; };
    std::vector<Node*> chain;
    std::function<void(Node*, int)> descend = [&](Node* parent, int depth) {
        if (depth == 40) return;
        Arena::Scope scope(a);
        Node* n = a.create<Node>();
        n->depth = depth;
        n->parent = parent;
        chain.push_back(n);
        descend(n, depth + 1);
        for (Node* up = n; up; up = up->parent)             // every ancestor is still intact
            CHECK(up->depth >= 0 && up->depth <= depth);
        chain.pop_back();
    };
    descend(nullptr, 0);
    CHECK_EQ(a.usedBytes(), 0u);
}

int main() { return tst::runAll("arena"); }

TEST(hugeRequestIsRefusedNotWrapped) {
    SizeClassPool pool(poolOpts());
    Arena a({&pool, 8192, 2});
    char* first = static_cast<char*>(a.allocate(16, 1));
    CHECK(first != nullptr);
    const auto used = a.usedBytes();
    CHECK(a.tryAllocate(SIZE_MAX - 7, 1) == nullptr);
    bool threw = false;
    try {
        (void)a.allocateArray<char>(SIZE_MAX - 7);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(a.usedBytes() == used);
    char* next = static_cast<char*>(a.allocate(64, 1));
    CHECK(next + 64 <= first || next >= first + 16);  // no overlap
}
