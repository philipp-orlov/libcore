// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// base::ClassicLocale: runs strtod and snprintf in the "C" locale for one scope, whatever the process has set.
#pragma once

#ifndef _WIN32
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

namespace base {

// uselocale only switches a per-thread pointer, so this allocates nothing after the first use.
class ClassicLocale {
public:
#ifndef _WIN32
    ClassicLocale() : previous_(c() ? uselocale(c()) : nullptr) {}
    ~ClassicLocale() {
        if (previous_)
            uselocale(previous_);
    }

private:
    static locale_t c() {
        static const locale_t instance = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
        return instance;
    }

    locale_t previous_;
#endif
};

}  // namespace base
