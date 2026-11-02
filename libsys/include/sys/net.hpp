// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Socket primitives behind one set of names (moved from libhttp's platform layer) so that nothing above libsys
// needs #ifdef _WIN32. Windows branches are carried over unchanged and are not compiled on this machine.

#if defined(_WIN32)
#define SYS_NET_WINDOWS 1
#else
#define SYS_NET_POSIX 1
#endif

#include <cstdint>
#include <string>

#if defined(SYS_NET_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace sys {

#if defined(SYS_NET_WINDOWS)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

// Platform Abstraction Layer: initializes/tears down platform networking
// (WSAStartup on Windows; no-op on Linux). Construct one instance for the
// lifetime of the process.
class Pal
{
public:
    Pal();
    ~Pal();
    Pal(const Pal&) = delete;
    Pal& operator=(const Pal&) = delete;
};

void closeSocket(socket_t s);
bool setNonBlocking(socket_t s);
bool setTcpNoDelay(socket_t s);
bool setReuseAddr(socket_t s);
bool setReusePort(socket_t s);  // no-op on Windows
int lastSocketError();
bool wouldBlock(int err);

// Where a listener binds. "" / "0.0.0.0" / "*" is every IPv4 address; "::" is
// every address of a dual-stack IPv6 socket (IPv4 clients arrive as mapped
// addresses); an IPv4 or IPv6 literal (brackets optional) binds exactly that
// address; anything else is resolved with getaddrinfo, preferring IPv4.
struct ListenAddress
{
    sockaddr_storage storage{};
    socklen_t length = 0;
    int family = AF_INET;
    bool dualStack = false;  // AF_INET6 socket that also takes IPv4
};
bool resolveListenAddress(const std::string& host, uint16_t port, ListenAddress& out);

}  // namespace sys
