// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/CSI.cpp
//
// Plain (un-prefixed) CSI final-byte handler bodies: cursor motion, erase +
// edit, region scroll, DECSTBM, SCOSC/SCORC, SM/RM, SGR, DSR, DECSSDT/DECSASD,
// the DEC VT300 locator, DECREQTPARM, TBC, DECSTR. Cold relative to the
// per-byte parser loop. The dispatch table + router stay in VT.cpp; the
// handlers it names are declared in VT/Internal.h and defined here.

#include "Base/Bits/OptionSet.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Mouse/Locator.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/SGR.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include "PAL/Capability.h"
#include <cstdint>
#include <cstdio>
#include <new>

namespace Ubiquitty::VT {

// SU/SD scroll their region unconditionally, unlike the line-feed family
// (IND/LF/NEL/VT/FF/RI), which does not scroll at all when DECLRMM is active
// and the cursor sits outside the horizontal margins. Ubiquitty::Screen::scrollUp/Ubiquitty::Screen::scrollDown carry
// that suppression; these entry points (Screen/Scroll.cpp) bypass it.

// Plain CSI final-byte handlers. Handlers take (Terminal*, char final) so cases
// sharing a handler (H and f) can distinguish themselves. The table (in
// VT.cpp) is sorted by `final`; the constexpr jump table that dispatches is
// built from it.
// The editing and erasing controls reset the pending wrap (DEC STD 070's Last
// Column Flag, which xterm resets in the same controls), so a character after
// them overwrites the last column rather than wrapping.
static void cancelPendingWrap(Ubiquitty::Terminal& t) { t.screens.active->wrapPending = false; }

void csiH_ich(Ubiquitty::Terminal& t, char)
{
    if (statusInsertCharacters(t, parameterOr(t, 0, 1)))
        return;
    cancelPendingWrap(t);
    Ubiquitty::Screen::insertCharacters(t, parameterOr(t, 0, 1));
}

void csiH_cuu(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    // CUU stops at the top scroll margin when the cursor starts at or below it
    // (DEC STD 070), independent of origin mode; started above the region it
    // stops at the top of the screen instead.
    const int topLimit = (s->cursorY >= s->scrollTop) ? s->scrollTop : 0;
    s->cursorY -= parameterOr(t, 0, 1);
    if (s->cursorY < topLimit)
        s->cursorY = topLimit;
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
}

void csiH_cud(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    // CUD stops at the bottom scroll margin when the cursor starts at or above
    // it (DEC STD 070), independent of origin mode.
    const int bottomLimit = (s->cursorY <= s->scrollBottom) ? s->scrollBottom : t.rows - 1;
    s->cursorY += parameterOr(t, 0, 1);
    if (s->cursorY > bottomLimit)
        s->cursorY = bottomLimit;
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
}

void cursorForward(Ubiquitty::Terminal& t, int count)
{
    if (statusCursorMoveBy(t, count))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    // A margin bounds only a cursor that starts inside it: begun RIGHT of the
    // right margin, CUF runs to the screen edge instead.
    int limit = effectiveRightVisible(t);
    if (s->cursorX > limit)
        limit = t.columns - 1;
    s->cursorX += count;
    if (s->cursorX > limit)
        s->cursorX = limit;
    s->wrapPending = false;
}

void csiH_cuf(Ubiquitty::Terminal& t, char)
{
    cursorForward(t, parameterOr(t, 0, 1));
}

void csiH_hpr(Ubiquitty::Terminal& t, char)
{
    if (statusCursorMoveBy(t, parameterOr(t, 0, 1)))
        return;
    auto& state = *t.screens.active;
    state.cursorX += parameterOr(t, 0, 1);
    Screen::cursorClampOrigin(t);
    state.wrapPending = false;
}

void csiH_hpb(Ubiquitty::Terminal& t, char)
{
    if (statusCursorMoveBy(t, -parameterOr(t, 0, 1)))
        return;
    auto& state = *t.screens.active;
    state.cursorX -= parameterOr(t, 0, 1);
    Screen::cursorClampOrigin(t);
    state.wrapPending = false;
}

void csiH_vpb(Ubiquitty::Terminal& t, char)
{
    auto& state = *t.screens.active;
    state.cursorY -= parameterOr(t, 0, 1);
    Screen::cursorClampOrigin(t);
    state.wrapPending = false;
    Screen::noteStringStart(t);
}

void csiH_vpr(Ubiquitty::Terminal& t, char)
{
    auto& state = *t.screens.active;
    state.cursorY += parameterOr(t, 0, 1);
    Screen::cursorClampOrigin(t);
    state.wrapPending = false;
    Screen::noteStringStart(t);
}

// Backward cursor motion for CUB and BS.
//
// Rules the naive "step left, clamp to the left margin" formulation gets wrong.
// (1) Begun LEFT of the left margin the move clamps to the screen edge — the
// margin bounds only a cursor that starts inside it. (2) Reverse wraparound
// requires DECAWM as well, and the deferred-wrap latch counts as the first
// cell back: the cursor is logically one past the right margin, so the first
// step only consumes the latch. (3) The two reverse-wrap modes differ as in
// xterm (cursor.c CursorBack): 45 goes back only onto a row that soft-wrapped
// into the one it leaves, and stops at the left margin of the first row of
// the wrapped text; 1045 wraps around the margin rectangle unconditionally,
// computed as a cell offset taken modulo its size, so a wrap off the top
// margin lands on the bottom margin rather than above the region.
void cursorBackward(Ubiquitty::Terminal& t, int n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const bool isExtendedReverseWrap = t.modes.reverseWrapExtended && t.modes.autowrap;
    const bool isReverseWrap = isExtendedReverseWrap || (t.modes.reverseWrap && t.modes.autowrap);
    if (isReverseWrap && s->wrapPending)
        n--;
    s->wrapPending = false;
    if (n <= 0)
        return;
    int left = Ubiquitty::Screen::effectiveLeft(t);
    if (s->cursorX < left)
        left = 0;
    const int next = s->cursorX - n;
    if (next >= left) {
        s->cursorX = next;
        return;
    }
    if (!isReverseWrap) {
        s->cursorX = left;
        return;
    }
    if (!isExtendedReverseWrap) {
        // Step back one row at a time while the row above soft-wrapped into
        // the current one; a row that ended with a line break stops the walk.
        const int right = Ubiquitty::Screen::effectiveRight(t);
        int remaining = n;
        int column = s->cursorX;
        int row = s->cursorY;
        while (remaining > column - left) {
            const int previous = row - 1;
            if (previous < 0 || !s->lineWrapped[previous]) {
                column = left;
                remaining = 0;
                break;
            }
            remaining -= column - left + 1;
            row = previous;
            column = right;
        }
        s->cursorY = row;
        s->cursorX = column - remaining;
        return;
    }
    const int inRow = Ubiquitty::Screen::effectiveRight(t) - left + 1;
    const int top = s->scrollTop;
    int offset = inRow * (s->cursorY - top) + (next - left);
    if (offset < 0) {
        const int length = inRow * (s->scrollBottom - top + 1);
        offset += ((-offset) / length + 1) * length;
    }
    s->cursorY = offset / inRow + top;
    s->cursorX = offset % inRow + left;
}

void csiH_cub(Ubiquitty::Terminal& t, char)
{
    if (statusCursorMoveBy(t, -parameterOr(t, 0, 1)))
        return;
    cursorBackward(t, parameterOr(t, 0, 1));
}

// CR column, shared by CNL/CPL: the left margin when the cursor is at or right
// of it, else column 0. Ubiquitty::Screen::effectiveLeft is 0 with DECLRMM off, so this is a plain
// column-0 CR there.
static int carriageReturnColumn(const Ubiquitty::Terminal& t)
{
    return Screen::lineHomeColumn(t);
}

// CNL / CPL are CUD / CUU followed by a CR: the vertical move stops at the
// scroll margin only when the cursor starts inside the region — begun outside it
// runs to the screen edge (as in csiH_cud / csiH_cuu) — and the column lands on
// the left margin, not unconditionally on column 1.
void csiH_cnl(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int bottomLimit = (s->cursorY <= s->scrollBottom) ? s->scrollBottom : t.rows - 1;
    s->cursorY += parameterOr(t, 0, 1);
    if (s->cursorY > bottomLimit)
        s->cursorY = bottomLimit;
    s->cursorX = carriageReturnColumn(t);
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
}

void csiH_cpl(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int topLimit = (s->cursorY >= s->scrollTop) ? s->scrollTop : 0;
    s->cursorY -= parameterOr(t, 0, 1);
    if (s->cursorY < topLimit)
        s->cursorY = topLimit;
    s->cursorX = carriageReturnColumn(t);
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
}

void csiH_cha(Ubiquitty::Terminal& t, char)
{
    // CHA — column absolute. Not restricted by DECSLRM; clip to full
    // screen so the cursor can land outside the margins. Under DECOM the column
    // is relative to the left margin, as for HPA.
    if (statusCursorMoveTo(t, parameterOr(t, 0, 1) - 1))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorX = parameterOr(t, 0, 1) - 1;
    if (t.modes.leftRightMarginMode && t.modes.originMode)
        s->cursorX += s->scrollLeft;
    cursorClampFull(t);
    s->wrapPending = false;
}

void csiH_cup(Ubiquitty::Terminal& t, char final)
{
    // Status display: one row — only the column component applies.
    if (statusCursorMoveTo(t, parameterOr(t, 1, 1) - 1))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    int p0 = parameterOr(t, 0, 1), p1 = parameterOr(t, 1, 1);
    s->cursorY = p0 - 1;
    s->cursorX = p1 - 1;
    if (t.modes.originMode) {
        s->cursorY += s->scrollTop;
        if (t.modes.leftRightMarginMode)
            s->cursorX += s->scrollLeft;
    }
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
    if (final == 'f')
        Screen::noteStringStart(t);
}

void csiH_cht(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int p0 = parameterOr(t, 0, 1);
    int rightBound = effectiveRightVisible(t);
    for (int i = 0; i < p0; i++) {
        int x = s->cursorX + 1;
        while (x <= rightBound && !t.tabStops[x])
            x++;
        s->cursorX = (x > rightBound) ? rightBound : x;
    }
    // Like HT, CHT leaves a pending wrap armed (xterm's TabToNextStop moves
    // with set_cur_col, which keeps do_wrap).
}

void csiH_cvt(Ubiquitty::Terminal& t, char)
{
    auto& screen = *t.screens.active;
    int remaining = parameterOr(t, 0, 1);
    int destination = screen.cursorY;
    for (int row = screen.cursorY + 1; row < t.rows && remaining > 0; ++row) {
        if (t.lineTabStops[static_cast<std::size_t>(row)]) {
            destination = row;
            --remaining;
        }
    }
    if (destination == screen.cursorY)
        return;
    screen.cursorY = destination;
    screen.wrapPending = false;
    cursorClampFull(t);
}

void csiH_ctc(Ubiquitty::Terminal& t, char)
{
    const auto& screen = *t.screens.active;
    const auto column = static_cast<std::size_t>(screen.cursorX);
    const auto row = static_cast<std::size_t>(screen.cursorY);
    for (int index = 0; index < std::max(1, t.parser.parameterCount); ++index) {
        switch (parameterOr(t, index, 0)) {
        case 0:
            t.tabStops[column] = true;
            if (!t.tabAlignments.empty()) {
                t.tabAlignments[column] = CharacterTabAlignment::Plain;
                if (!t.tabAlignmentTargets.empty())
                    t.tabAlignmentTargets[column] = 0;
            }
            break;
        case 1:
            t.lineTabStops[row] = true;
            break;
        case 2:
            t.tabStops[column] = false;
            break;
        case 3:
            t.lineTabStops[row] = false;
            break;
        case 4:
        case 5:
            t.tabStops.reset();
            t.tabAlignments.clear();
            t.tabAlignmentTargets.clear();
            break;
        case 6:
            t.lineTabStops.reset();
            break;
        default:
            break;
        }
    }
}

void csiH_tsr(Ubiquitty::Terminal& t, char)
{
    const int position = parameterOr(t, 0, 0);
    if (position < 1 || position > t.columns)
        return;
    t.tabStops[static_cast<std::size_t>(position - 1)] = false;
}

void csiH_slh(Ubiquitty::Terminal& t, char)
{
    const int position = parameterOr(t, 0, 0);
    if (position >= 1 && position <= t.columns)
        t.lineHome = position - 1;
}

// SLL: the one-based position CR and NEL move to when SIMD selects the line
// limit. A position outside the row, or a parameter list longer than SLL's one
// parameter, leaves the limit as it was, as SLH does.
void csiH_sll(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount > 1)
        return;
    const int position = parameterOr(t, 0, 0);
    if (position >= 1 && position <= t.columns)
        t.lineLimit = position;
}

// SIMD: 0 keeps the implicit movement along the character progression and 1
// reverses it. The terminal has one component, so no data moves.
void csiH_simd(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount > 1)
        return;
    switch (parameterOr(t, 0, 0)) {
    case 0:
        t.implicitMovementDirection = VT::ImplicitMovementDirection::WithProgression;
        break;
    case 1:
        t.implicitMovementDirection = VT::ImplicitMovementDirection::AgainstProgression;
        break;
    default:
        break;
    }
}

// SPD: the grid is always horizontal, top to bottom and left to right, so Ps1
// selects nothing it does not already have. Ps2 = 1 and 2 update one component
// from the other, which are the same grid here, and move the active position to
// the first position of the first line.
void csiH_spd(Ubiquitty::Terminal& t, char)
{
    if (t.statusLineActive || t.parser.parameterCount > 2)
        return;
    const int lastLineOrientation = 7;
    if (parameterOr(t, 0, 0) > lastLineOrientation)
        return;
    const int update = parameterOr(t, 1, 0);
    if (update != 1 && update != 2)
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorX = 0;
    s->cursorY = 0;
    s->wrapPending = false;
}

namespace {

enum class StringLayout {
    FlushToHome,
    Centre,
    FlushToLimit,
};

} // namespace

// Moves the string between the recorded start and the cursor so that it
// satisfies the layout, and puts the cursor after it. A string that does not fit
// between the margins stays where it is.
static void positionString(Ubiquitty::Terminal& t, StringLayout layout)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int row = s->cursorY;
    const int rowWidth = t.columns / textScaleForLineAttribute(s->lineAttribute[row]).widthScale;
    const int start = t.stringStartColumn;
    const int end = std::min(s->cursorX + (s->wrapPending ? 1 : 0), rowWidth);
    if (start < 0 || start >= end)
        return;
    const int length = end - start;
    const int home = Screen::lineHomeColumn(t);
    const int limit = Screen::lineLimitColumn(t);
    int destination = start;
    switch (layout) {
    case StringLayout::FlushToHome:
        destination = home;
        break;
    case StringLayout::Centre:
        destination = home + (limit - home + 1 - length) / 2;
        break;
    case StringLayout::FlushToLimit:
        destination = limit + 1 - length;
        break;
    }
    const bool isOutsideMargins = layout != StringLayout::FlushToHome && (limit - home + 1 < length);
    if (isOutsideMargins || destination < 0 || destination + length > rowWidth || destination == start)
        return;
    Ubiquitty::Screen::rectCopy(t, { row, start, row, end - 1 }, row + 1, destination + 1);
    const int vacatedFirst = destination > start ? start : std::max(start, destination + length);
    const int vacatedLast = destination > start ? std::min(end - 1, destination - 1) : end - 1;
    if (vacatedFirst <= vacatedLast)
        Ubiquitty::Screen::rectErase(t, { row, vacatedFirst, row, vacatedLast });
    const int newEnd = end + destination - start;
    s->cursorX = std::min(newEnd, rowWidth - 1);
    s->wrapPending = newEnd >= rowWidth;
}

// QUAD ends a string of graphic characters and positions it on the line: 0, 2
// and 4 flush it to the line home, centre it between the margins and flush it to
// the line limit. 1, 3 and 5 ask for a leader and 6 for both margins; neither
// has a pattern or a spacing in the standard, so they end the string and leave
// it where it is. Left-to-right writing only.
void csiH_quad(Ubiquitty::Terminal& t, char)
{
    if (t.statusLineActive)
        return;
    const int lastLayout = 6;
    const int layout = t.parser.parameterCount > 1 ? lastLayout + 1 : parameterOr(t, 0, 0);
    if (layout > lastLayout)
        return;
    if (!t.shouldWriteRightToLeft) {
        switch (layout) {
        case 0:
            positionString(t, StringLayout::FlushToHome);
            break;
        case 2:
            positionString(t, StringLayout::Centre);
            break;
        case 4:
            positionString(t, StringLayout::FlushToLimit);
            break;
        default:
            break;
        }
    }
    Ubiquitty::Screen::noteStringStart(t);
}

// ED and EL apply each selective parameter in turn, as DEC STD 070's
// erase_in_display and erase_in_line loop over every parameter and the VT100
// User Guide (3.3.3) makes CSI Psa;Psb F identical to CSI Psa F CSI Psb F.
// xterm applies only the first.
void csiH_ed(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount <= 1) {
        if (statusEraseInDisplay(t, parameterOr(t, 0, 0)))
            return;
        cancelPendingWrap(t);
        Ubiquitty::Screen::eraseInDisplay(t, parameterOr(t, 0, 0));
        return;
    }
    for (int i = 0; i < t.parser.parameterCount; i++) {
        if (statusEraseInDisplay(t, parameterOr(t, i, 0)))
            continue;
        cancelPendingWrap(t);
        Ubiquitty::Screen::eraseInDisplay(t, parameterOr(t, i, 0));
    }
}

static void eraseInLineOrStatus(Ubiquitty::Terminal& t, int mode)
{
    if (statusEraseInLine(t, mode))
        return;
    cancelPendingWrap(t);
    Ubiquitty::Screen::eraseInLine(t, mode);
}

void csiH_el(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount <= 1) {
        eraseInLineOrStatus(t, parameterOr(t, 0, 0));
        return;
    }
    for (int i = 0; i < t.parser.parameterCount; i++)
        eraseInLineOrStatus(t, parameterOr(t, i, 0));
}

void csiH_ef(Ubiquitty::Terminal& t, char)
{
    Screen::eraseInField(t, parameterOr(t, 0, 0));
}

void csiH_ea(Ubiquitty::Terminal& t, char)
{
    Screen::eraseInArea(t, parameterOr(t, 0, 0));
}

void csiH_il(Ubiquitty::Terminal& t, char)
{
    cancelPendingWrap(t);
    Ubiquitty::Screen::insertLines(t, parameterOr(t, 0, 1));
}

void csiH_dl(Ubiquitty::Terminal& t, char)
{
    cancelPendingWrap(t);
    Ubiquitty::Screen::deleteLines(t, parameterOr(t, 0, 1));
}

void csiH_dch(Ubiquitty::Terminal& t, char)
{
    if (statusDeleteCharacters(t, parameterOr(t, 0, 1)))
        return;
    cancelPendingWrap(t);
    Ubiquitty::Screen::deleteCharacters(t, parameterOr(t, 0, 1));
}

void csiH_su(Ubiquitty::Terminal& t, char) { Ubiquitty::Screen::scrollRegionUp(t, parameterOr(t, 0, 1)); }

// CSI Ps T = SD (scroll down). The 5-param form is the XTHIMOUSE highlight-
// region reply for hilite tracking (mode 1001): cache the region and return
// IMMEDIATELY — the parser never blocks waiting on the host, so a host that
// never sends the region cannot wedge the byte stream.
void csiH_sd(Ubiquitty::Terminal& t, char)
{
    if (t.modes.mouseHilite && t.parser.parameterCount >= 5) {
        t.modes.hiliteStartX = parameterOr(t, 1, 1);
        t.modes.hiliteStartY = parameterOr(t, 2, 1);
        t.modes.hiliteFirstRow = parameterOr(t, 3, 1);
        t.modes.hiliteLastRow = parameterOr(t, 4, 1);
        // Pf (param 0) nonzero starts highlighting; 0 aborts it.
        t.modes.hiliteRegionValid = (parameterOr(t, 0, 0) != 0);
        // The handshake completed (reply arrived before the deadline): drop the
        // bounded wait so tickHilite stops watching it.
        t.modes.hiliteAwaiting = false;
        return;
    }
    // Five or more parameters are XTHIMOUSE's form (func ; startx ; starty ;
    // first ; last), which with no highlight pending scrolls nothing. SD takes
    // one parameter (ECMA-48 8.3.113), so a second to fourth one is ignored and
    // SD scrolls, and a lone 0 is SD's default of one line.
    constexpr int highlightParameterCount = 5;
    if (t.parser.parameterCount >= highlightParameterCount)
        return;
    Ubiquitty::Screen::scrollRegionDown(t, parameterOr(t, 0, 1));
}

void csiH_ech(Ubiquitty::Terminal& t, char)
{
    if (statusEraseCharacters(t, parameterOr(t, 0, 1)))
        return;
    cancelPendingWrap(t);
    Ubiquitty::Screen::eraseCharacters(t, parameterOr(t, 0, 1));
}

// CBT is margin-blind: it ignores the DECSLRM left margin and walks back
// through stops to column 1 even from inside the margins. Origin mode is the
// exception, because there the cursor cannot leave the margins at all: it
// stops at the left margin, as xterm's does.
void csiH_cbt(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int leftmost = t.modes.originMode ? Ubiquitty::Screen::effectiveLeft(t) : 0;
    int p0 = parameterOr(t, 0, 1);
    for (int i = 0; i < p0; i++) {
        int x = s->cursorX - 1;
        while (x > leftmost && !t.tabStops[x])
            x--;
        s->cursorX = (x < leftmost) ? leftmost : x;
    }
}

// ECMA-48 §8.3.103: Pn defaults to 1 when OMITTED; an explicit 0 repeats
// nothing. parameterOr is unusable here: its 0-coerces-to-default rule would
// turn an explicit REP 0 into REP 1.
void csiH_rep(Ubiquitty::Terminal& t, char) { repeatPrecedingCharacter(t, t.parser.parameterCount == 0 ? 1 : t.parser.parameters[0]); }

void csiH_vpa(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorY = parameterOr(t, 0, 1) - 1;
    if (t.modes.originMode)
        s->cursorY += s->scrollTop;
    Screen::cursorClampOrigin(t);
    s->wrapPending = false;
    Screen::noteStringStart(t);
}

// DECSSDT (CSI Ps $ ~) — Select Status CellGrid Type.
// 0 = none, 1 = indicator, 2 = host-writable.
void csiH_decssdt(Ubiquitty::Terminal& t, char)
{
    int p = parameterOr(t, 0, 0);
    if (p > 2)
        return;
    if (t.statusLineType != p) {
        t.statusLineType = static_cast<uint8_t>(p);
        t.statusRow.reset();
        t.statusCursorX = 0;
    }
    if (p != 2)
        t.statusLineActive = 0;
}

// ---- DECSASD status-display routing (see VT/Internal.h) ----
// One row of plain Cells with its own cursor column; erases take the current
// pen's colors through eraseCell, matching the data screen's blank convention.

static int statusClampColumn(const Ubiquitty::Terminal& t, int column)
{
    if (column < 0)
        return 0;
    if (column > t.columns - 1)
        return t.columns - 1;
    return column;
}

bool statusCursorMoveTo(Ubiquitty::Terminal& t, int column)
{
    if (!t.statusLineActive)
        return false;
    t.statusCursorX = statusClampColumn(t, column);
    return true;
}

bool statusCursorMoveBy(Ubiquitty::Terminal& t, int delta)
{
    if (!t.statusLineActive)
        return false;
    t.statusCursorX = statusClampColumn(t, t.statusCursorX + delta);
    return true;
}

bool statusTab(Ubiquitty::Terminal& t)
{
    if (!t.statusLineActive)
        return false;
    int x = statusClampColumn(t, t.statusCursorX) + 1;
    while (x < t.columns - 1 && !t.tabStops[x])
        x++;
    t.statusCursorX = statusClampColumn(t, x);
    return true;
}

bool statusEraseInLine(Ubiquitty::Terminal& t, int mode)
{
    if (!t.statusLineActive)
        return false;
    const Ubiquitty::Rendering::StatusRowCells statusCells = Ubiquitty::Rendering::ensureStatusRow(t);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    const int x = statusClampColumn(t, t.statusCursorX);
    int from = 0, to = t.columns - 1;
    if (mode == 0)
        from = x; // cursor to end
    else if (mode == 1)
        to = x; // start through cursor
    for (int i = from; i <= to; i++)
        statusCells[i] = blank;
    return true;
}

bool statusEraseInDisplay(Ubiquitty::Terminal& t, int mode)
{
    if (!t.statusLineActive)
        return false;
    // The status display is one row, so ED's three modes erase the positions
    // EL's do. ED 3 names saved lines, which the status display does not have.
    if (mode >= 0 && mode <= 2)
        statusEraseInLine(t, mode);
    return true;
}

bool statusEraseCharacters(Ubiquitty::Terminal& t, int n)
{
    if (!t.statusLineActive)
        return false;
    const Ubiquitty::Rendering::StatusRowCells statusCells = Ubiquitty::Rendering::ensureStatusRow(t);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    const int x = statusClampColumn(t, t.statusCursorX);
    int end = x + n;
    if (end > t.columns)
        end = t.columns;
    for (int i = x; i < end; i++)
        statusCells[i] = blank;
    return true;
}

bool statusInsertCharacters(Ubiquitty::Terminal& t, int n)
{
    if (!t.statusLineActive)
        return false;
    const Ubiquitty::Rendering::StatusRowCells statusCells = Ubiquitty::Rendering::ensureStatusRow(t);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    const int x = statusClampColumn(t, t.statusCursorX);
    if (n > t.columns - x)
        n = t.columns - x;
    for (int i = t.columns - 1; i >= x + n; i--)
        statusCells[i] = statusCells[i - n];
    for (int i = x; i < x + n; i++)
        statusCells[i] = blank;
    return true;
}

bool statusDeleteCharacters(Ubiquitty::Terminal& t, int n)
{
    if (!t.statusLineActive)
        return false;
    const Ubiquitty::Rendering::StatusRowCells statusCells = Ubiquitty::Rendering::ensureStatusRow(t);
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    const int x = statusClampColumn(t, t.statusCursorX);
    if (n > t.columns - x)
        n = t.columns - x;
    for (int i = x; i < t.columns - n; i++)
        statusCells[i] = statusCells[i + n];
    for (int i = t.columns - n; i < t.columns; i++)
        statusCells[i] = blank;
    return true;
}

// DECSASD (CSI Ps $ }) — Select Active Status CellGrid.
// 0 = data screen receives writes; 1 = status line receives writes.
void csiH_decsasd(Ubiquitty::Terminal& t, char)
{
    int p = parameterOr(t, 0, 0);
    if (p == 0)
        t.statusLineActive = 0;
    else if (p == 1 && t.statusLineType == 2)
        t.statusLineActive = 1;
}

// DECAC (CSI Ps1 ; Pf ; Pb , |) — Assign Color, VT520: item 1 is normal text,
// item 2 the window frame, each a foreground and a background index 0..15.
// The manual gives no parameter a default, so anything but an item and two
// indices is ignored whole. The normal text colours are the default ones, so
// every pen and every cell of either screen holding the default, or the
// colours an earlier assignment resolved in its place, takes them.
void csiH_decac(Ubiquitty::Terminal& t, char)
{
    constexpr int lastColorIndex = 15;
    if (t.parser.parameterCount != 3)
        return;
    const int item = parameterOr(t, 0, 0);
    const int foreground = parameterOr(t, 1, 0);
    const int background = parameterOr(t, 2, 0);
    if (foreground > lastColorIndex || background > lastColorIndex)
        return;
    const Ubiquitty::ColorAssignment assigned { static_cast<uint16_t>(foreground), static_cast<uint16_t>(background) };
    if (item == 2) {
        t.windowFrameColors = assigned;
        return;
    }
    if (item != 1)
        return;
    t.normalTextColors = assigned;
    Ubiquitty::Screen::applyNormalTextColors(t, assigned);
    for (Ubiquitty::Screen::State* screen : { &t.screens.primary, &t.screens.alternate }) {
        Ubiquitty::penApplyNormalTextColors(&screen->pen, assigned);
        Ubiquitty::penApplyNormalTextColors(&screen->savedPen, assigned);
    }
}

// DECATC (CSI Ps1 ; Pf ; Pb , }) — Alternate Text Color, VT520: Ps1 is one of
// the 16 attribute combinations, Pf and Pb colour indices 0..15. The manual
// gives no parameter a default, so anything but a combination and two indices
// is ignored whole. The table is held for DECRQSS; the VT520 draws from it
// only under its alternate colour look-up table, which Core does not offer.
void csiH_decatc(Ubiquitty::Terminal& t, char)
{
    constexpr int lastColorIndex = 15;
    if (t.parser.parameterCount != 3)
        return;
    const int combination = parameterOr(t, 0, 0);
    const int foreground = parameterOr(t, 1, 0);
    const int background = parameterOr(t, 2, 0);
    if (combination < 0 || combination >= static_cast<int>(Ubiquitty::alternateTextCombinationCount) || foreground > lastColorIndex || background > lastColorIndex)
        return;
    if (!t.alternateTextColors) {
        // A refused allocation leaves the table unassigned, as an ignored DECATC does.
        t.alternateTextColors.reset(new (std::nothrow) Ubiquitty::AlternateTextColors(Ubiquitty::unassignedAlternateTextColors));
        if (!t.alternateTextColors)
            return;
    }
    (*t.alternateTextColors)[static_cast<std::size_t>(combination)] = Ubiquitty::ColorAssignment { static_cast<uint16_t>(foreground), static_cast<uint16_t>(background) };
}

// DEC VT300 locator protocol — CSI ' <final>: ' z DECELR, ' { DECSLE,
// ' | DECRQLP, ' w DECEFR. The button/motion → DECLRP emit path is
// locatorReportEvent (frontend mouse handler); the parser only stores state
// + answers DECRQLP.
void csiH_decelr(Ubiquitty::Terminal& t, char)
{
    int ps1 = parameterOr(t, 0, 0);
    int ps2 = parameterOr(t, 1, 0);
    // Ps1: 0 = disable (default), 1 = enable, 2 = enable for one report then
    // auto-disable. Enabling cancels any armed filter rectangle (a fresh
    // DECELR re-arms reporting from a clean slate).
    if (ps1 == 1) {
        t.locatorEnabled = true;
        t.locatorOneShot = false;
    } else if (ps1 == 2) {
        t.locatorEnabled = true;
        t.locatorOneShot = true;
    } else {
        t.locatorEnabled = false;
        t.locatorOneShot = false;
    }
    t.locatorFilterActive = false;
    // Ps2 coordinate unit: 0/2 = character cells (default), 1 = device pixels.
    t.locatorUseRows = (ps2 != 1);
}

void csiH_decsle(Ubiquitty::Terminal& t, char)
{
    // DECSLE selects which locator transitions emit an unsolicited DECLRP.
    // One code per invocation, but a list is honoured so an app can send
    // "1;3" to enable both:
    //   0  report only on explicit DECRQLP; also cancels the filter rectangle
    //   1  report button-down transitions      2  do not report button-down
    //   3  report button-up transitions        4  do not report button-up
    int count = t.parser.parameterCount > 0 ? t.parser.parameterCount : 1;
    for (int i = 0; i < count; i++) {
        switch (parameterOr(t, i, 0)) {
        case 0:
            t.locatorReportDown = false;
            t.locatorReportUp = false;
            t.locatorFilterActive = false;
            break;
        case 1:
            t.locatorReportDown = true;
            break;
        case 2:
            t.locatorReportDown = false;
            break;
        case 3:
            t.locatorReportUp = true;
            break;
        case 4:
            t.locatorReportUp = false;
            break;
        }
    }
}

void csiH_decrqlp(Ubiquitty::Terminal& t, char)
{
    // DECRQLP (CSI Ps ' |) — transmit one DECLRP now. DEC STD 070: a disabled
    // locator answers Pe = 0 (unavailable), so the host never blocks; an
    // enabled one answers Pe = 1. Pr;Pc are the LOCATOR's coordinates, not the
    // text cursor's, so with no pointer event published yet they are omitted,
    // the position undefined, rather than a cursor stand-in that reads as a
    // real pointer. That reply is still the one report a one-shot DECELR
    // (Ps1 = 2) allows.
    char buffer[64];
    if (!t.locatorEnabled) {
        respond(t, Ubiquitty::Capability::LocatorReport, "\033[0&w");
        return;
    }
    int n = Ubiquitty::Mouse::Locator::encodeReport(/*event*/ 1, t.locatorButtons, t.locatorRow, t.locatorColumn, /*page*/ 1, buffer);
    if (n > 0) {
        buffer[n] = 0;
        respond(t, Ubiquitty::Capability::LocatorReport, buffer);
    }
    if (t.locatorOneShot) {
        t.locatorEnabled = false;
        t.locatorOneShot = false;
    }
}

// DECEFR (CSI Pt ; Pl ; Pb ; Pr ' w) — enable a filter rectangle. Armed, the
// locator emits one DECLRP (Pe = 10) the first time it leaves the rect, then
// disarms. An omitted bound defaults to the CURRENT locator position;
// the coordinates are 1-based, so a literal 0 is never a real bound and is the
// same case. That is what makes the all-omitted form `CSI ' w` mean "report any
// locator motion": the rect collapses onto the pointer, so the next move leaves
// it. With no position published yet the omitted edge stays unbounded, which is
// unit-agnostic (DECELR Ps2 may have selected pixels, not cells).
// No-op unless the locator is enabled (DECELR).
void csiH_decefr(Ubiquitty::Terminal& t, char)
{
    if (!t.locatorEnabled)
        return;
    const int here = t.locatorRow;
    const int hereColumn = t.locatorColumn;
    const int top = parameterOr(t, 0, 0);
    const int left = parameterOr(t, 1, 0);
    const int bottom = parameterOr(t, 2, 0);
    const int right = parameterOr(t, 3, 0);
    t.locatorFilterTop = static_cast<uint16_t>(top ? top : (here ? here : 1));
    t.locatorFilterLeft = static_cast<uint16_t>(left ? left : (hereColumn ? hereColumn : 1));
    t.locatorFilterBottom = static_cast<uint16_t>(bottom ? bottom : (here ? here : 0xFFFF));
    t.locatorFilterRight = static_cast<uint16_t>(right ? right : (hereColumn ? hereColumn : 0xFFFF));
    t.locatorFilterActive = true;
}

// DECRQKT (CSI Ps , u) — Key Type Inquiry. Answers DECRPKT (CSI Ps1 ; Ps2 , v):
// Ps1 echoes keystation Ps, Ps2 is the type (0 = alphanumeric, 1 = function).
// Approximated from the keystation-map split: stations 1..90 report
// alphanumeric, above 90 function, 0/out-of-range alphanumeric (🟡 parity).
void csiH_decrqkt(Ubiquitty::Terminal& t, char)
{
    int station = parameterOr(t, 0, 0);
    int keyType = (station >= 1 && station <= 90) ? 0 : (station > 90 ? 1 : 0);
    char buffer[32];
    int n = std::snprintf(buffer, sizeof(buffer), "\033[%d;%d,v", station, keyType);
    if (n > 0 && n < static_cast<int>(sizeof(buffer)))
        respond(t, Ubiquitty::Capability::StatusReport, buffer);
}

// HPA (CSI Ps `) — Horizontal Position Absolute. Same column-absolute move as
// CHA. Not restricted by DECSLRM.
void csiH_hpa(Ubiquitty::Terminal& t, char)
{
    if (statusCursorMoveTo(t, parameterOr(t, 0, 1) - 1))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorX = parameterOr(t, 0, 1) - 1;
    if (t.modes.leftRightMarginMode && t.modes.originMode)
        s->cursorX += s->scrollLeft;
    cursorClampFull(t);
    s->wrapPending = false;
}

// DECREQTPARM (CSI Ps x) — request terminal parameters. Reply CSI Psol; Ppar;
// Pnbits; Pxspeed; Prspeed; Pclkmul; Pflags x; the request Ps becomes Psol
// (2 = unsolicited, 3 = solicited). Hard-coded "8N1, 9600/9600, x1, no flags".
void csiH_decreqtparm(Ubiquitty::Terminal& t, char)
{
    const int request = parameterOr(t, 0, 0);
    if (request > 1)
        return;
    int sol = request == 1 ? 3 : 2;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "\033[%d;1;1;128;128;1;0x", sol);
    respond(t, Ubiquitty::Capability::DeviceAttributesRead, buffer);
}

void csiH_tbc(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int p = parameterOr(t, 0, 0);
    switch (p) {
    case 0:
        t.tabStops[static_cast<std::size_t>(s->cursorX)] = false;
        break;
    case 1:
        t.lineTabStops[static_cast<std::size_t>(s->cursorY)] = false;
        break;
    case 2:
    case 3:
        t.tabStops.reset();
        t.tabAlignments.clear();
        t.tabAlignmentTargets.clear();
        break;
    case 4:
        t.lineTabStops.reset();
        break;
    case 5:
        t.tabStops.reset();
        t.tabAlignments.clear();
        t.tabAlignmentTargets.clear();
        t.lineTabStops.reset();
        break;
    default:
        break;
    }
}

void csiH_sm(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        setANSIMode(t, t.parser.parameters[i], Ubiquitty::Modes::ModeChange::Set);
}

void csiH_rm(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        setANSIMode(t, t.parser.parameters[i], Ubiquitty::Modes::ModeChange::Clear);
}

void csiH_sgr(Ubiquitty::Terminal& t, char)
{
    // GRCM reset (ECMA-48 7.2.8): this SGR cancels the earlier rendition, and the
    // aspects it names are then selected over the initial one.
    if (t.renditionCombination == VT::RenditionCombination::Replacing)
        penResetRendition(&t.screens.active->pen);
    // SGR sees DECAC's normal text colours as the default they stand in for, so
    // a colour it selects is explicit even at the assigned index.
    Ubiquitty::penReturnNormalTextColorsToDefault(&t.screens.active->pen);
    Ubiquitty::SGR::apply(t);
    Ubiquitty::penApplyNormalTextColors(&t.screens.active->pen, t.normalTextColors);
    // A run held open across this SGR (the Ground fast-path's lookahead)
    // survives only a blank-invariant pen change; a BCE-relevant one
    // commits here, after apply, when the new blank is knowable.
    Ubiquitty::Screen::scrollLaneFlushIfBlankChanged(t);
}

void csiH_decstbm(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int p0 = parameterOr(t, 0, 1), p1 = parameterOr(t, 1, t.rows);
    int top = p0 - 1;
    int bottom = p1 - 1;
    if (top < 0)
        top = 0;
    // Per DEC STD 070: an invalid region (top >= bottom, or a bottom margin
    // past the last line) is IGNORED — margins and cursor unchanged. xterm
    // takes a bottom past the screen as the last line (charproc.c
    // CASE_DECSTBM); Core follows the standard. A no-parameter CSI r resolves
    // to the full page (valid), so it still resets + homes.
    if (bottom >= t.rows || top >= bottom)
        return;
    s->scrollTop = top;
    s->scrollBottom = bottom;
    s->cursorX = 0;
    s->cursorY = t.modes.originMode ? s->scrollTop : 0;
    // The home is a cursor move like any other: a wrap armed by the previous
    // cell must not fire from the new position.
    s->wrapPending = false;
}

void csiH_scosc(Ubiquitty::Terminal& t, char)
{
    // CSI s is overloaded: SCOSC (save cursor, VT100) vs DECSLRM (set margins,
    // VT420). It is DECSLRM whenever DECLRMM (DECSET 69) is active at
    // VT400+ (even with no parameters — resets margins to full width and homes);
    // SCOSC only when DECLRMM is off.
    if (t.modes.leftRightMarginMode && t.conformanceLevel >= VT::ConformanceLevel::VT400) {
        Ubiquitty::Screen::State* s = t.screens.active;
        const int leftParameter = parameterOr(t, 0, 0);
        int left = (leftParameter > 0) ? leftParameter - 1 : 0;
        int right = (t.parser.parameterCount > 1 && t.parser.parameters[1] > 0) ? t.parser.parameters[1] - 1 : t.columns - 1;
        if (left < 0)
            left = 0;
        if (right >= t.columns)
            right = t.columns - 1;
        // DEC STD 070 ignores DECSLRM whose left margin is not left of its
        // right, as it ignores the same DECSTBM: nothing changes and the
        // cursor stays. xterm ignores it too (charproc.c CASE_ANSI_SC).
        if (left >= right)
            return;
        s->scrollLeft = left;
        s->scrollRight = right;
        // Narrower than the screen is a pane: keep the pair for the row-join
        // heuristic, which otherwise knows only the screen's own edges.
        if (right - left + 1 < t.columns)
            Ubiquitty::Screen::rememberMargins(*s, left, right);
        // Spec: after DECSLRM, cursor moves to home (origin if
        // DECOM, else absolute 0,0).
        s->cursorX = t.modes.originMode ? s->scrollLeft : 0;
        s->cursorY = t.modes.originMode ? s->scrollTop : 0;
        // The home is a cursor move like any other: a wrap armed by the
        // previous cell must not fire from the new position.
        s->wrapPending = false;
        return;
    }
    // With DECLRMM reset, `CSI Pl ; Pr s` is a DECSLRM the terminal ignores;
    // only the bare `CSI s` saves the cursor, as in xterm.
    if (t.parser.parameterCount > 1 || (t.parser.parameterCount == 1 && !(t.parser.parameterWasEmpty & 1u)))
        return;
    Ubiquitty::Screen::saveCursor(t);
}

void csiH_scorc(Ubiquitty::Terminal& t, char) { Ubiquitty::Screen::restoreCursor(t); }

void csiH_decstr(Ubiquitty::Terminal& t, char) { softResetInternal(t); }

// SL / SR — ECMA-48 Scroll Left / Right (CSI Ps SP @ / A). Shift the scroll
// region's cells horizontally by n columns, filling vacated cells with the
// current pen's blank. Honors DECSTBM and DECSLRM margins.
void csiH_sl(Ubiquitty::Terminal& t, char) { Ubiquitty::Screen::scrollRegionHorizontal(t, parameterOr(t, 0, 1), Ubiquitty::Screen::HorizontalScrollDirection::Left); }

void csiH_sr(Ubiquitty::Terminal& t, char) { Ubiquitty::Screen::scrollRegionHorizontal(t, parameterOr(t, 0, 1), Ubiquitty::Screen::HorizontalScrollDirection::Right); }
// Append raw bytes to the print sink (no-op when no sink is installed).
static void printSinkWrite(Ubiquitty::Terminal& t, const char* bytes, size_t n)
{
    if (!t.printSinkFile || n == 0)
        return;
    std::fwrite(bytes, 1, n, static_cast<FILE*>(t.printSinkFile));
}

// Append one code point as UTF-8 to the print sink.
static void printSinkWriteCodePoint(Ubiquitty::Terminal& t, uint32_t codePoint)
{
    if (codePoint == 0)
        codePoint = ' ';
    char buffer[4];
    int n = 0;
    if (codePoint < 0x80) {
        buffer[n++] = static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        buffer[n++] = static_cast<char>(0xC0 | (codePoint >> 6));
        buffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        buffer[n++] = static_cast<char>(0xE0 | (codePoint >> 12));
        buffer[n++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        buffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        buffer[n++] = static_cast<char>(0xF0 | (codePoint >> 18));
        buffer[n++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        buffer[n++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        buffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    printSinkWrite(t, buffer, static_cast<size_t>(n));
}

// Dump one display row [from..to] of the active screen to the print sink as
// UTF-8, trailing blanks trimmed, terminated by LF. Wide-continuation cells
// are skipped (the lead cell carries the glyph).
void printSinkWriteRow(Ubiquitty::Terminal& t, int row)
{
    if (!t.grid || row < 0 || row >= t.rows)
        return;
    int lastNonBlank = -1;
    for (int c = 0; c < t.columns; c++) {
        const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(c));
        if (cell && cell->codePoint != 0 && cell->codePoint != ' ' && !Ubiquitty::isTextContinuation(*cell))
            lastNonBlank = c;
    }
    for (int c = 0; c <= lastNonBlank; c++) {
        const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(c));
        if (cell && Ubiquitty::isTextContinuation(*cell))
            continue; // another cell already carries this glyph
        printSinkWriteCodePoint(t, cell ? cell->codePoint : ' ');
    }
    printSinkWrite(t, "\n", 1);
}

// Print-screen (CSI 0 i) / DECPEX-aware. DECPEX set ⇒ whole screen; reset ⇒
// the scroll region only. DECPFF set ⇒ a trailing form feed.
static void mediaCopyPrintScreen(Ubiquitty::Terminal& t)
{
    if (!t.printSinkFile)
        return;
    int from = 0, to = t.rows - 1;
    if (!t.printExtentFullScreen) {
        from = t.screens.active->scrollTop;
        to = t.screens.active->scrollBottom;
    }
    for (int row = from; row <= to; row++)
        printSinkWriteRow(t, row);
    if (t.printFormFeed)
        printSinkWrite(t, "\f", 1);
}

// MC controller sequences are consumed before character decoding; other
// printer data retains its encoding and never enters the display parser.
std::size_t consumePrinterBytes(Ubiquitty::Terminal& t, std::span<const char> bytes)
{
    static constexpr std::array<std::string_view, 4> controls { "\033[4i", "\033[5i", "\2334i", "\2335i" };
    std::size_t offset = 0;
    while (offset < bytes.size() && t.printMode == 5) {
        const auto byte = static_cast<uint8_t>(bytes[offset]);
        if (byte == 0x11 || byte == 0x13 || (!byte && t.shouldIgnorePrinterNUL)) {
            ++offset;
            continue;
        }
        if (!t.printerControlPrefixLength && byte != 0x1B && byte != 0x9B) {
            const auto start = offset++;
            while (offset < bytes.size()) {
                const auto next = static_cast<uint8_t>(bytes[offset]);
                if (next == 0x1B || next == 0x9B || next == 0x11 || next == 0x13 || (!next && t.shouldIgnorePrinterNUL))
                    break;
                ++offset;
            }
            printSinkWrite(t, bytes.data() + start, offset - start);
            continue;
        }
        assert(t.printerControlPrefixLength < t.printerControlPrefix.size());
        t.printerControlPrefix[t.printerControlPrefixLength++] = bytes[offset++];
        const std::string_view prefix(t.printerControlPrefix.data(), t.printerControlPrefixLength);
        bool isPrefix = false;
        for (const auto control : controls) {
            if (!control.starts_with(prefix))
                continue;
            isPrefix = true;
            if (control.size() == prefix.size()) {
                if (control[control.size() - 2] == '4') {
                    t.printMode = 0;
                    if (t.printSinkFile)
                        std::fflush(static_cast<FILE*>(t.printSinkFile));
                }
                t.printerControlPrefixLength = 0;
            }
            break;
        }
        if (isPrefix)
            continue;
        // A second introducer can start the terminator after an unmatched prefix.
        const bool isControlIntroducer = byte == 0x1B || byte == 0x9B;
        printSinkWrite(t, prefix.data(), prefix.size() - (isControlIntroducer ? 1 : 0));
        t.printerControlPrefixLength = isControlIntroducer ? 1 : 0;
        if (isControlIntroducer)
            t.printerControlPrefix[0] = static_cast<char>(byte);
    }
    return offset;
}

// MC (CSI Ps i) — ANSI Media Copy. 0=print screen, 4=controller off, 5=on.
void csiH_mc(Ubiquitty::Terminal& t, char)
{
    int p = parameterOr(t, 0, 0);
    switch (p) {
    case 0:
        mediaCopyPrintScreen(t);
        break;
    case 4:
        t.printMode = 0; // printer controller off
        break;
    case 5:
        t.printMode = 5;
        t.printerControlPrefixLength = 0;
        t.autoPrint = false;
        break;
    default:
        break; // 10/11 (HTML/SVG dump) unsupported; ignore
    }
}

// DEC MC (CSI ? Ps i) — DEC-specific Media Copy. 1=print cursor line,
// 4=auto-print off, 5=auto-print on, 10=print composed display, 11=print all
// pages. Auto-print copies each completed line to the sink (see linefeed).
void csiH_mc_dec(Ubiquitty::Terminal& t, char)
{
    int p = parameterOr(t, 0, 0);
    switch (p) {
    case 1:
        printSinkWriteRow(t, t.screens.active->cursorY); // print line containing cursor
        break;
    case 4:
        t.autoPrint = false;
        break;
    case 5:
        t.autoPrint = true;
        break;
    case 10: // print composed display — same dump (ignores DECPEX per spec)
    {
        if (t.printSinkFile) {
            for (int row = 0; row < t.rows; row++)
                printSinkWriteRow(t, row);
            if (t.printFormFeed)
                printSinkWrite(t, "\f", 1);
        }
        break;
    }
    case 11: // print all pages — Ubiquitty models one live page; dump it
        mediaCopyPrintScreen(t);
        break;
    default:
        break;
    }
}

// XTWINOPS title stack (CSI 22/23 t). One stack of frames, not one stack per
// label: a push records only the labels its sub-op names, and a pop consumes one
// WHOLE frame however few labels it restores — so pushing both and popping just
// the icon leaves nothing behind for a following window pop. A label the popped
// frame never recorded is taken from the nearest frame below that did, which is
// what makes push-icon, push-window, pop-both restore both. Overflow drops the
// oldest frame; a pop on an empty stack is a no-op.
/// Which titles an XTWINOPS stack control acts on.
///
/// Ps 0 names both, 1 the icon name alone, 2 the window title alone — so the two
/// are a set rather than a choice, and either, neither or both can be named.
enum class TitleTarget : uint8_t {
    Icon = 1u << 0,
    Window = 1u << 1
};
using TitleTargets = Ubiquitty::Bits::OptionSet<TitleTarget>;

/// Reads XTWINOPS's sub-parameter as the set of titles it names.
///
/// - Parameter sub: The sub-parameter: 0 both, 1 icon, 2 window.
/// - Returns: The titles the control acts on.
static constexpr TitleTargets titleTargetsFor(int sub)
{
    TitleTargets targets;
    if (sub != 2)
        targets.add(TitleTarget::Icon);
    if (sub != 1)
        targets.add(TitleTarget::Window);
    return targets;
}

// Records the named labels in a frame. Entry 0 pushes a new frame, dropping the
// oldest when all ten are in use; entry 1 to 10 overwrites that frame and leaves
// the stack's length alone (ctlseqs, XTWINOPS 22: "store the title into the stack
// ... without pushing").
static void titleStackPush(Ubiquitty::Terminal& t, TitleTargets targets, int entry)
{
    constexpr size_t capacity = Ubiquitty::Terminal::titleStackCapacity;
    size_t slot = 0;
    if (entry > 0) {
        slot = static_cast<size_t>(entry - 1) % capacity;
    } else {
        if (t.titleStackLength >= capacity) {
            for (size_t i = 1; i < capacity; i++) {
                t.titleUTF8Stack[i - 1] = t.titleUTF8Stack[i];
                t.iconNameUTF8Stack[i - 1] = t.iconNameUTF8Stack[i];
            }
            t.titleStackValid = static_cast<uint16_t>(t.titleStackValid >> 1);
            t.iconNameStackValid = static_cast<uint16_t>(t.iconNameStackValid >> 1);
            t.titleStackLength = static_cast<uint8_t>(capacity - 1);
        }
        slot = t.titleStackLength;
        t.titleStackLength++;
    }
    const uint16_t bit = static_cast<uint16_t>(1u << slot);
    t.titleUTF8Stack[slot] = targets.contains(TitleTarget::Window) ? t.titleUTF8 : FallibleString();
    t.iconNameUTF8Stack[slot] = targets.contains(TitleTarget::Icon) ? t.iconNameUTF8 : FallibleString();
    t.titleStackValid = static_cast<uint16_t>(targets.contains(TitleTarget::Window) ? (t.titleStackValid | bit) : (t.titleStackValid & ~bit));
    t.iconNameStackValid = static_cast<uint16_t>(targets.contains(TitleTarget::Icon) ? (t.iconNameStackValid | bit) : (t.iconNameStackValid & ~bit));
}

// The topmost frame at or below `slot` that recorded this label; -1 when none did.
static int titleStackSource(uint16_t valid, int slot)
{
    for (int i = slot; i >= 0; i--) {
        if (valid & (1u << i))
            return i;
    }
    return -1;
}

// Restores the named labels from a frame. Entry 0 pops the top frame; entry 1 to
// 10 reads that frame and leaves the stack as it is.
static void titleStackPop(Ubiquitty::Terminal& t, TitleTargets targets, int entry)
{
    constexpr size_t capacity = Ubiquitty::Terminal::titleStackCapacity;
    const bool isPop = entry <= 0;
    if (isPop && t.titleStackLength == 0)
        return;
    const int slot = isPop ? static_cast<int>(t.titleStackLength) - 1 : static_cast<int>(static_cast<size_t>(entry - 1) % capacity);
    if (targets.contains(TitleTarget::Icon)) {
        const int from = titleStackSource(t.iconNameStackValid, slot);
        if (from >= 0)
            Ubiquitty::setIconNameUTF8String(t, t.iconNameUTF8Stack[static_cast<size_t>(from)]);
    }
    if (targets.contains(TitleTarget::Window)) {
        const int from = titleStackSource(t.titleStackValid, slot);
        if (from >= 0) {
            Ubiquitty::setTitleUTF8String(t, t.titleUTF8Stack[static_cast<size_t>(from)]);
            t.titleChanged = true;
        }
    }
    if (!isPop)
        return;
    // The frame is spent whichever labels were asked for: a pop for the other label
    // must find the stack one frame shorter, not a second copy of this one.
    const uint16_t bit = static_cast<uint16_t>(1u << slot);
    t.titleUTF8Stack[static_cast<size_t>(slot)].clear();
    t.iconNameUTF8Stack[static_cast<size_t>(slot)].clear();
    t.titleStackValid = static_cast<uint16_t>(t.titleStackValid & ~bit);
    t.iconNameStackValid = static_cast<uint16_t>(t.iconNameStackValid & ~bit);
    t.titleStackLength--;
}

// XTTITLEPOS (CSI # S): the stack's length and its maximum index, in the form of
// the request (xterm misc.c, xtermReportTitleStack). Parameters are ignored.
void csiH_xttitlepos(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.putDecimal(t.titleStackLength);
    reply.put(';');
    reply.putDecimal(static_cast<int>(Ubiquitty::Terminal::titleStackCapacity));
    reply.put("#S");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::StatusReport, reply.span());
}

// XTPUSHSGR (CSI Pm # {, or CSI Pm # p): with no parameter every attribute is
// saved; otherwise each Ps names one (xterm charproc.c, CASE_XTERM_PUSH_SGR),
// 10 and 11 being the obsolete spellings of 30 and 31. A Ps naming nothing, 0
// included, adds nothing to the mask.
void csiH_xtpushsgr(Ubiquitty::Terminal& t, char)
{
    constexpr int maxPushParameter = 32;
    uint32_t mask = 0;
    if (t.parser.parameterCount == 0) {
        mask = ~0u;
    } else {
        for (int i = 0; i < t.parser.parameterCount; i++) {
            int item = parameterOr(t, i, 0);
            if (item == 10)
                item = 30;
            else if (item == 11)
                item = 31;
            if (item > 0 && item < maxPushParameter)
                mask |= 1u << (item - 1);
        }
    }
    Ubiquitty::SGR::push(t, mask);
}

// XTPOPSGR (CSI # }, or CSI # q) takes no parameters; any given are ignored.
void csiH_xtpopsgr(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::SGR::pop(t);
    Ubiquitty::penApplyNormalTextColors(&t.screens.active->pen, t.normalTextColors);
}

// Title mode 1 (CSI > 1 t) reports labels hex-encoded rather than literal.
// Hex is injection-proof by construction; the literal form is not, and the label
// is whatever OSC 0/1/2 last set, so that branch sanitizes before reflecting.
static std::string titleReportBody(const Ubiquitty::Terminal& t, const std::string& label)
{
    if ((t.titleModes & 0x02u) == 0)
        return Ubiquitty::sanitizedReplyLabel(label);
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(label.size() * 2);
    for (unsigned char c : label) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

// DECSLPP (CSI Ps t, Ps >= 24) — Set Lines Per Page. Shares 't' with XTWINOPS
// but is a core page-memory function, not gated by allowWindowOperations.
// Resizes to Ps rows. Display memory here is the one page on screen, so the
// page-movement functions NP, PP, PPA, PPR and PPB have no other page to move
// to and are ignored, as a terminal with only one page ignores them (VT510
// RM, NP and PP; DEC STD 070 §5.4.7).
static void decSetLinesPerPage(Ubiquitty::Terminal& t, int lines)
{
    if (lines < 1)
        return;
    if (lines > Ubiquitty::maxRows)
        lines = Ubiquitty::maxRows;
    if (lines != t.rows)
        Ubiquitty::resize(t, lines, t.columns);
}

void csiH_xtwinops(Ubiquitty::Terminal& t, char)
{
    // DECSLPP (Ps >= 24) is a core page-memory function, handled ahead of the
    // window-op gate so it works in a locked-down (allowWindowOperations off)
    // session too.
    if (parameterOr(t, 0, 0) >= 24) {
        decSetLinesPerPage(t, t.parser.parameters[0]);
        return;
    }
    // allowWindowOperations gates the WHOLE handler, not just the mutators: the
    // geometry reports (Ps 13/14/15/16) are a fingerprinting leak channel and
    // must stay silent when the flag is off. Narrowing this guard would re-expose
    // them.
    if (!t.allowWindowOperations)
        return;
    int op = parameterOr(t, 0, 0);
    int sub = parameterOr(t, 1, 0);
    // Presence-aware sub-parameters for resize/move: -1 = omitted (keep current),
    // 0 = present-zero (use the display's dimension), >0 = explicit (pixels for
    // op 4 / cells for op 8).
    int p1 = parameterOmitted(t, 1) ? -1 : t.parser.parameters[1];
    int p2 = parameterOmitted(t, 2) ? -1 : t.parser.parameters[2];

    switch (op) {
    case 3: { // move to (a=x, b=y): an omitted coordinate is 0, not "keep" —
        // unlike the resize ops, move has no keep-this-axis form.
        if (auto* c = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::WindowMutate))
            c->performWindowOperation(op, parameterOr(t, 1, 0), parameterOr(t, 2, 0));
        break;
    }
    case 1: // de-iconify
    case 2: // iconify
    case 5: // raise to front
    case 6: // lower to back
    case 7: // refresh
    case 4: // resize to (a=height, b=width) pixels; 0 = display size
    case 9: // maximize: a = 0 restore / 1 full / 2 vert / 3 horz
    case 10: // full-screen: a = 0 off / 1 on / 2 toggle
        // WindowMutate: these perturb the host window. An installed policy must
        // allow it (origin-aware); an unset policy answers PAL's default.
        if (auto* c = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::WindowMutate))
            c->performWindowOperation(op, p1, p2);
        break;
    case 8: // CSI 8 ; rows ; columns t — host-requested cell resize (0 axis =
        // fit display, omitted = keep current). A wired client owns the resize
        // (window + model); without one the model resizes directly for explicit
        // non-zero dims. WindowMutate-gated (origin-aware; an unset policy answers
        // PAL's default).
        // Gated once for the whole case rather than at the client: the no-client
        // branch below resizes the model directly, which is the same escape and
        // must not become reachable by having no frontend wired.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::WindowMutate))
            break; // host policy denied the resize — skip the side effect
        if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient)) {
            c->resize(p1, p2);
        } else if (p1 > 0 && p2 > 0) {
            int newRows = (p1 > Ubiquitty::maxRows) ? Ubiquitty::maxRows : p1;
            int newColumns = (p2 > Ubiquitty::maxColumns) ? Ubiquitty::maxColumns : p2;
            Ubiquitty::resize(t, newRows, newColumns);
        }
        break;
    case 11: // report window state: 1 = de-iconified, 2 = iconified
        respond(t, Ubiquitty::Capability::StatusReport, t.windowIconified ? "\033[2t" : "\033[1t");
        break;
    case 13: { // report window position in pixels: CSI 3 ; x ; y t. The
        // frontend publishes the top-left origin; Ps2=2 (text-area
        // position) shares it since Core has no separate inset.
        std::string resp = "\033[3;" + std::to_string(static_cast<int>(t.winPosX)) + ";" + std::to_string(static_cast<int>(t.winPosY)) + "t";
        respond(t, Ubiquitty::Capability::WindowGeometryRead, resp.c_str());
        break;
    }
    case 14: { // report text-area / window size in pixels: CSI 4 ; H ; W t
        int cw = t.cellPixelWidth ? t.cellPixelWidth : 8;
        int ch = t.cellPixelHeight ? t.cellPixelHeight : 16;
        // Prefer the TRUE frontend-published extent — the drawable under DECSET
        // 7402, the text area otherwise (reportedTextAreaPixels decides); the
        // per-cell product accumulates rounding residue, so it is only a fallback.
        const IntSize published = reportedTextAreaPixels(t);
        int tw = published.width ? published.width : t.columns * cw;
        int th = published.height ? published.height : t.rows * ch;
        std::string resp = "\033[4;" + std::to_string(th) + ";" + std::to_string(tw) + "t";
        respond(t, Ubiquitty::Capability::WindowGeometryRead, resp.c_str());
        break;
    }
    case 15: { // report screen size in pixels: CSI 5 ; H ; W t. Uses the
        // frontend-published display size; falls back to the TRUE drawn text
        // area, then the per-cell product, when the display size is unknown.
        int cw = t.cellPixelWidth ? t.cellPixelWidth : 8;
        int ch = t.cellPixelHeight ? t.cellPixelHeight : 16;
        const IntSize published = reportedTextAreaPixels(t);
        int sw = t.screenPixelWidth ? t.screenPixelWidth : (published.width ? published.width : t.columns * cw);
        int sh = t.screenPixelHeight ? t.screenPixelHeight : (published.height ? published.height : t.rows * ch);
        std::string resp = "\033[5;" + std::to_string(sh) + ";" + std::to_string(sw) + "t";
        respond(t, Ubiquitty::Capability::WindowGeometryRead, resp.c_str());
        break;
    }
    case 16: { // report cell size in pixels: CSI 6 ; H ; W t
        int cw = t.cellPixelWidth ? t.cellPixelWidth : 8;
        int ch = t.cellPixelHeight ? t.cellPixelHeight : 16;
        std::string resp = "\033[6;" + std::to_string(ch) + ";" + std::to_string(cw) + "t";
        respond(t, Ubiquitty::Capability::WindowGeometryRead, resp.c_str());
        break;
    }
    case 18: { // report text-area size in chars: CSI 8 ; H ; W t
        std::string resp = "\033[8;" + std::to_string(t.rows) + ";" + std::to_string(t.columns) + "t";
        respond(t, Ubiquitty::Capability::TerminalSizeRead, resp.c_str());
        break;
    }
    case 19: { // report screen size in chars: CSI 9 ; H ; W t
        // The DISPLAY's cell capacity, not the window's text area (that is 18).
        const int cw = t.cellPixelWidth ? t.cellPixelWidth : 8;
        const int ch = t.cellPixelHeight ? t.cellPixelHeight : 16;
        const int screenRows = t.screenPixelHeight ? t.screenPixelHeight / ch : t.rows;
        const int screenColumns = t.screenPixelWidth ? t.screenPixelWidth / cw : t.columns;
        std::string resp = "\033[9;" + std::to_string(screenRows < 1 ? 1 : screenRows) + ";" + std::to_string(screenColumns < 1 ? 1 : screenColumns) + "t";
        respond(t, Ubiquitty::Capability::TerminalSizeRead, resp.c_str());
        break;
    }
    case 20: { // report icon label (full label, no clip).
        std::string resp = "\033]L" + titleReportBody(t, t.iconNameUTF8) + "\033\\";
        respond(t, Ubiquitty::Capability::WindowTitleRead, resp.c_str());
        break;
    }
    case 21: { // report window title (full title, no fixed-buffer clip)
        std::string resp = "\033]l" + titleReportBody(t, t.titleUTF8) + "\033\\";
        respond(t, Ubiquitty::Capability::WindowTitleRead, resp.c_str());
        break;
    }
    case 22: // push title onto stack: Ps 0 both, 1 icon, 2 window; a third parameter names the entry
        if (sub == 0 || sub == 1 || sub == 2)
            titleStackPush(t, titleTargetsFor(sub), parameterOr(t, 2, 0));
        break;
    case 23: // pop title from stack: Ps 0 both, 1 icon, 2 window; a third parameter names the entry
        if (sub == 0 || sub == 1 || sub == 2)
            titleStackPop(t, titleTargetsFor(sub), parameterOr(t, 2, 0));
        break;
    default:
        // Ps >= 24 (DECSLPP) is handled before the window-op gate above; any
        // other unrecognised Ps is ignored.
        break;
    }
}

// Linux-console private CSI family `CSI Ps [; Pn] ]` (console_codes(4)). Only
// the power/display subset has an analogue here; palette/bell/console-switch/
// cursor-blink are kernel-VT concerns handled as documented no-ops.
//   CSI 9 ; n ]   set screen-blank timeout (minutes; 0 = never)
//   CSI 13 ]      unblank now (DPMS-on)
//   CSI 14 ; n ]  set VESA powerdown interval (minutes)
// No kernel idle timer exists here, so timeouts are stored (never fire); only
// the explicit unblank (13) acts.
void csiH_linuxPower(Ubiquitty::Terminal& t, char)
{
    // setterm caps both timeouts at 60 minutes; clamp the raw parameter the
    // same way (a stray huge value is harmless, just pinned).
    auto clampMinutes = [](int v) -> uint16_t {
        if (v < 0)
            v = 0;
        if (v > 60)
            v = 60;
        return static_cast<uint16_t>(v);
    };
    const int ps = parameterOr(t, 0, 0);
    switch (ps) {
    case 9: // set screen-blank timeout (minutes); 0 = never. Stored only — no
        // idle timer fires it here.
        t.screenBlankTimeout = clampMinutes(parameterOr(t, 1, 0));
        break;
    case 13: // unblank now (DPMS-on)
        // Waking the display is the one branch here that reaches past the
        // grid; 9 and 14 store a timeout this terminal never fires. The
        // capability is declared host-escaping and had no consulting site.
        if (t.screenBlanked) {
            if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::DisplayPower))
                break; // host policy denied the display-power change
            t.screenBlanked = false;
            if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient))
                c->setDisplayPower(Ubiquitty::DisplayPowerState::Active);
        }
        break;
    case 14: // VESA powerdown interval (minutes)
        t.vesaPowerdownTimeout = clampMinutes(parameterOr(t, 1, 0));
        break;
    default:
        // 1/2 (underline/dim color), 8 (set default color pair), 10/11 (bell
        // freq/duration), 12/15 (console switch), 16 (cursor blink interval):
        // kernel-VT functions with no terminal-emulator analogue — swallowed.
        break;
    }
}

} // namespace Ubiquitty::VT
