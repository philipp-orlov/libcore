// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstring>

#include "base/status.hpp"

#include "mini_test.hpp"
#include "sys/errno.hpp"
#include "sys/proc.hpp"
#include "sys/shm.hpp"
#include "sys/vm.hpp"

TEST(mapIsZeroedPageAlignedAndRounded) {
    const std::size_t page = sys::pageSize();
    sys::VmRegion r = sys::vmMap(1);
    CHECK(r.base != nullptr);
    CHECK_EQ(r.bytes, page);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(r.base) % page, 0u);
    for (std::size_t i = 0; i < r.bytes; ++i)
        CHECK_EQ(static_cast<unsigned char*>(r.base)[i], 0);
    sys::vmUnmap(r);
}

TEST(mapAlignedHonoursLargeAlignment) {
    const std::size_t align = std::size_t{2} << 20;
    for (int i = 0; i < 4; ++i) {
        sys::VmRegion r = sys::vmMapAligned(align, align);
        CHECK(r.base != nullptr);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(r.base) % align, 0u);
        std::memset(r.base, 0x5a, r.bytes);  // every page really is mapped
        sys::vmUnmap(r);
    }
}

TEST(hugeRequestFailsCleanly) {
    CHECK(!sys::tryVmMap(~std::size_t{0} - 100).base);
    bool threw = false;
    try {
        sys::vmMap(std::size_t{1} << 62);
    } catch (const base::Error& e) {
        threw = e.code() == base::Code::ResourceExhausted;
    }
    CHECK(threw);
}

TEST(discardReturnsPagesToTheOs) {
    const std::size_t page = sys::pageSize();
    const std::size_t pages = 4096;  // 16 MiB
    sys::VmRegion r = sys::vmMap(pages * page);
    std::memset(r.base, 1, r.bytes);
    const std::size_t before = sys::residentBytes();
    sys::vmDiscard(r.base, r.bytes);
    const std::size_t after = sys::residentBytes();
    CHECK(before >= after + (pages - 64) * page);
    CHECK_EQ(static_cast<unsigned char*>(r.base)[page * 17], 0);  // reads as zero again
    sys::vmUnmap(r);
}

TEST(prefaultMakesSharedMemoryResident) {
    // A shared memfd mapping (what rings and arenas use): resident after vmPrefault, before anything writes to it.
    const std::size_t bytes = std::size_t{8} << 20;
    sys::SharedMemory shm = sys::SharedMemory::create("prefault-test", bytes);
    const std::size_t before = sys::residentBytes();
    sys::vmPrefault(shm.data(), shm.size());
    const std::size_t after = sys::residentBytes();
    CHECK(after >= before + bytes - (std::size_t{256} << 10));
    CHECK_EQ(static_cast<unsigned char*>(shm.data())[12345], 0);   // contents untouched
    sys::vmPrefault(nullptr, 0);                                    // no-op
}

TEST(discardNeverTouchesPartialPages) {
    const std::size_t page = sys::pageSize();
    sys::VmRegion r = sys::vmMap(4 * page);
    auto* b = static_cast<unsigned char*>(r.base);
    std::memset(b, 7, r.bytes);
    // Range starts 100 bytes into page 0 and ends 100 bytes before the end of page 3: only pages 1 and 2 may go.
    sys::vmDiscard(b + 100, 4 * page - 200);
    CHECK_EQ(b[0], 7);
    CHECK_EQ(b[page - 1], 7);       // page 0 untouched
    CHECK_EQ(b[page], 0);           // page 1 discarded
    CHECK_EQ(b[3 * page - 1], 0);   // page 2 discarded
    CHECK_EQ(b[3 * page], 7);       // page 3 untouched
    sys::vmDiscard(b, 10);          // sub-page range: nothing happens
    CHECK_EQ(b[0], 7);
    sys::vmUnmap(r);
}

TEST(reserveCommit) {
    sys::VmRegion r = sys::vmReserve(1 << 20);
    sys::vmCommit(r.base, 1 << 16);
    static_cast<char*>(r.base)[100] = 42;
    CHECK_EQ(static_cast<char*>(r.base)[100], 42);
    sys::vmUnmap(r);
}

TEST(errnoMessageIsThreadSafeText) {
    CHECK(sys::errnoMessage(ENOENT).find("No such file") != std::string::npos);
    CHECK(sys::errnoMessage("open", ENOENT).rfind("open: ", 0) == 0);
}

int main() { return tst::runAll("vm"); }
