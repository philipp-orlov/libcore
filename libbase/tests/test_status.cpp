// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <type_traits>

#include "base/status.hpp"
#include "mini_test.hpp"

TEST(statusIsSmallTrivialAndTruncatesLongMessages) {
    static_assert(sizeof(base::Status) == 96);
    static_assert(std::is_trivially_copyable_v<base::Status>);
    base::Status ok;
    CHECK(ok.isOk());
    CHECK_EQ(ok.toString(), std::string("Ok"));
    base::Status s(base::Code::NotFound, "no such key", 7);
    CHECK(!s.isOk());
    CHECK_EQ(s.detail(), 7u);
    CHECK_EQ(s.toString(), std::string("NotFound: no such key"));
    base::Status longer(base::Code::Internal, std::string(500, 'x'));
    CHECK_EQ(longer.message().size(), base::Status::kMaxMessage);
}

TEST(codeNumbersMatchGrpc) {
    CHECK_EQ(static_cast<int>(base::Code::DeadlineExceeded), 4);
    CHECK_EQ(static_cast<int>(base::Code::Unimplemented), 12);
    CHECK_EQ(static_cast<int>(base::Code::Unauthenticated), 16);
}

TEST(resultHoldsAValueOrAStatus) {
    base::Result<std::string> good(std::string("value"));
    CHECK(good.ok());
    CHECK_EQ(good.value(), std::string("value"));
    base::Result<std::string> bad(base::Status(base::Code::InvalidArgument, "nope"));
    CHECK(!bad.ok());
    CHECK(bad.status().code() == base::Code::InvalidArgument);
    bool threw = false;
    try {
        bad.value();
    } catch (const base::Error& error) {
        threw = error.code() == base::Code::InvalidArgument;
    }
    CHECK(threw);
    base::Result<int> moved = base::Result<int>(5);
    CHECK_EQ(moved.value(), 5);
    base::Result<int> fromOk{base::Status()};  // an Ok status is not a value
    CHECK(!fromOk.ok());
}

TEST(libraryErrorsCanDeriveFromBaseError) {
    struct MyError : base::Error {
        MyError() : base::Error(base::Code::Aborted, "retry", 9) {}
    };
    try {
        throw MyError();
    } catch (const base::Error& error) {
        CHECK_EQ(error.status().detail(), 9u);
    }
}

int main() { return tst::runAll("status"); }
