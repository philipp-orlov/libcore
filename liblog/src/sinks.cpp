// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#include <syslog.h>
#endif

#include "lg/sink.hpp"
#include "sys/errno.hpp"

namespace lg {

namespace {

#if defined(_WIN32)
// The CRT's descriptors stand in for POSIX ones; a log file is binary, so lines are not translated.
using ssize_t = long long;
inline ssize_t rawWrite(int fd, const char* data, std::size_t size) {
    return ::_write(fd, data, static_cast<unsigned>(size > 0x40000000u ? 0x40000000u : size));
}
inline int rawOpen(const char* path, unsigned mode) {
    int fd = -1;
    if (::_sopen_s(&fd, path, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY | _O_NOINHERIT, _SH_DENYNO,
                   mode & 0200 ? _S_IREAD | _S_IWRITE : _S_IREAD) != 0)
        return -1;
    return fd;
}
inline int rawClose(int fd) { return ::_close(fd); }
inline void rawUnlink(const char* path) { ::_unlink(path); }
inline void rawRename(const char* from, const char* to) { ::MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING); }
inline std::uint64_t sizeOf(int fd) {
    struct _stat64 st {};
    return ::_fstat64(fd, &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}
#else
inline ssize_t rawWrite(int fd, const char* data, std::size_t size) { return ::write(fd, data, size); }
inline int rawOpen(const char* path, unsigned mode) {
    return ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, mode);
}
inline int rawClose(int fd) { return ::close(fd); }
inline void rawUnlink(const char* path) { ::unlink(path); }
inline void rawRename(const char* from, const char* to) { ::rename(from, to); }
inline std::uint64_t sizeOf(int fd) {
    struct stat st {};
    return ::fstat(fd, &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}
#endif

bool writeAll(int fd, const char* data, std::size_t size) {
    while (size) {
        const ssize_t n = rawWrite(fd, data, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(int fd) : fd_(fd) {}
    void write(Level, const char* data, std::size_t size) override { writeAll(fd_, data, size); }
private:
    int fd_;
};

class CallbackSink final : public Sink {
public:
    explicit CallbackSink(std::function<void(Level, const char*, std::size_t)> fn) : fn_(std::move(fn)) {}
    void write(Level level, const char* data, std::size_t size) override { fn_(level, data, size); }
    bool perLine() const override { return true; }
private:
    std::function<void(Level, const char*, std::size_t)> fn_;
};

class FileSink final : public Sink {
public:
    explicit FileSink(FileSinkOptions o) : options_(std::move(o)) {}
    ~FileSink() override { if (fd_ >= 0) rawClose(fd_); }
    bool open(std::string* error) {
        fd_ = rawOpen(options_.path.c_str(), options_.mode);
        if (fd_ < 0) {
            if (error)
                *error = sys::errnoMessage(options_.path.c_str(), errno);
            return false;
        }
        size_ = sizeOf(fd_);
        return true;
    }
    void write(Level, const char* data, std::size_t size) override {
        if (fd_ < 0 && !open(nullptr))
            return;
        if (writeAll(fd_, data, size))
            size_ += size;
        // Rotate after the write that reached the limit, so one batch larger than the limit is not split and the
        // next batch starts a fresh file.
        if (options_.rotateBytes && size_ >= options_.rotateBytes)
            rotate();
    }
    void reopen() override {
        if (fd_ >= 0) { rawClose(fd_); fd_ = -1; }
        open(nullptr);
    }
private:
    void rotate() {
        rawClose(fd_);
        fd_ = -1;
        const std::string base = options_.path;
        if (options_.keep == 0) {
            rawUnlink(base.c_str());
        } else {
            rawUnlink((base + "." + std::to_string(options_.keep)).c_str());
            for (unsigned i = options_.keep; i > 1; --i)
                rawRename((base + "." + std::to_string(i - 1)).c_str(), (base + "." + std::to_string(i)).c_str());
            rawRename(base.c_str(), (base + ".1").c_str());
        }
        open(nullptr);
    }
    FileSinkOptions options_;
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

#if defined(__linux__) || defined(__APPLE__)
class SyslogSink final : public Sink {
public:
    explicit SyslogSink(const char* ident) : ident_(ident) { ::openlog(ident_.c_str(), LOG_PID, LOG_USER); }
    ~SyslogSink() override { ::closelog(); }
    void write(Level level, const char* data, std::size_t size) override {
        int priority = LOG_INFO;
        switch (level) {
            case Level::Trace: case Level::Debug: priority = LOG_DEBUG; break;
            case Level::Info: priority = LOG_INFO; break;
            case Level::Warn: priority = LOG_WARNING; break;
            default: priority = LOG_ERR; break;
        }
        if (size && data[size - 1] == '\n') --size;
        ::syslog(priority, "%.*s", static_cast<int>(size), data);
    }
    bool perLine() const override { return true; }
private:
    std::string ident_;
};
#endif

}  // namespace

std::unique_ptr<Sink> makeConsoleSink(int fd) { return std::make_unique<ConsoleSink>(fd); }
std::unique_ptr<Sink> makeCallbackSink(std::function<void(Level, const char*, std::size_t)> fn) {
    return std::make_unique<CallbackSink>(std::move(fn));
}
std::unique_ptr<Sink> makeFileSink(const FileSinkOptions& options, std::string* error) {
    auto sink = std::make_unique<FileSink>(options);
    if (!sink->open(error))
        return nullptr;
    return sink;
}
#if defined(__linux__) || defined(__APPLE__)
std::unique_ptr<Sink> makeSyslogSink(const char* ident) { return std::make_unique<SyslogSink>(ident); }
#endif

}  // namespace lg
