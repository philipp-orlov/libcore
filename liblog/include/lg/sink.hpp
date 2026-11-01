// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lg {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };
const char* levelName(Level level) noexcept;       // "TRACE", "DEBUG", ...
Level parseLevel(const char* text, Level fallback) noexcept;   // case-insensitive; "warning" and "off"/"none" accepted

// Receives complete, newline-terminated, escaped lines in batches. Called only from the logger's consumer thread.
class Sink {
public:
    virtual ~Sink() = default;
    // One or more whole lines. For a batch, `level` is the highest level among its lines; a sink that needs the
    // level of every line returns true from perLine() and then gets exactly one line per call.
    virtual void write(Level level, const char* data, std::size_t size) = 0;
    virtual bool perLine() const { return false; }
    virtual void flush() {}
    virtual void reopen() {}   // after log rotation by an external tool
};

std::unique_ptr<Sink> makeConsoleSink(int fd = 2);                          // write(2) on fd, no stdio buffering
std::unique_ptr<Sink> makeCallbackSink(std::function<void(Level, const char*, std::size_t)> fn);

struct FileSinkOptions {
    std::string path;
    unsigned mode = 0640;             // applied to files it creates (before the umask)
    std::uint64_t rotateBytes = 0;    // 0 = never; else rename to path.1 .. path.<keep> when the file reaches this size
    unsigned keep = 3;
};
// O_NOFOLLOW | O_CLOEXEC | O_APPEND. Returns null (and sets *error) when the file cannot be opened.
std::unique_ptr<Sink> makeFileSink(const FileSinkOptions& options, std::string* error = nullptr);

#if defined(__linux__) || defined(__APPLE__)
std::unique_ptr<Sink> makeSyslogSink(const char* ident);
#endif

}  // namespace lg
