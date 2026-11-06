// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/DisplayIdle.h"
#include "Core/CellGrid.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Modes::DisplayIdle {

static void invalidateDisplay(Terminal& terminal)
{
    ++terminal.modelEpoch;
    if (terminal.grid)
        terminal.grid->invalidateShadow();
}

bool noteInput(Terminal& terminal, Ticks now)
{
    terminal.displayActivityTick = now.count;
    terminal.hasDisplayActivityTick = true;
    if (!terminal.isCRTBlanked)
        return false;
    terminal.isCRTBlanked = false;
    terminal.screenBlanked = false;
    invalidateDisplay(terminal);
    return true;
}

bool advance(Terminal& terminal, Ticks now)
{
    if (!terminal.modes.shouldSaveCRT) {
        if (terminal.isCRTBlanked)
            return noteInput(terminal, now);
        return false;
    }
    if (!terminal.hasDisplayActivityTick) {
        terminal.displayActivityTick = now.count;
        terminal.hasDisplayActivityTick = true;
        return false;
    }
    if (terminal.screenBlanked || now.since(Ticks(terminal.displayActivityTick)) < crtSaveDelayTicks)
        return false;
    terminal.isCRTBlanked = true;
    terminal.screenBlanked = true;
    invalidateDisplay(terminal);
    return true;
}

std::optional<std::uint32_t> ticksUntilDeadline(const Terminal& terminal, Ticks now)
{
    if (!terminal.modes.shouldSaveCRT || terminal.screenBlanked)
        return std::nullopt;
    if (!terminal.hasDisplayActivityTick)
        return crtSaveDelayTicks;
    const auto elapsed = now.since(Ticks(terminal.displayActivityTick));
    return elapsed >= crtSaveDelayTicks ? 0u : crtSaveDelayTicks - elapsed;
}

}
