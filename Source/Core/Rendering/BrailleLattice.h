// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Braille dots occupy one global 2x4 lattice. Cell advances stay fractional:
// neither cell rectangles nor quantized neighboring positions determine a dot.
// Horizontal pens use the frame's text phase policy; vertical centers retain
// their fractional position across terminal rows. Raster tiles instead share
// rounded boundaries so adjacent filled subcells meet without seams.

#include "Base/Compiler.h"
#include "Base/Geometry.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/SubpixelPhase.h"

#include <cstdint>
#include <string_view>

namespace Ubiquitty::Rendering {

/// Selects the raised-dot shape and the guides for unused lattice positions.
enum class UBIQUITTY_CLOSED_ENUM BrailleStyle {
    Regular,
    Outline6Dot,
    Outline8Dot,
    Pinpoint6Dot,
    Pinpoint8Dot,
    Square,
    Raster,
};

/// Supplies the procedural dot style when no preference is specified.
inline constexpr BrailleStyle defaultBrailleStyle = BrailleStyle::Regular;

/// Resolves a configuration token, using the default style for unknown tokens.
[[nodiscard]] BrailleStyle brailleStyleFromString(std::string_view);
/// Returns the configuration token for a dot style.
[[nodiscard]] std::string_view brailleStyleToString(BrailleStyle);

/// The first braille code point, U+2800, whose cell carries no dots.
inline constexpr std::uint32_t brailleBlankCodePoint = 0x2800;
/// One past the last, U+28FF.
inline constexpr std::uint32_t brailleEndCodePoint = 0x2900;
/// Dot columns in one cell.
inline constexpr int brailleDotColumns = 2;
/// Dot rows in one cell.
inline constexpr int brailleDotRows = 4;
/// Dots in one cell, and the most rects one cell can need.
inline constexpr int brailleDotsPerCell = brailleDotColumns * brailleDotRows;

/// Whether `codePoint` is a braille pattern.
///
/// - Parameter codePoint: The character to test.
/// - Returns: True for U+2800..U+28FF.
[[nodiscard]] constexpr bool isBraille(std::uint32_t codePoint)
{
    return codePoint >= brailleBlankCodePoint && codePoint < brailleEndCodePoint;
}

/// Whether the dot at `column`, `row` of a cell is raised.
///
/// The bit order is the Unicode block's, which is not the reading order: the
/// first six dots number down the left column then down the right, and the two
/// eight-dot additions come last. Reading it as a raster would put dots 7 and 8
/// in the wrong place, and only on characters that use them.
///
/// - Parameter codePoint: A braille pattern.
/// - Parameter column: 0 for the left dot column, 1 for the right.
/// - Parameter row: 0 at the top through 3 at the bottom.
/// - Returns: Whether that dot is raised.
[[nodiscard]] constexpr bool isBrailleDotRaised(std::uint32_t codePoint, int column, int row)
{
    if (!isBraille(codePoint) || column < 0 || column >= brailleDotColumns || row < 0 || row >= brailleDotRows)
        return false;
    // Dots 1..6 occupy bits 0..5 in column-major order; dots 7 and 8 are the
    // bottom row and were appended at bits 6 and 7.
    const int bit = row == 3 ? 6 + column : column * 3 + row;
    return ((codePoint - brailleBlankCodePoint) & (1u << bit)) != 0;
}

/// The grid a braille cell's dots are placed on.
///
/// The origin and the fractional cell extent, which is everything a dot's
/// position is derived from. The cell's own snapped rect is not among them:
/// interpolating inside a rect whose width alternates 14 and 15 puts the dots
/// on the alternating pitch this file opens with.
struct BrailleLattice {
    /// The grid's top-left corner in device pixels, including the shared
    /// vertical text-alignment shift when applied.
    float originX { 0.0f };
    float originY { 0.0f };
    /// One cell's extent in device pixels, fractional as measured.
    float cellWidth { 0.0f };
    float cellHeight { 0.0f };
    BrailleStyle style { defaultBrailleStyle };
    /// Horizontal raster policy shared with the frame's text glyphs.
    int phaseCount { subpixelPhaseCount };
    PhaseRounding phaseRounding { PhaseRounding::Nearest };
};

/// One dot's rectangle in device pixels.
struct BrailleDotRect {
    float left { 0.0f };
    float top { 0.0f };
    float right { 0.0f };
    float bottom { 0.0f };
};

/// Aligns the lattice with the text's x-height center without changing its pitch.
///
/// The uniform pixel shift stays within the outer dot margins so dots remain
/// inside their terminal rows, including at the viewport edges.
/// Raster cells have no margins and retain the cell origin to tile exactly.
///
/// - Parameters:
///   - lattice: The unshifted cell grid in device pixels.
///   - baselineFromTop: The text baseline relative to the cell top.
///   - xHeight: The text's measured x-height in device pixels.
/// - Returns: The lattice with a shared vertical origin for every pattern.
[[nodiscard]] BrailleLattice textAlignedBrailleLattice(BrailleLattice lattice, float baselineFromTop, float xHeight);

/// The side every dot on the grid is drawn at, in whole device pixels.
///
/// Half the horizontal dot pitch. That axis is the tighter of the two — two dot
/// columns across the cell's width against four rows down its greater height —
/// so a dot sized from it leaves a gap at least as wide as itself on both,
/// which is the proportion braille is set at. Unifont draws a 4 pixel dot in a
/// 16 pixel cell, and this returns 4 there.
///
/// Square, and not a fraction of each axis: a braille dot is round, and a
/// fraction of a sub-cell taller than it is wide would draw an ellipse that
/// leans further over as line spacing opens the row.
///
/// One size for every dot on the page, which is what the eye reads a lattice
/// by. Sizing each dot from its own snapped sub-cell would alternate 3 and 4
/// pixels wherever the advance is fractional, and a field of braille would come
/// out mottled with its pitch perfectly even.
///
/// - Parameter lattice: The grid, read for its cell extent.
/// - Returns: The side, at least one pixel and never wider than a sub-cell.
[[nodiscard]] float brailleDotSide(const BrailleLattice& lattice);

/// The rectangle the dot at `column`, `row` of the cell at `gridX`, `gridY`
/// occupies.
///
/// Centers derive from the global dot index. Horizontal positions use the
/// text phase selector; vertical positions remain fractional. The ink size
/// stays constant regardless of phase. Raster returns shared snapped tile
/// boundaries instead of dot bounds.
///
/// - Parameters:
///   - lattice: The grid the cell sits on.
///   - gridX: The cell's column in the grid.
///   - gridY: The cell's visible row.
///   - column: The dot's column within the cell, 0 or 1.
///   - row: The dot's row within the cell, 0 through 3.
/// - Returns: The rectangle, in device pixels.
[[nodiscard]] BrailleDotRect brailleDotRect(const BrailleLattice& lattice, int gridX, int gridY, int column, int row);

/// Writes one braille cell's raised dots and optional guides in its chosen style.
///
/// Rects rather than a decomposition, because the positions come from the grid
/// and a decomposition speaks in unit-cell coordinates a caller would scale
/// into the cell's own rect — the arrangement that costs the even pitch.
///
/// Written into the same stream the box fills use, so every backend that
/// already draws procedural cell geometry draws braille with no further
/// plumbing.
///
/// - Parameters:
///   - codePoint: The braille pattern; anything else writes nothing.
///   - lattice: The grid the cell sits on.
///   - gridX: The cell's column.
///   - gridY: The cell's visible row.
///   - foreground: The cell's resolved foreground.
///   - out: Quads to write, at most `maxOut`; null writes nothing and counts.
///   - maxOut: Write capacity. A quad past it is counted, never written.
/// - Returns: How many quads the cell needs, which may exceed `maxOut` so the
///   caller can grow and emit again.
[[nodiscard]] int emitBrailleDots(std::uint32_t codePoint, const BrailleLattice& lattice, int gridX, int gridY,
    Vec4 foreground, OverlayInstance* out, int maxOut);

} // namespace Ubiquitty::Rendering
