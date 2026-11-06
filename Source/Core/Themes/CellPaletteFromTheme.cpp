// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// CellPalette::fromTheme — copy a Theme's slots into the CellPalette. Own TU so
// CellColor.cpp stays free of any Themes/Themes.h dependency (Themes -> CellColor).
#include "Core/CellColor.h"
#include "Core/Themes/Themes.h"
#include <cstring>

// The palette is a model type and the theme is this module's; the definition
// belongs to the palette, so it opens the palette's namespace and names the
// theme through this one.
namespace Ubiquitty {

CellPalette CellPalette::fromTheme(const Themes::Theme& theme)
{
    CellPalette p {};
    std::memcpy(p.indexed, theme.ansi, sizeof(theme.ansi));
    p.fillExtended();
    p.defaultForeground = theme.defaultForeground;
    p.defaultBackground = theme.defaultBackground;
    p.cursor = theme.cursor;
    p.brightenBold = true;
    return p;
}

} // namespace Ubiquitty
