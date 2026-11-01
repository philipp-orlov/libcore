// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A very small test harness: TEST(name), CHECK, CHECK_EQ, runAll(). No dependency, works on any C++17 toolchain.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace tst {

struct TestCase {
    const char* name;
    void (*run)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

inline int& failures() {
    static int count = 0;
    return count;
}

inline void report(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "%s:%d: %s\n", file, line, message.c_str());
    ++failures();
}

template <class T>
std::string show(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string blob(std::size_t size, std::uint64_t seed) {
    std::string bytes(size, '\0');
    std::mt19937_64 random(seed);
    for (char& b : bytes)
        b = static_cast<char>('a' + random() % 26);
    return bytes;
}

inline int runAll(const char* suite) {
    for (const TestCase& test : registry()) {
        const int before = failures();
        try {
            test.run();
        } catch (const std::exception& error) {
            report(test.name, 0, std::string("threw: ") + error.what());
        }
        std::printf("[%s] %s %s\n", suite, failures() == before ? "ok  " : "FAIL", test.name);
    }
    return failures() == 0 ? 0 : 1;
}

}  // namespace tst

#define TEST(name)                                             \
    static void name();                                        \
    static ::tst::Registrar registrar_##name(#name, &name);    \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) ::tst::report(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);                                                                            \
        const auto vb_ = (b);                                                                            \
        if (!(va_ == vb_))                                                                               \
            ::tst::report(__FILE__, __LINE__,                                                            \
                          "CHECK_EQ(" #a ", " #b ") got " + ::tst::show(va_) + " vs " + ::tst::show(vb_)); \
    } while (0)
