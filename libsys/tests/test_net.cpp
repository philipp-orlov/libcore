// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <netinet/in.h>

#include "mini_test.hpp"
#include "sys/net.hpp"

TEST(listenAddressForms) {
    sys::ListenAddress a;
    CHECK(sys::resolveListenAddress("", 8080, a));
    CHECK_EQ(a.family, AF_INET);
    CHECK(!a.dualStack);
    CHECK(sys::resolveListenAddress("::", 8080, a));
    CHECK_EQ(a.family, AF_INET6);
    CHECK(a.dualStack);
    CHECK(sys::resolveListenAddress("[127.0.0.1]", 9, a));
    CHECK_EQ(a.family, AF_INET);
    CHECK(sys::resolveListenAddress("::1", 9, a));
    CHECK_EQ(a.family, AF_INET6);
    CHECK(!a.dualStack);
    CHECK(!sys::resolveListenAddress("definitely.not.a.host.invalid", 9, a));
}

TEST(socketOptions) {
    sys::Pal pal;
    const sys::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s != sys::kInvalidSocket);
    CHECK(sys::setNonBlocking(s));
    CHECK(sys::setTcpNoDelay(s));
    CHECK(sys::setReuseAddr(s));
    CHECK(sys::setReusePort(s));
    sys::closeSocket(s);
    CHECK(sys::wouldBlock(EAGAIN));
    CHECK(!sys::wouldBlock(ECONNRESET));
    CHECK(!sys::setNonBlocking(-1));
}

int main() { return tst::runAll("net"); }
