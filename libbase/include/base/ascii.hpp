// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Character tests that do not depend on the locale, for any character type: what a lexer or a number parser asks about
// the ASCII range, and nothing else. <cctype> answers per the process locale and is undefined on a negative char.
#pragma once

namespace base {

// 0-35 for 0-9, a-z and A-Z; -1 for anything else. Compare with the radix.
template <class C>
constexpr int digitValue(C c) noexcept {
    if (c >= C('0') && c <= C('9'))
        return int(c) - '0';
    if (c >= C('a') && c <= C('z'))
        return int(c) - 'a' + 10;
    if (c >= C('A') && c <= C('Z'))
        return int(c) - 'A' + 10;
    return -1;
}

// 0-15, or -1.
template <class C>
constexpr int hexValue(C c) noexcept {
    const int v = digitValue(c);
    return v < 16 ? v : -1;
}

}  // namespace base
