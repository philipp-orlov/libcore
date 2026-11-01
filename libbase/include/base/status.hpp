// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// How an operation ends: a Code, a short message and a library-specific detail number, as a value (Status,
// Result<T>) or as an exception (Error). Code numbers are gRPC's, so a gateway can map them one to one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace base {

enum class Code : uint16_t {
    Ok = 0,
    Cancelled = 1,
    Unknown = 2,
    InvalidArgument = 3,
    DeadlineExceeded = 4,
    NotFound = 5,
    AlreadyExists = 6,
    PermissionDenied = 7,
    ResourceExhausted = 8,
    FailedPrecondition = 9,
    Aborted = 10,
    OutOfRange = 11,
    Unimplemented = 12,
    Internal = 13,
    Unavailable = 14,
    DataLoss = 15,
    Unauthenticated = 16,
};

const char* codeName(Code code) noexcept;

// 96 bytes, trivially copyable, never allocates: a longer message is cut to kMaxMessage bytes.
class Status {
public:
    static constexpr std::size_t kMaxMessage = 87;

    Status() = default;
    Status(Code code, std::string_view message = {}, uint32_t detail = 0) noexcept : code_(code), detail_(detail) {
        const std::size_t n = message.size() < kMaxMessage ? message.size() : kMaxMessage;
        if (n)
            std::memcpy(message_, message.data(), n);
        length_ = static_cast<uint8_t>(n);
        message_[n] = '\0';
    }

    // Named `success`, not `ok`: a static `ok()` would make `status.ok()` compile on an instance and always report
    // success.
    static Status success() noexcept { return {}; }
    bool ok() const noexcept { return code_ == Code::Ok; }
    bool isOk() const noexcept { return ok(); }
    explicit operator bool() const noexcept { return isOk(); }
    Code code() const noexcept { return code_; }
    // Library-specific value (nosql::ErrorCode, sql::ErrorCode, errno, ...); 0 = none.
    uint32_t detail() const noexcept { return detail_; }
    std::string_view message() const noexcept { return {message_, length_}; }
    std::string toString() const;

private:
    Code code_ = Code::Ok;
    uint8_t length_ = 0;
    uint32_t detail_ = 0;
    char message_[kMaxMessage + 1] = {};
};

// What a library throws. Library-specific exceptions derive from it, so one catch site sees them all.
class Error : public std::runtime_error {
public:
    explicit Error(const Status& status) : std::runtime_error(status.toString()), status_(status) {}
    Error(Code code, std::string_view message, uint32_t detail = 0) : Error(Status(code, message, detail)) {}
    const Status& status() const noexcept { return status_; }
    Code code() const noexcept { return status_.code(); }

private:
    Status status_;
};

// A value or a Status, without exceptions on the path that expects failure.
template <class T>
class Result {
public:
    Result(T value) : ok_(true) { ::new (static_cast<void*>(&u_.value)) T(std::move(value)); }  // NOLINT
    Result(Status status) : ok_(false) {  // NOLINT
        if (status.isOk())
            status = Status(Code::Internal, "Result built from an Ok status");
        ::new (static_cast<void*>(&u_.status)) Status(status);
    }
    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : ok_(other.ok_) {
        if (ok_)
            ::new (static_cast<void*>(&u_.value)) T(std::move(other.u_.value));
        else
            ::new (static_cast<void*>(&u_.status)) Status(other.u_.status);
    }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    ~Result() {
        if (ok_)
            u_.value.~T();
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    Status status() const noexcept { return ok_ ? Status() : u_.status; }
    // The value; throws base::Error with the status when there is none.
    T& value() & {
        if (!ok_)
            throw Error(u_.status);
        return u_.value;
    }
    T&& value() && {
        if (!ok_)
            throw Error(u_.status);
        return std::move(u_.value);
    }

private:
    union U {
        U() {}
        ~U() {}
        T value;
        Status status;
    } u_;
    bool ok_;
};

}  // namespace base
