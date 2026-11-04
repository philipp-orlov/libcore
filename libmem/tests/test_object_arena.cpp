// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <stdexcept>
#include <string>
#include <vector>

#include "mem/object_arena.hpp"
#include "mini_test.hpp"

namespace {
struct Tracked {
    explicit Tracked(std::vector<int>& log, int id) : log_(log), id_(id) {}
    ~Tracked() { log_.push_back(id_); }
    std::vector<int>& log_;
    int id_;
};
struct Throws {
    Throws() { throw std::runtime_error("no"); }
    ~Throws() { CHECK(false); }
};
struct Plain {
    int a = 1;
};
}  // namespace

TEST(destructorsRunNewestFirstAtReset) {
    std::vector<int> log;
    mem::ObjectArena arena;
    arena.make<Tracked>(log, 1);
    arena.make<Plain>();
    arena.make<Tracked>(log, 2);
    CHECK(log.empty());
    arena.reset();
    CHECK_EQ(log.size(), 2u);
    CHECK_EQ(log[0], 2);
    CHECK_EQ(log[1], 1);
    arena.reset();
    CHECK_EQ(log.size(), 2u);
}

TEST(theDestructorOfTheArenaResetsIt) {
    std::vector<int> log;
    {
        mem::ObjectArena arena;
        arena.make<Tracked>(log, 7);
    }
    CHECK_EQ(log.size(), 1u);
}

TEST(aThrowingConstructorRegistersNothing) {
    mem::ObjectArena arena;
    bool threw = false;
    try {
        arena.make<Throws>();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(nodesOwningHeapStateAreCleanedAndTheArenaIsReusable) {
    mem::ObjectArena arena;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 5000; ++i) {
            auto* s = arena.make<std::string>(200, 'x');
            CHECK_EQ(s->size(), 200u);
        }
        CHECK(arena.usedBytes() > 0);
        arena.reset();
        CHECK_EQ(arena.usedBytes(), 0u);
    }
}

TEST(aLargeNodeFits) {
    struct Big {
        char bytes[1 << 20];
    };
    mem::ObjectArena arena;
    auto* big = arena.make<Big>();
    big->bytes[(1 << 20) - 1] = 1;
    CHECK_EQ(big->bytes[(1 << 20) - 1], 1);
}

int main() { return tst::runAll("object_arena"); }
