// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/MousePointerShape.h"

namespace Ubiquitty {

struct Terminal;

namespace Runtime {
struct RuntimeSnapshot;
}

namespace Input {

/// What a frontend knows about the pointer that the terminal model does not.
///
/// Everything here is a fact about the window or the gesture in progress —
/// where the pointer is, which modifiers are down, what the frontend has already
/// resolved about the thing under it. The terminal's own state (Tek mode, mouse
/// tracking, an app-requested shape) is read from the `Terminal` instead, so a
/// frontend cannot answer those two ways.
struct PointerContext {
    /// The pointer is grabbed and hidden for relative-motion reporting
    /// (DECSET 7403), so no shape is shown at all.
    bool isPointerLocked = false;

    /// The pointer is over an activatable link — an OSC 8 hyperlink, or a
    /// regex/path match with its activation modifier held.
    bool isOverLink = false;

    /// The window is chromeless and the window-drag modifier is held, so a drag
    /// here moves the window rather than selecting.
    bool isWindowDragArmed = false;

    /// A rectangular (column-block) selection is armed, by a held modifier or a
    /// sticky mode.
    bool isRectangularSelectArmed = false;

    /// A drag would select despite the application tracking the mouse, because
    /// the frontend's bypass modifier is held. Over such an application the
    /// pointer wears the I-beam (or the crosshair, when the block modifier is
    /// the one held) for as long as this holds.
    bool selectionBypassesMouseReporting = false;

    /// The pointer is past the end of the text on its row, where there is
    /// nothing to select.
    bool isOverTextVoid = false;

    /// A selection drag is in flight: a press opened a gesture and it has not
    /// ended.
    ///
    /// The gesture, not the button — a press an application received opens no
    /// selection, so a frontend reports what the ladder answered about the
    /// press rather than that a button went down.
    bool isSelectionDragInFlight = false;
};

/// The pointer shape for the terminal's current state.
///
/// One ordered decision, because the cases overlap and the order is the whole
/// content: a link outranks an app-requested shape so an `OSC 8` hyperlink stays
/// clickable under a TUI that asked for a crosshair; a window drag outranks the
/// rectangular-select crosshair because in a chromeless window the drag wins
/// regardless; a tracking application outranks the I-beam because it owns the
/// clicks, and advertising selectable text would mislead.
///
/// Returns a shape even when the pointer is hidden — a locked pointer resolves
/// to `Default` rather than to nothing, so a caller has one type to handle.
[[nodiscard]] MousePointerShape pointerShape(const Terminal*, const PointerContext&);

/// The pointer shape for a session's published snapshot.
///
/// The same ladder as the model-side reading, for a frontend that resolves
/// the pointer on the main thread between drains, where the model lock is
/// the wrong thing to take. One ordering, so an arm off the lock cannot rank
/// a link against an application's shape differently from an arm holding
/// the terminal.
[[nodiscard]] MousePointerShape pointerShape(const Runtime::RuntimeSnapshot&, const PointerContext&);

} // namespace Input
} // namespace Ubiquitty
