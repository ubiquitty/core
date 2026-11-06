// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// System light/dark theme resolution policy: pure integer logic over the theme
// count and an "is theme i dark" callable, with no registry/Preferences/platform
// dependency, so both frontends resolve identically.

#include "Core/Preferences/AppearanceMode.h"

#include <cstddef>

namespace Ubiquitty::Themes {

using Ubiquitty::Preferences::ColorScheme;

// The first theme of the requested scheme, or -1 when none exists. `isDark(i)`
// is true when theme i is dark, for 0 <= i < count.
template<typename DarknessFn> [[nodiscard]] int firstThemeOfDarkness(ColorScheme scheme, int count, DarknessFn isDark)
{
    const bool wantDark = (scheme == ColorScheme::Dark);
    for (int i = 0; i < count; ++i)
        if (isDark(i) == wantDark)
            return i;
    return -1;
}

// Resolve the theme index for the `scheme` appearance: the stored index when set,
// in range, and of the matching darkness; else the first theme of that darkness;
// else 0. `storedIndex` is ignored when !storedSet.
template<typename DarknessFn> [[nodiscard]] int resolveSystemTheme(ColorScheme scheme, int count, DarknessFn isDark, bool storedSet, int storedIndex)
{
    const bool wantDark = (scheme == ColorScheme::Dark);
    if (storedSet && storedIndex >= 0 && storedIndex < count && isDark(storedIndex) == wantDark)
        return storedIndex;
    const int first = firstThemeOfDarkness(scheme, count, isDark);
    return first >= 0 ? first : 0;
}

// Resolve the theme index the live auto-switch applies, layering a named-default
// tier between the per-mode override and the first-of-darkness fallback:
//   1. the per-mode override, when set / in range / of the matching darkness;
//   2. else the named default (namedDefaultIndex() → index or -1), when of that
//      darkness;
//   3. else the first theme of that darkness;
//   4. else -1.
template<typename DarknessFn, typename NamedDefaultFn> [[nodiscard]] int resolveAutoTheme(ColorScheme scheme, int count, DarknessFn isDark, bool storedSet, int storedIndex, NamedDefaultFn namedDefaultIndex)
{
    const bool wantDark = (scheme == ColorScheme::Dark);
    if (storedSet && storedIndex >= 0 && storedIndex < count && isDark(storedIndex) == wantDark)
        return storedIndex;
    const int named = namedDefaultIndex();
    if (named >= 0 && named < count && isDark(named) == wantDark)
        return named;
    return firstThemeOfDarkness(scheme, count, isDark);
}

} // namespace Ubiquitty::Themes
