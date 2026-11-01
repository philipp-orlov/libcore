// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <clocale>
#include <cstdlib>
#include <string>
#include <vector>

#include "cfg/cfg.hpp"
#include "mini_test.hpp"

namespace {
struct Opts {
    std::uint16_t port = 0;
    int threads = 0;
    std::size_t history = 0;
    double interval = 0;
    std::string mode, name;
    bool verbose = false;
    cfg::Schema schema;
    Opts() {
        schema.addInt(port, "port", std::uint16_t{8080}, std::uint16_t{1}, std::uint16_t{65535}, "listen port");
        schema.addInt(threads, "io-threads", 2, 1, 256);
        schema.addInt(history, "history", std::size_t{600}, std::size_t{1}, std::size_t{100000});
        schema.addDouble(interval, "interval", 1.0, 0.1, 3600.0);
        schema.addChoice(mode, "mode", "fast", {"fast", "safe"});
        schema.addString(name, "name", "svc");
        schema.addBool(verbose, "verbose", false);
    }
    base::Status argv(std::vector<const char*> a) {
        a.insert(a.begin(), "prog");
        return schema.fromArgv(static_cast<int>(a.size()), a.data());
    }
};
bool rejects(const base::Status& s, const char* mention) {
    return !s.ok() && s.code() == base::Code::InvalidArgument && std::string(s.message()).find(mention) != std::string::npos;
}
}  // namespace

TEST(defaultsApplyAndFormsAreEquivalent) {
    Opts o;
    CHECK_EQ(o.port, 8080);
    CHECK_EQ(o.mode, std::string("fast"));
    CHECK(o.argv({"--port", "9000", "--io-threads=8", "history=5000", "--verbose", "--mode", "safe", "--name=a b"}).ok());
    CHECK_EQ(o.port, 9000);
    CHECK_EQ(o.threads, 8);
    CHECK_EQ(o.history, 5000u);
    CHECK(o.verbose);
    CHECK_EQ(o.mode, std::string("safe"));
    CHECK_EQ(o.name, std::string("a b"));
    CHECK(o.argv({"--no-verbose"}).ok());
    CHECK(!o.verbose);
}

TEST(rangesAreEnforcedForEveryInteger) {
    Opts o;
    CHECK(rejects(o.argv({"--port", "70000"}), "port"));
    CHECK(rejects(o.argv({"--port", "0"}), "port"));
    CHECK(rejects(o.argv({"--port=-1"}), "port"));
    CHECK(rejects(o.argv({"--port", "99999999999999999999"}), "port"));
    CHECK(rejects(o.argv({"--port", "12x"}), "port"));
    CHECK(rejects(o.argv({"--port", ""}), "port"));
    CHECK(rejects(o.argv({"--io-threads", "0"}), "io-threads"));
    CHECK(rejects(o.argv({"--io-threads", "257"}), "io-threads"));
    CHECK(rejects(o.argv({"--history", "-5"}), "history"));
    CHECK(rejects(o.argv({"--history", "100001"}), "history"));
    CHECK(o.argv({"--port", "65535"}).ok());
    CHECK_EQ(o.port, 65535);
}

TEST(doublesChoicesAndUnknownOptions) {
    Opts o;
    CHECK(rejects(o.argv({"--interval", "0.01"}), "interval"));
    CHECK(rejects(o.argv({"--interval", "nan"}), "interval"));
    CHECK(rejects(o.argv({"--interval", "1,5"}), "interval"));
    CHECK(rejects(o.argv({"--interval", "inf"}), "interval"));
    CHECK(o.argv({"--interval", "2.5"}).ok());
    CHECK_EQ(o.interval, 2.5);
    CHECK(rejects(o.argv({"--mode", "turbo"}), "fast|safe"));
    CHECK(rejects(o.argv({"--bogus", "1"}), "unknown option"));
    CHECK(rejects(o.argv({"--port"}), "needs a value"));
    CHECK(rejects(o.argv({"--verbose=maybe"}), "boolean"));
}

TEST(localeDoesNotChangeNumberParsing) {
    if (!std::setlocale(LC_NUMERIC, "de_DE.UTF-8"))
        return;   // locale not installed: nothing to prove
    Opts o;
    CHECK(o.argv({"--interval", "2.5"}).ok());
    CHECK_EQ(o.interval, 2.5);
    CHECK(rejects(o.argv({"--interval", "2,5"}), "interval"));
    std::setlocale(LC_NUMERIC, "C");
}

TEST(environmentAndQuerySources) {
    ::setenv("GSCOPE_PORT", "7777", 1);
    ::setenv("GSCOPE_IO_THREADS", "4", 1);
    Opts o;
    CHECK(o.schema.fromEnv("GSCOPE_").ok());
    CHECK_EQ(o.port, 7777);
    CHECK_EQ(o.threads, 4);
    ::setenv("GSCOPE_PORT", "70000", 1);
    const auto st = o.schema.fromEnv("GSCOPE_");
    CHECK(rejects(st, "PORT"));
    CHECK(std::string(st.message()).find("GSCOPE_PORT") == 0);   // names the variable, not a flag
    ::unsetenv("GSCOPE_PORT");
    ::unsetenv("GSCOPE_IO_THREADS");

    Opts q;
    CHECK(q.schema.fromQuery("port=81&name=hello%20world&unknown=1&mode=safe&verbose").ok());
    CHECK_EQ(q.port, 81);
    CHECK_EQ(q.name, std::string("hello world"));
    CHECK_EQ(q.mode, std::string("safe"));
    CHECK(q.verbose);
    CHECK(rejects(q.schema.fromQuery("port=abc"), "port"));
}

TEST(requiredAndPositionalAndHelp) {
    std::string path;
    int level = 0;
    cfg::Schema s;
    s.addString(path, "input", "").required();
    s.addInt(level, "level", 3, 0, 9);
    const char* argv1[] = {"prog", "file.txt", "--level", "5"};
    CHECK(s.fromArgv(4, argv1).ok());
    CHECK_EQ(s.positional().size(), 1u);
    CHECK_EQ(s.positional()[0], std::string("file.txt"));
    CHECK(rejects(s.validate(), "input"));
    CHECK(s.set("input", "x").ok());
    CHECK(s.validate().ok());
    const std::string help = s.help();
    CHECK(help.find("--level <0..9>") != std::string::npos);
    CHECK(help.find("(required)") != std::string::npos);
}

int main() { return tst::runAll("cfg"); }
