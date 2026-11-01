// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string>

#include "base/hash.hpp"
#include "mini_test.hpp"

TEST(fnv1aMatchesPublishedVectors) {
    static_assert(base::fnv1a32("") == 0x811c9dc5u);
    static_assert(base::fnv1a32("a") == 0xe40c292cu);
    static_assert(base::fnv1a32("foobar") == 0xbf9cf968u);
    CHECK_EQ(base::fnv1a32("test.everything.Child"), base::fnv1a32(std::string("test.everything.Child")));
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

int main() { return tst::runAll("hash"); }
