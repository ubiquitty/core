// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Selection/TerminalSelection.h"

#include "Core/CellGrid.h"
#include "Core/Modes/Scroll.h"
#include "Core/Mouse/Mouse.h"
#include "Core/OSC/TextSizing.h"
#include "Core/RichText/Blocks.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/SelectionText.h"
#include "Core/Triggers/Triggers.h"
#include "Core/Unicode/Joining.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Ubiquitty::Selection {

namespace {

// The alternate screen keeps no scrollable history, so absolute equals
// visible there; the primary's surviving scrollback would otherwise shift
// every selection row by its count.
[[nodiscard]] int historyRows(const Terminal& terminal) noexcept
{
    if (terminal.screens.isUsingAlternateScreen)
        return 0;
    return Screen::scrollbackCount(&terminal);
}

[[nodiscard]] int totalRows(const Terminal& terminal) noexcept { return historyRows(terminal) + terminal.rows; }

[[nodiscard]] int absoluteRow(const Terminal& terminal, int visibleRow, int viewOffset) noexcept
{
    return Screen::absoluteRowForVisible(historyRows(terminal), viewOffset, visibleRow);
}

// Reads the same screen the renderer paints: the active grid through
// visibleCell on the alternate screen (whose selection rows are screen
// rows), the absolute buffer otherwise. Reading the primary buffer behind
// a tmux alternate screen is the failure this dispatch exists to prevent.
[[nodiscard]] const Cell* screenCell(const Terminal& terminal, int row, int column) noexcept
{
    if (terminal.screens.isUsingAlternateScreen)
        return Screen::visibleCell(&terminal, 0, row, column);
    return Screen::absoluteCell(&terminal, row, column);
}

[[nodiscard]] bool screenRowWrapped(const Terminal& terminal, int row) noexcept
{
    if (terminal.screens.isUsingAlternateScreen)
        return Screen::visibleRowWrapped(&terminal, 0, row);
    return Screen::absoluteRowWrapped(&terminal, row);
}

// The DEC line attribute of a selection row, read through the same dispatch
// as the cells: `Screen::rowLineAttribute` subtracts the scrollback even on
// the alternate screen, whose selection rows are screen rows.
[[nodiscard]] std::uint8_t screenRowLineAttribute(const Terminal& terminal, int row) noexcept
{
    if (terminal.screens.isUsingAlternateScreen) {
        if (!terminal.screens.active || row < 0 || row >= terminal.rows)
            return lineAttributeNormal;
        return terminal.screens.active->lineAttribute[row];
    }
    return Screen::rowLineAttribute(&terminal, row);
}

// How many screen cells one logical column of a row spans: 2 on a DECDWL or
// DECDHL row. A frontend reports the pointer in screen cells, and the gesture
// lands on the logical column under it.
[[nodiscard]] int screenRowWidthScale(const Terminal& terminal, int row) noexcept
{
    return textScaleForLineAttribute(screenRowLineAttribute(terminal, row)).widthScale;
}

// Whether `row` is the bottom half of a double-height pair whose top half is
// the row above it.
[[nodiscard]] bool isLowerHalfOfPair(const Terminal& terminal, int row) noexcept
{
    return row > 0 && screenRowLineAttribute(terminal, row) == lineAttributeDoubleHeightBottom
        && screenRowLineAttribute(terminal, row - 1) == lineAttributeDoubleHeightTop;
}

// The row a selection speaks of for `row`: the top half of a double-height
// pair for either half, since the pair shows one line of glyphs and a
// selection is of the glyphs. Every other row is its own.
[[nodiscard]] int selectionRowOf(const Terminal& terminal, int row) noexcept
{
    return isLowerHalfOfPair(terminal, row) ? row - 1 : row;
}

// `selectionRowOf` for a visible row, back in visible rows. A bottom half
// whose top half is scrolled off the viewport keeps its own row rather than
// naming one the viewport cannot show.
[[nodiscard]] int selectionVisibleRowOf(const Terminal& terminal, int visibleRow, int viewOffset) noexcept
{
    const int row = absoluteRow(terminal, visibleRow, viewOffset);
    const int mapped = selectionRowOf(terminal, row);
    return mapped == row || visibleRow <= 0 ? visibleRow : visibleRow - 1;
}

// The logical column under screen cell `screenColumn` of a visible row, kept
// on the row's drawn columns: a cell past the half a doubled row shows is its
// last visible column, which is the cell nothing draws past.
[[nodiscard]] int logicalColumnUnderScreenCell(const Terminal& terminal, int visibleRow, int viewOffset, int screenColumn) noexcept
{
    const int scale = screenRowWidthScale(terminal, absoluteRow(terminal, visibleRow, viewOffset));
    if (scale <= 1 || terminal.columns <= 0)
        return screenColumn;
    const int lastVisible = std::max(0, terminal.columns / scale - 1);
    if (screenColumn < 0)
        return screenColumn / scale;
    return std::min(screenColumn / scale, lastVisible);
}

struct LogicalLineRun {
    int firstRow;
    int lastRow;
};

[[nodiscard]] LogicalLineRun logicalLineRun(const Terminal& terminal, int rows, int row) noexcept
{
    if (rows <= 0) {
        return { .firstRow = 0, .lastRow = 0 };
    }
    if (row < 0) {
        row = 0;
    }
    if (row >= rows) {
        row = rows - 1;
    }

    LogicalLineRun run { .firstRow = row, .lastRow = row };
    while (run.firstRow > 0 && screenRowWrapped(terminal, run.firstRow - 1)) {
        --run.firstRow;
    }
    while (run.lastRow < rows - 1 && screenRowWrapped(terminal, run.lastRow)) {
        ++run.lastRow;
    }
    return run;
}

// The one context every callback family reads, pointers because the C
// callback shapes carry `void*`. Rows arriving at the cell callbacks are
// absolute buffer rows — the gesture shims convert before delegating.
struct BoundContext {
    Session* session;
    Terminal* terminal;
    int viewOffset;
    GestureStyle style;
    // Where the press is along the row, in cells from the grid's left edge;
    // non-finite when the frontend did not say. A word press reads it to find
    // the letter under the pointer inside a proportional run.
    double pointerXInCells = std::numeric_limits<double>::quiet_NaN();
    // Where the press is down the viewport, in rows; a word press inside a
    // rich text block reads it to find the line under the pointer.
    double pointerYInRows = std::numeric_limits<double>::quiet_NaN();
    // Row memo. The text walk asks for every column of a row before moving on,
    // and screenCell re-derives the history depth and re-resolves the ring slot
    // for each one. A row is contiguous on both sides of the history boundary,
    // so its base and stored length answer every column in it. Valid only for
    // the life of this context, which is one call under the model lock.
    int memoRow = -1;
    const Cell* memoBase = nullptr;
    int memoLength = 0;
    // Whether the memo row may need shaping, which is what puts its columns
    // through `logicalColumns`; false for every row the writer never flagged.
    bool isMemoRowShaped = false;
    // Whether the memo row is the bottom half of a double-height pair under
    // its top half: a property of the row, asked of each of its cells.
    bool isMemoRowLowerHalfOfPair = false;
    std::vector<std::uint32_t> logicalCodePoints {};
    std::vector<std::uint32_t> shapedCodePoints {};
    std::vector<std::uint16_t> logicalColumns {};
    std::vector<std::uint16_t> visualWideAttributes {};
    const SelectionBounds* copyBounds = nullptr;
    SelectionMode copyMode = SelectionMode::Character;
    // Run memo. A copy asks each cell of a proportional run in turn, and which
    // cell each of the run's letters belongs to is one layout through the
    // measurer. Named by the anchor's row and column; valid, like the row memo,
    // for the life of this context.
    int runMemoRow = -1;
    int runMemoColumn = -1;
    int runMemoWidthCells = 1;
    // The run's OSC 66 scale: each of its cells is this many columns, and
    // `runMemoOwners` names a letter's cell by the first column of it.
    int runMemoScale = 1;
    std::uint8_t runMemoAlign = 0;
    Ubiquitty::ProportionalRunStyle runMemoStyle {};
    std::span<const uint32_t> runMemoText {};
    std::vector<std::uint16_t> runMemoOwners {};
    // The columns of the run its text reaches, from the anchor: through the
    // cell the last letter's advance ends in, which is past the cell that
    // letter begins in where its ink runs on. Never short of the last owner's
    // cell.
    int runMemoTextColumns = 1;
    // Scaled-run copy memo: the run whose top band anchors at this row and
    // column, the cell its selected letters are copied at, and those letters.
    int scaledCopyRow = -1;
    int scaledCopyColumn = -1;
    int scaledCopyEmitRow = -1;
    int scaledCopyEmitColumn = -1;
    std::vector<std::uint32_t> scaledCopyText {};
};

// Resolves `row` into the row memo: its cells, and for a row that may need
// shaping, the permutation from visual to logical columns. Out of line, so the
// per-cell read below stays small enough to inline into every callback.
[[gnu::noinline]] void loadRowMemo(BoundContext& bound, int row)
{
    const Terminal& terminal = *bound.terminal;
    bound.memoRow = row;
    bool needsShaping = false;
    std::uint8_t lineAttribute = lineAttributeNormal;
    if (terminal.screens.isUsingAlternateScreen) {
        const auto visible = Screen::resolveVisibleRow(&terminal, 0, row);
        bound.memoBase = visible.base;
        bound.memoLength = visible.length;
        needsShaping = visible.needsShaping;
        lineAttribute = screenRowLineAttribute(terminal, row);
    } else {
        const Screen::AbsoluteRow resolved = Screen::resolveAbsoluteRow(&terminal, row);
        bound.memoBase = resolved.base;
        bound.memoLength = resolved.length;
        needsShaping = resolved.needsShaping;
        lineAttribute = resolved.lineAttribute;
    }
    bound.isMemoRowLowerHalfOfPair = row > 0 && lineAttribute == lineAttributeDoubleHeightBottom
        && screenRowLineAttribute(terminal, row - 1) == lineAttributeDoubleHeightTop;
    bound.isMemoRowShaped = needsShaping;
    if (!needsShaping) {
        bound.logicalColumns.clear();
        bound.visualWideAttributes.clear();
        return;
    }
    const int count = std::min(terminal.columns, maxColumns);
    auto& logical = bound.logicalCodePoints;
    auto& shaped = bound.shapedCodePoints;
    logical.resize(static_cast<std::size_t>(count));
    shaped.resize(static_cast<std::size_t>(count));
    bound.logicalColumns.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        logical[index] = bound.memoBase && index < bound.memoLength ? bound.memoBase[index].codePoint : 0;
    }
    // The renderer's permutation owns visual hit testing; shaping never
    // changes the original code points returned to the clipboard.
    const bool isShaped = Unicode::shapeVisualRow(logical.data(), shaped.data(), bound.logicalColumns.data(), count);
    bound.visualWideAttributes.clear();
    if (isShaped) {
        bound.visualWideAttributes.resize(static_cast<std::size_t>(count));
        for (int index = 0; index + 1 < count; ++index) {
            if (shaped[index] >= 0xFEF5 && shaped[index] <= 0xFEFC && !shaped[index + 1]) {
                bound.visualWideAttributes[index] = Ubiquitty::VT::AttributeSet { Ubiquitty::VT::Attribute::Wide }.toRaw();
                bound.visualWideAttributes[index + 1] = Ubiquitty::VT::AttributeSet { Ubiquitty::VT::Attribute::WideContinuation }.toRaw();
            }
        }
    }
    if (isShaped && bound.copyBounds) {
        const auto& bounds = *bound.copyBounds;
        const int first = bound.copyMode == SelectionMode::Rectangle || row == bounds.row0 ? bounds.column0 : 0;
        const int last = bound.copyMode == SelectionMode::Rectangle || row == bounds.row1 ? bounds.column1 : count - 1;
        // Copy the visually selected set in source order, including a
        // full line containing several differently directed runs.
        if (first >= 0 && last >= first && last < count) {
            std::sort(bound.logicalColumns.begin() + first, bound.logicalColumns.begin() + last + 1);
        }
    }
}

// One cell, resolving the row once per row rather than once per column. The
// alternate screen goes the long way: its rows are addressed through the view,
// which is not this memo's space. A row that cannot need shaping reads its
// column directly; only a shaped row maps through the permutation.
[[nodiscard]] inline const Cell* boundCell(BoundContext& bound, int row, int column) noexcept
{
    if (column < 0 || column >= bound.terminal->columns)
        return &defaultBlankCell;
    if (row != bound.memoRow) [[unlikely]]
        loadRowMemo(bound, row);
    int logicalColumn = column;
    if (bound.isMemoRowShaped && column < static_cast<int>(bound.logicalColumns.size())) [[unlikely]]
        logicalColumn = bound.logicalColumns[column];
    return logicalColumn < bound.memoLength ? &bound.memoBase[logicalColumn] : &defaultBlankCell;
}

// Loads the memo for the proportional run covering one cell, and answers
// whether a run covers it at all. The memo holds the anchor's column, the
// claim, the alignment, the run's text and the cell each code point's ink
// begins in — every fact a reader of the run needs, derived once. With no
// measurer, or an owner map that is not in order, the anchor owns the whole
// run.
[[nodiscard]] bool loadRunMemo(BoundContext& bound, int row, int column, const Cell& cell)
{
    const Terminal* terminal = bound.terminal;
    int anchorColumn = column;
    if (isProportionalRunContinuation(terminal, cell)) {
        while (anchorColumn > 0) {
            --anchorColumn;
            const Cell* left = boundCell(bound, row, anchorColumn);
            if (!left || !cellPen(*left).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                break;
        }
    } else if (!isProportionalRunAnchor(terminal, cell)) {
        return false;
    }
    const Cell* anchor = anchorColumn == column ? &cell : boundCell(bound, row, anchorColumn);
    if (!anchor || !isProportionalRunAnchor(terminal, *anchor))
        return false;
    if (row != bound.runMemoRow || anchorColumn != bound.runMemoColumn) {
        bound.runMemoRow = row;
        bound.runMemoColumn = anchorColumn;
        bound.runMemoText = runTextCodePoints(terminal, *anchor);
        bound.runMemoOwners.assign(bound.runMemoText.size(), std::uint16_t { 0 });
        const TextSizeSpec* spec = terminal->textSizeTable.lookup(cellPen(*anchor).extendedAttributes.textSizeSpecID());
        bound.runMemoWidthCells = spec && spec->runWidthCells > 0 ? spec->runWidthCells : 1;
        bound.runMemoScale = spec ? std::max<int>(spec->scale, 1) : 1;
        bound.runMemoAlign = spec ? spec->horizontalAlign : std::uint8_t { 0 };
        bound.runMemoStyle = OSC::TextSizing::runStyleOf(cellPen(*anchor));
        int textCells = 1;
        const bool hasOwners = spec && !bound.runMemoText.empty()
            && OSC::TextSizing::proportionalRunCellOwners(
                *bound.terminal, bound.runMemoText, bound.runMemoWidthCells, bound.runMemoAlign, bound.runMemoStyle, bound.runMemoOwners, &textCells)
            && std::ranges::is_sorted(bound.runMemoOwners);
        if (!hasOwners) {
            std::ranges::fill(bound.runMemoOwners, std::uint16_t { 0 });
            textCells = 1;
        }
        // A scaled run's letters are laid out in its own cells, each `scale`
        // columns of the row: a letter's cell is named by its first column, and
        // the columns after it inside the cell own nothing, as a wide glyph's
        // second half does.
        for (std::uint16_t& owner : bound.runMemoOwners)
            owner = static_cast<std::uint16_t>(owner * bound.runMemoScale);
        const int lastOwner = bound.runMemoOwners.empty() ? 0 : static_cast<int>(bound.runMemoOwners.back());
        bound.runMemoTextColumns = std::max(textCells * bound.runMemoScale, lastOwner + bound.runMemoScale);
    }
    return true;
}

// The letters one cell of a proportional run owns: those whose ink begins in
// it, by the layout placement uses, read from the anchor's text. Answers
// whether the cell belongs to a run at all; an empty span on a run cell is a
// cell no letter begins in.
//
// Every run cell, anchor or continuation, carries a text-size spec id, so a
// cell without one answers here, inline, before any call: the copy walk asks
// this of every cell it reads.
[[nodiscard]] bool runCellTextSlow(BoundContext&, int row, int column, const Cell&, std::span<const uint32_t>& owned);

[[nodiscard]] inline bool runCellText(BoundContext& bound, int row, int column, const Cell& cell, std::span<const uint32_t>& owned)
{
    owned = {};
    if (cell.pen.extendedAttributes.textSizeSpecID() == 0) [[likely]]
        return false;
    return runCellTextSlow(bound, row, column, cell, owned);
}

[[gnu::noinline]] bool runCellTextSlow(BoundContext& bound, int row, int column, const Cell& cell, std::span<const uint32_t>& owned)
{
    if (!loadRunMemo(bound, row, column, cell))
        return false;
    const std::uint16_t cellIndex = static_cast<std::uint16_t>(column - bound.runMemoColumn);
    const std::vector<std::uint16_t>& owners = bound.runMemoOwners;
    const auto first = std::ranges::lower_bound(owners, cellIndex);
    const auto last = std::ranges::upper_bound(first, owners.end(), cellIndex);
    owned = bound.runMemoText.subspan(static_cast<std::size_t>(first - owners.begin()), static_cast<std::size_t>(last - first));
    return true;
}

// The carets a point along a row falls on inside the proportional run under
// it: the boundaries before and after the letter the point is on, each as the
// cell whose letters it cuts and how many of that cell's letters stand to its
// left. Past the run's text both are the one boundary after its last letter.
// Answers false where the point is not inside a run, which leaves the
// boundary the frontend resolved standing.
//
// The run's own layout decides, so the letter a reader points at and the cell
// that letter is copied from are one answer: both come from the memo the copy
// walk already builds. The carets then descend into the coordinates a
// selection already speaks — a cell, and code points into that cell's text —
// so nothing downstream of a bound learns a new position space.
[[nodiscard]] bool runCaretsAtPoint(BoundContext& bound, int row, double xInCells, int& caret, int& subCaret, int& caretAfter, int& subCaretAfter)
{
    const Terminal& terminal = *bound.terminal;
    if (terminal.columns <= 0 || !std::isfinite(xInCells))
        return false;
    const int column = std::clamp(static_cast<int>(std::floor(xInCells)), 0, terminal.columns - 1);
    const Cell* pointed = boundCell(bound, row, column);
    if (!pointed || !loadRunMemo(bound, row, column, *pointed))
        return false;
    const int count = static_cast<int>(bound.runMemoText.size());
    const std::vector<std::uint16_t>& owners = bound.runMemoOwners;
    if (count <= 0 || static_cast<int>(owners.size()) != count)
        return false;
    const int anchorColumn = bound.runMemoColumn;
    const int widthCells = bound.runMemoWidthCells;
    const int scale = bound.runMemoScale;
    const double xInRun = std::clamp((xInCells - anchorColumn) / scale, 0.0, static_cast<double>(widthCells));
    // The owners are the cell each letter begins in, in text order, so the
    // letters one cell owns are a contiguous stretch of them: the boundary
    // before a letter is its cell and its distance from where that cell's
    // stretch starts; the boundary after the text is the one after the last
    // cell a letter begins in.
    const auto boundaryBefore = [&](int letter, int& cell, int& offset) {
        if (letter >= count) {
            cell = std::min(anchorColumn + bound.runMemoTextColumns, terminal.columns);
            offset = 0;
            return;
        }
        const std::uint16_t owner = owners[static_cast<std::size_t>(letter)];
        const auto stretch = std::ranges::lower_bound(owners, owner);
        cell = std::clamp(anchorColumn + static_cast<int>(owner), 0, terminal.columns);
        offset = letter - static_cast<int>(stretch - owners.begin());
    };
    int first = 0;
    int end = 0;
    if (OSC::TextSizing::proportionalRunLetterUnder(*bound.terminal, bound.runMemoText, widthCells, bound.runMemoAlign, bound.runMemoStyle, xInRun, first, end)) {
        boundaryBefore(first, caret, subCaret);
        boundaryBefore(end, caretAfter, subCaretAfter);
        return true;
    }
    const int index = OSC::TextSizing::proportionalRunCodePointUnder(
        *bound.terminal, bound.runMemoText, widthCells, bound.runMemoAlign, bound.runMemoStyle, xInRun);
    if (index < 0)
        return false;
    // Past the text, in the claim's unused tail: the cell boundary under the
    // point, no nearer the text than the boundary after its last letter. The
    // tail is the row's void, and the void rule reads how far past the text
    // the pointer went — clamping the selection to the last letter until the
    // pointer is far enough out to take the rest of the line — which it can
    // only do from a caret that moved with the pointer.
    int afterText = 0;
    int afterTextOffset = 0;
    boundaryBefore(count, afterText, afterTextOffset);
    caret = std::clamp(static_cast<int>(std::floor(xInCells + 0.5)), afterText, terminal.columns);
    subCaret = 0;
    caretAfter = caret;
    subCaretAfter = subCaret;
    return true;
}

// Whether a column of the memo's run, counted from its anchor, stands past
// the run's text: after the last cell a letter's ink reaches, `scale`
// columns of a scaled run, which is the row's void.
[[nodiscard]] bool isPastRunText(const BoundContext& bound, int cellIndex)
{
    return !bound.runMemoOwners.empty() && cellIndex >= bound.runMemoTextColumns;
}

uint32_t cellAt(void* context, int row, int column)
{
    auto& bound = *static_cast<BoundContext*>(context);
    const Cell* cell = boundCell(bound, row, column);
    if (!cell)
        return 0;
    // A proportional run's cell answers with the first letter whose ink begins
    // in it. An empty one inside the text, which no glyph begins in, is the
    // tail of the letter before it rather than a blank: a word boundary there
    // would cut "abcdef" where its third letter happened to overhang, so it
    // answers as the nearest cell to its left that owns a letter. An empty
    // one after the text — the claim's unused tail — is empty, as a cell
    // nothing printed in is: it is the row's void, where a drag past the last
    // letter stops at the letter until it has gone far enough to take the
    // rest of the line.
    std::span<const uint32_t> owned;
    if (runCellText(bound, row, column, *cell, owned)) {
        const std::vector<std::uint16_t>& owners = bound.runMemoOwners;
        const int cellIndex = column - bound.runMemoColumn;
        if (owned.empty() && !owners.empty() && (isPastRunText(bound, cellIndex) || cellIndex < static_cast<int>(owners.front())))
            return 0;
        for (int left = column - 1; left >= 0 && owned.empty(); --left) {
            const Cell* before = boundCell(bound, row, left);
            if (!before || !runCellText(bound, row, left, *before, owned))
                break;
        }
        return owned.empty() ? cell->codePoint : owned.front();
    }
    // A sized glyph's continuation cells are the glyph: a word walk crossing
    // them reads the letter they show, so a scaled word is one word rather
    // than a letter cut at every second cell. A proportional run's
    // continuations answered above.
    const Pen& pen = cellPen(*cell);
    if (pen.extendedAttributes.textSizeSpecID() != 0 && pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
        for (int left = column - 1; left >= 0; --left) {
            const Cell* before = boundCell(bound, row, left);
            if (!before)
                break;
            if (!cellPen(*before).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                return before->codePoint;
        }
    }
    return cell->codePoint;
}

// Pen attributes at an absolute cell, which is what lets bounds resolution
// recognise the trailing half of a wide glyph: an endpoint dropped on that
// half otherwise resolves one column past the only cell carrying the
// character, and the character is absent from the copy rather than clipped.
uint16_t attributesAt(void* context, int row, int column)
{
    auto& bound = *static_cast<BoundContext*>(context);
    const Cell* cell = boundCell(bound, row, column);
    if (!cell)
        return 0;
    // A proportional run's continuation that owns letters, whose ink begins
    // in it, is a cell of its own to a selection: the endpoints snap through
    // a wide pair's empty half, and through the empty cells inside a run,
    // but stop at a cell that holds letters — so a drag inside a run takes
    // the words under it, by the cells their ink begins in, rather than the
    // whole run. The claim's cells outside the text — the slack before an
    // aligned text, the unused tail after — are cells of their own too, and
    // blank: not a glyph's second half for an endpoint to run through.
    Ubiquitty::VT::AttributeSet attributes = cellPen(*cell).attributes;
    std::span<const uint32_t> owned;
    if (attributes.contains(Ubiquitty::VT::Attribute::WideContinuation) && runCellText(bound, row, column, *cell, owned)) {
        const std::vector<std::uint16_t>& owners = bound.runMemoOwners;
        const int cellIndex = column - bound.runMemoColumn;
        const bool isOutsideText = owned.empty() && !owners.empty()
            && (isPastRunText(bound, cellIndex) || cellIndex < static_cast<int>(owners.front()));
        if (!owned.empty() || isOutsideText)
            attributes.remove(Ubiquitty::VT::Attribute::WideContinuation);
    }
    const auto shapedAttributes = column >= 0 && column < static_cast<int>(bound.visualWideAttributes.size()) ? bound.visualWideAttributes[column] : 0;
    return attributes.toRaw() | shapedAttributes;
}

// One scaled proportional run, as a selection reads it: where it stands, and
// which of its letters a selection holds.
struct ScaledRunLetters {
    int topRow = 0;
    int anchorColumn = 0;
    int scale = 1;
    int claimColumns = 1;
    std::span<const uint32_t> text {};
    std::vector<std::uint16_t> owners {};
    // The columns of the run its text reaches, from the anchor: the last
    // letters' columns run through them.
    int textColumns = 1;
    std::vector<std::uint8_t> isSelected {};
    // The first band row that holds a selected letter, and where the
    // selection enters the run on it; -1 when no letter is selected.
    int firstSelectingRow = -1;
    int firstSelectingColumn = -1;
    // Whether some band row's span reaches the claim at all, and its cells no
    // letter's ink begins in: before an aligned text, and after the last
    // letter's cell.
    bool isTouched = false;
    bool isSlackHeld = false;
    bool isTailHeld = false;
};

// Where the scaled proportional run a cell belongs to stands: its top band's
// row and its anchor's column. False for any other cell.
[[nodiscard]] bool scaledRunOrigin(BoundContext& bound, int row, int column, const Cell& cell, int& topRow, int& anchorColumn)
{
    const Terminal& terminal = *bound.terminal;
    const unsigned specID = cellPen(cell).extendedAttributes.textSizeSpecID();
    const TextSizeSpec* spec = specID != 0 ? terminal.textSizeTable.lookup(specID) : nullptr;
    if (!spec || !spec->isProportional || spec->scale < 2)
        return false;
    anchorColumn = column;
    while (anchorColumn > 0) {
        const Cell* here = boundCell(bound, row, anchorColumn);
        if (!here || !cellPen(*here).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
            break;
        --anchorColumn;
    }
    const Cell* anchor = boundCell(bound, row, anchorColumn);
    if (!anchor || cellPen(*anchor).extendedAttributes.textSizeSpecID() != specID || cellPen(*anchor).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        return false;
    topRow = row - static_cast<int>(penTextBand(&cellPen(*anchor)));
    return true;
}

// The columns of a scaled run a letter answers for: from the first column of
// the cell its ink begins in to the column before the next cell a letter
// begins in, and for the letters of the last such cell through the last cell
// the text's advance reaches, where the last letter's ink runs on past the
// cell it begins in. The cells inside the text no ink begins in are the
// letter's before them, as an empty cell inside a run of one row reads as the
// letter before it; the cells before an aligned text and after the text are
// no letter's, the row's own cells, as kitty has no cell past a character for
// it.
[[nodiscard]] std::pair<int, int> scaledRunLetterColumns(const ScaledRunLetters& letters, int letter)
{
    const std::uint16_t owner = letters.owners[static_cast<std::size_t>(letter)];
    const auto next = std::ranges::upper_bound(letters.owners, owner);
    const int nextOwner = next == letters.owners.end() ? letters.textColumns : static_cast<int>(*next);
    const int first = letters.anchorColumn + static_cast<int>(owner);
    return { first, letters.anchorColumn + std::max(nextOwner, static_cast<int>(owner) + letters.scale) - 1 };
}

// The letters of the scaled run whose top band anchors at (topRow,
// anchorColumn) that `bounds` holds on any of its band rows. A scaled run's
// letter is a glyph `scale` rows tall over its columns
// (``scaledRunLetterColumns``), so, as kitty selects a sized character, a
// selection holding any of those columns on any of those rows holds the
// letter; a caret inside a packed cell, at the selection's own ends, cuts
// that cell's letters as it cuts a run of one row. Each band row is judged by its own span, so the
// letters held are a set and not a range: a selection entering the run on its
// top band and leaving it on a lower one holds the letters after its start
// and the letters before its end.
[[nodiscard]] bool scaledRunLettersAt(BoundContext& bound, const SelectionBounds& bounds, SelectionMode mode, int topRow, int anchorColumn, ScaledRunLetters& out)
{
    const Terminal& terminal = *bound.terminal;
    const Cell* anchor = boundCell(bound, topRow, anchorColumn);
    if (!anchor || !loadRunMemo(bound, topRow, anchorColumn, *anchor) || bound.runMemoScale < 2)
        return false;
    out.topRow = topRow;
    out.anchorColumn = anchorColumn;
    out.scale = bound.runMemoScale;
    out.claimColumns = bound.runMemoWidthCells * bound.runMemoScale;
    out.text = bound.runMemoText;
    out.owners = bound.runMemoOwners;
    out.textColumns = bound.runMemoTextColumns;
    const int count = static_cast<int>(out.text.size());
    out.isSelected.assign(static_cast<std::size_t>(count), 0);
    out.firstSelectingRow = -1;
    out.firstSelectingColumn = -1;
    if (count == 0 || static_cast<int>(out.owners.size()) != count)
        return false;
    const unsigned specID = cellPen(*anchor).extendedAttributes.textSizeSpecID();
    const int lastClaimed = anchorColumn + out.claimColumns - 1;
    const auto stretchStart = [&](int letter) {
        return static_cast<int>(std::ranges::lower_bound(out.owners, out.owners[static_cast<std::size_t>(letter)]) - out.owners.begin());
    };
    for (int band = 0; band < out.scale; ++band) {
        const int row = topRow + band;
        if (row < bounds.row0 || row > bounds.row1)
            continue;
        // A band row that no longer stacks under the top band holds none of
        // the run's letters.
        const Cell* bandAnchor = boundCell(bound, row, anchorColumn);
        if (!bandAnchor || cellPen(*bandAnchor).extendedAttributes.textSizeSpecID() != specID
            || static_cast<int>(penTextBand(&cellPen(*bandAnchor))) != band)
            break;
        const RowSpan span = rowSpan(&bounds, mode, row, terminal.columns, cellAt, &bound, attributesAt, &bound);
        if (!span.selected || span.column1 < anchorColumn || span.column0 > lastClaimed)
            continue;
        out.isTouched = true;
        const int textFirst = anchorColumn + out.owners.front();
        const int textLast = anchorColumn + out.textColumns - 1;
        out.isSlackHeld = out.isSlackHeld || (textFirst > anchorColumn && span.column0 < textFirst);
        out.isTailHeld = out.isTailHeld || (textLast < lastClaimed && span.column1 > textLast);
        const bool isCharacter = mode == SelectionMode::Character;
        const bool isStartCut = isCharacter && bounds.subColumn0 > 0 && row == bounds.row0 && span.column0 == bounds.column0;
        const bool isEndCut = isCharacter && bounds.subColumn1 > 0 && row == bounds.row1 && span.column1 == bounds.column1;
        bool isRowSelecting = false;
        for (int letter = 0; letter < count; ++letter) {
            const auto [cellFirst, cellLast] = scaledRunLetterColumns(out, letter);
            if (cellLast < span.column0 || cellFirst > span.column1)
                continue;
            const int intoCell = letter - stretchStart(letter);
            if (isStartCut && cellFirst == bounds.column0 && intoCell < bounds.subColumn0)
                continue;
            if (isEndCut && cellFirst == bounds.column1 && intoCell >= bounds.subColumn1)
                continue;
            out.isSelected[static_cast<std::size_t>(letter)] = 1;
            isRowSelecting = true;
        }
        if (isRowSelecting && out.firstSelectingRow < 0) {
            out.firstSelectingRow = row;
            out.firstSelectingColumn = std::max(anchorColumn, span.column0);
        }
    }
    return true;
}

// What a scaled run's cell contributes to a copy: every letter the selection
// holds, once and in the text's order, at the first cell the copy reaches on
// the first band row that holds one, and nothing at the run's other cells.
// Answers false for a cell of no scaled run, or a walk with no copy bounds.
[[nodiscard]] bool scaledRunCopyText(BoundContext& bound, int row, int column, const Cell& cell, std::span<const uint32_t>& text, bool& isContinuation)
{
    if (!bound.copyBounds)
        return false;
    int topRow = 0;
    int anchorColumn = 0;
    if (!scaledRunOrigin(bound, row, column, cell, topRow, anchorColumn))
        return false;
    if (topRow != bound.scaledCopyRow || anchorColumn != bound.scaledCopyColumn) {
        bound.scaledCopyRow = topRow;
        bound.scaledCopyColumn = anchorColumn;
        bound.scaledCopyEmitRow = -1;
        bound.scaledCopyEmitColumn = -1;
        bound.scaledCopyText.clear();
        ScaledRunLetters letters;
        if (scaledRunLettersAt(bound, *bound.copyBounds, bound.copyMode, topRow, anchorColumn, letters)) {
            bound.scaledCopyEmitRow = letters.firstSelectingRow;
            bound.scaledCopyEmitColumn = letters.firstSelectingColumn;
            for (std::size_t letter = 0; letter < letters.text.size(); ++letter) {
                if (letters.isSelected[letter])
                    bound.scaledCopyText.push_back(letters.text[letter]);
            }
        }
    }
    const bool isEmitted = row == bound.scaledCopyEmitRow && column == bound.scaledCopyEmitColumn;
    text = isEmitted ? std::span<const uint32_t>(bound.scaledCopyText) : std::span<const uint32_t> {};
    isContinuation = !isEmitted;
    return true;
}

// Whether the copy takes the text at (row, column) from the cell above it:
// the cell above is inside the copy's bounds and shows the same text. Two
// shapes repeat a row's text on the row below — a double-height pair's bottom
// half and a sized block's lower bands — and a reader of either sees one line.
// So the lower cell contributes text only when the upper one is outside the
// copy, where it is the only one of the pair the copy holds.
//
// How much of the row above a repeated cell answers for. A sized block shares
// its row with other cells, so the question is asked of the one column. A
// double-height pair's two rows are one line whole: once the top half is
// inside the copy at all, the bottom half repeats it — a selection of the
// pair is expressed on the top row, and the bottom row's own span, where the
// bounds run through it, says nothing about the glyphs.
enum class RepeatedExtent : std::uint8_t {
    Column,
    WholeRow,
};

[[nodiscard]] bool isTextTakenFromCellAbove(BoundContext& bound, int row, int column, RepeatedExtent extent)
{
    if (!bound.copyBounds || row <= bound.copyBounds->row0)
        return false;
    const RowSpan above = rowSpan(bound.copyBounds, bound.copyMode, row - 1, bound.terminal->columns, cellAt, &bound, attributesAt, &bound);
    if (!above.selected)
        return false;
    return extent == RepeatedExtent::WholeRow || (column >= above.column0 && column <= above.column1);
}

// Whether the copy walk skips a cell that is not a proportional run's. A wide
// run's continuation always; a sized block's lower band and a double-height
// bottom half only under the cell above them when that cell is in the copy.
[[nodiscard]] bool isCopyContinuation(BoundContext& bound, int row, int column, const Cell& cell)
{
    const Pen& pen = cellPen(cell);
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        return true;
    const bool isLowerBand = pen.extendedAttributes.textSizeSpecID() != 0 && penTextBand(&pen) != 0;
    if (isLowerBand)
        return isTextTakenFromCellAbove(bound, row, column, RepeatedExtent::Column);
    if (row != bound.memoRow)
        loadRowMemo(bound, row);
    if (bound.isMemoRowLowerHalfOfPair)
        return isTextTakenFromCellAbove(bound, row, column, RepeatedExtent::WholeRow);
    return false;
}

// What a cell a rich text block covers contributes to a copy: the block's
// copyable text at its top-left cell, where the reading order reads it whole,
// and nothing at its other cells.
struct BlockCellText {
    std::span<const uint32_t> text;
    bool isContinuation = false; // bool-ok: TextCell's own field, carried to it
};

// Selection carets retain document offsets even when the local renderer wraps
// the source anchor onto a later line. Grid selections enter at the first
// visible line, while character selections keep their document-relative cut.
[[nodiscard]] PackedCellCut visibleBlockCut(const Terminal& terminal, const RichText::Block& block, PackedCellCut cut)
{
    const RichText::Layout& layout = RichText::layoutOf(terminal, block);
    const int first = layout.firstVisibleLine() < layout.visibleLineCount() ? static_cast<int>(layout.line(layout.firstVisibleLine()).first) : static_cast<int>(layout.copyableEnd());
    cut.first = std::min(std::max(cut.first, first), cut.end);
    return cut;
}

// Empty for a cell no block covers.
[[nodiscard]] std::optional<BlockCellText> blockCellText(BoundContext& bound, int row, int column, const Cell& cell)
{
    if (!penRichTextCovered(&cellPen(cell)))
        return std::nullopt;
    const Terminal& terminal = *bound.terminal;
    const int screenRow = row - historyRows(terminal);
    const RichText::Block* block = RichText::blockCovering(terminal, RichText::activeScreen(terminal), screenRow, column);
    if (!block)
        return std::nullopt;
    if (screenRow != block->row || column != block->column)
        return BlockCellText { .text = {}, .isContinuation = true };
    PackedCellCut cut { 0, static_cast<int>(RichText::copyableLength(terminal, *block)) };
    if (bound.copyBounds) {
        const RowSpan span = rowSpan(bound.copyBounds, bound.copyMode, row, terminal.columns, cellAt, &bound, attributesAt, &bound);
        cut = packedCellCut(bound.copyBounds, bound.copyMode, span, row, column, cut.end);
    }
    cut = visibleBlockCut(terminal, *block, cut);
    return BlockCellText { .text = std::span<const uint32_t>(block->document.text).subspan(static_cast<std::size_t>(cut.first), static_cast<std::size_t>(cut.end - cut.first)), .isContinuation = cut.first == cut.end };
}

// The general text read: any row, any cell. Out of line so the plain-cell read
// in textCellAt makes no call and saves no registers.
[[gnu::noinline]] TextCell textCellAtAnyCell(BoundContext& bound, int row, int column)
{
    const Cell* cell = boundCell(bound, row, column);
    // Field-wise rather than aggregate initialisation: a braced initialiser
    // value-initialises every member it does not name, which puts the 64-byte
    // codePoints array back whatever its declaration says.
    TextCell text;
    text.codePoint = cell ? cell->codePoint : 0;
    // A proportional run's cell carries the letters whose ink begins in it,
    // from the anchor's text, on the span rather than the fixed array; a run
    // cell that owns none reads as a continuation.
    std::span<const uint32_t> owned;
    if (const std::optional<BlockCellText> block = cell ? blockCellText(bound, row, column, *cell) : std::nullopt) {
        text.isContinuation = block->isContinuation;
        text.runCodePoints = block->text;
        text.codePointCount = 0;
        text.isUncut = true;
        return text;
    }
    bool isScaledContinuation = false;
    if (cell && scaledRunCopyText(bound, row, column, *cell, owned, isScaledContinuation)) {
        text.isContinuation = isScaledContinuation;
        text.runCodePoints = owned;
        text.isUncut = true;
        text.codePointCount = 0;
        return text;
    }
    if (cell && runCellText(bound, row, column, *cell, owned)) {
        text.isContinuation = owned.empty();
        text.runCodePoints = owned;
        text.codePointCount = 0;
        return text;
    }
    text.isContinuation = cell && isCopyContinuation(bound, row, column, *cell);
    if (cell && isRunTextAnchor(bound.terminal, *cell)) {
        text.runCodePoints = text.isContinuation ? std::span<const uint32_t> {} : runTextCodePoints(bound.terminal, *cell);
        text.codePointCount = 0;
        return text;
    }
    text.codePointCount = cell ? cellClusterCodePoints(bound.terminal, cell, text.codePoints.data(), static_cast<int>(text.codePoints.size())) : 0;
    return text;
}

// A cell of the memo row, on a row with no permutation and no double-height
// pairing, with no text-size spec, no cluster id and no rich text block over
// it, is answered from the cell alone: it belongs to no run or block, only a
// wide continuation repeats a neighbour, and its cluster is its own storage.
// Everything else takes the general read.
TextCell textCellAt(void* context, int row, int column)
{
    auto& bound = *static_cast<BoundContext*>(context);
    if (row == bound.memoRow && !bound.isMemoRowShaped && !bound.isMemoRowLowerHalfOfPair
        && column >= 0 && column < bound.terminal->columns) [[likely]] {
        const Cell& cell = column < bound.memoLength ? bound.memoBase[column] : defaultBlankCell;
        if (cell.pen.extendedAttributes.textSizeSpecID() == 0 && cell.clusterID() == 0 && !penRichTextCovered(&cell.pen)) [[likely]] {
            TextCell text;
            text.codePoint = cell.codePoint;
            text.isContinuation = cell.pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
            text.codePointCount = cellInlineClusterCodePoints(cell, text.codePoints.data(), static_cast<int>(text.codePoints.size()));
            return text;
        }
    }
    return textCellAtAnyCell(bound, row, column);
}

// The plain walk's context beside the palette a styled copy resolves each
// cell through, so the two walks read the same rows the same way.
struct StyledBound {
    BoundContext* bound;
    const CellPalette* palette;
};

// One cell as the styled generator sees it: bold-as-bright, reverse, dim and
// hidden are baked by the palette exactly as on screen, so the RTF and HTML
// carry what the reader saw rather than the pen's raw indices.
StyledCell styledCellAt(void* context, int row, int column)
{
    auto& styled = *static_cast<StyledBound*>(context);
    const Cell* cell = boundCell(*styled.bound, row, column);
    if (!cell)
        return StyledCell {};
    const Pen& pen = cellPen(*cell);
    StyledCell out {};
    out.codePoint = cell->codePoint;
    std::span<const uint32_t> owned;
    const std::optional<BlockCellText> block = blockCellText(*styled.bound, row, column, *cell);
    if (block)
        owned = block->text;
    bool isScaledContinuation = false;
    const bool isScaledRunCell = !block && scaledRunCopyText(*styled.bound, row, column, *cell, owned, isScaledContinuation);
    const bool isRunCell = block || isScaledRunCell || runCellText(*styled.bound, row, column, *cell, owned);
    out.isContinuation = block ? block->isContinuation
        : isScaledRunCell      ? isScaledContinuation
        : isRunCell            ? owned.empty()
                               : isCopyContinuation(*styled.bound, row, column, *cell);
    out.isUncut = block.has_value() || isScaledRunCell;
    styled.palette->resolve(pen, ReverseMode::Normal, out.foreground, out.background);
    out.bold = pen.attributes.contains(Ubiquitty::VT::Attribute::Bold);
    out.italic = pen.attributes.contains(Ubiquitty::VT::Attribute::Italic);
    out.underline = pen.attributes.contains(Ubiquitty::VT::Attribute::Underline);
    if (!isRunCell && isRunTextAnchor(styled.bound->terminal, *cell))
        out.runCodePoints = out.isContinuation ? std::span<const uint32_t> {} : runTextCodePoints(styled.bound->terminal, *cell);
    else if (isRunCell)
        out.runCodePoints = owned;
    else
        out.codePointCount = cellClusterCodePoints(styled.bound->terminal, cell, out.codePoints.data(), static_cast<int>(out.codePoints.size()));
    return out;
}

// The stored width of an absolute row, for the trailing-blank scan.
int rowExtentAt(void* context, int row)
{
    const auto& bound = *static_cast<const BoundContext*>(context);
    return Screen::absoluteRowExtent(bound.terminal, row);
}

int rowWrappedAt(void* context, int row)
{
    const auto& bound = *static_cast<const BoundContext*>(context);
    return screenRowWrapped(*bound.terminal, row) ? 1 : 0;
}

std::uint8_t rowLineAttributeAt(void* context, int row)
{
    const auto& bound = *static_cast<const BoundContext*>(context);
    return screenRowLineAttribute(*bound.terminal, row);
}

void boundBeginCharacter(void* userData, int /*visibleRow*/, int column)
{
    auto& bound = *static_cast<BoundContext*>(userData);
    Terminal& terminal = *bound.terminal;
    begin(&bound.session->state, totalRows(terminal), terminal.columns,
        bound.session->gestureAnchorAbsoluteRow, column,
        bound.style.isRectangular ? SelectionMode::Rectangle : SelectionMode::Character);
}

// The word a press inside a proportional run grows to, by the run's letters
// rather than its cells. A cell of a run holds several letters, of more than
// one kind — the space before a word and its first letter begin in one cell as
// often as not — and a walk reading a cell as its first letter starts the word
// a letter late. The letter under the pointer is the layout's answer; the word
// is the contiguous stretch of letters of its kind around it, by the same
// classes the cell walk uses, carried past the run's edges into the plain
// cells beside it where the text runs on without a break; and the span is set
// by carets — a cell and a cut into its letters — so the copy and the fill take
// exactly those letters. Answers false where the press is not on a letter of
// a shaped run, which leaves the cell walk to it.
[[nodiscard]] bool beginRunWord(BoundContext& bound, int row, int column)
{
    const Terminal& terminal = *bound.terminal;
    if (!std::isfinite(bound.pointerXInCells) || terminal.columns <= 0)
        return false;
    const Cell* pointed = boundCell(bound, row, column);
    if (!pointed || !loadRunMemo(bound, row, column, *pointed))
        return false;
    const std::span<const uint32_t> text = bound.runMemoText;
    const std::vector<std::uint16_t>& owners = bound.runMemoOwners;
    const int count = static_cast<int>(text.size());
    if (count <= 0 || static_cast<int>(owners.size()) != count)
        return false;
    const int anchorColumn = bound.runMemoColumn;
    const int widthCells = bound.runMemoWidthCells;
    const int scale = bound.runMemoScale;
    const double xInRun = std::clamp((bound.pointerXInCells - anchorColumn) / scale, 0.0, static_cast<double>(widthCells));
    const int under = OSC::TextSizing::proportionalRunCodePointUnder(*bound.terminal, text, widthCells, bound.runMemoAlign, bound.runMemoStyle, xInRun);
    if (under < 0 || under >= count)
        return false;
    const Selection::Engine* engine = &bound.session->engine;
    const bool isWord = Selection::engineIsWordByte(engine, text[static_cast<std::size_t>(under)]);
    const bool isSpace = !isWord && text[static_cast<std::size_t>(under)] == ' ';
    // A letter of neither kind — a punctuation mark — is a word of one letter,
    // as the cell walk has it.
    const auto isSameKind = [&](uint32_t codePoint) {
        return isWord ? Selection::engineIsWordByte(engine, codePoint) : (isSpace && codePoint == ' ');
    };
    int first = under;
    int last = under;
    if (isWord || isSpace) {
        while (first > 0 && isSameKind(text[static_cast<std::size_t>(first - 1)]))
            --first;
        while (last + 1 < count && isSameKind(text[static_cast<std::size_t>(last + 1)]))
            ++last;
    }
    // The cells the word's ends stand in, and the cuts into them.
    const auto stretchStart = [&](int letter) {
        return static_cast<int>(std::ranges::lower_bound(owners, owners[static_cast<std::size_t>(letter)]) - owners.begin());
    };
    int startColumn = anchorColumn + owners[static_cast<std::size_t>(first)];
    int startSubColumn = first - stretchStart(first);
    int endColumn = anchorColumn + owners[static_cast<std::size_t>(last)];
    int endSubColumn = last - stretchStart(last) + 1;
    // Text running on past the run's edges without a break: a word whose first
    // letter is the run's first, in a run whose text begins at its claim's
    // first cell, joins the word in the cells before; likewise after.
    if (isWord && first == 0 && owners.front() == 0) {
        int before = anchorColumn - 1;
        while (before >= 0 && Selection::engineIsWordByte(engine, cellAt(&bound, row, before)))
            --before;
        if (before + 1 < anchorColumn) {
            startColumn = before + 1;
            startSubColumn = 0;
        }
    }
    if (isWord && last == count - 1 && bound.runMemoTextColumns == widthCells * scale) {
        int after = anchorColumn + widthCells * scale;
        while (after < terminal.columns && Selection::engineIsWordByte(engine, cellAt(&bound, row, after)))
            ++after;
        if (after > anchorColumn + widthCells * scale) {
            endColumn = after - 1;
            endSubColumn = 0;
        }
    }
    SelectionState& state = bound.session->state;
    setCharacterSpan(&state, totalRows(terminal), terminal.columns, row, startColumn, endColumn);
    state.startSubColumn = startSubColumn;
    state.endSubColumn = endSubColumn;
    return true;
}

// A word press on a rich text block (Docs/rich-text.md): the word around the
// character under the pointer, by the same word classes, found in the block's
// text and bounded by the block, set as a cut into the block's top-left cell.
// Answers false where the press is on no block.
[[nodiscard]] bool beginBlockWord(BoundContext& bound, int visibleRow, int row, int column)
{
    const Terminal& terminal = *bound.terminal;
    if (terminal.richTextBlocks.isEmpty())
        return false;
    const int screenRow = row - historyRows(terminal);
    const RichText::Block* block = RichText::blockCovering(terminal, RichText::activeScreen(terminal), screenRow, column);
    if (!block)
        return false;
    const RichText::Layout& layout = RichText::layoutOf(terminal, *block);
    const std::uint32_t copyable = layout.copyableEnd();
    if (!copyable)
        return false;
    const double along = (std::isfinite(bound.pointerXInCells) ? bound.pointerXInCells : column + 0.5) - block->column;
    const double down = (std::isfinite(bound.pointerYInRows) ? bound.pointerYInRows - visibleRow : 0.5) + (screenRow - block->row);
    const std::uint32_t under = std::min(layout.characterAt(static_cast<std::int64_t>(along * layout.cellWidth()), static_cast<std::int64_t>(down * layout.cellHeight())), copyable - 1);
    const std::span<const uint32_t> text = std::span<const uint32_t>(block->document.text).first(copyable);
    const Selection::Engine* engine = &bound.session->engine;
    const bool isWord = Selection::engineIsWordByte(engine, text[under]);
    const bool isSpace = !isWord && text[under] == ' ';
    const auto isSameKind = [&](uint32_t codePoint) {
        return isWord ? Selection::engineIsWordByte(engine, codePoint) : (isSpace && codePoint == ' ');
    };
    std::uint32_t first = under;
    std::uint32_t end = layout.graphemeEnd(under);
    if (isWord || isSpace) {
        while (first > 0 && (layout.continuesGrapheme(first) || isSameKind(text[first - 1])))
            --first;
        while (end < copyable && (layout.continuesGrapheme(end) || isSameKind(text[end])))
            ++end;
    }
    SelectionState& state = bound.session->state;
    const int anchorRow = row - (screenRow - block->row);
    setCharacterSpan(&state, totalRows(terminal), terminal.columns, anchorRow, block->column, block->column);
    state.startSubColumn = static_cast<int>(first);
    state.endSubColumn = static_cast<int>(end);
    return true;
}

void boundBeginWord(void* userData, int visibleRow, int column)
{
    auto& bound = *static_cast<BoundContext*>(userData);
    Terminal& terminal = *bound.terminal;
    const int row = absoluteRow(terminal, visibleRow, bound.viewOffset);
    if (beginBlockWord(bound, visibleRow, row, column))
        return;
    if (beginRunWord(bound, row, column))
        return;

    // A configured [characterClass] table is consulted first: it grows the
    // click to the widest contiguous token a class matches — a whole path
    // or URL. On a match the span is set directly; without a match, or
    // with no table at all, the built-in word walk below answers, so the
    // default double click is byte-for-byte unchanged.
    if (Triggers::characterClassCount(&terminal) > 0) {
        const int columns = terminal.columns;
        std::array<char, maxColumns> rowBytes;
        const int count = columns < maxColumns ? columns : maxColumns;
        for (int index = 0; index < count; ++index) {
            const Cell* cell = screenCell(terminal, row, index);
            const uint16_t codePoint = cell ? static_cast<uint16_t>(cell->codePoint) : ' ';
            rowBytes[static_cast<std::size_t>(index)] = (codePoint >= 0x20 && codePoint < 0x7F) ? static_cast<char>(codePoint) : ' ';
        }
        if (const std::optional<Triggers::MatchSpan> span = Triggers::expandWordSelection(
                &terminal, std::string_view { rowBytes.data(), static_cast<std::size_t>(count) }, column)) {
            setCharacterSpan(&bound.session->state, totalRows(terminal), columns, row, span->begin, span->end);
            return;
        }
    }

    doubleClickExpand(&bound.session->state, totalRows(terminal), terminal.columns, row, column,
        cellAt, &bound, &bound.session->engine, bound.style.smartToken);
}

void boundBeginLine(void* userData, int visibleRow, int /*column*/)
{
    auto& bound = *static_cast<BoundContext*>(userData);
    Terminal& terminal = *bound.terminal;
    const int rows = totalRows(terminal);
    const int clicked = absoluteRow(terminal, visibleRow, bound.viewOffset);

    // A triple click takes the logical line, which is not the visual row when
    // the line soft-wrapped. Selecting the row alone hands back a fragment of
    // what the user pointed at -- and for the case that motivates this, a
    // wrapped URL, a fragment is worse than nothing because it still looks like
    // a URL.
    //
    // The walk is the same one the link scan uses: a row joins the line above it
    // when that row is marked wrapped. Marked rows are the only ones joined, so
    // a hard-wrapped line -- tmux emitting each visual row separately -- still
    // selects one row, which is the truthful answer for content the terminal was
    // never told is continuous.
    const LogicalLineRun run = logicalLineRun(terminal, rows, clicked);
    expandToLine(&bound.session->state, rows, terminal.columns, run.firstRow);
    if (run.lastRow > run.firstRow) {
        // expandToLine anchors both ends on one row; carry the head to the end
        // of the group so the selection covers the whole logical line.
        bound.session->state.headRow = run.lastRow;
        bound.session->state.headColumn = terminal.columns - 1;
    }
}

void boundUpdate(void* userData, int visibleRow, int column)
{
    auto& bound = *static_cast<BoundContext*>(userData);
    Terminal& terminal = *bound.terminal;
    SelectionState& state = bound.session->state;
    const int rows = totalRows(terminal);
    const int row = absoluteRow(terminal, visibleRow, bound.viewOffset);
    if (state.mode != SelectionMode::Line) {
        update(&state, rows, terminal.columns, row, column);
        return;
    }

    // The anchor always remains on one edge of the triple-clicked run. Walking
    // from that edge recovers the whole anchor even after the drag reverses.
    const LogicalLineRun anchor = logicalLineRun(terminal, rows, state.anchorRow);
    const LogicalLineRun head = logicalLineRun(terminal, rows, row);
    state.anchorColumn = 0;
    state.headColumn = terminal.columns - 1;
    if (head.lastRow < anchor.firstRow) {
        state.anchorRow = anchor.lastRow;
        state.headRow = head.firstRow;
        return;
    }

    state.anchorRow = anchor.firstRow;
    state.headRow = head.lastRow;
}

void boundClear(void* userData) { clear(&static_cast<BoundContext*>(userData)->session->state); }

bool boundHasSelection(void* userData) { return Selection::hasSelection(&static_cast<BoundContext*>(userData)->session->state); }

[[nodiscard]] GestureOperations boundOperations(BoundContext& bound) noexcept
{
    return {
        .beginCharacter = boundBeginCharacter,
        .beginWord = boundBeginWord,
        .beginLine = boundBeginLine,
        .update = boundUpdate,
        .clear = boundClear,
        .hasSelection = boundHasSelection,
        .ud = &bound,
    };
}

// Bounds resolved the way the highlight draws them: wide-aware, with the
// session's word engine so a word-mode drag re-snaps live.
[[nodiscard]] bool resolveBounds(Session& session, Terminal& terminal, SelectionBounds& out) noexcept
{
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = 0, .style = {} };
    return getBoundsWideWith(&session.state, totalRows(terminal), terminal.columns,
        cellAt, &bound, attributesAt, &bound, &out, &session.engine, rowWrappedAt, &bound);
}

// One digest over the screen's live rows, the same bytes the renderer keys
// its own row digests on, so two screens that paint alike digest alike.
[[nodiscard]] std::uint64_t screenDigest(const Terminal& terminal) noexcept
{
    std::uint64_t digest = 0xCBF29CE484222325ull;
    const auto mix = [&digest](std::span<const std::byte> bytes) {
        for (const std::byte byte : bytes) {
            digest ^= static_cast<std::uint64_t>(byte);
            digest *= 0x100000001B3ull;
        }
    };
    if (!terminal.grid)
        return digest;
    for (int row = 0; row < terminal.rows; ++row) {
        const int width = std::clamp(terminal.grid->rowContentWidth(Row(row)), 0, terminal.columns);
        mix(std::as_bytes(std::span(&width, 1)));
        if (width == 0)
            continue;
        const Cell* cells = terminal.grid->cellAt(Row(row), Column(0));
        if (!cells)
            continue;
        mix(std::as_bytes(std::span(cells, static_cast<std::size_t>(width))));
    }
    return digest;
}

// Records what this view has seen of the buffer, and nothing on the terminal:
// another view of the same terminal judges its own selection against the same
// state, so no view may consume or reset anything there.
void observeBuffer(Session& session, const Terminal& terminal) noexcept
{
    session.observedScrollbackAppended = Screen::scrollbackTotalAppended(&terminal);
    session.observedScrollbackRows = historyRows(terminal);
    session.observedBufferEpoch = terminal.absoluteRowEpoch;
    session.wasUsingAlternateScreen = terminal.screens.isUsingAlternateScreen;
    session.hasBufferObservation = true;
    // Digested only while something stands to follow on the primary screen:
    // ends on the alternate screen name rows, which a repaint leaves where
    // they are, so every other drain pays nothing for a comparison it would
    // never make.
    session.hasScreenObservation = (Selection::hasSelection(&session.state) || session.gesture.isOpen) && !terminal.screens.isUsingAlternateScreen;
    session.observedScreenDigest = session.hasScreenObservation ? screenDigest(terminal) : 0;
}

// The ends as the session's rows hold them now.
[[nodiscard]] NamedEnds currentEnds(const Session& session) noexcept
{
    NamedEnds ends;
    ends.hasSelection = Selection::hasSelection(&session.state);
    ends.hasGesture = session.gesture.isOpen;
    ends.mode = session.state.mode;
    if (ends.hasSelection) {
        ends.anchorRow = session.state.anchorRow;
        ends.anchorColumn = session.state.anchorColumn;
        ends.headRow = session.state.headRow;
        ends.headColumn = session.state.headColumn;
    }
    if (ends.hasGesture) {
        ends.gestureAnchorRow = session.gestureAnchorAbsoluteRow;
        ends.gestureTargetRow = session.gestureTargetAbsoluteRow;
    }
    return ends;
}

[[nodiscard]] bool isSameEnds(const NamedEnds& now, const NamedEnds& named) noexcept
{
    return named.isNamed && now.hasSelection == named.hasSelection && now.hasGesture == named.hasGesture && now.mode == named.mode
        && now.anchorRow == named.anchorRow && now.anchorColumn == named.anchorColumn && now.headRow == named.headRow
        && now.headColumn == named.headColumn && now.gestureAnchorRow == named.gestureAnchorRow
        && now.gestureTargetRow == named.gestureTargetRow;
}

// Names the ends where the session's rows hold them. An end that cannot be
// named, on a row past the buffer or in a line whose start was evicted,
// leaves the ends unnamed, and they then stay where the rows say until an
// operation moves them.
//
// On the alternate screen an end names its row, not the characters on it: a
// full-screen program repaints its rows in place, erasing and rewriting them,
// and a selection there covers the cells the user dragged over.
void nameEnds(Session& session, const Terminal& terminal) noexcept
{
    NamedEnds ends = currentEnds(session);
    bool isNamed = true;
    const auto name = [&](Screen::LogicalPosition& position, int row, int column) {
        const auto captured = terminal.screens.isUsingAlternateScreen ? Screen::captureAlternateRow(terminal, row) : Screen::capturePosition(terminal, row, column);
        if (captured)
            position = *captured;
        isNamed = isNamed && captured.has_value();
    };
    if (ends.hasSelection) {
        name(session.anchorPosition, ends.anchorRow, ends.anchorColumn);
        name(session.headPosition, ends.headRow, ends.headColumn);
    }
    if (ends.hasGesture) {
        name(session.gestureAnchorPosition, ends.gestureAnchorRow, 0);
        name(session.gestureTargetPosition, ends.gestureTargetRow, 0);
    }
    ends.isNamed = isNamed;
    ends.absoluteRowEpoch = terminal.absoluteRowEpoch;
    session.namedEnds = ends;
}

// Names the ends an operation moved; ends it left alone keep their names.
void nameMovedEnds(Session& session, const Terminal& terminal) noexcept
{
    if (!isSameEnds(currentEnds(session), session.namedEnds))
        nameEnds(session, terminal);
}

// Names the ends an operation moved as it returns, before any output can
// renumber the rows the operation set them in.
class EndsNamedOnReturn {
public:
    EndsNamedOnReturn(Session& session, const Terminal& terminal) noexcept
        : m_session(session)
        , m_terminal(terminal)
    {
    }
    ~EndsNamedOnReturn() { nameMovedEnds(m_session, m_terminal); }
    EndsNamedOnReturn(const EndsNamedOnReturn&) = delete;
    EndsNamedOnReturn& operator=(const EndsNamedOnReturn&) = delete;

private:
    Session& m_session;
    const Terminal& m_terminal;
};

enum class EndsAfterResolving : std::uint8_t {
    Unchanged,
    Moved,
    Lost,
};

// Records the resolved ends as named, and whether resolving moved them.
[[nodiscard]] EndsAfterResolving settleResolvedEnds(Session& session, const NamedEnds& named) noexcept
{
    const NamedEnds now = currentEnds(session);
    const bool didMove = !isSameEnds(now, named);
    session.namedEnds = now;
    session.namedEnds.isNamed = true;
    session.namedEnds.absoluteRowEpoch = named.absoluteRowEpoch;
    return didMove ? EndsAfterResolving::Moved : EndsAfterResolving::Unchanged;
}

// Puts the ends where their positions' characters are now. A selection one of
// whose ends names nothing any more is lost whole, since a selection is one
// thing; so is a rectangle across a resize, whose columns a rewrap gives no
// meaning to.
//
// Without a rewrap, output and eviction move both ends alike, and only an
// in-place move of part of the screen moves one end and not the other: that
// selection would shear across content that moved and content that did not,
// so it is lost too. Its columns are the ones the gesture set, which name
// letters inside a proportional run the positions' cells cannot; only a
// rewrap, which moves characters to other columns, takes the positions'.
[[nodiscard]] EndsAfterResolving resolveEnds(Session& session, const Terminal& terminal) noexcept
{
    const NamedEnds named = session.namedEnds;
    const bool didRewrap = named.absoluteRowEpoch != terminal.absoluteRowEpoch;
    if (named.hasSelection && named.mode == SelectionMode::Rectangle && didRewrap)
        return EndsAfterResolving::Lost;
    const auto resolve = [&](const Screen::LogicalPosition& position) { return Screen::resolvePosition(terminal, position); };
    const auto isSheared = [didRewrap](int namedFirst, int first, int namedSecond, int second) {
        return !didRewrap && first - namedFirst != second - namedSecond;
    };
    const int lastColumn = std::max(terminal.columns - 1, 0);
    if (named.hasSelection) {
        const auto anchor = resolve(session.anchorPosition);
        const auto head = resolve(session.headPosition);
        if (!anchor || !head || isSheared(named.anchorRow, anchor->absoluteRow, named.headRow, head->absoluteRow))
            return EndsAfterResolving::Lost;
        SelectionState& state = session.state;
        state.anchorRow = anchor->absoluteRow;
        state.headRow = head->absoluteRow;
        switch (state.mode) {
        case SelectionMode::Character:
        case SelectionMode::Word:
            if (didRewrap) {
                state.anchorColumn = anchor->column;
                state.headColumn = head->column;
            }
            break;
        case SelectionMode::Line: {
            // A line selection holds whole logical lines, which a rewrap
            // spreads over other rows: its ends go back to their runs' edges.
            const int rows = totalRows(terminal);
            const LogicalLineRun anchorRun = logicalLineRun(terminal, rows, state.anchorRow);
            const LogicalLineRun headRun = logicalLineRun(terminal, rows, state.headRow);
            const bool isUpward = headRun.lastRow < anchorRun.firstRow;
            state.anchorRow = isUpward ? anchorRun.lastRow : anchorRun.firstRow;
            state.headRow = isUpward ? headRun.firstRow : headRun.lastRow;
            state.anchorColumn = 0;
            state.headColumn = lastColumn;
            break;
        }
        case SelectionMode::Rectangle:
            // Columns are the rectangle's own, not its ends' characters'.
            break;
        }
        state.anchorColumn = std::clamp(state.anchorColumn, 0, lastColumn);
        state.headColumn = std::clamp(state.headColumn, 0, lastColumn);
    }
    if (named.hasGesture) {
        const auto gestureAnchor = resolve(session.gestureAnchorPosition);
        const auto gestureTarget = resolve(session.gestureTargetPosition);
        if (!gestureAnchor || !gestureTarget
            || isSheared(named.gestureAnchorRow, gestureAnchor->absoluteRow, named.gestureTargetRow, gestureTarget->absoluteRow))
            return EndsAfterResolving::Lost;
        session.gestureAnchorAbsoluteRow = gestureAnchor->absoluteRow;
        session.gestureTargetAbsoluteRow = gestureTarget->absoluteRow;
    }
    return settleResolvedEnds(session, named);
}

// Moves the ends with the alternate screen's rows, which is all a selection
// there names: an erase or a rewrite of a selected row leaves it on its cells,
// and only an in-place move carries them. As with characters, a move that
// carries one end and not the other would shear the selection, and one that
// carries an end off its band takes the rows it covered; either loses it.
[[nodiscard]] EndsAfterResolving followAlternateRows(Session& session, const Terminal& terminal) noexcept
{
    const NamedEnds named = session.namedEnds;
    const auto isSheared = [](int namedFirst, int first, int namedSecond, int second) { return first - namedFirst != second - namedSecond; };
    if (named.hasSelection) {
        SelectionState& state = session.state;
        const auto anchor = Screen::resolveAlternateRow(terminal, session.anchorPosition, state.anchorColumn);
        const auto head = Screen::resolveAlternateRow(terminal, session.headPosition, state.headColumn);
        if (!anchor || !head || isSheared(named.anchorRow, *anchor, named.headRow, *head))
            return EndsAfterResolving::Lost;
        state.anchorRow = *anchor;
        state.headRow = *head;
    }
    if (named.hasGesture) {
        const auto gestureAnchor = Screen::resolveAlternateRow(terminal, session.gestureAnchorPosition, session.gesture.anchorColumn);
        const auto gestureTarget = Screen::resolveAlternateRow(terminal, session.gestureTargetPosition, session.gesture.targetColumn);
        if (!gestureAnchor || !gestureTarget || isSheared(named.gestureAnchorRow, *gestureAnchor, named.gestureTargetRow, *gestureTarget))
            return EndsAfterResolving::Lost;
        session.gestureAnchorAbsoluteRow = *gestureAnchor;
        session.gestureTargetAbsoluteRow = *gestureTarget;
    }
    return settleResolvedEnds(session, named);
}

// Moves the ends with a screen that paints back the rows it scrolls. Only
// that screen's rows move: each sits a row deeper in the buffer per row
// appended, while a row already in history keeps its number and the
// characters under it. Every end stands the same way for the move to apply: a
// selection reaching from history onto the screen names characters the
// repaint does not move, and its positions follow them already.
//
// The ends are in the numbering of the last observation; eviction since then
// lowers every row the move lands on.
//
// - Returns: Whether every end stood on the screen, so the move applied.
[[nodiscard]] bool followRepaintedScreen(Session& session, int appended, int evicted) noexcept
{
    const auto standsOnScreen = [&session](int absoluteRow) {
        return absoluteRow >= session.observedScrollbackRows;
    };
    const bool hasStandingSelection = Selection::hasSelection(&session.state);
    if (hasStandingSelection && (!standsOnScreen(session.state.anchorRow) || !standsOnScreen(session.state.headRow)))
        return false;
    if (session.gesture.isOpen
        && (!standsOnScreen(session.gestureAnchorAbsoluteRow) || !standsOnScreen(session.gestureTargetAbsoluteRow)))
        return false;
    const int rowDelta = appended - evicted;
    if (hasStandingSelection) {
        session.state.anchorRow += rowDelta;
        session.state.headRow += rowDelta;
    }
    if (session.gesture.isOpen) {
        session.gestureAnchorAbsoluteRow += rowDelta;
        session.gestureTargetAbsoluteRow += rowDelta;
    }
    return true;
}

bool synchronizeBufferRows(Session& session, const Terminal& terminal) noexcept
{
    const auto forget = [&](bool hadSelection) {
        clear(&session.state);
        gestureReset(&session.gesture);
        observeBuffer(session, terminal);
        nameEnds(session, terminal);
        return hadSelection;
    };
    if (!session.hasBufferObservation) {
        observeBuffer(session, terminal);
        nameEnds(session, terminal);
        return false;
    }

    const bool hadSelection = Selection::hasSelection(&session.state);
    if (session.gesture.isOpen && session.gesture.dragBegan && !hadSelection)
        return forget(false);
    if (session.wasUsingAlternateScreen != terminal.screens.isUsingAlternateScreen)
        return forget(hadSelection);
    // The alternate screen's ends name rows, which a resize or a clear verb
    // leaves meaning nothing: the program lays its frame out again, and nothing
    // there names what the selected cells held.
    if (terminal.screens.isUsingAlternateScreen && session.observedBufferEpoch != terminal.absoluteRowEpoch)
        return forget(hadSelection);

    // Ends an operation moved since they were named are named where they
    // stand; ends nothing moved go where their characters went, or on the
    // alternate screen where their rows went.
    //
    // Except, on the primary screen, in a frame that reproduces itself: the
    // program painted the same rows back, erasing each before it rewrote it,
    // which would invalidate every position on the screen. When the screen
    // digests as it did, the ends stay on the text: in place, or, when a line
    // feed on the bottom row scrolled rows into history first, as many rows
    // deeper as were appended. A genuine change leaves the digest changed and
    // the ends to their positions. The alternate screen is never digested:
    // its ends are rows, which an erase leaves named.
    const std::int64_t appended = Screen::scrollbackTotalAppended(&terminal) - session.observedScrollbackAppended;
    const std::int64_t retained = static_cast<std::int64_t>(historyRows(terminal)) - session.observedScrollbackRows;
    const std::int64_t evicted = std::clamp<std::int64_t>(appended - retained, 0, terminal.rows);
    // A gesture that ended, or a selection that was cleared, moved nothing:
    // what is left is still named.
    if (!session.gesture.isOpen && session.namedEnds.hasGesture) {
        session.namedEnds.hasGesture = false;
        session.namedEnds.gestureAnchorRow = 0;
        session.namedEnds.gestureTargetRow = 0;
    }
    if (!hadSelection && session.namedEnds.hasSelection) {
        session.namedEnds.hasSelection = false;
        session.namedEnds.mode = session.state.mode;
        session.namedEnds.anchorRow = 0;
        session.namedEnds.anchorColumn = 0;
        session.namedEnds.headRow = 0;
        session.namedEnds.headColumn = 0;
    }
    bool didMove = false;
    if (hadSelection || session.gesture.isOpen) {
        // A resize may leave the screen reading as before, but it renumbered
        // the rows, and a rectangle's columns went with it: no same frame.
        const bool isSameFrame = session.hasScreenObservation && session.observedBufferEpoch == terminal.absoluteRowEpoch
            && appended >= 0 && appended < terminal.rows && screenDigest(terminal) == session.observedScreenDigest;
        if (!isSameEnds(currentEnds(session), session.namedEnds)) {
            nameEnds(session, terminal);
        } else if (isSameFrame && (appended == 0 || followRepaintedScreen(session, static_cast<int>(appended), static_cast<int>(evicted)))) {
            didMove = appended > 0 && hadSelection;
            nameEnds(session, terminal);
        } else if (session.namedEnds.isNamed) {
            const EndsAfterResolving resolved = terminal.screens.isUsingAlternateScreen ? followAlternateRows(session, terminal) : resolveEnds(session, terminal);
            switch (resolved) {
            case EndsAfterResolving::Lost:
                return forget(hadSelection);
            case EndsAfterResolving::Moved:
                didMove = hadSelection;
                break;
            case EndsAfterResolving::Unchanged:
                break;
            }
        }
        // An end that cannot be named stands on a line whose start was
        // evicted, about to go itself; unnamed, its rows would drift.
        if (!session.namedEnds.isNamed)
            return forget(hadSelection);
    }
    observeBuffer(session, terminal);
    return didMove;
}

} // namespace

void init(Session& session) noexcept
{
    Selection::init(&session.state);
    gestureReset(&session.gesture);
    initEngine(&session.engine);
    session.gestureStyle = {};
    session.gestureAnchorAbsoluteRow = 0;
    session.gestureTargetAbsoluteRow = 0;
    session.gesturePointerXInCells = std::numeric_limits<double>::quiet_NaN();
    session.gesturePointerYInRows = std::numeric_limits<double>::quiet_NaN();
    session.gestureAnchorCaret = 0;
    session.gestureAnchorSlot = 0;
    session.gestureTargetCaret = 0;
    session.gestureAnchorSubCaret = 0;
    session.gestureTargetSubCaret = 0;
    session.usesCharacterCarets = false;
    session.observedScrollbackAppended = 0;
    session.viewportAnchor.reset();
    session.viewportHistoryEpoch = 0;
    session.usesLogicalViewport = false;
    session.observedScrollbackRows = 0;
    session.observedBufferEpoch = 0;
    session.wasUsingAlternateScreen = false;
    session.hasBufferObservation = false;
    session.viewportObservedHistory = {};
    session.hasViewportObservation = false;
}

bool dropSelectionAcrossResize(SelectionState& state, std::uint32_t epochBefore, const Terminal& terminal)
{
    const bool isScreenCellSelection = state.mode == SelectionMode::Rectangle || terminal.screens.isUsingAlternateScreen;
    if (!Selection::hasSelection(&state) || !isScreenCellSelection)
        return false;
    if (epochBefore == terminal.absoluteRowEpoch)
        return false; // the resize changed no dimension, so it renumbered nothing
    clear(&state);
    return true;
}

bool beginGesture(Session& session, Terminal& terminal, int visibleRow, int column, int clickCount, int viewOffset, const GestureStyle& style,
    double xInCells, double yInRows) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    // The press that brought the window forward opens nothing: the standing
    // selection stays, and the click after this one is the first that counts.
    if (style.isWindowActivating)
        return false;
    const bool didSynchronize = synchronizeBufferRows(session, terminal);
    // Kept on the session, not just this call: a single click's open is
    // deferred to the first drag motion, and the drag must apply the style
    // the press carried.
    session.gestureStyle = style;
    const int pointerRow = visibleRow;
    // A pointer on either half of a double-height pair addresses the pair's
    // one line of glyphs.
    visibleRow = selectionVisibleRowOf(terminal, visibleRow, viewOffset);
    session.gestureAnchorAbsoluteRow = absoluteRow(terminal, visibleRow, viewOffset);
    session.gestureTargetAbsoluteRow = session.gestureAnchorAbsoluteRow;
    session.usesCharacterCarets = false;
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = viewOffset, .style = style, .pointerXInCells = xInCells, .pointerYInRows = yInRows };
    const GestureOperations operations = boundOperations(bound);
    const bool didBegin = gestureBegin(&session.gesture, &operations, visibleRow,
        logicalColumnUnderScreenCell(terminal, visibleRow, viewOffset, column), clickCount, style.mode);
    if (session.gesture.clickCount == 2 && style.mode == GestureMode::Fresh
        && (session.state.startSubColumn > 0 || session.state.endSubColumn > 0)) {
        // A word inside a packed cell owns character offsets, not the whole
        // cell. Keep both ends of that word as the drag's immutable anchor.
        configureCharacterCaretGesture(session, terminal, CaretResolution {
                                                              .absoluteRow = session.state.anchorRow,
                                                              .caret = session.state.anchorColumn,
                                                              .slot = session.state.anchorColumn,
                                                              .subCaret = session.state.startSubColumn,
                                                              .caretAfter = session.state.headColumn + (session.state.endSubColumn == 0 ? 1 : 0),
                                                              .subCaretAfter = session.state.endSubColumn,
                                                          });
        session.gesture.targetRow = pointerRow;
        session.gesture.targetColumn = column;
        session.gestureTargetAbsoluteRow = absoluteRow(terminal, pointerRow, viewOffset);
        session.gesturePointerXInCells = xInCells;
        session.gesturePointerYInRows = yInRows;
    }
    return didSynchronize || didBegin;
}

bool dragGesture(Session& session, Terminal& terminal, int visibleRow, int column, int viewOffset, double xInCells, double yInRows) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    const bool didRebase = synchronizeBufferRows(session, terminal);
    const int pointerRow = visibleRow;
    visibleRow = selectionVisibleRowOf(terminal, visibleRow, viewOffset);
    if (session.usesCharacterCarets && session.gesture.clickCount == 2) {
        Session word;
        word.engine = session.engine;
        BoundContext head { .session = &word, .terminal = &terminal, .viewOffset = viewOffset, .style = session.gestureStyle, .pointerXInCells = xInCells, .pointerYInRows = yInRows };
        boundBeginWord(&head, visibleRow, logicalColumnUnderScreenCell(terminal, visibleRow, viewOffset, column));
        SelectionBounds bounds;
        if (!resolveBounds(word, terminal, bounds)) {
            return didRebase;
        }
        const bool isBeforeAnchor = std::tuple(bounds.row0, bounds.column0, bounds.subColumn0)
            < std::tuple(session.gestureAnchorAbsoluteRow, session.gestureAnchorCaret, session.gestureAnchorSubCaret);
        const int caret = isBeforeAnchor ? bounds.column0 : bounds.column1 + (bounds.subColumn1 == 0 ? 1 : 0);
        const int subCaret = isBeforeAnchor ? bounds.subColumn0 : bounds.subColumn1;
        const bool didDrag = dragCharacterCaretGesture(session, terminal, CaretResolution {
                                                                              .absoluteRow = isBeforeAnchor ? bounds.row0 : bounds.row1,
                                                                              .caret = caret,
                                                                              .slot = caret,
                                                                              .subCaret = subCaret,
                                                                              .caretAfter = caret,
                                                                              .subCaretAfter = subCaret,
                                                                          },
            viewOffset);
        // A packed word's caret names its anchor cell; output follows the
        // physical pointer row, which may lie farther down a tall block.
        session.gesture.targetRow = pointerRow;
        session.gesture.targetColumn = column;
        session.gestureTargetAbsoluteRow = absoluteRow(terminal, pointerRow, viewOffset);
        session.gesturePointerXInCells = xInCells;
        session.gesturePointerYInRows = yInRows;
        return didDrag || didRebase;
    }
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = viewOffset, .style = session.gestureStyle };
    const GestureOperations operations = boundOperations(bound);
    const bool didDrag = gestureDrag(&session.gesture, &operations, visibleRow,
        logicalColumnUnderScreenCell(terminal, visibleRow, viewOffset, column));
    if (didDrag) {
        session.gestureTargetAbsoluteRow = absoluteRow(terminal, visibleRow, viewOffset);
    }
    return didRebase || didDrag;
}

void configureCharacterCaretGesture(Session& session, const Terminal& terminal, const CaretResolution& press) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    synchronizeBufferRows(session, terminal);
    const int rows = totalRows(terminal);
    if (!session.gesture.isOpen || rows <= 0 || terminal.columns <= 0) {
        return;
    }
    const int absoluteRowValue = std::clamp(press.absoluteRow, 0, rows - 1);
    const int caret = std::clamp(press.caret, 0, terminal.columns);
    const int caretAfter = std::clamp(press.caretAfter, caret, terminal.columns);
    session.gestureAnchorAbsoluteRow = absoluteRowValue;
    session.gestureTargetAbsoluteRow = absoluteRowValue;
    session.gestureAnchorCaret = caret;
    session.gestureAnchorSlot = press.slot;
    session.gestureTargetCaret = caret;
    // The row's end boundary has no cell to its right to stand inside.
    session.gestureAnchorSubCaret = caret < terminal.columns ? std::max(press.subCaret, 0) : 0;
    session.gestureTargetSubCaret = session.gestureAnchorSubCaret;
    session.gestureAnchorCaretAfter = caretAfter;
    session.gestureAnchorSubCaretAfter = caretAfter < terminal.columns ? std::max(press.subCaretAfter, 0) : 0;
    session.usesCharacterCarets = true;
}

bool dragCharacterCaretGesture(Session& session, Terminal& terminal, const CaretResolution& head, int viewOffset) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    const bool didRebase = synchronizeBufferRows(session, terminal);
    const int rows = totalRows(terminal);
    if (!session.gesture.isOpen || !session.usesCharacterCarets || rows <= 0 || terminal.columns <= 0) {
        return didRebase;
    }
    const int absoluteRowValue = std::clamp(head.absoluteRow, 0, rows - 1);
    const int caret = std::clamp(head.caret, 0, terminal.columns);
    const int subCaret = caret < terminal.columns ? std::max(head.subCaret, 0) : 0;
    const int caretAfter = std::clamp(head.caretAfter, caret, terminal.columns);
    const int subCaretAfter = caretAfter < terminal.columns ? std::max(head.subCaretAfter, 0) : 0;
    const bool isOnLetter = caretAfter != caret || subCaretAfter != subCaret;
    // Between boundaries nothing opens until the pointer leaves the boundary
    // it went down on: the same row and the same threshold is still the press.
    // On a letter the first motion is the drag, and selects that letter.
    if (!session.gesture.dragBegan && !isOnLetter
        && absoluteRowValue == session.gestureAnchorAbsoluteRow
        && head.slot == session.gestureAnchorSlot
        && subCaret == session.gestureAnchorSubCaret) {
        return didRebase;
    }

    // The letters between the press and the pointer, both included: from the
    // far edge of the pressed letter to the far edge of the one under the
    // pointer. Where either is a boundary rather than a letter its two edges
    // are one, and this is the half-open span between two boundaries.
    const auto ordinal = [&](int row, int cell, int offset) {
        return (static_cast<long>(row) * (terminal.columns + 1) + cell) * static_cast<long>(Ubiquitty::runTextMaximumCodePoints + 1) + offset;
    };
    const bool isHeadAtOrAfterAnchor
        = ordinal(absoluteRowValue, caret, subCaret) >= ordinal(session.gestureAnchorAbsoluteRow, session.gestureAnchorCaret, session.gestureAnchorSubCaret);
    if (isHeadAtOrAfterAnchor) {
        charDragLinear(&session.state, rows, terminal.columns,
            session.gestureAnchorAbsoluteRow, session.gestureAnchorCaret, session.gestureAnchorSubCaret,
            absoluteRowValue, caretAfter, subCaretAfter);
    } else {
        charDragLinear(&session.state, rows, terminal.columns,
            session.gestureAnchorAbsoluteRow, session.gestureAnchorCaretAfter, session.gestureAnchorSubCaretAfter,
            absoluteRowValue, caret, subCaret);
    }
    session.gesture.dragBegan = true;
    session.gesture.targetRow = absoluteRowValue - historyRows(terminal) + viewOffset;
    session.gesture.targetColumn = caret < terminal.columns ? caret : terminal.columns - 1;
    session.gestureTargetAbsoluteRow = absoluteRowValue;
    session.gestureTargetCaret = caret;
    session.gestureTargetSubCaret = subCaret;
    return true;
}

bool endGesture(Session& session) noexcept
{
    session.gesture.isOpen = false;
    return Selection::hasSelection(&session.state);
}

bool isGestureOpen(const Session& session) noexcept
{
    return session.gesture.isOpen;
}

bool hasGestureDragBegun(const Session& session) noexcept
{
    return session.gesture.dragBegan;
}

bool needsOutputEvaluation(const Session& session) noexcept
{
    return hasSelection(session) || isGestureOpen(session);
}

namespace {

// The rows a held viewport moves by since the session last judged output,
// and the baseline the next transition is judged against. Facts from another
// epoch are not comparable: the rows were renumbered, and the offset across
// that is the resize's own business.
[[nodiscard]] int viewportRowsAppended(Session& session, const Screen::HistoryFacts& history) noexcept
{
    const bool isComparable = session.hasViewportObservation
        && session.viewportObservedHistory.absoluteRowEpoch == history.absoluteRowEpoch;
    const std::int64_t rows = isComparable ? history.appendedRows - session.viewportObservedHistory.appendedRows : 0;
    session.viewportObservedHistory = history;
    session.hasViewportObservation = true;
    return static_cast<int>(std::clamp<std::int64_t>(rows, 0, std::numeric_limits<int>::max()));
}

// The scroll-on-output mode, read only on the drains where it decides.
[[nodiscard]] Input::OutputSnapPolicy outputSnapPolicy(const Session& session, const Terminal& terminal, int viewOffset) noexcept
{
    const Input::SelectionPresence presence = Selection::hasSelection(&session.state)
        ? Input::SelectionPresence::Present
        : Input::SelectionPresence::Absent;
    if (!Input::isSnapPolicyConsulted(presence, viewOffset))
        return Input::OutputSnapPolicy::Indeterminate;
    return Modes::shouldSnapToTail(&terminal, Modes::SnapTrigger::Output)
        ? Input::OutputSnapPolicy::SnapToTail
        : Input::OutputSnapPolicy::HoldViewedRows;
}

OutputUpdate updateOpenGestureAfterOutput(Session& session, Terminal& terminal, OutputUpdate update) noexcept;

Input::ViewportAfterOutput anchoredViewportAfterOutput(Session& session, const Terminal& terminal, int viewOffset, Input::OutputSnapPolicy snapPolicy) noexcept
{
    const auto history = Screen::historyFacts(&terminal);
    auto viewport = viewportAfterOutput(session, viewOffset, snapPolicy, history);
    if (!session.usesLogicalViewport)
        return viewport;
    if (viewport.hold == Input::ViewportHold::Follow || !history.retainedRows) {
        session.viewportAnchor.reset();
        return viewport;
    }
    if (session.viewportAnchor) {
        if (session.viewportHistoryEpoch == terminal.historyEpoch) {
            if (const auto place = Screen::resolvePosition(terminal, *session.viewportAnchor)) {
                viewport.viewOffset = Screen::clampViewOffset(history.retainedRows - place->absoluteRow, history.retainedRows);
                return viewport;
            }
        }
        viewport.viewOffset = history.retainedRows;
    }
    observeViewport(session, terminal, static_cast<int>(viewport.viewOffset));
    return viewport;
}

} // namespace

void observeViewport(Session& session, const Terminal& terminal, int viewOffset) noexcept
{
    session.usesLogicalViewport = true;
    session.viewportHistoryEpoch = terminal.historyEpoch;
    session.viewportObservedHistory = Screen::historyFacts(&terminal);
    session.hasViewportObservation = true;
    const int rows = session.viewportObservedHistory.retainedRows;
    session.viewportAnchor.reset();
    if (viewOffset <= 0 || rows <= 0 || !terminal.lineStore)
        return;
    const int firstRow = rows - std::min(viewOffset, rows);
    const auto place = Screen::capturePosition(terminal, firstRow, 0);
    const auto hostPosition = terminal.lineStore->positionAt(firstRow);
    if (!place || !hostPosition)
        return;
    // Remote cache rows are replaced on rewrap. Their host line is durable;
    // a pin on a projection row is not.
    if (const auto anchor = Screen::historyPosition(terminal, hostPosition->line, place->cell)) {
        session.viewportAnchor = *anchor;
        session.viewportAnchor->voidCells = place->voidCells;
    }
}

void noteViewportBeforeResize(Session& session, const Terminal& terminal, int viewOffset) noexcept
{
    if (session.usesLogicalViewport) {
        if (!session.viewportAnchor)
            observeViewport(session, terminal, viewOffset);
        return;
    }
    const int rows = Screen::scrollbackCount(&terminal);
    if (viewOffset <= 0 || rows <= 0) {
        session.viewportAnchor.reset();
        return;
    }
    const int firstRow = rows - std::min(viewOffset, rows);
    if (session.viewportAnchor && session.viewportHistoryEpoch == terminal.historyEpoch) {
        if (const auto place = Screen::resolvePosition(terminal, *session.viewportAnchor); place && place->absoluteRow == firstRow)
            return;
    }
    session.viewportHistoryEpoch = terminal.historyEpoch;
    const auto anchor = Screen::capturePosition(terminal, firstRow, 0);
    session.viewportAnchor = anchor ? std::optional(*anchor) : std::nullopt;
}

int viewportAfterResize(Session& session, const Terminal& terminal, int viewOffset) noexcept
{
    const auto history = Screen::historyFacts(&terminal);
    session.viewportObservedHistory = history;
    session.hasViewportObservation = true;
    if (viewOffset <= 0 || history.retainedRows <= 0) {
        session.viewportAnchor.reset();
        return 0;
    }
    if (session.viewportAnchor && session.viewportHistoryEpoch == terminal.historyEpoch) {
        if (const auto place = Screen::resolvePosition(terminal, *session.viewportAnchor))
            return Screen::clampViewOffset(history.retainedRows - place->absoluteRow, history.retainedRows);
    }
    session.viewportAnchor.reset();
    if (session.usesLogicalViewport)
        observeViewport(session, terminal, history.retainedRows);
    return history.retainedRows;
}

Input::ViewportAfterOutput viewportAfterOutput(Session& session, int viewOffset, Input::OutputSnapPolicy snapPolicy, const Screen::HistoryFacts& history) noexcept
{
    const Input::SelectionPresence presence = Selection::hasSelection(&session.state)
        ? Input::SelectionPresence::Present
        : Input::SelectionPresence::Absent;
    return Input::viewportAfterOutput(presence, viewOffset, snapPolicy, history.retainedRows, viewportRowsAppended(session, history));
}

OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, int viewOffset, Input::OutputSnapPolicy snapPolicy) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    OutputUpdate update;
    if (!needsOutputEvaluation(session)) {
        update.viewport = anchoredViewportAfterOutput(session, terminal, viewOffset, snapPolicy);
        return update;
    }
    update.didChangeSelection = synchronizeBufferRows(session, terminal);
    update.viewport = anchoredViewportAfterOutput(session, terminal, viewOffset, snapPolicy);

    if (!session.gesture.isOpen) {
        return update;
    }
    return updateOpenGestureAfterOutput(session, terminal, update);
}

OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, int viewOffset) noexcept
{
    return updateAfterOutput(session, terminal, viewOffset, outputSnapPolicy(session, terminal, viewOffset));
}

OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, std::mutex& modelLock, int viewOffset, const Screen::HistoryFacts& history) noexcept
{
    if (needsOutputEvaluation(session) || session.usesLogicalViewport) {
        const std::scoped_lock hold(modelLock);
        return updateAfterOutput(session, terminal, viewOffset);
    }
    Input::OutputSnapPolicy snapPolicy = Input::OutputSnapPolicy::Indeterminate;
    if (Input::isSnapPolicyConsulted(Input::SelectionPresence::Absent, viewOffset)) {
        const std::unique_lock hold(modelLock, std::try_to_lock);
        if (hold.owns_lock())
            snapPolicy = outputSnapPolicy(session, terminal, viewOffset);
    }
    OutputUpdate update;
    update.viewport = viewportAfterOutput(session, viewOffset, snapPolicy, history);
    return update;
}

namespace {

// The open gesture's head, re-evaluated at its retained pointer cell now that
// output moved the content beneath it.
OutputUpdate updateOpenGestureAfterOutput(Session& session, Terminal& terminal, OutputUpdate update) noexcept
{
    const int targetAbsoluteRow = absoluteRow(terminal, session.gesture.targetRow, static_cast<int>(update.viewport.viewOffset));
    if (targetAbsoluteRow == session.gestureTargetAbsoluteRow) {
        return update;
    }

    if (session.usesCharacterCarets && session.gesture.clickCount == 2) {
        update.didChangeSelection = dragGesture(session, terminal, session.gesture.targetRow, session.gesture.targetColumn,
                                        static_cast<int>(update.viewport.viewOffset), session.gesturePointerXInCells, session.gesturePointerYInRows)
            || update.didChangeSelection;
        return update;
    }

    if (session.usesCharacterCarets) {
        charDragLinear(&session.state, totalRows(terminal), terminal.columns,
            session.gestureAnchorAbsoluteRow, session.gestureAnchorCaret, session.gestureAnchorSubCaret,
            targetAbsoluteRow, session.gestureTargetCaret, session.gestureTargetSubCaret);
        session.gesture.dragBegan = true;
        session.gestureTargetAbsoluteRow = targetAbsoluteRow;
        update.didChangeSelection = true;
        return update;
    }

    BoundContext bound {
        .session = &session,
        .terminal = &terminal,
        .viewOffset = static_cast<int>(update.viewport.viewOffset),
        .style = session.gestureStyle,
    };
    const GestureOperations operations = boundOperations(bound);
    const int targetRow = session.gesture.targetRow;
    const int targetColumn = session.gesture.targetColumn;
    update.didChangeSelection = gestureDrag(&session.gesture, &operations, targetRow, targetColumn) || update.didChangeSelection;
    session.gestureTargetAbsoluteRow = targetAbsoluteRow;
    return update;
}

} // namespace

bool hasSelection(const Session& session) noexcept { return Selection::hasSelection(&session.state); }

bool hasSelection(Session& session, const Terminal& terminal) noexcept
{
    synchronizeBufferRows(session, terminal);
    return Selection::hasSelection(&session.state);
}

bool clearSelection(Session& session) noexcept
{
    const bool didClear = Selection::hasSelection(&session.state);
    clear(&session.state);
    gestureReset(&session.gesture);
    return didClear;
}

bool clearSelectionForReportedPress(Session& session, const GestureStyle& style) noexcept
{
    return !style.isWindowActivating && clearSelectionForUserInput(session);
}

bool clearSelectionForUserInput(Session& session) noexcept
{
    const bool didClear = clearForUserInput(&session.state);
    gestureReset(&session.gesture);
    return didClear;
}

void selectAllText(Session& session, const Terminal& terminal) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    synchronizeBufferRows(session, terminal);
    gestureReset(&session.gesture);
    selectAll(&session.state, totalRows(terminal), terminal.columns);
}

std::optional<SelectionBounds> selectedBounds(Session& session, Terminal& terminal) noexcept
{
    synchronizeBufferRows(session, terminal);
    SelectionBounds bounds;
    if (!Selection::hasSelection(&session.state) || !resolveBounds(session, terminal, bounds))
        return std::nullopt;
    return bounds;
}

std::string selectedText(Session& session, Terminal& terminal)
{
    synchronizeBufferRows(session, terminal);
    if (!Selection::hasSelection(&session.state))
        return {};
    SelectionBounds bounds;
    if (!resolveBounds(session, terminal, bounds))
        return {};
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = 0, .style = {} };
    bound.copyBounds = &bounds;
    bound.copyMode = session.state.mode;
    // Rectangular blocks keep every row break: each grid row is its own
    // physical line whatever the wrap flags say.
    const SoftWrapNewline newline = session.state.mode == SelectionMode::Rectangle ? SoftWrapNewline::Keep : SoftWrapNewline::Join;
    return selectionText(&bounds, session.state.mode, terminal.columns, cellAt, &bound, textCellAt, &bound, newline, rowWrappedAt, &bound, rowExtentAt, &bound);
}

std::string copiedText(Session& session, Terminal& terminal)
{
    if (!terminal.shouldCopyRightToLeft)
        return selectedText(session, terminal);
    const CellPalette palette = CellPalette::fromTerminalPalette(terminal.palette);
    return styledClipboard(session, terminal, StyleColorMode::StylesOnly, palette, { nullptr, 0 }, StyledFlavors::None).plain;
}

StyledClipboard styledClipboard(Session& session, Terminal& terminal, StyleColorMode colorMode, const CellPalette& palette, const StyledFont& font, StyledFlavors flavors)
{
    synchronizeBufferRows(session, terminal);
    if (!Selection::hasSelection(&session.state))
        return {};
    SelectionBounds bounds;
    if (!resolveBounds(session, terminal, bounds))
        return {};
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = 0, .style = {} };
    bound.copyBounds = &bounds;
    bound.copyMode = session.state.mode;
    StyledBound styled { .bound = &bound, .palette = &palette };
    const SoftWrapNewline newline = session.state.mode == SelectionMode::Rectangle ? SoftWrapNewline::Keep : SoftWrapNewline::Join;
    // Three-byte arrays rather than the address of a colour's first channel:
    // the generator's reference-to-array parameter refuses a bare pointer,
    // so no site reads RGB through a struct's layout.
    const std::array<std::uint8_t, 3> defaultForeground = { palette.defaultForeground.red, palette.defaultForeground.green, palette.defaultForeground.blue };
    const std::array<std::uint8_t, 3> defaultBackground = { palette.defaultBackground.red, palette.defaultBackground.green, palette.defaultBackground.blue };
    auto walked = buildStyledRuns(&bounds, session.state.mode, terminal.columns, cellAt, &bound, textCellAt, &bound, styledCellAt, &styled, newline, rowWrappedAt, &bound,
        colorMode, font, defaultForeground, defaultBackground, rowExtentAt, &bound);
    if (terminal.shouldCopyRightToLeft)
        reverseCopyLines(walked);
    return emitStyledFlavors(walked, flavors);
}

int blockSelections(Session& session, Terminal& terminal, BlockSelections out) noexcept
{
    synchronizeBufferRows(session, terminal);
    if (terminal.richTextBlocks.isEmpty() || !Selection::hasSelection(&session.state))
        return 0;
    SelectionBounds bounds;
    if (!resolveBounds(session, terminal, bounds))
        return 0;
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = 0, .style = {} };
    bound.copyBounds = &bounds;
    bound.copyMode = session.state.mode;
    int written = 0;
    for (const RichText::Block& block : terminal.richTextBlocks.blocks(RichText::activeScreen(terminal))) {
        if (static_cast<std::size_t>(written) == out.size())
            break;
        const int anchorRow = historyRows(terminal) + block.row;
        if (anchorRow < bounds.row0 || anchorRow > bounds.row1)
            continue;
        const RowSpan span = rowSpan(&bounds, session.state.mode, anchorRow, terminal.columns, cellAt, &bound, attributesAt, &bound);
        if (!span.selected || block.column < span.column0 || block.column > span.column1)
            continue;
        const auto copyable = static_cast<int>(RichText::copyableLength(terminal, block));
        const PackedCellCut cut = visibleBlockCut(terminal, block, packedCellCut(&bounds, session.state.mode, span, anchorRow, block.column, copyable));
        if (cut.first >= cut.end)
            continue;
        // A character selection whose end stands at the block's anchor cell
        // ends inside the block's text; any other goes on past it that way.
        const bool isCharacterSelection = session.state.mode == SelectionMode::Character;
        out[static_cast<std::size_t>(written++)] = {
            .serial = block.serial,
            .first = static_cast<std::uint32_t>(cut.first),
            .end = static_cast<std::uint32_t>(cut.end),
            .isContinuedBefore = !isCharacterSelection || bounds.row0 != anchorRow || bounds.column0 != block.column,
            .isContinuedAfter = !isCharacterSelection || bounds.row1 != anchorRow || bounds.column1 != block.column,
        };
    }
    return written;
}

namespace {

// Cuts each selected row's span around the rich text blocks on it, so the
// covered cells under a block take no cell highlight: the block draws its own
// on the glyphs. A second piece goes after the spans while there is room.
int splitAroundBlocks(const Terminal& terminal, int viewOffset, ViewportSpans out, int count)
{
    const ScreenMode screen = RichText::activeScreen(terminal);
    int written = count;
    for (int index = 0; index < count; ++index) {
        Rendering::SelectionRowSpan& span = out[static_cast<std::size_t>(index)];
        const int screenRow = span.visibleRow - viewOffset;
        for (const RichText::Block& block : terminal.richTextBlocks.blocks(screen)) {
            const int blockRight = block.column + block.width - 1;
            if (screenRow < block.row || screenRow >= block.row + static_cast<int>(block.height) || span.column1 < block.column || span.column0 > blockRight)
                continue;
            const bool hasLeft = span.column0 < block.column;
            const bool hasRight = span.column1 > blockRight;
            if (hasLeft && hasRight && static_cast<std::size_t>(written) < out.size())
                out[static_cast<std::size_t>(written++)] = { span.visibleRow, blockRight + 1, span.column1, 0, span.subColumn1 };
            if (hasLeft) {
                span.column1 = block.column - 1;
                span.subColumn1 = 0;
            } else if (hasRight) {
                span.column0 = blockRight + 1;
                span.subColumn0 = 0;
            } else {
                span.column1 = span.column0 - 1;
            }
        }
    }
    return written;
}

// One visible row's run of a sized block: the anchor cell and the continuation
// cells after it that carry the same spec. A block is such a run on each of
// `scale` consecutive rows, at the same columns, with bands 0 to scale-1 from
// the top.
struct SizedBlockRun {
    int firstColumn = 0;
    int lastColumn = 0;
    int band = 0;
    int scale = 1;
    unsigned specID = 0;
};

// The block run whose anchor is the cell at (row, column), or nothing where
// that cell is not the anchor of a block more than one row tall. A proportional
// run is not a block here: its letters are selected by the cell each begins in,
// and the cuts inside its cells are the selection's own; a scaled run's letters
// are each a block of their own (``addScaledRunLetterSpans``).
[[nodiscard]] std::optional<SizedBlockRun> sizedBlockRunAt(BoundContext& bound, int row, int column)
{
    const Cell* cell = boundCell(bound, row, column);
    if (!cell)
        return std::nullopt;
    const Pen& pen = cellPen(*cell);
    const unsigned specID = pen.extendedAttributes.textSizeSpecID();
    if (specID == 0 || !pen.attributes.contains(Ubiquitty::VT::Attribute::Wide))
        return std::nullopt;
    const TextSizeSpec* spec = bound.terminal->textSizeTable.lookup(specID);
    if (!spec || spec->isProportional || spec->scale < 2)
        return std::nullopt;
    SizedBlockRun run { .firstColumn = column, .lastColumn = column, .band = penTextBand(&pen), .scale = spec->scale, .specID = specID };
    if (run.band >= run.scale)
        return std::nullopt;
    for (int next = column + 1; next < bound.terminal->columns; ++next) {
        const Cell* following = boundCell(bound, row, next);
        if (!following)
            break;
        const Pen& followingPen = cellPen(*following);
        if (!followingPen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation) || followingPen.extendedAttributes.textSizeSpecID() != specID)
            break;
        run.lastColumn = next;
    }
    return run;
}

// Whether the block `run` belongs to has a cell inside the selection on any of
// the rows it is backed on. `row` is the run's own row.
[[nodiscard]] bool isSizedBlockSelected(BoundContext& bound, const SelectionBounds& bounds, SelectionMode mode, int row, const SizedBlockRun& run)
{
    const int top = row - run.band;
    for (int band = 0; band < run.scale; ++band) {
        const int bandRow = top + band;
        if (bandRow < bounds.row0 || bandRow > bounds.row1)
            continue;
        if (bandRow != row) {
            const std::optional<SizedBlockRun> backing = sizedBlockRunAt(bound, bandRow, run.firstColumn);
            if (!backing || backing->band != band || backing->specID != run.specID || backing->lastColumn != run.lastColumn)
                break;
        }
        const RowSpan span = rowSpan(&bounds, mode, bandRow, bound.terminal->columns, cellAt, &bound, attributesAt, &bound);
        if (span.selected && span.column1 >= span.column0 && span.column0 <= run.lastColumn && span.column1 >= run.firstColumn)
            return true;
    }
    return false;
}

// Lights the whole of every sized block the selection touches. A block is one
// glyph over `scale` rows, so a selection that holds any cell of it holds the
// block: its other band rows and its claimed columns take a highlight piece
// where the selection's own spans do not already cover them. The copy needs no
// such step: it reads a block's text once, at the first band row it holds.
//
// Only the cells a piece adds are new; the pieces are disjoint from the spans
// they join, so a translucent highlight is never laid over itself.
int addSizedBlockSpans(Terminal& terminal, const SelectionBounds& bounds, SelectionMode mode, int viewOffset, Session& session, ViewportSpans out, int count)
{
    if (terminal.textSizeTable.liveCount() == 0)
        return count;
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = viewOffset, .style = {} };
    const int firstAbsoluteRow = historyRows(terminal) - viewOffset;
    // A block reaches at most scale-1 rows past a row it touches the selection on.
    constexpr int maxBandReach = 6;
    const int firstRow = std::max(firstAbsoluteRow, bounds.row0 - maxBandReach);
    const int lastRow = std::min(firstAbsoluteRow + terminal.rows - 1, bounds.row1 + maxBandReach);
    int written = count;
    for (int row = firstRow; row <= lastRow; ++row) {
        const int visibleRow = row - firstAbsoluteRow;
        for (int column = 0; column < terminal.columns;) {
            const std::optional<SizedBlockRun> run = sizedBlockRunAt(bound, row, column);
            if (!run) {
                ++column;
                continue;
            }
            column = run->lastColumn + 1;
            if (!isSizedBlockSelected(bound, bounds, mode, row, *run))
                continue;
            // The block's columns on this row less what the row's spans hold.
            std::vector<std::pair<int, int>> held;
            for (int index = 0; index < written; ++index) {
                const Rendering::SelectionRowSpan& span = out[static_cast<std::size_t>(index)];
                if (span.visibleRow == visibleRow && span.column1 >= span.column0)
                    held.emplace_back(span.column0, span.column1);
            }
            std::ranges::sort(held);
            int uncoveredFrom = run->firstColumn;
            // A piece that continues the one just added on this row, as the next
            // block of a banner does, extends it: a row of adjacent blocks takes
            // one span and not one each, and the span buffer holds a row's worth
            // of pieces however many blocks stand side by side.
            const auto addPiece = [&](int first, int last) {
                if (first > last)
                    return;
                if (written > count) {
                    Rendering::SelectionRowSpan& previous = out[static_cast<std::size_t>(written - 1)];
                    if (previous.visibleRow == visibleRow && previous.column1 + 1 == first) {
                        previous.column1 = last;
                        return;
                    }
                }
                if (static_cast<std::size_t>(written) < out.size())
                    out[static_cast<std::size_t>(written++)] = { visibleRow, first, last, 0, 0 };
            };
            for (const auto& [heldFirst, heldLast] : held) {
                if (heldLast < uncoveredFrom)
                    continue;
                if (heldFirst > run->lastColumn)
                    break;
                addPiece(uncoveredFrom, heldFirst - 1);
                uncoveredFrom = std::max(uncoveredFrom, heldLast + 1);
            }
            addPiece(uncoveredFrom, run->lastColumn);
        }
    }
    if (written > count) {
        std::sort(out.begin(), out.begin() + written, [](const Rendering::SelectionRowSpan& left, const Rendering::SelectionRowSpan& right) {
            return left.visibleRow != right.visibleRow ? left.visibleRow < right.visibleRow : left.column0 < right.column0;
        });
    }
    return written;
}

// Lights the letters a selection holds of every scaled proportional run it
// reaches, on every band row of the run: a scaled letter is a glyph over all
// of them, so a selection holding it on one band holds it on each, as kitty
// lights a sized character whole. On each visible band row the run's columns
// are taken out of the spans there and given the same pieces, one for each
// stretch of held letters over the stretch's columns
// (``scaledRunLetterColumns``), cut where a stretch begins or ends between two
// letters of one cell; the fill a renderer draws over a run's letters reads
// those cuts. The cells before an aligned text and after the last letter's
// cell light where some band row's span reaches them, on every band row, so
// every band row lights the same cells whatever its own span reached.
int addScaledRunLetterSpans(Terminal& terminal, const SelectionBounds& bounds, SelectionMode mode, int viewOffset, Session& session, ViewportSpans out, int count)
{
    if (terminal.textSizeTable.liveCount() == 0)
        return count;
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = viewOffset, .style = {} };
    const int firstAbsoluteRow = historyRows(terminal) - viewOffset;
    constexpr int maxBandReach = 6;
    const int firstRow = std::max(firstAbsoluteRow, bounds.row0 - maxBandReach);
    const int lastRow = std::min(firstAbsoluteRow + terminal.rows - 1, bounds.row1 + maxBandReach);
    std::vector<std::pair<int, int>> visited;
    int written = count;
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Cell* cell = boundCell(bound, row, column);
            int topRow = 0;
            int anchorColumn = 0;
            if (!cell || cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)
                || !scaledRunOrigin(bound, row, column, *cell, topRow, anchorColumn))
                continue;
            if (std::ranges::find(visited, std::pair { topRow, anchorColumn }) != visited.end())
                continue;
            visited.emplace_back(topRow, anchorColumn);
            ScaledRunLetters letters;
            if (!scaledRunLettersAt(bound, bounds, mode, topRow, anchorColumn, letters) || !letters.isTouched)
                continue;
            const int letterCount = static_cast<int>(letters.text.size());
            const int lastClaimed = anchorColumn + letters.claimColumns - 1;
            const auto stretchStart = [&](int letter) {
                return static_cast<int>(std::ranges::lower_bound(letters.owners, letters.owners[static_cast<std::size_t>(letter)]) - letters.owners.begin());
            };
            const auto stretchEnd = [&](int letter) {
                return static_cast<int>(std::ranges::upper_bound(letters.owners, letters.owners[static_cast<std::size_t>(letter)]) - letters.owners.begin());
            };
            // The pieces, the same on every band row: one for each stretch of
            // held letters, from the first column of its first letter, cut
            // into its cell by the letters before, to the last column of its
            // last letter, cut by the letters after; and the cells before an
            // aligned text and after the last letter's cell where a span holds
            // them, joined to the stretch beside them.
            std::vector<Rendering::SelectionRowSpan> pieces;
            const int textFirst = anchorColumn + letters.owners.front();
            const int textLast = anchorColumn + letters.textColumns - 1;
            // Each piece names the letters it lights, so a renderer whose
            // layout puts a letter a cell from the measurer's lights it all the
            // same; one over the slack or the tail alone names none.
            if (letters.isSlackHeld)
                pieces.push_back({ 0, anchorColumn, textFirst - 1, 0, 0, 0, 0 });
            for (int letter = 0; letter < letterCount;) {
                if (!letters.isSelected[static_cast<std::size_t>(letter)]) {
                    ++letter;
                    continue;
                }
                int last = letter;
                while (last + 1 < letterCount && letters.isSelected[static_cast<std::size_t>(last + 1)])
                    ++last;
                const int startCut = letter - stretchStart(letter);
                // A stretch ending before the last letter of its cell is cut
                // at that cell, which the cut counts into.
                const int endCut = last + 1 == stretchEnd(last) ? 0 : last + 1 - stretchStart(last);
                const int lastColumn = endCut == 0 ? scaledRunLetterColumns(letters, last).second
                                                   : anchorColumn + letters.owners[static_cast<std::size_t>(last)] + letters.scale - 1;
                const int firstColumn = scaledRunLetterColumns(letters, letter).first;
                if (!pieces.empty() && startCut == 0 && pieces.back().subColumn1 == 0 && pieces.back().column1 + 1 == firstColumn) {
                    pieces.back().column1 = lastColumn;
                    pieces.back().subColumn1 = endCut;
                    if (pieces.back().endLetter <= pieces.back().firstLetter)
                        pieces.back().firstLetter = letter;
                    pieces.back().endLetter = last + 1;
                } else {
                    pieces.push_back({ 0, firstColumn, lastColumn, startCut, endCut, letter, last + 1 });
                }
                letter = last + 1;
            }
            if (letters.isTailHeld) {
                if (!pieces.empty() && pieces.back().subColumn1 == 0 && pieces.back().column1 == textLast)
                    pieces.back().column1 = lastClaimed;
                else
                    pieces.push_back({ 0, textLast + 1, lastClaimed, 0, 0, 0, 0 });
            }
            for (int band = 0; band < letters.scale; ++band) {
                const int bandRow = topRow + band;
                const int visibleRow = bandRow - firstAbsoluteRow;
                if (visibleRow < 0 || visibleRow >= terminal.rows)
                    continue;
                // The row's own spans, less the run's columns.
                for (int index = 0; index < written; ++index) {
                    Rendering::SelectionRowSpan& span = out[static_cast<std::size_t>(index)];
                    if (span.visibleRow != visibleRow || span.column1 < anchorColumn || span.column0 > lastClaimed)
                        continue;
                    const Rendering::SelectionRowSpan whole = span;
                    if (whole.column0 < anchorColumn) {
                        span.column1 = anchorColumn - 1;
                        span.subColumn1 = 0;
                        if (whole.column1 > lastClaimed && static_cast<std::size_t>(written) < out.size())
                            out[static_cast<std::size_t>(written++)] = { visibleRow, lastClaimed + 1, whole.column1, 0, whole.subColumn1 };
                    } else if (whole.column1 > lastClaimed) {
                        span.column0 = lastClaimed + 1;
                        span.subColumn0 = 0;
                    } else {
                        // Wholly inside the claim: emptied, the pieces replace it.
                        span.column1 = span.column0 - 1;
                    }
                }
                for (Rendering::SelectionRowSpan piece : pieces) {
                    piece.visibleRow = visibleRow;
                    if (static_cast<std::size_t>(written) < out.size())
                        out[static_cast<std::size_t>(written++)] = piece;
                }
            }
        }
    }
    if (written == count && visited.empty())
        return count;
    // The emptied spans go, and the rest are put back in reading order.
    const auto end = std::remove_if(out.begin(), out.begin() + written, [](const Rendering::SelectionRowSpan& span) { return span.column1 < span.column0; });
    written = static_cast<int>(end - out.begin());
    std::sort(out.begin(), out.begin() + written, [](const Rendering::SelectionRowSpan& left, const Rendering::SelectionRowSpan& right) {
        return left.visibleRow != right.visibleRow ? left.visibleRow < right.visibleRow : left.column0 < right.column0;
    });
    return written;
}

} // namespace

int viewportSpansAndEnds(Session& session, Terminal& terminal, int viewOffset, ViewportSpans out, ViewportEnds& ends) noexcept
{
    ends = {};
    synchronizeBufferRows(session, terminal);
    if (!Selection::hasSelection(&session.state))
        return 0;
    SelectionBounds bounds;
    if (!resolveBounds(session, terminal, bounds))
        return 0;
    BoundContext bound { .session = &session, .terminal = &terminal, .viewOffset = viewOffset, .style = {} };
    const Viewport viewport {
        .rows = terminal.rows,
        .columns = terminal.columns,
        .firstAbsoluteRow = historyRows(terminal) - viewOffset,
    };
    const int count = collectViewportSpans(bounds, session.state.mode, viewport, cellAt, &bound, attributesAt, &bound, out, rowLineAttributeAt, &bound);
    // The ends are read off the selection's own row spans, before anything is
    // cut out of them or added to them.
    for (int index = 0; index < count; ++index) {
        const Rendering::SelectionRowSpan& span = out[static_cast<std::size_t>(index)];
        if (!ends.isVisible || span.visibleRow < ends.startRow) {
            ends.startRow = span.visibleRow;
            ends.startColumn = span.column0;
        }
        if (!ends.isVisible || span.visibleRow >= ends.endRow) {
            ends.endRow = span.visibleRow;
            ends.endColumn = span.column1;
        }
        ends.isVisible = true;
    }
    const int splitCount = terminal.richTextBlocks.isEmpty() ? count : splitAroundBlocks(terminal, viewOffset, out, count);
    const int blockCount = addSizedBlockSpans(terminal, bounds, session.state.mode, viewOffset, session, out, splitCount);
    return addScaledRunLetterSpans(terminal, bounds, session.state.mode, viewOffset, session, out, blockCount);
}

int viewportSpans(Session& session, Terminal& terminal, int viewOffset, ViewportSpans out) noexcept
{
    ViewportEnds ends;
    return viewportSpansAndEnds(session, terminal, viewOffset, out, ends);
}

int viewportSpanCapacity(int rows, int columns) noexcept
{
    if (rows <= 0 || columns <= 0)
        return 0;
    return rows * (columns / 2 + 2);
}

SelectionState characterSelectionOverVisibleRows(const Terminal& terminal, const SelectionBounds& visible) noexcept
{
    // Through `absoluteRow`, so the alternate screen answers zero history the
    // way every other row conversion here does. A raw scrollback count would
    // place the rectangle below the viewport by the primary screen's surviving
    // history whenever the alternate one is showing.
    SelectionState state {};
    state.active = true;
    state.mode = SelectionMode::Character;
    state.anchorRow = absoluteRow(terminal, visible.row0, 0);
    state.anchorColumn = visible.column0;
    state.headRow = absoluteRow(terminal, visible.row1, 0);
    state.headColumn = visible.column1;
    return state;
}

namespace {

// The mark provider the output search reads, over the terminal it was handed.
std::uint8_t commandRowMarkAt(void* context, int absoluteRow) noexcept
{
    return Screen::rowMark(static_cast<const Terminal*>(context), absoluteRow);
}

} // namespace

bool isCellInTextVoid(
    const Terminal& terminal, int viewOffset, int visibleRow, int column, TextRestriction restriction) noexcept
{
    if (restriction != TextRestriction::ToText)
        return false;
    if (terminal.screens.isUsingAlternateScreen || Mouse::tracksPointer(&terminal))
        return false;
    BoundContext bound { .session = nullptr, .terminal = const_cast<Terminal*>(&terminal), .viewOffset = viewOffset, .style = {} };
    const int row = selectionRowOf(terminal, absoluteRow(terminal, visibleRow, viewOffset));
    // -1 for a row that printed nothing at all, which puts every column of
    // it past the text — a blank line is void all the way across. The column
    // is a screen cell, so a doubled row halves it first.
    const int scale = screenRowWidthScale(terminal, row);
    return column / scale > Selection::lastPrintedColumn(row, terminal.columns, cellAt, &bound, attributesAt, &bound);
}

CaretResolution resolveSelectionCaret(const Terminal& terminal, int viewOffset, int visibleRow, int caret) noexcept
{
    return resolveSelectionCaret(terminal, viewOffset, visibleRow, caret, std::numeric_limits<double>::quiet_NaN());
}

CaretResolution resolveSelectionCaret(
    const Terminal& terminal, int viewOffset, int visibleRow, int caret, double xInCells, double yInRows) noexcept
{
    if (!terminal.richTextBlocks.isEmpty()) {
        const int row = selectionRowOf(terminal, absoluteRow(terminal, visibleRow, viewOffset));
        const int screenRow = row - historyRows(terminal);
        const int column = std::isfinite(xInCells) ? static_cast<int>(std::floor(xInCells)) : caret;
        if (const RichText::Block* block = RichText::blockCovering(terminal, RichText::activeScreen(terminal), screenRow, column)) {
            const RichText::Layout& layout = RichText::layoutOf(terminal, *block);
            const double along = (std::isfinite(xInCells) ? xInCells : static_cast<double>(caret)) - block->column;
            const double down = (std::isfinite(yInRows) ? yInRows - visibleRow : 0.5) + (screenRow - block->row);
            const std::uint32_t character = layout.characterAt(static_cast<std::int64_t>(along * layout.cellWidth()), static_cast<std::int64_t>(down * layout.cellHeight()));
            const std::uint32_t copyable = layout.copyableEnd();
            const auto first = static_cast<int>(std::min(character, copyable));
            const auto after = static_cast<int>(std::min(character < copyable ? layout.graphemeEnd(character) : character, copyable));
            const int anchorRow = row - (screenRow - block->row);
            BoundContext bound { .session = nullptr, .terminal = const_cast<Terminal*>(&terminal), .viewOffset = viewOffset, .style = {} };
            const int lastPrinted = Selection::lastPrintedColumn(anchorRow, terminal.columns, cellAt, &bound, attributesAt, &bound);
            return CaretResolution {
                .absoluteRow = anchorRow,
                .caret = block->column,
                .slot = Selection::caretSlot(block->column, lastPrinted),
                .subCaret = first,
                .caretAfter = block->column,
                .subCaretAfter = after,
            };
        }
    }
    return resolveSelectionCaret(terminal, viewOffset, visibleRow, caret, xInCells);
}

CaretResolution resolveSelectionCaret(
    const Terminal& terminal, int viewOffset, int visibleRow, int caret, double xInCells) noexcept
{
    BoundContext bound { .session = nullptr, .terminal = const_cast<Terminal*>(&terminal), .viewOffset = viewOffset, .style = {} };
    const int row = selectionRowOf(terminal, absoluteRow(terminal, visibleRow, viewOffset));
    // The caret and the point arrive in screen cells. On a doubled row a
    // logical column spans two of them, so both halve, the point before it is
    // rounded: the boundary nearest the pointer is the one between logical
    // columns, not between screen cells.
    const int scale = screenRowWidthScale(terminal, row);
    if (scale > 1) {
        const int lastBoundary = std::max(0, terminal.columns / scale);
        if (std::isfinite(xInCells)) {
            xInCells /= scale;
            caret = static_cast<int>(std::floor(xInCells + 0.5));
        } else {
            caret = (caret + scale / 2) / scale;
        }
        caret = std::clamp(caret, 0, lastBoundary);
    }
    // A point inside a proportional run names a letter rather than a boundary,
    // and the run's own layout is what says which. Off a run — and for a
    // frontend that measured no point at all — the boundary handed in stands.
    int resolvedCaret = caret;
    int subCaret = 0;
    int caretAfter = caret;
    int subCaretAfter = 0;
    (void)runCaretsAtPoint(bound, row, xInCells, resolvedCaret, subCaret, caretAfter, subCaretAfter);
    // -1 for a row that printed nothing, which is what collapses a blank
    // line's whole width to the one slot at column 0.
    const int lastPrinted = Selection::lastPrintedColumn(row, terminal.columns, cellAt, &bound, attributesAt, &bound);
    return CaretResolution {
        .absoluteRow = row,
        .caret = resolvedCaret,
        .slot = Selection::caretSlot(resolvedCaret, lastPrinted),
        .subCaret = subCaret,
        .caretAfter = caretAfter,
        .subCaretAfter = subCaretAfter,
    };
}

bool hasCommandUnderRow(const Terminal& terminal, int absoluteRow) noexcept
{
    return Screen::resolveCommandRegion(&terminal, totalRows(terminal), absoluteRow).found;
}

bool selectCommandLineUnderRow(SelectionState& state, const Terminal& terminal, int absoluteRow) noexcept
{
    const int rows = totalRows(terminal);
    const Screen::CommandRegionRows region = Screen::resolveCommandRegion(&terminal, rows, absoluteRow);
    if (!region.found)
        return false;
    const int from = std::max(region.promptRow, 0);
    const int to = std::min(region.commandLastRow, rows - 1);
    if (to < from)
        return false;
    Selection::begin(&state, rows, terminal.columns, from, 0, SelectionMode::Character);
    Selection::update(&state, rows, terminal.columns, to, terminal.columns - 1);
    return true;
}

bool selectCommandOutputUnderRow(SelectionState& state, const Terminal& terminal, int absoluteRow) noexcept
{
    const int rows = totalRows(terminal);
    const Screen::CommandRegionRows region = Screen::resolveCommandRegion(&terminal, rows, absoluteRow);
    if (!region.found)
        return false;
    SelectionBounds bounds;
    if (!Selection::lastCommandOutputRegion(
            commandRowMarkAt, const_cast<Terminal*>(&terminal), rows, terminal.columns, region.outputStart, &bounds))
        return false;
    Selection::selectRegion(&state, rows, terminal.columns, bounds);
    return true;
}

bool selectActiveSearchMatch(SelectionState& state, const Terminal& terminal) noexcept
{
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(terminal);
    if (!active || terminal.columns <= 0)
        return false;
    // A match's segments are stored together, one per row it covers, first
    // row first.
    const Screen::ScrollSearch::Match* first = nullptr;
    const Screen::ScrollSearch::Match* last = nullptr;
    const int segments = Screen::ScrollSearch::segmentCount(terminal);
    for (int index = 0; index < segments; ++index) {
        const Screen::ScrollSearch::Match* segment = Screen::ScrollSearch::segmentAt(terminal, index);
        if (!segment || segment->matchID != active->matchID || segment->length <= 0)
            continue;
        if (!first)
            first = segment;
        last = segment;
    }
    if (!first)
        return false;
    // The history the search counts and the selection does not: the
    // primary's, behind the alternate screen.
    const int historyOutOfView = Screen::scrollbackCount(&terminal) - historyRows(terminal);
    const int rows = totalRows(terminal);
    const int firstRow = first->absoluteRow - historyOutOfView;
    const int lastRow = last->absoluteRow - historyOutOfView;
    if (firstRow < 0 || lastRow >= rows)
        return false;
    Selection::selectRegion(&state, rows, terminal.columns, { firstRow, first->column, lastRow, last->column + last->length - 1 });
    return true;
}

void selectBetweenPoints(Session& session, Terminal& terminal, double fromX, double fromY, double toX, double toY) noexcept
{
    const EndsNamedOnReturn naming(session, terminal);
    const auto caretAt = [&terminal](double x, double y) {
        const int row = std::clamp(static_cast<int>(std::floor(y)), 0, std::max(terminal.rows - 1, 0));
        const int boundary = std::clamp(static_cast<int>(std::floor(x + 0.5)), 0, terminal.columns);
        return resolveSelectionCaret(terminal, 0, row, boundary, x, y);
    };
    const int row = std::clamp(static_cast<int>(std::floor(fromY)), 0, std::max(terminal.rows - 1, 0));
    // A single click's open waits for the drag, so the press reports nothing
    // yet; the caret configuration below is what opens it.
    (void)beginGesture(session, terminal, row, std::clamp(static_cast<int>(std::floor(fromX)), 0, std::max(terminal.columns - 1, 0)), 1, 0, {}, fromX);
    configureCharacterCaretGesture(session, terminal, caretAt(fromX, fromY));
    (void)dragCharacterCaretGesture(session, terminal, caretAt(toX, toY), 0);
    (void)endGesture(session);
}

} // namespace Ubiquitty::Selection
