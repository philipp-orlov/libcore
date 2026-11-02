// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <clocale>
#include <limits>
#include <string>

#include "bench/alloc_budget.hpp"
#include "json/json.hpp"
#include "json/writer.hpp"
#include "mini_test.hpp"

namespace {
std::string sample(json::Writer& w) {
    w.beginObject();
    w.key("name").value("a\"b\\c\n\x01");
    w.key("n").value(-5).key("big").value(std::int64_t{9223372036854775807ll}).key("pi").value(3.5);
    w.key("ok").value(true).key("none").null();
    w.key("list").beginArray().value(1).value("x").beginObject().key("k").value(2).endObject().beginArray().endArray().endArray();
    w.key("pre").raw("{\"already\":[1,2]}");
    w.endObject();
    return {};
}
}  // namespace

TEST(writesWhatTheParserReadsBack) {
    const std::string text = json::toString([](json::Writer& w) { sample(w); });
    json::Json parsed;
    std::string error;
    CHECK(json::Json::parse(text, parsed, error));
    CHECK_EQ(parsed["name"].asString(), std::string("a\"b\\c\n\x01"));
    CHECK_EQ(parsed["big"].dump(), std::string("9223372036854775807"));
    CHECK_EQ(parsed["list"].asArray().size(), 4u);
    CHECK_EQ(parsed["pre"]["already"].asArray().size(), 2u);
    CHECK(text.find("\\u0001") != std::string::npos);
}

TEST(unsigned64IsWrittenExactly) {
    CHECK_EQ(json::toString([](json::Writer& w) { w.value(std::uint64_t{18446744073709551615ull}); }), std::string("18446744073709551615"));
}

TEST(twoPassSizeIsExactAndTruncationIsReported) {
    json::Writer count(nullptr, 0);
    sample(count);
    char exact[4096];
    json::Writer w(exact, count.size());
    sample(w);
    CHECK(!w.truncated());
    CHECK_EQ(w.view().size(), count.size());
    char small[10];
    json::Writer t(small, sizeof small);
    sample(t);
    CHECK(t.truncated());
    CHECK_EQ(t.size(), count.size());   // the would-be length is still right
}

TEST(numbersAreLocaleFreeAndNonFiniteIsNull) {
    auto one = [](double d) { return json::toString([d](json::Writer& w) { w.value(d); }); };
    CHECK_EQ(one(0.1 + 0.2), std::string("0.30000000000000004"));
    CHECK_EQ(one(1e22), std::string("1e+22"));
    CHECK_EQ(one(5e-324), std::string("5e-324"));
    CHECK_EQ(one(-0.0), std::string("-0"));
    CHECK_EQ(one(std::numeric_limits<double>::quiet_NaN()), std::string("null"));
    CHECK_EQ(one(std::numeric_limits<double>::infinity()), std::string("null"));
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        CHECK_EQ(one(2.5), std::string("2.5"));
        std::setlocale(LC_NUMERIC, "C");
    }
}

TEST(structuralMistakesAreReported) {
    char buf[64];
    { json::Writer w(buf, sizeof buf); w.beginObject().value(1); CHECK(!w.good()); }       // value without key
    { json::Writer w(buf, sizeof buf); w.beginArray().endObject(); CHECK(!w.good()); }
    { json::Writer w(buf, sizeof buf); w.endArray(); CHECK(!w.good()); }
    { json::Writer w(buf, sizeof buf); for (int i = 0; i < 70; ++i) w.beginArray(); CHECK(!w.good()); }
}

TEST(writingAllocatesNothing) {
    char buf[4096];
    bench::AllocScope scope;
    for (int i = 0; i < 100000; ++i) {
        json::Writer w(buf, sizeof buf);
        sample(w);
    }
    CHECK_EQ(scope.calls(), 0ull);
}

int main() { return tst::runAll("json_writer"); }
