// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sys/file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "base/status.hpp"
#include "sys/errno.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <malloc.h>  // _aligned_malloc / _aligned_free
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#endif
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace sys {
namespace {

enum class Fault { NotFound, InvalidArgument, Busy, IoError, OutOfMemory };
base::Code toCode(Fault f)
{
    switch (f) {
        case Fault::NotFound: return base::Code::NotFound;
        case Fault::InvalidArgument: return base::Code::InvalidArgument;
        case Fault::Busy: return base::Code::Unavailable;
        case Fault::OutOfMemory: return base::Code::ResourceExhausted;
        default: return base::Code::Internal;
    }
}



#if !defined(_WIN32)
#ifdef MAP_NORESERVE
constexpr int kNoReserve = MAP_NORESERVE;
#else
constexpr int kNoReserve = 0;
#endif
#endif

#ifdef _WIN32
[[noreturn]] void fail(Fault code, const char* what)
{
    const DWORD e = ::GetLastError();
    throw base::Error(toCode(code), std::string(what) + ": win32 error " + std::to_string(e), static_cast<std::uint32_t>(e));
}
#else
[[noreturn]] void fail(Fault code, const char* what)
{
    const int e = errno;
    throw base::Error(toCode(code), std::string(what) + ": " + std::strerror(e), static_cast<std::uint32_t>(e));
}
#endif

}  // namespace

std::size_t systemPageSize() noexcept
{
#ifdef _WIN32
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096;
#else
    const long v = ::sysconf(_SC_PAGESIZE);
    return v > 0 ? std::size_t(v) : 4096;
#endif
}

#ifdef _WIN32

namespace {

/// The byte openFile() locks: past any file this store could have.
OVERLAPPED lockByte() noexcept
{
    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0x7fffffffu;
    return ov;
}

}  // namespace

FileHandle openFile(const std::filesystem::path& path, bool readOnly, bool create,
                    bool exclusiveCreate, bool lock, bool replaceable, unsigned /*mode*/)
{
    const DWORD access = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | (replaceable ? FILE_SHARE_DELETE : 0);
    DWORD disp;
    if (readOnly) {
        disp = OPEN_EXISTING;
    } else if (exclusiveCreate) {
        disp = CREATE_NEW;
    } else {
        disp = create ? OPEN_ALWAYS : OPEN_EXISTING;
    }
    HANDLE h =
        ::CreateFileW(path.c_str(), access, share, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            throw base::Error(toCode(Fault::NotFound), "cannot open " + path.string());
        if (e == ERROR_FILE_EXISTS)
            throw base::Error(toCode(Fault::InvalidArgument), "already exists: " + path.string());
        fail(Fault::IoError, "CreateFileW");
    }
    if (!lock)
        return h;
    // One process at a time: exclusive for writers, shared for readers. Windows
    // byte-range locks are mandatory, so the lock covers one byte far past any
    // data rather than the file: another handle, this process's own included,
    // may still read the store (a checkpoint, a snapshot copy).
    OVERLAPPED ov = lockByte();
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (readOnly ? 0 : LOCKFILE_EXCLUSIVE_LOCK);
    if (!::LockFileEx(h, flags, 0, 1, 0, &ov)) {
        ::CloseHandle(h);
        throw base::Error(toCode(Fault::Busy), "store is already open in another process: " + path.string());
    }
    return h;
}

void closeFile(FileHandle h) noexcept
{
    if (h && h != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov = lockByte();
        ::UnlockFileEx(h, 0, 1, 0, &ov);  // fails harmlessly on a handle that took no lock
        ::CloseHandle(h);
    }
}

std::uint64_t fileSize(FileHandle h)
{
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(h, &li))
        fail(Fault::IoError, "GetFileSizeEx");
    return std::uint64_t(li.QuadPart);
}

void resizeFile(FileHandle h, std::uint64_t bytes)
{
    observeIo(IoEvent::Resize, std::size_t(bytes));
    LARGE_INTEGER li;
    li.QuadPart = LONGLONG(bytes);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        fail(Fault::IoError, "SetFilePointerEx");
    if (!::SetEndOfFile(h))
        fail(Fault::IoError, "SetEndOfFile");
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->resized(h, bytes);
}

void syncFile(FileHandle h, bool)
{
    observeIo(IoEvent::Sync);
    if (!::FlushFileBuffers(h))
        fail(Fault::IoError, "FlushFileBuffers");
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->flushed(h, 0, 0);
}

void reserveSpace(FileHandle, std::uint64_t, std::uint64_t)
{
    observeIo(IoEvent::Reserve);
}

bool isNetworkFilesystem(FileHandle)
{
    return false;
}

bool isNetworkFilesystemType(unsigned long) noexcept
{
    return false;
}

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target, bool replace)
{
    observeIo(IoEvent::Replace);
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->replacing(source, target);
    // POSIX rename semantics where the volume has them (NTFS, Windows 10 1709
    // on): the target may still be open, as a replica's store is while a bundle
    // is applied to it, which MoveFileEx refuses with ERROR_ACCESS_DENIED. The
    // structure is FILE_RENAME_INFO with its Flags member, spelled out because
    // older SDK and MinGW headers lack it.
    struct RenameInfo
    {
        DWORD flags;
        HANDLE root;
        DWORD nameBytes;
        WCHAR name[1];
    };
    constexpr DWORD kReplaceIfExists = 0x1, kPosixSemantics = 0x2;
    constexpr auto kFileRenameInfoEx = FILE_INFO_BY_HANDLE_CLASS(22);
    bool renamed = false;
    HANDLE h = ::CreateFileW(source.c_str(), DELETE | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const std::wstring name = std::filesystem::absolute(target).wstring();
        std::vector<std::byte> buffer(sizeof(RenameInfo) + name.size() * sizeof(WCHAR));
        auto* info = reinterpret_cast<RenameInfo*>(buffer.data());
        info->flags = (replace ? kReplaceIfExists : 0) | kPosixSemantics;
        info->root = nullptr;
        info->nameBytes = DWORD(name.size() * sizeof(WCHAR));
        std::memcpy(info->name, name.c_str(), info->nameBytes);
        renamed =
            ::SetFileInformationByHandle(h, kFileRenameInfoEx, info, DWORD(buffer.size())) != 0;
        ::CloseHandle(h);
    }
    if (!renamed &&
        !::MoveFileExW(source.c_str(), target.c_str(),
                       (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        fail(Fault::IoError, "MoveFileExW");
    observeIo(IoEvent::Replaced);
}

void readAt(FileHandle h, std::uint64_t offset, void* dst, std::size_t length)
{
    observeIo(IoEvent::Read, length);
    auto* p = static_cast<std::byte*>(dst);
    while (length) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset & 0xffffffffu);
        ov.OffsetHigh = DWORD(offset >> 32);
        const DWORD want = DWORD(length > 0x10000000u ? 0x10000000u : length);
        DWORD got = 0;
        if (!::ReadFile(h, p, want, &got, &ov))
            fail(Fault::IoError, "ReadFile");
        if (got == 0)
            throw base::Error(toCode(Fault::IoError), "ReadFile: unexpected end of file");
        p += got;
        offset += got;
        length -= got;
    }
}

void writeAt(FileHandle h, std::uint64_t offset, const void* src, std::size_t length)
{
    observeIo(IoEvent::Write, length);
    const auto* p = static_cast<const std::byte*>(src);
    while (length) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset & 0xffffffffu);
        ov.OffsetHigh = DWORD(offset >> 32);
        const DWORD want =
            DWORD(std::min<std::size_t>(length, ioCallback() ? 65536u : 0x10000000u));
        DWORD put = 0;
        if (!::WriteFile(h, p, want, &put, &ov))
            fail(Fault::IoError, "WriteFile");
        if (put == 0)
            throw base::Error(toCode(Fault::IoError), "WriteFile made no progress");
        recordMappedWrite(h, offset, p, put);
        observeIo(IoEvent::WriteProgress, put);
        p += put;
        offset += put;
        length -= put;
    }
}

void writeAtV(FileHandle h, std::uint64_t offset, const Span* parts, unsigned count)
{
    // No positional gather write on Win32 for buffered handles; the segments
    // are few and the payload one is large, so sequential writes cost the same.
    for (unsigned i = 0; i < count; ++i) {
        if (!parts[i].size)
            continue;
        writeAt(h, offset, parts[i].data, parts[i].size);
        offset += parts[i].size;
    }
}

Mapping mapFile(FileHandle h, std::size_t size, bool writable)
{
    Mapping m;
    const DWORD prot = writable ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE sec = ::CreateFileMappingW(h, nullptr, prot, DWORD(std::uint64_t(size) >> 32),
                                      DWORD(size & 0xffffffffu), nullptr);
    if (!sec)
        fail(Fault::IoError, "CreateFileMappingW");
    void* base = ::MapViewOfFile(sec, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, size);
    if (!base) {
        ::CloseHandle(sec);
        fail(Fault::IoError, "MapViewOfFile");
    }
    m.base = static_cast<std::byte*>(base);
    m.size = size;
    m.section = sec;
    return m;
}

Mapping mapFileReserved(FileHandle h, std::size_t size, std::size_t, bool writable)
{
    return mapFile(h, size, writable);
}

bool extendMapping(FileHandle, Mapping&, std::size_t, bool)
{
    return false;
}

void unmap(Mapping& m) noexcept
{
    if (m.base)
        ::UnmapViewOfFile(m.base);
    if (m.section)
        ::CloseHandle(m.section);
    m = Mapping{};
}

void flushRange(FileHandle h, const Mapping& m, std::size_t offset, std::size_t length)
{
    observeIo(IoEvent::MetaBarrier, length);
    if (!length)
        return;
    if (!::FlushViewOfFile(m.base + offset, length))
        fail(Fault::IoError, "FlushViewOfFile");
    if (!::FlushFileBuffers(h))
        fail(Fault::IoError, "FlushFileBuffers");
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->flushed(h, 0, 0);
}

void advise(const Mapping& m, std::size_t offset, std::size_t length, Advice a) noexcept
{
    // PrefetchVirtualMemory is Windows 8+; resolve it at run time so an older
    // SDK or system still builds and runs, just without the hint.
    if (a != Advice::WillNeed || !m.base || !length || offset >= m.size)
        return;
    struct RangeEntry
    {
        PVOID VirtualAddress;
        SIZE_T NumberOfBytes;
    };
    using Fn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, RangeEntry*, ULONG);
    static const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory")));
    if (!fn)
        return;
    if (offset + length > m.size)
        length = m.size - offset;
    RangeEntry e{m.base + offset, length};
    fn(::GetCurrentProcess(), 1, &e, 0);
}

#else  // ---------------------------------------------------------- POSIX ---

FileHandle openFile(const std::filesystem::path& path, bool readOnly, bool create,
                    bool exclusiveCreate, bool lock, bool, unsigned mode)
{
    int flags = (readOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW;
    if (!readOnly && create)
        flags |= O_CREAT;
    if (!readOnly && exclusiveCreate)
        flags |= O_CREAT | O_EXCL;
    // A name made just now is not durable until its directory is: try to create exclusively
    // first, so a new file is known to be new.
    bool created = !readOnly && exclusiveCreate;
    int fd = -1;
    if (!readOnly && create && !exclusiveCreate) {
        fd = ::open(path.c_str(), flags | O_EXCL, mode_t(mode));
        if (fd >= 0)
            created = true;
        else if (errno != EEXIST)
            ;  // reported below, by the ordinary open
    }
    if (fd < 0)
        fd = ::open(path.c_str(), flags, mode_t(mode));
    if (fd < 0) {
        if (errno == ENOENT)
            throw base::Error(toCode(Fault::NotFound), "cannot open " + path.string());
        if (errno == EEXIST)
            throw base::Error(toCode(Fault::InvalidArgument), "already exists: " + path.string());
        if (errno == ELOOP)
            throw base::Error(toCode(Fault::InvalidArgument), "refusing to follow a symbolic link: " + path.string());
        fail(Fault::IoError, "open");
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail(Fault::IoError, "fstat");
    }
    if (st.st_uid != ::geteuid() && ::geteuid() != 0) {
        ::close(fd);
        throw base::Error(toCode(Fault::InvalidArgument),
                    "not owned by the effective user: " + path.string());
    }
    // A file created just now gets the requested mode whatever the umask.
    if (!readOnly && create && st.st_size == 0 && (st.st_mode & 07777) != mode_t(mode))
        (void)::fchmod(fd, mode_t(mode));
    if (created) {
        const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            // EINVAL: some filesystems cannot sync a directory; the name is as durable as it gets.
            if (::fsync(directory) != 0 && errno != EINVAL) {
                const int saved = errno;
                ::close(directory);
                ::close(fd);
                errno = saved;
                fail(Fault::IoError, "fsync parent directory");
            }
            ::close(directory);
        }
    }
    if (!lock)
        return fd;
    // Advisory whole-file lock in place of a .lck side file.
    if (::flock(fd, (readOnly ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
        ::close(fd);
        throw base::Error(toCode(Fault::Busy), "store is already open in another process: " + path.string());
    }
    return fd;
}

void closeFile(FileHandle fd) noexcept
{
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
}

std::uint64_t fileSize(FileHandle fd)
{
    struct stat st;
    if (::fstat(fd, &st) != 0)
        fail(Fault::IoError, "fstat");
    return std::uint64_t(st.st_size);
}

void resizeFile(FileHandle fd, std::uint64_t bytes)
{
    observeIo(IoEvent::Resize, std::size_t(bytes));
    int rc;
    do
        rc = ::ftruncate(fd, off_t(bytes));
    while (rc != 0 && errno == EINTR);
    if (rc != 0)
        fail(Fault::IoError, "ftruncate");
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->resized(fd, bytes);
}

void syncFile(FileHandle fd, bool metadata)
{
    observeIo(IoEvent::Sync);
    int rc;
    do
        rc = metadata ? ::fsync(fd) : ::fdatasync(fd);
    while (rc != 0 && errno == EINTR);
    if (rc != 0)
        fail(Fault::IoError, "fsync");
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->flushed(fd, 0, 0);
}

void reserveSpace(FileHandle fd, std::uint64_t offset, std::uint64_t length)
{
    observeIo(IoEvent::Reserve, std::size_t(length));
#ifdef __linux__
    if (!length)
        return;
    // fallocate rather than posix_fallocate: glibc's fallback writes every
    // block one by one on a filesystem without support, which is far too slow.
    int rc;
    do
        rc = ::fallocate(fd, 0, off_t(offset), off_t(length));
    while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        if (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL || errno == ENODEV)
            return;
        fail(Fault::IoError, "fallocate");
    }
#else
    (void)fd;
    (void)offset;
    (void)length;
#endif
}

bool isNetworkFilesystemType(unsigned long type) noexcept
{
    switch (type) {
        case 0x6969:      // NFS
        case 0x517B:      // SMB
        case 0xFF534D42:  // CIFS
        case 0xFE534D42:  // SMB2
        case 0x65735546:  // FUSE
        case 0x5346414F:  // AFS
        case 0x00C36400:  // Ceph
        case 0x0BD00BD0:  // Lustre
        case 0x01021997:  // 9p
        case 0x73757245:  // Coda
        case 0x564C:      // NCP
        case 0x47504653:  // GPFS
        case 0x7461636F:  // OCFS2
        case 0x01161970:  // GFS2
        case 0x786F4256:  // VirtualBox shared folders
            return true;
        default: return false;
    }
}

bool isNetworkFilesystem(FileHandle fd)
{
#ifdef __linux__
    struct statfs fs;
    return ::fstatfs(fd, &fs) == 0 && isNetworkFilesystemType(static_cast<unsigned long>(fs.f_type));
#else
    (void)fd;
    return false;
#endif
}

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target, bool replace)
{
    observeIo(IoEvent::Replace);
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->replacing(source, target);
    if (replace) {
        if (::rename(source.c_str(), target.c_str()) != 0)
            fail(Fault::IoError, "rename");
    } else {
        if (::link(source.c_str(), target.c_str()) != 0) {
            if (errno == EEXIST)
                throw base::Error(toCode(Fault::InvalidArgument), "already exists: " + target.string());
            fail(Fault::IoError, "link");
        }
        if (::unlink(source.c_str()) != 0)
            fail(Fault::IoError, "unlink");
    }
    const auto parent = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    observeIo(IoEvent::Replaced);
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        fail(Fault::IoError, "open parent directory");
    const int result = ::fsync(directory);
    const int saved = errno;
    ::close(directory);
    if (result != 0) {
        errno = saved;
        fail(Fault::IoError, "fsync parent directory");
    }
}

void readAt(FileHandle fd, std::uint64_t offset, void* dst, std::size_t length)
{
    observeIo(IoEvent::Read, length);
    auto* p = static_cast<std::byte*>(dst);
    while (length) {
        const ssize_t n = ::pread(fd, p, length, off_t(offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(Fault::IoError, "pread");
        }
        if (n == 0)
            throw base::Error(toCode(Fault::IoError), "pread: unexpected end of file");
        p += n;
        offset += std::uint64_t(n);
        length -= std::size_t(n);
    }
}

void writeAt(FileHandle fd, std::uint64_t offset, const void* src, std::size_t length)
{
    observeIo(IoEvent::Write, length);
    const auto* p = static_cast<const std::byte*>(src);
    while (length) {
        const ssize_t n = ::pwrite(
            fd, p, ioCallback() ? std::min<std::size_t>(length, 65536) : length, off_t(offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(Fault::IoError, "pwrite");
        }
        if (n == 0)
            throw base::Error(toCode(Fault::IoError), "pwrite made no progress");
        recordMappedWrite(fd, offset, p, std::size_t(n));
        observeIo(IoEvent::WriteProgress, std::size_t(n));
        p += n;
        offset += std::uint64_t(n);
        length -= std::size_t(n);
    }
}

void writeAtV(FileHandle fd, std::uint64_t offset, const Span* parts, unsigned count)
{
    for (unsigned index = 0; index < count; ++index)
        observeIo(IoEvent::Write, parts[index].size);
    // Gathered in groups small enough for a stack array and well under IOV_MAX;
    // a commit's longest contiguous run may hold hundreds of pages.
    constexpr unsigned kMaxIov = 64;
    iovec iov[kMaxIov];
    unsigned taken = 0;
    while (taken < count) {
        unsigned n = 0;
        std::size_t total = 0;
        for (; taken < count && n < kMaxIov; ++taken) {
            if (!parts[taken].size)
                continue;
            iov[n].iov_base = const_cast<void*>(parts[taken].data);
            iov[n].iov_len = parts[taken].size;
            total += parts[taken].size;
            ++n;
        }
        if (!n)
            continue;

        while (total) {
            const ssize_t w = ::pwritev(fd, iov, int(n), off_t(offset));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                fail(Fault::IoError, "pwritev");
            }
            if (w == 0)
                throw base::Error(toCode(Fault::IoError), "pwritev made no progress");
            if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed)) {
                std::uint64_t at = offset;
                std::size_t left = std::size_t(w);
                for (unsigned i = 0; i < n && left; ++i) {
                    const std::size_t part = std::min(left, iov[i].iov_len);
                    r->written(fd, at, iov[i].iov_base, part);
                    at += part;
                    left -= part;
                }
            }
            observeIo(IoEvent::WriteProgress, std::size_t(w));
            offset += std::uint64_t(w);
            total -= std::size_t(w);
            // Drop whole segments the kernel consumed, then trim the partial one.
            std::size_t done = std::size_t(w);
            unsigned first = 0;
            while (first < n && done >= iov[first].iov_len) {
                done -= iov[first].iov_len;
                ++first;
            }
            if (first) {
                for (unsigned i = first; i < n; ++i)
                    iov[i - first] = iov[i];
                n -= first;
            }
            if (n && done) {
                iov[0].iov_base = static_cast<std::byte*>(iov[0].iov_base) + done;
                iov[0].iov_len -= done;
            }
        }
    }
}

Mapping mapFile(FileHandle fd, std::size_t size, bool writable)
{
    const int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* base = ::mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        fail(Fault::IoError, "mmap");
    Mapping m;
    m.base = static_cast<std::byte*>(base);
    m.size = size;
    return m;
}

Mapping mapFileReserved(FileHandle fd, std::size_t size, std::size_t reserve, bool writable)
{
    // Only address space: PROT_NONE private memory is not charged against the
    // commit limit, and MAP_NORESERVE asks for no swap either. The file is
    // then mapped over the front of it, and later extents follow in place.
    const std::size_t sp = systemPageSize();
    const std::size_t mapped = (size + sp - 1) & ~(sp - 1);
    const std::size_t span = std::max(mapped, (reserve + sp - 1) & ~(sp - 1));
    void* base = ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | kNoReserve, -1, 0);
    if (base == MAP_FAILED)
        fail(Fault::IoError, "mmap (address space reservation for maxSize)");
    const int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    if (mapped && ::mmap(base, mapped, prot, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        const int e = errno;
        ::munmap(base, span);
        errno = e;
        fail(Fault::IoError, "mmap");
    }
    Mapping m;
    m.base = static_cast<std::byte*>(base);
    m.size = size;
    m.reserved = span;
    return m;
}

bool extendMapping(FileHandle fd, Mapping& m, std::size_t size, bool writable)
{
    if (!m.reserved || size > m.reserved)
        return false;
    // The system page that holds the old end is already mapped; it is the
    // same page-cache page whatever the file size, so it needs no remapping.
    const std::size_t sp = systemPageSize();
    const std::size_t from = (m.size + sp - 1) & ~(sp - 1);
    const std::size_t to = (size + sp - 1) & ~(sp - 1);
    if (to > from) {
        const int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
        if (::mmap(m.base + from, to - from, prot, MAP_SHARED | MAP_FIXED, fd, off_t(from)) ==
            MAP_FAILED) {
            const int e = errno;
            // A failed MAP_FIXED may already have dropped the reservation there;
            // take the range back so no other mapping can land inside ours.
            (void)::mmap(m.base + from, to - from, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | kNoReserve, -1, 0);
            errno = e;
            fail(Fault::IoError, "mmap (extend)");
        }
    }
    if (size > m.size)
        m.size = size;
    return true;
}

void unmap(Mapping& m) noexcept
{
    if (m.base)
        ::munmap(m.base, m.reserved ? m.reserved : m.size);
    m = Mapping{};
}

void flushRange(FileHandle fd, const Mapping& m, std::size_t offset, std::size_t length)
{
    observeIo(IoEvent::MetaBarrier, length);
    if (!length)
        return;
    // msync needs a system-page-aligned start; widen the window outwards.
    const std::size_t sp = systemPageSize();
    const std::size_t begin = offset & ~(sp - 1);
    std::size_t end = offset + length;
    end = (end + sp - 1) & ~(sp - 1);
    if (end > m.size)
        end = m.size;
    if (begin >= end)
        return;
    if (::msync(m.base + begin, end - begin, MS_SYNC) != 0) {
        // Some filesystems reject MS_SYNC on a range; fall back to a file sync.
        if (errno != EINVAL)
            fail(Fault::IoError, "msync");
        syncFile(fd, false);
        return;
    }
    if (WriteRecorder* r = writeRecorder.load(std::memory_order_relaxed))
        r->flushed(fd, begin, end - begin);
}

void advise(const Mapping& m, std::size_t offset, std::size_t length, Advice a) noexcept
{
    if (!m.base || !length || offset >= m.size)
        return;
    // madvise wants a page-aligned start; widen the window outwards like msync.
    const std::size_t sp = systemPageSize();
    const std::size_t begin = offset & ~(sp - 1);
    std::size_t end = offset + length;
    if (end > m.size)
        end = m.size;
    int adv = MADV_NORMAL;
    switch (a) {
    case Advice::Normal: adv = MADV_NORMAL; break;
    case Advice::Random: adv = MADV_RANDOM; break;
    case Advice::Sequential: adv = MADV_SEQUENTIAL; break;
    case Advice::WillNeed: adv = MADV_WILLNEED; break;
    }
    (void)::madvise(m.base + begin, end - begin, adv);
}

#endif

bool cloneFile(FileHandle source, FileHandle destination)
{
    observeIo(IoEvent::Clone);
#ifdef __linux__
    if (::ioctl(destination, FICLONE, source) == 0)
        return true;
    if (errno != EOPNOTSUPP && errno != ENOTTY && errno != EXDEV && errno != EINVAL && errno != ENOSYS)
        fail(Fault::IoError, "FICLONE");
#else
    (void)source;
    (void)destination;
#endif
    return false;
}

void flushData(FileHandle fd, const Mapping& mapping,
               const std::vector<std::pair<std::uint64_t, std::uint64_t>>& pageRuns,
               std::size_t pageSize)
{
    observeIo(IoEvent::DataBarrier);
#ifdef _WIN32
    for (const auto& [first, last] : pageRuns)
        if (!::FlushViewOfFile(mapping.base + first * pageSize, (last - first + 1) * pageSize))
            fail(Fault::IoError, "FlushViewOfFile");
#else
    (void)mapping;
    (void)pageRuns;
    (void)pageSize;
#endif
    syncFile(fd, false);
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.string().c_str(), "rb"), &std::fclose);
    if (!file)
        throw base::Error(base::Code::NotFound, errnoMessage(("open " + path.string()).c_str(), errno), static_cast<std::uint32_t>(errno));
    std::string text;
    char buffer[16384];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof buffer, file.get());
        text.append(buffer, got);
        if (got < sizeof buffer)
            break;
    }
    if (std::ferror(file.get()))
        throw base::Error(base::Code::Internal, "cannot read " + path.string());
    return text;
}

}  // namespace sys
