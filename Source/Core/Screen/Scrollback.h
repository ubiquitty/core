// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Screen/HistoryFacts.h"
#include "Core/Screen/ScrollLine.h"
#include "Forward.h"

// The scrollback store: the history tier behind the live screen.
//
// Retaining and dropping history is screen-model work, not sequence parsing. The
// ring itself is Core/LineStore.h; these are the operations over it.
//
// These live at Ubiquitty:: rather than Ubiquitty::Screen::, matching the
// namespace their callers already qualify.

namespace Ubiquitty {
struct Cell;

namespace Screen {

/// Appends a live grid row before its storage is recycled, preserving its
/// metadata and avoiding an intermediate Cell row when its bytes are compact.
///
/// - Parameters:
///   - terminal: The terminal owning the departing live row and history.
///   - row: The active grid's logical row before its storage is recycled.
void scrollbackAppendGridRow(Ubiquitty::Terminal& terminal, int row);

/// Retained line count, and the lifetime total ever appended (the latter keeps
/// growing after eviction, so a viewer can tell scrollback loss from no output).
[[nodiscard]] int scrollbackCount(const Ubiquitty::Terminal* t);
[[nodiscard]] long long scrollbackTotalAppended(const Ubiquitty::Terminal* t);
[[nodiscard]] const Ubiquitty::Screen::ScrollLine* scrollbackLine(const Ubiquitty::Terminal* t, int index);

/// How far the VIEW may scroll back, which is not the same question as how many
/// rows the buffer retains.
///
/// Zero while the alternate screen is active: it keeps no history of its own, so
/// no offset into one is reachable from it. `scrollbackCount` above answers the
/// storage question and stays true whichever screen is displayed — the remote
/// sync frame serialises it, and search, marks and absolute-row addressing all
/// convert through it, so it must not be made to lie.
///
/// The two questions have two names so that no caller has to remember to write
/// `isUsingAlternateScreen ? 0 :` in front of `scrollbackCount`; a caller that
/// asks the storage question for a limit lets a selection drag on the alternate
/// screen scroll into the primary's history. Ask this one for a limit; ask
/// `scrollbackCount` for a fact.
///
/// - Parameter t: The terminal, or null.
/// - Returns: The greatest reachable view offset, 0 on the alternate screen.
[[nodiscard]] int scrollableRowCount(const Ubiquitty::Terminal* t);

/// The retained history as one output transition sees it.
///
/// `scrollableRowCount`, `scrollbackTotalAppended` and the absolute-row epoch
/// read together, which is what a viewport policy judges output by.
///
/// - Parameter t: The terminal, or null.
/// - Returns: The facts, all zero for a null terminal.
[[nodiscard]] HistoryFacts historyFacts(const Ubiquitty::Terminal* t);

/// Drops every retained line, leaving the live screen untouched.
///
/// Marker arrays and any in-flight search state reset alongside the ring, so a
/// caller can request a full redraw without follow-up bookkeeping.
///
/// Re-bases `pendingPromptRow`: a prompt awaiting its command keeps its row but
/// loses the dropped history from its absolute index. Skipping that would leave
/// the prompt pointing into history the ring no longer holds.
///
/// The live rows keep their own metadata: this drops SAVED lines, and the
/// visible rows' soft-wrap joins and line attributes describe content the drop
/// never touched. ``resetLiveRowMetadata`` is the separate verb for a caller
/// that wants a pristine screen as well.
///
/// - Parameter t: The terminal whose history is dropped.
void dropScrollback(Ubiquitty::Terminal* t);

/// Returns every visible row to single-width, single-height, unwrapped.
///
/// The user's clear-the-screen verbs want this on top of dropping history: a
/// DECDHL/DECDWL banner left on screen makes the next application render its
/// top rows at twice the size. The protocol's own erases do not, because they
/// say which rows they erase and the metadata belongs to those rows.
///
/// - Parameter t: The terminal whose visible rows are reset.
void resetLiveRowMetadata(Ubiquitty::Terminal* t);

/// Resets the ring without that policy: `lineStore->clear()` and the two
/// counters, nothing else.
///
/// Only the sync and teleport path wants this: it clears and immediately
/// repopulates from a snapshot that carries its own prompt and mark state, so
/// re-basing pendingPromptRow against history that is about to be replaced would
/// be wrong. The name has to stand well apart from `dropScrollback`: these are
/// different contracts, and a reader who picks the wrong one gets a
/// pendingPromptRow re-based against rows the snapshot is about to replace.
void resetScrollbackStore(Ubiquitty::Terminal* t);

/// Drops the oldest entries, along with the marks they carried.
///
/// - Parameters:
///   - t: The terminal whose history is trimmed.
///   - absoluteRow: Drop everything below this absolute row; the count is
///     clamped to the retained line count, so a row past the end empties the
///     ring rather than misbehaving.
void dropScrollbackBefore(Ubiquitty::Terminal* t, int absoluteRow);

/// Drop history rows before `absoluteRow`, pulling live rows down to fill.
void dropRowsBefore(Ubiquitty::Terminal* t, int absoluteRow);

/// Append a scrolled-off row to the store. The row-level entry point behind
/// the cells directly.
void scrollbackAppend(Ubiquitty::Terminal* t, const Cell* cells, int length,
    uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping);

/// Folds cold scrollback blocks into the row form (`LineStore::foldColdBlocks`).
///
/// Off the append path by construction: the ingest loop calls it only on a
/// pass that drained nothing, under the model lock like every other write.
///
/// - Parameters:
///   - t: The terminal whose history folds.
///   - blockBudget: How many blocks this call may fold.
/// - Returns: Whether a cold raw block remains to fold.
[[nodiscard]] bool foldColdScrollback(Ubiquitty::Terminal& t, int blockBudget);

/// Clamps a requested scrollback view offset to what the buffer can show.
///
/// 0 is the live grid and `scrollbackCount` the oldest retained row, so a
/// frontend scrolling by a wheel notch, a page, or straight to the top passes
/// the arithmetic result and gets back a position that exists. Here rather than
/// in each frontend because every one of them was doing the same two
/// comparisons, and a scroll that lands one row past the end reads as a
/// terminal that has lost its history.
///
/// - Parameters:
///   - requestedOffset: Rows back from the live grid; may be out of range.
///   - scrollbackRowCount: Rows the buffer retains, from `scrollbackCount`.
/// - Returns: The requested offset clamped to `[0, scrollbackRowCount]`.
[[nodiscard]] constexpr int clampViewOffset(int requestedOffset, int scrollbackRowCount)
{
    const int ceiling = scrollbackRowCount > 0 ? scrollbackRowCount : 0;
    if (requestedOffset < 0)
        return 0;
    return requestedOffset > ceiling ? ceiling : requestedOffset;
}

/// Carries a scrolled-back view offset across a reflow.
///
/// A rewrap renumbers every history row — widening merges wrapped rows and the
/// count drops, narrowing splits them and it grows — so neither the offset nor
/// an absolute row index survives as an identity. What does survive is the
/// reader's PLACE as a fraction of the history, and this maps the offset
/// proportionally into the new count.
///
/// The failure this exists to prevent: treating the rewrap's count change as
/// tail growth. A growth compensation subtracts the delta from the offset, and
/// a wide-enough merge drives it to zero — the view a user had parked deep in
/// history lands on the live tail because the window got wider.
///
/// Approximate by design: a proportional hold keeps the reader within a few
/// rows of the text they were on, which is what a resize should feel like.
/// Holding the exact text line would need the rewrap itself to report where a
/// given old row landed, which is a property of the reflow and not of this
/// arithmetic.
///
/// - Parameters:
///   - offsetBefore: Rows back from the live grid when the reflow started.
///   - historyBefore: Scrollback rows retained before the reflow.
///   - historyAfter: Scrollback rows retained after it.
/// - Returns: The offset re-expressed against the new history, clamped to it.
///   Zero stays zero: a view on the live tail stays on the live tail.
[[nodiscard]] constexpr int viewOffsetAcrossReflow(int offsetBefore, int historyBefore, int historyAfter)
{
    if (offsetBefore <= 0 || historyBefore <= 0 || historyAfter <= 0)
        return 0;
    // 64-bit intermediate: offset and history are both bounded by the
    // scrollback limit, but their product is not bounded by int.
    const long long scaled = (static_cast<long long>(offsetBefore) * historyAfter + historyBefore / 2) / historyBefore;
    const int offset = scaled > historyAfter ? historyAfter : static_cast<int>(scaled);
    // A reader parked in history stays in history: the proportional map can
    // round a small offset to zero, which reads as the jump this prevents.
    return offset < 1 ? 1 : offset;
}

/// Carries a view offset across a resize, from the history on either side of it.
///
/// Most resizes renumber nothing. A height change moves rows across the
/// boundary between the screen and the NEWEST end of history — a shorter
/// screen pushes its top rows in, a taller one pulls the newest rows back out —
/// and every older row keeps its place, so the offset moves by exactly the
/// rows that crossed and the reader keeps the very rows they were reading. The
/// counts say which case this is: a push appends as many rows as the history
/// retains more of, and a pull appends none while the history retains fewer.
///
/// A pull that reaches the reader's rows lands on the live tail. Everything the
/// reader was looking at is on the live screen then, and holding the view back
/// by the rows that did not cross would leave the newest rows — the prompt,
/// the output that follows — below the bottom edge: the output a resize seemed
/// to eat.
///
/// Anything else renumbered the history: a rewrap to a new width, or a push
/// into a full history that evicted as many rows as it appended. Those keep
/// the reader's place proportionally, through ``viewOffsetAcrossReflow``.
///
/// - Parameters:
///   - offsetBefore: Rows back from the live grid when the resize started.
///   - before: The history facts published before the resize.
///   - after: The history facts published after it.
/// - Returns: The offset against the new history, clamped to it; zero is the
///   live tail, and a view on the tail stays there.
[[nodiscard]] constexpr int viewOffsetAcrossResize(int offsetBefore, const HistoryFacts& before, const HistoryFacts& after)
{
    if (offsetBefore <= 0 || after.retainedRows <= 0)
        return 0;
    const long long appended = after.appendedRows - before.appendedRows;
    const int retainedGrowth = after.retainedRows - before.retainedRows;
    const bool isPull = appended == 0 && retainedGrowth <= 0;
    const bool isPush = appended > 0 && appended == retainedGrowth;
    if (!isPull && !isPush)
        return viewOffsetAcrossReflow(offsetBefore, before.retainedRows, after.retainedRows);
    return clampViewOffset(offsetBefore + retainedGrowth, after.retainedRows);
}

} // namespace Screen
} // namespace Ubiquitty
