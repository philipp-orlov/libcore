// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <clocale>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>

#include "base/number.hpp"
#include "mini_test.hpp"

namespace {
std::string digits(const base::ShortestDigits& d) { return std::string(d.digits, std::size_t(d.count)); }
std::string shortest(double v) {
    char out[base::kMaxShortest];
    return std::string(out, base::formatShortest(v, out));
}
double rebuilt(const base::ShortestDigits& d) {
    const std::string text = "0." + digits(d) + "e" + std::to_string(d.pointPosition);
    return std::strtod(text.c_str(), nullptr);
}
}  // namespace

TEST(shortestDigitsAreTheShortestThatRoundTrip) {
    auto d = base::shortestDigits(0.1);
    CHECK_EQ(digits(d), std::string("1"));
    CHECK_EQ(d.pointPosition, 0);
    d = base::shortestDigits(1234.5);
    CHECK_EQ(digits(d), std::string("12345"));
    CHECK_EQ(d.pointPosition, 4);
    d = base::shortestDigits(1e21);
    CHECK_EQ(digits(d), std::string("1"));
    CHECK_EQ(d.pointPosition, 22);
    d = base::shortestDigits(5e-324);
    CHECK_EQ(digits(d), std::string("5"));
    CHECK_EQ(d.pointPosition, -323);
    d = base::shortestDigits(1.7976931348623157e308);
    CHECK_EQ(digits(d), std::string("17976931348623157"));
    d = base::shortestDigits(100.0);
    CHECK_EQ(digits(d), std::string("1"));
    CHECK_EQ(d.pointPosition, 3);
}

TEST(aFloatNeedsFewerDigitsThanTheDoubleItWidensTo) {
    CHECK_EQ(digits(base::shortestDigits(0.1f)), std::string("1"));
    CHECK_EQ(digits(base::shortestDigits(3.14159274f)), std::string("31415927"));
}

TEST(randomDoublesRoundTrip) {
    std::mt19937_64 random(17);
    for (int i = 0; i < 20000; ++i) {
        std::uint64_t bits = random();
        double v;
        std::memcpy(&v, &bits, sizeof v);
        if (!std::isfinite(v) || v == 0)
            continue;
        v = std::fabs(v);
        CHECK(rebuilt(base::shortestDigits(v)) == v);
        const std::string text = shortest(v);
        const base::ParsedDouble back = base::parseDouble(text);
        CHECK_EQ(back.consumed, text.size());
        CHECK(back.value == v);
    }
}

TEST(formatShortestHasToCharsSpelling) {
    CHECK_EQ(shortest(0.1), std::string("0.1"));
    CHECK_EQ(shortest(5e-324), std::string("5e-324"));
    CHECK_EQ(shortest(1e+22), std::string("1e+22"));
    CHECK_EQ(shortest(-2.5), std::string("-2.5"));
}

TEST(parseStopsWhereTheNumberStops) {
    auto check = [](const char* text, double value, std::size_t consumed) {
        const base::ParsedDouble p = base::parseDouble(text);
        CHECK_EQ(p.consumed, consumed);
        if (consumed)
            CHECK(p.value == value);
    };
    check("12.5abc", 12.5, 4);
    check("-3", -3, 2);
    check("+3", 3, 2);
    check(".5", 0.5, 2);
    check("5.", 5, 2);
    check("1e5x", 1e5, 3);
    check("1e", 1, 1);
    check("1e+", 1, 1);
    check("0x10", 0, 1);    // no hex floats, whatever strtod would do
    check("", 0, 0);
    check("abc", 0, 0);
    check(".", 0, 0);
    check("-", 0, 0);
    check(" 1", 0, 0);      // no leading blanks either
    check("Infinityx", std::numeric_limits<double>::infinity(), 8);
    check("-inf", -std::numeric_limits<double>::infinity(), 4);
    CHECK(std::isnan(base::parseDouble("nan").value));
}

TEST(outOfRangeReadsAsStrtodReadsIt) {
    base::ParsedDouble p = base::parseDouble("1e999");
    CHECK(p.outOfRange);
    CHECK_EQ(p.consumed, 5u);
    CHECK(std::isinf(p.value));
    p = base::parseDouble("-1e999");
    CHECK(p.outOfRange);
    CHECK(p.value < 0);
    p = base::parseDouble("1e-999");
    CHECK(p.outOfRange);
    CHECK_EQ(p.value, 0.0);
    p = base::parseDouble("5e-324");   // subnormal is in range
    CHECK(!p.outOfRange);
    CHECK(p.value > 0);
    const std::string longZeros = "1" + std::string(400, '0');
    p = base::parseDouble(longZeros);  // past the stack buffer
    CHECK(p.outOfRange);
    CHECK_EQ(p.consumed, longZeros.size());
}

TEST(theProcessLocaleDoesNotMatter) {
    if (!std::setlocale(LC_ALL, "de_DE.UTF-8") && !std::setlocale(LC_ALL, "fr_FR.UTF-8"))
        return;  // no comma locale on this machine
    CHECK_EQ(shortest(1.5), std::string("1.5"));
    CHECK_EQ(base::parseDouble("1.5").value, 1.5);
    CHECK_EQ(digits(base::shortestDigits(1.5)), std::string("15"));
    std::setlocale(LC_ALL, "C");
}

int main() { return tst::runAll("number"); }
