// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

// Shared placement and coverage for glyphs assembled across terminal rows.
// Rasterizers supply font ink bounds and draw with the returned transform.
// Core owns which pieces join, which may stretch, their stem donors, and the
// coverage at each seam. Horizontal font placement is preserved: centring a
// narrow extension independently of its hook would move their stems apart.

namespace Ubiquitty::Rendering {

/// Whether a code point must be drawn on the plain text pen.
///
/// True for the bracket-assembly pieces — the parenthesis, square-bracket and
/// curly-brace sections, the integral halves and their extensions — and for the
/// vertical box lines, whose whole content is which edge they stand on.
///
/// - Parameter codePoint: The cell's code point.
/// - Returns: True when no pen may re-place it.
[[nodiscard]] constexpr bool tilesOnTextPen(unsigned codePoint)
{
    // The integral's halves and the extension shared with the braces.
    if (codePoint == 0x2320 || codePoint == 0x2321)
        return true;
    // U+239B..U+23B1: parentheses, square brackets, curly braces, their
    // extensions and the two S-shaped brace sections. Nearly one contiguous
    // run, because Unicode allocated the family as one — except for U+23AF,
    // which sits in the middle of it and belongs to the other axis: the
    // horizontal line extension continues across a row, not down a column, and
    // fitting it to the cell's height would stretch a rule one stroke thick
    // into a filled cell.
    if (codePoint >= 0x239B && codePoint <= 0x23B1 && codePoint != 0x23AF)
        return true;
    // The summation's halves: a two-piece assembly, moved rather than shaped.
    if (codePoint == 0x23B2 || codePoint == 0x23B3)
        return true;
    // The radical's foot, which a horizontal line extension continues over.
    if (codePoint == 0x23B7)
        return true;
    // The left and right vertical box lines.
    if (codePoint == 0x23B8 || codePoint == 0x23B9)
        return true;
    // The vertical line extension: a stem and nothing else, so it is only ever
    // one row of a taller line.
    if (codePoint == 0x23D0)
        return true;
    // The half parentheses, a two-row assembly with no extension between them.
    if (codePoint >= 0x2E59 && codePoint <= 0x2E5C)
        return true;
    return false;
}

/// Whether a face's raster keeps the horizontal place its face drew it at.
///
/// A box-drawing piece meets its neighbours on the cell's centre column, and
/// its ink is lopsided by design: a corner's turning stroke stands at the
/// centre of the advance and its arm runs to one side. Centring that ink in
/// the cell moves the turning stroke by half the arm, off the column the
/// vertical in the next row stands on, so a frame breaks at every corner and a
/// double rule's rails cross. The bracket pieces are the same fact in another
/// family.
///
/// - Parameter codePoint: The cell's code point.
/// - Returns: True when the raster lands on the face's own pen.
[[nodiscard]] constexpr bool keepsFacePlacement(unsigned codePoint)
{
    return tilesOnTextPen(codePoint) || (codePoint >= 0x2500 && codePoint <= 0x257F);
}

/// Whether a face raster of this code point is drawn from the unhinted outline.
///
/// Hinting fits each glyph to the pixel grid on its own, so it moves the same
/// stroke by different amounts in different glyphs. A box-drawing piece is one
/// fragment of a rule that its neighbours continue, so a stroke moved in one
/// glyph and not the next is a step at the cell edge. JetBrains Mono at 20 px
/// under the bytecode hinter puts the light horizontal of U+2500 at rows
/// [-8, -6) but leaves it at its design [-8.41, -6.41) in U+253C, and moves
/// the vertical of U+253C 0.41 px right of U+2502's. The unhinted outlines
/// agree to the 1/64 pixel, and ``boxFamilyPixelOffset`` then lands the whole
/// family on whole pixels with one translation.
///
/// - Parameter codePoint: The cell's code point.
/// - Returns: True for the box-drawing block, U+2500..U+257F.
[[nodiscard]] constexpr bool rastersFromUnhintedOutline(unsigned codePoint)
{
    return codePoint >= 0x2500 && codePoint <= 0x257F;
}

/// The offset, in pixels, that centres one measured stroke on the pixel grid.
///
/// A stroke whose width rounds to an even number of pixels is centred on a
/// pixel boundary, an odd one on a pixel centre, so a stroke whose outline
/// width is a whole number of pixels covers whole pixels only. The offset is
/// in the same coordinates as the edges, whichever way they run.
///
/// - Parameters:
///   - low: The stroke's lower edge, in pixels from the pen.
///   - high: The stroke's upper edge.
/// - Returns: The translation, within [-0.5, 0.5]; 0 for an empty or
///   non-finite stroke.
[[nodiscard]] inline double strokePixelOffset(double low, double high)
{
    if (!std::isfinite(low) || !std::isfinite(high) || high <= low)
        return 0;
    const double center = (low + high) / 2;
    const long pixels = std::max(1L, std::lround(high - low));
    const double target = pixels % 2 == 0 ? std::floor(center + 0.5) : std::floor(center) + 0.5;
    return target - center;
}

/// One translation applied to every box-drawing outline of a face.
struct BoxFamilyPixelOffset {
    double x = 0;
    double y = 0;
};

/// The translation that lands a face's box-drawing family on whole pixels.
///
/// Measured from the family's reference rules, the horizontal of U+2500 for
/// the vertical offset and the vertical of U+2502 for the horizontal one,
/// and applied to every piece of the family alike. Every piece then keeps the
/// relative geometry its designer drew, so its strokes meet the neighbouring
/// cells' strokes exactly, and a face whose reference rule is a whole number
/// of pixels wide draws every rule of the family without an antialiased
/// fringe, which is what hinting did for that one glyph alone.
///
/// - Parameters:
///   - horizontalLow: U+2500's stroke lower edge, from its outline.
///   - horizontalHigh: Its upper edge.
///   - verticalLow: U+2502's stroke left edge.
///   - verticalHigh: Its right edge.
/// - Returns: The translation; an axis whose reference is absent stays 0.
[[nodiscard]] inline BoxFamilyPixelOffset boxFamilyPixelOffset(double horizontalLow, double horizontalHigh,
    double verticalLow, double verticalHigh)
{
    return { strokePixelOffset(verticalLow, verticalHigh), strokePixelOffset(horizontalLow, horizontalHigh) };
}

/// The slot column a face raster's leftmost ink lands on.
///
/// A glyph that keeps its face placement has its advance centred in the slot,
/// which is where the cell box sits, and its ink at the face's own bearing from
/// that pen, so every piece of the family shares one centre column whatever
/// its ink. Every other glyph is centred on its own ink.
///
/// - Parameters:
///   - codePoint: The cell's code point.
///   - slotWidth: The slot's width in pixels.
///   - inkLeftFromPen: The ink's left edge relative to the pen, in pixels.
///   - inkWidth: The ink's width in pixels.
///   - advanceInPixels: The glyph's advance in pixels.
/// - Returns: The slot column of the raster's left edge.
[[nodiscard]] inline int rasterLeftInSlot(unsigned codePoint, int slotWidth, int inkLeftFromPen, int inkWidth, double advanceInPixels)
{
    if (keepsFacePlacement(codePoint) && std::isfinite(advanceInPixels) && advanceInPixels > 0)
        return static_cast<int>(std::floor((slotWidth - advanceInPixels) / 2)) + inkLeftFromPen;
    return (slotWidth - inkWidth) / 2;
}

/// Which cell edges a tiling piece's stroke must reach.
///
/// `isTilingPiece` is false for every code point that is not one of these.
struct TilingReach {
    bool isTilingPiece = false;
    bool reachesTop = false; // bool-ok: boxReachEdges names the same fact so
    bool reachesBottom = false; // bool-ok: the two read alike at a call site
    /// A brace's middle: reaches both edges like an extension, and is a fixed
    /// design element like a hook.
    ///
    /// TeX's extensible recipe has four slots -- top, mid, bottom and rep --
    /// and only `rep` repeats. A curly bracket's point is `mid`: stretching it
    /// distorts the shape, and pinning it to an edge the way a hook is pinned
    /// moves it off the centre it is drawn about. It keeps its proportions and
    /// sits centred, and the extensions on either side take up the length.
    bool isMiddle = false; // bool-ok: reads as an adjective at the call site
};

/// The edges a piece continues into, for the 9-slice that extends its stroke.
///
/// Drawing the piece where the face put it lines the stems up; it does not make
/// them TOUCH. A face draws these with side bearings like any other glyph, so a
/// stem stops short of the row below by whatever the bearing is, and a tall
/// bracket reads as a dotted line of correctly-aligned segments. The same
/// 9-slice the box-drawing family uses replicates the stroke out to the edge it
/// continues into, without moving it off the column the face chose.
///
/// An upper hook reaches downward only, a lower hook upward only, and an
/// extension or a middle both ways. Stretching a hook toward the edge it does
/// not continue into would draw a stem where the bracket has already curved
/// away.
///
/// - Parameter codePoint: The cell's code point.
/// - Returns: The reach; `isTilingPiece` is false when it is not a piece.
[[nodiscard]] constexpr TilingReach tilingReach(unsigned codePoint)
{
    switch (codePoint) {
    // Extensions and middles: the stroke runs the whole cell.
    case 0x23AE: // ⎮ integral extension
    case 0x239C: // ⎜ left parenthesis extension
    case 0x239F: // ⎟ right parenthesis extension
    case 0x23A2: // ⎢ left square bracket extension
    case 0x23A5: // ⎥ right square bracket extension
    case 0x23AA: // ⎪ curly bracket extension
    case 0x23B8: // ⎸ left vertical box line
    case 0x23B9: // ⎹ right vertical box line
    case 0x23D0: // ⏐ vertical line extension
        return { true, true, true, false };
    // The braces' middles: both edges, and not stretched.
    case 0x23A8: // ⎨ left curly bracket middle
    case 0x23AC: // ⎬ right curly bracket middle
        return { true, true, true, true };
    // Upper hooks: the stroke continues into the row BELOW.
    case 0x2320: // ⌠ top half integral
    case 0x239B: // ⎛ left parenthesis upper hook
    case 0x239E: // ⎞ right parenthesis upper hook
    case 0x23A1: // ⎡ left square bracket upper corner
    case 0x23A4: // ⎤ right square bracket upper corner
    case 0x23A7: // ⎧ left curly bracket upper hook
    case 0x23AB: // ⎫ right curly bracket upper hook
    case 0x2E59: // ⹙ top half left parenthesis
    case 0x2E5A: // ⹚ top half right parenthesis
    case 0x23B0: // ⎰ upper-left / lower-right section
    case 0x23B2: // ⎲ summation top
        return { true, false, true, false };
    // Lower hooks: the stroke continues into the row ABOVE.
    case 0x2321: // ⌡ bottom half integral
    case 0x239D: // ⎝ left parenthesis lower hook
    case 0x23A0: // ⎠ right parenthesis lower hook
    case 0x23A3: // ⎣ left square bracket lower corner
    case 0x23A6: // ⎦ right square bracket lower corner
    case 0x23A9: // ⎩ left curly bracket lower hook
    case 0x23AD: // ⎭ right curly bracket lower hook
    case 0x2E5B: // ⹛ bottom half left parenthesis
    case 0x2E5C: // ⹜ bottom half right parenthesis
    case 0x23B1: // ⎱ upper-right / lower-left section
    case 0x23B3: // ⎳ summation bottom
    case 0x23B7: // ⎷ radical symbol bottom
        return { true, true, false, false };
    default:
        return {};
    }
}

/// The piece an extension's stem is always borrowed from.
///
/// Always used, not only when a face ships no extension of its own. An extension is
/// a straight stroke and nothing else, so it can be rebuilt from any piece of
/// the same bracket containing that stroke -- a hook's foot is the stem the
/// extension continues. Built that way it cannot disagree with its neighbours
/// about weight or column, because it is their ink; taken from the face's own
/// extension glyph it can disagree about both, and does:
///
///     Hiragino Mincho   draws the curly extension lighter than its hooks, so
///                       a tall brace thins across its middle rows
///     Hiragino Sans     puts the extension's stem at x 6.52..7.48 and the
///                       hooks' at x 7.64..13.47 -- no overlap at all
///     CommitMono        ships no integral extension
///     every face        including Menlo, ships no U+23D0
///
/// One rule answers all four, and the last two stop being special cases: a
/// missing extension draws on the same donor the others use.
///
/// - Parameter codePoint: An extension's code point.
/// - Returns: The donor's code point, or 0 when there is no safe donor.
[[nodiscard]] constexpr unsigned stemDonor(unsigned codePoint)
{
    switch (codePoint) {
    case 0x239C:
        return 0x239B; // ⎜ from ⎛'s foot
    case 0x239F:
        return 0x239E; // ⎟ from ⎞'s foot
    case 0x23A2:
        return 0x23A1; // ⎢ from ⎡'s foot
    case 0x23A5:
        return 0x23A4; // ⎥ from ⎤'s foot
    case 0x23AE:
        return 0x2320; // ⎮ from ⌠'s foot
    case 0x23D0:
        return 0x2502; // ⏐ from the box family's own vertical, which
                       // is exactly the stroke this character means
    default:
        return 0;
    }
}

/// Whether a piece is only ever MOVED, never reshaped.
///
/// A bracket built from hooks and extensions can have its extension stretched,
/// because an extension is a straight stroke whose only content is length. A
/// two-piece assembly has no such piece: the summation's halves, the S-shaped
/// brace sections, the half parentheses and the radical's foot are each the
/// whole of their own shape, and the assembly is finished by moving them until
/// they touch. Scaling one distorts the character -- a brace section loses the
/// point that is the reason it exists -- and closing a seam by repeating rows
/// would draw ink the face did not.
///
/// So these are placed and nothing else: the edge a piece continues into lands
/// on the cell boundary, at the size the face drew it.
///
/// - Parameter codePoint: The cell's code point.
/// - Returns: True when the piece may be moved but not reshaped.
[[nodiscard]] constexpr bool isMovedNotReshaped(unsigned codePoint)
{
    switch (codePoint) {
    case 0x23B2: // ⎲ summation top
    case 0x23B3: // ⎳ summation bottom
    case 0x23B0: // ⎰ brace section
    case 0x23B1: // ⎱ brace section
    case 0x23B7: // ⎷ radical symbol bottom
    case 0x2E59: // ⹙ half parentheses
    case 0x2E5A:
    case 0x2E5B:
    case 0x2E5C:
        return true;
    default:
        return false;
    }
}

/// A baseline and vertical scale in the font's upward-positive coordinates.
struct TilingPlacement {
    bool isApplicable = false;
    double baseline = 0;
    double verticalScale = 1;
};

/// Fits measured ink to its joining edges without widening its stroke.
///
/// Bounds use upward-positive coordinates. The returned baseline precedes
/// scaling: a font position y lands at `(y + baseline) * verticalScale`.
[[nodiscard]] inline TilingPlacement fitTilingGlyph(unsigned codePoint, double inkBottom, double inkHeight,
    double boxBottom, double boxHeight)
{
    if (!tilesOnTextPen(codePoint) || !std::isfinite(inkBottom) || !std::isfinite(inkHeight)
        || !std::isfinite(boxBottom) || !std::isfinite(boxHeight) || inkHeight <= 0.5 || boxHeight <= 0.5) {
        return {};
    }
    const auto reach = tilingReach(codePoint);
    const bool isExtender = reach.reachesTop && reach.reachesBottom && !reach.isMiddle;
    const double scale = isMovedNotReshaped(codePoint) ? 1.0
        : isExtender                                   ? boxHeight / inkHeight
                                                       : std::min(1.0, boxHeight / inkHeight);
    double baseline;
    if (reach.isMiddle) {
        baseline = (boxBottom + (boxHeight - inkHeight * scale) / 2) / scale - inkBottom;
    } else if (reach.reachesBottom) {
        baseline = boxBottom / scale - inkBottom;
    } else {
        baseline = (boxBottom + boxHeight) / scale - inkBottom - inkHeight;
    }
    return { true, baseline, scale };
}

using TilingCoverageSpan = std::span<std::uint8_t>;

/// Joins rasterized pieces inside a cell, excluding atlas margins.
///
/// Coverage rows run downward. Font measurement and outline drawing remain
/// with the rasterizer; seam coverage and donor reconstruction have one owner.
class TilingCoverage {
public:
    TilingCoverage(TilingCoverageSpan coverage, int width, int height, int left, int right, int top, int bottom)
        : m_coverage(coverage)
        , m_width(std::max(0, width))
        , m_height(std::max(0, height))
        , m_left(std::clamp(left, 0, m_width))
        , m_right(std::clamp(right, m_left, m_width))
        , m_top(std::clamp(top, 0, m_height))
        , m_bottom(std::clamp(bottom, m_top, m_height))
    {
        if (coverage.size() < static_cast<std::size_t>(m_width) * m_height) {
            m_width = m_height = m_left = m_right = m_top = m_bottom = 0;
        }
    }

    /// Clears overlapping margins and extends solid coverage to joining edges.
    ///
    /// - Parameter codePoint: The assembly piece whose connecting edges apply.
    void stretch(unsigned codePoint) const
    {
        const auto reach = tilingReach(codePoint);
        if (!reach.isTilingPiece || m_top == m_bottom || m_left == m_right) {
            return;
        }
        for (int row = 0; row < m_height; ++row) {
            if ((reach.reachesTop && row < m_top) || (reach.reachesBottom && row >= m_bottom)) {
                std::fill_n(m_coverage.data() + static_cast<std::size_t>(row) * m_width + m_left, m_right - m_left, 0);
            }
        }
        if (isMovedNotReshaped(codePoint) || m_bottom - m_top < 3) {
            return;
        }
        if (reach.reachesTop) {
            extendEdge(Edge::Top);
        }
        if (reach.reachesBottom) {
            extendEdge(Edge::Bottom);
        }
    }

    /// Repeats a donor row across the cell, including its horizontal margins.
    ///
    /// - Parameter source: The donor row, or an out-of-cell value to select bottom-edge ink.
    /// - Returns: Whether a usable donor row fills the cell.
    bool fillFromStemRow(int source = -1) const
    {
        if (m_top == m_bottom || m_left == m_right) {
            return false;
        }
        if (source < m_top || source >= m_bottom) {
            source = strongerEdgeRow(Edge::Bottom);
        }
        if (source < 0) {
            return false;
        }
        for (int row = m_top; row < m_bottom; ++row) {
            if (row != source) {
                std::copy_n(m_coverage.data() + static_cast<std::size_t>(source) * m_width, m_width,
                    m_coverage.data() + static_cast<std::size_t>(row) * m_width);
            }
        }
        return true;
    }

    /// Finishes the measured glyph and reconstructs an extension from its donor.
    ///
    /// - Parameters:
    ///   - codePoint: The requested assembly piece.
    ///   - drawnCodePoint: The glyph rasterized into coverage, possibly a stem donor.
    void finish(unsigned codePoint, unsigned drawnCodePoint) const
    {
        if (drawnCodePoint != codePoint) {
            stretch(drawnCodePoint);
            const auto reach = tilingReach(drawnCodePoint);
            const int source = reach.isTilingPiece ? (reach.reachesBottom ? m_bottom - 1 : m_top) : -1;
            fillFromStemRow(source);
        }
        stretch(codePoint);
    }

private:
    /// Identifies the cell boundary that joins the neighboring assembly piece.
    enum class Edge {
        Top,
        Bottom
    };

    /// Detects coverage strong enough to serve as a joining stem.
    ///
    /// - Parameter row: A valid coverage row.
    /// - Returns: Whether any in-cell sample is at least half opaque.
    bool rowHasInk(int row) const
    {
        for (int column = m_left; column < m_right; ++column) {
            if (m_coverage[static_cast<std::size_t>(row) * m_width + column] >= 128) {
                return true;
            }
        }
        return false;
    }

    /// Measures total in-cell coverage for comparing adjacent stem rows.
    ///
    /// - Parameter row: A valid coverage row.
    /// - Returns: The sum of the row's in-cell coverage samples.
    std::uint64_t rowMass(int row) const
    {
        std::uint64_t mass = 0;
        for (int column = m_left; column < m_right; ++column) {
            mass += m_coverage[static_cast<std::size_t>(row) * m_width + column];
        }
        return mass;
    }

    /// Selects solid edge ink while skipping a weaker antialiasing fringe.
    ///
    /// - Parameter edge: The edge from which to search inward.
    /// - Returns: The donor row, or -1 when the cell has no usable ink.
    int strongerEdgeRow(Edge edge) const
    {
        const int step = edge == Edge::Top ? 1 : -1;
        for (int row = edge == Edge::Top ? m_top : m_bottom - 1; row >= m_top && row < m_bottom; row += step) {
            if (!rowHasInk(row)) {
                continue;
            }
            const int next = row + step;
            if (next >= m_top && next < m_bottom && rowHasInk(next) && rowMass(next) > rowMass(row)) {
                return next;
            }
            return row;
        }
        return -1;
    }

    /// Carries solid stem coverage through the fringe to the joining edge.
    ///
    /// - Parameter edge: The joining edge of the cell.
    void extendEdge(Edge edge) const
    {
        const int source = strongerEdgeRow(edge);
        if (source < 0) {
            return;
        }
        const int step = edge == Edge::Top ? 1 : -1;
        for (int row = edge == Edge::Top ? m_top : m_bottom - 1; row != source; row += step) {
            std::copy_n(m_coverage.data() + static_cast<std::size_t>(source) * m_width + m_left, m_right - m_left,
                m_coverage.data() + static_cast<std::size_t>(row) * m_width + m_left);
        }
    }

    TilingCoverageSpan m_coverage;
    int m_width;
    int m_height;
    int m_left;
    int m_right;
    int m_top;
    int m_bottom;
};

} // namespace Ubiquitty::Rendering
