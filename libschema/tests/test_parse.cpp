// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <unistd.h>

#include <fstream>
#include <random>
#include <string>

#include "base/hash.hpp"
#include "mini_test.hpp"
#include "schema/parse.hpp"

namespace {
std::string dir() {
    static const std::string d = "/tmp/libschema_test_" + std::to_string(::getpid());
    static bool made = (::system(("mkdir -p " + d).c_str()), true);
    (void)made;
    return d;
}
std::string write(const std::string& name, const std::string& text) {
    const std::string path = dir() + "/" + name;
    std::ofstream(path) << text;
    return path;
}
const char* kGood = R"(
namespace demo.v1;
enum Color : ubyte { Red = 0, Green, Blue }
struct Vec { x: float; y: float; }
table Item { id: uint; name: string; pos: Vec; tags: [string]; color: Color = Green; }
root_type Item;
)";
}  // namespace

TEST(parsesAndLaysOutASchema) {
    const auto r = schema::parse(write("good.bufs", kGood), {});
    CHECK(r.ok());
    CHECK_EQ(r.schema.tables.size(), 1u);
    CHECK_EQ(r.schema.structs.size(), 1u);
    CHECK_EQ(r.schema.enums.size(), 1u);
    CHECK_EQ(r.schema.structs[0]->size, 8u);
    CHECK_EQ(r.schema.rootType, std::string("demo.v1.Item"));
    CHECK(r.schema.findTable("demo.v1.Item") != nullptr);
}

TEST(errorsAreLocatedDiagnostics) {
    const auto r = schema::parse(write("bad.bufs", "namespace n;\ntable T { a: nosuchtype; }\n"), {});
    CHECK(!r.ok());
    CHECK_EQ(r.errors.size(), 1u);
    CHECK(r.errors[0].file.find("bad.bufs") != std::string::npos);
    CHECK_EQ(r.errors[0].line, 2u);
    CHECK(r.errors[0].message.find("nosuchtype") != std::string::npos);
    const auto missing = schema::parse(dir() + "/does_not_exist.bufs", {});
    CHECK(!missing.ok());
}

TEST(includesAreSearchedInIncludeDirs) {
    write("shared.bufs", "namespace shared;\nstruct P { a: int; }\n");
    const std::string main = write("user.bufs", "include \"shared.bufs\";\nnamespace u;\ntable T { p: shared.P; }\n");
    CHECK(schema::parse(main, {}).ok());
    CHECK(schema::parse(main, {dir()}).ok());
    CHECK_EQ(schema::parse(main, {}).schema.files.size(), 2u);
}

TEST(idsAreFnv1aOfTheQualifiedName) {
    CHECK_EQ(schema::typeId("demo.v1.Item"), base::fnv1a32("demo.v1.Item"));
    CHECK_EQ(schema::methodId("a.Svc.Do"), base::fnv1a32("a.Svc.Do"));
    CHECK_EQ(bufsc::typeIdOf("demo.v1.Item"), schema::typeId("demo.v1.Item"));
}

TEST(garbageNeverCrashesTheParser) {
    std::mt19937_64 rng(9);
    const std::string seed = kGood;
    for (int i = 0; i < 3000; ++i) {
        std::string m = seed;
        for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) {
            const std::size_t at = rng() % m.size();
            if (rng() % 2) m[at] = static_cast<char>(rng()); else m.erase(at, 1 + rng() % 3);
        }
        (void)schema::parse(write("fuzz.bufs", m), {});   // must return, not throw or crash
    }
}

int main() { return tst::runAll("schema"); }
