// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/SwiftInterop.h"
#include "Core/Modes/ConditionalMode.h"
#include <cstdint>

// Behavioral-mode enums that cross the Core/Runtime boundary. They live in
// their own tiny header (rather than Terminal.h) so the lightweight Runtime
// layer — TerminalRuntime.h includes neither Terminal.h nor
// TerminalOperations.h — can name them in its public signatures without
// pulling in the full terminal model.
//
// The first enumerator of each equals `false` and the second `true`, so a
// static_cast preserves the boolean wire value.

namespace Ubiquitty::Modes {

/// Whether a resize rewraps the scrollback or keeps its line breaks.
enum class ReflowMode : uint8_t {
    Fixed,
    Reflow
};
/// Whether the terminal holds keyboard focus.
enum class FocusState : uint8_t {
    Unfocused,
    Focused
};
/// Whether a mode control turns its mode on or off.
///
/// SM and RM differ only in this, and so do DECSET and DECRST, so each pair
/// shares one body and passes which of the two it is.
enum class ModeChange : uint8_t {
    Clear,
    Set
};
/// Which of the two screens a control selects.
enum class ScreenChoice : uint8_t {
    Primary,
    Alternate
};
/// Whether a password prompt is showing.
enum class PasswordPrompt : uint8_t {
    Inactive,
    Active
};

/// How urgently a resize must finish. Settled (the default — font/theme change,
/// zoom, restore, the drag's final step) rewraps everything now. Interactive (a
/// live-drag intermediate step) rewraps only the visible screen and DEFERS the
/// O(scrollback) full-history rewrap until the drag settles — a subsequent
/// Settled resize, an explicit settleResize(), or the first access that needs
/// history at the new width. The deferred history renders at its stored width in
/// the meantime (the same mixed-width history a Fixed-mode pane already tolerates).
enum class ResizeUrgency : uint8_t {
    Settled,
    Interactive
};

/// Behavioral-mode enums standing in for boolean parameters. The first
/// enumerator equals `false`, the second `true`, so static_cast preserves the
/// wire value.
enum class AnchorMode : uint8_t {
    /// The first row stays put: the grid grows and shrinks downwards.
    Top UBIQUITTY_SWIFT_NAME(top),
    /// The last row stays put, for a drag whose bottom edge is the
    /// stationary one — the prompt stays where the reader is looking
    /// instead of riding the edge that is moving. The primary screen's
    /// alone: the alternate screen keeps its first row either way.
    Bottom UBIQUITTY_SWIFT_NAME(bottom),
    /// Keeps the primary screen's first visible content row at the top through
    /// reflow when the remaining content fits, adding blank rows below it.
    /// Falls back to the content-preserving split when the suffix cannot fit.
    VisibleTop UBIQUITTY_SWIFT_NAME(visibleTop)
};
/// Which byte sequence the arrow keys transmit (DECCKM).
///
/// `Application` sends SS3-prefixed sequences, which a full-screen editor asks
/// for so its bindings differ from the shell's.
enum class CursorKeyMode : uint8_t {
    Normal,
    Application
};
/// Which way the viewport moves through the scrollback.
enum class ScrollDirection : uint8_t {
    Down,
    Up
};
/// DEC private modes 1010 (scrollOnOutput) / 1011 (scrollOnKeypress).
/// Pure predicate the frontend consults before snapping a scrolled-back
/// view (viewOffset != 0) back to the live tail. The frontend owns
/// viewOffset; the Core owns the policy. Trigger: 0 = host output
/// arrived, 1 = a key was sent.
enum class SnapTrigger : int {
    Output = 0,
    Keypress = 1
};

/// Which of the two screen buffers is showing. Conditional modes take this as
/// their precondition, so it has to be nameable without including Terminal.h.
enum class ScreenBuffer : uint8_t {
    Primary,
    Alternate,
};

/// Whether a screen buffer is the alternate one.
///
/// - Parameter active: The buffer to test.
/// - Returns: `true` for the alternate screen.
constexpr bool isAlternateScreen(ScreenBuffer active) { return active == ScreenBuffer::Alternate; }

/// DEC 1007. The wheel becomes cursor keys only on the alternate screen; on the
/// primary screen it scrolls history, and honouring the mode there would take the
/// user's scrollback away.
using AlternateScrollMode = Modes::ConditionalMode<ScreenBuffer, isAlternateScreen>;

} // namespace Ubiquitty::Modes
