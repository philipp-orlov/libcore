// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "base/utf16.hpp"
#include "base/utf8.hpp"
#include "mini_test.hpp"

namespace {
std::string toUtf8(std::u16string_view s) {
    std::string out;
    base::appendUtf8FromUtf16(s, out);
    return out;
}
std::u16string toUtf16(std::string_view s) {
    std::u16string out;
    base::appendUtf16FromUtf8(s, out);
    return out;
}
}  // namespace

TEST(everyPlaneRoundTrips) {
    const std::u16string text = u"aé€\U0001F600z";
    const std::string bytes = toUtf8(text);
    CHECK_EQ(bytes, std::string("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z"));
    CHECK(base::isValidUtf8(bytes));
    CHECK(toUtf16(bytes) == text);
}

TEST(aLoneSurrogateSurvivesARoundTrip) {
    const std::u16string lone(1, char16_t(0xD83D));
    const std::string bytes = toUtf8(lone);
    CHECK_EQ(bytes, std::string("\xED\xA0\xBD"));
    CHECK(!base::isValidUtf8(bytes));
    CHECK(toUtf16(bytes) == lone);
}

TEST(badBytesDecodeToTheReplacementCharacter) {
    CHECK(toUtf16("\x80z") == std::u16string(u"�z"));
    CHECK(toUtf16("\xFFz") == std::u16string(u"�z"));
    CHECK(toUtf16("a\xE2\x82").size() == 3);  // a truncated sequence costs its lead byte, not the rest
    CHECK(toUtf16("\xC3") == std::u16string(u"�"));
}

TEST(codePointsAndSurrogatesConvertBothWays) {
    std::u16string out;
    base::appendCodePointUtf16(out, 0x41);
    base::appendCodePointUtf16(out, 0x1F600);
    CHECK(out == std::u16string(u"A\U0001F600"));
    CHECK(base::isHighSurrogate(out[1]));
    CHECK(base::isLowSurrogate(out[2]));
    CHECK_EQ(base::joinSurrogates(out[1], out[2]), 0x1F600u);
}

TEST(appendKeepsWhatIsThere) {
    std::string out = "x";
    base::appendUtf8FromUtf16(u"y", out);
    CHECK_EQ(out, std::string("xy"));
}

int main() { return tst::runAll("utf16"); }
