// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ProportionalRun.h"

#include "Core/Rendering/CellDecisions.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

namespace {

/// Walks source-ordered clusters while opening each interior space once.
class JustificationCursor {
public:
    JustificationCursor(ProportionalSourceSpan codePoints, double gapExtraInPixels)
        : m_codePoints(codePoints)
        , m_gapExtraInPixels(std::max(0.0, gapExtraInPixels))
        , m_lastInked(codePoints.size())
    {
        if (m_gapExtraInPixels == 0.0) {
            m_next = m_lastInked;
            return;
        }
        while (m_next < m_lastInked && codePoints[m_next] == 0x20u) {
            ++m_next;
        }
        while (m_lastInked > m_next && codePoints[m_lastInked - 1] == 0x20u) {
            --m_lastInked;
        }
    }

    double offsetBefore(std::size_t sourceIndex)
    {
        const std::size_t end = std::min(sourceIndex, m_lastInked);
        while (m_next < end) {
            if (m_codePoints[m_next] == 0x20u) {
                m_opened += m_gapExtraInPixels;
            }
            ++m_next;
        }
        return m_opened;
    }

private:
    ProportionalSourceSpan m_codePoints;
    double m_gapExtraInPixels = 0.0;
    std::size_t m_next = 0;
    std::size_t m_lastInked = 0;
    double m_opened = 0.0;
};

} // namespace

void proportionalJustificationOffsets(ProportionalSourceSpan codePoints,
    double gapExtraInPixels, ProportionalSourceOffsets& offsets)
{
    offsets.resize(codePoints.size() + 1);
    JustificationCursor cursor(codePoints, gapExtraInPixels);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        offsets[index] = cursor.offsetBefore(index);
    }
}

ProportionalRunLayout layoutProportionalRun(std::span<const ProportionalShapedGlyph> shaped,
    std::span<const std::uint32_t> codePoints, int widthCells, std::uint8_t horizontalAlign,
    double cellAdvanceInPixels, std::span<ProportionalGlyphPlacement> out)
{
    ProportionalRunLayout layout;
    if (shaped.empty() || widthCells < 1 || cellAdvanceInPixels <= 0.0)
        return layout;
    double textWidth = 0.0;
    bool hasSourceOrderedClusters = true;
    std::uint16_t previousCluster = 0;
    for (const ProportionalShapedGlyph& glyph : shaped) {
        textWidth += static_cast<double>(glyph.advanceInPixels);
        hasSourceOrderedClusters &= glyph.cluster >= previousCluster;
        previousCluster = glyph.cluster;
    }

    // Justification opens each interior space, not each glyph: the words
    // spread while the letters keep the face's own spacing. A ligature's
    // glyphs share a cluster, so a space is found by the code point its glyph
    // begins at.
    double gapExtra = 0.0;
    if (horizontalAlign == 3) {
        const int gapCount = proportionalJustifyGapCount(codePoints);
        gapExtra = proportionalJustifyGapInPixels(textWidth, widthCells, cellAdvanceInPixels, gapCount);
        textWidth += gapExtra * static_cast<double>(gapCount);
    }

    // Right and centre alignment move the whole run by the slack; a justified
    // run fills its cells and a left one starts where the cells do.
    const double available = static_cast<double>(widthCells) * cellAdvanceInPixels;
    const double slack = std::max(0.0, available - textWidth);
    layout.penOffsetInPixels = horizontalAlign == 1 ? slack : (horizontalAlign == 2 ? slack / 2.0 : 0.0);
    layout.widthInPixels = textWidth;

    double pen = layout.penOffsetInPixels;
    // Source-ordered runs need constant workspace. Reordered justified runs
    // own one source-sized prefix table so visual order cannot open gaps twice.
    ProportionalSourceOffsets sourceOffsets;
    if (gapExtra > 0.0 && !hasSourceOrderedClusters) {
        proportionalJustificationOffsets(codePoints, gapExtra, sourceOffsets);
    }
    JustificationCursor justification(codePoints, gapExtra);
    int written = 0;
    for (const ProportionalShapedGlyph& glyph : shaped) {
        const double opened = gapExtra <= 0.0 ? 0.0
            : hasSourceOrderedClusters        ? justification.offsetBefore(glyph.cluster)
                                              : sourceOffsets[std::min<std::size_t>(glyph.cluster, codePoints.size())];
        if (static_cast<std::size_t>(written) < out.size()) {
            ProportionalGlyphPlacement& placement = out[static_cast<std::size_t>(written)];
            placement.glyph = glyph.glyph;
            placement.xInPixels = static_cast<float>(pen + opened + static_cast<double>(glyph.offsetXInPixels));
            placement.yInPixels = glyph.offsetYInPixels;
            placement.cluster = glyph.cluster;
            ++written;
        }
        pen += static_cast<double>(glyph.advanceInPixels);
    }
    layout.glyphCount = written;
    return layout;
}

void proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement> placements, const ProportionalRunLayout& layout,
    int codePointCount, int widthCells, double cellAdvanceInPixels, std::span<std::uint16_t> cellOfCodePoint)
{
    const int lastCell = std::max(0, widthCells - 1);
    const int count = std::min(codePointCount, static_cast<int>(cellOfCodePoint.size()));
    // A code point no glyph begins at — one a ligature absorbed, or one the
    // shaper dropped — takes the cell of the nearest glyph before it, which
    // is the cluster it rides; before any glyph, the first cell.
    int owner = 0;
    int next = 0;
    for (int index = 0; index < count; ++index) {
        while (next < layout.glyphCount && placements[static_cast<std::size_t>(next)].cluster <= index) {
            const double x = static_cast<double>(placements[static_cast<std::size_t>(next)].xInPixels);
            owner = cellAdvanceInPixels > 0.0 ? static_cast<int>(std::floor(x / cellAdvanceInPixels)) : 0;
            ++next;
        }
        // The first code point's cell is where its ink begins like every
        // other's, however far in the alignment put it: a pointer on a
        // centred run's first letter is on that cell, and a selection from
        // it starts there rather than at the anchor cells of slack away. The
        // anchor still stores the first code point for the grid's own
        // readers; the owner map is what the copy and the pointer read.
        cellOfCodePoint[static_cast<std::size_t>(index)] = static_cast<std::uint16_t>(std::clamp(owner, 0, lastCell));
    }
}

int proportionalRunTextCells(const ProportionalRunLayout& layout, int widthCells, double cellAdvanceInPixels)
{
    const int lastCell = std::max(1, widthCells);
    if (layout.glyphCount <= 0 || cellAdvanceInPixels <= 0.0)
        return 1;
    // The epsilon forgives an advance that lands on a cell edge by a rounding
    // error, as the measure query's cell count does.
    const double end = layout.penOffsetInPixels + layout.widthInPixels;
    const int cells = static_cast<int>(std::ceil(end / cellAdvanceInPixels - 1e-6));
    return std::clamp(cells, 1, lastCell);
}

int proportionalRunCodePointAt(std::span<const ProportionalGlyphPlacement> placements,
    const ProportionalRunLayout& layout, int codePointCount, double xInPixels)
{
    if (layout.glyphCount <= 0 || codePointCount <= 0)
        return 0;
    const double end = layout.penOffsetInPixels + layout.widthInPixels;
    if (xInPixels >= end)
        return codePointCount;
    // Glyphs come in text order; the one whose extent holds the pixel names
    // its cluster, and a pixel before the first glyph names the first.
    for (int index = 0; index < layout.glyphCount; ++index) {
        const double left = static_cast<double>(placements[static_cast<std::size_t>(index)].xInPixels);
        const double right = index + 1 < layout.glyphCount
            ? static_cast<double>(placements[static_cast<std::size_t>(index + 1)].xInPixels)
            : end;
        if (xInPixels < right)
            return std::min(static_cast<int>(placements[static_cast<std::size_t>(index)].cluster), codePointCount);
        (void)left;
    }
    return codePointCount;
}

ProportionalRunCaret proportionalRunCaretAt(std::span<const ProportionalGlyphPlacement> placements,
    const ProportionalRunLayout& layout, int codePointCount, double xInPixels)
{
    if (layout.glyphCount <= 0 || codePointCount <= 0)
        return ProportionalRunCaret {};
    const double end = layout.penOffsetInPixels + layout.widthInPixels;
    if (xInPixels >= end)
        return ProportionalRunCaret { .codePointIndex = codePointCount, .didRoundForward = true };
    const int glyphCount = std::min(layout.glyphCount, static_cast<int>(placements.size()));
    for (int index = 0; index < glyphCount; ++index) {
        const double left = static_cast<double>(placements[static_cast<std::size_t>(index)].xInPixels);
        const double right = index + 1 < glyphCount
            ? static_cast<double>(placements[static_cast<std::size_t>(index + 1)].xInPixels)
            : end;
        if (xInPixels >= right)
            continue;
        // The nearer of the glyph's two edges: its left edge is the caret
        // before its cluster, and its right edge the caret after it, which is
        // the next glyph's cluster or the end of the text for the last glyph.
        const bool isNearerRight = xInPixels - left > right - xInPixels;
        const int forward = index + 1 < glyphCount
            ? static_cast<int>(placements[static_cast<std::size_t>(index + 1)].cluster)
            : codePointCount;
        const int back = static_cast<int>(placements[static_cast<std::size_t>(index)].cluster);
        return ProportionalRunCaret {
            .codePointIndex = std::clamp(isNearerRight ? forward : back, 0, codePointCount),
            .didRoundForward = isNearerRight,
        };
    }
    return ProportionalRunCaret { .codePointIndex = codePointCount, .didRoundForward = true };
}

} // namespace Ubiquitty::Rendering
