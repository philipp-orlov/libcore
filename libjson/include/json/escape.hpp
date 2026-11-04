// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JSON string escaping over any sink with put(const char*, size_t). Header-only so counting sinks (two-pass writers)
// inline it. Unescaped runs go out in one call; only control bytes, '"' and '\\' are handled one at a time.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace json {

// Writes the escaped body of `s` (no surrounding quotes), for text in any code unit: UTF-8 here, UTF-16 for a language
// runtime whose strings are. Only ASCII is ever escaped, so the encoding does not matter. The sink takes runs of `C`
// through put(const C*, size_t) and escapes through put(const char*, size_t).
template <class Sink, class C>
inline void escapeUnitsTo(Sink& out, std::basic_string_view<C> s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::size_t run = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<std::make_unsigned_t<C>>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        if (i > run)
            out.put(s.data() + run, i - run);
        run = i + 1;
        switch (c) {
            case '"': out.put("\\\"", 2); break;
            case '\\': out.put("\\\\", 2); break;
            case '\n': out.put("\\n", 2); break;
            case '\r': out.put("\\r", 2); break;
            case '\t': out.put("\\t", 2); break;
            case '\b': out.put("\\b", 2); break;
            case '\f': out.put("\\f", 2); break;
            default: {
                const char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
                out.put(esc, 6);
            }
        }
    }
    if (s.size() > run)
        out.put(s.data() + run, s.size() - run);
}

template <class Sink>
inline void escapeTo(Sink& out, std::string_view s) {
    escapeUnitsTo<Sink, char>(out, s);
}

// Size of the escaped body, without quotes.
inline std::size_t escapedSize(std::string_view s) noexcept {
    std::size_t n = s.size();
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        n += (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') ? 1 : 5;
    }
    return n;
}

// Appends `"escaped"` to a std::string.
inline void appendQuoted(std::string& out, std::string_view s) {
    struct Sink {
        std::string& s;
        void put(const char* p, std::size_t n) { s.append(p, n); }
    } sink{out};
    out.push_back('"');
    escapeTo(sink, s);
    out.push_back('"');
}

}  // namespace json
