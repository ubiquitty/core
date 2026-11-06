// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Themes/RethemeDefaults.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Terminal.h"
#include "Core/VT/Internal.h"

namespace Ubiquitty::Themes {

void rethemeDefaults(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    // A theme apply moves colours the epoch's consumers key their caches on, and
    // it reaches the palette by direct write rather than through the OSC handlers
    // that bump it themselves.
    ++t->paletteEpoch;
    Ubiquitty::Screen::markAllDirty(*t);
    // DEC mode 2031: tell a subscribed app the color scheme just
    // changed so it re-themes live. Only the unsolicited notification is
    // gated on the subscription; a direct DSR 996 query always answers.
    if (t->modes.colorThemeReporting)
        VT::reportColorScheme(*t);
}

} // namespace Ubiquitty::Themes
