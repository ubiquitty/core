// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Screen mutation — everything a sequence handler does to the grid, as opposed
// to Addressing.h which only asks it questions.
//
// The VT handlers are the callers: CSI.cpp's EL/ED/ICH/DCH/IL/DL, Escape.cpp's
// DECSC/DECRC, Modes.cpp's alternate-screen flip. They parse parameters and call
// in here; none of the parsing lives on this side of the line.

// Terminal.h rather than a forward declaration: `scrollLaneFlush` below is
// inline and dereferences `t->scrollLaneRun`, so this header needs the complete
// type. A forward declaration compiles only where every includer reaches
// Terminal.h first, an ordering nothing states and nothing checks, so a TU
// including this header on its own would break.
#include "Core/Screen/Screen.h"
#include "Core/Terminal.h"

#include <cstdint>

namespace Ubiquitty::Screen {

/// How much of each row an in-place shift moved.
enum class ShiftWidth : std::uint8_t {
    /// Whole rows moved, their metadata with them: a line on the band moves.
    FullRows,
    /// A column band moved inside rows that stayed put (DECSLRM): a line on
    /// the band keeps its rows and has its cells rewritten.
    Rectangle,
};

/// Records a content shift that moved rows in place, without appending to
/// history (region scrolls, IL/DL, a margined band, and the alternate screen's
/// scrolls; the passive terminal's applied scroll ops too). Rich-text blocks
/// and qualified areas on the band move with it, and so does a position pinned
/// on the band, a band's through a band pin, which is how every view's
/// selection follows the move.
///
/// Called before the rows move.
///
/// - Parameters:
///   - t: The terminal whose content moved.
///   - top: The inclusive top visible row of the band.
///   - bottom: The inclusive bottom visible row.
///   - rows: Rows the band moved, positive up (``CellGrid::scrollRegion``'s sign).
///   - width: Whether whole rows or a column band moved.
///   - left: A band's first column; unread for whole rows.
///   - right: A band's last column; unread for whole rows.
void noteInPlaceContentShift(Ubiquitty::Terminal& t, int top, int bottom, int rows, ShiftWidth width, int left = 0, int right = -1);

// Region scroll — SU/SD. Unlike scrollUp/scrollDown these bypass the
// cursor-outside-margins suppression, which is why the CSI handlers call them
// directly rather than the plain forms.
/// Scrolls the region up, ignoring the cursor-outside-margins suppression (SU).
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - n: Rows to move.
void scrollRegionUp(Ubiquitty::Terminal& t, int n);
/// Scrolls the region down, ignoring the cursor-outside-margins suppression (SD).
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - n: Rows to move.
void scrollRegionDown(Ubiquitty::Terminal& t, int n);

/// Scrolls the region up, suppressed while the cursor sits outside the margins.
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - n: Rows to move.
void scrollUp(Ubiquitty::Terminal& t, int n);

/// Scrolls the active screen up by `n` rows, as `n` line feeds at the bottom would.
///
/// Captures scrollback and shifts landed images — sixel records and kitty or
/// iTerm placements — up in lockstep. Image placement uses it to make room when
/// an image lands taller than the space below the cursor, so the image ends
/// flush with the bottom row instead of clipping off it.
///
/// - Parameters:
///   - t: The terminal to scroll. Null is a no-op — `scrollUp` dereferences
///     `t->modes` unguarded, so a caller that does not pre-check depends on
///     this guard.
///   - n: Rows to scroll; a non-positive count is a no-op.
void scrollViewportUp(Ubiquitty::Terminal* t, int n);
// Deferred-state scroll lane.
// scrollLaneFlush commits accumulated per-run bookkeeping and must run before
// every processBytes return and before any dispatch that can observe image
// placements or paint state.
[[nodiscard]] bool scrollLaneEligible(const Ubiquitty::Terminal& t);
/// Accumulates one line feed into the deferred scroll lane.
///
/// - Parameter t: The terminal whose lane accumulates the feed.
void scrollLaneLinefeed(Ubiquitty::Terminal& t);
/// Commits an accumulated scroll-lane run to the screen and scrollback.
///
/// - Parameter t: The terminal whose lane run is active.
void scrollLaneFlushActive(Ubiquitty::Terminal& t);
/// Commits the lane run only when the blank cell it would write has changed.
///
/// A pen change mid-run would otherwise land the wrong blank in rows the run
/// has already claimed.
///
/// - Parameter t: The terminal whose lane may need committing.
void scrollLaneFlushIfBlankChanged(Ubiquitty::Terminal& t);
/// Commits an accumulated band-lane run (deferred DECSLRM band scrolls).
///
/// - Parameter t: The terminal whose band run is active.
void bandLaneFlushActive(Ubiquitty::Terminal& t);
/// Commits any accumulated scroll-lane run, cheaply when there is none.
///
/// Must run before every `processBytes` return and before any dispatch that can
/// observe image placements or paint state.
///
/// - Parameter t: The terminal whose lane is committed.
inline void scrollLaneFlush(Ubiquitty::Terminal& t)
{
    if (t.scrollLaneRun.active) [[unlikely]]
        scrollLaneFlushActive(t);
    if (t.bandScrollRun.active) [[unlikely]]
        bandLaneFlushActive(t);
}
/// Scrolls the region down, suppressed while the cursor sits outside the margins.
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - n: Rows to move.
void scrollDown(Ubiquitty::Terminal& t, int n);
/// Erases `n` characters from the cursor rightward (ECH), leaving the cells blank in place.
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: How many characters, clamped to the line.
void eraseCharacters(Ubiquitty::Terminal& t, int n);
/// Erases part or all of the cursor's row (EL).
///
/// - Parameters:
///   - t: The terminal to erase in.
///   - mode: 0 to the row's end, 1 from its start, 2 the whole row.
void eraseInLine(Ubiquitty::Terminal& t, int mode);
/// Erases the selected portion of the tab-delimited active field (EF).
///
/// - Parameters:
///   - t: The terminal to erase in.
///   - mode: 0 through the field end, 1 from its start, or 2 for the whole field.
void eraseInField(Ubiquitty::Terminal& t, int mode);
/// Erases part or all of the qualified area holding the active position (EA).
///
/// An area runs from the position a DAQ marked to the position before the
/// next DAQ. With no DAQ at or before the active position there is no area and
/// nothing is erased. A protected area is kept while ERM is reset.
///
/// - Parameters:
///   - t: The terminal to erase in.
///   - mode: 0 from the active position to the area's end, 1 from its start through the active position, 2 the whole area.
void eraseInArea(Ubiquitty::Terminal& t, int mode);
/// Records that a qualified area begins at the active position (DAQ).
///
/// - Parameters:
///   - t: The terminal whose active screen holds the area.
///   - protection: Whether the area's contents are protected against erasure.
void defineQualifiedArea(Ubiquitty::Terminal& t, AreaProtection protection);
/// Moves the active screen's qualified area starts with a band of rows that
/// scrolled in place, as ``noteInPlaceContentShift`` reports it.
///
/// - Parameters:
///   - t: The terminal whose content moved.
///   - top: The band's first row.
///   - bottom: The band's last row.
///   - rows: Rows the band moved, positive up.
void noteAreaRowsShifted(Ubiquitty::Terminal& t, int top, int bottom, int rows);
/// Moves the primary screen's qualified area starts up with rows that a scroll
/// fed to the scrollback; a start that left the page is dropped.
///
/// - Parameters:
///   - t: The terminal that scrolled.
///   - lines: Rows fed to the scrollback.
void scrollAreasIntoHistory(Ubiquitty::Terminal& t, int lines);
/// Forgets every qualified area of both screens, as a resize does.
///
/// - Parameter t: The terminal whose areas are forgotten.
void forgetQualifiedAreas(Ubiquitty::Terminal& t);
/// Right-justifies the active field's contents and moves to its following tab stop.
///
/// Explicit spaces belong to the contents; erased trailing positions do not.
///
/// - Parameter t: The terminal receiving HTJ.
void justifyActiveField(Ubiquitty::Terminal& t);
/// Erases part or all of the display (ED).
///
/// - Parameters:
///   - t: The terminal to erase in.
///   - mode: 0 to the end, 1 from the start, 2 the screen, 3 the scrollback too.
void eraseInDisplay(Ubiquitty::Terminal& t, int mode);
/// Inserts blank rows at the cursor, pushing the region's rows down (IL).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: Rows to insert.
void insertLines(Ubiquitty::Terminal& t, int n);
/// Deletes rows at the cursor, pulling the region's rows up (DL).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: Rows to delete.
void deleteLines(Ubiquitty::Terminal& t, int n);
/// Inserts `n` blank characters at the cursor, shifting the rest of the line right (ICH).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: How many characters, clamped to the line.
void insertCharacters(Ubiquitty::Terminal& t, int n);

/// Opens cells at the cursor by shifting the left part of the row toward its margin.
///
/// - Parameters:
///   - terminal: The terminal whose active row is edited.
///   - count: Cells to open, clamped to the distance through the left margin.
void insertCharactersRightToLeft(Ubiquitty::Terminal& terminal, int count);
/// Deletes `n` characters at the cursor, shifting the rest of the line left (DCH).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: How many characters, clamped to the line.
void deleteCharacters(Ubiquitty::Terminal& t, int n);
/// Inserts blank columns across the whole scroll region (DECIC).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: Columns to insert.
void insertColumns(Ubiquitty::Terminal& t, int n); // DECIC — region-wide column insert
/// Deletes columns across the whole scroll region (DECDC).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - n: Columns to delete.
void deleteColumns(Ubiquitty::Terminal& t, int n); // DECDC — region-wide column delete
/// Moves the cursor right, panning the region left at the right margin (DECFI).
///
/// - Parameter t: The terminal to move within.
void forwardIndex(Ubiquitty::Terminal& t); // DECFI — cursor right / pan left at the margin
/// Moves the cursor left, panning the region right at the left margin (DECBI).
///
/// - Parameter t: The terminal to move within.
void backIndex(Ubiquitty::Terminal& t); // DECBI — cursor left / pan right at the margin
/// Switches between the primary and alternate screens.
///
/// - Parameters:
///   - t: The terminal to switch.
///   - screen: Which screen becomes active.
void applyAlternateScreenChange(Ubiquitty::Terminal& t, Ubiquitty::Modes::ScreenChoice screen);
/// Records a margin pair DECSLRM set narrower than the screen.
///
/// The pair feeds the row-join heuristic in the link walk: a multiplexer
/// scrolls a pane by setting the pane's columns as margins, so the pairs seen
/// here are the pane geometry even though they are cleared again before the
/// pane's text is written.
///
/// - Parameters:
///   - s: The screen the margins were set on.
///   - left: The left margin, 0-based inclusive.
///   - right: The right margin, 0-based inclusive.
void rememberMargins(State& s, int left, int right);

/// Drops every remembered margin pair.
///
/// - Parameter s: The screen.
void forgetMargins(State& s);

/// Saves cursor position, pen and character sets for a later restore (DECSC).
///
/// - Parameter t: The terminal whose state is saved.
void saveCursor(Ubiquitty::Terminal& t);
/// Restores the cursor state saved by DECSC (DECRC).
///
/// - Parameter t: The terminal whose state is restored.
void restoreCursor(Ubiquitty::Terminal& t);

/// Puts the cursor on the screen's last column with its wrap deferred, the
/// state a character printed into that column leaves: the next character
/// printed wraps to the following row first when autowrap is on.
///
/// - Parameter t: The terminal whose active screen cursor is placed; its row
///   is unchanged.
void deferWrapAtLastColumn(Ubiquitty::Terminal& t);

/// Clamps the cursor into the origin-mode region.
///
/// Horizontal gridFloor applies only when DECLRMM and DECOM are both set — the
/// origin-mode-with-margins case. Outside it the cursor is clamped to the full
/// screen, because DECOM alone constrains rows and not columns.
///
/// - Parameter t: The terminal whose active screen cursor is clamped.
void cursorClampOrigin(Ubiquitty::Terminal& t);

/// Records the active position as where the string a later QUAD positions begins.
///
/// The formator functions (LF, VT, FF, NEL, RI, HVP, VPA, VPB and VPR) and QUAD
/// call it after they move the active position. A pending wrap puts the start one
/// column past the cursor, where the next graphic character lands.
///
/// - Parameter t: The terminal whose active position starts the string.
inline void noteStringStart(Ubiquitty::Terminal& t)
{
    t.stringStartColumn = t.screens.active->cursorX + (t.screens.active->wrapPending ? 1 : 0);
}

/// Which way a horizontal region scroll moves its content.
enum class HorizontalScrollDirection {
    Left,
    Right
};
/// Scrolls the region sideways, blanking the columns it reveals (SL and SR).
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - n: Columns to move.
///   - direction: Which way the content travels.
void scrollRegionHorizontal(Ubiquitty::Terminal& t, int n, HorizontalScrollDirection direction);

// DEC rectangle-area ops (DECFRA/ERA/SERA/RARA/CARA/CRA). The prefixed-CSI
// handlers (VT/Prefixed.cpp) and DECRQCRA build a RectArea then call these;
// the bodies live in Screen/Rect.cpp.
struct RectArea {
    int top, left, bot, right;
};
/// Fills a rectangle with one character (DECFRA).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - r: The rectangle, already clamped to the grid.
///   - ch: The code point to write into every cell.
void rectFill(Ubiquitty::Terminal& t, const RectArea& r, uint32_t ch);
/// Blanks every cell of a rectangle (DECERA).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - r: The rectangle, already clamped to the grid.
void rectErase(Ubiquitty::Terminal& t, const RectArea& r);
/// Blanks only the unprotected cells of a rectangle (DECSERA).
///
/// Cells marked protected by DECSCA keep their contents.
///
/// - Parameters:
///   - t: The terminal to edit.
///   - r: The rectangle, already clamped to the grid.
void rectSelectiveErase(Ubiquitty::Terminal& t, const RectArea& r);
/// Copies one rectangle over another, overlapping safely (DECCRA).
///
/// - Parameters:
///   - t: The terminal to edit.
///   - src: The source rectangle, already clamped to the grid.
///   - dt: Destination top row.
///   - dl: Destination left column.
void rectCopy(Ubiquitty::Terminal& t, const RectArea& src, int dt, int dl);
/// Flips the named attributes on every cell of a rectangle (DECRARA).
///
/// The walk honours DECSACE: in stream mode the region is reading-order, so the
/// first and last rows are partial and the middle rows are full width; in
/// rectangle mode it is the block the corners describe. Which one a terminal
/// means by the same coordinates is the mode's whole purpose.
///
/// - Parameters:
///   - t: The terminal to edit.
///   - r: The rectangle, already clamped to the grid.
///   - toggleMask: The attributes to invert. Attributes outside it are left as
///     they are, so an empty mask is a no-op rather than a clear.
void rectReverseAttributes(Ubiquitty::Terminal& t, const RectArea& r, Ubiquitty::VT::AttributeSet toggleMask);
/// Sets and clears attributes across a rectangle (DECCARA).
///
/// The walk honours DECSACE, exactly as `rectReverseAttributes` describes.
///
/// - Parameters:
///   - t: The terminal to edit.
///   - r: The rectangle, already clamped to the grid.
///   - setMask: Attributes to turn on.
///   - clearMask: Attributes to turn off.
void rectChangeAttributes(Ubiquitty::Terminal& t, const RectArea& r, uint16_t setMask, uint16_t clearMask);

/// Gives the cells of both screens the colours DECAC assigned to normal text
/// in place of the default ones.
///
/// The VT520 DECAC normal text colours are the default foreground and
/// background, so every position of either screen held in the default colours,
/// or in the colours an earlier assignment resolved in their place, takes them,
/// blank positions included. A colour a cell was given explicitly stays,
/// whatever its index. Scrollback keeps the colours it was written in.
///
/// - Parameters:
///   - t: The terminal whose screens to recolour.
///   - assigned: The normal text colours now assigned.
void applyNormalTextColors(Ubiquitty::Terminal& t, const Ubiquitty::ColorAssignment& assigned);

} // namespace Ubiquitty::Screen
