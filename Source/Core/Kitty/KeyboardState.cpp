// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Kitty/KeyboardState.h"

#include "Core/Kitty/KeyboardFlags.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"

namespace Ubiquitty::Kitty {

void setFlags(Ubiquitty::Terminal& t, int flags, int mode)
{
    uint16_t* current = &t.kittyKeyboardFlags[Ubiquitty::screenIndex(t)];
    constexpr Kitty::KeyFlagSet allKnownFlags { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents, Kitty::KeyFlag::ReportAlternateKeys, Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportText };
    flags &= static_cast<int>(allKnownFlags.toRaw());
    switch (mode) {
    case 2:
        *current |= static_cast<uint16_t>(flags);
        break;
    case 3:
        *current &= static_cast<uint16_t>(~flags);
        break;
    case 1:
    default:
        *current = static_cast<uint16_t>(flags);
        break;
    }
}

void pushFlags(Ubiquitty::Terminal& t, int flags)
{
    int index = Ubiquitty::screenIndex(t);
    uint8_t* length = &t.kittyKeyboardStackLength[index];
    if (*length >= Ubiquitty::Kitty::maxKeyStackDepth) {
        // Full: FIFO-evict the OLDEST entry (shift down), then push at the top,
        // per the kitty protocol.
        for (int i = 1; i < Ubiquitty::Kitty::maxKeyStackDepth; i++)
            t.kittyKeyboardStack[index][i - 1] = t.kittyKeyboardStack[index][i];
        *length = Ubiquitty::Kitty::maxKeyStackDepth - 1;
    }
    t.kittyKeyboardStack[index][(*length)++] = t.kittyKeyboardFlags[index];
    setFlags(t, flags, 1);
}

void popFlags(Ubiquitty::Terminal& t, int count)
{
    int index = Ubiquitty::screenIndex(t);
    uint8_t* length = &t.kittyKeyboardStackLength[index];
    if (count <= 0)
        count = 1;
    bool emptied = false;
    while (count-- > 0 && *length > 0) {
        t.kittyKeyboardFlags[index] = t.kittyKeyboardStack[index][--(*length)];
        if (*length == 0)
            emptied = true;
    }
    // A pop that EMPTIES the stack resets all flags to 0, overriding the restored
    // stack[0].
    if (emptied)
        t.kittyKeyboardFlags[index] = 0;
}

} // namespace Ubiquitty::Kitty
