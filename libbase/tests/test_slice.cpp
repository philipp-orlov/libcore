// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <string>
#include <vector>

#include "base/slice.hpp"
#include "base/span.hpp"
#include "mini_test.hpp"

TEST(sliceOrderingAndPrefix) {
    using base::Slice;
    CHECK(Slice("abc") < Slice("abd"));
    CHECK(Slice("ab") < Slice("abc"));
    CHECK(Slice("abc") == Slice(std::string("abc")));
    CHECK(Slice("abc").startsWith("ab"));
    CHECK(Slice("abc").endsWith("bc"));
    CHECK(!Slice("abc").startsWith("abcd"));
    CHECK(Slice("abc").startsWith(""));
    CHECK_EQ(Slice("abc").compare(Slice("abc")), 0);
}

TEST(sliceSubspanClamps) {
    base::Slice s("abcdef");
    CHECK_EQ(s.subspan(2, 3).string(), std::string("cde"));
    CHECK_EQ(s.subspan(4).string(), std::string("ef"));
    CHECK_EQ(s.subspan(4, 100).string(), std::string("ef"));
    CHECK(s.subspan(100).empty());
}

TEST(sliceAsChecksSize) {
    const std::uint32_t v = 0xdeadbeef;
    CHECK_EQ(base::Slice::ref(v).as<std::uint32_t>(), v);
    bool threw = false;
    try {
        (void)base::Slice("abc").as<std::uint32_t>();
    } catch (const base::Error& e) {
        threw = e.code() == base::Code::InvalidArgument;
    }
    CHECK(threw);
}

TEST(writableSliceAssignClamps) {
    char buf[4] = {};
    base::WritableSlice w(buf, 3);
    w.assign("abcdef", 6);
    CHECK_EQ(std::string(buf, 3), std::string("abc"));
    CHECK_EQ(buf[3], '\0');
    base::Slice view = w;
    CHECK_EQ(view.size(), 3u);
}

TEST(spanViews) {
    std::vector<int> v{1, 2, 3, 4};
    base::Span<int> s(v);
    CHECK_EQ(s.size(), 4u);
    int sum = 0;
    for (int x : s)
        sum += x;
    CHECK_EQ(sum, 10);
    CHECK_EQ(s.subspan(1, 2).front(), 2);
    CHECK_EQ(s.last(1).back(), 4);
    CHECK(s.subspan(9).empty());
    const std::array<int, 2> a{7, 8};
    base::Span<const int> c(a);
    CHECK_EQ(c[1], 8);
}
int main() { return tst::runAll("slice"); }
