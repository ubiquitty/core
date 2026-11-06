// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/SyncOutput.h"
#include "Core/CellGrid.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Modes::SyncOutput {

bool isActive(const Ubiquitty::Terminal* t) { return t ? t->modes.syncOutput : false; }

void touch(Ubiquitty::Terminal* t, Ticks currentTicks)
{
    if (!t || !t->modes.syncOutput)
        return;
    // First touch after enable — capture the start time so elapsed can age the
    // block. Store tick 1 for tick 0 so 0 stays the "uninitialised" sentinel.
    if (t->modes.syncOutputTouchTicks == 0)
        t->modes.syncOutputTouchTicks = currentTicks.count ? currentTicks.count : 1;
}

uint32_t elapsedTicks(const Ubiquitty::Terminal* t, Ticks currentTicks)
{
    if (!t || !t->modes.syncOutput || t->modes.syncOutputTouchTicks == 0)
        return 0;
    if (currentTicks.count < t->modes.syncOutputTouchTicks)
        return 0; // wraparound — treat as fresh.
    return currentTicks.since(Ticks(t->modes.syncOutputTouchTicks));
}

void expire(Ubiquitty::Terminal* t)
{
    if (!t || !t->modes.syncOutput)
        return;
    t->modes.syncOutput = false;
    t->modes.syncOutputTouchTicks = 0;
    if (t->grid)
        t->grid->requestFullRedraw();
}

uint32_t ticksUntilHoldExpires(const Ubiquitty::Terminal& t, Ticks currentTicks)
{
    if (!isActive(&t))
        return 0;
    // An unstamped hold has its whole budget ahead of it: the stamp lands on
    // the first touch, which the asking frontend performs on the way here.
    const uint32_t elapsed = elapsedTicks(&t, currentTicks);
    if (elapsed > maxHoldTicks)
        return 1;
    return maxHoldTicks - elapsed + 1;
}

bool shouldWithholdFrame(Ubiquitty::Terminal& t, Ticks currentTicks)
{
    if (!isActive(&t))
        return false;
    touch(&t, currentTicks);
    if (elapsedTicks(&t, currentTicks) > maxHoldTicks) {
        expire(&t);
        return false;
    }
    return true;
}

} // namespace Ubiquitty::Modes::SyncOutput
