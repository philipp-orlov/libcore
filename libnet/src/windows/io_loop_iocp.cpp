// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "net/io_loop.hpp"
#include "sched/task_queue.hpp"
#include "sched/timer_heap.hpp"

// Windows backend, built on IOCP -- the native completion-port proactor,
// so this mirrors the epoll backend's *behavior* exactly (same public
// API, same "callbacks always fire from the loop, never synchronously
// nested" contract) while using genuinely async OS calls (AcceptEx /
// WSARecv / WSASend with OVERLAPPED) instead of emulating readiness.
//
// Lifetime: the completion key is a pointer to the handler (a connection's or
// a listener's Impl), and a completion can arrive long after the owner is
// gone -- closing a socket cancels its pending operations, and each then
// completes, aborted, on a later round. So every pending operation holds a
// shared_ptr to its Impl (readSelf, writeSelf, acceptSelf), taken before the
// call is issued and dropped when its completion has been dispatched (or at
// once if the call failed to start). The Impl owns what the kernel touches
// meanwhile: the OVERLAPPED, and the receive buffer, which is copied to the
// caller's on completion and only while the read is still wanted.
//
// Built and run on Windows by tunnel's end-to-end tests (the relay's server
// runs on it); the 23 of tests/e2e_windows.py and the relay's own exercise
// accept, read, write and close under load.

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "net/log.hpp"

namespace net {

namespace {

constexpr ULONG_PTR kWakeupKey = 0;
constexpr ULONG_PTR kIoKey =
    1;  // per-handler completion key; handler ptr carried via OVERLAPPED subtype instead

LPFN_ACCEPTEX g_AcceptEx = nullptr;
LPFN_GETACCEPTEXSOCKADDRS g_GetAcceptExSockaddrs = nullptr;

void ensureExtensionFunctionsLoaded(socket_t anySocket)
{
    if (g_AcceptEx && g_GetAcceptExSockaddrs)
        return;

    GUID guidAcceptEx = WSAID_ACCEPTEX;
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes = 0;
    WSAIoctl(anySocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
             &g_AcceptEx, sizeof(g_AcceptEx), &bytes, nullptr, nullptr);
    WSAIoctl(anySocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidGetAcceptExSockaddrs,
             sizeof(guidGetAcceptExSockaddrs), &g_GetAcceptExSockaddrs,
             sizeof(g_GetAcceptExSockaddrs), &bytes, nullptr, nullptr);
}

enum class OpKind
{
    Read,
    Write,
    Accept,
    Wakeup
};

struct IocpOp : OVERLAPPED
{
    OpKind kind;
};

// Base for anything that can own pending IOCP operations; the handler
// pointer travels as the OVERLAPPED's containing object is looked up by
// downcasting from the IocpOp -- each concrete Impl below embeds its ops
// and implements OnComplete itself, dispatched from IoLoop::run().
struct IocpHandler
{
    virtual void onComplete(IocpOp* op, DWORD bytesTransferred, bool ok, int errorCode) = 0;
    virtual ~IocpHandler() = default;
};

// What the completion key holds: the same address however the Impl is derived.
template <class Impl>
ULONG_PTR keyOf(Impl* impl)
{
    return reinterpret_cast<ULONG_PTR>(static_cast<IocpHandler*>(impl));
}

}  // namespace

struct IoLoop::Impl
{
    Impl(size_t taskCapacity, size_t timerCapacity) : tasks(taskCapacity), timers(timerCapacity) {}
    HANDLE iocp = nullptr;
    std::atomic<bool> started{false};
    std::thread::id loopThreadId;
    uint64_t iteration = 0;
    std::chrono::steady_clock::time_point roundStart = std::chrono::steady_clock::now();

    sched::BasicTaskQueue<std::function<void()>> tasks;

    sched::BasicTimerHeap<std::function<void()>> timers;

    void wakeUp() { PostQueuedCompletionStatus(iocp, 0, kWakeupKey, nullptr); }

    DWORD computeTimeoutMilliseconds()
    {
        // -1 (no timer armed) is INFINITE.
        return static_cast<DWORD>(timers.waitMilliseconds(sched::BasicTimerHeap<std::function<void()>>::Clock::now()));
    }

    void fireExpiredTimers()
    {
        for (size_t fired = 0; fired < timers.capacity(); ++fired) {
            TimerCallback callback;
            if (!timers.popDue(sched::BasicTimerHeap<std::function<void()>>::Clock::now(), callback))
                break;

            try {
                callback();
            } catch (...) {
                NET_LOG_ERROR("Timer callback threw");
            }
        }
    }

    void drainPostedTasks() { tasks.drain(); }
};

IoLoop::IoLoop(size_t taskCapacity, size_t timerCapacity)
    : impl_(std::make_unique<Impl>(taskCapacity, timerCapacity))
{
    impl_->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
}

IoLoop::~IoLoop()
{
    if (impl_->iocp)
        CloseHandle(impl_->iocp);
}

void IoLoop::run()
{
    if (impl_->started.exchange(true))
        throw std::logic_error("IoLoop::run may only be called once");

    impl_->loopThreadId = std::this_thread::get_id();

    while (!impl_->tasks.closed() || impl_->tasks.hasPending()) {
        DWORD timeoutMs = impl_->computeTimeoutMilliseconds();
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(impl_->iocp, &bytesTransferred, &completionKey,
                                            &overlapped, timeoutMs);
        ++impl_->iteration;
        impl_->roundStart = std::chrono::steady_clock::now();
        if (!ok && overlapped == nullptr) {
            // Timeout (no completion within timeoutMs) -- fall through to
            // process timers/tasks below.
        } else if (overlapped != nullptr) {
            int errorCode = ok ? 0 : static_cast<int>(GetLastError());
            auto* op = static_cast<IocpOp*>(overlapped);
            if (op->kind == OpKind::Wakeup) {
                // no-op marker; nothing to do besides looping around
            } else {
                auto* handler = reinterpret_cast<IocpHandler*>(completionKey);
                try {
                    handler->onComplete(op, bytesTransferred, ok != 0, errorCode);
                } catch (...) {
                    NET_LOG_ERROR("I/O completion callback threw");
                }
            }
        }
        // completionKey == kWakeupKey with overlapped == nullptr is the
        // explicit wakeUp() ping; nothing to do besides looping around.

        impl_->fireExpiredTimers();
        impl_->drainPostedTasks();
    }
}

uint64_t IoLoop::iteration() const
{
    return impl_->iteration;
}

std::chrono::steady_clock::time_point IoLoop::now() const
{
    return impl_->roundStart;
}

void IoLoop::stop()
{
    impl_->tasks.close();
    impl_->wakeUp();
}

void IoLoop::post(std::function<void()> callback)
{
    if (!tryPost(std::move(callback)))
        throw std::runtime_error("reactor task queue is full or stopped");
}

bool IoLoop::tryPost(std::function<void()> callback)
{
    if (!impl_->tasks.tryPush(std::move(callback)))
        return false;

    impl_->wakeUp();
    return true;
}

void IoLoop::postControl(std::function<void()> callback)
{
    if (!impl_->tasks.tryPush(std::move(callback), true))
        throw std::runtime_error("reactor control queue is full or stopped");

    impl_->wakeUp();
}

size_t IoLoop::failedTasks() const
{
    return impl_->tasks.failedTasks();
}

size_t IoLoop::rejectedTasks() const
{
    return impl_->tasks.rejectedTasks();
}

size_t IoLoop::pendingTasks() const
{
    return impl_->tasks.pendingTasks();
}

uint64_t IoLoop::addTimer(std::chrono::milliseconds delay, TimerCallback callback)
{
    const uint64_t id =
        impl_->timers.tryAdd(sched::BasicTimerHeap<std::function<void()>>::after(delay), std::move(callback));
    if (!id)
        throw std::runtime_error("reactor timer capacity exhausted");

    impl_->wakeUp();
    return id;
}

void IoLoop::cancelTimer(uint64_t id)
{
    impl_->timers.cancel(id);
}

bool IoLoop::refreshTimer(uint64_t id, std::chrono::milliseconds delay)
{
    if (!impl_->timers.refresh(id, sched::BasicTimerHeap<std::function<void()>>::after(delay)))
        return false;

    impl_->wakeUp();
    return true;
}

// ---------------------------------------------------------------------
// TcpConnection
// ---------------------------------------------------------------------

struct TcpConnection::Impl : IocpHandler, std::enable_shared_from_this<TcpConnection::Impl>
{
    IoLoop& loop;
    socket_t fd;
    bool closed = false;
    bool associated = false;

    IocpOp readOp{};
    WSABUF readWsaBuf{};
    IoCallback readCallback;
    // What the kernel fills: always this buffer, which lives as long as the
    // read can complete (see the note at the top). A caller's own buffer gets
    // a copy when the read completes; with none, the bytes are read from here.
    std::vector<unsigned char> recvBuffer;
    unsigned char* callerReadBuffer = nullptr;
    std::shared_ptr<Impl> readSelf;   // held while a read is pending
    std::shared_ptr<Impl> writeSelf;  // held while a write is pending

    IocpOp writeOp{};
    WSABUF writeWsaBuf{};
    const unsigned char* writeBuf = nullptr;
    size_t writeLen = 0;
    size_t writeOffset = 0;
    IoCallback writeCallback;

    explicit Impl(IoLoop& l, socket_t f) : loop(l), fd(f)
    {
        std::memset(&readOp, 0, sizeof(readOp));
        std::memset(&writeOp, 0, sizeof(writeOp));
        readOp.kind = OpKind::Read;
        writeOp.kind = OpKind::Write;
    }

    void ensureAssociated()
    {
        if (associated)
            return;

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), loop.impl()->iocp, keyOf(this), 0);
        associated = true;
    }

    void submitRead()
    {
        readWsaBuf.buf = reinterpret_cast<CHAR*>(recvBuffer.data());
        readWsaBuf.len = static_cast<ULONG>(pendingReadCap);
        DWORD flags = 0;
        std::memset(&readOp, 0, sizeof(WSAOVERLAPPED));
        readOp.kind = OpKind::Read;
        readSelf = shared_from_this();
        int rc = WSARecv(fd, &readWsaBuf, 1, nullptr, &flags, &readOp, nullptr);
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                readSelf.reset();  // no completion will come
                auto callback = std::move(readCallback);
                readCallback = nullptr;
                if (callback)
                    callback(IoResult{0, false, err});
            }
        }
    }

    void submitWrite()
    {
        writeWsaBuf.buf = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(writeBuf + writeOffset));
        writeWsaBuf.len = static_cast<ULONG>(writeLen - writeOffset);
        std::memset(&writeOp, 0, sizeof(WSAOVERLAPPED));
        writeOp.kind = OpKind::Write;
        writeSelf = shared_from_this();
        int rc = WSASend(fd, &writeWsaBuf, 1, nullptr, 0, &writeOp, nullptr);
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                writeSelf.reset();  // no completion will come
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeOffset, false, err});
            }
        }
    }

    size_t pendingReadCap = 0;

    void onComplete(IocpOp* op, DWORD bytesTransferred, bool ok, int errorCode) override
    {
        if (op->kind == OpKind::Read) {
            auto keepAlive = std::move(readSelf);  // dropped on return, after the callback
            if (!ok) {
                closed = true;
                auto callback = std::move(readCallback);
                readCallback = nullptr;
                if (callback)
                    callback(IoResult{0, false, errorCode});
                return;
            }
            auto callback = std::move(readCallback);
            readCallback = nullptr;
            if (!callback)
                return;  // closed meanwhile: the callback was answered then, and the caller's buffer is no longer ours to write
            unsigned char* data = recvBuffer.data();
            if (callerReadBuffer) {
                if (bytesTransferred)
                    std::memcpy(callerReadBuffer, recvBuffer.data(), bytesTransferred);
                data = callerReadBuffer;
            }
            callback(IoResult{bytesTransferred, true, 0, data});
        } else if (op->kind == OpKind::Write) {
            auto keepAlive = std::move(writeSelf);  // dropped on return, after the callback
            if (!ok) {
                closed = true;
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeOffset, false, errorCode});
                return;
            }
            writeOffset += bytesTransferred;
            if (writeOffset >= writeLen) {
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeLen, true, 0});
            } else {
                submitWrite();  // partial write; continue with the remainder
            }
        }
    }
};

TcpConnection::TcpConnection(IoLoop& loop, socket_t fd)
    : loop_(loop), fd_(fd), impl_(std::make_shared<Impl>(loop, fd))
{
    setNonBlocking(fd_);  // harmless with overlapped I/O; kept for parity
    setTcpNoDelay(fd_);
    impl_->ensureAssociated();
}

TcpConnection::~TcpConnection()
{
    close();
}

void TcpConnection::asyncRead(unsigned char* buffer, size_t bufferCapacity, IoCallback callback)
{
    if (impl_->closed) {
        callback(IoResult{0, false, WSAECONNABORTED});
        return;
    }
    if (!buffer)
        bufferCapacity = 16 * 1024;  // the connection's own, made on first use
    if (impl_->recvBuffer.size() < bufferCapacity)
        impl_->recvBuffer.resize(bufferCapacity);
    impl_->callerReadBuffer = buffer;
    impl_->pendingReadCap = bufferCapacity;
    impl_->readCallback = std::move(callback);
    impl_->submitRead();
}

void TcpConnection::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    if (impl_->closed) {
        callback(IoResult{0, false, WSAECONNABORTED});
        return;
    }
    if (length == 0) {
        callback(IoResult{0, true, 0});
        return;
    }
    impl_->writeBuf = data;
    impl_->writeLen = length;
    impl_->writeOffset = 0;
    impl_->writeCallback = std::move(callback);
    impl_->submitWrite();
}

size_t TcpConnection::tryWrite(const unsigned char*, size_t, const unsigned char*, size_t)
{
    // IOCP has no immediate send that leaves the buffers with the caller;
    // callers copy and use asyncWrite.
    return 0;
}

void TcpConnection::close()
{
    if (impl_->closed && fd_ == kInvalidSocket)
        return;

    // A callback below may destroy this connection; the Impl is kept for the rest of the function.
    const std::shared_ptr<Impl> impl = impl_;
    impl->closed = true;
    closeSocket(fd_);
    fd_ = kInvalidSocket;

    if (impl->readCallback) {
        auto callback = std::move(impl->readCallback);
        impl->readCallback = nullptr;
        callback(IoResult{0, false, WSA_OPERATION_ABORTED});
    }
    if (impl->writeCallback) {
        auto callback = std::move(impl->writeCallback);
        impl->writeCallback = nullptr;
        callback(IoResult{impl->writeOffset, false, WSA_OPERATION_ABORTED});
    }
}

bool TcpConnection::isOpen() const
{
    return !impl_->closed && fd_ != kInvalidSocket;
}

const std::string& TcpConnection::peerAddress() const
{
    if (peerResolved_)
        return peerAddress_;

    peerResolved_ = true;
    sockaddr_storage addr{};
    int len = sizeof(addr);
    if (fd_ == kInvalidSocket || getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return peerAddress_;

    char host[NI_MAXHOST] = {0};
    char port[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return peerAddress_;
    }
    peerAddress_ = host;
    peerAddress_ += ':';
    peerAddress_ += port;
    return peerAddress_;
}

// ---------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------

struct TcpListener::Impl : IocpHandler, std::enable_shared_from_this<TcpListener::Impl>
{
    IoLoop& loop;
    socket_t fd = kInvalidSocket;
    int family = AF_INET;
    socket_t pendingAcceptSocket = kInvalidSocket;
    AcceptCallback onAccept;
    IocpOp acceptOp{};
    std::vector<char> acceptAddrBuf;  // holds local+remote sockaddr output from AcceptEx
    std::shared_ptr<Impl> acceptSelf;  // held while an AcceptEx is pending

    static constexpr size_t kAddrLen = sizeof(sockaddr_in6) + 16;

    explicit Impl(IoLoop& l) : loop(l), acceptAddrBuf(kAddrLen * 2)
    {
        acceptOp.kind = OpKind::Accept;
    }

    void armAccept()
    {
        if (fd == kInvalidSocket)
            return;  // closed
        pendingAcceptSocket = ::socket(family, SOCK_STREAM, 0);
        if (pendingAcceptSocket == kInvalidSocket)
            return;

        DWORD bytesReceived = 0;
        std::memset(&acceptOp, 0, sizeof(WSAOVERLAPPED));
        acceptOp.kind = OpKind::Accept;
        acceptSelf = shared_from_this();
        BOOL rc = g_AcceptEx(fd, pendingAcceptSocket, acceptAddrBuf.data(), 0,
                             static_cast<DWORD>(kAddrLen), static_cast<DWORD>(kAddrLen),
                             &bytesReceived, &acceptOp);
        if (!rc) {
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                acceptSelf.reset();  // no completion will come
                closeSocket(pendingAcceptSocket);
                pendingAcceptSocket = kInvalidSocket;
                NET_LOG_WARN("AcceptEx failed: %d", err);
            }
        }
    }

    void onComplete(IocpOp*, DWORD, bool ok, int errorCode) override
    {
        auto keepAlive = std::move(acceptSelf);  // dropped on return
        if (fd == kInvalidSocket) {
            // The listener was closed: this is the accept it cancelled, or one that got in first. Nobody wants either.
            if (pendingAcceptSocket != kInvalidSocket)
                closeSocket(pendingAcceptSocket);
            pendingAcceptSocket = kInvalidSocket;
            return;
        }
        if (ok) {
            setsockopt(pendingAcceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&fd), sizeof(fd));
            if (onAccept)
                onAccept(pendingAcceptSocket);
            else
                closeSocket(pendingAcceptSocket);
        } else {
            NET_LOG_WARN("AcceptEx completion failed: %d", errorCode);
            if (pendingAcceptSocket != kInvalidSocket)
                closeSocket(pendingAcceptSocket);
        }
        pendingAcceptSocket = kInvalidSocket;
        armAccept();  // keep accepting for the listener's lifetime
    }
};

TcpListener::TcpListener(IoLoop& loop, const std::string& host, uint16_t port, bool /*reusePort*/)
    : loop_(loop), impl_(std::make_shared<Impl>(loop))
{
    // Where to bind is what the other backends bind: "" and "0.0.0.0" are every IPv4 address, "::" every address of a
    // dual-stack socket, a literal that address, and a name whatever it resolves to. (A bare inet_pton here would turn a
    // name into 0.0.0.0, which is every interface.)
    sys::ListenAddress address;
    if (!sys::resolveListenAddress(host, port, address)) {
        NET_LOG_ERROR("cannot resolve the address to listen on: %s", host.c_str());
        return;  // no fd: the listener is closed from the start
    }
    impl_->family = address.family;
    impl_->fd = ::socket(address.family, SOCK_STREAM, 0);
    if (impl_->fd == kInvalidSocket) {
        NET_LOG_ERROR("socket() failed: %d", WSAGetLastError());
        return;
    }
    setReuseAddr(impl_->fd);
    if (address.family == AF_INET6) {
        const DWORD v6only = address.dualStack ? 0 : 1;
        setsockopt(impl_->fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    }
    ensureExtensionFunctionsLoaded(impl_->fd);

    if (::bind(impl_->fd, reinterpret_cast<const sockaddr*>(&address.storage), address.length) != 0) {
        NET_LOG_ERROR("bind(%s:%u) failed: %d", host.c_str(), port, WSAGetLastError());
    }
    if (::listen(impl_->fd, SOMAXCONN) != 0) {
        NET_LOG_ERROR("listen() failed: %d", WSAGetLastError());
    }

    CreateIoCompletionPort(reinterpret_cast<HANDLE>(impl_->fd), loop_.impl()->iocp, keyOf(impl_.get()), 0);
}

TcpListener::~TcpListener()
{
    close();
}

void TcpListener::asyncAccept(AcceptCallback onAccept)
{
    impl_->onAccept = std::move(onAccept);
    impl_->armAccept();
}

void TcpListener::close()
{
    if (impl_->fd == kInvalidSocket)
        return;

    closeSocket(impl_->fd);  // cancels the pending AcceptEx, which then completes, aborted, and is cleaned up there
    impl_->fd = kInvalidSocket;
}

}  // namespace net
