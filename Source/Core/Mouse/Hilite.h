// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// Hilite mouse tracking, DEC private mode 1001.
//
// The protocol has the terminal await a highlight region from the host. That
// await is bounded by a wall-clock deadline here, so a host that never answers
// cannot stall input + rendering (the XTHIMOUSE reply returns immediately).
//
// The handlers call in; the frontend drives tickHilite once per frame.

namespace Ubiquitty::Mouse {

// Deadline for the XTHIMOUSE reply, in PAL::monotonicTicks (60 Hz) units.
// ~400 ms: long enough for a cooperating app over a busy PTY, short enough
// that a non-cooperating app's stale "awaiting" state clears before the next
// user gesture.
inline constexpr uint32_t hiliteAwaitDeadlineTicks = 24;

// Turn off X10/normal/cell-motion/any-event tracking, mode 1001 and relative pointer lock, leaving the
// report-encoding selections (1005/1006/1015/1016) alone — a re-enabling app
// finds its chosen encoding still in effect. Returns true when any tracking mode
// was actually on, so the caller knows whether the published snapshot changed.
bool clearTracking(Ubiquitty::Terminal* t);

/// Releases pointer capture left by a departed foreground application.
///
/// Unknown groups and non-shell successors preserve capture. A shell that
/// enabled capture itself keeps it while it remains the observed owner.
///
/// - Parameters:
///   - terminal: The terminal holding the capture modes.
///   - ownerProcessGroup: The last observed capture owner, updated in place.
///   - foregroundProcessGroup: The current foreground group, or zero if unknown.
///   - isForegroundShell: Whether the current foreground was observed as a shell.
/// - Returns: Whether capture was released.
[[nodiscard]] bool retireTrackingIfStale(Ubiquitty::Terminal& terminal, int& ownerProcessGroup, int foregroundProcessGroup, bool isForegroundShell); // bool-ok: sampled process fact

// Arm the bounded wait after the frontend has sent the hilite press report.
// Stamps hiliteDeadlineTick = nowTicks + hiliteAwaitDeadlineTicks and drops any
// stale region so the previous gesture's highlight never lingers. No-op unless
// mode 1001 is active.
void beginHiliteAwait(Ubiquitty::Terminal* t, uint32_t nowTicks);

// Per-drain / per-frame deadline check. If a hilite await is outstanding and
// nowTicks has reached the deadline with no XTHIMOUSE reply, abort cleanly:
// clear the await and leave no region highlighted. Returns true when it changed
// state (so the frontend can repaint / re-evaluate animation). Cheap no-op when
// nothing is awaiting.
bool tickHilite(Ubiquitty::Terminal* t, uint32_t nowTicks);

} // namespace Ubiquitty::Mouse
