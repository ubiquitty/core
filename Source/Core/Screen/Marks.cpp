// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Clear-to-mark: the policy behind Cmd-L.

#include "Core/Screen/Marks.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/LineStore.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Internal.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <cstring>

namespace Ubiquitty::Screen {

// Delete absolute rows [from, to) and slide everything below up by (to - from),
// then pull history back down so the prompt stays bottom-anchored and the
// scrollback count shrinks. `to` sits on the live screen; `from` may be in
// scrollback. Used by dropRowsToPreviousMark to eat a command block.
static void deleteRowsBetween(Ubiquitty::Terminal& t, int from, int to)
{
    if (!t.screens.active || from < 0 || to <= from)
        return;
    const int history = t.scrollbackCount;
    const int removed = to - from;
    // Deleting a middle interval gives rows above and below it different maps;
    // one global offset cannot keep an absolute-row anchor attached truthfully.
    ++t.absoluteRowEpoch;

    // Live-screen portion [max(from, history), to): scroll up so the prompt below
    // slides up and the bottom is blanked. scrollRegion rotates cells + RowMeta
    // together, so no stale mark / line-attribute rides into the blanks.
    const int liveFrom = (from > history) ? (from - history) : 0;
    const int liveCount = (to - history) - liveFrom; // to is always on-screen
    if (liveCount > 0 && t.grid) {
        Cell blank;
        Ubiquitty::Screen::eraseCell(&blank, t.screens.active);
        if (hasActivePins(t))
            notePositionsShiftedInPlace(t, liveFrom, t.rows - 1, liveCount);
        t.grid->scrollRegion(Ubiquitty::Row(liveFrom), Ubiquitty::Row(t.rows - 1), liveCount, blank);
        t.screens.active->cursorY -= liveCount;
        if (t.screens.active->cursorY < 0)
            t.screens.active->cursorY = 0;
        // The surviving row above the revealed blanks never soft-wraps into them.
        const int lastRow = t.rows - liveCount - 1;
        if (lastRow >= 0)
            t.screens.active->lineWrapped[lastRow] = false;
    }

    // Scrollback portion [from, min(to, history)): the NEWEST history lines in
    // the range — drop them off the tail so the kept history ends at `from` and
    // joins the slid-up prompt with no gap.
    if (from < history) {
        const int scrollbackDrop = ((to < history) ? to : history) - from;
        const std::uint64_t nextOrdinalBefore = t.lineStore ? t.lineStore->nextOrdinal() : 0;
        if (t.lineStore)
            t.lineStore->dropNewest(scrollbackDrop);
        t.scrollbackCount = t.lineStore ? t.lineStore->count() : 0;
        notePositionsHistoryTailDeleted(t, nextOrdinalBefore);
    }

    // The pending prompt (and any mark on it) rode `removed` rows higher.
    if (t.pendingPromptRow >= to)
        t.pendingPromptRow -= removed;
    else if (t.pendingPromptRow >= from)
        t.pendingPromptRow = from;

    // Re-anchor: pull liveCount history lines back onto the screen (capped at
    // available history) so a full screen stays full and the prompt rides back to
    // the bottom. No-op with no history. pendingPromptRow is absolute and
    // unchanged by the pull (the slide is offset by the shrinking history above).
    pullHistoryDown(&t, liveCount);
    // The block's lines are gone from history; the ones above it are renumbered
    // past every number the block had.
    retireHistory(t);

    t.scrollSearchState.reset();
    Ubiquitty::Screen::markAllDirty(t);
}

// Cmd-L "Clear to Previous Mark": eat the bottom-most executed-command block
// (previous prompt mark up to the current prompt) and slide the prompt up into
// its place. Returns the absolute row the prompt landed on, or -1 when there is
// no executed command above the prompt.
int dropRowsToPreviousMark(Ubiquitty::Terminal* t)
{
    if (!t || !t->screens.active)
        return -1;
    const int history = scrollbackCount(t);
    // Bottom anchor: the current prompt's top row (recorded at OSC 133 A as
    // pendingPromptRow), else the cursor row when no prompt is pending.
    int bottom = (t->pendingPromptRow >= 0) ? t->pendingPromptRow : history + t->screens.active->cursorY;
    if (bottom < 0)
        bottom = 0;
    if (bottom > history + t->rows)
        bottom = history + t->rows;
    // The most recent executed command strictly above the anchor — the block we
    // eat. None → -1.
    const int prev = findPromptMarkBefore(t, bottom, osc133MarkCmdExecuted);
    if (prev < 0 || prev >= bottom)
        return -1;
    deleteRowsBetween(*t, prev, bottom);
    return prev;
}

} // namespace Ubiquitty::Screen
