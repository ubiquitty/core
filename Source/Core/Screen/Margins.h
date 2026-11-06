// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Screen.h"
#include "Core/Terminal.h"

// Every question about DECSET 69 left/right margins, in one place.
//
// `leftRightMarginMode` on its own answers none of them. The flag says the host
// enabled margins; it does not say the margins currently constrain anything, nor
// where the cursor sits relative to them. Editing, scrolling and horizontal
// motion each need a derived answer, and the derivation belongs here once.
//
// Screen state only — the active screen's margins, the mode bit, and the column
// count. Nothing here reads parser state, which is why it lives in Screen rather
// than behind an include of the sequence layer.
//
// Inline: these are the rect-scroll, line-edit and horizontal-motion paths.

namespace Ubiquitty::Screen {

/// Resolves SLH against the active row width and the applicable DEC left margin.
///
/// - Parameter t: The terminal whose active row supplies the bounds.
/// - Returns: The zero-based destination column.
[[nodiscard]] inline int lineHomeColumn(const Ubiquitty::Terminal& t)
{
    const auto& screen = *t.screens.active;
    const int left = t.modes.leftRightMarginMode && screen.cursorX >= screen.scrollLeft ? screen.scrollLeft : 0;
    const int right = t.columns / textScaleForLineAttribute(screen.lineAttribute[screen.cursorY]).widthScale - 1;
    return std::min(std::max(t.lineHome, left), right);
}

/// Resolves the CR destination in the writing direction.
///
/// Cursor addressing retains `lineHomeColumn` regardless of DECRLM.
///
/// - Parameter t: The terminal whose active row supplies the width and margins.
/// - Returns: The zero-based destination column.
[[nodiscard]] inline int writingHomeColumn(const Ubiquitty::Terminal& t)
{
    if (!t.shouldWriteRightToLeft)
        return lineHomeColumn(t);
    const auto& screen = *t.screens.active;
    const int last = t.columns / textScaleForLineAttribute(screen.lineAttribute[screen.cursorY]).widthScale - 1;
    return t.modes.leftRightMarginMode && screen.cursorX <= screen.scrollRight ? std::min(last, screen.scrollRight) : last;
}

/// Resolves the line limit position on the active row (SLL).
///
/// The limit stays inside the left and right margins that apply to the cursor, and a limit nobody
/// set is the far edge of the row in the writing direction.
///
/// - Parameter t: The terminal whose active row supplies the width and margins.
/// - Returns: The zero-based column.
[[nodiscard]] inline int lineLimitColumn(const Ubiquitty::Terminal& t)
{
    const auto& screen = *t.screens.active;
    const int last = t.columns / textScaleForLineAttribute(screen.lineAttribute[screen.cursorY]).widthScale - 1;
    const bool isInsideMargins = t.modes.leftRightMarginMode && screen.cursorX >= screen.scrollLeft && screen.cursorX <= screen.scrollRight;
    const int left = isInsideMargins ? screen.scrollLeft : 0;
    const int right = isInsideMargins ? std::min(last, screen.scrollRight) : last;
    const int steps = t.lineLimit > 0 ? t.lineLimit - 1 : last;
    const int target = t.shouldWriteRightToLeft ? last - steps : steps;
    return std::min(std::max(target, left), right);
}

/// Resolves the destination of carriage return and next line.
///
/// With SIMD at its default they move to the line home position. Against the
/// progression they move to the line limit position.
///
/// - Parameter t: The terminal whose active row supplies the width and margins.
/// - Returns: The zero-based destination column.
[[nodiscard]] inline int returnColumn(const Ubiquitty::Terminal& t)
{
    if (t.implicitMovementDirection == VT::ImplicitMovementDirection::AgainstProgression)
        return lineLimitColumn(t);
    return writingHomeColumn(t);
}

/// The row's usable bounds. Mode off gives the full width regardless of the stored
/// margins, which is what makes this safe to call unconditionally — and is why
/// every horizontal-motion path should route through it rather than branch on the
/// mode itself.
[[nodiscard]] inline int effectiveLeft(const Ubiquitty::Terminal& t)
{
    return t.modes.leftRightMarginMode ? t.screens.active->scrollLeft : 0;
}

[[nodiscard]] inline int effectiveRight(const Ubiquitty::Terminal& t)
{
    return t.modes.leftRightMarginMode ? t.screens.active->scrollRight : t.columns - 1;
}

// The margins genuinely narrow the row.
//
/// Distinct from the mode being on: with the mode set and the margins spanning the
/// full width, the narrow path and the wide path give identical results, and the
/// wide one moves whole rows instead of per-cell spans. Callers that only need
/// bounds should use effectiveLeft/effectiveRight, which already fold this in.
[[nodiscard]] inline bool isRowNarrowedByMargins(const Ubiquitty::Terminal& t, const Ubiquitty::Screen::State* s)
{
    return t.modes.leftRightMarginMode && (s->scrollLeft > 0 || s->scrollRight < t.columns - 1);
}

// The cursor sits outside the usable bounds.
//
/// DEC suppresses margin-relative edits here rather than gridFloor the cursor into
/// the region: an IL issued with the cursor parked outside the margins does
/// nothing, and moving the cursor to make it work would be the wrong repair.
[[nodiscard]] inline bool isCursorOutsideMargins(const Ubiquitty::Terminal& t)
{
    const Ubiquitty::Screen::State* s = t.screens.active;
    return s->cursorX < effectiveLeft(t) || s->cursorX > effectiveRight(t);
}

} // namespace Ubiquitty::Screen
