// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "base/status.hpp"
#include "mem/class_table.hpp"
#include "mini_test.hpp"

using mem::ClassTable;

static void checkTable(const ClassTable& t, std::size_t maxProbe) {
    for (std::size_t c = 0; c < t.count(); ++c) {
        CHECK_EQ(t.classOf(t.bytesOf(c)), c);
        CHECK_EQ(t.bytesOf(c) % 64, 0u);
        if (c) {
            CHECK(t.bytesOf(c) > t.bytesOf(c - 1));
            CHECK_EQ(t.classOf(t.bytesOf(c - 1) + 1), c);   // one byte over the previous class
        }
    }
    // classOf is the smallest class that fits, for every size up to maxProbe.
    std::size_t cls = 0;
    for (std::size_t n = 1; n <= maxProbe && n <= t.maxBytes(); ++n) {
        while (t.bytesOf(cls) < n)
            ++cls;
        if (t.classOf(n) != cls) {
            CHECK_EQ(t.classOf(n), cls);
            return;
        }
    }
    CHECK_EQ(t.classOf(t.maxBytes() + 1), ClassTable::npos);
}

TEST(pow2Table) {
    const ClassTable t = ClassTable::pow2(256, std::size_t{64} << 20);
    CHECK_EQ(t.bytesOf(0), 256u);
    CHECK_EQ(t.maxBytes(), std::size_t{64} << 20);
    CHECK_EQ(t.count(), 19u);
    checkTable(t, 1u << 20);
    CHECK_EQ(t.classOf(0), 0u);
    CHECK_EQ(t.classOf(257), 1u);
}

TEST(pow2RoundsLimitsUp) {
    const ClassTable t = ClassTable::pow2(100, 1000);
    CHECK_EQ(t.bytesOf(0), 128u);
    CHECK_EQ(t.maxBytes(), 1024u);
    checkTable(t, 2000);
}

TEST(quarterStepsTable) {
    const ClassTable t = ClassTable::quarterSteps(256, 1u << 20);
    CHECK_EQ(t.bytesOf(0), 256u);
    CHECK_EQ(t.bytesOf(1), 320u);
    CHECK_EQ(t.bytesOf(2), 384u);
    CHECK_EQ(t.bytesOf(3), 448u);
    CHECK_EQ(t.bytesOf(4), 512u);
    CHECK_EQ(t.maxBytes(), 1u << 20);
    checkTable(t, 1u << 21);
}

TEST(customTable) {
    const ClassTable t = ClassTable::custom({64, 192, 1024, 4096});
    CHECK_EQ(t.count(), 4u);
    CHECK_EQ(t.classOf(65), 1u);
    CHECK_EQ(t.classOf(4096), 3u);
    CHECK_EQ(t.classOf(4097), ClassTable::npos);
    checkTable(t, 5000);
    bool threw = false;
    try { ClassTable::custom({64, 64}); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { ClassTable::custom({100}); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
}

int main() { return tst::runAll("class_table"); }
