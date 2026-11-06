// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// DEC selective erasure uses DECSCA protection. ECMA erasure uses guarded
// areas set by SPA/EPA or DAQ. Each cell retains both properties independently.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"

namespace Ubiquitty::VT {

static void noteProtection(Ubiquitty::Terminal& t, VT::ProtectedMode mode)
{
    t.protectedMode = static_cast<VT::ProtectedMode>(std::to_underlying(t.protectedMode) | std::to_underlying(mode));
}

void escapeH_spa(Ubiquitty::Terminal& t, char)
{
    noteProtection(t, VT::ProtectedMode::ISO);
    penSetGuarded(&t.screens.active->pen, true);
}

void escapeH_epa(Ubiquitty::Terminal& t, char)
{
    noteProtection(t, VT::ProtectedMode::ISO);
    penSetGuarded(&t.screens.active->pen, false);
}

// DAQ begins a qualified area at the active position (ECMA-48 8.3.25). Selector
// 1 (protected and guarded) and 8 (protected and unguarded) protect its
// contents against erasure; guarding adds only exclusion from transfer, which
// Core has no device for. Selectors 0 and 2...7 and 9...11 begin an unprotected
// area, and the last of 0, 1 and 8 decides protection when several are given.
// A value past 11 is undefined, and a DAQ that names only such values changes
// nothing.
void csiH_daq(Ubiquitty::Terminal& t, char)
{
    bool isDefined = false;
    Ubiquitty::Screen::AreaProtection protection = Ubiquitty::Screen::AreaProtection::Unprotected;
    for (int index = 0; index < std::max(1, t.parser.parameterCount); ++index) {
        const int selector = parameterOr(t, index, 0);
        if (selector < 0 || selector > 11)
            continue;
        isDefined = true;
        if (selector == 1 || selector == 8)
            protection = Ubiquitty::Screen::AreaProtection::Protected;
        else if (selector == 0)
            protection = Ubiquitty::Screen::AreaProtection::Unprotected;
    }
    if (!isDefined)
        return;
    noteProtection(t, VT::ProtectedMode::ISO);
    penSetGuarded(&t.screens.active->pen, protection == Ubiquitty::Screen::AreaProtection::Protected);
    Ubiquitty::Screen::defineQualifiedArea(t, protection);
}

// Cell-protection (DECSCA, DECSED/DECSEL selective erase) is a VT200+
// feature; a VT100-level host has no concept of it. Inert below VT200.
static inline bool levelAllowsVT200(const Ubiquitty::Terminal& t) { return t.conformanceLevel >= VT::ConformanceLevel::VT200; }

void csiH_decsca(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT200(t))
        return;
    int p = parameterOr(t, 0, 0);
    // VT510 DECSCA defines 0, 1 and 2; another value changes nothing.
    if (p > 2)
        return;
    noteProtection(t, VT::ProtectedMode::DEC);
    Pen* pen = &t.screens.active->pen;
    if (p == 1)
        pen->attributes.add(VT::Attribute::Protected);
    else
        pen->attributes.remove(VT::Attribute::Protected);
}

// DECSEL — Selective Erase in Line. Honours Attribute::Protected.
static void selectiveEraseInLine(Ubiquitty::Terminal& t, int mode)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int from, to;
    if (mode == 0) {
        from = s->cursorX;
        to = t.columns - 1;
    } else if (mode == 1) {
        from = 0;
        to = s->cursorX;
    } else if (mode == 2) {
        from = 0;
        to = t.columns - 1;
    } else
        return;
    // Like EL and ED, a selective erase resets the pending wrap.
    s->wrapPending = false;
    if (!t.grid)
        return;
    if (Ubiquitty::Screen::hasActivePins(t)) [[unlikely]]
        Ubiquitty::Screen::notePositionsRewritten(t, s->cursorY, s->cursorY);
    Ubiquitty::Screen::expandRangeToWholeRuns(t.grid.get(), s->cursorY, t.columns, from, to);
    for (int column = from; column <= to; column++) {
        const Ubiquitty::Cell* codePoint = t.grid->cellAt(Ubiquitty::Row(s->cursorY), Ubiquitty::Column(column));
        if (codePoint && Ubiquitty::cellPen(*codePoint).attributes.contains(VT::Attribute::Protected))
            continue;
        if (codePoint)
            t.grid->setCell(Ubiquitty::Row(s->cursorY), Ubiquitty::Column(column), Ubiquitty::Screen::selectivelyErasedCell(*codePoint));
    }
    Ubiquitty::Screen::scrubBlockDamage(t, s->cursorY, s->cursorY);
}

// DECSED — Selective Erase in Display. Same protection rules as DECSEL but
// extended to the rectangle around the cursor.
static void selectiveEraseInDisplay(Ubiquitty::Terminal& t, int mode)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int rfrom, rto;
    if (mode == 0) {
        rfrom = s->cursorY;
        rto = t.rows - 1;
    } else if (mode == 1) {
        rfrom = 0;
        rto = s->cursorY;
    } else if (mode == 2) {
        rfrom = 0;
        rto = t.rows - 1;
    } else
        return;
    // Like EL and ED, a selective erase resets the pending wrap.
    s->wrapPending = false;
    if (!t.grid)
        return;
    if (Ubiquitty::Screen::hasActivePins(t)) [[unlikely]]
        Ubiquitty::Screen::notePositionsRewritten(t, rfrom, rto);
    for (int row = rfrom; row <= rto; row++) {
        int from = 0, to = t.columns - 1;
        if (mode == 0 && row == s->cursorY)
            from = s->cursorX;
        if (mode == 1 && row == s->cursorY)
            to = s->cursorX;
        Ubiquitty::Screen::expandRangeToWholeRuns(t.grid.get(), row, t.columns, from, to);
        for (int column = from; column <= to; column++) {
            const Ubiquitty::Cell* codePoint = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            if (codePoint && Ubiquitty::cellPen(*codePoint).attributes.contains(VT::Attribute::Protected))
                continue;
            if (codePoint)
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Screen::selectivelyErasedCell(*codePoint));
        }
    }
    Ubiquitty::Screen::scrubBlockDamage(t, rfrom, rto);
}

// DECSEL and DECSED apply each selective parameter in turn: DEC STD 070's
// selective_erase_in_line and selective_erase_in_display loop over every
// parameter, as ED and EL do. xterm applies only the first.
void csiH_decsel(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount <= 1) {
        selectiveEraseInLine(t, parameterOr(t, 0, 0));
        return;
    }
    for (int i = 0; i < t.parser.parameterCount; i++)
        selectiveEraseInLine(t, parameterOr(t, i, 0));
}

void csiH_decsed(Ubiquitty::Terminal& t, char)
{
    if (t.parser.parameterCount <= 1) {
        selectiveEraseInDisplay(t, parameterOr(t, 0, 0));
        return;
    }
    for (int i = 0; i < t.parser.parameterCount; i++)
        selectiveEraseInDisplay(t, parameterOr(t, i, 0));
}

void protectionReset(Ubiquitty::Terminal& t)
{
    t.screens.active->pen.attributes.remove(VT::Attribute::Protected);
    penSetGuarded(&t.screens.active->pen, false);
}

} // namespace Ubiquitty::VT
