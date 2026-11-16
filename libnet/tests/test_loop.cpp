// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <functional>
#include <thread>
#include <vector>

#include "mini_test.hpp"
#include "net/io_loop.hpp"
#include "sys/net.hpp"

// The client side of these tests is plain blocking sockets, spelled the same on both platforms.
#ifdef _WIN32
using ssize_t = int;
static const int SHUT_WR = SD_SEND;
static int sendAll(net::socket_t s, const char* data, size_t n) { return ::send(s, data, static_cast<int>(n), 0); }
static ssize_t receive(net::socket_t s, char* data, size_t n) { return ::recv(s, data, static_cast<int>(n), 0); }
#else
static int sendAll(net::socket_t s, const char* data, size_t n) { return static_cast<int>(::send(s, data, n, 0)); }
static ssize_t receive(net::socket_t s, char* data, size_t n) { return ::recv(s, data, n, 0); }
#endif
static const sys::Pal pal;

using namespace std::chrono_literals;

namespace {
uint16_t freePort() {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    sys::closeSocket(s);
    return ntohs(a.sin_port);
}
struct Loop {
    net::IoLoop loop;
    std::thread thread{[this] { loop.run(); }};
    ~Loop() { loop.stop(); thread.join(); }
};
}  // namespace

TEST(postAndTimersRunOnTheLoopThread) {
    Loop l;
    std::atomic<int> ran{0};
    std::thread::id loopThread;
    l.loop.post([&] { loopThread = std::this_thread::get_id(); ++ran; });
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> fired{false};
    l.loop.addTimer(40ms, [&] { fired = true; });
    const auto cancelled = l.loop.addTimer(10ms, [&] { ran += 100; });
    l.loop.cancelTimer(cancelled);
    while (!fired && std::chrono::steady_clock::now() - t0 < 3s) std::this_thread::sleep_for(1ms);
    CHECK(fired);
    CHECK(std::chrono::steady_clock::now() - t0 >= 35ms);
    CHECK_EQ(ran.load(), 1);                         // the cancelled timer never ran
    CHECK(loopThread != std::this_thread::get_id());
}

TEST(taskQueueRejectsWhenFullButControlLaneStaysOpen) {
    net::IoLoop loop(2, 16);                          // never run: tasks just queue
    CHECK(loop.tryPost([] {}));
    CHECK(loop.tryPost([] {}));
    CHECK(!loop.tryPost([] {}));
    CHECK_EQ(loop.rejectedTasks(), 1u);
    loop.postControl([] {});                          // reserved slots
}

// Loop objects belong to their loop's thread: everything that touches a listener or a connection runs there.
template <class F>
static void onLoop(net::IoLoop& loop, F&& f) {
    std::promise<void> done;
    loop.post([&] { f(); done.set_value(); });
    done.get_future().wait();
}

TEST(echoServerOverTheLoop) {
    Loop l;
    const uint16_t port = freePort();
    std::atomic<int> accepted{0};
    std::vector<std::shared_ptr<net::TcpConnection>> connections;   // loop thread only
    std::unique_ptr<net::TcpListener> listener;
    auto echo = std::make_shared<std::function<void(std::shared_ptr<net::TcpConnection>)>>();
    *echo = [echo](std::shared_ptr<net::TcpConnection> c) {
        c->asyncRead(nullptr, 0, [echo, c](net::IoResult r) {
            if (!r.ok || r.bytes == 0) { c->close(); return; }
            auto copy = std::make_shared<std::vector<unsigned char>>(r.data, r.data + r.bytes);
            c->asyncWrite(copy->data(), copy->size(), [echo, c, copy](net::IoResult w) {
                if (w.ok) (*echo)(c); else c->close();
            });
        });
    };
    onLoop(l.loop, [&] {
        listener = std::make_unique<net::TcpListener>(l.loop, "127.0.0.1", port, false);
        listener->asyncAccept([&](net::socket_t fd) {
            auto c = std::make_shared<net::TcpConnection>(l.loop, fd);
            connections.push_back(c);
            ++accepted;
            (*echo)(c);
        });
    });

    for (int client = 0; client < 4; ++client) {
        net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK_EQ(::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        for (int round = 0; round < 50; ++round) {
            char out[64], in[64];
            const int n = std::snprintf(out, sizeof out, "client %d round %d", client, round);
            CHECK_EQ(sendAll(s, out, static_cast<size_t>(n)), n);
            size_t got = 0;
            while (got < static_cast<size_t>(n)) {
                const ssize_t r = receive(s, in + got, sizeof in - got);
                if (r <= 0) break;
                got += static_cast<size_t>(r);
            }
            CHECK_EQ(got, static_cast<size_t>(n));
            CHECK(std::memcmp(in, out, got) == 0);
        }
        sys::closeSocket(s);
    }
    CHECK_EQ(accepted.load(), 4);
    onLoop(l.loop, [&] {
        listener->close();
        for (auto& c : connections) c->close();
        connections.clear();
        listener.reset();
        *echo = nullptr;   // break the callback's self reference
    });
}

// Data and the peer's FIN that arrive before the first read: a short read takes the data, and the read after it has to
// see the end, though epoll (edge-triggered) reported both in one event that has already been handled.
TEST(endOfStreamThatArrivedWithTheDataIsSeen) {
    Loop l;
    const uint16_t port = freePort();
    std::unique_ptr<net::TcpListener> listener;
    std::shared_ptr<net::TcpConnection> connection;   // loop thread only
    std::promise<void> acceptedPromise;
    onLoop(l.loop, [&] {
        listener = std::make_unique<net::TcpListener>(l.loop, "127.0.0.1", port, false);
        listener->asyncAccept([&](net::socket_t fd) {
            connection = std::make_shared<net::TcpConnection>(l.loop, fd);
            acceptedPromise.set_value();
        });
    });
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    acceptedPromise.get_future().wait();
    CHECK_EQ(sendAll(s, "hello", 5), 5);
    ::shutdown(s, SHUT_WR);
    std::this_thread::sleep_for(100ms);  // both are in, and the loop has seen their event with no read waiting

    std::promise<size_t> first;
    std::promise<bool> second;
    onLoop(l.loop, [&] {
        connection->asyncRead(nullptr, 0, [&](net::IoResult r) {
            first.set_value(r.ok ? r.bytes : 0);
            connection->asyncRead(nullptr, 0, [&](net::IoResult end) { second.set_value(end.ok && end.bytes == 0); });
        });
    });
    auto firstResult = first.get_future();
    CHECK(firstResult.wait_for(2s) == std::future_status::ready);
    CHECK_EQ(firstResult.get(), size_t{5});
    auto secondResult = second.get_future();
    const bool ended = secondResult.wait_for(2s) == std::future_status::ready;
    CHECK(ended);
    if (ended)
        CHECK(secondResult.get());
    sys::closeSocket(s);
    onLoop(l.loop, [&] {
        if (connection)
            connection->close();
        connection.reset();
        listener->close();
        listener.reset();
    });
}

// A megabyte each way: the server reads into a buffer of its own (never the loop's), and answers with tryWrite first
// and asyncWrite for whatever the socket did not take at once. Every byte arrives, in order, on both sides.
TEST(largeTransferThroughCallerBuffersAndTryWrite) {
    Loop l;
    const uint16_t port = freePort();
    constexpr size_t kTotal = 1 << 20;
    std::unique_ptr<net::TcpListener> listener;
    std::shared_ptr<net::TcpConnection> connection;   // loop thread only
    std::vector<unsigned char> received(kTotal);
    std::vector<unsigned char> reply(kTotal);
    for (size_t i = 0; i < kTotal; ++i)
        reply[i] = static_cast<unsigned char>(i * 31 + 7);
    size_t got = 0;
    std::atomic<size_t> immediate{0};
    std::promise<bool> finished;
    std::function<void()> readOn = [&] {
        connection->asyncRead(received.data() + got, received.size() - got, [&](net::IoResult r) {
            if (!r.ok || r.bytes == 0) { finished.set_value(false); return; }
            CHECK(r.data == received.data() + got);   // in place: nothing was copied through another buffer
            got += r.bytes;
            if (got < kTotal) { readOn(); return; }
            const size_t sent = connection->tryWrite(reply.data(), 16, reply.data() + 16, reply.size() - 16);
            immediate = sent;
            connection->asyncWrite(reply.data() + sent, reply.size() - sent,
                                   [&](net::IoResult w) { finished.set_value(w.ok); });
        });
    };
    onLoop(l.loop, [&] {
        listener = std::make_unique<net::TcpListener>(l.loop, "127.0.0.1", port, false);
        listener->asyncAccept([&](net::socket_t fd) {
            connection = std::make_shared<net::TcpConnection>(l.loop, fd);
            readOn();
        });
    });
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    std::vector<char> out(kTotal);
    for (size_t i = 0; i < kTotal; ++i)
        out[i] = static_cast<char>(i * 17 + 3);
    std::thread writer([&] {
        size_t at = 0;
        while (at < kTotal) {
            const int n = sendAll(s, out.data() + at, std::min<size_t>(kTotal - at, 64 * 1024));
            if (n <= 0) break;
            at += static_cast<size_t>(n);
        }
    });
    std::vector<char> in(kTotal);
    size_t read = 0;
    while (read < kTotal) {
        const ssize_t n = receive(s, in.data() + read, kTotal - read);
        if (n <= 0) break;
        read += static_cast<size_t>(n);
    }
    writer.join();
    auto done = finished.get_future();
    CHECK(done.wait_for(5s) == std::future_status::ready);
    CHECK_EQ(read, kTotal);
    CHECK(std::memcmp(in.data(), reply.data(), kTotal) == 0);
    CHECK(std::memcmp(received.data(), out.data(), kTotal) == 0);
    CHECK(immediate.load() >= 16u);   // an empty socket buffer takes at least the head at once
    sys::closeSocket(s);
    onLoop(l.loop, [&] {
        connection->close();
        connection.reset();
        listener->close();
        listener.reset();
    });
}

// Posting from the loop's own thread, and timers armed there, need no wakeup and still run: a chain of tasks each
// posting the next, ending in a timer.
TEST(tasksAndTimersQueuedFromTheLoopThreadRun) {
    Loop l;
    std::promise<int> done;
    std::atomic<int> hops{0};
    std::function<void()> hop = [&] {
        if (++hops < 1000) { l.loop.post(hop); return; }
        l.loop.addTimer(5ms, [&] { done.set_value(hops.load()); });
    };
    l.loop.post(hop);
    auto result = done.get_future();
    CHECK(result.wait_for(5s) == std::future_status::ready);
    CHECK_EQ(result.get(), 1000);
}

int main() { return tst::runAll("net_loop"); }
