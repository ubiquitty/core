// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Modes/TerminalModes.h"

namespace Ubiquitty::Runtime {

/// The window facts a shell reports about one terminal surface.
///
/// Every field is something only a windowing system can answer, and none of
/// them is a decision: which combination means the keyboard is on this surface
/// is the rule below, so a shell cannot grow a conjunction the other arms
/// disagree with.
struct SurfaceFocusFacts {
    /// Whether the surface's window is the one the system sends keys to.
    bool isWindowKey = false;
    /// Whether the surface is that window's first responder.
    ///
    /// Per-surface, not per-window: a split window is key as a whole and only
    /// one of its panes answers keys.
    bool isFirstResponder = false;
    /// Whether the application owns the keyboard at all.
    bool isApplicationActive = false;
};

/// What the window facts mean for one surface.
struct SurfaceFocusVerdict {
    /// The state to publish to the session.
    Modes::FocusState focus = Modes::FocusState::Unfocused;
    /// Whether the caret must be held solid before the next frame draws.
    bool wantsActivityNote = false;
};

/// Resolves a surface's focus from the window facts around it.
///
/// Two policies a shell gets wrong by having its own opinion.
///
/// Focus is the conjunction, not the window's key state alone and not the
/// responder alone. A window carries one key status and a split window carries
/// several panes, so a pane fed the window's answer keeps a blinking caret it
/// has no keyboard for; and a responder-only answer never moves when the
/// window loses key status, which is what a switch to another application is.
/// The same signal drives the blink freeze, the unfocused-bell gate and
/// DECSET 1004 focus reporting, so all three go wrong together. The
/// application's activation is the third term because a window can report
/// itself key while another application owns the keyboard.
///
/// The activity note is the arrival edge only. A caret that resumes its blink
/// on the frame a window is clicked into reads as a flicker rather than as
/// focus, so arrival holds it solid, the hold a keystroke asks for. Only on the
/// edge, because a recompute answering the state already published would
/// re-stamp the hold and a window under a run of edges would never blink.
///
/// - Parameters:
///   - facts: What the shell reports about the window around the surface.
///   - published: The state the surface last published.
/// - Returns: The state to publish, and whether the caret must latch solid.
[[nodiscard]] constexpr SurfaceFocusVerdict surfaceFocus(const SurfaceFocusFacts& facts, Modes::FocusState published)
{
    SurfaceFocusVerdict verdict;
    verdict.focus = (facts.isApplicationActive && facts.isWindowKey && facts.isFirstResponder)
        ? Modes::FocusState::Focused
        : Modes::FocusState::Unfocused;
    verdict.wantsActivityNote = verdict.focus == Modes::FocusState::Focused && published != Modes::FocusState::Focused;
    return verdict;
}

/// Who holds a touch shell's keyboard, as one terminal surface sees it.
enum class TouchKeyboardHolder : uint8_t {
    /// The terminal: the surface, the input view that types into it, or no
    /// responder at all.
    Terminal,
    /// A text input that is not the terminal's, such as a find field or a
    /// sheet's name field.
    OtherTextInput,
};

/// Resolves a touch shell's surface focus from who holds its keyboard.
///
/// A touch shell's surface is not where its keys arrive. The keyboard is a
/// separate input view, so that the selection menu, which needs the surface
/// as first responder, does not raise it; the surface takes first responder
/// only for that menu and for a hardware keyboard's click. A handover between
/// the two is the terminal keeping its keyboard, and a surface that reads its
/// own responder status as focus draws the hollow outline from then on while
/// every key still reaches the shell. A keyboard put away leaves no responder
/// at all, and the terminal is still what the screen is for, so that is not a
/// focus change either. Focus leaves only for another text input, the one
/// state in which a key typed does not reach the shell.
///
/// The activity note is the arrival edge only, as in ``surfaceFocus``.
///
/// - Parameters:
///   - holder: Who holds the keyboard.
///   - published: The state the surface last published.
/// - Returns: The state to publish, and whether the caret must latch solid.
[[nodiscard]] constexpr SurfaceFocusVerdict touchSurfaceFocus(TouchKeyboardHolder holder, Modes::FocusState published)
{
    SurfaceFocusVerdict verdict;
    verdict.focus = holder == TouchKeyboardHolder::Terminal ? Modes::FocusState::Focused : Modes::FocusState::Unfocused;
    verdict.wantsActivityNote = verdict.focus == Modes::FocusState::Focused && published != Modes::FocusState::Focused;
    return verdict;
}

} // namespace Ubiquitty::Runtime
