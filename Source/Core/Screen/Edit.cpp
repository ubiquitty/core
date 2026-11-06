// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Screen/Edit.cpp — Line, column and character insert/delete — IL, DL, ICH, DCH, DECIC, DECDC —
// plus the index moves (forwardIndex/backIndex) and the alternate-screen flip.
//
// The shift* helpers are the shared primitive: everything here is ultimately a
// memmove of cells within a row or of rows within a region, with wide-pair
// orphans scrubbed at the edges.
//
// Cold relative to the per-byte parser loop, which stays in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/RichText/Blocks.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Internal.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/Triggers/Engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Ubiquitty::Screen {

void insertLines(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int top = s->cursorY;
    int bot = s->scrollBottom;
    if (s->cursorY < s->scrollTop || s->cursorY > s->scrollBottom)
        return;
    // DECSLRM also restricts IL/DL to fire only when the cursor is inside the
    // horizontal margins; outside, no-op.
    if (isCursorOutsideMargins(t))
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    // insertLines = scroll [top..bot] DOWN by n (blanks fill at top);
    // scrollRegion's sign is n>0=up, so pass -n.
    if (isRowNarrowedByMargins(t, s)) {
        scrollRectDown(t, top, bot, s->scrollLeft, s->scrollRight, n);
        s->cursorX = lineHomeColumn(t);
        return;
    }
    // scrollRegion rotates m_rowMeta down with the cells and resets the inserted
    // top rows to NORMAL. Clear the join into the block's new top so a surviving
    // predecessor doesn't claim continuation into inserted blanks.
    noteInPlaceContentShift(t, top, bot, -n, ShiftWidth::FullRows);
    if (t.grid)
        t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), -n, Ubiquitty::Screen::scrollBlankCell(s));
    scrubBlockDamage(t, top, bot);
    if (top > 0)
        s->lineWrapped[top - 1] = false;
    s->cursorX = lineHomeColumn(t);
}

void deleteLines(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int top = s->cursorY;
    int bot = s->scrollBottom;
    if (s->cursorY < s->scrollTop || s->cursorY > s->scrollBottom)
        return;
    if (isCursorOutsideMargins(t))
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    // deleteLines = scroll [top..bot] up by n (n>0 = up).
    if (isRowNarrowedByMargins(t, s)) {
        scrollRectUp(t, top, bot, s->scrollLeft, s->scrollRight, n);
        s->cursorX = lineHomeColumn(t);
        return;
    }
    // scrollRegion rotates m_rowMeta up with the cells and resets the revealed
    // bottom rows to NORMAL.
    noteInPlaceContentShift(t, top, bot, n, ShiftWidth::FullRows);
    if (t.grid)
        t.grid->scrollRegion(Ubiquitty::Row(top), Ubiquitty::Row(bot), n, Ubiquitty::Screen::scrollBlankCell(s));
    scrubBlockDamage(t, top, bot);
    if (top > 0)
        s->lineWrapped[top - 1] = false;
    s->cursorX = lineHomeColumn(t);
}

// InsertRight opens erase blanks at `from` and pushes the tail right (ICH /
// DECIC / DECBI pan); DeleteLeft removes at `from` and pulls the tail left,
// blanking the freed columns (DCH / DECDC / DECFI pan).
enum class ShiftDirection : uint8_t {
    InsertRight,
    DeleteLeft
};

// Per-row column-shift core, shared by ICH/DCH and DECIC/DECDC: shifts row
// `row`'s cells within [from, rightBound) by n, filling per ShiftDirection.
static void shiftRowCellsAt(Ubiquitty::Terminal& t, int row, int from, int rightBound, int n, ShiftDirection direction)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (n > rightBound - from)
        n = rightBound - from;
    if (n <= 0 || !t.grid)
        return;
    if (hasActivePins(t)) [[unlikely]]
        notePositionsRewritten(t, row, row);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    // Only a multicell half can be split by the window's edges, and RowFlag::HasMulticell is latched by
    // every write of one and never stale-false. Without it the scrub below cannot reach a blanking
    // branch, and reading the row whole to shift it elsewhere and store it back buys nothing.
    if (!t.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell)) {
        t.grid->shiftRowCells(row, from, rightBound, n, blank,
            direction == ShiftDirection::InsertRight ? Ubiquitty::RowShiftDirection::InsertRight
                                                     : Ubiquitty::RowShiftDirection::DeleteLeft);
        scrubBlockDamage(t, row, row);
        return;
    }
    Ubiquitty::CellScratchBuffer<maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
    const std::size_t moved = static_cast<std::size_t>(rightBound - from - n);
    if (direction == ShiftDirection::InsertRight) {
        std::memmove(&rowBuffer[from + n], &rowBuffer[from], moved * sizeof(Ubiquitty::Cell));
        for (int i = 0; i < n; i++)
            rowBuffer[from + i] = blank;
    } else {
        std::memmove(&rowBuffer[from], &rowBuffer[from + n], moved * sizeof(Ubiquitty::Cell));
        for (int i = rightBound - n; i < rightBound; i++)
            rowBuffer[i] = blank;
    }
    // A pair straddling either edge of the shifted window keeps one half in
    // place while the other leaves, so the span written back has to reach past
    // the window to cover the half that was scrubbed.
    int firstTouched = from;
    int lastTouched = rightBound - 1;
    scrubMulticellOrphans(rowBuffer, t.columns, blank, &t.textSizeTable, t.ambiguousWidth, firstTouched, lastTouched);
    Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
    t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
    scrubBlockDamage(t, row, row);
}

static void shiftRowCharacters(Ubiquitty::Terminal& t, int n, ShiftDirection direction)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int from = s->cursorX;
    // DECSLRM: ICH/DCH shift within the right margin, no-op when the cursor is
    // outside [scrollLeft, scrollRight] (matching IL/DL). No margins → full width.
    if (from < effectiveLeft(t) || from > effectiveRight(t))
        return;
    const int rightBound = effectiveRight(t) + 1;
    shiftRowCellsAt(t, s->cursorY, from, rightBound, n, direction);
}

void justifyActiveField(Ubiquitty::Terminal& t)
{
    const auto field = activeCharacterField(t);
    if (!field || !t.grid)
        return;
    auto& screen = *t.screens.active;
    int contentEnd = field->followingStop;
    while (contentEnd > field->firstColumn) {
        const Cell* cell = t.grid->logicalCellAt(Row(screen.cursorY), Column(contentEnd - 1));
        if (cell && (cell->codePoint || cellPen(*cell).attributes.contains(VT::Attribute::WideContinuation)))
            break;
        --contentEnd;
    }
    shiftRowCellsAt(t, screen.cursorY, field->firstColumn, field->followingStop,
        field->followingStop - contentEnd, ShiftDirection::InsertRight);
    screen.cursorX = field->followingStop;
    screen.wrapPending = false;
    t.parser.clusterActive = false;
}

// DECIC / DECDC (VT420) — insert/delete N blank columns at the cursor, shifting
// [scrollTop..scrollBottom] × [cursorX..right margin]; revealed columns take
// erase blanks. No-op outside the margins (DEC STD 070). Cursor does not move.
static void shiftColumns(Ubiquitty::Terminal& t, int n, ShiftDirection direction)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (s->cursorY < s->scrollTop || s->cursorY > s->scrollBottom)
        return;
    if (isCursorOutsideMargins(t))
        return;
    const int rightBound = effectiveRight(t) + 1;
    for (int row = s->scrollTop; row <= s->scrollBottom; row++)
        shiftRowCellsAt(t, row, s->cursorX, rightBound, n, direction);
}

void insertColumns(Ubiquitty::Terminal& t, int n) { shiftColumns(t, n, ShiftDirection::InsertRight); }

void deleteColumns(Ubiquitty::Terminal& t, int n) { shiftColumns(t, n, ShiftDirection::DeleteLeft); }

// DECFI (ESC 9) / DECBI (ESC 6) — VT420 forward/back index. Away from the margin
// a plain one-column cursor move; at the right (DECFI) / left (DECBI) margin they
// pan the scroll region one column, revealing an erase-blank at that margin.
// Pans within the margins; outside them, a plain one-column move (DEC STD 070).
void forwardIndex(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int left = effectiveLeft(t);
    const int right = effectiveRight(t);
    if (s->cursorX < right) {
        // On a double-width line the move stops at the last column shown, the
        // line's own right edge (xterm cursor.c, CursorForward: LineMaxCol).
        const bool isDoubled = s->cursorY >= 0 && s->cursorY < t.rows && s->lineAttribute[s->cursorY] != lineAttributeNormal;
        const int lastShown = isDoubled ? std::min(right, t.columns / 2 - 1) : right;
        s->wrapPending = false;
        if (s->cursorX < lastShown)
            s->cursorX++;
        return;
    }
    // DEC STD 070: outside the margins this is a plain one-column move, never a
    // pan; only the PAGE edge suppresses it.
    if (s->cursorX > right) {
        s->wrapPending = false;
        if (s->cursorX < t.columns - 1)
            s->cursorX++;
        return;
    }
    // At the right margin the data moves and the cursor does not, so a pending
    // wrap stays pending (xterm util.c, xtermColScroll leaves do_wrap).
    if (s->cursorY < s->scrollTop || s->cursorY > s->scrollBottom)
        return;
    for (int row = s->scrollTop; row <= s->scrollBottom; row++)
        shiftRowCellsAt(t, row, left, right + 1, 1, ShiftDirection::DeleteLeft);
}

void backIndex(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int left = effectiveLeft(t);
    const int right = effectiveRight(t);
    s->wrapPending = false;
    if (s->cursorX > left) {
        s->cursorX--;
        return;
    }
    // DEC STD 070: outside the margins this is a plain one-column move, never a
    // pan; only the PAGE edge suppresses it.
    if (s->cursorX < left) {
        if (s->cursorX > 0)
            s->cursorX--;
        return;
    }
    if (s->cursorY < s->scrollTop || s->cursorY > s->scrollBottom)
        return;
    for (int row = s->scrollTop; row <= s->scrollBottom; row++)
        shiftRowCellsAt(t, row, left, right + 1, 1, ShiftDirection::InsertRight);
}

void insertCharacters(Ubiquitty::Terminal& t, int n) { shiftRowCharacters(t, n, ShiftDirection::InsertRight); }

void insertCharactersRightToLeft(Ubiquitty::Terminal& terminal, int count)
{
    const auto& screen = *terminal.screens.active;
    if (isCursorOutsideMargins(terminal))
        return;
    shiftRowCellsAt(terminal, screen.cursorY, effectiveLeft(terminal), screen.cursorX + 1, count, ShiftDirection::DeleteLeft);
}

void deleteCharacters(Ubiquitty::Terminal& t, int n) { shiftRowCharacters(t, n, ShiftDirection::DeleteLeft); }

void applyAlternateScreenChange(Ubiquitty::Terminal& t, Ubiquitty::Modes::ScreenChoice screen)
{
    const bool isAlternate = (screen == Ubiquitty::Modes::ScreenChoice::Alternate);
    Ubiquitty::Screen::State* oldScreen;
    int r;
    if (isAlternate == t.screens.isUsingAlternateScreen)
        return;
    t.typography.subscriptionID = 0;
    t.typography.needsReport = false;
    t.richTextMeasurements.clear();
    if (!isAlternate)
        Ubiquitty::RichText::removeOverlays(t);
    oldScreen = t.screens.active;
    t.screens.activate(isAlternate ? Ubiquitty::Modes::ScreenBuffer::Alternate : Ubiquitty::Modes::ScreenBuffer::Primary);
    (void)oldScreen;
    (void)r;
    // CellGrid owns its primary/isAlternate stores + bb-shadow; flipping active is
    // one call and the iterator's (active != bbShadow) diff surfaces the repaint.
    t.grid->setAlternateActive((isAlternate != 0) ? Ubiquitty::ScreenMode::Alternate : Ubiquitty::ScreenMode::Primary);
    // Screen-flip discontinuity: drop the REP predecessor. A DECSET/DECRST flip
    // is recorded by csiDispatch after this returns, so it stays repeatable.
    t.parser.precedingItem.setNone();
    OSC::notifyMousePointerShape(t);
}

} // namespace Ubiquitty::Screen
