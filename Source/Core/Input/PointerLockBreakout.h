// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The reserved break-out gesture for pointer lock (private DECSET 7403), plus the
// escape-hatch hint policy. A hostile/hung app that locks the pointer and never
// sends `7403l` would trap it, so the frontend intercepts a MANDATORY break-out
// chord ABOVE the entire configurable key stack (host/app, [keys] rebinds, escape
// stream) — nothing an app or config does can suppress it.
//
// Gesture: HOLD Control+Command. ⌃⌘ sits outside the escape stream (unlike "hold
// Esc"), never reaches a DOS app, and matches VM release chords. ARMED only while
// Control+Command are the SOLE modifiers with no other key, and while pointer lock
// is active, so it never shadows a real ⌃⌘ shortcut; the hold keeps a passing brush
// of the two modifiers from breaking a lock the user still wants.

#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::Input {

// Hold the armed chord this long, in the 60 Hz ticks every arm's clock counts
// (``PAL::monotonicTicks``): 500 ms. A fixed reserved constant, not
// config-tunable, because both ends of the range defeat the safety valve: 0
// would fire on a mere brush of the modifiers; a huge value would never fire.
inline constexpr std::uint32_t pointerLockBreakoutHoldTicks = 30;

// The keyboard state the arming predicate reads; the frontend fills these from
// the current modifier flags and its key-down state.
struct PointerLockKeyState {
    bool isControlHeld = false;
    bool isCommandHeld = false;
    bool isShiftHeld = false;
    bool isOptionHeld = false;
    bool isAnyNonModifierKeyHeld = false;
};

// True when the break-out chord is ARMED: Control and Command are the only
// modifiers down and no non-modifier key is pressed. Any extra modifier or key
// makes this false (it is then a real ⌃⌘ shortcut, not the break-out), which
// disarms the hold.
[[nodiscard]] constexpr bool isPointerLockBreakoutArmed(const PointerLockKeyState& keys)
{
    return keys.isControlHeld && keys.isCommandHeld && !keys.isShiftHeld && !keys.isOptionHeld
        && !keys.isAnyNonModifierKeyHeld;
}

/// Whether the frontend holds the pointer grab it performs for 7403.
enum class PointerLockGrab : std::uint8_t {
    /// The grab ended, or was never taken.
    Released UBIQUITTY_SWIFT_NAME(released),
    /// The pointer is captured and the cursor hidden.
    Held UBIQUITTY_SWIFT_NAME(held)
};

/// What a frontend sees about its surface that Core cannot.
///
/// Whether this surface is the live input target -- on AppKit the active
/// app's key window's first responder, on GTK the focused widget, on the web
/// the document's pointer-lock element -- is the one fact the lock decision
/// needs from outside the model. A backgrounded surface must not hold a
/// pointer no window owns, and must take it back on return without the
/// application re-sending the mode.
struct PointerLockFacts {
    /// Whether the surface is the live input target right now.
    bool isSurfaceFocused = false;
};

/// The grab a frontend should hold.
enum class PointerLockVerdict : std::uint8_t {
    /// Release the grab, or leave it untaken.
    Release UBIQUITTY_SWIFT_NAME(release),
    /// Capture the pointer and hide the cursor.
    Hold UBIQUITTY_SWIFT_NAME(hold)
};

/// Whether the mode bit is set: the terminal's half of the decision.
enum class PointerLockMode : std::uint8_t {
    /// 7403 is clear.
    Clear UBIQUITTY_SWIFT_NAME(clear),
    /// 7403 is set.
    Set UBIQUITTY_SWIFT_NAME(set)
};

/// Decides whether a surface should hold the grab.
///
/// The mode is the application's ask and the focus is the surface's
/// standing to answer it; both are needed. Idempotent, so a frontend
/// re-asks on every edge that can move either half -- a mode drain, a
/// focus change, a window-key change, app activation -- and reconciles the
/// OS grab against the answer.
///
/// - Parameters:
///   - mode: Whether 7403 is set.
///   - facts: What the surface sees.
/// - Returns: Whether to hold the grab.
[[nodiscard]] constexpr PointerLockVerdict resolvePointerLock(PointerLockMode mode, const PointerLockFacts& facts)
{
    return mode == PointerLockMode::Set && facts.isSurfaceFocused ? PointerLockVerdict::Hold : PointerLockVerdict::Release;
}

/// The break-out chord's state: armed at a tick, or not.
///
/// Modifiers do not auto-repeat, so the hold is measured from the tick the
/// chord became the sole modifiers to the tick a frontend asks again; the
/// frontend asks once, at ``pointerLockChordTicksUntilDeadline``.
struct PointerLockChord {
    /// Whether the chord is the sole modifiers held while the grab is held.
    bool isArmed = false;
    /// The tick it became so.
    std::uint32_t armedAtTicks = 0;
};

/// What the chord's tick decided.
enum class PointerLockChordVerdict : std::uint8_t {
    /// Keep the grab.
    Keep UBIQUITTY_SWIFT_NAME(keep),
    /// The chord was held for the threshold: release the grab and clear the mode.
    BreakOut UBIQUITTY_SWIFT_NAME(breakOut)
};

} // namespace Ubiquitty::Input
