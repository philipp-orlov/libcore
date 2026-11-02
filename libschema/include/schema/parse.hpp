// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Library entry point of the `.bufs` front end: parse a schema and the files it includes without exceptions and with
// located diagnostics.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "schema/schema.hpp"

namespace schema {

using bufsc::Schema;

struct Diagnostic {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;   // 0: not known (the parser reports lines)
    std::string message;
};

struct ParseResult {
    Schema schema;
    std::vector<Diagnostic> errors;   // empty on success. The parser stops at the first error it cannot recover from.
    bool ok() const noexcept { return errors.empty(); }
};

ParseResult parse(const std::string& path, const std::vector<std::string>& includeDirs);

// Type and method ids are FNV-1a of the qualified name; both are on the wire and must never change.
uint32_t typeId(std::string_view qualifiedName) noexcept;
inline uint32_t methodId(std::string_view qualifiedName) noexcept { return typeId(qualifiedName); }

// True when two different qualified names in the include closure of `schema` share a type id; the colliding pair is
// described in `*message`.
bool findIdCollision(const Schema& schema, std::string* message);

}  // namespace schema
