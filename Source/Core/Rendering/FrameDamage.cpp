// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/FrameDamage.h"

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/PaintRun.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/PendingScroll.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Log.h"

#include <algorithm>
#include <cstring>

namespace Ubiquitty::Rendering {

bool anyCellBlinks(const Cell* cells, int rows, int columns)
{
    if (!cells || rows <= 0 || columns <= 0)
        return false;
    const std::size_t total = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns);
    for (std::size_t i = 0; i < total; ++i) {
        if (cellPen(cells[i]).attributes.contains(Ubiquitty::VT::Attribute::Blink))
            return true;
    }
    return false;
}

bool anyCellBlinks(const Terminal& terminal)
{
    // Three callers a frame ask this — the capture, the snapshot and the
    // animation clock — of content none of them changed, and the answer is a
    // whole-screen read. The grid keeps it until a write moves its content
    // generation, so a settled screen is scanned once rather than per frame per
    // caller.
    const BlinkReading standing = terminal.grid ? terminal.grid->blinkReading() : BlinkReading::Unread;
    if (standing != BlinkReading::Unread)
        return standing == BlinkReading::ACellBlinks;
    const int history = Ubiquitty::Screen::scrollbackCount(&terminal);
    bool isBlinkFound = false;
    for (int row = 0; row < terminal.rows && !isBlinkFound; ++row) {
        int length = 0;
        const Cell* cells = Ubiquitty::Screen::absoluteRowCells(&terminal, history + row, &length);
        isBlinkFound = anyCellBlinks(cells, 1, length);
    }
    if (terminal.grid)
        terminal.grid->recordBlinkReading(isBlinkFound ? BlinkReading::ACellBlinks : BlinkReading::NoCellBlinks);
    return isBlinkFound;
}

bool markBlinkRowsDirty(const Terminal& terminal, std::span<std::uint8_t> rowDirty)
{
    // A standing reading of false is the whole answer: no row carries blink, so
    // no row is marked. A standing true still needs the walk, since it says
    // that some row does and not which.
    if (terminal.grid && terminal.grid->blinkReading() == BlinkReading::NoCellBlinks)
        return false;
    const int history = Ubiquitty::Screen::scrollbackCount(&terminal);
    const int rows = std::min(terminal.rows, static_cast<int>(rowDirty.size()));
    bool isBlinkRowFound = false;
    for (int row = 0; row < rows; ++row) {
        int length = 0;
        const Cell* cells = Ubiquitty::Screen::absoluteRowCells(&terminal, history + row, &length);
        if (anyCellBlinks(cells, 1, length)) {
            rowDirty[static_cast<std::size_t>(row)] = 1;
            isBlinkRowFound = true;
        }
    }
    // Only a walk that reached every row describes the whole screen. A span
    // shorter than the grid leaves rows unread, and recording their absence as
    // an answer is the stale-FALSE the hint forbids.
    if (terminal.grid && rows == terminal.rows)
        terminal.grid->recordBlinkReading(isBlinkRowFound ? BlinkReading::ACellBlinks : BlinkReading::NoCellBlinks);
    return isBlinkRowFound;
}

FrameDamage drainFrameDamage(CellGrid& grid, const CaptureEligibility& eligibility, std::span<std::uint8_t> rowDirty)
{
    FrameDamage damage;
    if (rowDirty.empty())
        return damage;
    const int rows = static_cast<int>(rowDirty.size());

    damage.canReuse = captureCanReuse(eligibility);
    // UBQ_REUSEWHY: which term refused the damage set. A frontend can reach
    // only part of the eligibility, and on a plain appending stream every
    // part it reaches passes while reuse still comes back false. The refusing
    // term is one of these, and there is no way to tell which from outside
    // this function.
    static const bool isRefusalExplained = std::getenv("UBQ_REUSEWHY") != nullptr;
    if (isRefusalExplained && !damage.canReuse) {
        PAL::logLine(stderr,
            "[reusewhy] trust={} init={} gen={}/{} view={}/{} rows={}/{} cols={}/{} rev={}/{} pal={}/{} img={}/{}",
            eligibility.trustDamage, eligibility.bufferInitialized,
            eligibility.generationBeforeDrain, eligibility.bufferGeneration,
            eligibility.currentViewOffset, eligibility.lastViewOffset,
            eligibility.currentRows, eligibility.lastRows,
            eligibility.currentColumns, eligibility.lastColumns,
            eligibility.currentReverseVideo, eligibility.lastReverseVideo,
            eligibility.currentPaletteEpoch, eligibility.lastPaletteEpoch,
            eligibility.hasImagesOnViewport, eligibility.hadImagesOnViewport);
    }
    std::ranges::fill(rowDirty, std::uint8_t { 0 });

    {
        auto session = grid.beginPaint();
        Screen::PendingScroll pendingScroll;
        if (session.nextPendingScroll(&pendingScroll)) {
            const int top = pendingScroll.topRow.value();
            const int bottom = pendingScroll.botRow.value();
            if (top >= 0 && top <= bottom && bottom < rows) {
                damage.scrollTop = top;
                damage.scrollBottom = bottom;
                damage.scrollDelta = pendingScroll.shiftRows;
                // The exposed band is marked here, before the runs are read.
                // Scroll coalescing (or a snapshot re-encode) can drop the
                // incremental runs for the rows the shift uncovered, and a
                // blit that cleared them then repaints nothing — the top of
                // the screen keeps the previous frame's lines.
                const Screen::ScrollExposedRows exposed = Screen::scrollExposedRows(top, bottom, pendingScroll.shiftRows);
                for (int row = exposed.firstRow; row <= exposed.lastRow; ++row)
                    rowDirty[row] = 1;
            }
            session.ackScroll(pendingScroll);
        }
        PaintRun run;
        // Row-granular: this drain wants which rows changed, and the run walk
        // splits a row at every pen change to answer a question about glyphs
        // that nobody here asks.
        Row dirtyRow { 0 };
        while (session.nextDirtyRow(&dirtyRow)) {
            const int row = dirtyRow.value();
            if (row >= 0 && row < rows)
                rowDirty[row] = 1;
            session.ackRow(dirtyRow);
        }
        damage.hasBlink = grid.hasBlinkHint();
    }

    damage.generationAfterDrain = grid.currentDamageGeneration();
    return damage;
}

namespace {

/// The focused search match's run on the rows a frame shows.
struct FocusedMatchRun {
    int row0 = 0;
    int row1 = -1; // row0 > row1: no focused match on screen
    int column0 = 0;
    int column1 = -1;
};

/// Reads the run the builder bakes the focused match's fill into, from the
/// terminal's own search: the segments of the active match that reach the
/// visible rows, first row's first column to last row's last column.
FocusedMatchRun focusedMatchRun(const Terminal& terminal, int viewOffset)
{
    FocusedMatchRun run;
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(terminal);
    if (!active || terminal.rows <= 0)
        return run;
    const int history = Screen::scrollbackCount(&terminal);
    const int resolvedOffset = Screen::resolvedViewOffset(&terminal, viewOffset);
    const int count = Screen::ScrollSearch::segmentCount(terminal);
    for (int index = 0; index < count; ++index) {
        const Screen::ScrollSearch::Match* segment = Screen::ScrollSearch::segmentAt(terminal, index);
        if (!segment || segment->matchID != active->matchID || segment->length <= 0)
            continue;
        const int row = Screen::visibleRowForAbsolute(history, resolvedOffset, segment->absoluteRow);
        if (row < 0 || row >= terminal.rows)
            continue;
        const int lastColumn = segment->column + segment->length - 1;
        if (run.row0 > run.row1) {
            run = { row, row, segment->column, lastColumn };
            continue;
        }
        if (row < run.row0 || (row == run.row0 && segment->column < run.column0)) {
            run.row0 = row;
            run.column0 = segment->column;
        }
        if (row > run.row1 || (row == run.row1 && lastColumn > run.column1)) {
            run.row1 = row;
            run.column1 = lastColumn;
        }
    }
    return run;
}

} // namespace

RetainedCapture planRetainedCapture(CellGrid& grid, const Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedCaptureRequest& request, std::span<std::uint8_t> rowDirty)
{
    RetainedCapture capture;
    if (rowDirty.empty() || request.rows != static_cast<int>(rowDirty.size()))
        return capture;

    CaptureEligibility eligibility;
    eligibility.trustDamage = request.shouldTrustDamage && request.isGridCacheEnabled;
    eligibility.bufferInitialized = baseline.isInitialized;
    eligibility.isSameGrid = baseline.grid == &grid;
    eligibility.bufferGeneration = baseline.generation;
    eligibility.generationBeforeDrain = grid.currentDamageGeneration();
    eligibility.lastViewOffset = baseline.viewOffset;
    eligibility.currentViewOffset = request.viewOffset;
    eligibility.lastRows = baseline.rows;
    eligibility.lastColumns = baseline.columns;
    eligibility.currentRows = request.rows;
    eligibility.currentColumns = request.columns;
    eligibility.lastReverseVideo = baseline.isReverseVideo;
    const bool isFrameReverseVideo = isReverseVideo(&terminal);
    eligibility.currentReverseVideo = isFrameReverseVideo;
    eligibility.lastPaletteEpoch = baseline.paletteEpoch;
    eligibility.currentPaletteEpoch = terminal.paletteEpoch;
    // A picture reaches the retained pixels outside the row damage, so the
    // frame that shows one rebuilds whole, and so does the frame after it
    // left: the rows it covered were never written, and a Load would keep it
    // standing there while the text moved on.
    const bool hasImagesOnViewport = Image::hasPlacementOnViewport(terminal, request.viewOffset)
        || Kitty::hasPlaceholders(&terminal, request.viewOffset);
    eligibility.hadImagesOnViewport = baseline.hadImagesOnViewport;
    eligibility.hasImagesOnViewport = hasImagesOnViewport;
    capture.damage = drainFrameDamage(grid, eligibility, rowDirty);

    // The drain reports the grid's sticky hint; the screen this capture reads
    // is the sharper fact, and sharper still is which rows carry the blink. A
    // blinking cell changes phase without a write, so its row must rebuild on
    // every tick — but only its row, where refusing the retained target
    // rebuilds every row of a screen on which one cell animates. The rows the
    // scan finds enter the dirty set and the hazard is answered there.
    //
    // Scrolled back, the rows on display are outside the live-screen reading
    // and the hint stands — a blink cell up there must not freeze mid-phase in
    // retained pixels, and this scan cannot say which row it is on.
    if (capture.damage.hasBlink && request.viewOffset == 0) {
        markBlinkRowsDirty(terminal, rowDirty);
        capture.damage.hasBlink = false;
    }

    // The focused search match's rows, marked after the drain because the
    // drain clears the span. Its highlight is baked into the cells, so a run
    // that arrives on a row whose bytes did not change finds that row clean,
    // the cell pass never repaints it, and the pill's flat fill is missing
    // under the glyphs while the ring the overlay draws still shows its ends.
    //
    // Both runs are marked, this frame's and the last's, because a match that
    // moves leaves a row it must un-paint as well as reaching one it must
    // paint. The old run's pixels travel with the scroll blit: a run inside
    // the scrolled region lands shifted by the pending delta, and the shifted
    // rows are the ones the repaint must cover. Only a match whose run moved
    // owes rows; one that merely persists is already in the retained pixels.
    //
    // The run compared and recorded is the one this frame paints, which for a
    // frame built without the view tiers is none: the retained pixels then
    // hold the match's cells unfilled whatever the search says, and a baseline
    // that recorded the search's run would let the next frame that draws the
    // ring Load those cells as if they carried the fill.
    const FocusedMatchRun match = request.isFocusedMatchBaked ? focusedMatchRun(terminal, request.viewOffset) : FocusedMatchRun {};
    const bool didMatchChange = !baseline.isInitialized
        || baseline.matchRow0 != match.row0 || baseline.matchRow1 != match.row1
        || baseline.matchColumn0 != match.column0 || baseline.matchColumn1 != match.column1;
    if (didMatchChange) {
        for (int row = match.row0; row <= match.row1; ++row) {
            if (row >= 0 && row < request.rows)
                rowDirty[static_cast<std::size_t>(row)] = 1;
        }
        for (int row = baseline.matchRow0; row <= baseline.matchRow1; ++row) {
            int ghost = row;
            if (capture.damage.scrollDelta != 0 && ghost >= capture.damage.scrollTop
                && ghost <= capture.damage.scrollBottom)
                ghost -= capture.damage.scrollDelta;
            if (ghost >= 0 && ghost < request.rows)
                rowDirty[static_cast<std::size_t>(ghost)] = 1;
        }
    }

    // The status row is the last row and changes outside the dirty set — the
    // indicator carries the cursor's position, which moves without that row
    // being written. It is one row, so it is marked rather than charged to the
    // whole frame.
    capture.hasStatusLine = false;
    if (terminal.statusLineType != 0 && request.rows > 0 && request.rows <= static_cast<int>(rowDirty.size()))
        rowDirty[static_cast<std::size_t>(request.rows - 1)] = 1;
    capture.rowDirtyCount = rowDirty.size();

    capture.refresh.canReuseRows = capture.damage.canReuse;
    capture.refresh.scrollTop = capture.damage.scrollTop;
    capture.refresh.scrollBottom = capture.damage.scrollBottom;
    capture.refresh.scrollDelta = capture.damage.scrollDelta;
    capture.refresh.viewOffset = request.viewOffset;
    const int history = Screen::scrollbackCount(&terminal);
    capture.refresh.firstAbsoluteRow = history - std::clamp(request.viewOffset, 0, history);
    capture.refresh.generationAfterDrain = capture.damage.generationAfterDrain;
    capture.refresh.baseGeneration = baseline.generation;
    capture.refresh.rowDirty = rowDirty;

    baseline.isInitialized = true;
    baseline.grid = &grid;
    baseline.generation = capture.damage.generationAfterDrain;
    baseline.viewOffset = request.viewOffset;
    baseline.rows = request.rows;
    baseline.columns = request.columns;
    baseline.isReverseVideo = isFrameReverseVideo;
    baseline.paletteEpoch = terminal.paletteEpoch;
    baseline.hadImagesOnViewport = hasImagesOnViewport;
    baseline.matchRow0 = match.row0;
    baseline.matchRow1 = match.row1;
    baseline.matchColumn0 = match.column0;
    baseline.matchColumn1 = match.column1;
    // Recorded on every frame, marked on only some: the baseline is what the
    // retained pixels hold, so it must follow the frame even when the frame
    // owed no row. Advancing it only on a change would compare the next frame
    // against something the buffer stopped showing.
    return capture;
}

void invalidateRetainedFrame(Terminal& terminal, RetainedFrameBaseline& baseline)
{
    baseline = RetainedFrameBaseline();
    if (terminal.grid)
        terminal.grid->invalidateShadow();
}

RetainedEncodePlan planRetainedEncode(const RetainedCapture& capture, const RetainedEncodeRequest& request)
{
    RetainedEncodePlan plan;
    RetainedTargetEligibility target;
    target.isTargetMatching = request.isTargetMatching;
    target.hasBackdrop = request.hasBackdrop;
    target.hasBlink = capture.damage.hasBlink;
    target.hasStatusLine = capture.hasStatusLine;
    target.rowDirtyCount = capture.rowDirtyCount;
    target.rows = request.rows;
    plan.shouldLoadRetainedTarget = canReuseRetainedTarget(capture.damage, target);
    if (plan.shouldLoadRetainedTarget && capture.damage.scrollDelta != 0) {
        plan.blitPlan = scrollBlitPlan(capture.damage, request.rows, request.cellHeightInPixels,
            request.originYInPixels, request.textureHeightInPixels);
        if (plan.blitPlan.bandCount == 0)
            plan.shouldLoadRetainedTarget = false;
    }
    return plan;
}

RetainedFramePlan planRetainedFrame(CellGrid& grid, const Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedFrameRequest& request, std::span<std::uint8_t> rowDirty)
{
    RetainedFramePlan plan;
    if (rowDirty.empty() || request.rows != static_cast<int>(rowDirty.size()))
        return plan;

    RetainedCaptureRequest captureRequest;
    captureRequest.rows = request.rows;
    captureRequest.columns = request.columns;
    captureRequest.viewOffset = request.viewOffset;
    captureRequest.isGridCacheEnabled = request.isGridCacheEnabled;
    const RetainedCapture capture = planRetainedCapture(grid, terminal, baseline, captureRequest, rowDirty);
    plan.damage = capture.damage;
    plan.refresh = capture.refresh;

    RetainedEncodeRequest encodeRequest;
    encodeRequest.isTargetMatching = request.isTargetMatching;
    encodeRequest.hasBackdrop = request.hasBackdrop;
    encodeRequest.rows = request.rows;
    encodeRequest.cellHeightInPixels = request.cellHeightInPixels;
    encodeRequest.originYInPixels = request.originYInPixels;
    encodeRequest.textureHeightInPixels = request.textureHeightInPixels;
    const RetainedEncodePlan encode = planRetainedEncode(capture, encodeRequest);
    plan.shouldLoadRetainedTarget = encode.shouldLoadRetainedTarget;
    plan.blitPlan = encode.blitPlan;
    // The two answers are one decision. `canReuseRows` lets the builder re-read
    // only the rows in the dirty set, which is sound exactly while the pixels
    // of the clean rows are still on the target; a frame that does not load
    // that target is about to clear them, so trusting the set there draws the
    // changed rows onto an empty texture and every clean row is gone. On screen
    // that is text vanishing as the reader types and returning on whichever
    // later frame rebuilds.
    //
    // Reconciled here rather than at each frontend: the encode verdict folds in
    // hazards the drain never sees — the frontend's own refusal above all, from
    // a target that no longer matches the drawable or sheets that need
    // re-baselining — and a backend left to notice the combination for itself
    // is a backend that can miss it.
    if (!plan.shouldLoadRetainedTarget)
        plan.refresh.canReuseRows = false;
    return plan;
}

RetainedFramePlan planRetainedFrame(Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedFrameRequest& request, RowDirtySpan rowDirty)
{
    if (!terminal.grid)
        return RetainedFramePlan {};
    return planRetainedFrame(*terminal.grid, terminal, baseline, request, rowDirty);
}

bool canSkipGridCapture(const Terminal& terminal, OcclusionSkip occlusionSkip)
{
    GridSkipEligibility eligibility;
    eligibility.isFullscreenContentActive = terminal.modes.fullscreenContent;
    eligibility.isLetterboxOpaque = terminal.fullscreenContentDisplay.letterbox == FullscreenContentLetterbox::Color;
    eligibility.isOcclusionSkipEnabled = occlusionSkip == OcclusionSkip::Enabled;
    return captureCanSkipGrid(eligibility);
}

RetainedFramePlan planSkippedGridFrame(const Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedFrameRequest& request, RowDirtySpan rowDirty)
{
    RetainedFramePlan plan;
    // Gone whatever else this returns: a frame that reads no cell cannot vouch
    // for the retained rows, so the first plan after it re-reads every row.
    baseline.isInitialized = false;
    if (rowDirty.empty() || request.rows != static_cast<int>(rowDirty.size()))
        return plan;
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    // The combination planRetainedFrame refuses — no target and rows kept.
    // The invariant it protects, that a full rebuild reads fresh cells, is
    // about cells the build reads, and this build reads none.
    plan.shouldLoadRetainedTarget = false;
    plan.refresh.canReuseRows = true;
    plan.refresh.viewOffset = request.viewOffset;
    plan.refresh.firstAbsoluteRow = Screen::scrollbackCount(&terminal) - request.viewOffset;
    plan.refresh.rowDirty = rowDirty;
    return plan;
}

template<bool shouldCollectStatistics>
[[nodiscard]] static FrameCopyStatistics refreshCapturedRowsImplementation(const Terminal& terminal, const CapturedRowRefresh& refresh,
    std::span<Cell> cells, std::span<std::uint8_t> lineAttributes, std::span<std::uint8_t> rowMarks)
{
    FrameCopyStatistics statistics;
    const int rows = static_cast<int>(lineAttributes.size());
    if (rows <= 0 || rowMarks.size() != lineAttributes.size() || cells.empty()
        || cells.size() % lineAttributes.size() != 0)
        return statistics;
    const int columns = static_cast<int>(cells.size() / lineAttributes.size());
    const std::size_t rowBytes = static_cast<std::size_t>(columns) * sizeof(Cell);

    const bool canReuseRows = refresh.canReuseRows && static_cast<int>(refresh.rowDirty.size()) == rows;
    if (canReuseRows && refresh.scrollDelta != 0 && refresh.scrollTop >= 0
        && refresh.scrollBottom >= refresh.scrollTop && refresh.scrollBottom < rows) {
        const int shift = refresh.scrollDelta > 0 ? refresh.scrollDelta : -refresh.scrollDelta;
        if (shift > 0 && shift <= refresh.scrollBottom - refresh.scrollTop) {
            if (refresh.scrollDelta > 0) {
                for (int row = refresh.scrollTop; row <= refresh.scrollBottom - shift; ++row) {
                    std::memmove(cells.data() + static_cast<std::size_t>(row) * columns,
                        cells.data() + static_cast<std::size_t>(row + shift) * columns, rowBytes);
                    if constexpr (shouldCollectStatistics) {
                        statistics.cellBytesCopied += rowBytes;
                        ++statistics.rowsCopied;
                    }
                }
            } else {
                for (int row = refresh.scrollBottom; row >= refresh.scrollTop + shift; --row) {
                    std::memmove(cells.data() + static_cast<std::size_t>(row) * columns,
                        cells.data() + static_cast<std::size_t>(row - shift) * columns, rowBytes);
                    if constexpr (shouldCollectStatistics) {
                        statistics.cellBytesCopied += rowBytes;
                        ++statistics.rowsCopied;
                    }
                }
            }
        }
    }

    for (int row = 0; row < rows; ++row) {
        if constexpr (shouldCollectStatistics) {
            ++statistics.rowsInspected;
        }
        // viewOffset-aware: visible row maps to firstAbsoluteRow + row, so a
        // line scrolled into history keeps its DECDWL/DECDHL attribute — history
        // rows read their captured attribute, live rows forward to the Screen.
        lineAttributes[row] = Screen::rowLineAttribute(&terminal, refresh.firstAbsoluteRow + row);
        rowMarks[row] = Screen::rowMark(&terminal, refresh.firstAbsoluteRow + row);
        if (canReuseRows && !refresh.rowDirty[row])
            continue;
        // CellGrid-time shape plus RTL reorder for this row (a plain copy when
        // all-LTR). The logical store is never mutated, so copy and selection
        // keep the original code points; the renderer consumes visual order.
        Screen::getDisplayRow(terminal, refresh.viewOffset, row, cells.data() + static_cast<std::size_t>(row) * columns, columns);
        if constexpr (shouldCollectStatistics) {
            statistics.cellBytesCopied += rowBytes;
            ++statistics.rowsCopied;
        }
    }
    return statistics;
}

FrameCopyStatistics refreshCapturedRowsWithStatistics(const Terminal& terminal, const CapturedRowRefresh& refresh,
    std::span<Cell> cells, std::span<std::uint8_t> lineAttributes, std::span<std::uint8_t> rowMarks)
{
    return refreshCapturedRowsImplementation<true>(terminal, refresh, cells, lineAttributes, rowMarks);
}

void refreshCapturedRows(const Terminal& terminal, const CapturedRowRefresh& refresh,
    std::span<Cell> cells, std::span<std::uint8_t> lineAttributes, std::span<std::uint8_t> rowMarks)
{
    (void)refreshCapturedRowsImplementation<false>(terminal, refresh, cells, lineAttributes, rowMarks);
}

ScrollBlitPlan scrollBlitPlan(const FrameDamage& damage, int rows, int cellHeightInPixels,
    int originYInPixels, int textureHeightInPixels)
{
    ScrollBlitPlan plan;
    if (damage.scrollDelta == 0 || cellHeightInPixels <= 0 || rows <= 0
        || damage.scrollTop < 0 || damage.scrollBottom < damage.scrollTop || damage.scrollBottom >= rows)
        return plan;

    // A shift consuming the whole region is not a refusal: the region band
    // drops out below (no row survives the shift), but the pinned bands above
    // and below it carry unchanged content the drain never marked dirty — a
    // caller that rebuilt only dirty rows over a cleared target would blank
    // them. A full-screen scroll past its own height still yields no bands,
    // because every band's row count reaches zero.
    const int shift = damage.scrollDelta > 0 ? damage.scrollDelta : -damage.scrollDelta;
    const int regionRows = damage.scrollBottom - damage.scrollTop + 1;

    const auto addBand = [&](int sourceRow, int destinationRow, int rowCount) {
        if (rowCount <= 0)
            return;
        const int sourceY = originYInPixels + sourceRow * cellHeightInPixels;
        const int destinationY = originYInPixels + destinationRow * cellHeightInPixels;
        int height = rowCount * cellHeightInPixels;
        const int overrun = std::max(sourceY, destinationY) + height - textureHeightInPixels;
        if (overrun > 0)
            height -= overrun;
        if (height <= 0 || sourceY < 0 || destinationY < 0)
            return;
        plan.bands[plan.bandCount++] = { sourceY, destinationY, height };
    };

    addBand(0, 0, damage.scrollTop);
    if (damage.scrollDelta > 0)
        addBand(damage.scrollTop + shift, damage.scrollTop, regionRows - shift);
    else
        addBand(damage.scrollTop, damage.scrollTop + shift, regionRows - shift);
    addBand(damage.scrollBottom + 1, damage.scrollBottom + 1, rows - damage.scrollBottom - 1);
    return plan;
}

CapturedRowMismatch verifyCapturedRows(const Terminal& terminal, const CapturedRowRefresh& refresh,
    std::span<const Cell> cells, std::span<Cell> scratch, std::span<std::uint8_t> scratchLineAttributes,
    std::span<std::uint8_t> scratchRowMarks)
{
    CapturedRowMismatch mismatch;
    if (scratch.size() != cells.size() || scratchLineAttributes.empty())
        return mismatch;
    const int rows = static_cast<int>(scratchLineAttributes.size());
    const int columns = rows > 0 ? static_cast<int>(cells.size() / static_cast<std::size_t>(rows)) : 0;
    if (rows <= 0 || columns <= 0)
        return mismatch;

    // The reference is this same function with reuse denied, so the check
    // cannot drift from what it verifies: a re-read spelled separately would
    // be a second implementation and could disagree for its own reasons.
    CapturedRowRefresh full = refresh;
    full.canReuseRows = false;
    full.scrollDelta = 0;
    refreshCapturedRows(terminal, full, scratch, scratchLineAttributes, scratchRowMarks);

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const std::size_t index = static_cast<std::size_t>(row) * columns + column;
            // Byte-wise, because the invariant is stated byte for byte and
            // Cell has no equality of its own: a field-wise compare here
            // would be a third opinion about which fields matter.
            if (std::memcmp(&cells[index], &scratch[index], sizeof(Cell)) == 0)
                continue;
            ++mismatch.cellCount;
            if (mismatch.row < 0) {
                mismatch.row = row;
                mismatch.column = column;
            }
        }
    }
    return mismatch;
}

} // namespace Ubiquitty::Rendering
