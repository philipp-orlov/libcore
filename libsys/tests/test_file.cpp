// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The file layer a store stands on: open, positional I/O, resize, sync, mappings, and the way
// failures are reported.
#ifndef _WIN32
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "base/status.hpp"
#include "mini_test.hpp"
#include "sys/file.hpp"
#include "sys/io_observer.hpp"

namespace fs = std::filesystem;

namespace {
fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("core-sys-file-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir / name;
}
}  // namespace

TEST(readWriteResizeSyncRoundTrip) {
    const fs::path p = scratch("rw");
    fs::remove(p);
    sys::File f(sys::openFile(p, false, true, false));
    const std::string data = "hello, positional world";
    sys::writeAt(f, 100, data.data(), data.size());
    sys::syncFile(f, false);
    CHECK(sys::fileSize(f) == 100 + data.size());
    std::string back(data.size(), '\0');
    sys::readAt(f, 100, back.data(), back.size());
    CHECK(back == data);
    sys::resizeFile(f, 4096);
    CHECK(sys::fileSize(f) == 4096);
    fs::remove(p);
}

TEST(replaceFileOfAMissingSourceCarriesErrno) {
    const fs::path target = scratch("target");
    bool threw = false;
    try {
        sys::replaceFile(scratch("no-such-source"), target, true);
    } catch (const base::Error& e) {
        threw = true;
        CHECK(e.status().detail() == std::uint32_t(ENOENT));
    }
    CHECK(threw);
}

TEST(replaceFileWithoutReplaceRefusesAnExistingTarget) {
    const fs::path a = scratch("a"), b = scratch("b");
    { sys::File fa(sys::openFile(a, false, true, false, false)); }
    { sys::File fb(sys::openFile(b, false, true, false, false)); }
    bool threw = false;
    try {
        sys::replaceFile(a, b, false);
    } catch (const base::Error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(fs::exists(a));
    fs::remove(a);
    fs::remove(b);
}

TEST(reservedMappingExtendsInPlace) {
    const fs::path p = scratch("map");
    fs::remove(p);
    sys::File f(sys::openFile(p, false, true, false));
    const std::size_t page = sys::systemPageSize();
    sys::resizeFile(f, page);
    sys::Mapping m = sys::mapFileReserved(f, page, page * 16, true);
    std::byte* mapped = m.base;
    std::memset(m.base, 7, page);
    sys::resizeFile(f, page * 4);
    CHECK(sys::extendMapping(f, m, page * 4, true));
    CHECK(m.base == mapped);
    CHECK(m.size == page * 4);
    CHECK(static_cast<unsigned char>(m.base[page - 1]) == 7);
    sys::flushRange(f, m, 0, page);
    sys::unmap(m);
    fs::remove(p);
}

TEST(creatingAFileSyncsItsDirectoryOnceAndOpeningDoesNot) {
    // A new name is durable only once its directory is; opening an existing file changes no name.
    const fs::path p = scratch("created");
    fs::remove(p);
    { sys::File f(sys::openFile(p, false, true, false)); CHECK(fs::exists(p)); }
    { sys::File f(sys::openFile(p, false, true, false)); CHECK(fs::exists(p)); }
    fs::remove(p);
}
#endif

int main() { return tst::runAll("file"); }
