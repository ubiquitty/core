// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The run invariant and the two operations that maintain it.
//
// A run occupies its anchor's column plus a chain of continuations:
// `Attribute::Wide` at the lead and `Attribute::WideContinuation` at every
// column the glyph covers after it. CellGrid::writeWideCell asserts that shape
// at the moment of writing, and nothing asserts it again — so an editing
// control that lands on part of a run can leave the grid holding a chain the
// write path would have rejected. An anchor with a short chain renders a glyph
// over a neighbour's columns; a continuation with no anchor behind it renders
// as a hole that no cursor motion can reach.
//
// Two callers, two shapes. An ERASE knows its range before it touches anything,
// so it widens the range to whole runs. A SHIFT cannot know: what it orphans
// depends on what moved, so it scrubs afterwards and reports how far the damage
// reached, since the row span it writes back has to cover it.

#include "Core/Cell.h"
#include "Core/CellContent.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"

namespace Ubiquitty::Screen {

/// Widens the inclusive column range `[from, to]` so that neither end splits a
/// run.
///
/// A range starting mid-chain walks left to the run's anchor, and a range
/// ending where the chain continues walks right to its last continuation.
/// Erasing part of a run is not a partial erase of the glyph — it is one cell
/// wide in the model and the whole chain wide on screen, so the whole of it
/// goes, at whatever width the chain reaches.
///
/// - Parameters:
///   - grid: The grid to read; null leaves the range as given.
///   - row: The row the range lies on.
///   - columns: The row's width, bounding the rightward walk.
///   - from: The range's first column, moved left to its run's anchor.
///   - to: The range's last column, moved right to its run's end.
inline void expandRangeToWholeRuns(const Ubiquitty::CellGrid* grid, int row, int columns, int& from, int& to)
{
    if (!grid)
        return;
    auto isContinuationAt = [&](int column) {
        const Ubiquitty::Cell* cell = grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        return cell && Ubiquitty::cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
    };
    while (from > 0 && isContinuationAt(from))
        --from;
    while (to + 1 < columns && isContinuationAt(to + 1))
        ++to;
}

/// Blanks every run cell the anchor's claim no longer backs, and widens
/// `[firstTouched, lastTouched]` to cover what it blanked.
///
/// A shift moves cells past each other, so a run straddling either edge of the
/// shifted window keeps part of its chain in place while the rest leaves. The
/// damage lands outside the window, which is why the caller passes the span it
/// was about to write and gets back the span it has to write.
///
/// The chain is judged against the anchor's ``claimedRunColumns``, not against
/// itself: a chain shortened behind its anchor still reads as an unbroken
/// chain, and only the claim says cells are missing. A chain shorter than the
/// claim blanks whole — the glyph's columns are gone, so the glyph goes. A
/// chain longer than the claim keeps the claimed run and blanks the excess.
/// A continuation no anchor's walk reaches is a fragment and blanks alone.
///
/// - Parameters:
///   - row: The row's cells.
///   - columns: The row's width.
///   - blank: What a blanked cell becomes.
///   - table: The terminal's spec table, for sized anchors' claims; null
///     where none is reachable.
///   - ambiguous: The terminal's ambiguous-width choice, for sized anchors'
///     claims.
///   - firstTouched: Widened left to the first cell blanked.
///   - lastTouched: Widened right to the last cell blanked.
inline void scrubMulticellOrphans(Ubiquitty::Cell* row, int columns, const Ubiquitty::Cell& blank,
    const Ubiquitty::TextSizeTable* table, Ubiquitty::Unicode::Width::AmbiguousWidth ambiguous, int& firstTouched, int& lastTouched)
{
    auto blankAt = [&](int column) {
        row[column] = blank;
        if (column < firstTouched)
            firstTouched = column;
        if (column > lastTouched)
            lastTouched = column;
    };
    int i = 0;
    while (i < columns) {
        const Ubiquitty::VT::AttributeSet attributes = Ubiquitty::cellPen(row[i]).attributes;
        if (attributes.contains(Ubiquitty::VT::Attribute::Wide)) {
            int chainEnd = i + 1;
            while (chainEnd < columns
                && Ubiquitty::cellPen(row[chainEnd]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++chainEnd;
            const int stored = chainEnd - i;
            const int claimed = Ubiquitty::claimedRunColumns(row[i], table, ambiguous);
            const int keep = stored < claimed ? 0 : claimed;
            for (int j = i + keep; j < chainEnd; ++j)
                blankAt(j);
            i = chainEnd;
        } else if (attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            blankAt(i);
            ++i;
        } else {
            ++i;
        }
    }
}

} // namespace Ubiquitty::Screen
