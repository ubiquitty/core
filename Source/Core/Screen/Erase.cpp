// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Screen/Erase.cpp — Erase operations — ECH, EL, ED.
//
// Every span honors ECMA guarded areas independently of DECSCA protection.
//
// Cold relative to the per-byte parser loop, which stays in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/RichText/Blocks.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/Triggers/Engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Ubiquitty::Screen {

static void eraseSpanRespectingGuard(Ubiquitty::Terminal& t, int row, int from, int to, const Ubiquitty::Cell& blank)
{
    if (!t.grid || to < from)
        return;
    // An erase that reaches no written cell changes no character a position
    // names: the `\e[K` a program writes past its text, for one.
    if (hasActivePins(t) && from < t.grid->rowContentWidth(Ubiquitty::Row(row))) [[unlikely]]
        notePositionsRewritten(t, row, row);
    // Half a wide character is not half an erase: the glyph is one cell in the
    // model and two columns on screen, so a range that ends inside a pair takes
    // the whole pair.
    expandRangeToWholeRuns(t.grid.get(), row, t.columns, from, to);
    // ERM set (ECMA-48 7.2.4): the contents of protected areas are erased with the rest.
    const bool isGuardIgnored = t.erasureMode == Ubiquitty::VT::ErasureMode::All
        || (t.protectedMode != Ubiquitty::VT::ProtectedMode::ISO && t.protectedMode != Ubiquitty::VT::ProtectedMode::Both);
    if (isGuardIgnored) {
        t.grid->eraseRowRange(Ubiquitty::Row(row), Ubiquitty::Column(from), Ubiquitty::Column(to), blank);
        scrubBlockDamage(t, row, row);
        return;
    }
    for (int column = from; column <= to; column++) {
        const Ubiquitty::Cell* c = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        if (c && Ubiquitty::penIsGuarded(&Ubiquitty::cellPen(*c)))
            continue; // inside a guarded area — survives ED/EL/ECH
        t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), blank);
    }
    scrubBlockDamage(t, row, row);
}

void eraseCharacters(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int end;
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    end = s->cursorX + n;
    if (end > t.columns)
        end = t.columns;
    eraseSpanRespectingGuard(t, s->cursorY, s->cursorX, end - 1, blank);
}

void eraseInLine(Ubiquitty::Terminal& t, int mode)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int from, to;
    Ubiquitty::Cell blank;
    switch (mode) {
    case 0:
        from = s->cursorX;
        to = t.columns - 1;
        break;
    case 1:
        from = 0;
        to = s->cursorX;
        break;
    case 2:
        from = 0;
        to = t.columns - 1;
        break;
    default:
        return;
    }
    Ubiquitty::Screen::eraseCell(&blank, t);
    eraseSpanRespectingGuard(t, s->cursorY, from, to, blank);
    // Erasing the right edge of the row drops any soft-wrap join into
    // the row below; erasing the left edge drops the previous row's
    // join into this one. EL2 (whole row) drops both.
    if (to == t.columns - 1)
        s->lineWrapped[s->cursorY] = false;
    if (from == 0 && s->cursorY > 0)
        s->lineWrapped[s->cursorY - 1] = false;
}

void eraseInField(Ubiquitty::Terminal& t, int mode)
{
    const auto field = activeCharacterField(t);
    if (!field || mode < 0 || mode > 2)
        return;
    const auto& screen = *t.screens.active;
    const int first = mode == 0 ? screen.cursorX : field->firstColumn;
    const int last = mode == 1 ? screen.cursorX : field->followingStop - 1;
    Cell blank;
    eraseCell(&blank, t);
    eraseSpanRespectingGuard(t, screen.cursorY, first, last, blank);
}

static ScreenMode activeScreenMode(const Ubiquitty::Terminal& t)
{
    return t.screens.isUsingAlternateScreen ? ScreenMode::Alternate : ScreenMode::Primary;
}

void defineQualifiedArea(Ubiquitty::Terminal& t, AreaProtection protection)
{
    if (t.statusLineActive)
        return;
    const auto& screen = *t.screens.active;
    // A character in the last column leaves the cursor there with the wrap
    // pending; the active position after it is the first of the next row.
    if (screen.wrapPending && screen.cursorY + 1 < t.rows)
        t.qualifiedAreas.define(activeScreenMode(t), { screen.cursorY + 1, 0, protection });
    else
        t.qualifiedAreas.define(activeScreenMode(t), { screen.cursorY, screen.cursorX, protection });
}

void noteAreaRowsShifted(Ubiquitty::Terminal& t, int top, int bottom, int rows)
{
    if (t.qualifiedAreas.isEmpty() || rows == 0)
        return;
    t.qualifiedAreas.shiftRows(activeScreenMode(t), top, bottom, rows);
}

void scrollAreasIntoHistory(Ubiquitty::Terminal& t, int lines)
{
    if (lines <= 0)
        return;
    t.qualifiedAreas.shiftRows(ScreenMode::Primary, 0, t.rows - 1, lines);
}

void forgetQualifiedAreas(Ubiquitty::Terminal& t)
{
    t.qualifiedAreas.clear();
}

void eraseInArea(Ubiquitty::Terminal& t, int mode)
{
    if (mode < 0 || mode > 2 || t.statusLineActive)
        return;
    const auto& screen = *t.screens.active;
    // The active qualified area begins at the last DAQ at or before the active
    // position and ends before the next one (ECMA-48 6.5.2, 8.3.25); with no
    // DAQ before the position there is no area to erase.
    const ScreenMode which = activeScreenMode(t);
    const int total = t.qualifiedAreas.count(which);
    int holding = -1;
    for (int index = 0; index < total; ++index) {
        if (!t.qualifiedAreas.start(which, index).isAtOrBefore(screen.cursorY, screen.cursorX))
            break;
        holding = index;
    }
    if (holding < 0)
        return;
    const AreaStart area = t.qualifiedAreas.start(which, holding);
    if (area.protection == AreaProtection::Protected && t.erasureMode == Ubiquitty::VT::ErasureMode::Protect)
        return;
    const long columns = t.columns;
    const long pageEnd = static_cast<long>(t.rows) * columns;
    const long cursor = static_cast<long>(screen.cursorY) * columns + screen.cursorX;
    const long begin = static_cast<long>(area.row) * columns + area.column;
    long end = pageEnd;
    if (holding + 1 < total) {
        const AreaStart following = t.qualifiedAreas.start(which, holding + 1);
        end = std::min(pageEnd, static_cast<long>(following.row) * columns + following.column);
    }
    const long first = mode == 0 ? cursor : begin;
    const long last = mode == 1 ? cursor : end - 1;
    if (first > last || first < 0 || last >= pageEnd)
        return;
    Cell blank;
    eraseCell(&blank, t);
    auto& state = *t.screens.active;
    for (long position = first; position <= last;) {
        const int row = static_cast<int>(position / columns);
        const int from = static_cast<int>(position % columns);
        const int to = static_cast<int>(std::min<long>(columns - 1, last - static_cast<long>(row) * columns));
        eraseSpanRespectingGuard(t, row, from, to, blank);
        // As EL: a row whose end is erased no longer continues into the next,
        // and a row whose start is erased is no longer continued from the last.
        if (to == t.columns - 1)
            state.lineWrapped[row] = false;
        if (from == 0 && row > 0)
            state.lineWrapped[row - 1] = false;
        position = static_cast<long>(row + 1) * columns;
    }
}

void eraseInDisplay(Ubiquitty::Terminal& t, int mode)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int rfrom, rto, r;
    switch (mode) {
    case 0:
        eraseInLine(t, 0);
        rfrom = s->cursorY + 1;
        rto = t.rows - 1;
        break;
    case 1:
        eraseInLine(t, 1);
        rfrom = 0;
        rto = s->cursorY - 1;
        break;
    case 2:
        rfrom = 0;
        rto = t.rows - 1;
        break;
    case 3:
        // ED 3, Erase Saved Lines: drop scrollback, leave the screen.
        dropScrollback(&t);
        return;
    default:
        return;
    }
    // Images go with a whole-screen clear (`clear` emits ED 2) and stay through
    // ED 0 and ED 1, which is kitty's rule: its erase-in-display clears the
    // graphics only for modes 2 and 3. A partial erase is what a shell does at
    // every prompt, and the image the shell just printed sits with the cursor
    // on its last row, where xterm leaves it after a sixel. Dropping the
    // placements the erased rows touched took that picture with the prompt's
    // ED 0 and left its rows blank. Sixels anchor to the same grid and follow
    // the same rule.
    if (mode == 2) {
        // The page is cleared, and the boundaries DAQ marked on it go with it.
        t.qualifiedAreas.clear(activeScreenMode(t));
        Ubiquitty::Image::clearPlacementsInRows(t, 0, t.rows - 1);
        Ubiquitty::RichText::removeOverlays(t);
        ++t.eraseDisplayGeneration;
    }
    {
        Ubiquitty::Cell blank;
        Ubiquitty::Screen::eraseCell(&blank, t);
        for (r = rfrom; r <= rto; r++) {
            eraseSpanRespectingGuard(t, r, 0, t.columns - 1, blank);
            // An erased row loses its DECDWL/DECDHL line-attribute and
            // soft-wrap join, else a DHL banner keeps drawing at 2×.
            s->lineAttribute[r] = lineAttributeNormal;
            s->lineWrapped[r] = false;
            // The OSC 133 prompt mark dies with the cells too, or the renderer
            // keeps drawing edge brackets on a blanked row (the stray brackets
            // surviving `clear`).
            s->mark[r] = 0;
        }
        // Clear the join into the topmost cleared row so the surviving
        // predecessor doesn't claim continuation into a blank.
        if (rfrom > 0)
            s->lineWrapped[rfrom - 1] = false;
    }
}

} // namespace Ubiquitty::Screen
