// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The circle round a character: U+20DD COMBINING ENCLOSING CIRCLE, and SGR 52
// (encircled), which asks for the same mark. Core draws it rather than the
// font. A face that lacks the mark sends the whole cluster to a fallback face,
// so the letter inside is another font's; the mark is placed on each letter's
// own ink, so a c's circle sits lower than an A's and a g's lower still; and
// the fallback's circle is wider and deeper than a cell, so the slot the
// cluster is drawn into crops it. Drawn here, the letter is the one the cell
// draws without the mark, and the circle is one size at one height on every
// cell of a row, on every renderer.

#include "Core/Rendering/CellInstance.h"
#include "Core/TextSizeTable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering {

/// U+20DD COMBINING ENCLOSING CIRCLE, the mark Core draws in place of the font.
inline constexpr std::uint32_t combiningEnclosingCircle = 0x20DD;

/// The size an enclosed character is drawn at, as an OSC 66 spec: seven
/// eighths of its own, its cell box shrunk about the box's centre, so the
/// circle clears the letter while an e and a k keep one baseline. A bitmap
/// face keeps its own size, its glyphs being whole pixels that a smaller size
/// would resample.
inline constexpr TextSizeSpec enclosedTextSize { .scale = 1, .widthCells = 0, .fractionNumerator = 7, .fractionDenominator = 8, .verticalAlign = 2, .horizontalAlign = 2 };

/// The stroke of a frame or an enclosing circle, as a share of the underline's
/// weight. A line round text reads heavier than one under it, so it is drawn
/// at two thirds of the rule: at 2x a 2-pixel line beside 3-pixel stems, the
/// weight Unicode's own enclosed forms draw their circles at.
inline constexpr float enclosureWeightShare = 2.0f / 3.0f;

/// Returns the stroke of a frame or an enclosing circle, in whole device
/// pixels, never under one.
///
/// - Parameter ruleWeight: The underline's weight, in device pixels.
/// - Returns: The stroke.
[[nodiscard]] inline float enclosureWeight(float ruleWeight)
{
    return std::max(1.0f, std::round(ruleWeight * enclosureWeightShare));
}

/// Removes every enclosing circle from a cluster, keeping the order of the rest.
///
/// - Parameter codePoints: The cluster, edited in place.
/// - Returns: How many code points remain; the cluster's own count when it held
///   no enclosing circle.
[[nodiscard]] inline int dropEnclosingCircles(std::span<std::uint32_t> codePoints)
{
    int kept = 0;
    for (const std::uint32_t codePoint : codePoints) {
        if (codePoint != combiningEnclosingCircle)
            codePoints[static_cast<std::size_t>(kept++)] = codePoint;
    }
    return kept;
}

/// The overlay shape kind that draws a circle round its ink box: Braille's dot,
/// which every renderer's box-fill pass draws, hollow at its stroke.
inline constexpr float enclosingCircleShapeKind = -2.0f;

/// Returns the circle round a character's cells, as the hollow overlay every
/// renderer draws.
///
/// The circle is two cells across, the width of a wide character, whatever it
/// encloses: a wide character's circle is its own two cells, and a narrow
/// character's reaches half a cell into the cells beside it, so one space
/// between two enclosed letters keeps their circles apart. It is centred on
/// the character's cells and the row, and never taller than the row: on a row
/// shorter than two cells it is the row's height across rather than reaching
/// into the rows beside it. The box is whole pixels, so every circle on a
/// screen is the same raster. It is drawn as Braille's dot is, the circle in
/// the ink box `plA` and the quad half a pixel wider for the edge's coverage.
/// The quad stops at the grid's edges: a renderer repainting part of a frame
/// leaves the margins alone, so ink drawn there would stay, and a circle in
/// the first or last column is cut at the grid's edge on every renderer.
///
/// - Parameters:
///   - cellsLeft: The left edge of the character's first cell, in device pixels.
///   - rowTop: The top of the row, in device pixels.
///   - cellsRight: The right edge of the character's last cell, in device pixels.
///   - rowBottom: The bottom of the row, in device pixels.
///   - cellWidth: One cell's width, in device pixels.
///   - gridLeft: The grid's left edge, in device pixels.
///   - gridRight: The grid's right edge, in device pixels.
///   - weight: The stroke, in whole device pixels.
///   - color: The circle's colour.
/// - Returns: The overlay instance that draws the circle.
[[nodiscard]] inline OverlayInstance enclosingCircle(float cellsLeft, float rowTop, float cellsRight, float rowBottom, float cellWidth, float gridLeft, float gridRight, float weight, Vec4 color)
{
    const float smallest = weight * 2.0f + 1.0f;
    const float side = std::max(std::min(std::round(cellWidth * 2.0f), std::round(rowBottom - rowTop)), smallest);
    const float left = std::round((cellsLeft + cellsRight - side) * 0.5f);
    const float top = std::round((rowTop + rowBottom - side) * 0.5f);
    OverlayInstance circle {};
    circle.topLeftInPixels = { std::max(left - 0.5f, gridLeft), top - 0.5f };
    circle.bottomRightInPixels = { std::min(left + side + 0.5f, gridRight), top + side + 0.5f };
    circle.color = color;
    circle.cornerRadius = side * 0.5f;
    circle.cornerStyle = 0.0f;
    circle.strokeWidth = weight;
    circle.shapeKind = enclosingCircleShapeKind;
    circle.plA = { left, top, left + side, top + side };
    return circle;
}

} // namespace Ubiquitty::Rendering
