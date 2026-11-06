// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Blanking a cell, and the dirty-marking that follows a bulk change.
//
// Both are inline rather than extern: with callers in four modules, an extern
// call would be a real call for a function body smaller than the call sequence.
// The erase paths (ECH/EL/ED, the rect family, scroll fills)
// run this per cell, and Screen is their main consumer, so the parser is not
// the right owner.
//
// clearCluster lives here because eraseCell needs it; a
// blank cell carries no cluster continuation, so the two are one operation.

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Screen/Screen.h"
#include "Core/Terminal.h"

namespace Ubiquitty {

// Stamp `cell` with the screen's current SGR pen. A cell operation, not a parser
// one; it lived in VT/Internal.h, which is why the scroll-blank helper below used
// to open-code the assignment rather than depend on the sequence layer.
[[gnu::always_inline]] inline void cellSetPenFromScreen(Ubiquitty::Cell& cell, const Ubiquitty::Screen::State* s) { cell.pen = s->pen; }

namespace Screen {

inline void clearCluster(Ubiquitty::Cell* c)
{
    for (int i = 0; i < Ubiquitty::cellMaxCombining; ++i)
        c->combining[i] = 0;
    c->clusterPacked = 0; // combiningLength (bit 0) + clusterID (bits 1-15)
    c->linkIDLo = c->linkIDHi = 0;
}

/// Removes a cell's character while preserving its rendition for DEC selective erase.
/// - Parameter original: The cell whose character is erased.
/// - Returns: An empty cell with the same graphic attributes and colors.
inline Ubiquitty::Cell selectivelyErasedCell(const Ubiquitty::Cell& original)
{
    Ubiquitty::Cell blank {};
    blank.pen = original.pen;
    blank.pen.attributes.remove({ VT::Attribute::Wide, VT::Attribute::WideContinuation, VT::Attribute::Emoji });
    blank.pen.extendedAttributes.setTextSizeSpecID(0);
    penSetTextBand(&blank.pen, 0);
    penSetRichTextCovered(&blank.pen, false);
    blank.pen.semanticRole = SemanticRole::Output;
    return blank;
}

// Blank `c` with the screen's pen MINUS character attributes: an erase keeps the
// colours (BCE) but drops bold/underline/etc, so text written into the hole later
// does not inherit the erased run's styling.
inline void eraseCell(Ubiquitty::Cell* c, const Ubiquitty::Screen::State* s)
{
    c->codePoint = 0;
    Pen erased = s->pen;
    erased.attributes = {};
    penSetGuarded(&erased, false);
    erased.semanticRole = SemanticRole::Output;
    Ubiquitty::cellSetPen(*c, erased);
    clearCluster(c);
}

/// Blanks `c` the way an erase leaves a cell under the terminal's erase colour mode.
///
/// DECECM (DEC ?117) set: the screen's default colours, whatever the pen says.
/// Reset: ``eraseCell(Cell*, const State*)``, the pen's colours without its
/// attributes.
///
/// - Parameters:
///   - c: The cell to blank.
///   - t: The terminal whose mode and active screen decide the blank.
inline void eraseCell(Ubiquitty::Cell* c, const Ubiquitty::Terminal& t)
{
    if (t.modes.shouldEraseToDefaultColors) {
        *c = defaultBlankCell;
        return;
    }
    eraseCell(c, t.screens.active);
}

// Everything on screen is suspect — used after a change too broad to track cell
// by cell (reflow, theme change, alternate-screen flip). Guarded because a
// Terminal can outlive its CellGrid during teardown.
/// Marks every row dirty, so the next frame repaints the whole grid.
///
/// - Parameter t: The terminal to invalidate.
inline void markAllDirty(Ubiquitty::Terminal& t)
{
    if (t.grid) {
        t.grid->requestFullRedraw();
        t.grid->invalidateShadow();
    }
}

// The blank a SCROLL leaves behind, which keeps the full pen — unlike eraseCell,
// which drops character attributes. A scrolled-in row is new space in the current
// style; an erased span is a hole in existing text.
inline Ubiquitty::Cell scrollBlankCell(const Ubiquitty::Screen::State* s)
{
    Ubiquitty::Cell blank {}; // value-init: Cell embeds non-trivial OptionSet fields
    cellSetPenFromScreen(blank, s);
    blank.pen.semanticRole = SemanticRole::Output;
    return blank;
}

// True when the screen's pen paints the same blank as a default-constructed cell,
// so a bulk fill can memset rather than stamp each cell.
inline bool eraseBlankIsDefault(const Ubiquitty::Screen::State* s)
{
    return s->pen.foregroundIndex == colorDefault && s->pen.backgroundIndex == colorDefault;
}

} // namespace Screen
} // namespace Ubiquitty
