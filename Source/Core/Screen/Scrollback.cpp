// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The scrollback store's mutation surface.

#include "Core/Screen/Scrollback.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/LineStore.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Internal.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace Ubiquitty::Screen {

static LineStore& ensureLineStore(Terminal& terminal)
{
    if (!terminal.lineStore) {
        terminal.lineStore = std::make_unique<LineStore>();
        terminal.lineStore->init(terminal.scrollbackLimit);
        terminal.lineStore->restartNumbering(terminal.historyEpochBase);
        terminal.scrollbackCapacity = terminal.lineStore->capacity();
    }
    return *terminal.lineStore;
}

// Append a scrolled-off line into the scrollback store, lazily created on the
// first captured line. length == 0 is valid: a fully-blank line still occupies a
// history slot (visibleCell reconstructs cells past the stored length as blank).
void scrollbackAppend(Ubiquitty::Terminal* t, const Ubiquitty::Cell* cells, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping)
{
    if (!t || !cells || length < 0)
        return;
    ensureLineStore(*t).append(cells, length, lineAttribute, mark, wrapped, needsShaping);
    t->scrollbackCount = t->lineStore->count();
}

void scrollbackAppendGridRow(Terminal& terminal, int row)
{
    if (!terminal.grid || row < 0 || row >= terminal.grid->rows())
        return;
    const auto captured = terminal.grid->scrollCaptureMetadata(row);
    const auto& meta = captured.meta;
    if (const auto compact = terminal.grid->compactASCIIRow(Row(row))) {
        ensureLineStore(terminal).appendDefaultASCII(*compact, captured.contentWidth, meta.lineAttribute, meta.oscMark,
            meta.has(RowFlag::Wrapped), meta.has(RowFlag::NeedsShaping));
        terminal.scrollbackCount = terminal.lineStore->count();
        return;
    }
    const Cell* cells = captured.contentWidth > 0 ? terminal.grid->cellAt(Row(row), Column(0)) : &defaultBlankCell;
    scrollbackAppend(&terminal, cells, captured.contentWidth, meta.lineAttribute, meta.oscMark,
        meta.has(RowFlag::Wrapped), meta.has(RowFlag::NeedsShaping));
}

bool foldColdScrollback(Ubiquitty::Terminal& t, int blockBudget)
{
    return t.lineStore && t.lineStore->foldColdBlocks(blockBudget);
}

int scrollbackCount(const Ubiquitty::Terminal* t) { return t ? t->scrollbackCount : 0; }

// The alternate screen keeps no history, so no offset into one is reachable
// from it. Every caller asking "how far may the view scroll" comes here; the
// callers asking "how many rows are retained" keep using scrollbackCount above.
int scrollableRowCount(const Ubiquitty::Terminal* t)
{
    if (!t || t->screens.isUsingAlternateScreen)
        return 0;
    return scrollbackCount(t);
}

long long scrollbackTotalAppended(const Ubiquitty::Terminal* t) { return t && t->lineStore ? t->lineStore->totalAppended() : 0; }

HistoryFacts historyFacts(const Ubiquitty::Terminal* t)
{
    return {
        .retainedRows = scrollableRowCount(t),
        .appendedRows = scrollbackTotalAppended(t),
        .absoluteRowEpoch = t ? t->absoluteRowEpoch : 0,
    };
}

void dropScrollback(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    // The live screen stays put while history drops, so a prompt awaiting its
    // command (A fired, C pending) keeps its row but loses `oldHistory` from
    // its absolute index; one that lived in the dropped history is gone.
    const int oldHistory = t->scrollbackCount;
    if (t->pendingPromptRow >= 0) {
        t->pendingPromptRow = (t->pendingPromptRow >= oldHistory) ? t->pendingPromptRow - oldHistory : -1;
    }
    if (t->lineStore)
        t->lineStore->clear();
    t->scrollbackCount = 0;
    // The live rows keep their metadata: ED 3 erases SAVED lines, and wiping
    // the visible rows' soft-wrap joins and line attributes rewrites content
    // the erase never touched — a reflow after a clear-history would rejoin
    // nothing, and a DECDHL banner on screen would fall back to single height.
    // The user's clear wants that reset and asks for it by name.
    t->scrollSearchState.reset();
    // Run text only the dropped history named is free now.
    sweepRunText(*t);
    retireHistory(*t);
}

void resetLiveRowMetadata(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    Ubiquitty::Screen::State* s = t->screens.active;
    if (!s)
        return;
    for (int r = 0; r < t->rows; r++) {
        s->lineAttribute[r] = lineAttributeNormal;
        s->lineWrapped[r] = false;
    }
}

void dropScrollbackBefore(Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t)
        return;
    int drop = absoluteRow;
    if (drop <= 0)
        return;
    if (drop > t->scrollbackCount)
        drop = t->scrollbackCount;
    // Dropped slots' marks ride their StoredLine records; dropping them
    // oldest-first from the store is enough.
    if (t->lineStore)
        t->lineStore->dropOldest(drop);
    t->scrollbackCount = t->lineStore ? t->lineStore->count() : 0;
    t->scrollSearchState.reset();
    // Run text only the dropped lines named is free now.
    sweepRunText(*t);
    // The survivors are renumbered above every number the dropped lines had.
    retireHistory(*t);
}

void dropRowsBefore(Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t)
        return;
    if (absoluteRow <= 0)
        return;
    const int history = t->scrollbackCount;
    // Rows [0, absoluteRow) go away; a pending prompt re-indexes by absoluteRow
    // (or is gone if it sat in the dropped range). Computed up front and assigned
    // at each exit so the internal dropScrollback can't double-adjust it.
    const int32_t pend = t->pendingPromptRow;
    const int32_t newPend = (pend < 0 || pend < absoluteRow) ? -1 : pend - absoluteRow;
    // Live-screen rows to discard (above absoluteRow). Wipe scrollback first;
    // anything between [absoluteRow, history) stays as the new oldest history.
    if (absoluteRow <= history) {
        dropScrollbackBefore(t, absoluteRow);
        t->pendingPromptRow = newPend;
        return;
    }
    // absoluteRow lies inside the live screen: shift it to row 0 destructively.
    dropScrollback(t);
    int shift = absoluteRow - history;
    if (shift > t->rows)
        shift = t->rows;
    Ubiquitty::Screen::State* s = &t->screens.primary;
    // scrollRegion rotates the cells and m_rowMeta (lineAttribute / wrapped /
    // mark) up by `shift` in one pass and resets the revealed bottom rows;
    // the meta rides the same ring as the cells, so no separate per-row
    // meta shift is needed.
    Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    if (t->grid && !t->screens.isUsingAlternateScreen && hasActivePins(*t))
        notePositionsShiftedInPlace(*t, 0, t->rows - 1, shift);
    if (t->grid)
        t->grid->scrollRegion(Ubiquitty::Row(0), Ubiquitty::Row(t->rows - 1), shift, blank);
    // The last surviving content row never joins a soft-wrap into the
    // blanks revealed below it.
    if (t->rows - shift - 1 >= 0)
        s->lineWrapped[t->rows - shift - 1] = false;
    // Cursor follows the shift; clamp at row 0 if it would go negative.
    s->cursorY -= shift;
    if (s->cursorY < 0)
        s->cursorY = 0;
    t->pendingPromptRow = newPend;
    Ubiquitty::Screen::markAllDirty(*t);
}

// Shift the live screen DOWN by k rows and fill the freed top k rows from the
// newest k history lines, dropping them off the store — the move the resize grow
// uses to reclaim history. Re-anchors content to the bottom after rows were
// removed beneath it, so the screen stays full (prompt at the bottom) and the
// scrollback count shrinks by k instead of leaving dead blanks below the prompt.
// k is clamped to the available history; cursorY follows the slide.
void pullHistoryDown(Ubiquitty::Terminal* t, int k)
{
    if (!t || !t->screens.active || !t->grid || k <= 0)
        return;
    const int history = scrollbackCount(t);
    if (k > history)
        k = history;
    if (k <= 0)
        return;
    Ubiquitty::Screen::State* s = t->screens.active;
    Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    CellScratchBuffer<maxColumns> rowStorage;
    Cell* rowBuffer = rowStorage.data();
    // Content first, then the tail it does not reach. Erasing the row whole and
    // writing the span back over it passes the row twice and stamps the live
    // width at full width, which is the width the next scrollback capture then
    // copies — for a row whose content ends long before it.
    auto putRow = [&](int dest, const Cell* cells, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped) {
        if (length > 0)
            t->grid->setRowSpan(Ubiquitty::Row(dest), Ubiquitty::Column(0), Ubiquitty::RowView { const_cast<Cell*>(cells), length });
        if (length < t->columns)
            t->grid->eraseRowRange(Ubiquitty::Row(dest), Ubiquitty::Column(length), Ubiquitty::Column(t->columns - 1), blank);
        s->lineAttribute[dest] = lineAttribute;
        s->lineWrapped[dest] = wrapped;
        s->mark[dest] = mark;
    };
    // Shift live rows down by k (high -> low so a dest never clobbers an
    // unread source). The row's live content width, not the row's width: past
    // it a row is blank, and the tail erase below covers it.
    for (int row = t->rows - 1 - k; row >= 0; --row) {
        const int width = t->grid->rowContentWidth(Ubiquitty::Row(row));
        t->grid->readRow(Ubiquitty::Row(row), rowBuffer);
        putRow(row + k, rowBuffer, width, s->lineAttribute[row], s->mark[row], s->lineWrapped[row]);
    }
    // Fill the freed top k rows from the newest k history lines (oldest first).
    for (int row = 0; row < k; ++row) {
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, history - k + row);
        if (line)
            putRow(row, line->cells, line->length, line->lineAttribute, line->mark, line->wrapped);
        else
            putRow(row, nullptr, 0, lineAttributeNormal, 0, false);
    }
    const std::uint64_t nextOrdinalBefore = t->lineStore ? t->lineStore->nextOrdinal() : 0;
    if (t->lineStore)
        t->lineStore->dropNewest(k);
    t->scrollbackCount = t->lineStore ? t->lineStore->count() : 0;
    notePositionsPulledFromHistory(*t, nextOrdinalBefore);
    s->cursorY += k;
    if (s->cursorY > t->rows - 1)
        s->cursorY = t->rows - 1;
}

const Ubiquitty::Screen::ScrollLine* scrollbackLine(const Ubiquitty::Terminal* t, int index) { return t && t->lineStore ? t->lineStore->at(index) : nullptr; }

void resetScrollbackStore(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    if (t->lineStore)
        t->lineStore->clear();
    t->scrollbackCount = 0;
}

} // namespace Ubiquitty::Screen
