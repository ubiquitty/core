// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Screen/Scroll.cpp — Scrolling the screen and its regions.
//
// scrollRect* move whole rows inside a rectangle; the region/full-screen
// entry points sit on top. The scroll LANE is the deferred-state fast path for
// a flood: per-run bookkeeping accumulates and commits once per processBytes
// rather than per line, so a scrolling flood does not pay the guard dust per line.
//
// Cold relative to the per-byte parser loop, which stays in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/RichText/Blocks.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Internal.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/Triggers/Engine.h"
#include <algorithm>

#include <cstdlib>
#include <cstring>

namespace Ubiquitty::Screen {

// Per-rectangle scroll for DECSLRM mode: shifts cells inside
// [top..bot] × [left..right] by |n| rows (n > 0 = content moves up). The normal
// CellGrid::scrollRegion fast path assumes full-row scrolls and would corrupt
// cells outside the rectangle.

namespace {

// A partial-width scroll moves a column band through rows whose cells outside
// the band stay put, so a run straddling either band edge keeps part of its
// chain while the rest moves — on the source row, on the destination row, or
// both. The copy loops cannot see that; this pass can: re-read each touched
// row whole, blank what the anchor's claim no longer backs, and write back
// the span the scrub reached.
void scrubScrolledRows(Ubiquitty::Terminal& t, int top, int bot, const Ubiquitty::Cell& blank)
{
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    for (int row = top; row <= bot; row++) {
        // Only a Wide lead or a WideContinuation gives the scrub anything to do, and RowFlag::HasMulticell
        // is latched by every write of one and never stale-false. Without it the row walk cannot reach a
        // blanking branch, so reading the row whole to prove that is the whole cost on ASCII content.
        if (!t.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell))
            continue;
        t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
        int firstTouched = t.columns;
        int lastTouched = -1;
        Ubiquitty::Screen::scrubMulticellOrphans(rowBuffer, t.columns, blank, &t.textSizeTable, t.ambiguousWidth, firstTouched, lastTouched);
        if (lastTouched < firstTouched)
            continue;
        Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
        t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
    }
}

// A rectangle scroll bumps every row of the band as it moves it, so the damage
// is already recorded row by row, and the columns outside the band still match
// the shadow they were painted from. Saying the whole screen on top of that
// repaints the panes beside this one on every line they did not print.
//
// A queued blit is the exception: it describes a shift the PAL has not
// performed yet, and moving cells under it invalidates its premise. Dropping it
// loses the rows it stood for, so those are said the wide way.
void noteRectangleScrollDamage(Ubiquitty::Terminal& t)
{
    if (t.grid->hasPendingScroll())
        Ubiquitty::Screen::markAllDirty(t);
}

} // namespace

// Defined with the band lane further down; scrollRegionUp's margined branch
// calls it first. Outside the helper namespace above so declaration and
// definition share one linkage.
static bool bandLaneDeferOrFlush(Ubiquitty::Terminal& t, int top, int bot, int n);

// Out of line on purpose: inlined into scrollRegionUp, its branches would join
// that hot function's control flow and detach the trained profile.
[[gnu::noinline]] void noteInPlaceContentShift(Ubiquitty::Terminal& t, int top, int bottom, int rows, ShiftWidth width, int left, int right)
{
    if (rows == 0 || bottom < top)
        return;
    Ubiquitty::RichText::noteRowsShifted(t, top, bottom, rows);
    noteAreaRowsShifted(t, top, bottom, rows);
    if (hasActivePins(t)) [[unlikely]] {
        if (width == ShiftWidth::FullRows)
            notePositionsShiftedInPlace(t, top, bottom, rows);
        else
            notePositionsShiftedInBand(t, top, bottom, left, right, rows);
    }
}

void scrollRectUp(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n)
{
    if (!t.grid)
        return;
    if (t.bandScrollRun.active)
        bandLaneFlushActive(t);
    Ubiquitty::Screen::State* s = t.screens.active;
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    int height = bot - top + 1;
    if (n > height)
        n = height;
    const bool isFullWidth = left == 0 && right == t.columns - 1;
    noteInPlaceContentShift(t, top, bot, n, isFullWidth ? ShiftWidth::FullRows : ShiftWidth::Rectangle, left, right);
    // Full row width: route through CellGrid::scrollRegion so RowMeta (OSC 133
    // marks, soft-wrap join, needsShaping, line attribute) rotates with the
    // cells instead of staying glued to the wrong row.
    if (isFullWidth) {
        t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), n, blank);
        return;
    }
    // Partial-width rect: per-row metadata can't be split along a column
    // boundary, so a horizontal-margined scroll leaves each row's mark / line
    // attribute / wrap join in place; the move touches cells only.
    t.grid->scrollRectangleCells(top, bot, left, right, n, blank, Ubiquitty::RectangleScrollDirection::Up);
    scrubScrolledRows(t, top, bot, blank);
    scrubBlockDamage(t, top, bot);
    noteRectangleScrollDamage(t);
}

void scrollRectDown(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n)
{
    if (!t.grid)
        return;
    if (t.bandScrollRun.active)
        bandLaneFlushActive(t);
    Ubiquitty::Screen::State* s = t.screens.active;
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    int height = bot - top + 1;
    if (n > height)
        n = height;
    const bool isFullWidth = left == 0 && right == t.columns - 1;
    noteInPlaceContentShift(t, top, bot, -n, isFullWidth ? ShiftWidth::FullRows : ShiftWidth::Rectangle, left, right);
    // Full row width: route through CellGrid::scrollRegion so RowMeta rotates
    // with the cells. n negated because scrollRegion treats positive n as
    // content moving up. Partial-width policy as in scrollRectUp.
    if (isFullWidth) {
        t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), -n, blank);
        return;
    }
    // Partial-width rect: keep each row's metadata in place; the move touches
    // cells only.
    t.grid->scrollRectangleCells(top, bot, left, right, n, blank, Ubiquitty::RectangleScrollDirection::Down);
    scrubScrolledRows(t, top, bot, blank);
    scrubBlockDamage(t, top, bot);
    noteRectangleScrollDamage(t);
}

// A line feed is confined to the horizontal margins, and a cursor outside them
// has no line to feed: with DECLRMM active and the cursor outside
// [scrollLeft, scrollRight] the whole feed is dropped — no scroll at all, not
// even of the margin rectangle. SU/SD are unconditional and enter
// through scrollRegionUp/scrollRegionDown instead.
static bool indexScrollBlocked(const Ubiquitty::Terminal& t)
{
    if (!t.modes.leftRightMarginMode)
        return false;
    const Ubiquitty::Screen::State* s = t.screens.active;
    return s->cursorX < s->scrollLeft || s->cursorX > s->scrollRight;
}

// noinline: keeps the LineStore init/grow arms, the image-placement shift and
// the generic scrollRegion branch out of linefeed's per-line path.
[[gnu::noinline]] void scrollRegionUp(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int top = s->scrollTop, bot = s->scrollBottom, r;
    bool capture;
    bool tracksMarks;
    size_t rowBytes;
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    // DECSLRM active: take the cell-by-cell rectangle path (preserves cells
    // outside the rect). No scrollback capture — partial-row content isn't
    // terminal scrollback by DEC's model.
    if (isRowNarrowedByMargins(t, s)) {
        if (bandLaneDeferOrFlush(t, top, bot, n))
            return;
        scrollRectUp(t, top, bot, s->scrollLeft, s->scrollRight, n);
        return;
    }
    if (t.bandScrollRun.active)
        bandLaneFlushActive(t); // the margins were released mid-run
    Diagnostics::diagLog(t, "core-scroll-up", static_cast<uint32_t>(n));
    capture = (s == &t.screens.primary && top == 0 && bot == t.rows - 1);
    // A captured scroll appends to history, and absolute rows keep their
    // numbers through it; every other scroll moves content in place. The test
    // is here rather than inside the callee because this is the hot path of
    // steady output and `capture` is already computed.
    if (!capture)
        noteInPlaceContentShift(t, top, bot, n, ShiftWidth::FullRows);
    // OSC 133 markers exist only for the primary screen; alternate-screen
    // scrolls must not perturb the primary's live marker row.
    tracksMarks = (s == &t.screens.primary);
    if (capture && t.grid) {
        // Append straight from the live row pointer (not recycled until
        // scrollRegion below). Copy only the row's live content width — cells
        // past it are default blank, which visibleCell reconstructs on read.
        // A soft-wrapped line keeps full width: its join needs the run to the margin.
        for (r = 0; r < n; r++) {
            scrollbackAppendGridRow(t, top + r);
        }
    }
    rowBytes = static_cast<size_t>(t.columns) * sizeof(Ubiquitty::Cell);
    (void)rowBytes;
    // Images flow with the text: shift placements up on a full-screen scroll,
    // dropping any scrolled past the top.
    if (tracksMarks && top == 0 && bot == t.rows - 1 && t.images.livePlacementCount != 0)
        Ubiquitty::Image::scrollPlacementsUp(t, n);
    // Blocks ride a scroll that feeds the scrollback; an in-place one was
    // followed by noteInPlaceContentShift above.
    if (capture && !t.richTextBlocks.isEmpty())
        Ubiquitty::RichText::scrollBlocksIntoHistory(t, n);
    if (capture && !t.qualifiedAreas.isEmpty())
        scrollAreasIntoHistory(t, n);
    // CellGrid::scrollRegion rotates m_rowMeta (line attribute / lineWrapped
    // views) with the cells and resets revealed rows to NORMAL. The boundary row
    // (bot - n) loses its wrap join as its neighbour scrolled away; clear it
    // after the rotation so the line-continuation invariant holds.
    {
        Ubiquitty::Cell blank;
        const Ubiquitty::Cell* scrollBlank = &blank;
        if (Ubiquitty::Screen::eraseBlankIsDefault(s))
            scrollBlank = &Ubiquitty::defaultBlankCell;
        else
            Ubiquitty::Screen::eraseCell(&blank, s);
        if (t.grid) {
            if (top == 0 && bot == t.rows - 1 && n == 1) {
                if (scrollBlank == &Ubiquitty::defaultBlankCell)
                    t.grid->scrollFullViewportUpOneDefaultBlank();
                else
                    t.grid->scrollFullViewportUpOne(*scrollBlank);
            } else
                t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), n, *scrollBlank);
        }
    }
    if (bot - n >= top)
        s->lineWrapped[bot - n] = false;
}

// Line-feed scroll: IND/LF/NEL/VT/FF and the frontend's "as if n line feeds"
// viewport feed.
void scrollUp(Ubiquitty::Terminal& t, int n)
{
    if (indexScrollBlocked(t))
        return;
    scrollRegionUp(t, n);
}

// Deferred-state scroll lane. Eligible only when linefeed()'s at-bottom branch
// reduces to: capture the top row, rotate the viewport, account the scroll
// (amortizable per run). Triggers and auto-print observe completed lines, so
// with either armed the classic path runs instead.
bool scrollLaneEligible(const Ubiquitty::Terminal& t)
{
    if (!t.isScrollLaneEnabled || !t.grid)
        return false;
    const Ubiquitty::Screen::State* s = t.screens.active;
    if (s != &t.screens.primary)
        return false;
    if (s->scrollTop != 0 || s->scrollBottom != t.rows - 1)
        return false;
    if (isRowNarrowedByMargins(t, s))
        return false;
    if (s->cursorY != s->scrollBottom)
        return false;
    if (t.triggersEngine && t.triggersEngine->ruleCount() != 0)
        return false;
    if (t.autoPrint && t.printSinkFile)
        return false;
    return true;
}

void scrollLaneLinefeed(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    s->wrapPending = false;
    // Append the departing top row directly from its current representation.
    // The history count stays exact mid-run; only paint-side accounting defers.
    scrollbackAppendGridRow(t, 0);
    if (!t.scrollLaneRun.active) {
        // Run open: build the BCE blank once (the pen is frozen for the run's
        // lifetime; any SGR flushes and closes it). With default fg/bg the
        // CANONICAL default blank is used so inert pen decoration bytes never
        // make it byte-distinct — capture width and the deferred-blank path both
        // key on byte equality with defaultBlankCell.
        if (Ubiquitty::Screen::eraseBlankIsDefault(s))
            t.scrollLaneRun.blank = Ubiquitty::defaultBlankCell;
        else
            Ubiquitty::Screen::eraseCell(&t.scrollLaneRun.blank, s);
        t.scrollLaneRun.active = true;
    }
    t.grid->scrollLaneRotateOnly(t.scrollLaneRun.blank);
    ++t.scrollLaneRun.scrolledLines;
}

void scrollLaneFlushActive(Ubiquitty::Terminal& t)
{
    const int scrolledLines = t.scrollLaneRun.scrolledLines;
    t.scrollLaneRun.active = false;
    t.scrollLaneRun.scrolledLines = 0;
    t.grid->scrollLaneCommit(scrolledLines);
    // Placements shift once by the accumulated count; interleaved classic
    // scrolls shift incrementally themselves and the two compose additively.
    if (t.images.livePlacementCount != 0)
        Ubiquitty::Image::scrollPlacementsUp(t, scrolledLines);
    if (!t.richTextBlocks.isEmpty())
        Ubiquitty::RichText::scrollBlocksIntoHistory(t, scrolledLines);
    if (!t.qualifiedAreas.isEmpty())
        scrollAreasIntoHistory(t, scrolledLines);
    Diagnostics::diagLog(t, "core-scroll-lane", static_cast<uint32_t>(scrolledLines));
}

// Blank-invariant SGR continuation: a plain SGR mutates only the pen, so the
// run survives it UNLESS the new pen's BCE blank differs from the run's frozen
// blank (capture width and the deferred-blank reveal both key on that value).
// fg-only churn — log accents, syntax spans, emoji palettes — keeps runs open.
// Deferred band scrolls: the margined sibling of the scroll lane. Same
// lifetime and flush sites; eligibility differs where the shapes differ. The
// alternate screen is eligible -- band scrolls never feed scrollback, and tmux
// runs its panes there, which is the workload this lane exists for.
static bool bandLaneEligibleShape(const Ubiquitty::Terminal& t, const Ubiquitty::Screen::State* s, int top, int bot, int n)
{
    if (!t.isBandScrollLaneEnabled || !t.grid)
        return false;
    if (n != 1 || s->cursorY != bot || top != s->scrollTop)
        return false;
    if (s->cursorX < s->scrollLeft || s->cursorX > s->scrollRight)
        return false;
    if (t.triggersEngine && t.triggersEngine->ruleCount() != 0)
        return false;
    if (t.autoPrint && t.printSinkFile)
        return false;
    return true;
}

void bandLaneFlushActive(Ubiquitty::Terminal& t)
{
    auto& run = t.bandScrollRun;
    const int scrolledLines = run.scrolledLines;
    run.active = false;
    run.scrolledLines = 0;
    if (scrolledLines <= 0 || !t.grid)
        return;
    t.grid->bandLaneCommit(run.top, run.bot, run.left, run.right, scrolledLines, run.blank);
    scrubScrolledRows(t, run.top, run.bot, run.blank);
    scrubBlockDamage(t, run.top, run.bot);
    noteRectangleScrollDamage(t);
    Diagnostics::diagLog(t, "core-band-lane", static_cast<uint32_t>(scrolledLines));
}

// Defers one margined scroll into the band lane, or reports that the classic
// rectangle path must run. A run whose geometry, blank or pool no longer holds
// flushes first; a shape this lane does not own flushes and declines.
static bool bandLaneDeferOrFlush(Ubiquitty::Terminal& t, int top, int bot, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    auto& run = t.bandScrollRun;
    // A pinned line follows the band by its cells, which a deferred run leaves
    // where they were until the commit; a terminal holding positions moves
    // each band at once. Runs never span a capture: one commits before
    // processBytes returns.
    if (hasActivePins(t)) [[unlikely]] {
        if (run.active)
            bandLaneFlushActive(t);
        return false;
    }
    const bool shapeHolds = bandLaneEligibleShape(t, s, top, bot, n);
    if (run.active) {
        Ubiquitty::Cell blank;
        const Ubiquitty::Cell* next = &blank;
        if (Ubiquitty::Screen::eraseBlankIsDefault(s))
            next = &Ubiquitty::defaultBlankCell;
        else
            Ubiquitty::Screen::eraseCell(&blank, s);
        const bool runHolds = shapeHolds && run.top == top && run.bot == bot
            && run.left == s->scrollLeft && run.right == s->scrollRight
            && std::memcmp(next, &run.blank, sizeof(Ubiquitty::Cell)) == 0
            && run.scrolledLines < t.grid->bandLanePoolCapacity();
        if (!runHolds)
            bandLaneFlushActive(t);
    }
    if (!shapeHolds)
        return false;
    if (!run.active) {
        if (t.grid->bandLanePoolCapacity() < 1)
            return false;
        run.top = top;
        run.bot = bot;
        run.left = s->scrollLeft;
        run.right = s->scrollRight;
        if (Ubiquitty::Screen::eraseBlankIsDefault(s))
            run.blank = Ubiquitty::defaultBlankCell;
        else
            Ubiquitty::Screen::eraseCell(&run.blank, s);
        run.scrolledLines = 0;
        run.active = true;
    }
    t.grid->bandLaneDefer(bot, run.left, run.right, run.blank);
    ++run.scrolledLines;
    // The deferred row is content moved in place; recorded here, in a block
    // that already exists, rather than in the caller's return branch.
    noteInPlaceContentShift(t, top, bot, n, ShiftWidth::Rectangle, run.left, run.right);
    return true;
}

void scrollLaneFlushIfBlankChanged(Ubiquitty::Terminal& t)
{
    if (t.bandScrollRun.active) {
        const Ubiquitty::Screen::State* s = t.screens.active;
        Ubiquitty::Cell bandBlank;
        const Ubiquitty::Cell* bandNext = &bandBlank;
        if (Ubiquitty::Screen::eraseBlankIsDefault(s))
            bandNext = &Ubiquitty::defaultBlankCell;
        else
            Ubiquitty::Screen::eraseCell(&bandBlank, s);
        if (std::memcmp(bandNext, &t.bandScrollRun.blank, sizeof(Ubiquitty::Cell)) != 0)
            bandLaneFlushActive(t);
    }
    if (!t.scrollLaneRun.active)
        return;
    const Ubiquitty::Screen::State* s = t.screens.active;
    Ubiquitty::Cell blank;
    const Ubiquitty::Cell* next = &blank;
    if (Ubiquitty::Screen::eraseBlankIsDefault(s))
        next = &Ubiquitty::defaultBlankCell;
    else
        Ubiquitty::Screen::eraseCell(&blank, s);
    if (std::memcmp(next, &t.scrollLaneRun.blank, sizeof(Ubiquitty::Cell)) != 0)
        scrollLaneFlushActive(t);
}

void scrollRegionDown(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int top = s->scrollTop, bot = s->scrollBottom;
    size_t rowBytes;
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    if (isRowNarrowedByMargins(t, s)) {
        scrollRectDown(t, top, bot, s->scrollLeft, s->scrollRight, n);
        return;
    }
    noteInPlaceContentShift(t, top, bot, -n, ShiftWidth::FullRows);
    Diagnostics::diagLog(t, "core-scroll-down", static_cast<uint32_t>(n));
    rowBytes = static_cast<size_t>(t.columns) * sizeof(Ubiquitty::Cell);
    (void)rowBytes;
    // CellGrid::scrollRegion rotates m_rowMeta (line attribute / lineWrapped
    // views) down with the cells and resets revealed top rows to NORMAL.
    {
        Ubiquitty::Cell blank;
        Ubiquitty::Screen::eraseCell(&blank, s);
        if (t.grid)
            t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), -n, blank);
    }
}

// Reverse-index scroll (RI). Same margin rule as the line-feed direction.
void scrollDown(Ubiquitty::Terminal& t, int n)
{
    if (indexScrollBlocked(t))
        return;
    scrollRegionDown(t, n);
}

void scrollRegionHorizontal(Ubiquitty::Terminal& t, int n, HorizontalScrollDirection direction)
{
    if (!t.grid || n <= 0)
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    int top = s->scrollTop, bot = s->scrollBottom;
    int left = 0, right = t.columns - 1;
    if (t.modes.leftRightMarginMode) {
        left = s->scrollLeft;
        right = s->scrollRight;
    }
    // A cursor outside the scrolling margins leaves the region alone, as it
    // does for IL and DL (DEC STD 070, and xterm).
    if (s->cursorY < top || s->cursorY > bot || s->cursorX < left || s->cursorX > right)
        return;
    if (n > right - left + 1)
        n = right - left + 1;
    if (hasActivePins(t)) [[unlikely]]
        notePositionsRewritten(t, top, bot);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    // Row-wise, as the rectangle scrolls and the row shifts are: the band
    // arrives whole from one sequential read, moves inside the buffer, and goes
    // back as one span. Per cell it was two slot resolutions, an idempotency
    // probe and the row's blink / shaping / multicell bookkeeping, repeated for
    // every column the band holds and every line an app scrolls.
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    const int bandWidth = right - left + 1;
    const int kept = bandWidth - n;
    for (int row = top; row <= bot; row++) {
        t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
        Ubiquitty::Cell* band = rowBuffer + left;
        if (direction == HorizontalScrollDirection::Right) {
            // SR: cells move toward higher columns; blank the left n. Backward,
            // because the destination overlaps the source from above.
            std::copy_backward(band, band + kept, band + bandWidth);
            std::fill(band, band + n, blank);
        } else {
            // SL: cells move toward lower columns; blank the right n.
            std::copy(band + n, band + bandWidth, band);
            std::fill(band + kept, band + bandWidth, blank);
        }

        // The row's live width follows content, and a move neither adds nor
        // removes any: what the band holds afterwards is what it held before,
        // in different columns. Writing it back has to leave that width where
        // a per-cell write would, and the two bulk paths each move it — a span
        // write claims every column it covers, and a default-blank erase
        // reaching the row's last column shrinks the width to its start.
        //
        // So: content up to the band's last cell that is not a default blank
        // goes back as a span, and the default-blank tail as an erase, with the
        // row's last column written on its own through the per-cell path whose
        // rule this preserves. A styled blank is content and stays in the span.
        int spanLast = right;
        while (spanLast >= left
            && std::memcmp(&rowBuffer[spanLast], &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) == 0)
            --spanLast;
        if (spanLast >= left) {
            Ubiquitty::RowView view = { band, spanLast - left + 1 };
            t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(left), view);
        }
        int eraseLast = right;
        if (eraseLast >= t.columns - 1 && spanLast < eraseLast) {
            t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(t.columns - 1), Ubiquitty::defaultBlankCell);
            eraseLast = t.columns - 2;
        }
        if (spanLast < eraseLast) {
            t.grid->eraseRowRange(Ubiquitty::Row(row), Ubiquitty::Column(spanLast + 1),
                Ubiquitty::Column(eraseLast), Ubiquitty::defaultBlankCell);
        }
    }
    // The columns move independently, so a run straddling either edge of the
    // shifted band keeps the cells that landed inside and loses the rest —
    // the same tear the row shifts and the rect scrolls repair, on the same
    // two passes.
    scrubScrolledRows(t, top, bot, blank);
    scrubBlockDamage(t, top, bot);
    noteRectangleScrollDamage(t);
}

void scrollViewportUp(Ubiquitty::Terminal* t, int n)
{
    if (t && n > 0)
        scrollUp(*t, n);
}

} // namespace Ubiquitty::Screen
