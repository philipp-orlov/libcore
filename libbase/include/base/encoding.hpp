// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// RFC 4648 base64 (standard alphabet) and lower-case hex. Every function writes into a caller buffer and never
// allocates; the std::string overloads are conveniences for cold paths.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace base {

// Bytes needed for the padded base64 text of `n` input bytes (no terminator).
constexpr std::size_t base64Size(std::size_t n) noexcept { return ((n + 2) / 3) * 4; }

// Upper bound of the decoded size of `n` base64 characters.
constexpr std::size_t base64MaxDecodedSize(std::size_t n) noexcept { return (n / 4) * 3 + 3; }

// Writes base64Size(len) characters to `out`; returns that count.
std::size_t base64Encode(const void* data, std::size_t len, char* out) noexcept;

// Decodes into `out` (capacity >= base64MaxDecodedSize(in.size())). Characters outside the alphabet ('=', blanks,
// line breaks) are skipped, as the libhttp implementation always did. Returns the byte count.
std::size_t base64Decode(std::string_view in, void* out) noexcept;

// Strict variant: returns false for any character outside the alphabet or misplaced padding. `*written` is set on
// success only.
bool base64DecodeStrict(std::string_view in, void* out, std::size_t* written) noexcept;

std::string base64Encode(const void* data, std::size_t len);
std::string base64Decode(std::string_view in);

// Hex.
constexpr std::size_t hexSize(std::size_t n) noexcept { return n * 2; }
void hexEncode(const void* data, std::size_t len, char* out) noexcept;               // writes 2*len characters
bool hexDecode(std::string_view in, void* out, std::size_t* written) noexcept;       // in.size() must be even
std::string hexEncode(const void* data, std::size_t len);

}  // namespace base
