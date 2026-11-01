// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool writeAll(int fd, const char* data, std::size_t size) {
    while (size) {
        const ssize_t n = ::write(fd, data, size);
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
    ~FileSink() override { if (fd_ >= 0) ::close(fd_); }
    bool open(std::string* error) {
        fd_ = ::open(options_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, options_.mode);
        if (fd_ < 0) {
            if (error)
                *error = sys::errnoMessage(options_.path.c_str(), errno);
            return false;
        }
        struct stat st {};
        size_ = ::fstat(fd_, &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
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
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        open(nullptr);
    }
private:
    void rotate() {
        ::close(fd_);
        fd_ = -1;
        const std::string base = options_.path;
        if (options_.keep == 0) {
            ::unlink(base.c_str());
        } else {
            ::unlink((base + "." + std::to_string(options_.keep)).c_str());
            for (unsigned i = options_.keep; i > 1; --i)
                ::rename((base + "." + std::to_string(i - 1)).c_str(), (base + "." + std::to_string(i)).c_str());
            ::rename(base.c_str(), (base + ".1").c_str());
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
