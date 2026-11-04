// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A very small test harness: TEST(name), CHECK, CHECK_EQ, runAll(). No dependency, works on any C++17 toolchain.
// A failed check is counted and the test goes on; define TST_FAIL_FAST before including this to end the test instead,
// for tests whose later steps depend on what was just checked.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <random>
#include <sstream>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
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

struct Failed {};

// Reports and ends the test, whichever mode the checks are in.
[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    report(file, line, message);
    throw Failed{};
}

#if defined(TST_FAIL_FAST)
#define TST_REPORT ::tst::fail
#else
#define TST_REPORT ::tst::report
#endif

template <class T, class = void>
struct IsPrintable : std::false_type {};
template <class T>
struct IsPrintable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>> : std::true_type {};

template <class T>
std::string show(const T& value) {
    if constexpr (IsPrintable<T>::value) {
        std::ostringstream out;
        out << std::boolalpha << value;
        return out.str();
    } else {
        return "<value>";
    }
}

inline std::string blob(std::size_t size, std::uint64_t seed) {
    std::string bytes(size, '\0');
    std::mt19937_64 random(seed);
    for (char& b : bytes)
        b = static_cast<char>('a' + random() % 26);
    return bytes;
}

// `filter`: run only the tests whose name contains it.
inline int runAll(const char* suite, const char* filter = nullptr) {
    for (const TestCase& test : registry()) {
        if (filter && !std::strstr(test.name, filter))
            continue;
        const int before = failures();
        try {
            test.run();
        } catch (const Failed&) {
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
        if (!(cond)) TST_REPORT(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);                                                                            \
        const auto vb_ = (b);                                                                            \
        if (!(va_ == vb_))                                                                               \
            TST_REPORT(__FILE__, __LINE__,                                                               \
                          "CHECK_EQ(" #a ", " #b ") got " + ::tst::show(va_) + " vs " + ::tst::show(vb_)); \
    } while (0)
