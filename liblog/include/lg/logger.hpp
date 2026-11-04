// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Asynchronous logger for request paths.
//   producer: level check (one relaxed load) -> reserve a slot in the calling thread's own SPSC ring -> vsnprintf
//             straight into the slot -> publish. No lock, no syscall, no allocation after the thread's first line.
//   consumer: one thread merges the rings by timestamp, escapes control characters, adds the time prefix (formatted
//             once per second) and writes batches to the sinks.
// A full ring drops the line and counts it; the consumer reports drops at most once per second.
// flush() waits for every line produced before the call; the destructor/shutdown() drain every ring to empty.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "lg/sink.hpp"

#ifndef LG_MIN_LEVEL
#define LG_MIN_LEVEL 0   // compile-time floor: 0=Trace ... 5=Off; call sites below it compile to nothing
#endif

namespace lg {

struct LoggerOptions {
    std::size_t slotBytes = 256;      // one line, including the cut-off marker; 128..1024
    std::size_t ringSlots = 256;      // per producer thread, power of two (64 KiB at the default slot size)
    Level level = Level::Info;
    bool timestamps = true;
    bool utc = true;                  // local time needs localtime_r once per second only
    std::chrono::milliseconds idlePoll{5};
};

class Logger {
public:
    explicit Logger(LoggerOptions options = {});
    ~Logger();   // shutdown()
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void addSink(std::unique_ptr<Sink> sink);          // thread-safe; lines already queued reach the new sink too
    void clearSinks();
    void setLevel(Level level) noexcept { level_.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed); }
    Level level() const noexcept { return static_cast<Level>(level_.load(std::memory_order_relaxed)); }
    bool enabled(Level level) const noexcept { return static_cast<std::uint8_t>(level) >= level_.load(std::memory_order_relaxed); }

    void write(Level level, const char* fmt, ...) noexcept __attribute__((format(printf, 3, 4)));
    void writeV(Level level, const char* fmt, va_list args) noexcept;
    void writeRaw(Level level, std::string_view message) noexcept;   // no formatting

    void flush();                      // every line produced before this call has reached the sinks when it returns
    void reopenSinks();                // e.g. after logrotate; run from a normal thread, not a signal handler
    void shutdown() noexcept;          // drain, join, stop accepting lines; idempotent

    std::uint64_t producedLines() const noexcept;
    std::uint64_t writtenLines() const noexcept { return consumed_.load(std::memory_order_acquire); }
    std::uint64_t droppedLines() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t truncatedLines() const noexcept { return truncated_.load(std::memory_order_relaxed); }

    // Process-wide instance (console sink on stderr unless LG_LEVEL / LG_FILE say otherwise), created on first use.
    static Logger& instance();

private:
    struct Ring;
    friend struct RingHandle;
    Ring* ringForThisThread() noexcept;
    void retireRing(Ring* ring) noexcept;
    void consumerLoop();
    bool drainOnce(bool all);
    void wakeConsumer() noexcept;

    const LoggerOptions options_;
    const std::uint64_t gen_;
    std::atomic<std::uint8_t> level_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> wakePending_{false};
    std::atomic<bool> consumerParked_{false};
    std::atomic<unsigned> flushWaiters_{0};
    std::atomic<std::uint64_t> dropped_{0}, truncated_{0}, consumed_{0};
    std::atomic<std::uint32_t> wakeWord_{0}, progressWord_{0}, started_{0};

    mutable std::mutex ringsMu_;
    std::vector<Ring*> rings_;
    std::uint64_t retiredProduced_ = 0;

    std::mutex sinksMu_;
    std::vector<std::unique_ptr<Sink>> sinks_;

    std::thread consumer_;
    std::mutex shutdownMu_;
};

}  // namespace lg

#define LG_LOG_(logger, lvl, ...)                                                       \
    do {                                                                                \
        if (static_cast<int>(lvl) >= LG_MIN_LEVEL && (logger).enabled(lvl))             \
            (logger).write(lvl, __VA_ARGS__);                                           \
    } while (0)
#define LG_TRACE(...) LG_LOG_(::lg::Logger::instance(), ::lg::Level::Trace, __VA_ARGS__)
#define LG_DEBUG(...) LG_LOG_(::lg::Logger::instance(), ::lg::Level::Debug, __VA_ARGS__)
#define LG_INFO(...) LG_LOG_(::lg::Logger::instance(), ::lg::Level::Info, __VA_ARGS__)
#define LG_WARN(...) LG_LOG_(::lg::Logger::instance(), ::lg::Level::Warn, __VA_ARGS__)
#define LG_ERROR(...) LG_LOG_(::lg::Logger::instance(), ::lg::Level::Error, __VA_ARGS__)
