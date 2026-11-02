// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <set>
#include <thread>
#include <vector>

#include "mem/page_class_allocator.hpp"
#include "mini_test.hpp"

using mem::PageClassAllocator;

TEST(blocksDoNotOverlapAndFreedBlocksAreReused) {
    PageClassAllocator a(1 << 20);
    std::vector<std::pair<std::uint64_t, std::size_t>> got;
    for (std::size_t size : {1u, 4096u, 5000u, 9000u, 70000u, 4096u}) {
        std::uint64_t off;
        CHECK(a.allocate(size, off));
        got.push_back({off, size});
        CHECK_EQ(off % 4096, 0u);
    }
    for (std::size_t i = 0; i < got.size(); ++i)
        for (std::size_t j = i + 1; j < got.size(); ++j) {
            const std::uint64_t ai = got[i].first, bi = got[j].first;
            auto span = [](std::size_t n) { std::uint64_t s = 4096; while (s < n) s <<= 1; return s; };
            CHECK(ai + span(got[i].second) <= bi || bi + span(got[j].second) <= ai);
        }
    const std::uint64_t first = got[1].first;
    a.releaseHeld(first);
    std::uint64_t again;
    CHECK(a.allocate(4096, again));
    CHECK_EQ(again, first);
}

TEST(stateMachineRefusesWrongTransitions) {
    PageClassAllocator a(64 * 1024);
    std::uint64_t off;
    CHECK(a.allocate(4096, off));
    CHECK(!a.releaseSent(off));          // Held, not Sent
    CHECK(a.markSent(off));
    CHECK(!a.markSent(off));             // already Sent
    CHECK(a.releaseSent(off));
    CHECK(!a.releaseSent(off));          // double release by a misbehaving peer
    a.releaseHeld(off);                  // a free block: ignored
    CHECK_EQ(a.inUse(), 0u);
    CHECK(!a.markSent(1));               // not page aligned
    CHECK(!a.markSent(1 << 30));         // outside
}

TEST(fullRegionRefusesAndAnIdleOneStartsOver) {
    PageClassAllocator a(16 * 4096);
    std::uint64_t off;
    std::vector<std::uint64_t> small;
    for (int i = 0; i < 16; ++i) { CHECK(a.allocate(4096, off)); small.push_back(off); }
    CHECK(!a.allocate(4096, off));       // full
    for (auto o : small) a.releaseHeld(o);
    CHECK(a.allocate(64 * 1024, off));   // all idle: the region is handed out afresh, as one big block
    CHECK_EQ(off, 0u);
    CHECK(!a.allocate(128 * 1024, off)); // larger than the region
}

TEST(concurrentUseKeepsTheBooks) {
    PageClassAllocator a(8 << 20);
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t)
        ts.emplace_back([&, t] {
            std::uint64_t rng = 12345 + t;
            for (int i = 0; i < 30000; ++i) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                std::uint64_t off;
                if (a.allocate(1 + rng % 40000, off)) {
                    if (rng & 1) { CHECK(a.markSent(off)); CHECK(a.releaseSent(off)); }
                    else a.releaseHeld(off);
                }
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(a.inUse(), 0u);
}

int main() { return tst::runAll("page_class_allocator"); }
