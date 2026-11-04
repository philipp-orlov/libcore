// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <unordered_map>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/pool_allocator.hpp"
#include "mini_test.hpp"

namespace {
mem::SizeClassPool& testPool() {
    static mem::SizeClassPool* pool = [] {
        mem::PoolOptions options;
        options.classes = mem::ClassTable::pow2(64, std::size_t{1} << 20);
        options.smallLimit = std::size_t{16} << 10;
        options.name = "test";
        return new mem::SizeClassPool(options);
    }();
    return *pool;
}
template <class T>
using Alloc = mem::PoolAllocator<T, &testPool>;
template <class T>
using Vec = std::vector<T, Alloc<T>>;
using Str = std::basic_string<char, std::char_traits<char>, Alloc<char>>;
}  // namespace

TEST(aVectorOverThePoolBehavesLikeAVector) {
    Vec<int> v;
    for (int i = 0; i < 100000; ++i)
        v.push_back(i);
    for (int i = 0; i < 100000; ++i)
        CHECK_EQ(v[std::size_t(i)], i);
    Vec<int> moved = std::move(v);
    CHECK_EQ(moved.size(), 100000u);
    moved.clear();
    moved.shrink_to_fit();
    CHECK_EQ(moved.capacity(), 0u);
}

TEST(sizeIsTheSizeOfTheStandardContainer) {
    static_assert(sizeof(Vec<int>) == sizeof(std::vector<int>));
    static_assert(sizeof(Alloc<int>) == 1);
}

TEST(steadyStateStaysOutOfTheProcessHeap) {
    Str text;
    Vec<int> numbers;
    text.reserve(32);
    numbers.reserve(1);
    bench::AllocScope scope;
    for (int round = 0; round < 1000; ++round) {
        Vec<int> v;
        for (int i = 0; i < 300; ++i)
            v.push_back(i);
        Str s(1000, 'x');
        s += s;
    }
    CHECK_EQ(scope.calls(), 0u);
    CHECK(testPool().stats().misuse == 0);
}

TEST(aRequestBeyondTheLargestClassIsMappedDirectly) {
    const auto before = testPool().stats();
    {
        Vec<char> big(std::size_t{3} << 20, 'x');
        CHECK_EQ(big[(std::size_t{3} << 20) - 1], 'x');
    }
    const auto after = testPool().stats();
    CHECK_EQ(after.acquires, before.acquires);
    CHECK_EQ(after.misuse, 0u);
}

TEST(mapNodesComeFromThePoolToo) {
    std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, Alloc<std::pair<const int, int>>> map;
    for (int i = 0; i < 5000; ++i)
        map[i] = i * 2;
    CHECK_EQ(map[4999], 9998);
    map.clear();
    CHECK(testPool().stats().misuse == 0);
}

int main() { return tst::runAll("pool_allocator"); }
