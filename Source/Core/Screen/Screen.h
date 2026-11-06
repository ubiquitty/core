// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include "Core/LineAttributeView.h"
#include "Core/LineWrappedView.h"
#include "Core/RowMarkView.h"
#include <cstdint>

namespace Ubiquitty::Screen {

struct State {
    int cursorX, cursorY;
    int scrollTop, scrollBottom;
    int scrollLeft, scrollRight; // DECSLRM left/right margins (inclusive, 0-based). Default [0, columns-1].
                                 // Consulted only when Terminal::leftRightMarginMode (DECSET 69) is set.

    /// A pair of columns DECSLRM once set as margins, narrower than the screen.
    struct RememberedMargins {
        int left;
        int right;
    };
    /// The narrower-than-the-screen margin pairs DECSLRM has set, newest first.
    ///
    /// A multiplexer scrolls a pane by setting the pane's columns as the
    /// margins and clearing them again afterwards, so the margins are live for
    /// the scroll and gone by the time the pane's text is written -- and the
    /// text is written a visual row at a time, so the terminal is never told
    /// that two rows are one line. The pairs are the pane geometry, though, and
    /// a row-join that has them knows where a hard wrap at a pane edge can
    /// fall. Eight covers any layout a person reads; a ninth evicts the oldest.
    /// Cleared with the margins themselves: a reset, DECALN, a resize.
    RememberedMargins rememberedMargins[8];
    int rememberedMarginCount;
    Pen pen; // current SGR pen

    // DECSC/DECRC saved state
    int savedX, savedY;
    Pen savedPen;
    int savedCharacterSetActive;
    int savedCharacterSetGR; // GR invocation (LS1R/2R/3R) snapshot
    int savedCharacterSetG0, savedCharacterSetG1, savedCharacterSetG2, savedCharacterSetG3;
    int savedSingleShift; // a pending SS2/SS3 (2 or 3) travels with DECSC/DECRC; 0 when none
    // DECSC also snapshots DECAWM (autowrap) and DECOM (origin mode). On Screen because primary/alternate
    // each get their own DECSC slot.
    bool savedAutowrap;
    bool savedOriginMode;
    bool savedWrapPending; // the deferred-wrap latch travels with DECSC/DECRC
    // not part of the DECSC/DECRC record — DECRC restores no scrolling region, so restoreCursor() leaves
    // the current margins alone.
    // Still saved + resize-reclamped for the Sync snapshot wire format (teleport/reattach).
    int savedScrollTop;
    int savedScrollBottom;
    bool hasSavedCursor;

    bool wrapPending; // deferred wrap at right margin

    // DECDWL/DECDHL per-row width/height attribute (ESC # 3/4/5/6). Renderer scales glyphs horizontally
    // (DWL) or vertically (DHL). Read-through view: bytes live in CellGrid::m_rowMeta (one store of record).
    LineAttributeView lineAttribute;

    // Soft-wrap marker: cursor advanced off row r's right margin and continued on r+1 without CR/LF.
    // Selection word-mode extends words across it; text-copy suppresses the row separator. Cleared wherever
    // lineAttribute is reset. Read-through view over the same m_rowMeta record.
    LineWrappedView lineWrapped;

    // OSC-133 prompt mark for row r. Read-through view over the same m_rowMeta record.
    RowMarkView mark;
};

} // namespace Ubiquitty::Screen
