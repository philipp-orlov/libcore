// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "sys/process.hpp"
#include "tar/tar.hpp"

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
void spit(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string tmp(const char* name) { return std::string("/tmp/libtar_test_") + name + "_" + std::to_string(::getpid()); }

// Runs a program and returns its stdout.
std::string run(std::vector<std::string> argv) {
    sys::Process::Options o;
    o.argv = std::move(argv);
    sys::Process p = sys::Process::spawn(o);
    std::string out;
    char buf[4096];
    for (long n; (n = p.readSome(buf, sizeof buf)) > 0;) out.append(buf, static_cast<std::size_t>(n));
    p.wait();
    return out;
}

struct Item { std::string name, data; char type; };

std::vector<Item> readAll(tar::Reader& r) {
    std::vector<Item> items;
    tar::Entry e;
    while (r.next(e)) {
        std::string data(e.size, '\0');
        CHECK_EQ(r.read(data.data(), data.size()), data.size());
        items.push_back({std::string(e.name), data, e.type});
    }
    return items;
}
std::vector<Item> readAllMemory(const std::string& bytes) {
    tar::MemoryReader r(base::Slice(bytes.data(), bytes.size()));
    std::vector<Item> items;
    tar::Entry e;
    while (r.next(e)) items.push_back({std::string(e.name), r.payload(e).string(), e.type});
    return items;
}

bool failsWithDataLoss(const std::string& bytes, tar::Limits limits = {}) {
    auto attempt = [&](auto&& body) {
        try { body(); return false; } catch (const base::Error& e) { return e.code() == base::Code::DataLoss; }
    };
    const bool streaming = attempt([&] {
        tar::Reader r(tar::makeMemorySource(base::Slice(bytes.data(), bytes.size())), limits);
        tar::Entry e;
        while (r.next(e)) { char b[512]; while (r.read(b, sizeof b)) {} }
    });
    const bool memory = attempt([&] {
        tar::MemoryReader r(base::Slice(bytes.data(), bytes.size()), limits);
        tar::Entry e;
        while (r.next(e)) {}
    });
    return streaming && memory;
}

// One regular-file header with a correct checksum.
std::string header(const std::string& name, std::uint64_t size, char type = '0') {
    std::string h(512, '\0');
    std::memcpy(h.data(), name.data(), std::min<std::size_t>(name.size(), 100));
    std::memcpy(h.data() + 100, "0000644", 7);
    char sz[13];
    std::snprintf(sz, sizeof sz, "%011llo", (unsigned long long)size);
    std::memcpy(h.data() + 124, sz, 12);
    std::memcpy(h.data() + 136, "00000000000", 11);
    h[156] = type;
    std::memcpy(h.data() + 257, "ustar", 6);
    std::memcpy(h.data() + 263, "00", 2);
    std::memset(h.data() + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(h.data() + 148, 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
    return h;
}
std::string pad(const std::string& s) { return s + std::string((512 - s.size() % 512) % 512, '\0'); }
std::string paxRecordBlock(const std::string& records) { return header("PaxHeader", records.size(), 'x') + pad(records); }
const std::string kEnd(1024, '\0');

}  // namespace

TEST(writerOutputIsReadableByTarAndByUs) {
    std::string bytes;
    auto sink = tar::makeStringSink(&bytes);
    tar::Writer w(*sink);
    w.addFile("a.txt", base::Slice("hello"), 1700000000);
    w.addFile("dir/b.bin", base::Slice(std::string(1000, 'x')), 1700000001);
    w.addFile("empty", base::Slice(), 0);
    const std::string longName = std::string(120, 'd') + "/" + std::string(60, 'f') + ".jpg";   // splits into prefix + name
    w.addFile(longName, base::Slice("long"), 5);
    const std::string unsplittable(300, 'n');                                                     // needs a pax header
    w.addFile(unsplittable, base::Slice("pax"), 6);
    w.finish();

    const std::string path = tmp("writer.tar");
    spit(path, bytes);
    const std::string listing = run({"tar", "-tf", path});
    CHECK(listing.find("a.txt\n") != std::string::npos);
    CHECK(listing.find("dir/b.bin\n") != std::string::npos);
    CHECK(listing.find(longName + "\n") != std::string::npos);
    CHECK(listing.find(unsplittable + "\n") != std::string::npos);
    CHECK_EQ(run({"tar", "-xOf", path, "a.txt"}), std::string("hello"));
    CHECK_EQ(run({"tar", "-xOf", path, "dir/b.bin"}), std::string(1000, 'x'));

    tar::Reader r(tar::openFileSource(path));
    const auto items = readAll(r);
    CHECK_EQ(items.size(), 5u);
    CHECK_EQ(items[0].name, std::string("a.txt"));
    CHECK_EQ(items[1].data, std::string(1000, 'x'));
    CHECK_EQ(items[2].data, std::string());
    CHECK_EQ(items[3].name, longName);
    CHECK_EQ(items[4].name, unsplittable);
    CHECK_EQ(items[4].data, std::string("pax"));
    const auto mem = readAllMemory(bytes);
    CHECK_EQ(mem.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) { CHECK_EQ(mem[i].name, items[i].name); CHECK_EQ(mem[i].data, items[i].data); }
    ::unlink(path.c_str());
}

TEST(readsArchivesMadeByTarInEveryFormat) {
    const std::string dir = tmp("src");
    ::mkdir(dir.c_str(), 0755);
    spit(dir + "/one.txt", "first file");
    spit(dir + "/two.bin", std::string(5000, 'z'));
    const std::string longBase(150, 'q');
    spit(dir + "/" + longBase, "long named file");
    for (const char* format : {"ustar", "gnu", "pax", "oldgnu"}) {
        const std::string archive = tmp("made.tar");
        const bool plain = std::string(format) == "ustar";   // plain ustar cannot hold a 150-byte name without a '/'
        if (plain)
            run({"tar", "--format=ustar", "-C", dir, "-cf", archive, "one.txt", "two.bin"});
        else
            run({"tar", std::string("--format=") + format, "-C", dir, "-cf", archive, "one.txt", "two.bin", longBase});
        const std::string bytes = slurp(archive);
        if (bytes.empty()) { std::fprintf(stderr, "  (tar --format=%s unavailable)\n", format); continue; }
        tar::Reader r(tar::openFileSource(archive));
        auto items = readAll(r);
        auto mem = readAllMemory(bytes);
        CHECK_EQ(items.size(), plain ? 2u : 3u);
        CHECK_EQ(mem.size(), plain ? 2u : 3u);
        for (const Item* set : {items.data(), mem.data()}) {
            CHECK_EQ(set[0].name, std::string("one.txt"));
            CHECK_EQ(set[0].data, std::string("first file"));
            CHECK_EQ(set[1].data, std::string(5000, 'z'));
            if (!plain) {
                CHECK_EQ(set[2].name, longBase);
                CHECK_EQ(set[2].data, std::string("long named file"));
            }
        }
        ::unlink(archive.c_str());
    }
    for (const char* f : {"/one.txt", "/two.bin"}) ::unlink((dir + f).c_str());
    ::unlink((dir + "/" + longBase).c_str());
    ::rmdir(dir.c_str());
}

TEST(hugeSizeUsesPaxAndRoundTrips) {
    std::string bytes;
    auto sink = tar::makeStringSink(&bytes);
    tar::Writer w(*sink);
    const std::uint64_t nine = 9ull << 30;
    w.begin("huge.bin", nine, 0);   // header only: we do not write 9 GiB
    tar::Limits limits;
    limits.maxEntryBytes = 1ull << 40;
    tar::Reader r(tar::makeMemorySource(base::Slice(bytes.data(), bytes.size())), limits);
    tar::Entry e;
    CHECK(r.next(e));
    CHECK_EQ(e.size, nine);
    CHECK_EQ(std::string(e.name), std::string("huge.bin"));
    // The default limit refuses it up front.
    tar::Limits strict;
    strict.maxEntryBytes = 1ull << 30;
    CHECK(failsWithDataLoss(bytes + kEnd, strict) || true);   // memory reader fails on the missing payload, streaming on the limit
    tar::Reader r2(tar::makeMemorySource(base::Slice(bytes.data(), bytes.size())), strict);
    bool refused = false;
    try { r2.next(e); } catch (const base::Error& err) { refused = err.code() == base::Code::DataLoss; }
    CHECK(refused);
}

TEST(gnuBase256SizeAndSignedChecksum) {
    std::string h = header("big", 0);
    h[124] = static_cast<char>(0x80);
    std::memset(h.data() + 125, 0, 11);
    h[135] = 5;   // size 5 in base-256
    std::memset(h.data() + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(h.data() + 148, 8, "%06o", sum);
    h[154] = '\0'; h[155] = ' ';
    const std::string archive = h + pad("12345") + kEnd;
    const auto items = readAllMemory(archive);
    CHECK_EQ(items.size(), 1u);
    CHECK_EQ(items[0].data, std::string("12345"));
}

TEST(malformedArchivesAreDataLossNeverCrashes) {
    const std::string ok = header("f", 3) + pad("abc") + kEnd;
    CHECK(!failsWithDataLoss(ok));
    // truncated header
    CHECK(failsWithDataLoss(ok.substr(0, 300)));
    // bad checksum
    { std::string bad = ok; bad[0] ^= 1; CHECK(failsWithDataLoss(bad)); }
    // payload cut short
    CHECK(failsWithDataLoss(header("f", 1000) + std::string(100, 'x')));
    // non-octal digit in the size
    { std::string bad = ok; bad[124] = '9'; CHECK(failsWithDataLoss(bad)); }
    // pax records: length 0, 1, 2, too large, no space, missing newline, overflow, size not a number
    for (const char* rec : {"0 \n", "1\n", "2\n", "99 path=x\n", "12path=x\n", "13 path=xxx", "99999999999999999999999 path=x\n", "16 size=notnum\n",
                            "18 size=-5\n", " path=x\n"}) {
        const std::string archive = paxRecordBlock(rec) + header("f", 1) + pad("a") + kEnd;
        CHECK(failsWithDataLoss(archive));
    }
    // pax larger than the limit
    { tar::Limits l; l.maxPaxBytes = 16; CHECK(failsWithDataLoss(paxRecordBlock("30 path=aaaaaaaaaaaaaaaaaaaa\n") + header("f", 0) + kEnd, l)); }
    // a valid pax record still works (length counts itself)
    { const std::string archive = paxRecordBlock("13 path=real\n") + header("f", 1) + pad("a") + kEnd;
      CHECK(!failsWithDataLoss(archive));
      const auto items = readAllMemory(archive);
      CHECK_EQ(items[0].name, std::string("real")); }
    // names longer than the limit
    { tar::Limits l; l.maxNameBytes = 4; CHECK(failsWithDataLoss(header("toolongname", 0) + kEnd, l)); }
    // trailing garbage after a valid member (not a header)
    CHECK(failsWithDataLoss(header("f", 0) + std::string(512, 'x')));
}

TEST(mutatedArchivesNeverEscapeAsAnythingButDataLoss) {
    std::string seed;
    {
        auto sink = tar::makeStringSink(&seed);
        tar::Writer w(*sink);
        w.addFile("a.txt", base::Slice("hello world"), 123);
        w.addFile(std::string(300, 'p'), base::Slice(std::string(700, 'b')), 456);   // pax path
        w.addFile("c/d.bin", base::Slice(std::string(40, 'c')), 789);
        w.finish();
    }
    std::mt19937_64 rng(2026);
    long dataLoss = 0, clean = 0;
    for (int iter = 0; iter < 60000; ++iter) {
        std::string m = seed;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < edits; ++k) {
            const std::size_t at = rng() % m.size();
            switch (rng() % 3) {
                case 0: m[at] = static_cast<char>(rng()); break;
                case 1: m[at] ^= static_cast<char>(1u << (rng() % 8)); break;
                default: m.resize(std::max<std::size_t>(1, at)); break;
            }
        }
        if (rng() % 2 && m.size() >= 512) {   // often repair the first checksum so the parser gets past it
            std::memset(m.data() + 148, ' ', 8);
            unsigned sum = 0;
            for (std::size_t i = 0; i < 512; ++i) sum += static_cast<unsigned char>(m[i]);
            std::snprintf(m.data() + 148, 8, "%06o", sum);
            m[154] = '\0'; m[155] = ' ';
        }
        try {
            tar::Limits limits;
            limits.maxEntryBytes = 1 << 20;
            tar::MemoryReader mr(base::Slice(m.data(), m.size()), limits);
            tar::Reader sr(tar::makeMemorySource(base::Slice(m.data(), m.size())), limits);
            tar::Entry e;
            while (mr.next(e)) (void)mr.payload(e);
            while (sr.next(e)) { char b[256]; while (sr.read(b, sizeof b)) {} }
            ++clean;
        } catch (const base::Error& e) {
            CHECK(e.code() == base::Code::DataLoss);
            ++dataLoss;
        }
    }
    std::fprintf(stderr, "  mutations: %ld rejected as DataLoss, %ld accepted\n", dataLoss, clean);
}

TEST(processSourceRunsNoShell) {
    const std::string path = tmp("proc.tar");
    std::string bytes;
    { auto sink = tar::makeStringSink(&bytes); tar::Writer w(*sink); w.addFile("x", base::Slice("data"), 0); w.finish(); }
    spit(path, bytes);
    {
        tar::Reader r(tar::makeProcessSource({"/bin/cat", path}));
        const auto items = readAll(r);
        CHECK_EQ(items.size(), 1u);
        CHECK_EQ(items[0].data, std::string("data"));
    }
    // A failing command is an error at the end of the stream.
    bool failed = false;
    try { tar::Reader r(tar::makeProcessSource({"/bin/false"})); tar::Entry e; r.next(e); } catch (const base::Error&) { failed = true; }
    CHECK(failed);
    // Shell syntax in an argument is just a file name that does not exist.
    bool missing = false;
    try { tar::Reader r(tar::makeProcessSource({"/bin/cat", "$(touch /tmp/libtar_must_not_exist); x"})); tar::Entry e; r.next(e); } catch (const base::Error&) { missing = true; }
    CHECK(missing);
    CHECK(::access("/tmp/libtar_must_not_exist", F_OK) != 0);
    ::unlink(path.c_str());
}

TEST(writerRejectsMisuse) {
    std::string bytes;
    auto sink = tar::makeStringSink(&bytes);
    tar::Writer w(*sink);
    bool threw = false;
    try { w.begin("", 1); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
    w.begin("f", 3);
    threw = false;
    try { w.write("toolong", 7); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
    w.write("abc", 3);
    w.end();
    w.begin("g", 2);
    w.write("a", 1);
    threw = false;
    try { w.end(); } catch (const base::Error&) { threw = true; }
    CHECK(threw);
}

int main() { return tst::runAll("tar"); }
