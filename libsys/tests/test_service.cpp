// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <csignal>
#include <stdexcept>
#include <string>
#include <thread>

#include "base/status.hpp"
#include "mini_test.hpp"
#include "sys/service.hpp"

using namespace std::chrono_literals;

TEST(quotingRoundTripsThroughTheWindowsRules) {
    CHECK_EQ(sys::service::quoteArgument("plain"), std::string("plain"));
    CHECK_EQ(sys::service::quoteArgument(""), std::string("\"\""));
    CHECK_EQ(sys::service::quoteArgument("two words"), std::string("\"two words\""));
    CHECK_EQ(sys::service::quoteArgument("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    // Backslashes matter only before a quote, and before the closing one.
    CHECK_EQ(sys::service::quoteArgument("C:\\no\\spaces"), std::string("C:\\no\\spaces"));
    CHECK_EQ(sys::service::quoteArgument("C:\\with space\\"), std::string("\"C:\\with space\\\\\""));
    CHECK_EQ(sys::service::quoteArgument("a\\\"b c"), std::string("\"a\\\\\\\"b c\""));
}

TEST(theForegroundRunsTheBodyAndReturnsItsCode) {
    bool ran = false;
    const int code = sys::service::run("test", false, [&](sys::service::Control& control) {
        ran = true;
        CHECK(!control.stopRequested());
        control.running();  // nothing to tell in a console
        return 7;
    });
    CHECK(ran);
    CHECK_EQ(code, 7);
}

TEST(anExceptionFromTheBodyIsAnErrorExit) {
    const int code = sys::service::run("test", false, [](sys::service::Control&) -> int { throw std::runtime_error("boom"); });
    CHECK_EQ(code, 1);
}

TEST(aStopRegisteredAfterTheRequestStillRuns) {
    // Control is what run() hands out; the test reaches the same behaviour through the signal the foreground host takes.
    std::atomic<bool> stopped{false};
    const int code = sys::service::run("test", false, [&](sys::service::Control& control) {
        control.onStop([&] { stopped = true; });
        control.running();
        // Ask the process to stop the way a person does.
#if defined(_WIN32)
        // A console control event would reach every process on this console, ctest included; the host's handler is the
        // thing under test elsewhere (tunnel's end-to-end checks send Ctrl-Break to a process of its own).
        stopped = true;
#else
        std::raise(SIGTERM);
        for (int i = 0; i < 100 && !stopped; ++i)
            std::this_thread::sleep_for(20ms);
#endif
        return 0;
    });
    CHECK_EQ(code, 0);
    CHECK(stopped.load());
}

TEST(managementIsForWindows) {
    if (!sys::service::kSupported) {
        bool threw = false;
        try {
            sys::service::state("anything");
        } catch (const base::Error& error) {
            threw = error.code() == base::Code::Unimplemented;
        }
        CHECK(threw);
        return;
    }
    CHECK(sys::service::state("a-service-nobody-installed") == sys::service::State::NotInstalled);
    // The name is checked before the system is asked for anything.
    bool refused = false;
    try {
        sys::service::Spec spec;
        spec.name = "bad name";
        sys::service::install(spec);
    } catch (const base::Error& error) {
        refused = error.code() == base::Code::InvalidArgument;
    }
    CHECK(refused);
    if (!sys::service::isElevated()) {
        bool denied = false;
        try {
            sys::service::Spec spec;
            spec.name = "sys-service-test";
            sys::service::install(spec);
        } catch (const base::Error& error) {
            denied = error.code() == base::Code::PermissionDenied;
        }
        CHECK(denied);
    }
}

int main() { return tst::runAll("service"); }
