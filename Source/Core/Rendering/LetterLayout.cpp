// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/LetterLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Ubiquitty::Rendering {

namespace {

/// Lays one shaped run's cells out from `penInPixels`, writing their extents.
///
/// - Returns: The pen after the run, or a negative value when the run must
///   keep the grid's pitch because the shaper refused it.
double placeShapedRun(std::span<const LetterCell> cells, std::size_t first, std::size_t end,
    std::span<const std::uint32_t> codePointPool, double penInPixels, double widthScale,
    LetterShaper shaper, void* shaperContext, LetterLayoutScratch& scratch, std::span<LetterExtent> out)
{
    // The run's text, cell by cell, and which cell each code point came from.
    static thread_local std::vector<std::uint32_t> text;
    text.clear();
    scratch.cellOfCodePoint.clear();
    for (std::size_t cell = first; cell < end; ++cell) {
        const LetterCell& letter = cells[cell];
        const std::size_t start = letter.clusterStart;
        const std::size_t count = letter.clusterCount;
        if (count == 0 || start > codePointPool.size() || count > codePointPool.size() - start)
            return -1.0;
        for (std::size_t k = 0; k < count; ++k) {
            text.push_back(codePointPool[start + k]);
            scratch.cellOfCodePoint.push_back(static_cast<std::uint16_t>(cell - first));
        }
    }
    if (text.empty() || text.size() > std::numeric_limits<std::uint16_t>::max())
        return -1.0;
    // A face with contextual forms can answer more glyphs than code points; a
    // ligature fewer. Twice the text and one is what the OSC 66 route offers.
    scratch.shaped.assign(2 * text.size() + 1, Ubiquitty::ProportionalShapedGlyph {});
    const int shapedCount = shaper(shaperContext, std::span<const std::uint32_t>(text), cells[first].face, cells[first].weight,
        scratch.shaped.data(), static_cast<int>(scratch.shaped.size()));
    if (shapedCount <= 0 || shapedCount > static_cast<int>(scratch.shaped.size()))
        return -1.0;

    // Each cell begins where the first glyph of one of its code points begins:
    // the pen after the glyphs shaped before that glyph. A glyph whose cluster
    // names no code point of the text is the shaper's to explain and is placed
    // on the run's first cell.
    const std::size_t cellCount = end - first;
    constexpr float unset = std::numeric_limits<float>::infinity();
    scratch.runLefts.assign(cellCount, unset);
    double runPen = 0.0;
    for (int g = 0; g < shapedCount; ++g) {
        const Ubiquitty::ProportionalShapedGlyph& glyph = scratch.shaped[static_cast<std::size_t>(g)];
        const std::size_t codePoint = std::min<std::size_t>(glyph.cluster, text.size() - 1);
        const std::size_t cell = scratch.cellOfCodePoint[codePoint];
        scratch.runLefts[cell] = std::min(scratch.runLefts[cell], static_cast<float>(runPen));
        runPen += static_cast<double>(glyph.advanceInPixels);
    }
    if (!(runPen > 0.0) || !std::isfinite(runPen))
        return -1.0;
    // A cell no glyph begins in stands where the next cell begins, with no
    // advance; the last such cell stands at the run's end. Then a cell can
    // never stand left of the one before it, whatever order the shaper
    // answered in.
    float next = static_cast<float>(runPen);
    for (std::size_t cell = cellCount; cell-- > 0;) {
        if (scratch.runLefts[cell] == unset)
            scratch.runLefts[cell] = next;
        next = scratch.runLefts[cell];
    }
    for (std::size_t cell = 1; cell < cellCount; ++cell)
        scratch.runLefts[cell] = std::max(scratch.runLefts[cell], scratch.runLefts[cell - 1]);

    for (std::size_t cell = 0; cell < cellCount; ++cell) {
        const float left = scratch.runLefts[cell];
        const float right = cell + 1 < cellCount ? scratch.runLefts[cell + 1] : static_cast<float>(runPen);
        LetterExtent& extent = out[first + cell];
        extent.leftInPixels = static_cast<float>(penInPixels + static_cast<double>(left) * widthScale);
        extent.advanceInPixels = static_cast<float>(static_cast<double>(right - left) * widthScale);
    }
    return penInPixels + runPen * widthScale;
}

} // namespace

bool layoutRowLetters(std::span<const LetterCell> cells, std::span<const std::uint32_t> codePointPool,
    double cellAdvanceInPixels, double widthScale, LetterShaper shaper, void* shaperContext,
    LetterLayoutScratch& scratch, std::span<LetterExtent> out)
{
    const std::size_t columns = std::min(cells.size(), out.size());
    const double pitch = cellAdvanceInPixels * (widthScale > 0.0 ? widthScale : 1.0);
    bool didShape = false;
    double pen = 0.0;
    std::size_t column = 0;
    while (column < columns) {
        const LetterCell& cell = cells[column];
        if (cell.kind == LetterCellKind::Letter && shaper != nullptr) {
            std::size_t end = column + 1;
            while (end < columns && cells[end].kind == LetterCellKind::Letter && cells[end].runKey == cell.runKey
                && cells[end].face == cell.face && cells[end].weight == cell.weight)
                ++end;
            const double after = placeShapedRun(cells, column, end, codePointPool, pen, widthScale > 0.0 ? widthScale : 1.0,
                shaper, shaperContext, scratch, out);
            if (after >= 0.0) {
                didShape = true;
                pen = after;
                column = end;
                continue;
            }
            // Refused: the run keeps the pitch, cell by cell.
            for (std::size_t k = column; k < end; ++k) {
                out[k].leftInPixels = static_cast<float>(pen);
                out[k].advanceInPixels = static_cast<float>(pitch);
                pen += pitch;
            }
            column = end;
            continue;
        }
        if (cell.kind == LetterCellKind::CellBound) {
            // The lead takes the claim; each continuation inside it takes the
            // slice the claim's own pitch puts there, so a pointer on the
            // second half of a wide glyph still names the second column.
            const double claim = static_cast<double>(cell.widthCells > 0.0f ? cell.widthCells : 1.0f) * cellAdvanceInPixels;
            out[column].leftInPixels = static_cast<float>(pen);
            out[column].advanceInPixels = static_cast<float>(claim);
            std::size_t continuation = column + 1;
            std::size_t covered = 1;
            while (continuation < columns && cells[continuation].kind == LetterCellKind::Continuation)
                ++continuation, ++covered;
            const double slice = claim / static_cast<double>(covered);
            for (std::size_t k = 1; k < covered; ++k) {
                out[column + k].leftInPixels = static_cast<float>(pen + slice * static_cast<double>(k));
                out[column + k].advanceInPixels = static_cast<float>(slice);
            }
            pen += claim;
            column = continuation;
            continue;
        }
        // Empty, a letter with no shaper, or a continuation whose lead the
        // caller did not mark: one cell at the pitch.
        out[column].leftInPixels = static_cast<float>(pen);
        out[column].advanceInPixels = static_cast<float>(pitch);
        pen += pitch;
        ++column;
    }
    return didShape;
}

int letterColumnAt(std::span<const LetterExtent> letters, double xInPixels)
{
    if (letters.empty())
        return 0;
    int last = 0;
    for (std::size_t column = 0; column < letters.size(); ++column) {
        const LetterExtent& extent = letters[column];
        if (extent.advanceInPixels <= 0.0f)
            continue;
        last = static_cast<int>(column);
        if (xInPixels < static_cast<double>(extent.leftInPixels) + static_cast<double>(extent.advanceInPixels))
            return static_cast<int>(column);
    }
    return last;
}

double letterXInCells(std::span<const LetterExtent> letters, double xInPixels)
{
    if (letters.empty())
        return 0.0;
    const int column = letterColumnAt(letters, xInPixels);
    const LetterExtent& extent = letters[static_cast<std::size_t>(column)];
    if (extent.advanceInPixels <= 0.0f)
        return static_cast<double>(column);
    const double fraction = (xInPixels - static_cast<double>(extent.leftInPixels)) / static_cast<double>(extent.advanceInPixels);
    return std::clamp(static_cast<double>(column) + fraction, 0.0, static_cast<double>(letters.size()));
}

} // namespace Ubiquitty::Rendering
