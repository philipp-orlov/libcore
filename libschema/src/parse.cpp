// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "schema/parse.hpp"

#include <charconv>
#include <map>

#include "base/hash.hpp"

namespace schema {

uint32_t typeId(std::string_view qualifiedName) noexcept { return base::fnv1a32(qualifiedName); }

ParseResult parse(const std::string& path, const std::vector<std::string>& includeDirs) {
    ParseResult result;
    try {
        result.schema = bufsc::load(path, includeDirs);
    } catch (const bufsc::SchemaError& e) {
        // "file:line: message" (a file name may itself contain colons: split from the right of the line number).
        const std::string text = e.what();
        Diagnostic d;
        d.message = text;
        const std::size_t second = text.find(": ", 0);
        if (second != std::string::npos) {
            const std::string where = text.substr(0, second);
            const std::size_t colon = where.rfind(':');
            unsigned line = 0;
            if (colon != std::string::npos) {
                const char* b = where.data() + colon + 1;
                const auto r = std::from_chars(b, where.data() + where.size(), line);
                if (r.ec == std::errc{} && r.ptr == where.data() + where.size()) {
                    d.file = where.substr(0, colon);
                    d.line = line;
                    d.message = text.substr(second + 2);
                }
            }
            if (d.file.empty()) {
                d.file = where;
                d.message = text.substr(second + 2);
            }
        }
        result.errors.push_back(std::move(d));
    }
    if (result.errors.empty()) {
        std::string message;
        if (findIdCollision(result.schema, &message))
            result.errors.push_back({path, 0, 0, message});
    }
    return result;
}

bool findIdCollision(const Schema& schema, std::string* message) {
    std::map<uint32_t, std::string> seen;
    auto note = [&](const std::string& qualified) {
        const uint32_t id = typeId(qualified);
        auto [it, inserted] = seen.emplace(id, qualified);
        if (!inserted && it->second != qualified) {
            if (message)
                *message = "type id collision: '" + it->second + "' and '" + qualified + "' both hash to " + std::to_string(id);
            return true;
        }
        return false;
    };
    for (const auto& d : schema.structs)
        if (note(d->qualified())) return true;
    for (const auto& d : schema.tables)
        if (note(d->qualified())) return true;
    return false;
}

}  // namespace schema
