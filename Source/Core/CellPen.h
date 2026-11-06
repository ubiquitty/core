// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// CellPen.h — read a cell's pen through cellPen, write it through cellSetPen. Both are plain field access.
// The seam is kept because it names the intent at the call site, and because it was the interchange point
// that let a 16-byte interned Cell be evaluated against this one without touching every caller.

#include "Core/Cell.h"
#include "Core/CellContent.h"
#include "Core/Pen.h"
#include "Core/TextSizeTable.h"
#include "Core/Unicode/Width.h"

#include <algorithm>
#include <span>

namespace Ubiquitty {

inline const Pen& cellPen(const Cell& cell) { return cell.pen; }
inline void cellSetPen(Cell& cell, const Pen& pen) { cell.pen = pen; }

/// Replaces only the attribute set of `cell`'s pen.
///
/// A glyph run shares one pen across its cells and varies the attributes alone
/// — a wide pair's two halves differ from the run in nothing else. Writing the
/// whole pen per cell to change that one field stores the colour fields back
/// over themselves, once per placed glyph.
///
/// - Parameters:
///   - cell: The cell whose pen is amended.
///   - attributes: The attribute set the pen carries afterwards.
inline void cellSetPenAttributes(Cell& cell, VT::AttributeSet attributes) { cell.pen.attributes = attributes; }

/// Whether a text walk must skip `cell` because another cell already carries
/// the glyph it holds.
///
/// Two shapes repeat a glyph, and the second is easy to miss. A wide run's
/// continuation cells sit beside the anchor that holds the character. A sized
/// block's lower bands sit BELOW the band that does: every covered row carries
/// the code point so a renderer can crop that row's slice without walking to
/// the block's top, which is a rendering need and a text hazard — copying a
/// three-row block yields the glyph three times where the reader sees one.
///
/// Only for turning cells into text. A walk that measures runs, hit-tests a
/// click or decides a paint asks the attributes directly, because for those a
/// band row is a real row.
///
/// - Parameters:
///   - cell: The cell to judge.
/// - Returns: Whether the cell contributes no text of its own.
[[nodiscard]] inline bool isTextContinuation(const Cell& cell)
{
    const Pen& pen = cellPen(cell);
    if (pen.attributes.contains(VT::Attribute::WideContinuation)) {
        // Every continuation carries only its run's claim: a proportional run's
        // text rides its anchor, and a wide pair's or a sized block's
        // continuation repeats the character its anchor holds.
        return true;
    }
    return pen.extendedAttributes.textSizeSpecID() != 0 && penTextBand(&pen) != 0;
}

/// How many columns an anchor cell claims for its run.
///
/// The claim and the storage are separate facts: the chain of continuation
/// cells beside an anchor is what the grid holds, and this is what the glyph
/// needs. An editing control can shorten a chain without touching its anchor,
/// so a scrub judges the chain against the claim rather than against itself —
/// a chain that only described itself could never be wrong.
///
/// A sized anchor claims its spec's scale times its width — the width the
/// spec states, a proportional run's `c`, or the glyph's own for a spec that
/// leaves the terminal to compute it. A wide character claims ``wideRunColumns``. A cell carrying no
/// run claims itself. Any producer of a new run kind must answer here — the
/// write path asks nowhere else, and a scrub can only judge what this knows.
///
/// - Parameters:
///   - anchor: The run's anchor cell.
///   - table: The terminal's spec table, or null where none is reachable —
///     a sized anchor then claims like the wide character its attribute
///     spells, which erases rather than crops a damaged sized chain.
///   - ambiguous: The terminal's ambiguous-width choice, which measures a
///     sized anchor whose spec leaves the width to its glyph.
/// - Returns: The claimed width in columns, at least 1.
[[nodiscard]] inline int claimedRunColumns(const Cell& anchor, const TextSizeTable* table, Unicode::Width::AmbiguousWidth ambiguous)
{
    const Pen& pen = cellPen(anchor);
    const unsigned specID = pen.extendedAttributes.textSizeSpecID();
    if (specID != 0 && table) {
        if (const TextSizeSpec* spec = table->lookup(specID)) {
            // A proportional run claims the cells its code carried, each its
            // scale wide; its text has no per-glyph width the grid could measure.
            if (spec->isProportional)
                return std::max<int>(spec->scale, 1) * (spec->runWidthCells > 0 ? spec->runWidthCells : 1);
            const int width = spec->widthCells != 0
                ? spec->widthCells
                : Unicode::Width::forCodePoint(anchor.codePoint, ambiguous);
            return spec->scale * (width > 0 ? width : 1);
        }
    }
    return pen.attributes.contains(VT::Attribute::Wide) ? wideRunColumns : 1;
}

} // namespace Ubiquitty
