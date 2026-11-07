// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <string>

#include "base/encoding.hpp"
#include "base/hash.hpp"
#include "mini_test.hpp"

TEST(fnv1aMatchesPublishedVectors) {
    static_assert(base::fnv1a32("") == 0x811c9dc5u);
    static_assert(base::fnv1a32("a") == 0xe40c292cu);
    static_assert(base::fnv1a32("foobar") == 0xbf9cf968u);
    CHECK_EQ(base::fnv1a32("test.everything.Child"), base::fnv1a32(std::string("test.everything.Child")));
}

namespace {
std::string sha256Hex(const std::string& text) {
    uint8_t digest[32];
    base::Sha256::digest(text.data(), text.size(), digest);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}
}  // namespace

TEST(sha256MatchesFipsVectors) {
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256Hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(sha256StreamedInPiecesMatchesOneShot) {
    std::string text;
    for (int i = 0; i < 1000; ++i)
        text.push_back(static_cast<char>(i * 7));
    for (const std::size_t piece : {1u, 3u, 63u, 64u, 65u, 200u}) {
        base::Sha256 hasher;
        for (std::size_t at = 0; at < text.size(); at += piece)
            hasher.update(text.data() + at, std::min(piece, text.size() - at));
        uint8_t streamed[32], oneShot[32];
        hasher.finish(streamed);
        base::Sha256::digest(text.data(), text.size(), oneShot);
        CHECK(std::memcmp(streamed, oneShot, 32) == 0);
    }
}

TEST(crc32cMatchesKnownVectors) {
    CHECK_EQ(base::crc32c("123456789", 9), 0xe3069283u);
    CHECK_EQ(base::crc32cPortable("123456789", 9), 0xe3069283u);
    CHECK_EQ(base::crc32c(nullptr, 0), 0u);
    const unsigned char zeros[32] = {};
    CHECK_EQ(base::crc32c(zeros, 32), 0x8a9136aau);  // RFC 3720 B.4
    unsigned char ones[32];
    std::memset(ones, 0xff, sizeof ones);
    CHECK_EQ(base::crc32c(ones, 32), 0x62a8ab43u);
    unsigned char ascending[32];
    for (int i = 0; i < 32; ++i)
        ascending[i] = static_cast<unsigned char>(i);
    CHECK_EQ(base::crc32c(ascending, 32), 0x46dd794eu);
}

TEST(crc32cKernelAgreesWithPortableAtEveryLengthAndAlignment) {
    const std::string bytes = tst::blob(4096 + 64, 5);
    for (std::size_t offset = 0; offset < 64; offset += 7)
        for (std::size_t size = 0; size <= 4096; size += (size < 300 ? 1 : 97))
            CHECK_EQ(base::crc32c(bytes.data() + offset, size), base::crc32cPortable(bytes.data() + offset, size));
}

TEST(crc32cContinuesFromAPreviousValue) {
    const std::string bytes = tst::blob(1000, 9);
    const uint32_t whole = base::crc32c(bytes.data(), bytes.size());
    for (std::size_t split : {0u, 1u, 7u, 8u, 9u, 500u, 999u, 1000u}) {
        const uint32_t first = base::crc32c(bytes.data(), split);
        CHECK_EQ(base::crc32c(bytes.data() + split, bytes.size() - split, first), whole);
    }
}

TEST(crc32cReportsItsBackend) {
    const std::string name = base::crc32cBackend();
    CHECK(name == "sse4.2" || name == "armv8-crc" || name == "portable");
    std::printf("crc32c backend: %s\n", name.c_str());
}


TEST(hashBytes64MatchesLibsqlGoldens) {
    // Values produced by libsql's original hashBytes before it was lifted.
    struct G { const char* s; uint64_t h; };
    const G goldens[] = {{"", 0x8fcdb8f33ae3211full},          {"a", 0x1bee43fa30aa591bull},
                         {"abc", 0x07d0ad41eb43aac9ull},       {"abcd", 0xad977ea2754b5d08ull},
                         {"abcdefg", 0x032e83540409888eull},   {"abcdefgh", 0x9ecbe134d48b121cull},
                         {"hello world, this is a key", 0x2f205e621b72a950ull}};
    for (const G& g : goldens)
        CHECK_EQ(base::hashBytes64(g.s, std::strlen(g.s)), g.h);
}

TEST(sha1KnownVectors) {
    auto hex = [](const uint8_t* d) { return base::hexEncode(d, 20); };
    uint8_t out[20];
    base::Sha1::digest("", 0, out);
    CHECK_EQ(hex(out), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    base::Sha1::digest("abc", 3, out);
    CHECK_EQ(hex(out), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    base::Sha1::digest(two.data(), two.size(), out);
    CHECK_EQ(hex(out), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    // RFC 6455 section 1.3 example.
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    base::Sha1::digest(key.data(), key.size(), out);
    CHECK_EQ(base::base64Encode(out, 20), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(sha1StreamingEqualsOneShot) {
    const std::string data = tst::blob(1000, 7);
    uint8_t whole[20];
    base::Sha1::digest(data.data(), data.size(), whole);
    for (std::size_t chunk : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 127u, 999u}) {
        base::Sha1 s;
        for (std::size_t i = 0; i < data.size(); i += chunk)
            s.update(data.data() + i, std::min(chunk, data.size() - i));
        uint8_t got[20];
        s.finish(got);
        CHECK(std::memcmp(got, whole, 20) == 0);
    }
}
int main() { return tst::runAll("hash"); }
