// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// DEC 1001 hilite tracking: the bounded await for the host's highlight region.

#include "Core/Mouse/Hilite.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Mouse {

bool clearTracking(Ubiquitty::Terminal* t)
{
    if (!t)
        return false;
    const bool wasTracking = Ubiquitty::Mouse::tracksPointer(t) || t->modes.mouseRelativePointerLock;
    t->modes.mouseX10 = t->modes.mouseButtonTracking = t->modes.mouseCellMotion = t->modes.mouseAnyEvent = false;
    // Hilite (1001) is a tracking mode too — drop it and its handshake state so a
    // foreground-process change can't strand an awaiting highlight.
    t->modes.mouseHilite = false;
    t->modes.hiliteRegionValid = false;
    t->modes.hiliteAwaiting = false;
    t->modes.mouseRelativePointerLock = false;
    return wasTracking;
}

bool retireTrackingIfStale(Ubiquitty::Terminal& terminal, int& ownerProcessGroup, int foregroundProcessGroup, bool isForegroundShell) // bool-ok: sampled process fact
{
    if (!tracksPointer(&terminal) && !terminal.modes.mouseRelativePointerLock) {
        ownerProcessGroup = 0;
        return false;
    }
    if (ownerProcessGroup == 0) {
        ownerProcessGroup = foregroundProcessGroup;
        return false;
    }
    if (!isForegroundShell || foregroundProcessGroup <= 0 || foregroundProcessGroup == ownerProcessGroup)
        return false;
    ownerProcessGroup = 0;
    return clearTracking(&terminal);
}

void beginHiliteAwait(Ubiquitty::Terminal* t, uint32_t nowTicks)
{
    if (!t || !t->modes.mouseHilite)
        return;
    // A fresh press supersedes any earlier gesture: drop the previous region and
    // re-arm the deadline. The parser never blocks waiting for the host;
    // tickHilite enforces the bound.
    t->modes.hiliteRegionValid = false;
    t->modes.hiliteAwaiting = true;
    t->modes.hiliteDeadlineTick = nowTicks + hiliteAwaitDeadlineTicks;
}

bool tickHilite(Ubiquitty::Terminal* t, uint32_t nowTicks)
{
    if (!t || !t->modes.hiliteAwaiting)
        return false;
    // Cyclic 32-bit tick comparison (monotonicTicks wraps): reached the deadline
    // once the signed delta is non-negative. No XTHIMOUSE arrived in time — abort
    // the wait so the terminal stays responsive and no highlight is stranded.
    if (static_cast<int32_t>(nowTicks - t->modes.hiliteDeadlineTick) >= 0) {
        t->modes.hiliteAwaiting = false;
        t->modes.hiliteRegionValid = false;
        return true;
    }
    return false;
}

} // namespace Ubiquitty::Mouse
