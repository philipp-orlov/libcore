// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// base::ClassicLocale: runs strtod and snprintf in the "C" locale for one scope, whatever the process has set.
#pragma once

#include <locale.h>
#ifndef _WIN32
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#else
#include <cstring>
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
#else
    // The C runtime's locale is "C" until a program changes it, so there is almost never anything to switch: the decimal
    // point is looked at, and only a program that set another one pays for a per-thread switch of LC_NUMERIC.
    ClassicLocale() noexcept {
        const lconv* conventions = localeconv();
        if (!conventions || !conventions->decimal_point || conventions->decimal_point[0] == '.')
            return;
        const char* current = setlocale(LC_NUMERIC, nullptr);
        if (!current || std::strlen(current) >= sizeof saved_)
            return;
        std::strcpy(saved_, current);
        previousMode_ = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        setlocale(LC_NUMERIC, "C");
        switched_ = true;
    }
    ~ClassicLocale() {
        if (!switched_)
            return;
        setlocale(LC_NUMERIC, saved_);
        if (previousMode_ != -1)
            _configthreadlocale(previousMode_);
    }
    ClassicLocale(const ClassicLocale&) = delete;
    ClassicLocale& operator=(const ClassicLocale&) = delete;

private:
    char saved_[128];
    int previousMode_ = -1;
    bool switched_ = false;
#endif
};

}  // namespace base
