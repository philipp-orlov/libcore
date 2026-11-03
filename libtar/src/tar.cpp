// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "tar/tar.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>

#include "sys/errno.hpp"
#include "sys/process.hpp"

namespace tar {

namespace {

constexpr std::size_t kBlock = 512;

[[noreturn]] void dataLoss(const std::string& what, std::uint64_t offset) {
    throw base::Error(base::Code::DataLoss, "tar: " + what + " at offset " + std::to_string(offset));
}

bool isZero(const unsigned char* block) {
    for (std::size_t i = 0; i < kBlock; ++i)
        if (block[i])
            return false;
    return true;
}

std::string_view cString(const char* field, std::size_t length) { return {field, ::strnlen(field, length)}; }

// Octal ("  0001234\0") or GNU base-256 (high bit set). Anything else is an error.
std::uint64_t parseNumber(const char* field, std::size_t length, std::uint64_t offset, const char* what) {
    const auto* p = reinterpret_cast<const unsigned char*>(field);
    if (p[0] & 0x80u) {
        if (p[0] & 0x40u)
            dataLoss(std::string("negative ") + what, offset);
        std::uint64_t v = p[0] & 0x3Fu;
        for (std::size_t i = 1; i < length; ++i) {
            if (v >> 56)
                dataLoss(std::string(what) + " overflows 64 bits", offset);
            v = (v << 8) | p[i];
        }
        return v;
    }
    std::size_t i = 0;
    while (i < length && p[i] == ' ')
        ++i;
    std::uint64_t v = 0;
    bool any = false;
    for (; i < length; ++i) {
        const unsigned char c = p[i];
        if (c == 0 || c == ' ')
            break;
        if (c < '0' || c > '7')
            dataLoss(std::string("bad digit in ") + what, offset);
        if (v >> 61)
            dataLoss(std::string(what) + " overflows 64 bits", offset);
        v = (v << 3) | (c - '0');
        any = true;
    }
    (void)any;
    return v;
}

struct Parsed {
    char type = '0';
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::string name;      // ustar prefix + name
};

void parseHeader(const unsigned char* block, std::uint64_t offset, Parsed& out) {
    const char* h = reinterpret_cast<const char*>(block);
    std::uint64_t unsignedSum = 0;
    std::int64_t signedSum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const unsigned char c = (i >= 148 && i < 156) ? ' ' : block[i];
        unsignedSum += c;
        signedSum += static_cast<signed char>(c);
    }
    const std::uint64_t stored = parseNumber(h + 148, 8, offset, "checksum");
    if (stored != unsignedSum && static_cast<std::int64_t>(stored) != signedSum)
        dataLoss("header checksum mismatch", offset);
    out.type = h[156];
    out.size = parseNumber(h + 124, 12, offset, "size");
    out.mtime = parseNumber(h + 136, 12, offset, "mtime");
    const std::string_view name = cString(h, 100);
    out.name.assign(name);
    if (std::memcmp(h + 257, "ustar", 5) == 0) {
        const std::string_view prefix = cString(h + 345, 155);
        if (!prefix.empty()) {
            out.name.insert(0, "/");
            out.name.insert(0, prefix);
        }
    }
}

bool carriesPayload(char type) {
    switch (type) {
        case '1': case '2': case '3': case '4': case '5': case '6': return false;   // links, devices, directories, fifos
        default: return true;
    }
}

struct PaxState {
    bool hasPath = false, hasSize = false, hasMtime = false;
    std::string path;
    std::uint64_t size = 0, mtime = 0;
};

// "<len> key=value\n" records. A record is <digits> ' ' <body> '\n' and <len> counts all of it.
void parsePax(const unsigned char* data, std::size_t n, std::uint64_t offset, const Limits& limits, PaxState& pax) {
    std::size_t pos = 0;
    while (pos < n) {
        std::size_t i = pos, length = 0;
        while (i < n && data[i] >= '0' && data[i] <= '9') {
            if (length > (static_cast<std::size_t>(-1) - 9) / 10)
                dataLoss("pax record length overflows", offset + pos);
            length = length * 10 + (data[i++] - '0');
        }
        const std::size_t digits = i - pos;
        if (digits == 0 || i >= n || data[i] != ' ' || length < digits + 2 || length > n - pos || data[pos + length - 1] != '\n')
            dataLoss("malformed pax record", offset + pos);
        const std::string_view body(reinterpret_cast<const char*>(data) + i + 1, length - digits - 2);
        const std::size_t eq = body.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = body.substr(0, eq), value = body.substr(eq + 1);
            if (key == "path") {
                if (value.size() > limits.maxNameBytes)
                    dataLoss("pax path longer than the limit", offset + pos);
                pax.path.assign(value);
                pax.hasPath = true;
            } else if (key == "size") {
                std::uint64_t v = 0;
                const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
                if (value.empty() || r.ec != std::errc{} || r.ptr != value.data() + value.size())
                    dataLoss("pax size is not a number", offset + pos);
                pax.size = v;
                pax.hasSize = true;
            } else if (key == "mtime") {
                // "1700000000.123456": the integer part is what we keep.
                const std::size_t dot = value.find('.');
                const std::string_view whole = value.substr(0, dot);
                std::uint64_t v = 0;
                const auto r = std::from_chars(whole.data(), whole.data() + whole.size(), v);
                if (whole.empty() || r.ec != std::errc{} || r.ptr != whole.data() + whole.size())
                    dataLoss("pax mtime is not a number", offset + pos);
                pax.mtime = v;
                pax.hasMtime = true;
            }
        }
        pos += length;
    }
}

// ---- sources ----

class FileSource final : public ByteSource {
public:
    explicit FileSource(const std::string& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            const int e = errno;
            throw base::Error(base::Code::NotFound, sys::errnoMessage(("tar: cannot open " + path).c_str(), e), static_cast<std::uint32_t>(e));
        }
    }
    ~FileSource() override { if (fd_ >= 0) ::close(fd_); }
    std::size_t read(void* buffer, std::size_t size) override {
        for (;;) {
            const ssize_t n = ::read(fd_, buffer, size);
            if (n >= 0)
                return static_cast<std::size_t>(n);
            if (errno == EINTR)
                continue;
            const int e = errno;
            throw base::Error(base::Code::Internal, sys::errnoMessage(("tar: read error in " + path_).c_str(), e), static_cast<std::uint32_t>(e));
        }
    }
private:
    std::string path_;
    int fd_ = -1;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(base::Slice bytes) : bytes_(bytes) {}
    std::size_t read(void* buffer, std::size_t size) override {
        const std::size_t n = std::min(size, bytes_.size() - pos_);
        if (n)
            std::memcpy(buffer, bytes_.data() + pos_, n);
        pos_ += n;
        return n;
    }
private:
    base::Slice bytes_;
    std::size_t pos_ = 0;
};

class ProcessSource final : public ByteSource {
public:
    explicit ProcessSource(std::vector<std::string> argv) : name_(argv.empty() ? std::string() : argv[0]) {
        sys::Process::Options o;
        o.argv = std::move(argv);
        process_ = sys::Process::spawn(o);
    }
    std::size_t read(void* buffer, std::size_t size) override {
        if (done_)
            return 0;
        const long n = process_.readSome(buffer, size);
        if (n > 0)
            return static_cast<std::size_t>(n);
        done_ = true;
        const int status = process_.wait();
        if (status != 0)
            throw base::Error(base::Code::DataLoss, "tar: command '" + name_ + "' failed with status " + std::to_string(status));
        return 0;
    }
private:
    std::string name_;
    sys::Process process_;
    bool done_ = false;
};

class FileSink final : public ByteSink {
public:
    FileSink(const std::string& path, unsigned mode) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd_ < 0) {
            const int e = errno;
            throw base::Error(base::Code::Internal, sys::errnoMessage(("tar: cannot create " + path).c_str(), e), static_cast<std::uint32_t>(e));
        }
    }
    ~FileSink() override { if (fd_ >= 0) ::close(fd_); }
    void write(const void* data, std::size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size) {
            const ssize_t n = ::write(fd_, p, size);
            if (n < 0) {
                if (errno == EINTR) continue;
                const int e = errno;
                throw base::Error(base::Code::Internal, sys::errnoMessage("tar: write error", e), static_cast<std::uint32_t>(e));
            }
            p += n;
            size -= static_cast<std::size_t>(n);
        }
    }
private:
    int fd_ = -1;
};

class StringSink final : public ByteSink {
public:
    explicit StringSink(std::string* out) : out_(out) {}
    void write(const void* data, std::size_t size) override { out_->append(static_cast<const char*>(data), size); }
private:
    std::string* out_;
};

}  // namespace

std::unique_ptr<ByteSource> openFileSource(const std::string& path) { return std::make_unique<FileSource>(path); }
std::unique_ptr<ByteSource> makeMemorySource(base::Slice bytes) { return std::make_unique<MemorySource>(bytes); }
std::unique_ptr<ByteSource> makeProcessSource(std::vector<std::string> argv) { return std::make_unique<ProcessSource>(std::move(argv)); }
std::unique_ptr<ByteSink> openFileSink(const std::string& path, unsigned mode) { return std::make_unique<FileSink>(path, mode); }
std::unique_ptr<ByteSink> makeStringSink(std::string* out) { return std::make_unique<StringSink>(out); }

// ---- streaming Reader ----

Reader::Reader(std::unique_ptr<ByteSource> source, Limits limits) : source_(std::move(source)), limits_(limits) {
    name_.reserve(256);
}

void Reader::fail(const std::string& what) const { dataLoss(what, offset_); }

bool Reader::readHeader(unsigned char* block) {
    std::size_t got = 0;
    while (got < kBlock) {
        const std::size_t n = source_->read(block + got, kBlock - got);
        if (n == 0)
            break;
        got += n;
    }
    offset_ += got;
    if (got == 0)
        return false;
    if (got < kBlock)
        fail("truncated header");
    return true;
}

void Reader::discard(std::uint64_t bytes) {
    unsigned char scratch[4096];
    while (bytes) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, sizeof scratch));
        const std::size_t n = source_->read(scratch, want);
        if (n == 0)
            fail("archive ends inside an entry");
        offset_ += n;
        bytes -= n;
    }
}

void Reader::skipPayload() {
    discard(payloadLeft_ + paddingLeft_);
    payloadLeft_ = paddingLeft_ = 0;
}

std::size_t Reader::read(void* dst, std::size_t n) {
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, payloadLeft_));
    std::size_t total = 0;
    while (total < n) {
        const std::size_t r = source_->read(static_cast<char*>(dst) + total, n - total);
        if (r == 0)
            fail("archive ends inside an entry");
        total += r;
        offset_ += r;
    }
    payloadLeft_ -= total;
    return total;
}

bool Reader::next(Entry& entry) {
    if (finished_)
        return false;
    skipPayload();
    PaxState pax;
    std::string longName;
    bool haveLong = false;
    for (;;) {
        unsigned char block[kBlock];
        if (!readHeader(block)) {
            finished_ = true;
            return false;
        }
        if (isZero(block)) {
            finished_ = true;
            return false;
        }
        Parsed h;
        parseHeader(block, offset_ - kBlock, h);
        const std::uint64_t padded = (h.size + kBlock - 1) / kBlock * kBlock;
        if (h.type == 'x' || h.type == 'g' || h.type == 'L' || h.type == 'K') {
            if (h.size > limits_.maxPaxBytes)
                fail("extended header larger than the limit");
            meta_.resize(static_cast<std::size_t>(padded));
            std::size_t got = 0;
            while (got < meta_.size()) {
                const std::size_t r = source_->read(meta_.data() + got, meta_.size() - got);
                if (r == 0)
                    fail("archive ends inside an extended header");
                got += r;
            }
            offset_ += got;
            const std::uint64_t at = offset_ - got;
            if (h.type == 'x')
                parsePax(meta_.data(), static_cast<std::size_t>(h.size), at, limits_, pax);
            else if (h.type == 'L') {
                const std::string_view n = cString(reinterpret_cast<const char*>(meta_.data()), static_cast<std::size_t>(h.size));
                if (n.size() > limits_.maxNameBytes)
                    fail("GNU long name longer than the limit");
                longName.assign(n);
                haveLong = true;
            }
            continue;   // 'g' (global) and 'K' (long link name) are validated by size only
        }
        if (h.name.size() > limits_.maxNameBytes)
            fail("name longer than the limit");
        std::uint64_t size = h.size;
        if (pax.hasSize)
            size = pax.size;
        const bool payload = carriesPayload(h.type);
        if (!payload)
            size = 0;
        if (size > limits_.maxEntryBytes)
            fail("entry larger than the limit");
        name_ = pax.hasPath ? pax.path : (haveLong ? longName : h.name);
        entry.name = name_;
        entry.size = size;
        entry.mtime = pax.hasMtime ? pax.mtime : h.mtime;
        entry.type = h.type;
        entry.dataOffset = offset_;
        payloadLeft_ = size;
        paddingLeft_ = (size + kBlock - 1) / kBlock * kBlock - size;
        return true;
    }
}

// ---- MemoryReader ----

bool MemoryReader::next(Entry& entry) {
    if (finished_)
        return false;
    PaxState pax;
    std::string longName;
    bool haveLong = false;
    for (;;) {
        if (pos_ == data_.size() || (pos_ + kBlock <= data_.size() && isZero(reinterpret_cast<const unsigned char*>(data_.data()) + pos_))) {
            finished_ = true;
            return false;
        }
        if (data_.size() - pos_ < kBlock)
            dataLoss("truncated header", pos_);
        const auto* block = reinterpret_cast<const unsigned char*>(data_.data()) + pos_;
        Parsed h;
        parseHeader(block, pos_, h);
        const std::uint64_t headerAt = pos_;
        pos_ += kBlock;
        const std::uint64_t padded = (h.size + kBlock - 1) / kBlock * kBlock;
        if (h.type == 'x' || h.type == 'g' || h.type == 'L' || h.type == 'K') {
            if (h.size > limits_.maxPaxBytes)
                dataLoss("extended header larger than the limit", headerAt);
            if (padded > data_.size() - pos_ || h.size > padded)
                dataLoss("archive ends inside an extended header", pos_);
            const auto* body = reinterpret_cast<const unsigned char*>(data_.data()) + pos_;
            if (h.type == 'x')
                parsePax(body, static_cast<std::size_t>(h.size), pos_, limits_, pax);
            else if (h.type == 'L') {
                const std::string_view n = cString(reinterpret_cast<const char*>(body), static_cast<std::size_t>(h.size));
                if (n.size() > limits_.maxNameBytes)
                    dataLoss("GNU long name longer than the limit", pos_);
                longName.assign(n);
                haveLong = true;
            }
            pos_ += padded;
            continue;
        }
        if (h.name.size() > limits_.maxNameBytes)
            dataLoss("name longer than the limit", headerAt);
        std::uint64_t size = pax.hasSize ? pax.size : h.size;
        if (!carriesPayload(h.type))
            size = 0;
        if (size > limits_.maxEntryBytes)
            dataLoss("entry larger than the limit", headerAt);
        const std::uint64_t paddedSize = (size + kBlock - 1) / kBlock * kBlock;
        if (size > data_.size() - pos_)
            dataLoss("archive ends inside an entry", pos_);
        name_ = pax.hasPath ? pax.path : (haveLong ? longName : h.name);
        entry.name = name_;
        entry.size = size;
        entry.mtime = pax.hasMtime ? pax.mtime : h.mtime;
        entry.type = h.type;
        entry.dataOffset = pos_;
        // The padding may be missing at the very end of a file that was cut off exactly after the data: tolerated.
        pos_ += std::min<std::uint64_t>(paddedSize, data_.size() - pos_);
        return true;
    }
}

base::Slice MemoryReader::payload(const Entry& entry) const {
    return data_.subspan(static_cast<std::size_t>(entry.dataOffset), static_cast<std::size_t>(entry.size));
}

// ---- Writer ----

namespace {
void putOctal(char* dst, std::size_t width, std::uint64_t v) {
    for (std::size_t i = width - 1; i-- > 0;) {
        dst[i] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    }
    dst[width - 1] = '\0';
}

void sealChecksum(unsigned char* block) {
    std::memset(block + 148, ' ', 8);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i)
        sum += block[i];
    // Six octal digits, NUL, space: what GNU tar writes.
    char* c = reinterpret_cast<char*>(block) + 148;
    for (int i = 5; i >= 0; --i) {
        c[i] = static_cast<char>('0' + (sum & 7));
        sum >>= 3;
    }
    c[6] = '\0';
    c[7] = ' ';
}

void makeHeader(unsigned char* block, std::string_view name, std::string_view prefix, std::uint64_t size, std::uint64_t mtime, char type) {
    std::memset(block, 0, kBlock);
    char* h = reinterpret_cast<char*>(block);
    if (!name.empty())
        std::memcpy(h, name.data(), std::min<std::size_t>(name.size(), 100));
    std::memcpy(h + 100, "0000644", 7);
    std::memcpy(h + 108, "0000000", 7);
    std::memcpy(h + 116, "0000000", 7);
    putOctal(h + 124, 12, size);
    putOctal(h + 136, 12, mtime);
    h[156] = type;
    std::memcpy(h + 257, "ustar", 6);   // magic "ustar\0"
    std::memcpy(h + 263, "00", 2);
    std::memcpy(h + 265, "core", 4);
    std::memcpy(h + 297, "core", 4);
    if (!prefix.empty())
        std::memcpy(h + 345, prefix.data(), std::min<std::size_t>(prefix.size(), 155));
    sealChecksum(block);
}

void appendPaxRecord(std::string& out, std::string_view key, std::string_view value) {
    // length counts the digits of itself: iterate to a fixed point.
    const std::size_t base = key.size() + value.size() + 3;   // ' ' '=' '\n'
    std::size_t len = base + 1;
    for (;;) {
        const std::size_t digits = std::to_string(len).size();
        if (base + digits == len)
            break;
        len = base + digits;
    }
    out += std::to_string(len);
    out += ' ';
    out.append(key);
    out += '=';
    out.append(value);
    out += '\n';
}
}  // namespace

void Writer::begin(std::string_view name, std::uint64_t size, std::uint64_t mtime) {
    if (open_)
        throw base::Error(base::Code::FailedPrecondition, "tar: begin() while an entry is open");
    if (name.empty() || name.find('\0') != std::string_view::npos)
        throw base::Error(base::Code::InvalidArgument, "tar: entry names must be non-empty and contain no NUL");
    std::string_view shortName = name, prefix;
    std::string pax;
    if (name.size() > 100) {
        // ustar can hold it when a '/' splits it into prefix (<= 155) and name (<= 100).
        bool split = false;
        for (std::size_t slash = name.size(); slash-- > 0 && !split;) {
            if (name[slash] != '/')
                continue;
            if (slash <= 155 && name.size() - slash - 1 <= 100 && name.size() - slash - 1 > 0) {
                prefix = name.substr(0, slash);
                shortName = name.substr(slash + 1);
                split = true;
            }
        }
        if (!split) {
            appendPaxRecord(pax, "path", name);
            shortName = name.substr(0, 100);
        }
    }
    constexpr std::uint64_t kOctalMax = (std::uint64_t{1} << 33) - 1;   // 11 octal digits
    std::uint64_t headerSize = size;
    if (size > kOctalMax) {
        appendPaxRecord(pax, "size", std::to_string(size));
        headerSize = 0;
    }
    unsigned char block[kBlock];
    if (!pax.empty()) {
        makeHeader(block, "PaxHeader", {}, pax.size(), mtime, 'x');
        sink_.write(block, kBlock);
        sink_.write(pax.data(), pax.size());
        const std::size_t padding = (kBlock - pax.size() % kBlock) % kBlock;
        if (padding) {
            const unsigned char zeros[kBlock] = {};
            sink_.write(zeros, padding);
        }
    }
    makeHeader(block, shortName, prefix, headerSize, mtime, '0');
    sink_.write(block, kBlock);
    size_ = size;
    written_ = 0;
    open_ = true;
}

void Writer::write(const void* data, std::size_t size) {
    if (!open_ || size > size_ - written_)
        throw base::Error(base::Code::FailedPrecondition, "tar: write beyond the declared entry size");
    sink_.write(data, size);
    written_ += size;
}

void Writer::end() {
    if (!open_ || written_ != size_)
        throw base::Error(base::Code::FailedPrecondition, "tar: entry ended with " + std::to_string(written_) + " of " + std::to_string(size_) + " bytes");
    const std::size_t padding = (kBlock - size_ % kBlock) % kBlock;
    if (padding) {
        const unsigned char zeros[kBlock] = {};
        sink_.write(zeros, padding);
    }
    open_ = false;
}

void Writer::finish() {
    if (open_)
        throw base::Error(base::Code::FailedPrecondition, "tar: finish() while an entry is open");
    const unsigned char zeros[2 * kBlock] = {};
    sink_.write(zeros, sizeof zeros);
}

}  // namespace tar
