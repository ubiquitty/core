// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/BrailleLattice.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

namespace {

/// The snapped device pixel a sub-grid boundary falls on.
///
/// One expression for every boundary: the right edge of sub-cell `k` and the
/// left edge of sub-cell `k + 1` are the same call on the same index, so the
/// two cannot round apart and open a gap the pitch has to absorb.
///
/// - Parameter origin: The grid's origin on this axis, device pixels.
/// - Parameter index: The sub-grid index of the boundary.
/// - Parameter pitch: One sub-cell's extent, device pixels, fractional.
/// - Returns: The boundary, snapped to a whole device pixel.
float latticeBoundary(float origin, int index, float pitch)
{
    return std::round(origin + static_cast<float>(index) * pitch);
}

} // namespace

BrailleStyle brailleStyleFromString(std::string_view name)
{
    if (name == "regular") {
        return BrailleStyle::Regular;
    }
    if (name == "square") {
        return BrailleStyle::Square;
    }
    if (name == "raster") {
        return BrailleStyle::Raster;
    }
    if (name == "outline-6") {
        return BrailleStyle::Outline6Dot;
    }
    if (name == "outline-8") {
        return BrailleStyle::Outline8Dot;
    }
    if (name == "pinpoint-6") {
        return BrailleStyle::Pinpoint6Dot;
    }
    if (name == "pinpoint-8") {
        return BrailleStyle::Pinpoint8Dot;
    }
    return defaultBrailleStyle;
}

std::string_view brailleStyleToString(BrailleStyle style)
{
    switch (style) {
    case BrailleStyle::Regular:
        return "regular";
    case BrailleStyle::Outline6Dot:
        return "outline-6";
    case BrailleStyle::Outline8Dot:
        return "outline-8";
    case BrailleStyle::Pinpoint6Dot:
        return "pinpoint-6";
    case BrailleStyle::Pinpoint8Dot:
        return "pinpoint-8";
    case BrailleStyle::Square:
        return "square";
    case BrailleStyle::Raster:
        return "raster";
    }
    return "regular";
}

float brailleDotSide(const BrailleLattice& lattice)
{
    const float subCellWidth = lattice.cellWidth / static_cast<float>(brailleDotColumns);
    const float subCellHeight = lattice.cellHeight / static_cast<float>(brailleDotRows);
    const float side = std::round(lattice.style == BrailleStyle::Square
            ? std::min(subCellWidth, subCellHeight) * 0.75f
            : subCellWidth * 0.5f);
    // A cell too small to hold that leaves the dot filling its sub-cell rather
    // than overhanging into its neighbour's.
    const float ceiling = std::floor(std::min(subCellWidth, subCellHeight));
    return std::max(1.0f, std::min(side, ceiling));
}

BrailleLattice textAlignedBrailleLattice(BrailleLattice lattice, float baselineFromTop, float xHeight)
{
    if (lattice.style == BrailleStyle::Raster || lattice.cellWidth <= 0.0f || lattice.cellHeight <= 0.0f || xHeight <= 0.0f) {
        return lattice;
    }
    const float textCenter = baselineFromTop - xHeight * 0.5f;
    const float offset = std::round(textCenter - lattice.cellHeight * 0.5f);
    const float minimumPitch = std::floor(lattice.cellHeight / static_cast<float>(brailleDotRows));
    const float margin = std::max(0.0f, std::floor((minimumPitch - brailleDotSide(lattice)) * 0.5f));
    lattice.originY += std::clamp(offset, -margin, margin);
    return lattice;
}

BrailleDotRect brailleDotRect(const BrailleLattice& lattice, int gridX, int gridY, int column, int row)
{
    BrailleDotRect rect;
    if (lattice.cellWidth <= 0.0f || lattice.cellHeight <= 0.0f)
        return rect;
    // The sub-cell extents. Fractional on purpose: the rounding belongs on the
    // boundaries below, where two neighbours share it, and never on the pitch —
    // a rounded pitch multiplied by an index is an error that grows with the
    // index, which is the drift across a wide plot this exists to avoid.
    const float subCellWidth = lattice.cellWidth / static_cast<float>(brailleDotColumns);
    const float subCellHeight = lattice.cellHeight / static_cast<float>(brailleDotRows);
    const int columnIndex = gridX * brailleDotColumns + column;
    const int rowIndex = gridY * brailleDotRows + row;
    if (lattice.style == BrailleStyle::Raster) {
        return { latticeBoundary(lattice.originX, columnIndex, subCellWidth),
            latticeBoundary(lattice.originY, rowIndex, subCellHeight),
            latticeBoundary(lattice.originX, columnIndex + 1, subCellWidth),
            latticeBoundary(lattice.originY, rowIndex + 1, subCellHeight) };
    }

    const float side = brailleDotSide(lattice);
    // Quantize each global pen, never the advance or a cell-relative offset.
    // The phase carries its fraction exactly as a text atlas mask does.
    const float idealCenterX = lattice.originX + (static_cast<float>(columnIndex) + 0.5f) * subCellWidth;
    const auto phase = phaseForPen(idealCenterX, lattice.phaseCount, lattice.phaseRounding);
    const float centerX = static_cast<float>(phase.snappedPenX) + static_cast<float>(phase.phase) / lattice.phaseCount;
    const float centerY = lattice.originY + (static_cast<float>(rowIndex) + 0.5f) * subCellHeight;
    rect.left = centerX - side * 0.5f;
    rect.right = rect.left + side;
    rect.top = centerY - side * 0.5f;
    rect.bottom = rect.top + side;
    return rect;
}

int emitBrailleDots(std::uint32_t codePoint, const BrailleLattice& lattice, int gridX, int gridY,
    Vec4 foreground, OverlayInstance* out, int maxOut)
{
    if (!isBraille(codePoint))
        return 0;
    int needed = 0;
    const bool isOutline = lattice.style == BrailleStyle::Outline6Dot || lattice.style == BrailleStyle::Outline8Dot;
    const bool isPinpoint = lattice.style == BrailleStyle::Pinpoint6Dot || lattice.style == BrailleStyle::Pinpoint8Dot;
    const int guideRows = lattice.style == BrailleStyle::Outline6Dot || lattice.style == BrailleStyle::Pinpoint6Dot ? 3 : 4;
    for (int column = 0; column < brailleDotColumns; ++column) {
        for (int row = 0; row < brailleDotRows; ++row) {
            const bool isRaised = isBrailleDotRaised(codePoint, column, row);
            if (!isRaised && (!(isOutline || isPinpoint) || row >= guideRows)) {
                continue;
            }
            const BrailleDotRect dot = brailleDotRect(lattice, gridX, gridY, column, row);
            // A degenerate rect is a cell too small to carry the lattice — a
            // sub-cell under a pixel wide rounds to nothing. Counted out rather
            // than emitted as an empty quad the backends would still encode.
            if (dot.right <= dot.left || dot.bottom <= dot.top)
                continue;
            if (out && needed < maxOut) {
                OverlayInstance& quad = out[needed];
                quad = {};
                quad.topLeftInPixels = { dot.left, dot.top };
                quad.bottomRightInPixels = { dot.right, dot.bottom };
                quad.color = foreground;
                quad.cornerRadius = lattice.style == BrailleStyle::Square || lattice.style == BrailleStyle::Raster
                    ? 0.0f
                    : (dot.right - dot.left) * 0.5f;
                quad.cornerStyle = 0.0f;
                quad.strokeWidth = !isRaised && isOutline ? std::max(0.5f, quad.cornerRadius * 0.35f) : 0.0f;
                if (!isRaised && isPinpoint) {
                    const float inset = (dot.right - dot.left) / 3.0f;
                    quad.topLeftInPixels = { dot.left + inset, dot.top + inset };
                    quad.bottomRightInPixels = { dot.right - inset, dot.bottom - inset };
                    quad.cornerRadius = (dot.right - dot.left) / 6.0f;
                }
                quad.shadeDensity = 0.0f;
                quad.shapeKind = 0.0f; // ordinary axis-aligned fill
                quad.plA = { 0.0f, 0.0f, 0.0f, 0.0f };
                quad.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
                if (lattice.style != BrailleStyle::Raster) {
                    quad.shapeKind = -2.0f;
                    quad.plA = { quad.topLeftInPixels.x, quad.topLeftInPixels.y,
                        quad.bottomRightInPixels.x, quad.bottomRightInPixels.y };
                    // The raster quad includes the fractional edge's coverage footprint.
                    quad.topLeftInPixels.x -= 0.5f;
                    quad.topLeftInPixels.y -= 0.5f;
                    quad.bottomRightInPixels.x += 0.5f;
                    quad.bottomRightInPixels.y += 0.5f;
                }
            }
            ++needed;
        }
    }
    return needed;
}

} // namespace Ubiquitty::Rendering
