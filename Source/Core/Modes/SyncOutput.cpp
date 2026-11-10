// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/SyncOutput.h"
#include "Core/CellGrid.h"
#include "Core/Terminal.h"

#include <limits>

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
    // Cyclic, so a stamp taken before the 32-bit counter wraps still ages. A
    // stamp ahead of now, from an asker reading a clock a little behind the
    // stamper's, is fresh.
    const uint32_t elapsed = currentTicks.since(Ticks(t->modes.syncOutputTouchTicks));
    return elapsed > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ? 0 : elapsed;
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

BlockMark blockMark(const Ubiquitty::Terminal& t)
{
    return { .isOpen = t.modes.syncOutput, .touchTicks = t.modes.syncOutputTouchTicks };
}

uint32_t ticksUntilHoldExpires(const Ubiquitty::Terminal& t, Ticks currentTicks)
{
    if (!isActive(&t))
        return 0;
    return ticksUntilHoldExpires(t.modes.syncOutputTouchTicks, currentTicks);
}

uint32_t ticksUntilHoldExpires(uint32_t touchTicks, Ticks currentTicks) noexcept UBIQUITTY_NONBLOCKING
{
    // An unstamped hold has its whole budget ahead of it: the stamp lands on
    // the first touch, which the asking frontend performs on the way here. A
    // clock behind the stamp has wrapped, and the block reads as fresh.
    const uint32_t elapsed = touchTicks == 0 || currentTicks.count < touchTicks ? 0 : currentTicks.count - touchTicks;
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
