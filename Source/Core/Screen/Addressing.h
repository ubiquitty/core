// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Screen/Scrollback.h"

// The screen as a queryable model: where a row's cells actually live, what a
// visible row resolves to once scrollback and reflow are accounted for, and the
// marks a shell integration hangs off absolute rows.
//
// This is the surface the RENDERER consumes, and it is what a Tek plot or a
// sixel image mutates — none of which involves an escape sequence. It lived in
// Terminal.h, so asking "what is on row 7" meant compiling the parser; the
// implementation is in Core/Screen/ and now the declarations are beside it.
//
// Addresses come in two spaces and they are not interchangeable:
//   - VISIBLE (viewOffset, row) — what the user is looking at, scrolled.
//   - ABSOLUTE row — a fixed index across history + live screen, stable while
//     rows age into scrollback. Marks and prompt navigation use these.

#include "Core/Cell.h"
#include "Core/RowMeta.h"
#include "Core/Screen/ScrollLine.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Ubiquitty {

struct Terminal;

namespace Screen {

/// A field starts at one tab stop and ends immediately before the next.
struct CharacterField {
    /// The zero-based column of the field's starting stop.
    int firstColumn;
    /// The zero-based column of the stop immediately following the field.
    int followingStop;
};

/// Finds the tab-delimited field containing the active presentation position.
///
/// Both boundary stops must exist on the visible part of the line.
///
/// - Parameter terminal: The terminal whose tab stops delimit the field.
/// - Returns: The field, or no value outside a delimited field.
[[nodiscard]] std::optional<CharacterField> activeCharacterField(const Terminal& terminal);

/// The view offset as the addressing applies it, rather than as the caller
/// holds it.
///
/// Two policies live here and nowhere else. An alternate screen shows the
/// application's own page, so no scrollback row is on it and the offset is
/// zero whatever the frontend still has stored — and frontends do keep a
/// stale one, because entering the alternate screen resets no scroll
/// position. The rest is the clamp into `[0, scrollbackCount]`.
///
/// Any consumer mapping between absolute and visible rows resolves the offset
/// through this first, or it answers for a row this terminal says is not
/// shown.
///
/// - Parameters:
///   - t: The terminal, or null for a resolved offset of zero.
///   - viewOffset: The offset as held by the caller.
/// - Returns: The offset the addressing uses, in [0, scrollbackCount].
[[nodiscard]] int resolvedViewOffset(const Ubiquitty::Terminal* t, int viewOffset);

/// The visible row an absolute row lands on.
///
/// The inverse of the map `visibleCell` applies — `absolute = history -
/// viewOffset + row` — as one function, because it was hand-solved at five
/// places in Core and two more in the frontends, and a policy added to the
/// forward direction reached none of them.
///
/// - Parameters:
///   - history: The scrollback row count.
///   - viewOffset: An offset already through ``resolvedViewOffset``.
///   - absoluteRow: The row to place, in absolute buffer coordinates.
/// - Returns: The visible row, which the caller range-checks against `rows`;
///   a value outside `[0, rows)` means the row is not on screen.
[[nodiscard]] constexpr int visibleRowForAbsolute(int history, int viewOffset, int absoluteRow)
{
    return absoluteRow - history + viewOffset;
}

/// The absolute row a visible row reads from.
///
/// The other direction of ``visibleRowForAbsolute``, stated here for the
/// same reason: a frontend that hand-solves it answers for a row this
/// terminal never showed, and the two directions have to move together.
///
/// - Parameters:
///   - history: The scrollback row count.
///   - viewOffset: An offset already through ``resolvedViewOffset``.
///   - visibleRow: A zero-based row of the visible grid.
/// - Returns: The absolute row.
[[nodiscard]] constexpr int absoluteRowForVisible(int history, int viewOffset, int visibleRow)
{
    return history - viewOffset + visibleRow;
}

[[nodiscard]] const Cell* visibleCell(const Ubiquitty::Terminal* t, int viewOffset, int row, int column);

// A visible row (viewOffset/row space) resolved to its CONTIGUOUS source cells
// + valid length in one call, so a full-row consumer doesn't re-run
// visibleCell's history/alt-screen resolution per column. base is null (or
// length 0) for an all-blank row; cells at [length, columns) are blank.
// needsShaping flags a display-time bidi transform this struct does not apply
// (callers wanting shaping go through getDisplayRow).
struct VisibleRow {
    const Cell* base;
    int length;
    bool needsShaping;
};
[[nodiscard]] VisibleRow resolveVisibleRow(const Ubiquitty::Terminal* t, int viewOffset, int row);

/// Reads one column of an already-resolved row, with ``visibleCell``'s blank convention.
///
/// Columns at or past the row's length are blank, and so is every column of a row with no cells —
/// which is what ``visibleCell`` answers for them. A caller that resolved the row once therefore
/// reads exactly the cells it would have read one ``visibleCell`` call at a time, without paying
/// that call's history depth and ring resolution per column.
///
/// - Parameters:
///   - row: A row from ``resolveVisibleRow``.
///   - column: The column to read.
///   - columns: The terminal's width; a column at or past it is blank.
/// - Returns: The cell, never null.
[[nodiscard]] inline const Cell* cellInResolvedRow(const VisibleRow& row, int column, int columns)
{
    if (column < 0 || column >= columns || !row.base || column >= row.length)
        return &defaultBlankCell;
    return &row.base[column];
}

// Soft-wrap flag for a visible row (same viewOffset/row addressing as
// visibleCell): true when the row continues into the next without an
// explicit CR/LF. Reads the scrollback line's `wrapped` for aged rows and the
// active screen's lineWrapped view for on-screen rows. Lets the frontend stitch
// a URL that wraps across visual rows into one link. false out of range.
[[nodiscard]] bool visibleRowWrapped(const Ubiquitty::Terminal* t, int viewOffset, int row);

// Fill `out[0..columns)` with one visible row's cells in VISUAL paint order,
// applying Arabic cursive shaping and reversing strong-RTL runs. The logical
// store is not mutated (copy/paste, selection, search keep the original
// codePoints); only out's base `ch` is rewritten and cells reordered. Gated on
// the row's O(1) needs-shaping flag so all-LTR rows pay only the copy. `out`
// must hold at least `columns` cells.
/// Copies one displayed row into a caller's buffer.
///
/// - Parameters:
///   - t: The terminal to read.
///   - viewOffset: Rows scrolled back from the live tail; 0 is the tail.
///   - row: Zero-based row within the view.
///   - out: Receives the row's cells.
///   - columns: How many cells to copy.
void getDisplayRow(const Ubiquitty::Terminal& t, int viewOffset, int row, Cell* out, int columns);

// Running count of shape passes getDisplayRow has actually run (a shaped-row
// cache hit does not). Tests read it to assert a cached fetch skipped reshaping.
[[nodiscard]] uint64_t shapedRowInvocationCount();

// O(1) read of the visible row's needs-shaping flag. True → the row might
// contain cursive/RTL content and getDisplayRow runs the full pass; false → a
// guaranteed all-LTR row. A superset (set on any mayNeedShaping write, never
// precisely cleared), so a false positive only wastes a pass, never misses.
[[nodiscard]] bool rowMayNeedShaping(const Ubiquitty::Terminal& t, int viewOffset, int row);

// Address the full buffer by absolute row: [0, scrollbackCount) is
// history (0 = oldest), [scrollbackCount, scrollbackCount + rows) is
// the live screen. Out-of-range rows return the default blank cell.
// The selection model stores rows in this space so a selection can
// span scrollback + screen independent of the current viewOffset.
[[nodiscard]] const Cell* absoluteCell(const Ubiquitty::Terminal* t, int absoluteRow, int column);

/// The cells backing one absolute buffer row.
///
/// A row is contiguous on both sides of the history boundary — a `ScrollLine`
/// owns `cells[0, length)` and the grid stores a row as one span — so a caller
/// walking a row column by column can resolve it once instead of re-deriving
/// the history depth and re-resolving the ring slot for every column.
///
/// - Parameters:
///   - t: The terminal to address.
///   - absoluteRow: The row, in the space `absoluteCell` uses.
///   - lengthOut: Receives how many columns are backed by real cells; columns
///     at or past it read as the default blank, as they do through
///     `absoluteCell`.
/// - Returns: The row's first cell, or null when the row is out of range or
///   unbacked, in which case `lengthOut` receives zero.
[[nodiscard]] const Cell* absoluteRowCells(const Ubiquitty::Terminal* t, int absoluteRow, int* lengthOut);

/// An absolute buffer row's cells, shaping flag and line attribute, from one
/// lookup.
///
/// What ``absoluteRowCells`` answers plus whether the row may need a
/// display-time bidi transform and its DEC line attribute, as
/// ``rowLineAttribute`` reports it, so a reader that resolves a row once does
/// not resolve it again for either.
struct AbsoluteRow {
    const Cell* base = nullptr;
    int length = 0;
    bool needsShaping = false;
    uint8_t lineAttribute = lineAttributeNormal;
};

/// Resolves an absolute buffer row once.
///
/// - Parameters:
///   - t: The terminal to address.
///   - absoluteRow: The row, in the space `absoluteCell` uses.
/// - Returns: The row's first cell and backed length, as ``absoluteRowCells``
///   reports them, its shaping flag and its line attribute; an empty, normal
///   row out of range.
[[nodiscard]] AbsoluteRow resolveAbsoluteRow(const Ubiquitty::Terminal* t, int absoluteRow);

// The stored width of an absolute row: every column at or beyond the returned
// count is blank, so a walk looking for a row's last written cell may start
// there instead of at the screen edge.
//
// The contract is one-sided. A count past the row's content is always correct
// and only lengthens a scan; one short of it hides content from the caller. So
// the column count is returned whenever the extent cannot be read in this
// coordinate space — the alternate screen addresses its rows through the view,
// not absolutely.
[[nodiscard]] int absoluteRowExtent(const Ubiquitty::Terminal* t, int absoluteRow);

// OSC 133 prompt-mark bits for an absolute buffer row. 0 for rows out
// of range or on the alternate screen (markers aren't recorded there).
[[nodiscard]] uint8_t rowMark(const Ubiquitty::Terminal* t, int absoluteRow);

// Soft-wrap flag for an absolute buffer row (same addressing as
// absoluteCell): true when the row continues into the next without an
// explicit CR/LF. The absolute-row complement of visibleRowWrapped — the
// selection-to-text policy reads it (selection rows are stored absolute) so a
// reflowed logical line copies back as a single line. false out of range.
[[nodiscard]] bool absoluteRowWrapped(const Ubiquitty::Terminal* t, int absoluteRow);

// Or `bit` into the mark of absolute row `absoluteRow`, whether it is on the live
// screen or already captured into scrollback. No-op when the row is out of
// range. Used at OSC 133;D to flag a failed command's prompt row, which may
// have scrolled into history while the command ran.
void addRowMark(Ubiquitty::Terminal* t, int absoluteRow, uint8_t bit);

// DECDWL/DECDHL line attribute (LINE_ATTR_*) for an absolute buffer
// row, addressed in the same space as rowMark/absoluteCell:
// [0, scrollbackCount) is history (0 = oldest), the remainder is the
// live screen. Read-only; the renderer/snapshot use this so a history
// row carries its OWN attribute instead of the live row at the same index.
// History rows read their captured Ubiquitty::Screen::ScrollLine::lineAttribute; live rows
// forward to the active Screen's lineAttribute.
[[nodiscard]] uint8_t rowLineAttribute(const Ubiquitty::Terminal* t, int absoluteRow);

/// The ``RowFlag`` set of an absolute row, raw.
///
/// A live row answers its metadata. A history row keeps no flags of its own,
/// so ``RowFlag/Flowed`` is answered from its cells — a scan, paid only for
/// the rows a capture copies — and the other flags read as clear: nothing
/// past the live screen shapes or holds a multicell run that a reader of
/// the flags asks about.
///
/// - Parameters:
///   - t: The terminal.
///   - absoluteRow: The row, history first.
/// - Returns: The flags, as ``RowMeta::flags`` holds them.
[[nodiscard]] uint8_t rowFlags(const Ubiquitty::Terminal* t, int absoluteRow);

// Most recent absolute row at or before `absoluteRowExclusive - 1` whose mark
// carries any bit in `markBits`. Returns -1 if none. Cmd-L passes
// osc133MarkCmdExecuted so it targets the previous prompt that actually
// ran a command — skipping the current bare prompt, which has PROMPT_START
// but no CMD_EXECUTED until it runs one.
[[nodiscard]] int findPromptMarkBefore(const Ubiquitty::Terminal* t, int absoluteRowExclusive, uint8_t markBits);

// First absolute row at or after `absoluteRowInclusive` whose mark carries any bit
// in `markBits`. Returns -1 if none. Mirror of findPromptMarkBefore for
// forward (Jump to Next Prompt) navigation.
[[nodiscard]] int findPromptMarkAfter(const Ubiquitty::Terminal* t, int absoluteRowInclusive, uint8_t markBits);

// UTF-8 text of an absolute buffer row (same addressing as absoluteCell): base +
// combining codePoints per cell, wide continuations skipped, the wide sentinel
// rendered as a single space, trailing spaces trimmed. Beam's command-block label
// + find-result line extraction; portable so the non-Apple frontends share it.
[[nodiscard]] std::string absoluteRowText(const Ubiquitty::Terminal& t, int absoluteRow, int columns);

// The executed OSC 133 command whose prompt sits at or above `absoluteRow`,
// resolved through the shared Terminal::resolveCommandRegion rule (marks read via
// rowMark) so the "last command line" span is never forked. `found` is false when
// no CMD_EXECUTED prompt is at/above the row (out of range, alt screen, no shell
// integration). A Swift/interop-clean value: no callback, no Selection type.
struct CommandRegionRows {
    bool found = false;
    int promptRow = -1; // the ; A row (CMD_EXECUTED-marked)
    int outputStart = -1; // the ; C row; == promptRow when no ; C was emitted
    int commandLastRow = -1; // last row of the typed command line
};
[[nodiscard]] CommandRegionRows resolveCommandRegion(const Ubiquitty::Terminal* t, int totalRows, int absoluteRow);

} // namespace Screen
} // namespace Ubiquitty
