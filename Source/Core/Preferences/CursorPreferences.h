// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Preferences/CursorPreferences.h — the user's [cursor] settings, as the
// terminal state the cursor resolves against.
//
// These fields are not frame style: they are the terminal's answer when no
// program has set a cursor with DECSCUSR. A terminal that never receives them
// answers with the struct defaults — shape 0, which is a block — which is wrong
// for a user who configured a bar, and not visible until a program asks the
// terminal to fall back.
//
// One function because a terminal reaches a frontend by more routes than a
// frontend has constructors: spawned, split, restored, adopted from a
// multiplexer. Each route that seeds these by hand is a route that can forget,
// and forgetting is silent — the cursor still draws, in the wrong shape.
//

#include "Core/Cursor/Style.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Preferences::Effective {

/// Applies the user's `[cursor]` preferences to a terminal.
///
/// Seeds the state `Cursor::resolve` falls back to when `cursorStyle` is
/// `FollowUserPreference` — which is every terminal until a program sends
/// DECSCUSR, and every terminal again after one sends `CSI 0 SP q`.
///
/// An unset `blink` is its default, never a vacancy: a program's `cnorm` (mode
/// 12 reset) does not stop the blink for a reader who wrote nothing, exactly
/// as it does not for one who wrote `blink = true`. The two documents
/// configure the same terminal.
///
/// Call it on every terminal a frontend shows, including one it adopted rather
/// than created — the preferences are local display policy, not content, so
/// they do not travel with a terminal that came from somewhere else.
///
/// - Parameters:
///   - terminal: Filled in place. Fields these settings do not name are left alone.
///   - configuration: The configuration to read.
inline void applyCursorPreferences(Ubiquitty::Terminal& terminal, const Values& configuration)
{
    terminal.cursorShapePref = static_cast<std::uint8_t>(cursorShape(configuration));
    terminal.cursorBlinkPref = cursorBlink(configuration);
    terminal.cursorHoldFrames = static_cast<std::uint32_t>(cursorLinger(configuration));
    terminal.cursorBlinkPeriodMilliseconds = static_cast<std::uint32_t>(cursorBlinkRate(configuration));
    // The caret's glide travels with the blink for the reason the header
    // states: a route that seeds the blink by hand and forgets the glide draws
    // a caret that snaps, which reads as the setting not working rather than as
    // a missing seed. The frame capture carries both to the animator.
    terminal.cursorMotionModePref = cursorMotion(configuration);
    terminal.cursorMotionDurationMilliseconds = static_cast<std::uint32_t>(cursorMotionDuration(configuration));
}

/// Applies the user's `[cursor]` preferences and hands the shape back to them.
///
/// The same five fields as `applyCursorPreferences`, plus the one thing that
/// separates a reader picking a shape from a configuration being re-read:
/// `cursorStyle` returns to `FollowUserPreference`, so the pick is on screen
/// at once rather than behind whatever DECSCUSR a program last sent.
///
/// It has to be a second entry rather than a flag on the first, because the
/// two events want opposite answers. A reload must leave `cursorStyle` alone —
/// revoking a running program's DECSCUSR because an unrelated key moved would
/// be a config change reaching into the screen. A pick must clear it, because
/// a shell that sets the cursor at every prompt (fish, vi-mode, p10k) leaves a
/// style standing that outranks the preference, and a reader who chooses a bar
/// and sees a block has been told the setting does not work.
///
/// Handing it back is not taking it away: the style is cleared, not pinned, so
/// the next DECSCUSR the program sends is honoured like any other.
///
/// - Parameters:
///   - terminal: Filled in place. Fields these settings do not name are left alone.
///   - configuration: The configuration to read.
inline void applyPickedCursorPreferences(Ubiquitty::Terminal& terminal, const Values& configuration)
{
    applyCursorPreferences(terminal, configuration);
    terminal.cursorStyle = Ubiquitty::Cursor::Style::FollowUserPreference;
}

} // namespace Ubiquitty::Preferences::Effective
