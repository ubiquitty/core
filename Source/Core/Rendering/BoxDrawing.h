// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/ShadeStyle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// BoxDrawing.h — portable codePoint → fill-primitive decomposition for
// box-drawing, block-element, and legacy-computing glyphs, so a frontend draws
// them with cell-exact geometry instead of font glyphs (font glyphs leave seams:
// they neither reach the cell edge nor pixel-align, so neighbours don't tile).
//
// No platform types: pure C++ in namespace Ubiquitty::BoxDrawing, host-testable.
// The decomposition is in UNIT cell coordinates — [0,1]×[0,1], origin top-left,
// x right, y down — so the caller scales to its device-pixel cell rect and snaps
// edges. Stubs/blocks always reach exactly to a cell edge (coord 0 or 1) so
// adjacent cells abut with no gap.

namespace Ubiquitty::Rendering::BoxDrawing {

// Line weights for stubs / diagonals.
enum class Weight : unsigned char {
    Light = 0,
    Heavy = 1,
    Double = 2,
};

// Stub direction from the cell centre to an edge.
enum class Direction : unsigned char {
    Up = 0,
    Down = 1,
    Left = 2,
    Right = 3,
};

enum class PrimitiveKind : unsigned char {
    Rect = 0, // filled unit rect [x0,y0,x1,y1] with alpha
    Stub = 1, // centre → edge line, weight + direction
    Diagonal = 2, // corner-to-corner line, weight
    Arc = 3, // quarter-circle stroke, weight + bulge quadrant (direction)
    Triangle = 4, // filled unit triangle (x0,y0)(x1,y1)(x2,y2), alpha
    Stipple = 5, // ordered-dither fill of [x0,y0,x1,y1] at density `alpha`
                 // (fraction of foreground pixels); a checkerboard, not a tint.
    EdgeLine = 6, // weight-thick stroke at an arbitrary centreline, for the
                  // terminal-graphics family whose lines sit on cell edges
                  // where no Stub can reach. direction picks the axis (Up =
                  // vertical: x0 is the unit centreline, spanning y0..y1;
                  // Right = horizontal: y0 is the centreline, spanning
                  // x0..x1). The stroke band shifts inside the cell when the
                  // centreline sits on an edge, so an edge line is flush
                  // rather than half-clipped.
};

struct Primitive {
    PrimitiveKind kind;
    Weight weight; // Stub / Diagonal / Arc
    Direction direction; // Stub direction; Arc bulge quadrant
    // Rect: unit corners + alpha. Diagonal: (x0,y0)→(x1,y1) unit corners.
    // Arc: (x0,y0) = centre, x1 = radius (unit-x), y1 unused. Triangle:
    // three unit corners (x0,y0)(x1,y1)(x2,y2). Stipple: fill region.
    float x0, y0, x1, y1;
    float x2, y2; // Triangle third vertex; 0 elsewhere
    float alpha; // Rect translucency / Stipple density; 1.0 elsewhere
    unsigned char dashes; // Stub: 0 solid, else dash count (2/3/4); 0 elsewhere
    // Stub: distance from the cell centre (unit coords) at which the
    // straight stroke STOPS, leaving the rest to a joining Arc. 0 = the
    // stub reaches the centre (the default for non-rounded joins).
    float inset;
    // Stipple: Inverse lights the complement of the field at this density —
    // the legacy "inverse medium shade" family, the pixels ▒ leaves dark.
    ShadePolarity polarity;
};

// A decoded glyph: up to maxPrimitives primitives. count == 0 means
// the codePoint has no geometric form (the caller falls back to the font).
constexpr int maxPrimitives = 12;

struct Glyph {
    Primitive prim[maxPrimitives];
    int count;
};

// Analytic Powerline-separator shape for the renderer's edge-antialiased
// single-quad path: a separator whose silhouette is one analytic shape (a
// triangle, a 2-segment chevron, or a half-disc) resolves to one PowerlineShape,
// and the fragment computes feathered (~1 device-px) coverage from its SDF. All
// coordinates are UNIT cell coords ([0,1]×[0,1], origin top-left, x right, y
// down), scaled to the device cell rect by the caller. Kinds:
//
// - SolidTriangle:   corners (a,b,c) = parameters {ax,ay, bx,by, cx,cy}; coverage
//   is the intersection of the three edge half-planes (filled).
// - OutlineTriangle: a round-capped capsule pair a→b→c (segment distance minus
//   strokeHalf, p[6]); the joiner for stepped diagonal RUNS, whose caps spill
//   past the endpoints so consecutive segments meet without a notch. Not a
//   separator shape: a stroke centred on its polyline is half-clipped wherever
//   the polyline touches the cell edge.
// - SolidHalfDisc:   centre (cx,cy)=p[0..1], radii (rx,ry)=p[2..3] (unit per
//   axis), and sideSign=p[4] (+1 keeps the x≥cx half-plane, −1 keeps x≤cx);
//   coverage is inside the ellipse and on the kept side (a bulging half-disc
//   with its flat diameter on a cell edge).
// - OutlineHalfDisc: same parameters + strokeHalf=p[5]; coverage is the band
//   from the arc INWARD by the full stroke, so the arc itself is the outer
//   boundary — the bundled face's construction, whose stroke never crosses
//   the silhouette it outlines — kept to the sideSign half.
// - OutlineWedge:    the thin chevron separators. Same corners as the solid
//   triangle they thin — {ax,ay, bx,by, cx,cy}, b the apex — plus strokeHalf
//   (p[6]); coverage is the band from the two apex edges INWARD by the full
//   stroke. The edges pass through the cell corners and apex, so the quad's
//   own clip finishes the stroke ends as points at the corners instead of
//   chopping a centred stroke mid-width.
// - OutlineDiagonal: the thin corner wedges — one stroke along a→b
//   ({ax,ay, bx,by}) banded INWARD toward the witness point (wx,wy)=p[4..5],
//   which names the solid sibling's filled side; strokeHalf in p[6]. The line
//   itself is the outer boundary, corner to corner, as the face draws it.
enum class PowerlineShapeKind : unsigned char {
    None = 0,
    SolidTriangle = 1,
    OutlineTriangle = 2,
    SolidHalfDisc = 3,
    OutlineHalfDisc = 4,
    OutlineWedge = 5,
    OutlineDiagonal = 6,
    EllipticalArc = 7, // center, radii in p[0..3]; half-widths in p[4..5]; quadrant directions in p[6..7]
};

struct PowerlineShape {
    PowerlineShapeKind kind;
    float p[8]; // meaning per kind (see above); unused slots are 0
};

// Map a simple Powerline separator to its analytic single-quad shape. Returns
// true (and fills `out`) for U+E0B0–U+E0BF: the arrows, half-discs, and corner
// wedges. Returns false (out->kind set to None) for icons and Powerline Extra
// artwork whose font silhouette must not be replaced by an approximation.
bool powerlineShape(unsigned codePoint, PowerlineShape* out);

/// Resolves a cell's code point to the analytic single-quad shape the builder draws.
///
/// The Geometric Shapes corner triangles U+25E2–U+25E5 are the half-cells the
/// solid Powerline corner wedges draw, so each resolves to its wedge's shape
/// and its diagonal is feathered rather than cut on the pixel grid. Every
/// other code point resolves as `powerlineShape` resolves it.
///
/// - Parameters:
///   - codePoint: The cell's code point.
///   - out: The shape to fill; its kind is `None` when there is none.
/// - Returns: Whether the code point has an analytic shape.
bool analyticCellShape(unsigned codePoint, PowerlineShape* out);

/// Reports whether a code point is an assigned Powerline glyph.
///
/// Membership follows the original Powerline and Powerline Extra private-use
/// sets as carried by the bundled Symbols Nerd Font: the holes inside
/// U+E0A0–U+E0D7 are unassigned upstream rather than missing resources.
///
/// - Parameter codePoint: The code point to classify.
/// - Returns: Whether the bundled symbol face assigns it artwork.
bool isPowerlineGlyph(unsigned codePoint);

/// The lowest code point ``covers`` can claim: the first terminal-graphics
/// vertical line. Every code point below it is text, so a caller asking per
/// cell can rule it out with one compare before making the call.
inline constexpr unsigned lowestCoveredCodePoint = 0x23B8;

// True for any codePoint decompose() can render geometrically.
// Cheap range test; the per-codePoint detail lives in the decomposition.
bool covers(unsigned codePoint);

// True for the dashed line glyphs (triple / quadruple U+2504–250B, double
// U+254C–254F). These render from the FONT glyph, not box-processed: a per-cell
// procedural or edge-stretched dash staggers under fractional cell advance.
// Excluded from covers() so the renderer routes them to the glyph atlas.
bool isDash(unsigned codePoint);

// True for codePoints that should always use procedural geometry even when the
// font ships a glyph: solid block elements, shades, quadrants (U+2580–259F),
// sextants (U+1FB00–1FB3B), and analytic Powerline separators. These shapes
// must land on the snapped cell edges; a font raster carries bearings and a
// phase-zero bitmap that a fractional cell quad resamples. Box-drawing LINES
// (U+2500–257F) are not included: there the font's weight/dashes matter, so
// procedural is only the fallback when the font ships no glyph.
bool prefersProcedural(unsigned codePoint);

// Fill `out` with the unit-coordinate primitives for `codePoint`. Returns
// the primitive count (0 when not covered — `out` is then left with count 0).
int decompose(unsigned codePoint, Glyph* out);

/// A per-codePoint memo over ``decompose``.
///
/// ``decompose`` is a pure function of `codePoint` — the geometry is in unit
/// cell coordinates, with no font, cell-position or neighbour dependence — so a
/// cached ``Glyph`` is byte-identical to a fresh call and never needs
/// invalidation. Every backend that draws box glyphs procedurally wants this
/// memo, and the purity that makes it safe is a property of ``decompose``
/// rather than of any one of them.
///
/// Indexed by family page and low byte rather than hashed. The covered code
/// points come in 256-wide families, and a code point's low byte already tells
/// it apart from every other member of its family; what a hash of the whole
/// value adds is collisions between members of one family. Each family drawn
/// together gets its own page, so box drawing, block elements, braille, the
/// legacy-computing sextants, the octants and the Powerline separators never
/// evict one another. The rest — the terminal-graphics scan lines among them —
/// share the last page, where a collision recomputes into the slot: it costs a
/// `decompose` and cannot return a wrong glyph, because the stored code point
/// is compared before the value is used.
///
/// The glyphs live in a vector that grows by the distinct code points drawn,
/// so an instance costs its 3 KiB index until it is used.
class GlyphGeometryCache {
public:
    /// - Parameters:
    ///   - codePoint: The box or block codePoint to decompose.
    ///   - out: Receives the unit-coordinate geometry.
    /// - Returns: The primitive count, 0 when the codePoint has no geometric form.
    int decomposeCached(unsigned codePoint, Glyph* out)
    {
        std::uint16_t& position = m_index[slotFor(codePoint)];
        if (position != 0 && m_entries[position - 1u].codePoint == codePoint) {
            *out = m_entries[position - 1u].glyph;
            return out->count;
        }
        decompose(codePoint, out);
        ++m_decompositions;
        if (position == 0) {
            m_entries.push_back({ codePoint, *out });
            position = static_cast<std::uint16_t>(m_entries.size());
        } else {
            m_entries[position - 1u] = { codePoint, *out };
        }
        return out->count;
    }

    /// How many lookups had to decompose: one per distinct code point, plus
    /// one per eviction on the shared page.
    [[nodiscard]] std::size_t decompositions() const { return m_decompositions; }

private:
    static constexpr unsigned pageCount = 6;

    /// Names the family page a code point's block indexes into.
    ///
    /// - Parameter block: The code point shifted right by eight.
    /// - Returns: The family's own page, or the last page, which the rest share.
    static constexpr unsigned pageFor(unsigned block)
    {
        switch (block) {
        case 0x25: // box drawing, block elements, the corner triangles
            return 0;
        case 0x28: // braille
            return 1;
        case 0x1FB: // legacy computing: sextants, wedges, eighths
            return 2;
        case 0x1CD: // octants
            return 3;
        case 0xE0: // Powerline separators
            return 4;
        default:
            return pageCount - 1;
        }
    }

    /// Names the index slot a code point owns: its family page and low byte.
    ///
    /// - Parameter codePoint: The code point to place.
    /// - Returns: An index into `m_index`.
    static constexpr unsigned slotFor(unsigned codePoint)
    {
        return pageFor(codePoint >> 8) * 256u + (codePoint & 0xFFu);
    }

    /// One held glyph and the code point it answers for, compared before the
    /// glyph is used.
    struct Entry {
        unsigned codePoint;
        Glyph glyph;
    };

    /// One-based positions into `m_entries`, zero where empty.
    std::array<std::uint16_t, pageCount * 256> m_index {};
    std::vector<Entry> m_entries;
    std::size_t m_decompositions = 0;
};

// Integer row shift that centres a box-drawing font glyph's ink box vertically
// in its cell box. A font box glyph is baseline-placed, so its connection
// midline sits off the cell centre; shifting aligns the ink-box centre with the
// cell-box centre before edge-stretch, so the stretch reaches every edge
// symmetrically. Inputs are pixel rows: [inkMinY,inkMaxY] inclusive ink extent,
// [cellTop,cellBottom) the cell box. Rounds to a whole row; returns 0 when no
// shift is needed or no ink is present (inkMaxY < inkMinY).
int centerShiftY(int inkMinY, int inkMaxY, int cellTop, int cellBottom);

/// Whether the family's centring shift may move a glyph's raster.
///
/// The shift is one number for the whole family, measured from U+2500, and it
/// exists so a stroke drawn from the face lands on the row the procedural
/// stroke would take. That is the right answer for a character whose meaning is
/// a shape, and the wrong one for a character whose meaning is a POSITION: the
/// terminal-graphics family U+23B8..U+23BD names the scan row or the cell edge
/// it stands on — scans 1, 3, 7 and 9 of the VT100 cell, and the left and right
/// verticals — so centring replaces the character with a measurement of the
/// reference's ink. U+23BA is scan 1 or it is nothing; moved to the row U+2500
/// occupies it is U+2500.
///
/// The exempt glyphs still belong to the family for every other purpose. They
/// decompose, so a face without them draws Core's geometry rather than a
/// hairline from whatever the cascade substitutes, and they edge-stretch along
/// their own axis so a run of them meets with no gap.
///
/// - Parameter codePoint: The box-drawing character about to be rasterised.
/// - Returns: True when the glyph takes the family's shift.
[[nodiscard]] bool takesFamilyCentring(unsigned codePoint);

// A grid-of-subcells fill pattern for the quadrant (U+2596–259F) and sextant
// (U+1FB00–1FB3B) block glyphs: `mask` is a bitfield over a `columns`×`rows`
// grid in reading order (bit 0 = top-left, then left-to-right, top-to-bottom).
// A renderer drawing these as a single mask-driven quad reads the pattern here
// so every frontend shares one bit-layout table. Quadrants are 2×2; sextants
// 2×3, decoded with the same skip-the-four-aliased-patterns rule
// decomposeSextant uses (empty / left half / right half / full).
struct MosaicMask {
    unsigned mask;
    int columns;
    int rows;
};

// Resolve `codePoint` to its MosaicMask. Returns true and fills `out` for the
// quadrant + sextant block glyphs; returns false (out left zeroed) for every
// other codePoint — the caller then falls back to decompose / the font.
bool mosaicMaskForCodePoint(unsigned codePoint, MosaicMask* out);

} // namespace Ubiquitty::Rendering::BoxDrawing
