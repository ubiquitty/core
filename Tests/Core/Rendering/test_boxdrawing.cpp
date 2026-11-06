// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Box-drawing geometry: code point to unit-coordinate fill primitives. Pure
// table-driven decomposition, free of platform types.

#include "Harness.h"

#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxEdgeStretch.h"
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>
#include <utility>

using namespace Ubiquitty::Rendering::BoxDrawing;

static bool approx(float a, float b) { return fabsf(a - b) < 1e-5f; }

/// Whether a unit-space point is (x, y), to the float tolerance.
static bool isAt(float pointX, float pointY, float x, float y) { return approx(pointX, x) && approx(pointY, y); }

/// The primitives a decomposition filled in, in order.
static std::span<const Primitive> primitivesOf(const Glyph& glyph)
{
    return { glyph.prim, static_cast<std::size_t>(glyph.count) };
}

/// How many stubs a glyph sends toward each edge of its cell.
struct StubReach {
    int up = 0;
    int down = 0;
    int left = 0;
    int right = 0;
};

static StubReach stubReach(const Glyph& glyph)
{
    StubReach reach;
    for (const Primitive& primitive : primitivesOf(glyph)) {
        if (primitive.kind != PrimitiveKind::Stub)
            continue;
        switch (primitive.direction) {
        case Direction::Up:
            ++reach.up;
            break;
        case Direction::Down:
            ++reach.down;
            break;
        case Direction::Left:
            ++reach.left;
            break;
        case Direction::Right:
            ++reach.right;
            break;
        }
    }
    return reach;
}

/// Whether a primitive's horizontal extent runs from `left` to `right`.
static bool spansColumns(const Primitive& primitive, float left, float right)
{
    return approx(primitive.x0, left) && approx(primitive.x1, right);
}

/// Whether a primitive's vertical extent runs from `top` to `bottom`.
static bool spansRows(const Primitive& primitive, float top, float bottom)
{
    return approx(primitive.y0, top) && approx(primitive.y1, bottom);
}

/// A rectangle in unit cell coordinates.
struct UnitBox {
    float left;
    float top;
    float right;
    float bottom;
};

constexpr UnitBox wholeCell { 0, 0, 1, 1 };

/// Whether a primitive covers exactly the box.
static bool occupies(const Primitive& primitive, UnitBox box)
{
    return spansColumns(primitive, box.left, box.right) && spansRows(primitive, box.top, box.bottom);
}

/// Full block U+2588 is one solid rect covering the whole cell.
TEST(testFullBlock)
{
    Glyph g;
    int n = decompose(0x2588, &g);
    const Primitive& solid = g.prim[0];
    CHECK_EQ(n, 1);
    CHECK(solid.kind == PrimitiveKind::Rect);
    CHECK_MSG(occupies(solid, wholeCell), "U+2588 spans the unit cell");
    CHECK_MSG(approx(solid.alpha, 1.0f), "and is fully opaque");
}

/// Left half block U+258C covers the left 50%.
TEST(testLeftHalf)
{
    Glyph g;
    int n = decompose(0x258C, &g);
    CHECK_EQ(n, 1);
    const Primitive& p = g.prim[0];
    CHECK(p.kind == PrimitiveKind::Rect);
    CHECK_MSG(occupies(p, { 0, 0, 0.5f, 1 }), "U+258C fills the left half of the cell");
}

/// Right half block U+2590 covers the right 50%.
TEST(testRightHalf)
{
    Glyph g;
    decompose(0x2590, &g);
    CHECK_EQ(g.count, 1);
    const Primitive& p = g.prim[0];
    CHECK_MSG(spansColumns(p, 0.5f, 1), "U+2590 runs from the midline to the right edge");
}

/// Lower one-eighth block U+2581 covers the bottom 1/8 of the cell.
TEST(testLowerEighth)
{
    Glyph g;
    decompose(0x2581, &g);
    CHECK_EQ(g.count, 1);
    const Primitive& p = g.prim[0];
    CHECK_MSG(spansColumns(p, 0, 1), "U+2581 spans the cell's full width");
    CHECK_MSG(spansRows(p, 7.0f / 8, 1), "and only its bottom eighth");
}

/// Light cross U+253C emits four light stubs, one to each edge.
TEST(testLightCross)
{
    Glyph g;
    decompose(0x253C, &g);
    CHECK_EQ(g.count, 4);
    const StubReach reach = stubReach(g);
    CHECK_EQ(reach.up, 1);
    CHECK_EQ(reach.down, 1);
    CHECK_EQ(reach.left, 1);
    CHECK_EQ(reach.right, 1);
    for (const Primitive& arm : primitivesOf(g))
        CHECK_MSG(arm.weight == Weight::Light, "every arm of ┼ is light");
}

/// Light horizontal U+2500 is two horizontal stubs (left + right), no
/// vertical stubs — so it reaches both cell edges and tiles.
TEST(testLightHorizontal)
{
    Glyph g;
    decompose(0x2500, &g);
    CHECK_EQ(g.count, 2);
    const StubReach reach = stubReach(g);
    CHECK_EQ(reach.left, 1);
    CHECK_EQ(reach.right, 1);
    CHECK_EQ(reach.up, 0);
    CHECK_EQ(reach.down, 0);
}

/// Light down+right corner U+250C joins down + right edges only, so a run
/// of │ above and ─ to the right meet it with no gap.
TEST(testCorner)
{
    Glyph g;
    decompose(0x250C, &g);
    CHECK_EQ(g.count, 2);
    const StubReach reach = stubReach(g);
    CHECK_EQ(reach.down, 1);
    CHECK_EQ(reach.right, 1);
    CHECK_EQ(reach.up, 0);
    CHECK_EQ(reach.left, 0);
}

/// Double vertical+right tee U+2560 carries double weight on all three.
TEST(testDoubleWeight)
{
    Glyph g;
    decompose(0x2560, &g);
    CHECK_EQ(g.count, 3);
    for (const Primitive& arm : primitivesOf(g))
        CHECK_MSG(arm.weight == Weight::Double, "every arm of ╠ is double");
}

/// Shades U+2591/2/3 are full-cell ORDERED-DITHER stipples at 25 / 50 / 75%
/// density — a checkerboard of solid foreground over the cell background, not a flat tint.
TEST(testShades)
{
    Glyph g;
    const Primitive& field = g.prim[0];
    decompose(0x2591, &g);
    CHECK_MSG(g.count == 1 && field.kind == PrimitiveKind::Stipple, "U+2591 is a single dither field");
    CHECK_MSG(approx(field.alpha, 0.25f), "that lights a quarter of its pixels");
    decompose(0x2592, &g);
    CHECK_MSG(field.kind == PrimitiveKind::Stipple && approx(field.alpha, 0.5f), "U+2592 dithers half its pixels");
    decompose(0x2593, &g);
    CHECK_MSG(field.kind == PrimitiveKind::Stipple && approx(field.alpha, 0.75f), "U+2593 dithers three quarters");
    CHECK_MSG(occupies(field, wholeCell), "and the field is the whole cell");
}

/// Lower-right quadrant U+2597 covers the bottom-right quarter.
TEST(testQuadrant)
{
    Glyph g;
    decompose(0x2597, &g);
    CHECK_EQ(g.count, 1);
    const Primitive& p = g.prim[0];
    CHECK_MSG(occupies(p, { 0.5f, 0.5f, 1, 1 }), "U+2597 fills the lower-right quarter");
}

/// Sextant-1 U+1FB00 fills only the top-left of the 2×3 grid; sextant
/// U+1FB3B (23456) fills five of six cells. The skipped left-half (21)
/// and right-half (42) patterns are not produced by any sextant codePoint.
TEST(testSextant)
{
    Glyph g;
    decompose(0x1FB00, &g);
    CHECK_EQ(g.count, 1);
    const Primitive& p = g.prim[0];
    CHECK_MSG(occupies(p, { 0, 0, 0.5f, 1.0f / 3 }), "U+1FB00 fills the top-left cell of the 2x3 grid");

    decompose(0x1FB3B, &g);
    CHECK_EQ(g.count, 5);
}

/// True iff the glyph contains a filled unit Rect for sub-cell (row,column) of a
/// 2-column × `rows`-row grid (the octant / sextant sub-cell convention: bit
/// index = row*2 + column, top-left = LSB).
static bool hasSubcell(const Glyph& g, int row, int column, int rows)
{
    const float xs[3] = { 0.0f, 0.5f, 1.0f };
    const float x0 = xs[column], x1 = xs[column + 1];
    const float y0 = static_cast<float>(row) / rows;
    const float y1 = static_cast<float>(row + 1) / rows;
    for (int i = 0; i < g.count; ++i) {
        const Primitive& p = g.prim[i];
        if (p.kind == PrimitiveKind::Rect && approx(p.x0, x0) && approx(p.x1, x1) && approx(p.y0, y0) && approx(p.y1, y1))
            return true;
    }
    return false;
}

/// Octants U+1CD00..1CDE5 (2 columns × 4 rows). Unicode assigns the 230
/// codePoints to the 256 fill patterns in ascending order, skipping the 26 that
/// already exist as other glyphs. The naming is reading-order positions 1..8
/// (1 = top-left, bit 0). Pinned against the Unicode 16.0 chart:
///   U+1CD00 = OCTANT-3   (sub-cell row1,col0 only)              pattern 0x04
///   U+1CD01 = OCTANT-23  (row0,col1 + row1,col0)                pattern 0x06
///   U+1CD02 = OCTANT-123 (row0,col0 + row0,col1 + row1,col0)    pattern 0x07
///   U+1CD10 = OCTANT-45  (row1,col1 + row2,col0)                pattern 0x18
///   U+1CDA0 = OCTANT-3568                                        pattern 0xB4
///   U+1CDE5 = OCTANT-2345678 (full minus top-left)              pattern 0xFE
TEST(testOctant)
{
    // Each pattern sets bit row * 2 + column for every lit sub-cell.
    const struct {
        unsigned codePoint;
        unsigned pattern;
        const char* meaning;
    } octants[] = {
        { 0x1CD00, 0x04, "U+1CD00, OCTANT-3, the first octant" },
        { 0x1CD01, 0x06, "U+1CD01, OCTANT-23" },
        { 0x1CD02, 0x07, "U+1CD02, OCTANT-123" },
        { 0x1CD10, 0x18, "U+1CD10, OCTANT-45, from the middle of the run" },
        { 0x1CDA0, 0xB4, "U+1CDA0, OCTANT-3568, from the middle of the run" },
        { 0x1CDE5, 0xFE, "U+1CDE5, OCTANT-2345678, the last octant" },
    };
    for (const auto& octant : octants) {
        Glyph g;
        decompose(octant.codePoint, &g);
        CHECK_MSG(g.count == std::popcount(octant.pattern), octant.meaning);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 2; ++column) {
                const bool isLit = ((octant.pattern >> (row * 2 + column)) & 1u) != 0;
                CHECK_MSG(hasSubcell(g, row, column, 4) == isLit, octant.meaning);
            }
        }
    }

    // A single lit sub-cell is the rect for exactly that cell of the 2x4 grid.
    Glyph first;
    decompose(0x1CD00, &first);
    CHECK_MSG(occupies(first.prim[0], { 0, 1.0f / 4, 0.5f, 2.0f / 4 }), "U+1CD00 is the rect for row 1, column 0");
}

/// mosaicMaskForCodePoint resolves the quadrant + sextant block glyphs to a
/// grid bitfield (bit 0 = top-left, reading order). Quadrants are 2×2; sextants
/// are 2×3 with the same skip-the-four-aliased-patterns remap decomposeSextant
/// uses. Every other codePoint returns false with a zeroed-out result.
TEST(testMosaicMask)
{
    // A row whose grid is zero leaves the grid unasserted.
    const struct {
        unsigned codePoint;
        unsigned mask;
        int columns;
        int rows;
        const char* meaning;
    } mosaics[] = {
        // Quadrants: lower-right is bit 3 and upper-left bit 0; U+2599 lights
        // the upper-left, lower-left, and lower-right.
        { 0x2597, 1u << 3, 2, 2, "U+2597, the lower-right quadrant, on the 2x2 grid" },
        { 0x2598, 1u << 0, 0, 0, "U+2598, the upper-left quadrant" },
        { 0x2599, (1u << 0) | (1u << 2) | (1u << 3), 0, 0, "U+2599, three quadrants" },
        // Sextants: the first is pattern 1. Index 19 is pattern 20, just under
        // the skipped 21 that U+258C already draws, and index 20 jumps past it
        // to 22: the remap the renderer shares with decompose. The last takes
        // both skips, past 21 and 42, to 62, every cell but the top-left.
        { 0x1FB00, 1u, 2, 3, "U+1FB00, the first sextant, on the 2x3 grid" },
        { 0x1FB00 + 19, 20u, 0, 0, "U+1FB13, sextant index 19" },
        { 0x1FB00 + 20, 22u, 0, 0, "U+1FB14, sextant index 20, past the skipped 21" },
        { 0x1FB3B, 62u, 0, 0, "U+1FB3B, sextant index 59, past both skips" },
    };
    for (const auto& expected : mosaics) {
        MosaicMask resolved {};
        CHECK_MSG(mosaicMaskForCodePoint(expected.codePoint, &resolved), expected.meaning);
        CHECK_MSG(resolved.mask == expected.mask, expected.meaning);
        if (expected.columns != 0)
            CHECK_MSG(resolved.columns == expected.columns && resolved.rows == expected.rows, expected.meaning);
    }

    // A refusal clears what the caller's mask held, here the last sextant.
    MosaicMask previous { .mask = 62u, .columns = 2, .rows = 3 };
    CHECK_MSG(!mosaicMaskForCodePoint('A', &previous), "a letter is not a mosaic");
    CHECK_MSG(previous.mask == 0 && previous.columns == 0 && previous.rows == 0, "and the refusal leaves the mask zeroed");

    // A line is no block fill, and the upper half block, though a fill, is one
    // solid rect rather than a sub-cell grid, so neither is mosaic-routed.
    MosaicMask unrouted {};
    CHECK_MSG(!mosaicMaskForCodePoint(0x2500, &unrouted), "U+2500, a line, is not a mosaic");
    CHECK_MSG(!mosaicMaskForCodePoint(0x2580, &unrouted), "U+2580, the upper half block, is not a mosaic");
}

/// Dashed light horizontal U+2504 still emits connected left+right stubs
/// (so it tiles) and tags them with a dash count of 3.
TEST(testDashed)
{
    Glyph g;
    decompose(0x2504, &g);
    CHECK_EQ(g.count, 2);
    const StubReach reach = stubReach(g);
    CHECK_MSG(reach.left == 1 && reach.right == 1, "the triple dash still reaches both side edges, so it tiles");
    CHECK_EQ(g.prim[0].dashes, 3);
}

TEST(testRoundedCorner)
{
    Glyph glyph;
    CHECK(decompose(0x256D, &glyph) == 1);
    CHECK(glyph.prim[0].kind == PrimitiveKind::Arc);
    CHECK(approx(glyph.prim[0].x0, 1.0f));
    CHECK(approx(glyph.prim[0].y0, 1.0f));
    CHECK(approx(glyph.prim[0].x1, 0.5f));
}

TEST(testRoundedCornerFontReach)
{
    for (unsigned codePoint = 0x256D; codePoint <= 0x2570; ++codePoint) {
        bool left, right, top, bottom;
        CHECK(Ubiquitty::Rendering::boxReachEdges(codePoint, &left, &right, &top, &bottom));
        CHECK(left == (codePoint == 0x256E || codePoint == 0x256F));
        CHECK(right == (codePoint == 0x256D || codePoint == 0x2570));
        CHECK(top == (codePoint == 0x256F || codePoint == 0x2570));
        CHECK(bottom == (codePoint == 0x256D || codePoint == 0x256E));
    }
}

/// Each rounded corner centres its arc at the cell corner opposite its
/// bulge, so the four corners frame a rounded box.
TEST(testRoundedCornerQuadrants)
{
    Glyph g;
    auto findArc = [&](unsigned cp) -> const Primitive* {
        decompose(cp, &g);
        for (int i = 0; i < g.count; ++i)
            if (g.prim[i].kind == PrimitiveKind::Arc)
                return &g.prim[i];
        return nullptr;
    };
    // The centre is the corner the two arms meet across.
    const struct {
        unsigned codePoint;
        float centreX;
        float centreY;
        const char* meaning;
    } corners[] = {
        { 0x256E, 0.0f, 1.0f, "╮ joins down and left, so its arc is centred bottom-left" },
        { 0x256F, 0.0f, 0.0f, "╯ joins up and left, so its arc is centred top-left" },
        { 0x2570, 1.0f, 0.0f, "╰ joins up and right, so its arc is centred top-right" },
    };
    for (const auto& corner : corners) {
        const Primitive* arc = findArc(corner.codePoint);
        CHECK_MSG(arc && isAt(arc->x0, arc->y0, corner.centreX, corner.centreY), corner.meaning);
    }
}

/// Diagonals U+2571/2/3 emit corner-to-corner line(s); the cross is two.
TEST(testDiagonal)
{
    Glyph g;
    const Primitive& stroke = g.prim[0];
    decompose(0x2571, &g);
    CHECK_MSG(g.count == 1 && stroke.kind == PrimitiveKind::Diagonal, "U+2571 is a single corner-to-corner stroke");
    decompose(0x2573, &g);
    CHECK_EQ(g.count, 2);
}

/// Coverage predicate matches Decompose: a covered codePoint yields >0
/// primitives, an uncovered one (a letter) yields 0.
TEST(testCoverage)
{
    const struct {
        unsigned codePoint;
        bool isCovered;
        const char* meaning;
    } claims[] = {
        { 0x2500, true, "the light horizontal is covered" },
        { 0x2588, true, "so is the full block" },
        { 0x1FB00, true, "and the first sextant" },
        { 0x1CD00, true, "and the first octant" },
        { 0x1CDE5, true, "and the last octant" },
        { 0x1CCFF, false, "the code point under the octants is not" },
        { 0x1CDE6, false, "nor the person glyph above them" },
        { 'A', false, "nor a letter" },
    };
    for (const auto& claim : claims)
        CHECK_MSG(covers(claim.codePoint) == claim.isCovered, claim.meaning);
    Glyph letter;
    CHECK_MSG(decompose('A', &letter) == 0, "a letter decomposes to no primitives");

    // Dashes are in the box family: covered, so the builder routes them down
    // the box lane and the family's centring shift reaches them. Outside the
    // family a dash rides the text baseline, and under a line spacing the
    // dashed continuation of a solid line sits rows away from the line it
    // continues. Within the lane the font glyph still wins
    // (prefersProcedural declines a dash, and a dash no box font can draw is
    // demoted to the text lane by the builder) and the edge stretch still
    // refuses it (pinned in test_box_edge_stretch) — the stretch is what
    // staggers a dash under fractional advance and closes its gaps, not the
    // shift. Triple/quadruple 2504..250B + double 254C..254F, both weights
    // and orientations.
    constexpr struct {
        unsigned first;
        unsigned last;
    } dashRuns[] = { { 0x2504, 0x250B }, { 0x254C, 0x254F } };
    for (const auto& run : dashRuns) {
        for (unsigned codePoint = run.first; codePoint <= run.last; ++codePoint) {
            CHECK_MSG(isDash(codePoint), "every triple, quadruple, and double dash is classed as a dash");
            CHECK_MSG(covers(codePoint), "a dash is box-covered, so the family shift reaches it");
            CHECK_MSG(!prefersProcedural(codePoint), "a dash keeps the font glyph inside the lane");
        }
    }
    // Only the dashes are classed apart; the solid line, corner, and cross
    // beside them stay covered.
    for (const unsigned solid : { 0x2500u, 0x250Cu, 0x253Cu })
        CHECK_MSG(!isDash(solid) && covers(solid), "a solid line, corner, or cross is covered and is no dash");
}

/// Procedural-preference policy: solid blocks / shades / quadrants / sextants
/// always draw procedurally (no gap at the cell edge), but box-drawing LINES keep font-first
/// (procedural is the fallback only when the font ships no glyph).
TEST(testPrefersProcedural)
{
    const struct {
        unsigned codePoint;
        bool isProcedural;
        const char* meaning;
    } preferences[] = {
        // Fills reach every cell edge only when drawn as geometry.
        { 0x2581, true, "an eighth block is drawn procedurally" },
        { 0x2588, true, "so is the full block" },
        { 0x2592, true, "and a shade" },
        { 0x2596, true, "and a quadrant" },
        { 0x1FB00, true, "and a sextant" },
        { 0x1CD00, true, "and the first octant" },
        { 0x1CDE5, true, "and the last octant" },
        // Lines keep the face's glyph and fall back only where it has none.
        { 0x2500, false, "a box line keeps the font glyph" },
        { 0x253C, false, "so does a box cross" },
        { 0x256D, false, "and a rounded corner" },
        // A font diagonal neither reaches the cell corners nor edge-stretches,
        // so it cannot tile without gaps.
        { 0x2571, true, "the rising diagonal is drawn procedurally" },
        { 0x2572, true, "so is the falling diagonal" },
        { 0x2573, true, "and the diagonal cross" },
        { 0x1FBA0, true, "and a legacy half-diagonal" },
        { 0x1FBAF, true, "and a legacy diagonal combination" },
        { 0x1FBD0, true, "and the first corner-to-edge diagonal" },
        { 0x1FBDF, true, "and the last corner-to-edge diagonal" },
        { 0xE0B0, true, "a solid Powerline separator is drawn procedurally" },
        { 0xE0B1, true, "so is an outline separator" },
        { 0xE0B4, true, "and a rounded one" },
        { 0xE0A0, false, "a Powerline emblem keeps the font glyph" },
        { 0xE0D0, false, "so does a Powerline symbol with no analytic shape" },
        { 0x2574, false, "and a plain stub" },
        { 'A', false, "a letter is never procedural" },
    };
    for (const auto& preference : preferences)
        CHECK_MSG(prefersProcedural(preference.codePoint) == preference.isProcedural, preference.meaning);
}

/// Centring policy: the family's shift moves the shapes and leaves the
/// positions alone.
///
/// The shift is measured from U+2500 and centres every box glyph's ink on the
/// cell, so a stroke drawn from the face lands on the row the procedural stroke
/// takes. The terminal-graphics family is named by position — scans 1, 3, 7 and
/// 9, and the left and right verticals — so centring one moves it to the row
/// U+2500 occupies, which is the character U+2500.
TEST(testTakesFamilyCentring)
{
    for (unsigned cp = 0x23B8; cp <= 0x23BD; ++cp) {
        CHECK_MSG(covers(cp), "the terminal-graphics family stays in the box lane");
        CHECK_MSG(!takesFamilyCentring(cp), "a scan line keeps the row it names");
    }
    CHECK_MSG(takesFamilyCentring(0x2500), "the light horizontal centres with the family");
    CHECK_MSG(takesFamilyCentring(0x2502), "the light vertical centres with the family");
    CHECK_MSG(takesFamilyCentring(0x250C), "a corner centres with the family");
    CHECK_MSG(takesFamilyCentring(0x2504), "a dash centres with the family");
    CHECK_MSG(takesFamilyCentring(0x23BE), "a dentistry elbow is outside the family");
    CHECK_MSG(takesFamilyCentring(0x23B7), "the radical's foot is outside the family");
    CHECK_MSG(takesFamilyCentring('A'), "a letter never reaches the box lane");
}

/// Sum of triangle areas in a glyph, in unit² — for verifying wedge /
/// triangular-block coverage.
static float triArea(const Glyph& g)
{
    float a = 0;
    for (int i = 0; i < g.count; ++i) {
        const Primitive& p = g.prim[i];
        if (p.kind == PrimitiveKind::Triangle)
            a += fabsf((p.x1 - p.x0) * (p.y2 - p.y0) - (p.x2 - p.x0) * (p.y1 - p.y0)) * 0.5f;
    }
    return a;
}

/// One-quarter triangular blocks U+1FB6C-6F are a single triangle of area
/// 1/4 (base 1, height 1/2).
TEST(testTriangularQuarter)
{
    Glyph g;
    const Primitive& wedge = g.prim[0];
    decompose(0x1FB6C, &g); // left triangle
    CHECK_MSG(g.count == 1 && wedge.kind == PrimitiveKind::Triangle, "U+1FB6C is a single triangle");
    CHECK_MSG(approx(triArea(g), 0.25f), "covering a quarter of the cell");
    decompose(0x1FB6D, &g); // upper triangle
    CHECK_MSG(approx(triArea(g), 0.25f), "and U+1FB6D, the upper triangle, covers a quarter too");
}

/// Three-quarter triangular block U+1FB68 fills 3/4 of the cell (full minus
/// the opposite quarter triangle).
TEST(testTriangularThreeQuarter)
{
    Glyph g;
    decompose(0x1FB68, &g);
    const float covered = triArea(g);
    CHECK_MSG(g.count >= 3, "U+1FB68 is built from several triangles");
    CHECK_MSG(approx(covered, 0.75f), "that together cover three quarters of the cell");
}

/// Wedge diagonal U+1FB45 (LOWER RIGHT block, LL→UC→...) fills a polygon;
/// U+1FB40 (LOWER LEFT, UL→LC) is the left half cut diagonally — area 1/4.
TEST(testWedge)
{
    Glyph g;
    decompose(0x1FB40, &g); // LL block, cut UL→LC
    CHECK_MSG(g.count > 0, "U+1FB40 decomposes");
    // Triangle UL(0,0)-LC(0.5,1)-LL(0,1): area = 1/2*base*height = 1/4.
    CHECK_MSG(approx(triArea(g), 0.25f), "into a quarter of the cell");
    decompose(0x1FB47, &g); // LR block, LC→LMR
    CHECK_MSG(g.count > 0, "U+1FB47 decomposes");
}

/// Vertical / horizontal one-eighth blocks U+1FB70-7B select a single
/// eighth-wide column / row.
TEST(testLegacyEighths)
{
    Glyph g;
    const Primitive& bar = g.prim[0];
    decompose(0x1FB70, &g); // vertical eighth-2
    CHECK_MSG(g.count == 1 && bar.kind == PrimitiveKind::Rect, "U+1FB70 is a single rect");
    CHECK_MSG(spansColumns(bar, 1.0f / 8, 2.0f / 8), "in the second eighth-wide column");
    CHECK_MSG(spansRows(bar, 0, 1), "at the full height of the cell");
    decompose(0x1FB76, &g); // horizontal eighth-2
    CHECK_MSG(spansRows(bar, 1.0f / 8, 2.0f / 8), "U+1FB76 is the second eighth-tall row");
}

/// Upper-N / right-N eighth blocks.
TEST(testLegacyPartialBlocks)
{
    Glyph g;
    const Primitive& block = g.prim[0];
    decompose(0x1FB82, &g); // upper one quarter
    CHECK_MSG(approx(block.y1, 2.0f / 8) && approx(block.x1, 1), "U+1FB82 ends two eighths down and reaches the right edge");
    decompose(0x1FB87, &g); // right one quarter
    CHECK_MSG(spansColumns(block, 6.0f / 8, 1), "U+1FB87 holds the rightmost two eighths");
    decompose(0x1FBCE, &g); // left two-thirds
    CHECK_MSG(approx(block.x1, 2.0f / 3), "U+1FBCE ends two thirds across");
}

/// Legacy medium-shade halves U+1FB8C-8F are stipple fills of one half.
TEST(testLegacyShade)
{
    Glyph g;
    const Primitive& shade = g.prim[0];
    decompose(0x1FB8C, &g); // left half medium shade
    CHECK_MSG(g.count == 1 && shade.kind == PrimitiveKind::Stipple, "U+1FB8C is a single dither field");
    CHECK_MSG(approx(shade.x1, 0.5f) && approx(shade.alpha, 0.5f), "over the left half, at half density");
    decompose(0x1FB9C, &g); // upper-left triangular medium shade
    CHECK_MSG(shade.kind == PrimitiveKind::Triangle && approx(shade.alpha, 0.5f), "U+1FB9C shades a triangle at half density");
}

/// Box-drawing light diagonals U+1FBA0.. are stroked Diagonals, not fills.
TEST(testLegacyDiagonal)
{
    Glyph g;
    const Primitive& stroke = g.prim[0];
    decompose(0x1FBA0, &g);
    CHECK_MSG(g.count == 1 && stroke.kind == PrimitiveKind::Diagonal, "U+1FBA0 is a single stroked diagonal, not a fill");
    decompose(0x1FBAE, &g); // diamond
    CHECK_EQ(g.count, 4);
}

/// Legacy coverage: geometric families covered, pictographic ones deferred.
TEST(testLegacyCoverage)
{
    const struct {
        unsigned codePoint;
        bool isCovered;
        const char* meaning;
    } families[] = {
        // Geometric families are drawn as primitives.
        { 0x1FB6C, true, "a triangular quarter block is covered" },
        { 0x1FB45, true, "so is a wedge" },
        { 0x1FB70, true, "and a vertical eighth" },
        { 0x1FB95, true, "and the checker board" },
        { 0x1FBA0, true, "and a light diagonal" },
        // Pictographic ones are the font's to draw.
        { 0x1FBC5, false, "a stick figure is left to the font" },
        { 0x1FBF0, false, "so is a segmented digit" },
        { 0x1FBE0, false, "and a circle slice" },
    };
    for (const auto& family : families)
        CHECK_MSG(covers(family.codePoint) == family.isCovered, family.meaning);
}

/// Powerline separators U+E0B0-B3: the FILLED ones span edge to edge so
/// consecutive prompt segments meet with no gap.
TEST(testPowerline)
{
    Glyph g;
    decompose(0xE0B0, &g); // right-pointing filled triangle
    const Primitive& p = g.prim[0];
    CHECK_MSG(g.count == 1 && p.kind == PrimitiveKind::Triangle, "U+E0B0 is a single filled triangle");
    // Vertices: (0,0)(1,0.5)(0,1) — full height left edge, point at right mid.
    CHECK_MSG(isAt(p.x0, p.y0, 0, 0), "its first vertex is the cell's top-left corner");
    CHECK_MSG(isAt(p.x1, p.y1, 1, 0.5f), "its point is the middle of the right edge");
    CHECK_MSG(isAt(p.x2, p.y2, 0, 1), "its last vertex is the cell's bottom-left corner");
    CHECK_MSG(approx(triArea(g), 0.5f), "so it fills half the cell");

    decompose(0xE0B2, &g); // left-pointing filled triangle
    CHECK_MSG(g.count == 1 && p.kind == PrimitiveKind::Triangle, "U+E0B2 is a single filled triangle too");
    CHECK_MSG(approx(p.x0, 1) && approx(p.x1, 0), "starting on the right edge and pointing left");

    decompose(0xE0B1, &g); // thin right angle
    CHECK_MSG(g.count == 2 && p.kind == PrimitiveKind::Diagonal, "U+E0B1, the thin angle, is two strokes");
    CHECK_MSG(!covers(0xE0A0), "Powerline branch icon stays on the bundled font");
    CHECK_MSG(!covers(0xE0C0), "Powerline flame keeps its font artwork");
    CHECK_MSG(!covers(0xE0D0), "Powerline Lego glyph keeps its font artwork");
    CHECK_MSG(covers(0xE0BF), "last analytic Powerline separator is covered");
    for (const unsigned outside : { 0xE099u, 0xE0E0u })
        CHECK_MSG(!covers(outside), "the code points either side of the Powerline set are not covered");
}

TEST(testAssignedPowerlineGlyphs)
{
    CHECK(isPowerlineGlyph(0xE0A0));
    CHECK(isPowerlineGlyph(0xE0A3));
    CHECK(isPowerlineGlyph(0xE0B0));
    CHECK(isPowerlineGlyph(0xE0C8));
    CHECK(isPowerlineGlyph(0xE0CA));
    CHECK(isPowerlineGlyph(0xE0CC));
    CHECK(isPowerlineGlyph(0xE0D2));
    CHECK(isPowerlineGlyph(0xE0D4));
    CHECK(isPowerlineGlyph(0xE0D6));
    CHECK(isPowerlineGlyph(0xE0D7));
    CHECK(!isPowerlineGlyph(0xE0C9));
    CHECK(!isPowerlineGlyph(0xE0CB));
    CHECK(!isPowerlineGlyph(0xE0D3));
    CHECK(!isPowerlineGlyph(0xE0D5));
}

/// Analytic single-quad SDF classification: powerlineShape maps each simple
/// separator to the shape the renderer's edge-AA path draws, while icons and
/// complex Extra artwork stay on the bundled font. The fragment derives
/// feathered coverage from these parameters, so the orientation encoded here
/// is the separator's orientation.
TEST(testPowerlineShapeKind)
{
    PowerlineShape s;
    // The slots BoxDrawing.h assigns beyond a shape's corners.
    constexpr int sideSignSlot = 4; // a half-disc's kept side
    constexpr int halfDiscStrokeSlot = 5; // an outline half-disc's stroke half-width
    constexpr int wedgeStrokeSlot = 6; // an outline wedge's stroke half-width

    // E0B0 right-pointing solid triangle: corners (0,0)(1,0.5)(0,1).
    CHECK_MSG(powerlineShape(0xE0B0, &s), "U+E0B0 resolves to an analytic shape");
    CHECK_MSG(s.kind == PowerlineShapeKind::SolidTriangle, "a solid triangle");
    CHECK_MSG(isAt(s.p[0], s.p[1], 0, 0), "whose first corner is the cell's top-left");
    CHECK_MSG(isAt(s.p[2], s.p[3], 1, 0.5f), "whose apex is the middle of the right edge");
    CHECK_MSG(isAt(s.p[4], s.p[5], 0, 1), "and whose last corner is the cell's bottom-left");

    // E0B2 mirrors E0B0 (points left): vertices (1,0)(0,0.5)(1,1).
    CHECK_MSG(powerlineShape(0xE0B2, &s) && s.kind == PowerlineShapeKind::SolidTriangle, "U+E0B2 resolves to a solid triangle");
    CHECK_MSG(approx(s.p[0], 1) && approx(s.p[2], 0) && approx(s.p[4], 1), "pointing left, the mirror image of U+E0B0");

    // E0B1/E0B3 thin chevrons → OutlineWedge with a non-zero stroke half:
    // the band runs INWARD from the two apex edges, so the corners and apex
    // stay on the stroke's outer boundary and the ends finish as points.
    CHECK_MSG(powerlineShape(0xE0B1, &s) && s.kind == PowerlineShapeKind::OutlineWedge, "E0B1 outline chevron");
    CHECK_MSG(s.p[wedgeStrokeSlot] > 0.0f, "the thin chevron has a stroke to band inward by");
    CHECK_MSG(powerlineShape(0xE0B3, &s) && s.kind == PowerlineShapeKind::OutlineWedge, "E0B3 outline chevron");

    // The rounded separators are half-discs centred on the flat edge, a full
    // cell wide and half a cell tall, bulging toward the side their sign keeps.
    CHECK_MSG(powerlineShape(0xE0B4, &s) && s.kind == PowerlineShapeKind::SolidHalfDisc, "U+E0B4 resolves to a solid half-disc");
    CHECK_MSG(isAt(s.p[0], s.p[1], 0, 0.5f), "centred on the middle of the left edge");
    CHECK_MSG(isAt(s.p[2], s.p[3], 1, 0.5f), "with radii of the full width and half the height");
    CHECK_MSG(s.p[sideSignSlot] > 0.0f, "keeping the half to the right of its centre");
    CHECK_MSG(powerlineShape(0xE0B6, &s) && s.kind == PowerlineShapeKind::SolidHalfDisc, "U+E0B6 resolves to a solid half-disc");
    CHECK_MSG(approx(s.p[0], 1) && s.p[sideSignSlot] < 0.0f, "centred on the right edge and bulging left");

    // The thin rounded separators keep the same disc and add a stroke.
    CHECK_MSG(powerlineShape(0xE0B5, &s) && s.kind == PowerlineShapeKind::OutlineHalfDisc, "U+E0B5 resolves to an outline half-disc");
    CHECK_MSG(s.p[halfDiscStrokeSlot] > 0.0f, "with a stroke half-width to band inward by");
    CHECK_MSG(powerlineShape(0xE0B7, &s) && s.kind == PowerlineShapeKind::OutlineHalfDisc, "and so does U+E0B7");

    // Corner wedges ◣◢◤◥ are solid triangles; their thin forms are single
    // diagonal strokes banded toward the solid sibling's filled side, so the
    // corner-to-corner line stays the stroke's outer boundary.
    CHECK_MSG(powerlineShape(0xE0B8, &s) && s.kind == PowerlineShapeKind::SolidTriangle, "U+E0B8, a corner wedge, resolves to a solid triangle");
    CHECK_MSG(powerlineShape(0xE0B9, &s) && s.kind == PowerlineShapeKind::OutlineDiagonal, "E0B9 corner wedge thin");
    CHECK_MSG(approx(s.p[4], 0) && approx(s.p[5], 1), "E0B9 witness is the solid sibling's bottom-left corner");
    CHECK_MSG(powerlineShape(0xE0BB, &s) && s.kind == PowerlineShapeKind::OutlineDiagonal, "E0BB corner wedge thin");
    CHECK_MSG(approx(s.p[4], 1) && approx(s.p[5], 1), "E0BB witness is the solid sibling's bottom-right corner");
    CHECK_MSG(powerlineShape(0xE0BD, &s) && s.kind == PowerlineShapeKind::OutlineDiagonal, "E0BD corner wedge thin");
    CHECK_MSG(approx(s.p[4], 0) && approx(s.p[5], 0), "E0BD witness is the solid sibling's top-left corner");
    CHECK_MSG(powerlineShape(0xE0BF, &s) && s.kind == PowerlineShapeKind::OutlineDiagonal, "E0BF corner wedge thin");
    CHECK_MSG(approx(s.p[4], 1) && approx(s.p[5], 0), "E0BF witness is the solid sibling's top-right corner");

    // The Geometric Shapes corner triangles are the solid wedges' half-cells,
    // drawn by the same feathered shape rather than a grid-cut fill. The cell
    // resolver answers for them; the Powerline resolver does not.
    const std::pair<unsigned, unsigned> twins[] = { { 0x25E3, 0xE0B8 }, { 0x25E2, 0xE0BA }, { 0x25E4, 0xE0BC }, { 0x25E5, 0xE0BE } };
    for (const auto& [triangle, wedge] : twins) {
        PowerlineShape wedgeShape {};
        CHECK_MSG(powerlineShape(wedge, &wedgeShape), "the wedge maps to a shape");
        CHECK_MSG(!powerlineShape(triangle, &s) && s.kind == PowerlineShapeKind::None, "the corner triangle is not a Powerline separator");
        CHECK_MSG(analyticCellShape(triangle, &s) && s.kind == PowerlineShapeKind::SolidTriangle, "the corner triangle is a solid analytic triangle");
        bool isSameTriangle = true;
        for (int i = 0; i < 6; ++i)
            isSameTriangle = isSameTriangle && approx(s.p[i], wedgeShape.p[i]);
        CHECK_MSG(isSameTriangle, "with the corners of the wedge that draws the same half-cell");
    }

    // Every other cell resolves as the Powerline resolver does, and the
    // triangles' Geometric Shapes neighbours stay text.
    for (unsigned codePoint = 0xE0B0; codePoint <= 0xE0BF; ++codePoint) {
        PowerlineShape separator {};
        CHECK_MSG(analyticCellShape(codePoint, &s) && powerlineShape(codePoint, &separator) && s.kind == separator.kind,
            "a separator resolves to its own shape");
    }
    CHECK_MSG(!analyticCellShape(0x25E1, &s) && s.kind == PowerlineShapeKind::None, "U+25E1 is text");
    CHECK_MSG(!analyticCellShape(0x25E6, &s) && s.kind == PowerlineShapeKind::None, "U+25E6 is text");

    // Icons and complex Powerline Extra artwork stay on the bundled font.
    CHECK_MSG(!powerlineShape(0xE0A0, &s) && s.kind == PowerlineShapeKind::None, "U+E0A0, an emblem, has no analytic shape and reports none");
    const struct {
        unsigned codePoint;
        const char* meaning;
    } unshaped[] = {
        { 0xE0C0, "U+E0C0, the flame, keeps its font artwork" },
        { 0xE0C4, "U+E0C4, the pixelated separator, keeps its font artwork" },
        { 0xE0C8, "U+E0C8, the ice waveform, keeps its font artwork" },
        { 0xE0CC, "U+E0CC, the honeycomb, keeps its font artwork" },
        { 0xE0CE, "U+E0CE, the Lego separator, keeps its font artwork" },
        { 0xE0D0, "U+E0D0, the Lego block, keeps its font artwork" },
        { 0x2588, "and the full block is no Powerline shape at all" },
    };
    for (const auto& glyph : unshaped)
        CHECK_MSG(!powerlineShape(glyph.codePoint, &s), glyph.meaning);
}

/// --- Cell-midline symmetry of the FALLBACK procedural stub geometry. ----
///
/// Box-drawing lines are font-first (edge-stretched); the procedural form is the
/// fallback used only when the font ships no glyph. That fallback anchors a
/// horizontal stroke on the cell VERTICAL midline and a vertical stroke on the
/// cell HORIZONTAL midline, so it is symmetric by construction. These tests
/// replicate the procedural stub→pixel mapping on a sample cell
/// and assert the fallback's extents are centred / mirrored.

struct PxRect {
    float x0, y0, x1, y1;
};

/// Map a single light/heavy Stub onto the cell rect [L,T,R,B] exactly as
/// emitBoxFill does: snapped centre, symmetric stroke band, edge→centre span.
static PxRect stubRect(const Primitive& primitive, float L, float T, float R, float B)
{
    const float cellHeight = B - T;
    float light = roundf(cellHeight / 12.0f);
    if (light < 1.0f)
        light = 1.0f;
    float heavy = roundf(light * 2.0f);
    if (heavy < light + 1.0f)
        heavy = light + 1.0f;
    const float cx = roundf((L + R) * 0.5f);
    const float cy = roundf((T + B) * 0.5f);
    const float thick = (primitive.weight == Weight::Heavy) ? heavy : light;
    float half = roundf(thick * 0.5f);
    if (half < 0.5f)
        half = 0.5f;
    const float insetX = primitive.inset * (cx - L);
    const float insetY = primitive.inset * (cy - T);
    PxRect r { 0, 0, 0, 0 };
    if (primitive.direction == Direction::Up) {
        r = { cx - half, T, cx + (thick - half), cy - insetY };
    } else if (primitive.direction == Direction::Down) {
        r = { cx - half, cy + insetY, cx + (thick - half), B };
    } else if (primitive.direction == Direction::Left) {
        r = { L, cy - half, cx - insetX, cy + (thick - half) };
    } else {
        r = { cx + insetX, cy - half, R, cy + (thick - half) };
    }
    return r;
}

static const Primitive* findStub(const Glyph& glyph, Direction direction)
{
    for (int i = 0; i < glyph.count; ++i)
        if (glyph.prim[i].kind == PrimitiveKind::Stub && glyph.prim[i].direction == direction)
            return &glyph.prim[i];
    return nullptr;
}

/// Whether two device rects cover the same rows.
static bool shareRows(const PxRect& first, const PxRect& second) { return approx(first.y0, second.y0) && approx(first.y1, second.y1); }

/// Whether two device rects cover the same columns.
static bool shareColumns(const PxRect& first, const PxRect& second) { return approx(first.x0, second.x0) && approx(first.x1, second.x1); }

/// A device rect's extent down the cell.
static float heightOf(const PxRect& rect) { return rect.y1 - rect.y0; }

/// ┼ U+253C: the horizontal stroke is centred on the cell — the band above the
/// stroke equals the band below it, and the vertical stroke's left/right
/// margins match. (The off-centre font glyph would make these unequal.)
TEST(testCrossCentered)
{
    const float L = 0, T = 0, R = 12, B = 24; // even, even stroke → exact
    Glyph g;
    decompose(0x253C, &g);

    // Horizontal stroke = the union of the L and R stubs (same band in y).
    const Primitive* left = findStub(g, Direction::Left);
    const Primitive* right = findStub(g, Direction::Right);
    REQUIRE_MSG(left && right, "U+253C has an arm toward each side edge");
    PxRect lr = stubRect(*left, L, T, R, B);
    PxRect rr = stubRect(*right, L, T, R, B);
    CHECK_MSG(shareRows(lr, rr), "both horizontal arms of ┼ lie in one band");
    const float marginAbove = lr.y0 - T;
    const float marginBelow = B - lr.y1;
    CHECK_MSG(approx(marginAbove, marginBelow), "with as much cell above the band as below it");

    // Vertical stroke = union of U and D stubs; centred in x.
    const Primitive* up = findStub(g, Direction::Up);
    const Primitive* down = findStub(g, Direction::Down);
    PxRect ur = stubRect(*up, L, T, R, B);
    PxRect dr = stubRect(*down, L, T, R, B);
    CHECK_MSG(shareColumns(ur, dr), "both vertical arms of ┼ lie in one column band");
    const float marginLeft = ur.x0 - L;
    const float marginRight = R - ur.x1;
    CHECK_MSG(approx(marginLeft, marginRight), "with as much cell to its left as to its right");

    // The arms mirror about the horizontal midline, the symmetry the fallback
    // geometry is built to guarantee.
    CHECK_MSG(approx(heightOf(ur), heightOf(dr)), "the up arm is exactly as long as the down arm");
}

/// ╰ (U+2570, up+right) and ╭ (U+256D, down+right) are mirror images about the
/// horizontal midline: the up-arm of ╰ must be the same length as the down-arm
/// of ╭, or up-reaching corners draw longer than down-reaching ones.
TEST(testRoundedMirror)
{
    Glyph up, down;
    decompose(0x2570, &up);
    decompose(0x256D, &down);
    CHECK(up.count == 1 && down.count == 1);
    CHECK(approx(up.prim[0].x0, down.prim[0].x0));
    CHECK(approx(up.prim[0].y0, 1.0f - down.prim[0].y0));
    CHECK(approx(up.prim[0].x1, down.prim[0].x1));
}

/// ┗ (U+2517, heavy up+right) and ┏ (U+250F, heavy down+right) corners: the
/// up-reaching arm of ┗ has exactly the length of the down-reaching arm of ┏.
TEST(testHeavyCornerMirror)
{
    const float L = 0, T = 0, R = 13, B = 26;
    Glyph up, down;
    decompose(0x2517, &up); // ┗ up+right (heavy)
    decompose(0x250F, &down); // ┏ down+right (heavy)
    PxRect upArm = stubRect(*findStub(up, Direction::Up), L, T, R, B);
    PxRect downArm = stubRect(*findStub(down, Direction::Down), L, T, R, B);
    CHECK_MSG(approx(heightOf(upArm), heightOf(downArm)), "┗ reaches up exactly as far as ┏ reaches down");
    CHECK_MSG(shareColumns(upArm, downArm), "in the same heavy column");
}

/// Seam-freedom: a vertical line │ in cell N's bottom meets the same line in
/// cell N+1's top at the same x column with no gap. Stub down of row N ends at
/// B; stub up of row N+1 starts at T == previous B; their x bands coincide.
TEST(testVerticalSeamless)
{
    const float L = 0, R = 13;
    const float Ta = 0, Ba = 26; // row N
    const float Tb = 26, Bb = 52; // row N+1 (Tb == Ba)
    Glyph g;
    decompose(0x2502, &g); // │ light vertical (up+down)
    PxRect downN = stubRect(*findStub(g, Direction::Down), L, Ta, R, Ba);
    PxRect upN1 = stubRect(*findStub(g, Direction::Up), L, Tb, R, Bb);
    const struct {
        float actual;
        float expected;
        const char* meaning;
    } boundary[] = {
        { downN.y1, Ba, "the upper cell's down arm ends on its bottom edge" },
        { upN1.y0, Tb, "the lower cell's up arm starts on its top edge" },
        { downN.y1, upN1.y0, "so the two meet at the shared boundary with no gap" },
    };
    for (const auto& edge : boundary)
        CHECK_MSG(approx(edge.actual, edge.expected), edge.meaning);
    CHECK_MSG(shareColumns(downN, upN1), "and in the same column on either side of it");
}

/// Double horizontal ═ U+2550 draws two parallel light rails, both reaching
/// the left and right edges so doubles tile too.
TEST(testDoubleRails)
{
    Glyph g;
    decompose(0x2550, &g);
    CHECK_EQ(g.count, 2);
    for (const Primitive& rail : primitivesOf(g))
        CHECK_MSG(rail.weight == Weight::Double, "each half of ═ is a double-weight stub");
}

/// Vertical-centering shift for a baseline-placed box font glyph. The renderer
/// shifts the rasterised ink by this many rows so the connection midline lands on
/// the cell box centre before the symmetric edge-stretch. A glyph already centred
/// needs no shift; a baseline-placed (low-sitting) glyph shifts up, and the
/// result centres the ink box in the cell box (so up-/down-reaches mirror).
TEST(testCenterShift)
{
    // Every case shifts within the cell box rows [0, 24), whose centre is 12.
    const struct {
        int inkMinY;
        int inkMaxY;
        int shift;
        const char* meaning;
    } placements[] = {
        { 10, 14, 0, "ink already centred is left where it is" },
        // A baseline-placed crossbar sits low, centred on 18: up six rows.
        { 16, 20, -6, "ink below the centre moves up onto it" },
        { 4, 8, 6, "ink above the centre moves down by the same amount" },
        // A half-row offset rounds away from zero, keeping the stroke on whole rows.
        { 13, 14, -2, "an odd offset rounds away from zero" },
        // An extent whose maximum is under its minimum has no ink to move.
        { 5, 4, 0, "no ink, no shift" },
    };
    for (const auto& placement : placements)
        CHECK_MSG(centerShiftY(placement.inkMinY, placement.inkMaxY, 0, 24) == placement.shift, placement.meaning);

    const int upward = centerShiftY(16, 20, 0, 24);
    CHECK_MSG((16 + upward) + (20 + upward) == 0 + 24, "the shifted ink box is centred in the cell box");
}

/// The renderer measures the box family's connection-stroke row r once (from a
/// reference ─) and applies a SINGLE uniform shift to every box glyph. This test
/// mirrors that contract: given a measured stroke band and the cell box, the
/// shift moves the stroke's row to the cell centre, and the same
/// shift centres an asymmetric glyph's shared stroke too (no per-glyph divergence,
/// so straight lines, corners and junctions keep meeting on one row).
TEST(testUniformCenterShift)
{
    // A baseline-placed font box glyph: the cell box spans rows [4, 36) (height
    // 32, centre 20), and the font draws every horizontal connection stroke as a
    // 3px band whose centre sits ~55% down the box (low, like the real bug).
    const int cellTop = 4, cellBottom = 36; // cell box [4,36)
    const int strokeMinY = 21, strokeMaxY = 23; // R band, centre 22
    const int shift = centerShiftY(strokeMinY, strokeMaxY, cellTop, cellBottom);

    // The single shift must move the stroke band up onto the cell centre.
    CHECK_MSG(shift < 0, "a stroke below the centre is shifted up");
    const int newMinY = strokeMinY + shift, newMaxY = strokeMaxY + shift;
    const int bandCenter2 = newMinY + newMaxY; // 2 × band centre
    const int cellCenter2 = cellTop + cellBottom; // 2 × cell centre
    CHECK_MSG(bandCenter2 == cellCenter2, "and lands with its centre on the cell's centre");

    // A corner's arm is drawn on the same row as the straight line's stroke, so
    // the one shift measured from the straight line carries the arm with it and
    // the junction stays gapless; no glyph is centred on its own ink.
    const int cornerArmMinY = strokeMinY, cornerArmMaxY = strokeMaxY;
    const int shiftedArmMinY = cornerArmMinY + shift;
    const int shiftedArmMaxY = cornerArmMaxY + shift;
    CHECK_MSG(shiftedArmMinY == newMinY && shiftedArmMaxY == newMaxY, "the corner's arm moves onto the straight line's shifted row");

    // Before/after fraction: the stroke centre moves from ~55% of the box height
    // down to exactly the centre (50%).
    const int boxHeight = cellBottom - cellTop;
    const double beforeFrac = (((strokeMinY + strokeMaxY) * 0.5) - cellTop) / boxHeight;
    const double afterFrac = (((newMinY + newMaxY) * 0.5) - cellTop) / boxHeight;
    const bool isCentred = afterFrac > 0.49 && afterFrac < 0.51;
    CHECK_MSG(beforeFrac > 0.5, "unshifted, the stroke sits below the middle of the box");
    CHECK_MSG(isCentred, "shifted, it sits at the middle");
}

/// --- 9-slice edge-stretch (the font-first box-line path). ----------------
///
/// The renderer 9-slices a rasterised box font glyph: the central junction is
/// left intact and the four edge slabs are stretched outward so the connecting
/// strokes reach the cell edges and tile. The pixel transform lives in
/// BoxEdgeStretch.cpp, but it is a pure function of the rasterised buffer +
/// reach flags, so we replicate it here on a synthetic glyph and assert the two
/// required invariants: (a) every reached edge ends up inked at the stroke's
/// row/column, and (b) the central junction is not moved (no recentring), so a
/// corner's horizontal arm stays on the same row as a straight ─ and they tile.
///
/// `edge9slice` is an independent reference for edgeStretchBoxScratch.
static void edge9slice(unsigned char* px, int W, int H, int gx0, int gx1, int gy0, int gy1, bool rL, bool rR, bool rT, bool rB)
{
    if (!(rL || rR || rT || rB))
        return;
    auto at = [&](int x, int y) -> unsigned char& { return px[y * W + x]; };
    const int boxWidth = gx1 - gx0, boxHeight = gy1 - gy0;
    const int cx = (gx0 + gx1) / 2, cy = (gy0 + gy1) / 2;
    int insetX = boxWidth / 6;
    if (insetX < 2)
        insetX = 2;
    int insetY = boxHeight / 6;
    if (insetY < 2)
        insetY = 2;
    if (insetX > boxWidth / 2 - 1 && boxWidth > 3)
        insetX = boxWidth / 2 - 1;
    if (insetY > boxHeight / 2 - 1 && boxHeight > 3)
        insetY = boxHeight / 2 - 1;
    const unsigned char inkThreshold = 128;
    auto colInk = [&](int x) {
        for (int y = gy0; y < gy1; ++y)
            if (at(x, y) >= inkThreshold)
                return true;
        return false;
    };
    auto rowInk = [&](int y) {
        for (int x = gx0; x < gx1; ++x)
            if (at(x, y) >= inkThreshold)
                return true;
        return false;
    };
    if (rL) {
        int sx = gx0 + insetX;
        if (sx >= gx1)
            sx = gx1 - 1;
        while (sx < cx && !colInk(sx))
            ++sx;
        for (int y = gy0; y < gy1; ++y) {
            unsigned char v = at(sx, y);
            for (int x = 0; x < sx; ++x)
                at(x, y) = v;
        }
    }
    if (rR) {
        int sx = gx1 - 1 - insetX;
        if (sx < gx0)
            sx = gx0;
        while (sx > cx && !colInk(sx))
            --sx;
        for (int y = gy0; y < gy1; ++y) {
            unsigned char v = at(sx, y);
            for (int x = sx + 1; x < W; ++x)
                at(x, y) = v;
        }
    }
    if (rT) {
        int sy = gy0 + insetY;
        if (sy >= gy1)
            sy = gy1 - 1;
        while (sy < cy && !rowInk(sy))
            ++sy;
        for (int x = 0; x < W; ++x) {
            unsigned char v = at(x, sy);
            for (int y = 0; y < sy; ++y)
                at(x, y) = v;
        }
    }
    if (rB) {
        int sy = gy1 - 1 - insetY;
        if (sy < gy0)
            sy = gy0;
        while (sy > cy && !rowInk(sy))
            --sy;
        for (int x = 0; x < W; ++x) {
            unsigned char v = at(x, sy);
            for (int y = sy + 1; y < H; ++y)
                at(x, y) = v;
        }
    }
}

/// The 9-slice extends a corner's strokes to the EXACT cell edges (leaving no
/// gap of the kind a stretch sampled 2px inside the ink extremity would), while the
/// central junction — and the horizontal stroke's ROW and the vertical
/// stroke's COLUMN — stay put (no recentring). The corner therefore tiles with a
/// straight ─/│ drawn at the same baseline-relative position.
TEST(testEdge9sliceCorner)
{
    const int W = 20, H = 24, gx0 = 2, gx1 = 18, gy0 = 2, gy1 = 22;
    // Synthetic ┌ (down+right): the font draws strokes at a CONSISTENT
    // baseline-relative position — vertical at column vcol, horizontal at row
    // hrow — but stopping short of the cell edges (the gap). Both vcol/hrow are
    // OFF the geometric cell centre, exactly like Menlo's baseline-placed box.
    const int vcol = 9, hrow = 15;
    unsigned char px[W * H];
    auto draw = [&](unsigned char* b) {
        for (int i = 0; i < W * H; ++i)
            b[i] = 0;
        // vertical arm: column vcol, stopping 4px SHORT of the edge — a font
        // whose stroke falls well short. The 9-slice must still find it (walk
        // the slab line in toward the centre until it carries ink) and extend.
        for (int y = hrow; y < gy1 - 4; ++y)
            b[y * W + vcol] = 255;
        // horizontal arm: row hrow, stopping 4px short of the right edge.
        for (int x = vcol; x < gx1 - 4; ++x)
            b[hrow * W + x] = 255;
    };
    draw(px);
    edge9slice(px, W, H, gx0, gx1, gy0, gy1,
        /*L*/ false, /*R*/ true, /*T*/ false, /*B*/ true);
    const auto inkAt = [&](int x, int y) { return px[y * W + x]; };

    // (a) The down arm now reaches the BOTTOM cell edge (gy1-1) at vcol.
    CHECK_MSG(inkAt(vcol, gy1 - 1) == 255, "the down arm of ┌ is stretched to the bottom edge");
    // (b) The right arm reaches the RIGHT cell edge (gx1-1) at hrow.
    CHECK_MSG(inkAt(gx1 - 1, hrow) == 255, "the right arm of ┌ is stretched to the right edge");
    // (c) The stretch leaves the junction where the face drew it: the
    //     horizontal stroke is still on row hrow from the junction to the right
    //     edge, the row a straight ─ occupies, so the two tile.
    CHECK_MSG(inkAt(gx1 - 1, hrow) == 255 && inkAt(vcol, hrow) == 255, "the horizontal stroke of ┌ keeps its row");
    // (d) ┌ joins only down and right, so nothing reaches the left or top edges.
    CHECK_MSG(inkAt(0, hrow) == 0, "┌ is not stretched to the left edge");
    CHECK_MSG(inkAt(vcol, 0) == 0, "nor to the top edge");
}

/// Two stacked │ cells tile: cell N's down arm reaches its bottom edge at column
/// vcol, cell N+1's up arm reaches its top edge at the same vcol — so across the
/// shared boundary the stroke is continuous (the gap-free seam).
TEST(testEdge9sliceVerticalTile)
{
    const int W = 20, H = 24, gx0 = 2, gx1 = 18, gy0 = 2, gy1 = 22;
    const int vcol = 9;
    unsigned char a[W * H], b[W * H];
    for (int i = 0; i < W * H; ++i)
        a[i] = b[i] = 0;
    // │ in both: a centred vertical stroke that stops short of both edges.
    for (int y = gy0 + 3; y < gy1 - 3; ++y) {
        a[y * W + vcol] = 255;
        b[y * W + vcol] = 255;
    }
    edge9slice(a, W, H, gx0, gx1, gy0, gy1, false, false, true, true);
    edge9slice(b, W, H, gx0, gx1, gy0, gy1, false, false, true, true);
    // The upper cell's last row and the lower cell's first row meet at the boundary.
    const unsigned char upperCellBottom = a[(gy1 - 1) * W + vcol];
    const unsigned char lowerCellTop = b[gy0 * W + vcol];
    CHECK_MSG(upperCellBottom == 255, "the upper │ is stretched down to its bottom edge");
    CHECK_MSG(lowerCellTop == 255, "the lower │ is stretched up to its top edge");
    // Same column on both sides of the boundary → no gap.
    CHECK_MSG(upperCellBottom == lowerCellTop, "so the stroke runs on unbroken from one cell into the next");
}

/// Powerline E0B0 right separator: a single filled triangle whose hypotenuse
/// spans the FULL cell height (top-left → bottom-left) with the point at the
/// right-edge midpoint, so the foreground fills a solid half-cell wedge edge-to-edge. We
/// scan-convert it on a device cell exactly as emitBoxFill does and assert the
/// coverage is the full triangle (≈ half the cell) and reaches all three named
/// corners — i.e. it is solid + full-cell, not thin/undersized.
static int scanTriangleCoverage(const Glyph& g, int W, int H, float L, float T, float R, float B, unsigned char* cov)
{
    for (int i = 0; i < W * H; ++i)
        cov[i] = 0;
    int painted = 0;
    for (int i = 0; i < g.count; ++i) {
        const Primitive& p = g.prim[i];
        if (p.kind != PrimitiveKind::Triangle)
            continue;
        float vx[3] = { L + p.x0 * (R - L), L + p.x1 * (R - L), L + p.x2 * (R - L) };
        float vy[3] = { T + p.y0 * (B - T), T + p.y1 * (B - T), T + p.y2 * (B - T) };
        float ymin = vy[0], ymax = vy[0];
        for (int k = 1; k < 3; ++k) {
            if (vy[k] < ymin)
                ymin = vy[k];
            if (vy[k] > ymax)
                ymax = vy[k];
        }
        for (int yy = (int)floorf(ymin); yy < (int)ceilf(ymax); ++yy) {
            float yc = yy + 0.5f;
            float xs[2];
            int nx = 0;
            for (int e = 0; e < 3 && nx < 2; ++e) {
                float ya = vy[e], yb = vy[(e + 1) % 3], xa = vx[e], xb = vx[(e + 1) % 3];
                if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya)) {
                    float t = (yc - ya) / (yb - ya);
                    xs[nx++] = xa + t * (xb - xa);
                }
            }
            if (nx < 2)
                continue;
            float xa = xs[0] < xs[1] ? xs[0] : xs[1], xb = xs[0] < xs[1] ? xs[1] : xs[0];
            int sx0 = (int)(xa + 0.5f), sx1 = (int)(xb + 0.5f);
            if (sx1 <= sx0)
                continue;
            for (int x = sx0; x < sx1; ++x)
                if (x >= 0 && x < W && yy >= 0 && yy < H && !cov[yy * W + x]) {
                    cov[yy * W + x] = 255;
                    ++painted;
                }
        }
    }
    return painted;
}

TEST(testPowerlineFillSolid)
{
    Glyph g;
    decompose(0xE0B0, &g);
    // Retina-ish cell on a FRACTIONAL origin (exposes per-cell rounding gaps).
    const int W = 64, H = 48;
    const float L = 4.3f, T = 4.0f, R = L + 24.0f, Bv = T + 40.0f; // 24×40 cell
    static unsigned char cov[64 * 48];
    int painted = scanTriangleCoverage(g, W, H, L, T, R, Bv, cov);
    float cellArea = (R - L) * (Bv - T);
    // The right-pointing triangle (0,0)(1,0.5)(0,1) is exactly half the cell.
    float frac = painted / cellArea;
    const bool isAboutHalf = frac > 0.40f && frac < 0.60f;
    CHECK_MSG(isAboutHalf, "U+E0B0 paints about half its cell, a solid wedge");
    // Solid: every device row in the cell band has at least one painted pixel
    // (no dropped scanlines → no horizontal gaps), and the left edge (the full-
    // height side of the wedge) is inked top-to-bottom.
    const int leftColumn = static_cast<int>(L + 0.5f);
    const int firstRow = static_cast<int>(T);
    const int endRow = static_cast<int>(Bv);
    int rowsWithInk = 0;
    for (int y = firstRow; y < endRow; ++y) {
        const unsigned char* const scanline = cov + y * W;
        bool any = false;
        for (int x = static_cast<int>(L); x < static_cast<int>(R); ++x) {
            if (scanline[x]) {
                any = true;
                break;
            }
        }
        if (any)
            ++rowsWithInk;
        // The wedge's flat side is the cell's left edge, so it is lit on every row.
        CHECK_MSG(scanline[leftColumn] == 255, "the wedge's flat left side is lit from top to bottom");
    }
    CHECK_MSG(rowsWithInk == endRow - firstRow, "no row of the cell is left unpainted");
}

/// The cell-height-derived box-fill budget always covers a scan-converted glyph.
/// A full-cell triangle needs ~cellHeight span quads; multi-triangle glyphs need a
/// few times that. The budget = ceil(cellHeight)*6 + 64 must exceed the worst case so
/// the powerline shapes never truncate (a fixed 72 would, past ~retina).
TEST(testBoxfillBudgetCoversTriangles)
{
    for (int cellHeight = 12; cellHeight <= 80; cellHeight += 4) {
        int budget = ((int)ceilf((float)cellHeight) + 2) * 6 + 64;
        if (budget < 72)
            budget = 72;
        // The fallback half-disc is four triangles, each needing no more than
        // one span per device row.
        const int fourTriangleSpans = 4 * (cellHeight + 1);
        CHECK_MSG(budget >= fourTriangleSpans, "the budget holds a full-cell glyph made of four triangles");
    }
}

// Every code point covers() claims decomposes into primitives that stay on the
// unit cell. Coverage of the per-code-point cases comes from the sweep; the
// exact drawings belong to the golden tier (test_boxglyph_golden), so nothing
// here restates a table.
TEST(testEveryCoveredCodePointDecomposesWithinTheCell)
{
    static constexpr struct {
        unsigned first;
        unsigned last;
    } ranges[] = {
        { 0x2500, 0x257F }, // box drawing
        { 0x2580, 0x259F }, // block elements + quadrants
        { 0x25E2, 0x25E5 }, // corner triangles
        { 0x2571, 0x2573 }, // diagonals
        { 0x1FB00, 0x1FBFF }, // legacy computing
        { 0xE0B0, 0xE0BF }, // analytic Powerline separators
    };
    int decomposed = 0;
    for (const auto& range : ranges) {
        for (unsigned codePoint = range.first; codePoint <= range.last; ++codePoint) {
            if (!covers(codePoint))
                continue;
            Glyph g;
            const int count = decompose(codePoint, &g);
            if (count == 0)
                continue; // covered ranges may hold font-delegated members (dashes)
            decomposed++;
            CHECK_MSG(count <= maxPrimitives, "the primitive budget holds");
            for (int i = 0; i < count; ++i) {
                const Primitive& p = g.prim[i];
                const float low = -0.001f;
                const float high = 1.001f;
                CHECK_MSG(p.x0 >= low && p.x0 <= high && p.y0 >= low && p.y0 <= high,
                    "a primitive's first corner is on the unit cell");
                if (p.kind == PrimitiveKind::Rect || p.kind == PrimitiveKind::Diagonal
                    || p.kind == PrimitiveKind::Triangle || p.kind == PrimitiveKind::Stipple) {
                    CHECK_MSG(p.x1 >= low && p.x1 <= high && p.y1 >= low && p.y1 <= high,
                        "and so is its second");
                }
                if (p.kind == PrimitiveKind::Triangle)
                    CHECK_MSG(p.x2 >= low && p.x2 <= high && p.y2 >= low && p.y2 <= high,
                        "and a triangle's third");
                CHECK_MSG(p.alpha > 0.0f && p.alpha <= 1.0f, "ink is visible and never over unity");
            }
        }
    }
    CHECK_MSG(decomposed > 300, "the sweep reached the bulk of the covered set");
}

// The corner-to-edge diagonals U+1FBD0..1FBDF: each is one or two diagonal
// strokes and nothing else — a case that fell through to a rect or a stub
// would tile wrongly against its neighbours.
TEST(testCornerToEdgeDiagonalsAreDiagonalsAlone)
{
    for (unsigned codePoint = 0x1FBD0; codePoint <= 0x1FBDF; ++codePoint) {
        Glyph g;
        const int count = decompose(codePoint, &g);
        CHECK_MSG(count >= 1 && count <= 2, "one stroke, or a two-stroke V/chevron");
        for (int i = 0; i < count; ++i)
            CHECK_MSG(g.prim[i].kind == PrimitiveKind::Diagonal, "every primitive is a diagonal");
    }
}

// The quadrant mosaics U+2596..259F resolve to 2x2 masks whose bit count
// matches the character's quadrant count — the semantic the Unicode names
// carry, asserted without restating which bit is which.
TEST(testQuadrantMasksCarryTheirQuadrantCount)
{
    static constexpr struct {
        unsigned codePoint;
        int quadrants;
    } quadrantCharacters[] = {
        { 0x2596, 1 }, { 0x2597, 1 }, { 0x2598, 1 }, { 0x259D, 1 },
        { 0x259A, 2 }, { 0x259E, 2 },
        { 0x2599, 3 }, { 0x259B, 3 }, { 0x259C, 3 }, { 0x259F, 3 }
    };
    for (const auto& c : quadrantCharacters) {
        MosaicMask mask;
        CHECK_MSG(mosaicMaskForCodePoint(c.codePoint, &mask), "the quadrant resolves to a mosaic");
        CHECK_MSG(mask.columns == 2 && mask.rows == 2, "on the 2x2 grid");
        CHECK_MSG(__builtin_popcount(mask.mask) == c.quadrants, "with its own quadrant count lit");
    }
    MosaicMask none;
    CHECK(!mosaicMaskForCodePoint('A', &none));
    CHECK(none.mask == 0 && none.columns == 0 && none.rows == 0);
}

// Every simple Powerline separator resolves to a shape whose parameters are
// unit coordinates. The Extra artwork resolves to none: its silhouette is the
// bundled face's to draw, and a quad approximation here would replace it.
TEST(testPowerlineShapesStayOnTheUnitCell)
{
    int resolved = 0;
    for (unsigned codePoint = 0xE0B0; codePoint <= 0xE0BF; ++codePoint) {
        PowerlineShape shape;
        if (!powerlineShape(codePoint, &shape))
            continue;
        resolved++;
        CHECK_MSG(shape.kind != PowerlineShapeKind::None, "a resolved shape names its kind");
        // Coordinates are unit; the half-disc's bulge-direction parameter is a
        // sign, so the band is [-1, 1.5] with the documented stroke slack.
        for (float parameter : shape.p)
            CHECK_MSG(parameter >= -1.001f && parameter <= 1.501f,
                "parameters are unit coordinates, a sign, or the stroke slack");
    }
    CHECK_MSG(resolved == 16, "the analytic set is exactly U+E0B0–U+E0BF");
}

UBIQUITTY_TEST_MAIN()
