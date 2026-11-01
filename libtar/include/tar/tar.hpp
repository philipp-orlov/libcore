// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Hardened tar: ustar with GNU long names / base-256 numbers and pax extended headers, for streaming (files,
// pipes, child processes) and for memory-mapped archives (zero copy).
//
//   * Every malformed input is base::Error(Code::DataLoss, "tar: ... at offset N"); nothing else escapes, and no
//     input makes the reader read outside the buffer it was given or allocate more than Limits allows.
//   * Header checksums are verified (signed and unsigned sum, as GNU tar does).
//   * pax records are validated before they are sliced (BUG-H2), numbers go through std::from_chars.
//   * A child process is started only by constructing ProcessSource explicitly, from an argv vector, never a shell
//     string (BUG-H3).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/slice.hpp"
#include "base/status.hpp"

namespace tar {

struct Limits {
    std::uint64_t maxPaxBytes = 64u << 10;     // size of one pax or GNU long-name record block
    std::uint64_t maxNameBytes = 4096;
    std::uint64_t maxEntryBytes = 1ull << 40;
};

struct Entry {
    std::string_view name;        // valid until the next call to next()
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::uint64_t dataOffset = 0; // offset of the payload in the archive
    char type = '0';              // '0' regular, '5' directory, '2' symlink, ...
    bool regular() const noexcept { return type == '0' || type == '\0' || type == '7'; }
};

// ---- sources ----
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Up to `size` bytes; 0 at end of stream. Throws base::Error on failure.
    virtual std::size_t read(void* buffer, std::size_t size) = 0;
};
std::unique_ptr<ByteSource> openFileSource(const std::string& path);
std::unique_ptr<ByteSource> makeMemorySource(base::Slice bytes);
// Runs argv[0] with argv (no shell); the stream is its stdout. At end of stream a non-zero exit status is an error.
std::unique_ptr<ByteSource> makeProcessSource(std::vector<std::string> argv);

// ---- streaming reader ----
class Reader {
public:
    explicit Reader(std::unique_ptr<ByteSource> source, Limits limits = {});
    // Next entry (any type except the metadata headers, which are folded into the entry that follows). False at the
    // end of the archive. Unread payload of the previous entry is skipped.
    bool next(Entry& entry);
    // Payload of the current entry; returns 0 when it is used up. Throws DataLoss if the archive ends early.
    std::size_t read(void* dst, std::size_t n);
    std::uint64_t remaining() const noexcept { return payloadLeft_; }
    void skipPayload();

private:
    bool readHeader(unsigned char* block);
    void discard(std::uint64_t bytes);
    [[noreturn]] void fail(const std::string& what) const;

    std::unique_ptr<ByteSource> source_;
    Limits limits_;
    std::uint64_t offset_ = 0;
    std::uint64_t payloadLeft_ = 0, paddingLeft_ = 0;
    bool finished_ = false;
    std::string name_;
    std::vector<unsigned char> meta_;
};

// ---- zero-copy reader over memory (a mapped file) ----
class MemoryReader {
public:
    explicit MemoryReader(base::Slice archive, Limits limits = {}) : data_(archive), limits_(limits) {}
    bool next(Entry& entry);
    base::Slice payload(const Entry& entry) const;    // points into the archive

private:
    base::Slice data_;
    Limits limits_;
    std::uint64_t pos_ = 0;
    bool finished_ = false;
    std::string name_;
};

// ---- writer ----
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void write(const void* data, std::size_t size) = 0;   // throws base::Error on failure
};
std::unique_ptr<ByteSink> openFileSink(const std::string& path, unsigned mode = 0644);
std::unique_ptr<ByteSink> makeStringSink(std::string* out);

class Writer {
public:
    explicit Writer(ByteSink& sink) : sink_(sink) {}
    // Starts a regular file of exactly `size` bytes. Names over 100 bytes use the ustar prefix when they split at a
    // '/', otherwise a pax header; sizes of 8 GiB and above use a pax size record.
    void begin(std::string_view name, std::uint64_t size, std::uint64_t mtime = 0);
    void write(const void* data, std::size_t size);
    void end();      // checks that exactly `size` bytes were written, then pads to 512
    void finish();   // two zero blocks
    void addFile(std::string_view name, base::Slice contents, std::uint64_t mtime = 0) {
        begin(name, contents.size(), mtime);
        write(contents.data(), contents.size());
        end();
    }

private:
    ByteSink& sink_;
    std::uint64_t size_ = 0, written_ = 0;
    bool open_ = false;
};

}  // namespace tar
