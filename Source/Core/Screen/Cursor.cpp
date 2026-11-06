// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Screen/Cursor.cpp — Cursor save and restore — DECSC/DECRC and the SCO variants —
// and the deferred-wrap placement a recovered screen restores.
//
// The saved slot is per screen, so an alternate-screen flip does not clobber
// the primary's saved cursor.
//
// Cold relative to the per-byte parser loop, which stays in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Screen/Operations.h"
#include "Core/Triggers/Engine.h"

#include <cstdlib>
#include <cstring>

namespace Ubiquitty::Screen {

void saveCursor(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    s->savedX = s->cursorX;
    s->savedY = s->cursorY;
    s->savedPen = s->pen;
    s->savedCharacterSetActive = t.characterSetActive;
    s->savedCharacterSetGR = t.characterSetGR;
    s->savedCharacterSetG0 = t.characterSetG[0];
    s->savedCharacterSetG1 = t.characterSetG[1];
    s->savedCharacterSetG2 = t.characterSetG[2];
    s->savedCharacterSetG3 = t.characterSetG[3];
    s->savedSingleShift = t.singleShift;
    // savedAutowrap is snapshot-only: DECRC does not put DECAWM back (see
    // restoreCursor). It stays in the record for the Sync snapshot wire.
    s->savedAutowrap = t.modes.autowrap;
    s->savedOriginMode = t.modes.originMode;
    s->savedWrapPending = s->wrapPending;
    // Not part of the DECSC/DECRC record — kept for the Sync snapshot wire
    // format only (Sync/CheckpointLedger.h).
    s->savedScrollTop = s->scrollTop;
    s->savedScrollBottom = s->scrollBottom;
    s->hasSavedCursor = true;
}

void restoreCursor(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!s->hasSavedCursor) {
        // DEC STD 070: a DECRC with nothing saved homes the cursor, resets
        // origin mode and the character attributes, and puts ASCII in G0..G3
        // with G0 in GL and GR at its default.
        s->cursorX = s->cursorY = 0;
        s->wrapPending = false;
        penResetRendition(&s->pen);
        penApplyNormalTextColors(&s->pen, t.normalTextColors);
        t.modes.originMode = false;
        t.characterSetG = {}; // US-ASCII in every slot, as at construction
        t.characterSetActive = 0;
        t.characterSetGR = 0;
        t.singleShift = 0;
        return;
    }
    s->cursorX = s->savedX;
    s->cursorY = s->savedY;
    const SemanticRole role = s->pen.semanticRole;
    s->pen = s->savedPen;
    s->pen.semanticRole = role;
    t.characterSetActive = s->savedCharacterSetActive;
    t.characterSetGR = s->savedCharacterSetGR;
    t.characterSetG[0] = s->savedCharacterSetG0;
    t.characterSetG[1] = s->savedCharacterSetG1;
    t.characterSetG[2] = s->savedCharacterSetG2;
    t.characterSetG[3] = s->savedCharacterSetG3;
    t.singleShift = s->savedSingleShift;
    t.modes.originMode = s->savedOriginMode;
    // DECRC restores origin mode, the DECSCA protection
    // attribute (carried in the pen) and the last-column flag — not autowrap,
    // reverse wrap or insert mode. Restoring DECAWM would let a DECRC silently
    // re-enable wrapping an application had just turned off. The scroll margins
    // (DECSTBM) are not part of the record either. cursorClampOrigin honors
    // DECOM: with origin mode active the saved Y stays inside the CURRENT region.
    cursorClampOrigin(t);
    s->wrapPending = s->savedWrapPending;
}

void deferWrapAtLastColumn(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorX = t.columns - 1;
    s->wrapPending = true;
}

// In origin mode, vertical cursor is constrained to the scroll region;
// horizontal is constrained to [scrollLeft, scrollRight] only when
// DECLRMM is set and DECOM is set (the origin-mode-with-margins case).
void cursorClampOrigin(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (t.modes.originMode && t.modes.leftRightMarginMode) {
        if (s->cursorX < s->scrollLeft)
            s->cursorX = s->scrollLeft;
        if (s->cursorX > s->scrollRight)
            s->cursorX = s->scrollRight;
    } else {
        if (s->cursorX < 0)
            s->cursorX = 0;
        if (s->cursorX >= t.columns)
            s->cursorX = t.columns - 1;
    }
    if (t.modes.originMode) {
        if (s->cursorY < s->scrollTop)
            s->cursorY = s->scrollTop;
        if (s->cursorY > s->scrollBottom)
            s->cursorY = s->scrollBottom;
    } else {
        if (s->cursorY < 0)
            s->cursorY = 0;
        if (s->cursorY >= t.rows)
            s->cursorY = t.rows - 1;
    }
    // A double-width or double-height line shows only its left half, and the
    // cursor stops where it can still be seen.
    const int visibleRight = t.columns / 2 - 1;
    if (s->lineAttribute[s->cursorY] != lineAttributeNormal && s->cursorX > visibleRight)
        s->cursorX = visibleRight;
}

void rememberMargins(State& s, int left, int right)
{
    constexpr int capacity = static_cast<int>(sizeof(s.rememberedMargins) / sizeof(s.rememberedMargins[0]));
    // A pair already known moves to the front rather than being kept twice --
    // the multiplexer sets the same pane's margins on every scroll it does.
    int at = s.rememberedMarginCount;
    for (int index = 0; index < s.rememberedMarginCount; ++index) {
        if (s.rememberedMargins[index].left == left && s.rememberedMargins[index].right == right) {
            at = index;
            break;
        }
    }
    const bool isNew = at == s.rememberedMarginCount;
    if (at == capacity)
        at = capacity - 1; // full and new: the oldest goes
    for (int index = at; index > 0; --index)
        s.rememberedMargins[index] = s.rememberedMargins[index - 1];
    s.rememberedMargins[0] = { left, right };
    if (isNew && s.rememberedMarginCount < capacity)
        ++s.rememberedMarginCount;
}

void forgetMargins(State& s) { s.rememberedMarginCount = 0; }

} // namespace Ubiquitty::Screen
