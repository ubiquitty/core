// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// DEC private mode 2026 — synchronised output.
//
// While the mode is set the frontend withholds presentation so an application can
// compose a frame without tearing. The block is AGE-BOUNDED here: an application
// that sets 2026 and never clears it cannot freeze the display, because the
// frontend expires the block once elapsed exceeds its budget.

namespace Ubiquitty::Modes::SyncOutput {

[[nodiscard]] bool isActive(const Ubiquitty::Terminal* t);

/// Stamp the first touch after enable, so elapsed can age the block. Tick 0 is the
/// "uninitialised" sentinel, so a currentTicks of 0 stores 1.
void touch(Ubiquitty::Terminal* t, Ticks currentTicks);

/// Ticks since the block began, for the frontend's expiry budget.
[[nodiscard]] uint32_t elapsedTicks(const Ubiquitty::Terminal* t, Ticks currentTicks);

/// Drop the block and its touch stamp — the frontend presents on the next frame.
void expire(Ubiquitty::Terminal* t);

/// The longest a block may withhold presentation before it is expired.
///
/// One second at the 60 Hz PAL tick. The number belongs here rather than in a
/// frontend: an application that sets 2026 and never clears it would otherwise
/// freeze one frontend's display for a different length of time than another's,
/// and "how long until we stop trusting the application" is a property of the
/// mode, not of who is drawing.
constexpr uint32_t maxHoldTicks = 60;

/// Whether the frontend should withhold this frame.
///
/// The whole of the per-frame decision: stamps the block on first sight, expires
/// it once it has outlived `maxHoldTicks`, and answers false thereafter. A
/// frontend that repaints whenever bytes arrive — rather than asking this —
/// renders every intermediate state of a frame the application was still
/// composing, which is exactly what synchronised output exists to prevent.
///
/// Two obligations come with a true answer, both of which have been got wrong
/// here already:
///
/// **Withholding defers a frame; it never discards one.** A caller that clears
/// its pending-repaint state on a true answer drops frames outright rather than
/// delaying them, so a stream of blocks loses screens instead of showing them
/// late. Ask before resetting anything.
///
/// **The answer must be re-asked while it stays true.** Expiry only advances
/// when this is called, so a frontend whose repaints are driven purely by
/// arriving bytes never reaches the watchdog if an application opens a block and
/// then goes quiet — the display stays stale indefinitely. Such a frontend must
/// keep asking on a timer of its own.
///
/// Deferring is the floor, not the goal. A frontend that captures the complete
/// frame at the sync-clear instant can present *that* mid-block and keep
/// delivering completed frames at its native rate; one that only defers can have
/// every tick land inside a block and fall back to the watchdog's cadence.
[[nodiscard]] bool shouldWithholdFrame(Ubiquitty::Terminal& t, Ticks currentTicks);

/// How many ticks until a held block outlives its budget.
///
/// The hold defers frames; it never discards one. So a frontend that holds
/// must come back and ask once the budget is spent, and this names when. It
/// is the frontend's timer that asks, because a frame clock cannot: a display
/// link stops with its display, and a view on a closed laptop or an occluded
/// window would otherwise keep a stale hold forever. The scenario that found
/// this passed on a desk and failed on a runner whose lid was shut.
///
/// - Parameters:
///   - t: The terminal holding the block.
///   - currentTicks: Now, on the frontend's monotonic 60 Hz clock.
/// - Returns: Ticks until `shouldWithholdFrame` will expire the hold; at
///   least 1, so a caller can always sleep on the answer. Zero when nothing
///   is held.
[[nodiscard]] uint32_t ticksUntilHoldExpires(const Ubiquitty::Terminal& t, Ticks currentTicks);

} // namespace Ubiquitty::Modes::SyncOutput
