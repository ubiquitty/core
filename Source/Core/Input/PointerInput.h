// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PointerInput — frontend-neutral pointer / tab-nav / scroll-follow policy. Each
// frontend extracts raw fields from its native event and calls these, so the xterm
// button/modifier mapping, tab jump, and hold-selection clamp are defined once. Plain scalars.

#include "Base/SwiftInterop.h"
#include "Base/Types.h"

namespace Ubiquitty::Input {

// A pointer button in xterm's identity space (what the report encodes),
// independent of any platform's button-numbering quirk (NSEvent orders them
// left/right/middle; GDK left/middle/right). Frontends map their native
// button to this.
// The Swift names carry the language's own case convention, so a Swift
// caller writes `.left` rather than the C++ spelling.
enum class PointerButton {
    Left UBIQUITTY_SWIFT_NAME(left),
    Middle UBIQUITTY_SWIFT_NAME(middle),
    Right UBIQUITTY_SWIFT_NAME(right),
    Other UBIQUITTY_SWIFT_NAME(other),
    /// No button: a plain move, which any-event tracking (1003) reports with
    /// xterm's button code 3 and every other mode leaves unreported.
    None UBIQUITTY_SWIFT_NAME(none),
    // Extended buttons (physical 8-11 — back/forward + two more). xterm encodes
    // them in the high-button range Cb 128-131 (the 0x80 class bit), distinct
    // from the wheel class (0x40). A frontend maps its native extra-button
    // identity to these; Other stays the catch-all for anything unmapped.
    Button8 UBIQUITTY_SWIFT_NAME(button8),
    Button9 UBIQUITTY_SWIFT_NAME(button9),
    Button10 UBIQUITTY_SWIFT_NAME(button10),
    Button11 UBIQUITTY_SWIFT_NAME(button11)
};

/// Up/Down are the vertical wheel; Left/Right are the horizontal wheel.
enum class WheelDirection : uint8_t {
    Down UBIQUITTY_SWIFT_NAME(down),
    Up UBIQUITTY_SWIFT_NAME(up),
    Left UBIQUITTY_SWIFT_NAME(left),
    Right UBIQUITTY_SWIFT_NAME(right)
};
/// The modifier keys held during a pointer event.
enum class ModifierState : uint8_t {
    OtherModifiers,
    AcceleratorOnly
};

// xterm mouse-report button code: left 0, middle 1, right 2 (Other → 0);
// extended buttons 8-11 → 128-131 (the 0x80 high-button class).
int xtermButtonCode(PointerButton);

// Wheel-notch xterm button code: up → 64, down → 65, left → 66, right → 67 (the
// 0x40 wheel class). The wheel reports as a press of these synthetic buttons.
int xtermWheelButtonCode(WheelDirection direction);

// The wheel direction a two-axis scroll delta reports: the dominant axis, vertical
// winning ties. deltaY > 0 → Up; deltaX > 0 → Left (xterm's convention). Assumes
// the caller dropped the all-zero delta.
WheelDirection wheelDirectionFromScrollDelta(double deltaX, double deltaY);

// The delta on the axis wheelDirectionFromScrollDelta() reports, so a caller
// can throttle the axis it is about to send. Same dominance rule, vertical
// winning ties, so the two never disagree about which axis is in play.
double wheelDominantDelta(double deltaX, double deltaY);

// Motion-report dedup: xterm reports motion only when the coordinate CHANGES, so
// this gate suppresses redundant reports. Cell-granular modes (X10 / SGR 1006 /
// URXVT 1015) dedup on (row, column); SGR-pixel mode (1016) dedups on device pixel.
// A mouseDown resets the sentinels so a press always reports.
class MotionDedup {
public:
    // Cell-granular modes: report only when (row, column) differs from the last
    // reported cell. Updates the remembered cell when it returns true.
    bool shouldReport(int row, int column);

    // SGR-pixel mode (1016): report only when the device pixel (px, py) differs
    // from the last reported pixel. Updates the remembered pixel when it returns
    // true.
    bool shouldReport1016(int px, int py);

    // Record a cell as "last reported" without gating. The SGR-pixel path calls this
    // after a pixel-mode report so the cell sentinel stays coherent if the mode flips
    // back to cell granularity mid-drag.
    void noteCell(int row, int column);

    // Clear both sentinels so the next report always fires.
    void reset();

    /// Records a press as the last reported position.
    ///
    /// A press is itself a report at that cell, so motion inside the same cell
    /// afterwards is redundant — which is what xterm does, and what decides
    /// whether an application reads a click as a click or as a drag. Clearing
    /// alone lets the first motion after every press through, and a click that
    /// wobbles one pixel then arrives as press, motion, release.
    ///
    /// The pixel sentinel stays cleared, so sub-cell motion still reports under
    /// SGR-pixel mode (1016), which is the granularity that mode exists for.
    ///
    /// - Parameters:
    ///   - row: The pressed cell's row.
    ///   - column: Its column.
    void notePress(int row, int column);

private:
    int m_lastRow = -1; // -1 = nothing reported yet
    int m_lastColumn = -1;
    int m_lastPixelX = sentinelPixel; // sentinelPixel = nothing reported yet
    int m_lastPixelY = sentinelPixel;

public:
    // The "no pixel reported yet" sentinel, distinct from any real device pixel (>= 0).
    static constexpr int sentinelPixel = -2147483647 - 1; // INT_MIN, header-safe
};

// xterm mouse-report modifier bits (TERM_MOUSE_MOD_*): shift 4, meta 8
// (Option/Alternate), control 16, OR'd together. Matches the bit set xterm's
// mouse encoding expects; Core::Mouse::encodeReport consumes the result.
int xtermMouseModifiers(bool shift, bool alternate, bool control);

// ⌘1–9 tab jump (and its GTK/X11 accelerator equivalent): returns 1..9 when
// only the tab-switch accelerator is held and `character` is an ASCII digit
// '1'..'9'; otherwise 0 (no jump). `modifiers` is the frontend's
// "exactly the accelerator modifier is down" check (Command on macOS).
int commandNumberTabPosition(ModifierState modifiers, uint32_t character);

/// What a view knows when it is about to re-resolve a link hover without a
/// mouse move behind it.
struct HoverReResolveFacts {
    bool isSelecting; // a drag owns the pointer
    bool isPointerHiddenForTyping; // typing put it away
    bool isWindowKey; // this window owns the pointer
    bool isPointerInsideView; // the pointer is over this view's bounds
};

/// Whether to re-resolve the link hover at wherever the pointer now is.
///
/// The occasions with no mouse move behind them: a modifier press, which
/// changes whether a link opens, and an output drain, which scrolls the grid
/// under a pointer that never moved. Neither carries a usable location, so
/// the view asks the window where the pointer is — and these are the four
/// reasons it should not.
///
/// The key-window one is the least obvious and the one that bites: native
/// window tabs share a frame, so a tab in the background re-resolving on its
/// own output reads one pointer position with the tab in front of it, and a
/// link under it would set the hand cursor for the whole screen.
///
/// - Parameter facts: What the view knows.
/// - Returns: Whether to re-resolve.
[[nodiscard]] bool shouldReResolveLinkHover(const HoverReResolveFacts& facts);

// Viewport-hold while the tail grows: push viewOffset by the rows that left
// the screen for history since the last drain (clamped to scrollbackCount) so
// the viewed rows stay put instead of scrolling out from under the eye. The
// growth is the rows appended, not the change in the retained count: once
// history is at its limit each appended row evicts one, the count stands
// still while the content shifts a row per line, and a hold measured by the
// count drifts toward the tail one row per line of output. Returns the new
// viewOffset (== input when nothing was appended or it is already at the
// oldest line). The caller snaps to the tail instead when the view follows.
int heldViewOffsetForSelection(int viewOffset, int scrollbackCount, int rowsAppended);

/// The caller's verdict on whether the viewport stays put.
///
/// The Swift names carry the language's own case convention, so a Swift
/// caller writes `.hold` rather than the C++ spelling.
enum class ViewportHold : std::uint8_t {
    /// The view follows the live tail.
    Follow UBIQUITTY_SWIFT_NAME(follow),
    /// The viewed rows stay put while the tail grows beneath.
    Hold UBIQUITTY_SWIFT_NAME(hold),
};

/// The view offset after output landed under a scrolled-back view.
///
/// A held viewport compensates by exactly the tail's growth — the reader's
/// rows stay put while history accumulates beneath — and an unheld one snaps
/// to the live tail. The hold decision itself belongs to the caller: a
/// selection always holds, a busy model lock holds (the non-disruptive
/// default for a user reading history), and otherwise the scroll-on-output
/// mode decides.
///
/// Spelled in `ptrdiff_t` rather than `int` because the seam is consumed by
/// Swift, where `int` imports as `Int32` and every call pays a conversion
/// wrapper per argument; the pointer-width type imports as `Int` and the
/// ceremony disappears.
///
/// - Parameters:
///   - hold: The caller's hold verdict.
///   - viewOffset: Rows above the tail before the output.
///   - scrollbackCount: Rows retained after the output.
///   - rowsAppended: Rows scrolled into history by the output, the delta of
///     the monotonic appended count rather than of the retained one.
/// - Returns: The offset to present.
[[nodiscard]] std::ptrdiff_t viewOffsetAfterOutput(ViewportHold hold, std::ptrdiff_t viewOffset, std::ptrdiff_t scrollbackCount, std::ptrdiff_t rowsAppended);

/// Whether a selection stands when output lands.
enum class SelectionPresence : std::uint8_t {
    /// Nothing is selected.
    Absent UBIQUITTY_SWIFT_NAME(absent),
    /// A selection is standing, and its rows are what the viewport holds.
    Present UBIQUITTY_SWIFT_NAME(present),
};

/// The scroll-on-output mode, as the caller managed to read it.
///
/// `Indeterminate` is a real answer rather than a missing one. The mode lives
/// behind the model lock, and the per-drain path cannot afford to block on it
/// under flood, so a caller that finds the lock busy has no verdict to give.
enum class OutputSnapPolicy : std::uint8_t {
    /// The mode returns the view to the live tail.
    SnapToTail UBIQUITTY_SWIFT_NAME(snapToTail),
    /// The mode leaves the viewed rows where they are.
    HoldViewedRows UBIQUITTY_SWIFT_NAME(holdViewedRows),
    /// The mode could not be read.
    Indeterminate UBIQUITTY_SWIFT_NAME(indeterminate),
};

/// Whether the viewport holds its rows when output lands beneath it.
///
/// A view already at the tail always follows, including while a selection
/// gesture is open: the selected content moves with the terminal while the
/// gesture head remains at the pointer. A standing selection holds only when
/// the view is already back in history. Between those, the mode decides.
///
/// An unread mode holds, which is the non-disruptive answer: a reader part-way
/// up their history keeps their place, and the next drain re-asks. Following on
/// an unread mode would yank the viewport to the tail for a lock that happened
/// to be busy, and under flood it is busy on exactly the drains a reader
/// notices.
///
/// - Parameters:
///   - selection: Whether a selection stands.
///   - viewOffset: Rows above the tail before the output.
///   - snapPolicy: What the scroll-on-output mode says, if it could be read.
/// - Returns: Whether to hold the viewed rows or follow the tail.
[[nodiscard]] ViewportHold viewportHoldForOutput(SelectionPresence selection, std::ptrdiff_t viewOffset, OutputSnapPolicy snapPolicy);

/// Whether the scroll-on-output mode decides this drain.
///
/// A standing selection or a view already at the tail answers without it,
/// so a caller whose mode costs something to read -- a non-blocking borrow of
/// the model lock, taken on every drain -- asks this first and skips the read.
/// The answer is the policy's own precondition rather than a caller's guess at
/// it, which is what keeps the skip from becoming a fourth composition.
///
/// - Parameters:
///   - selection: Whether a selection stands.
///   - viewOffset: Rows above the tail before the output.
/// - Returns: True when the mode is consulted.
[[nodiscard]] bool isSnapPolicyConsulted(SelectionPresence selection, std::ptrdiff_t viewOffset);

/// What a viewport presents after output landed beneath it.
struct ViewportAfterOutput {
    /// The offset to present.
    std::ptrdiff_t viewOffset = 0;
    /// The verdict that produced it, for a caller whose follow path does more
    /// than move the offset.
    ViewportHold hold = ViewportHold::Follow;
};

/// Resolves the viewport a frontend presents once output has landed.
///
/// The whole policy in one call: the hold verdict and the offset it implies.
/// Frontends own where the offset is stored and what a follow does besides
/// moving it -- a native scroller to mirror, a redraw to mark -- and nothing
/// else. Three arms composed these fragments separately and reached three
/// answers about when a scrolled-back view snaps during output.
///
/// - Parameters:
///   - selection: Whether a selection stands.
///   - viewOffset: Rows above the tail before the output.
///   - snapPolicy: What the scroll-on-output mode says, if it could be read.
///   - scrollbackCount: Rows retained after the output.
///   - rowsAppended: Rows scrolled into history by the output, the delta of
///     the monotonic appended count rather than of the retained one.
/// - Returns: The offset to present, and the verdict behind it.
[[nodiscard]] ViewportAfterOutput viewportAfterOutput(SelectionPresence selection, std::ptrdiff_t viewOffset, OutputSnapPolicy snapPolicy, std::ptrdiff_t scrollbackCount, std::ptrdiff_t rowsAppended);

} // namespace Ubiquitty::Input
