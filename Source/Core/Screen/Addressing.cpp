// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Addressing.cpp — resolving an address to cells.
//
// A row identifier arrives in one of two spaces and this file is what turns
// either into memory: VISIBLE (viewOffset, row) for what the user is looking at,
// ABSOLUTE for a fixed index across history + live screen. Both resolve against
// the same two-tier store — scrollback for aged rows, CellGrid for live ones —
// and every function here answers a question rather than changing anything, the
// one exception being addRowMark.

// The scrollback/visible-row queries the frontend reads at paint (visibleCell,
// getDisplayRow, rowMark, findPromptMark*) plus the width-change reflow +
// resize machinery. Off the per-byte parse loop; the reflow helpers are
// file-local statics that only run on resize.

#include "Core/Screen/Addressing.h"
#include "Core/CellContent.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/GridBounds.h"
#include "Core/LineStore.h"
#include "Core/RowMeta.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/Marks.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Unicode/Joining.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "Core/Unicode/Width.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace Ubiquitty::Screen {

std::optional<CharacterField> activeCharacterField(const Terminal& terminal)
{
    if (terminal.statusLineActive)
        return std::nullopt;
    const auto& screen = *terminal.screens.active;
    assert(screen.cursorY >= 0 && screen.cursorY < terminal.rows);
    const int columns = terminal.columns / textScaleForLineAttribute(screen.lineAttribute[screen.cursorY]).widthScale;
    if (screen.cursorX < 0 || screen.cursorX >= columns)
        return std::nullopt;
    int first = screen.cursorX;
    while (first >= 0 && !terminal.tabStops[static_cast<std::size_t>(first)])
        --first;
    if (first < 0)
        return std::nullopt;
    int next = screen.cursorX + 1;
    while (next < columns && !terminal.tabStops[static_cast<std::size_t>(next)])
        ++next;
    if (next == columns)
        return std::nullopt;
    return CharacterField { first, next };
}

int resolvedViewOffset(const Ubiquitty::Terminal* t, int viewOffset)
{
    if (!t || t->screens.isUsingAlternateScreen)
        return 0;
    const int history = scrollbackCount(t);
    if (viewOffset < 0)
        return 0;
    return viewOffset > history ? history : viewOffset;
}

const Cell* visibleCell(const Ubiquitty::Terminal* t, int viewOffset, int row, int column)
{
    int history, absolute;
    if (!t || row < 0 || row >= t->rows || column < 0 || column >= t->columns)
        return &defaultBlankCell;
    history = scrollbackCount(t);
    viewOffset = resolvedViewOffset(t, viewOffset);
    absolute = history - viewOffset + row;
    if (absolute < history) {
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absolute);
        if (!line || column >= line->length)
            return &defaultBlankCell;
        return &line->cells[column];
    }
    if (t->grid) {
        const Cell* codePoint = t->grid->cellAt(Ubiquitty::Row(absolute - history), Ubiquitty::Column(column));
        return codePoint ? codePoint : &defaultBlankCell;
    }
    return &defaultBlankCell;
}

// Resolve a visible row (viewOffset/row space) to its CONTIGUOUS source cells,
// valid length, and O(1) needs-shaping flag — once, so a full-row consumer
// copies the row in one memcpy instead of a per-cell visibleCell call.
VisibleRow resolveVisibleRow(const Ubiquitty::Terminal* t, int viewOffset, int row)
{
    VisibleRow r { nullptr, 0, false };
    if (!t || row < 0 || row >= t->rows)
        return r;
    const int history = scrollbackCount(t);
    viewOffset = resolvedViewOffset(t, viewOffset);
    const int absolute = history - viewOffset + row;
    if (absolute < history) {
        if (const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absolute)) {
            r.base = line->cells;
            r.length = line->length;
            r.needsShaping = line->needsShaping;
        }
    } else if (t->grid) {
        // Live rows are full-width + contiguous, so cellAt(row, 0) is the row
        // base; visibleCell blanks column >= t->columns, mirrored by length = columns.
        r.base = t->grid->cellAt(Ubiquitty::Row(absolute - history), Ubiquitty::Column(0));
        r.length = r.base ? t->columns : 0;
        r.needsShaping = t->grid->rowMeta(absolute - history).has(Ubiquitty::RowFlag::NeedsShaping);
    }
    return r;
}

bool rowMayNeedShaping(const Ubiquitty::Terminal& t, int viewOffset, int row) { return resolveVisibleRow(&t, viewOffset, row).needsShaping; }

bool visibleRowWrapped(const Ubiquitty::Terminal* t, int viewOffset, int row)
{
    if (!t || row < 0 || row >= t->rows)
        return false;
    int history = scrollbackCount(t);
    viewOffset = resolvedViewOffset(t, viewOffset);
    int absolute = history - viewOffset + row;
    if (absolute < history) {
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absolute);
        return line && line->wrapped;
    }
    return t->screens.active ? static_cast<bool>(t->screens.active->lineWrapped[absolute - history]) : false;
}

// Counts shape passes (single-threaded, like the rest of CellGrid), so the
// shaped-row-cache tests can assert a cached fetch skipped its reshape.
static uint64_t s_shapedRowInvocations = 0;
uint64_t shapedRowInvocationCount() { return s_shapedRowInvocations; }

void getDisplayRow(const Ubiquitty::Terminal& t, int viewOffset, int row, Cell* out, int columns)
{
    if (!out || columns <= 0)
        return;

    // Resolve once, copy in one memcpy; the logical store is never mutated. Cells
    // past the valid length get the canonical blank, not a zeroed Cell (whose
    // backgroundIndex 0 paints ANSI-black).
    const VisibleRow src = resolveVisibleRow(&t, viewOffset, row);
    const int valid = src.length < columns ? src.length : columns;
    if (valid > 0)
        std::memcpy(out, src.base, static_cast<std::size_t>(valid) * sizeof(Cell));
    for (int c = valid; c < columns; ++c)
        out[c] = defaultBlankCell;
    if (!src.needsShaping)
        return;

    // Shaped-row cache, keyed on the live row's index + content generation. LIVE
    // rows only — a scrollback row's absolute index rebinds on ring eviction and
    // would serve stale glyphs; live rows never evict and their generation tracks
    // every mutation.
    int history = scrollbackCount(&t);
    int shapeViewOffset = resolvedViewOffset(&t, viewOffset);
    if (shapeViewOffset < 0)
        shapeViewOffset = 0;
    if (shapeViewOffset > history)
        shapeViewOffset = history;
    const int absoluteRow = history - shapeViewOffset + row;
    const bool isCacheable = absoluteRow >= history && t.grid;
    const uint32_t rowGen = isCacheable ? t.grid->rowGeneration(absoluteRow - history) : 0;
    if (isCacheable && t.grid->lookupShapedRow(absoluteRow, rowGen, out, columns))
        return;
    ++s_shapedRowInvocations;

    // Shape + visually order at most maxColumns cells: build the base-codepoint
    // row, get shaped codepoints + the logical→visual permutation, then re-emit
    // cells in visual order carrying each source cell's pen/combining/link from
    // its LOGICAL slot — only the base `ch` is rewritten to the shaped form.
    std::uint32_t logical[maxColumns];
    std::uint32_t shapedCodePoints[maxColumns];
    std::uint16_t perm[maxColumns];
    const int n = columns <= maxColumns ? columns : maxColumns;
    for (int c = 0; c < n; ++c)
        logical[c] = out[c].codePoint;
    if (!Ubiquitty::Unicode::shapeVisualRow(logical, shapedCodePoints, perm, n)) {
        if (isCacheable) // cache the (unchanged) row so the next fetch also skips
            t.grid->storeShapedRow(absoluteRow, rowGen, out, columns);
        return;
    }

    // Visual cell c is the logical cell perm[c] with its base rewritten to
    // shapedCodePoints[c]. Copy into a scratch first since perm is a permutation of
    // `out` and an in-place write would clobber not-yet-read sources.
    CellScratchBuffer<maxColumns> scratchStorage;
    Cell* scratch = scratchStorage.data();
    for (int c = 0; c < n; ++c) {
        scratchStorage.constructAt(static_cast<std::size_t>(c), out[perm[c]]);
        scratch[c].codePoint = shapedCodePoints[c];
    }
    // A lam-alef ligature (FEF5..FEFC at the alef cell, 0 at the lam cell) is one
    // glyph spanning the pair. Mark it a wide unit (ligature cell = lead, the
    // collapsed 0 cell = continuation) so the renderer draws one crossed glyph
    // across both cells instead of a gap or two disconnected forms.
    for (int c = 0; c + 1 < n; ++c) {
        const uint32_t cp = scratch[c].codePoint;
        if (cp >= 0xFEF5 && cp <= 0xFEFC && scratch[c + 1].codePoint == wideContinuationSentinelCodePoint) {
            Pen leadPen = Ubiquitty::cellPen(scratch[c]);
            leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
            Ubiquitty::cellSetPen(scratch[c], leadPen);
            Pen contPen = Ubiquitty::cellPen(scratch[c + 1]);
            contPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            Ubiquitty::cellSetPen(scratch[c + 1], contPen);
        }
    }
    for (int c = 0; c < n; ++c)
        out[c] = scratch[c];
    if (isCacheable)
        t.grid->storeShapedRow(absoluteRow, rowGen, out, columns);
}

const Cell* absoluteRowCells(const Ubiquitty::Terminal* t, int absoluteRow, int* lengthOut)
{
    int length = 0;
    const Cell* base = nullptr;
    if (t && absoluteRow >= 0) {
        const int history = scrollbackCount(t);
        if (absoluteRow < history) {
            const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow);
            if (line) {
                base = line->cells;
                length = line->length;
            }
        } else if (absoluteRow < history + t->rows && t->grid) {
            base = t->grid->cellAt(Ubiquitty::Row(absoluteRow - history), Ubiquitty::Column(0));
            length = base ? t->columns : 0;
        }
    }
    if (lengthOut)
        *lengthOut = length;
    return base;
}

AbsoluteRow resolveAbsoluteRow(const Ubiquitty::Terminal* t, int absoluteRow)
{
    AbsoluteRow resolved;
    if (!t || absoluteRow < 0)
        return resolved;
    const int history = scrollbackCount(t);
    if (absoluteRow < history) {
        if (const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow)) {
            resolved.base = line->cells;
            resolved.length = line->length;
            resolved.needsShaping = line->needsShaping;
            resolved.lineAttribute = line->lineAttribute;
        }
    } else if (absoluteRow < history + t->rows && t->grid) {
        resolved.base = t->grid->cellAt(Ubiquitty::Row(absoluteRow - history), Ubiquitty::Column(0));
        resolved.length = resolved.base ? t->columns : 0;
        resolved.needsShaping = t->grid->rowMeta(absoluteRow - history).has(Ubiquitty::RowFlag::NeedsShaping);
        if (t->screens.active)
            resolved.lineAttribute = t->screens.active->lineAttribute[absoluteRow - history];
    }
    return resolved;
}

int absoluteRowExtent(const Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t)
        return 0;
    if (t->screens.isUsingAlternateScreen)
        return t->columns;
    int length = 0;
    return absoluteRowCells(t, absoluteRow, &length) ? length : t->columns;
}

const Cell* absoluteCell(const Ubiquitty::Terminal* t, int absoluteRow, int column)
{
    if (!t || column < 0 || column >= t->columns)
        return &defaultBlankCell;
    int length = 0;
    const Cell* base = absoluteRowCells(t, absoluteRow, &length);
    return (base && column < length) ? &base[column] : &defaultBlankCell;
}

uint8_t rowMark(const Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t)
        return 0;
    const int history = scrollbackCount(t);
    if (absoluteRow < 0 || absoluteRow >= history + t->rows)
        return 0;
    if (absoluteRow < history) {
        // The captured row carries its own mark; the live screen index is
        // a parallel space, so an absolute history row does not address it.
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow);
        return line ? line->mark : 0;
    }
    return t->screens.active->mark[absoluteRow - history];
}

std::string absoluteRowText(const Ubiquitty::Terminal& t, int absoluteRow, int columns)
{
    std::string line;
    for (int column = 0; column < columns; ++column) {
        const Cell* cell = absoluteCell(&t, absoluteRow, column);
        if (!cell)
            continue;
        if (Ubiquitty::isTextContinuation(*cell))
            continue; // another cell already carries this glyph
        if (cell->codePoint == wideContinuationSentinelCodePoint) {
            line.push_back(' ');
            continue;
        }
        Ubiquitty::Unicode::appendUTF8(line, cell->codePoint);
        for (int i = 0; i < cell->combiningLength() && i < cellMaxCombining; ++i)
            Ubiquitty::Unicode::appendUTF8(line, cell->combining[i]);
    }
    const std::size_t lastGlyph = line.find_last_not_of(' ');
    if (lastGlyph == std::string::npos)
        line.clear();
    else
        line.resize(lastGlyph + 1);
    return line;
}

// Resolve the executed command whose prompt sits at or above `absoluteRow` into
// its prompt row, output-start row, and command-line span (OSC 133 rule). Prompt
// scan: most recent CMD_EXECUTED row at or before absoluteRow (inclusive). Output
// scan: first OUTPUT_START at or after the prompt row (none → the prompt row is
// the start, i.e. no ; C emitted).
CommandRegionRows resolveCommandRegion(const Ubiquitty::Terminal* t, int totalRows, int absoluteRow)
{
    CommandRegionRows rows;
    if (!t || totalRows <= 0)
        return rows;

    int hi = absoluteRow;
    if (hi >= totalRows)
        hi = totalRows - 1;
    int promptRow = -1;
    for (int r = hi; r >= 0; --r) {
        if (rowMark(t, r) & Screen::osc133MarkCmdExecuted) {
            promptRow = r;
            break;
        }
    }
    if (promptRow < 0)
        return rows;

    int outputStart = promptRow;
    for (int r = promptRow; r < totalRows; ++r) {
        const uint8_t mark = rowMark(t, r);
        // The next block's prompt bounds this one: an OutputStart past it
        // belongs to that block, so none exists for this one and the scan is
        // done. Without the bound every block walks to the end of the buffer,
        // and building the whole palette is blocks times buffer.
        if (r > promptRow && (mark & Screen::osc133MarkCmdExecuted)) {
            break;
        }
        if (mark & Screen::osc133MarkOutputStart) {
            outputStart = r;
            break;
        }
    }

    rows.found = true;
    rows.promptRow = promptRow;
    rows.outputStart = outputStart;
    rows.commandLastRow = (outputStart > promptRow) ? (outputStart - 1) : promptRow;
    return rows;
}

bool absoluteRowWrapped(const Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t)
        return false;
    const int history = scrollbackCount(t);
    if (absoluteRow < 0 || absoluteRow >= history + t->rows)
        return false;
    if (absoluteRow < history) {
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow);
        return line && line->wrapped;
    }
    return t->screens.active ? static_cast<bool>(t->screens.active->lineWrapped[absoluteRow - history]) : false;
}

void addRowMark(Ubiquitty::Terminal* t, int absoluteRow, uint8_t bit)
{
    if (!t)
        return;
    const int history = scrollbackCount(t);
    if (absoluteRow < 0 || absoluteRow >= history + t->rows)
        return;
    if (absoluteRow < history) {
        // History row: or the bit into the captured Ubiquitty::Screen::ScrollLine so it rides
        // the line exactly like a mark set while it was live.
        if (t->lineStore) {
            if (Ubiquitty::Screen::ScrollLine* line = t->lineStore->at(absoluteRow))
                line->mark = static_cast<uint8_t>(line->mark | bit);
        }
        return;
    }
    const int rel = absoluteRow - history;
    t->screens.active->mark[rel] = static_cast<uint8_t>(t->screens.active->mark[rel] | bit);
}

uint8_t rowLineAttribute(const Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t || !t->screens.active)
        return lineAttributeNormal;
    const int history = scrollbackCount(t);
    if (absoluteRow < 0 || absoluteRow >= history + t->rows)
        return lineAttributeNormal;
    if (absoluteRow < history) {
        // The captured row carries its own attribute; the live Screen index is
        // a parallel space, so an absolute history row does not address it.
        const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow);
        return line ? line->lineAttribute : static_cast<uint8_t>(lineAttributeNormal);
    }
    return t->screens.active->lineAttribute[absoluteRow - history];
}

uint8_t rowFlags(const Ubiquitty::Terminal* t, int absoluteRow)
{
    if (!t || !t->screens.active || !t->grid)
        return 0;
    const int history = scrollbackCount(t);
    if (absoluteRow < 0 || absoluteRow >= history + t->rows)
        return 0;
    if (absoluteRow >= history)
        return t->grid->rowMeta(absoluteRow - history).flags.toRaw();
    const Ubiquitty::Screen::ScrollLine* line = scrollbackLine(t, absoluteRow);
    if (!line || !line->cells)
        return 0;
    for (int column = 0; column < line->length; ++column) {
        const unsigned specID = Ubiquitty::cellPen(line->cells[column]).extendedAttributes.textSizeSpecID();
        if (specID == 0)
            continue;
        const Ubiquitty::TextSizeSpec* spec = t->textSizeTable.lookup(specID);
        if (spec && spec->isProportional)
            return Ubiquitty::Bits::OptionSet<Ubiquitty::RowFlag> { Ubiquitty::RowFlag::Flowed }.toRaw();
    }
    return 0;
}

int findPromptMarkBefore(const Ubiquitty::Terminal* t, int absoluteRowExclusive, uint8_t markBits)
{
    if (!t)
        return -1;
    const int history = scrollbackCount(t);
    int hi = absoluteRowExclusive - 1;
    if (hi >= history + t->rows)
        hi = history + t->rows - 1;
    for (int r = hi; r >= 0; --r) {
        if (rowMark(t, r) & markBits)
            return r;
    }
    return -1;
}

int findPromptMarkAfter(const Ubiquitty::Terminal* t, int absoluteRowInclusive, uint8_t markBits)
{
    if (!t)
        return -1;
    const int total = scrollbackCount(t) + t->rows;
    int lo = absoluteRowInclusive;
    if (lo < 0)
        lo = 0;
    for (int r = lo; r < total; ++r) {
        if (rowMark(t, r) & markBits)
            return r;
    }
    return -1;
}

} // namespace Ubiquitty::Screen
