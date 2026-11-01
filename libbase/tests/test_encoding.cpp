// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <random>
#include <string>

#include "base/encoding.hpp"
#include "base/utf8.hpp"
#include "mini_test.hpp"

TEST(base64Rfc4648Vectors) {
    const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        const std::string p = plain[i];
        CHECK_EQ(base::base64Encode(p.data(), p.size()), std::string(coded[i]));
        CHECK_EQ(base::base64Decode(coded[i]), p);
        char buf[16];
        std::size_t n = 0;
        CHECK(base::base64DecodeStrict(coded[i], buf, &n));
        CHECK_EQ(std::string(buf, n), p);
    }
}

TEST(base64StrictRejectsBadInput) {
    char buf[16];
    std::size_t n = 0;
    CHECK(!base::base64DecodeStrict("Zg=", buf, &n));      // length not a multiple of 4
    CHECK(!base::base64DecodeStrict("Zg=A", buf, &n));     // data after padding
    CHECK(!base::base64DecodeStrict("=g==", buf, &n));     // padding too early
    CHECK(!base::base64DecodeStrict("Zm9v====", buf, &n)); // padding in a non-final quantum
    CHECK(!base::base64DecodeStrict("Zm 9", buf, &n));     // blank
    CHECK(!base::base64DecodeStrict("Zm-9", buf, &n));     // URL alphabet
    // The lenient decoder skips what the strict one rejects, as libhttp always did.
    CHECK_EQ(base::base64Decode("Zm 9\nv"), std::string("foo"));
}

TEST(base64RandomRoundTrip) {
    std::mt19937_64 rng(1);
    for (int len = 0; len < 300; ++len) {
        std::string raw(len, '\0');
        for (char& c : raw)
            c = static_cast<char>(rng());
        std::string text(base::base64Size(raw.size()), '\0');
        CHECK_EQ(base::base64Encode(raw.data(), raw.size(), text.data()), text.size());
        std::string back(base::base64MaxDecodedSize(text.size()), '\0');
        back.resize(base::base64Decode(text, back.data()));
        CHECK_EQ(back, raw);
        std::size_t n = 0;
        std::string strict(back.size() + 3, '\0');
        CHECK(base::base64DecodeStrict(text, strict.data(), &n));
        CHECK_EQ(std::string(strict.data(), n), raw);
    }
}

TEST(hexRoundTrip) {
    const unsigned char raw[] = {0x00, 0x01, 0xab, 0xff};
    CHECK_EQ(base::hexEncode(raw, 4), std::string("0001abff"));
    unsigned char out[4];
    std::size_t n = 0;
    CHECK(base::hexDecode("0001ABff", out, &n));
    CHECK_EQ(n, 4u);
    CHECK(std::memcmp(out, raw, 4) == 0);
    CHECK(!base::hexDecode("abc", out, &n));
    CHECK(!base::hexDecode("zz", out, &n));
}

namespace {
// Byte-at-a-time reference validator with the RFC 3629 table.
bool referenceUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = s[i];
        if (c < 0x80) { ++i; continue; }
        int n;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else if (c >= 0xF1 && c <= 0xF3) n = 3;
        else return false;
        if (i + n >= s.size()) return false;
        for (int k = 1; k <= n; ++k) {
            const unsigned char b = s[i + k];
            const unsigned l = (k == 1) ? lo : 0x80, h = (k == 1) ? hi : 0xBF;
            if (b < l || b > h) return false;
        }
        i += n + 1;
    }
    return true;
}
}  // namespace

TEST(utf8Corpus) {
    CHECK(base::isValidUtf8(""));
    CHECK(base::isValidUtf8("plain ascii text longer than eight bytes"));
    CHECK(base::isValidUtf8("\xC3\xA9"));                  // e acute
    CHECK(base::isValidUtf8("\xE2\x82\xAC"));              // euro
    CHECK(base::isValidUtf8("\xF0\x9F\x98\x80"));          // emoji
    CHECK(base::isValidUtf8("\xF4\x8F\xBF\xBF"));          // U+10FFFF
    CHECK(!base::isValidUtf8("\xC0\x80"));                 // overlong NUL
    CHECK(!base::isValidUtf8("\xE0\x80\x80"));             // overlong
    CHECK(!base::isValidUtf8("\xED\xA0\x80"));             // surrogate
    CHECK(!base::isValidUtf8("\xF4\x90\x80\x80"));         // above U+10FFFF
    CHECK(!base::isValidUtf8("\xF5\x80\x80\x80"));
    CHECK(!base::isValidUtf8("\xC3"));                     // truncated
    CHECK(!base::isValidUtf8("\xE2\x82"));
    CHECK(!base::isValidUtf8("abcdefgh\xF0\x9F\x98"));     // truncated after the ASCII fast path
    CHECK(!base::isValidUtf8("\x80"));                     // lone continuation
}

TEST(utf8MatchesReferenceOnRandomBytes) {
    std::mt19937_64 rng(5);
    const unsigned char interesting[] = {0x00, 0x41, 0x7F, 0x80, 0xBF, 0xC0, 0xC2, 0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF, 0x9F, 0xA0, 0x8F, 0x90};
    for (int iter = 0; iter < 200000; ++iter) {
        std::string s(rng() % 12, '\0');
        for (char& c : s)
            c = static_cast<char>(interesting[rng() % sizeof interesting]);
        CHECK_EQ(base::isValidUtf8(s), referenceUtf8(s));
    }
}

TEST(utf8EncodeCodepointRoundTrip) {
    char buf[4];
    for (std::uint32_t cp : {0u, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xD7FFu, 0xE000u, 0xFFFFu, 0x10000u, 0x10FFFFu}) {
        const std::size_t n = base::encodeCodepoint(cp, buf);
        CHECK(n >= 1 && n <= 4);
        CHECK(base::isValidUtf8(std::string_view(buf, n)));
    }
    CHECK_EQ(base::encodeCodepoint(0xD800, buf), 0u);
    CHECK_EQ(base::encodeCodepoint(0x110000, buf), 0u);
}
int main() { return tst::runAll("encoding"); }
