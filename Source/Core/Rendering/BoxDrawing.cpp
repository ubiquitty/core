// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// BoxDrawing.cpp — unit-coordinate decomposition of box-drawing, block,
// and legacy-computing glyphs. See BoxDrawing.h for the model.
//
// Coverage:
// U+1CD00–1CDE5 octants (Legacy Computing Supplement): 2×4 sub-cell fills.
// U+2500–257F  box drawing: light/heavy/double lines, dashes, corners
// (incl. rounded U+256D–2570), tees, crosses, diagonals.
// U+2580–259F  block elements: half / eighth blocks, quadrants, shades.
// U+1FB00–1FBFF Symbols for Legacy Computing: sextants, wedge / triangle
// diagonals, eighth blocks, fills/shades/checker, two-thirds blocks,
// centred quarters, box-drawing light diagonals.
// Left to the font path: the pictographic legacy symbols (stick figures,
// arrows, circle slices, segmented digits).

#include "Core/Rendering/BoxDrawing.h"

#include <array>

namespace Ubiquitty::Rendering::BoxDrawing {

namespace {

using W = Weight;
using D = Direction;

void addStub(Glyph* g, Direction direction, Weight w, unsigned char dashes = 0, float inset = 0.0f)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Stub;
    p.weight = w;
    p.direction = direction;
    p.x0 = p.y0 = p.x1 = p.y1 = 0.0f;
    p.x2 = p.y2 = 0.0f;
    p.alpha = 1.0f;
    p.dashes = dashes;
    p.inset = inset;
}

// A quarter-circle stroke centred at (cx,cy) with unit-x radius r. The
// renderer derives the swept quadrant from the centre's position relative
// to the cell centre (0.5,0.5).
void addArc(Glyph* g, float cx, float cy, float r, Weight w)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Arc;
    p.weight = w;
    p.direction = Direction::Up;
    p.x0 = cx;
    p.y0 = cy;
    p.x1 = r;
    p.y1 = 0.0f;
    p.x2 = p.y2 = 0.0f;
    p.alpha = 1.0f;
    p.dashes = 0;
    p.inset = 0.0f;
}

void addRect(Glyph* g, float x0, float y0, float x1, float y1, float alpha = 1.0f)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Rect;
    p.weight = W::Light;
    p.direction = D::Up;
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    p.x2 = p.y2 = 0.0f;
    p.alpha = alpha;
    p.dashes = 0;
    p.inset = 0.0f;
}

void addDiagonal(Glyph* g, float x0, float y0, float x1, float y1)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Diagonal;
    p.weight = W::Light;
    p.direction = D::Up;
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    p.x2 = p.y2 = 0.0f;
    p.alpha = 1.0f;
    p.dashes = 0;
    p.inset = 0.0f;
}

void addTriangle(Glyph* g, float x0, float y0, float x1, float y1, float x2, float y2)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Triangle;
    p.weight = W::Light;
    p.direction = D::Up;
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    p.x2 = x2;
    p.y2 = y2;
    p.alpha = 1.0f;
    p.dashes = 0;
    p.inset = 0.0f;
}

// Ordered-dither fill of [x0,y0,x1,y1] at `density` (0..1 fraction of foreground
// pixels). The renderer stamps a checkerboard of solid foreground, so the cell
// background shows through — the shade glyphs and medium-shade variants.
void addStipple(Glyph* g, float x0, float y0, float x1, float y1, float density)
{
    if (g->count >= maxPrimitives)
        return;
    Primitive& p = g->prim[g->count++];
    p.kind = PrimitiveKind::Stipple;
    p.weight = W::Light;
    p.direction = D::Up;
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    p.x2 = p.y2 = 0.0f;
    p.alpha = density;
    p.dashes = 0;
    p.inset = 0.0f;
    p.polarity = ShadePolarity::Normal;
}

// The medium-shade field's complement over [x0,y0,x1,y1]: the pixels ▒ leaves
// dark, so an inverse shade beside a ▒ fills the field rather than repeating it.
void addInverseStipple(Glyph* g, float x0, float y0, float x1, float y1)
{
    addStipple(g, x0, y0, x1, y1, 0.5f);
    if (g->count > 0 && g->prim[g->count - 1].kind == PrimitiveKind::Stipple)
        g->prim[g->count - 1].polarity = ShadePolarity::Inverse;
}

// ---- Box drawing U+2500..257F ----------------------------------------

// Encode up/down/left/right weights into one descriptor; 0 = no stub,
// 1 = light, 2 = heavy, 3 = double.
struct Edges {
    unsigned char up, down, left, right;
};

Weight weightOf(unsigned char e) { return e == 3 ? W::Double : e == 2 ? W::Heavy
                                                                      : W::Light; }

void emitEdges(Glyph* g, Edges e, unsigned char dashes = 0)
{
    if (e.up)
        addStub(g, D::Up, weightOf(e.up), dashes);
    if (e.down)
        addStub(g, D::Down, weightOf(e.down), dashes);
    if (e.left)
        addStub(g, D::Left, weightOf(e.left), dashes);
    if (e.right)
        addStub(g, D::Right, weightOf(e.right), dashes);
}

// Edge descriptors for U+2500..U+257F, indexed by (codePoint - 0x2500).
// L = light, H = heavy, B = double. An all-zero row is a codePoint this
// table doesn't model: the dashed (2504..250B), rounded (256D..2570),
// and diagonal (2571..2573) glyphs are decomposed as special cases by
// decomposeBox.
constexpr unsigned char L = 1, H = 2, B = 3, _ = 0;
constexpr Edges boxEdgeTable[0x80] = {
    // Horizontal / vertical lines.
    { _, _, L, L }, // 2500 light horizontal
    { _, _, H, H }, // 2501 heavy horizontal
    { L, L, _, _ }, // 2502 light vertical
    { H, H, _, _ }, // 2503 heavy vertical
    { _, _, _, _ }, // 2504 (not edge-modelled)
    { _, _, _, _ }, // 2505 (not edge-modelled)
    { _, _, _, _ }, // 2506 (not edge-modelled)
    { _, _, _, _ }, // 2507 (not edge-modelled)
    { _, _, _, _ }, // 2508 (not edge-modelled)
    { _, _, _, _ }, // 2509 (not edge-modelled)
    { _, _, _, _ }, // 250A (not edge-modelled)
    { _, _, _, _ }, // 250B (not edge-modelled)
    // Corners (light).
    { _, L, _, L }, // 250C down+right
    { _, L, _, H }, // 250D
    { _, H, _, L }, // 250E
    { _, H, _, H }, // 250F heavy down+right
    { _, L, L, _ }, // 2510 down+left
    { _, L, H, _ }, // 2511
    { _, H, L, _ }, // 2512
    { _, H, H, _ }, // 2513
    { L, _, _, L }, // 2514 up+right
    { L, _, _, H }, // 2515
    { H, _, _, L }, // 2516
    { H, _, _, H }, // 2517
    { L, _, L, _ }, // 2518 up+left
    { L, _, H, _ }, // 2519
    { H, _, L, _ }, // 251A
    { H, _, H, _ }, // 251B
    // Vertical + right tees.
    { L, L, _, L }, // 251C
    { L, L, _, H }, // 251D
    { H, L, _, L }, // 251E
    { L, H, _, L }, // 251F
    { H, H, _, L }, // 2520
    { H, L, _, H }, // 2521
    { L, H, _, H }, // 2522
    { H, H, _, H }, // 2523
    // Vertical + left tees.
    { L, L, L, _ }, // 2524
    { L, L, H, _ }, // 2525
    { H, L, L, _ }, // 2526
    { L, H, L, _ }, // 2527
    { H, H, L, _ }, // 2528
    { H, L, H, _ }, // 2529
    { L, H, H, _ }, // 252A
    { H, H, H, _ }, // 252B
    // Horizontal + down tees.
    { _, L, L, L }, // 252C
    { _, L, H, L }, // 252D
    { _, L, L, H }, // 252E
    { _, L, H, H }, // 252F
    { _, H, L, L }, // 2530
    { _, H, H, L }, // 2531
    { _, H, L, H }, // 2532
    { _, H, H, H }, // 2533
    // Horizontal + up tees.
    { L, _, L, L }, // 2534
    { L, _, H, L }, // 2535
    { L, _, L, H }, // 2536
    { L, _, H, H }, // 2537
    { H, _, L, L }, // 2538
    { H, _, H, L }, // 2539
    { H, _, L, H }, // 253A
    { H, _, H, H }, // 253B
    // Crosses.
    { L, L, L, L }, // 253C
    { L, L, H, L }, // 253D
    { L, L, L, H }, // 253E
    { L, L, H, H }, // 253F
    { H, L, L, L }, // 2540
    { L, H, L, L }, // 2541
    { H, H, L, L }, // 2542
    { H, L, H, L }, // 2543
    { H, L, L, H }, // 2544
    { L, H, H, L }, // 2545
    { L, H, L, H }, // 2546
    { H, L, H, H }, // 2547
    { L, H, H, H }, // 2548
    { H, H, H, L }, // 2549
    { H, H, L, H }, // 254A
    { H, H, H, H }, // 254B
    { _, _, _, _ }, // 254C (not edge-modelled)
    { _, _, _, _ }, // 254D (not edge-modelled)
    { _, _, _, _ }, // 254E (not edge-modelled)
    { _, _, _, _ }, // 254F (not edge-modelled)
    // Double lines.
    { _, _, B, B }, // 2550 double horizontal
    { B, B, _, _ }, // 2551 double vertical
    { _, L, _, B }, // 2552 down single, right double
    { _, B, _, L }, // 2553
    { _, B, _, B }, // 2554 double down+right
    { _, L, B, _ }, // 2555
    { _, B, L, _ }, // 2556
    { _, B, B, _ }, // 2557 double down+left
    { L, _, _, B }, // 2558
    { B, _, _, L }, // 2559
    { B, _, _, B }, // 255A double up+right
    { L, _, B, _ }, // 255B
    { B, _, L, _ }, // 255C
    { B, _, B, _ }, // 255D double up+left
    { L, L, _, B }, // 255E
    { B, B, _, L }, // 255F
    { B, B, _, B }, // 2560 double vertical+right
    { L, L, B, _ }, // 2561
    { B, B, L, _ }, // 2562
    { B, B, B, _ }, // 2563 double vertical+left
    { _, L, B, B }, // 2564
    { _, B, L, L }, // 2565
    { _, B, B, B }, // 2566 double horizontal+down
    { L, _, B, B }, // 2567
    { B, _, L, L }, // 2568
    { B, _, B, B }, // 2569 double horizontal+up
    { L, L, B, B }, // 256A
    { B, B, L, L }, // 256B
    { B, B, B, B }, // 256C double cross
    { _, _, _, _ }, // 256D (not edge-modelled)
    { _, _, _, _ }, // 256E (not edge-modelled)
    { _, _, _, _ }, // 256F (not edge-modelled)
    { _, _, _, _ }, // 2570 (not edge-modelled)
    { _, _, _, _ }, // 2571 (not edge-modelled)
    { _, _, _, _ }, // 2572 (not edge-modelled)
    { _, _, _, _ }, // 2573 (not edge-modelled)
    // Half lines (stub to one edge only).
    { _, _, L, _ }, // 2574 light left
    { L, _, _, _ }, // 2575 light up
    { _, _, _, L }, // 2576 light right
    { _, L, _, _ }, // 2577 light down
    { _, _, H, _ }, // 2578 heavy left
    { H, _, _, _ }, // 2579 heavy up
    { _, _, _, H }, // 257A heavy right
    { _, H, _, _ }, // 257B heavy down
    // Mixed-weight half-and-half lines.
    { _, _, L, H }, // 257C light left, heavy right
    { L, H, _, _ }, // 257D light up, heavy down
    { _, _, H, L }, // 257E heavy left, light right
    { H, L, _, _ }, // 257F heavy up, light down

};

bool boxEdges(unsigned codePoint, Edges* out)
{
    if (codePoint < 0x2500 || codePoint > 0x257F)
        return false;
    const Edges& e = boxEdgeTable[codePoint - 0x2500];
    if (!e.up && !e.down && !e.left && !e.right)
        return false;
    *out = e;
    return true;
}

bool decomposeBox(unsigned codePoint, Glyph* g)
{
    // Dashed lines: same stubs as solid, plus a dash count.
    switch (codePoint) {
    case 0x2504:
        emitEdges(g, { 0, 0, 1, 1 }, 3);
        return true; // light triple dash horiz
    case 0x2505:
        emitEdges(g, { 0, 0, 2, 2 }, 3);
        return true; // heavy
    case 0x2506:
        emitEdges(g, { 1, 1, 0, 0 }, 3);
        return true; // light triple dash vert
    case 0x2507:
        emitEdges(g, { 2, 2, 0, 0 }, 3);
        return true; // heavy
    case 0x2508:
        emitEdges(g, { 0, 0, 1, 1 }, 4);
        return true; // light quadruple dash horiz
    case 0x2509:
        emitEdges(g, { 0, 0, 2, 2 }, 4);
        return true; // heavy
    case 0x250A:
        emitEdges(g, { 1, 1, 0, 0 }, 4);
        return true; // light quadruple dash vert
    case 0x250B:
        emitEdges(g, { 2, 2, 0, 0 }, 4);
        return true; // heavy
    // Double dash variants (U+254C..254F).
    case 0x254C:
        emitEdges(g, { 0, 0, 1, 1 }, 2);
        return true; // light double dash horiz
    case 0x254D:
        emitEdges(g, { 0, 0, 2, 2 }, 2);
        return true; // heavy
    case 0x254E:
        emitEdges(g, { 1, 1, 0, 0 }, 2);
        return true; // light double dash vert
    case 0x254F:
        emitEdges(g, { 2, 2, 0, 0 }, 2);
        return true; // heavy
    default:
        break;
    }

    // Rounded corners meet the adjacent strokes at the cell edges.
    {
        const float r = 0.5f; // unit radius = half the cell
        switch (codePoint) {
        case 0x256D: // arc down+right (bulge top-left)
            addArc(g, 0.5f + r, 0.5f + r, r, W::Light);
            return true;
        case 0x256E: // arc down+left (bulge top-right)
            addArc(g, 0.5f - r, 0.5f + r, r, W::Light);
            return true;
        case 0x256F: // arc up+left (bulge bottom-right)
            addArc(g, 0.5f - r, 0.5f - r, r, W::Light);
            return true;
        case 0x2570: // arc up+right (bulge bottom-left)
            addArc(g, 0.5f + r, 0.5f - r, r, W::Light);
            return true;
        default:
            break;
        }
    }

    // Diagonals.
    switch (codePoint) {
    case 0x2571:
        addDiagonal(g, 0.0f, 1.0f, 1.0f, 0.0f);
        return true; // /
    case 0x2572:
        addDiagonal(g, 0.0f, 0.0f, 1.0f, 1.0f);
        return true; // backslash
    case 0x2573:
        addDiagonal(g, 0.0f, 1.0f, 1.0f, 0.0f);
        addDiagonal(g, 0.0f, 0.0f, 1.0f, 1.0f);
        return true; // X
    default:
        break;
    }

    Edges e;
    if (boxEdges(codePoint, &e)) {
        emitEdges(g, e);
        return true;
    }
    return false;
}

// ---- Block elements U+2580..259F -------------------------------------

bool decomposeBlock(unsigned codePoint, Glyph* g)
{
    switch (codePoint) {
    case 0x2580:
        addRect(g, 0, 0, 1, 0.5f);
        return true; // upper half
    // Lower one-eighth .. full block (2581..2588).
    case 0x2581:
        addRect(g, 0, 7.0f / 8, 1, 1);
        return true;
    case 0x2582:
        addRect(g, 0, 6.0f / 8, 1, 1);
        return true;
    case 0x2583:
        addRect(g, 0, 5.0f / 8, 1, 1);
        return true;
    case 0x2584:
        addRect(g, 0, 0.5f, 1, 1);
        return true; // lower half
    case 0x2585:
        addRect(g, 0, 3.0f / 8, 1, 1);
        return true;
    case 0x2586:
        addRect(g, 0, 2.0f / 8, 1, 1);
        return true;
    case 0x2587:
        addRect(g, 0, 1.0f / 8, 1, 1);
        return true;
    case 0x2588:
        addRect(g, 0, 0, 1, 1);
        return true; // full block
    // Left seven-eighths .. one-eighth (2589..258F).
    case 0x2589:
        addRect(g, 0, 0, 7.0f / 8, 1);
        return true;
    case 0x258A:
        addRect(g, 0, 0, 6.0f / 8, 1);
        return true;
    case 0x258B:
        addRect(g, 0, 0, 5.0f / 8, 1);
        return true;
    case 0x258C:
        addRect(g, 0, 0, 0.5f, 1);
        return true; // left half
    case 0x258D:
        addRect(g, 0, 0, 3.0f / 8, 1);
        return true;
    case 0x258E:
        addRect(g, 0, 0, 2.0f / 8, 1);
        return true;
    case 0x258F:
        addRect(g, 0, 0, 1.0f / 8, 1);
        return true;
    case 0x2590:
        addRect(g, 0.5f, 0, 1, 1);
        return true; // right half
    // Shades: an ordered-dither checkerboard of solid foreground over the cell
    // background (light/medium/dark = 25/50/75% foreground pixels), not a flat tint.
    case 0x2591:
        addStipple(g, 0, 0, 1, 1, 0.25f);
        return true;
    case 0x2592:
        addStipple(g, 0, 0, 1, 1, 0.50f);
        return true;
    case 0x2593:
        addStipple(g, 0, 0, 1, 1, 0.75f);
        return true;
    // Right eighth blocks.
    case 0x2594:
        addRect(g, 0, 0, 1, 1.0f / 8);
        return true; // upper one-eighth
    case 0x2595:
        addRect(g, 7.0f / 8, 0, 1, 1);
        return true; // right one-eighth
    // Quadrants (2596..259F).
    case 0x2596:
        addRect(g, 0, 0.5f, 0.5f, 1);
        return true; // lower left
    case 0x2597:
        addRect(g, 0.5f, 0.5f, 1, 1);
        return true; // lower right
    case 0x2598:
        addRect(g, 0, 0, 0.5f, 0.5f);
        return true; // upper left
    case 0x2599: // UL+LL+LR
        addRect(g, 0, 0, 0.5f, 1);
        addRect(g, 0.5f, 0.5f, 1, 1);
        return true;
    case 0x259A: // UL + LR
        addRect(g, 0, 0, 0.5f, 0.5f);
        addRect(g, 0.5f, 0.5f, 1, 1);
        return true;
    case 0x259B: // UL+UR+LL
        addRect(g, 0, 0, 1, 0.5f);
        addRect(g, 0, 0.5f, 0.5f, 1);
        return true;
    case 0x259C: // UL+UR+LR
        addRect(g, 0, 0, 1, 0.5f);
        addRect(g, 0.5f, 0.5f, 1, 1);
        return true;
    case 0x259D:
        addRect(g, 0.5f, 0, 1, 0.5f);
        return true; // upper right
    case 0x259E: // UR + LL
        addRect(g, 0.5f, 0, 1, 0.5f);
        addRect(g, 0, 0.5f, 0.5f, 1);
        return true;
    case 0x259F: // UR+LL+LR
        addRect(g, 0.5f, 0, 1, 0.5f);
        addRect(g, 0, 0.5f, 1, 1);
        return true;
    default:
        return false;
    }
}

// ---- Sextants U+1FB00..1FB3B (2 columns × 3 rows) -----------------------
//
// The six cells fill in reading order; bit 0 = top-left, bit 1 = top-
// right, bit 2 = mid-left, bit 3 = mid-right, bit 4 = bottom-left, bit 5 =
// bottom-right. Unicode assigns the 60 codePoints to the bit-patterns 1..62
// in ascending order, OMITTING the four that already exist as other glyphs:
// 0 (space), 0b010101=21 (left half U+258C), 0b101010=42 (right half
// U+2590), and 0b111111=63 (full block U+2588).
bool decomposeSextant(unsigned codePoint, Glyph* g)
{
    if (codePoint < 0x1FB00 || codePoint > 0x1FB3B)
        return false;
    int index = static_cast<int>(codePoint - 0x1FB00); // 0..59
    int pattern = index + 1; // pattern 0 (empty) is the space; skip
    if (pattern >= 21)
        pattern += 1; // skip 21 (left half)
    if (pattern >= 42)
        pattern += 1; // skip 42 (right half)

    const float xs[3] = { 0.0f, 0.5f, 1.0f };
    const float ys[4] = { 0.0f, 1.0f / 3, 2.0f / 3, 1.0f };
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 2; ++column) {
            int bit = row * 2 + column;
            if (pattern & (1 << bit))
                addRect(g, xs[column], ys[row], xs[column + 1], ys[row + 1]);
        }
    }
    return g->count > 0;
}

// ---- Octants U+1CD00..1CDE5 (2 columns × 4 rows) ------------------------
//
// The eight cells fill in reading order, same bit layout as the sextants:
// bit = row*2 + column, bit 0 = top-left (LSB). Unicode assigns the 230
// codePoints to the 256 fill patterns in ascending order, OMITTING the 26
// patterns that already exist as other glyphs (so U+1CD00 is OCTANT-3 =
// pattern 0x04, not the single top-left cell): empty (space), full (U+2588),
// the four halves (upper/lower U+2580/2584, left/right U+258C/2590), and the
// twenty whose fill already has a block-element / quadrant (U+2596..259F)
// glyph. octantReservedSet is that exact set (verified against the Unicode 16.0
// chart's BLOCK OCTANT-N names); the non-reserved patterns map to U+1CD00.. in
// ascending order, exactly mirroring the skip-the-aliased-patterns sextant remap.
constexpr unsigned char octantReservedSet[26] = { 0x00, 0x01, 0x02, 0x03, 0x05, 0x0A, 0x0F, 0x14, 0x28, 0x3F, 0x40, 0x50, 0x55, 0x5A, 0x5F, 0x80, 0xA0, 0xA5, 0xAA, 0xAF, 0xC0, 0xF0, 0xF5, 0xFA, 0xFC, 0xFF };

bool octantReserved(int pattern)
{
    for (unsigned char r : octantReservedSet)
        if (pattern == r)
            return true;
    return false;
}

// Map an octant codePoint to its 8-bit fill pattern: the (codePoint-0x1CD00)-th
// pattern in 0..255 ascending that is not reserved.
int octantPattern(unsigned codePoint)
{
    int index = static_cast<int>(codePoint - 0x1CD00); // 0..229
    int seen = 0;
    for (int pattern = 0; pattern < 256; ++pattern) {
        if (octantReserved(pattern))
            continue;
        if (seen == index)
            return pattern;
        ++seen;
    }
    return 0;
}

bool decomposeOctant(unsigned codePoint, Glyph* g)
{
    if (codePoint < 0x1CD00 || codePoint > 0x1CDE5)
        return false;
    const int pattern = octantPattern(codePoint);

    const float xs[3] = { 0.0f, 0.5f, 1.0f };
    const float ys[5] = { 0.0f, 1.0f / 4, 2.0f / 4, 3.0f / 4, 1.0f };
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 2; ++column) {
            int bit = row * 2 + column;
            if (pattern & (1 << bit))
                addRect(g, xs[column], ys[row], xs[column + 1], ys[row + 1]);
        }
    }
    return g->count > 0;
}

// ---- Legacy computing wedges / blocks / fills U+1FB3C..1FBFF ----------
//
// Reference grid (origin top-left, x right, y down), matching the Unicode
// chart's named anchor points:
//   x: left 0, middle-left 1/4, centre 1/2, middle-right 3/4, right 1
//   y: upper 0, middle 1/2, lower 1
// "upper centre" = (1/2,0); "middle left" = (0,1/2); "lower right" = (1,1);
// "upper middle left" = (1/4,0); "lower middle right" = (3/4,1); etc.

// Smooth-mosaic wedges U+1FB3C..1FB67. A 4-row × 3-column '#' grid (the Unicode
// chart's reference cells) defines a boundary polygon over 10 fixed nodes:
// the four corners, the top / bottom edge midpoints, and the third-points on
// the left and right edges. Each glyph's grid is stored as a 12-char mask;
// the polygon is the active nodes taken in a fixed cyclic order (the
// canonical "SmoothMosaic" construction). Fan-triangulated.
struct Point {
    float x, y;
};

// Ten polygon nodes in cyclic order: tl, ul, ll, bl, bc, br, lr, ur, tr, tc.
// Coords: left=0 centre=1/2 right=1; top=0 upper=1/3 lower=2/3 bottom=1.
const Point mosaicNode[10] = {
    { 0.0f, 0.0f }, // tl
    { 0.0f, 1.0f / 3 }, // ul
    { 0.0f, 2.0f / 3 }, // ll
    { 0.0f, 1.0f }, // bl
    { 0.5f, 1.0f }, // bc
    { 1.0f, 1.0f }, // br
    { 1.0f, 2.0f / 3 }, // lr
    { 1.0f, 1.0f / 3 }, // ur
    { 1.0f, 0.0f }, // tr
    { 0.5f, 0.0f }, // tc
};

// Decode the 12-char grid (rows 0..3, columns 0..2; only '#' counts as filled)
// into the 10 active-node booleans (the smooth-mosaic node decoding rule):
// collinear edge-midpoint / third-point nodes are suppressed when both
// neighbouring corner nodes are present (so the polygon stays minimal).
void mosaicNodes(const char* m, std::array<bool, 10>& on)
{
    auto h = [&](int i) { return m[i] == '#'; };
    on[0] = h(0); // tl
    on[1] = h(4) && (!h(0) || !h(8)); // ul
    on[2] = h(8) && (!h(4) || !h(12)); // ll
    on[3] = h(12); // bl
    on[4] = h(13) && (!h(12) || !h(14)); // bc
    on[5] = h(14); // br
    on[6] = h(10) && (!h(14) || !h(6)); // lr
    on[7] = h(6) && (!h(10) || !h(2)); // ur
    on[8] = h(2); // tr
    on[9] = h(1) && (!h(2) || !h(0)); // tc
}

void fillMosaic(Glyph* g, const char* grid)
{
    std::array<bool, 10> on;
    mosaicNodes(grid, on);
    Point poly[10];
    int n = 0;
    for (int i = 0; i < 10; ++i)
        if (on[i])
            poly[n++] = mosaicNode[i];
    for (int i = 1; i + 1 < n; ++i)
        addTriangle(g, poly[0].x, poly[0].y, poly[i].x, poly[i].y, poly[i + 1].x, poly[i + 1].y);
}

bool decomposeWedge(unsigned codePoint, Glyph* g)
{
    // 12-char grids (4 rows of 3), '#'=filled. '/'/'\\' are decoration only
    // and read as empty, matching the reference construction.
    switch (codePoint) {
    case 0x1FB3C:
        fillMosaic(g, "...\n...\n#..\n##.");
        return true;
    case 0x1FB3D:
        fillMosaic(g, "...\n...\n#..\n###");
        return true;
    case 0x1FB3E:
        fillMosaic(g, "...\n#..\n#..\n##.");
        return true;
    case 0x1FB3F:
        fillMosaic(g, "...\n#..\n##.\n###");
        return true;
    case 0x1FB40:
        fillMosaic(g, "#..\n#..\n##.\n##.");
        return true;
    case 0x1FB41:
        fillMosaic(g, ".##\n###\n###\n###");
        return true;
    case 0x1FB42:
        fillMosaic(g, "..#\n###\n###\n###");
        return true;
    case 0x1FB43:
        fillMosaic(g, ".##\n.##\n###\n###");
        return true;
    case 0x1FB44:
        fillMosaic(g, "..#\n.##\n###\n###");
        return true;
    case 0x1FB45:
        fillMosaic(g, ".##\n.##\n.##\n###");
        return true;
    case 0x1FB46:
        fillMosaic(g, "...\n..#\n###\n###");
        return true;
    case 0x1FB47:
        fillMosaic(g, "...\n...\n..#\n.##");
        return true;
    case 0x1FB48:
        fillMosaic(g, "...\n...\n..#\n###");
        return true;
    case 0x1FB49:
        fillMosaic(g, "...\n..#\n..#\n.##");
        return true;
    case 0x1FB4A:
        fillMosaic(g, "...\n..#\n.##\n###");
        return true;
    case 0x1FB4B:
        fillMosaic(g, "..#\n..#\n.##\n.##");
        return true;
    case 0x1FB4C:
        fillMosaic(g, "##.\n###\n###\n###");
        return true;
    case 0x1FB4D:
        fillMosaic(g, "#..\n###\n###\n###");
        return true;
    case 0x1FB4E:
        fillMosaic(g, "##.\n##.\n###\n###");
        return true;
    case 0x1FB4F:
        fillMosaic(g, "#..\n##.\n###\n###");
        return true;
    case 0x1FB50:
        fillMosaic(g, "##.\n##.\n##.\n###");
        return true;
    case 0x1FB51:
        fillMosaic(g, "...\n#..\n###\n###");
        return true;
    case 0x1FB52:
        fillMosaic(g, "###\n###\n###\n.##");
        return true;
    case 0x1FB53:
        fillMosaic(g, "###\n###\n###\n..#");
        return true;
    case 0x1FB54:
        fillMosaic(g, "###\n###\n.##\n.##");
        return true;
    case 0x1FB55:
        fillMosaic(g, "###\n###\n.##\n..#");
        return true;
    case 0x1FB56:
        fillMosaic(g, "###\n.##\n.##\n.##");
        return true;
    case 0x1FB57:
        fillMosaic(g, "##.\n#..\n...\n...");
        return true;
    case 0x1FB58:
        fillMosaic(g, "###\n#..\n...\n...");
        return true;
    case 0x1FB59:
        fillMosaic(g, "##.\n#..\n#..\n...");
        return true;
    case 0x1FB5A:
        fillMosaic(g, "###\n##.\n#..\n...");
        return true;
    case 0x1FB5B:
        fillMosaic(g, "##.\n##.\n#..\n#..");
        return true;
    case 0x1FB5C:
        fillMosaic(g, "###\n###\n#..\n...");
        return true;
    case 0x1FB5D:
        fillMosaic(g, "###\n###\n###\n##.");
        return true;
    case 0x1FB5E:
        fillMosaic(g, "###\n###\n###\n#..");
        return true;
    case 0x1FB5F:
        fillMosaic(g, "###\n###\n##.\n##.");
        return true;
    case 0x1FB60:
        fillMosaic(g, "###\n###\n##.\n#..");
        return true;
    case 0x1FB61:
        fillMosaic(g, "###\n##.\n##.\n##.");
        return true;
    case 0x1FB62:
        fillMosaic(g, ".##\n..#\n...\n...");
        return true;
    case 0x1FB63:
        fillMosaic(g, "###\n..#\n...\n...");
        return true;
    case 0x1FB64:
        fillMosaic(g, ".##\n..#\n..#\n...");
        return true;
    case 0x1FB65:
        fillMosaic(g, "###\n.##\n..#\n...");
        return true;
    case 0x1FB66:
        fillMosaic(g, ".##\n.##\n..#\n..#");
        return true;
    case 0x1FB67:
        fillMosaic(g, "###\n###\n..#\n...");
        return true;
    // ---- Three-quarter triangular blocks (full minus one corner). ---
    case 0x1FB68: // upper+right+lower (left tri cut)
        addTriangle(g, 0, 0, 1, 0, 0.5f, 0.5f);
        addTriangle(g, 1, 0, 1, 1, 0.5f, 0.5f);
        addTriangle(g, 1, 1, 0, 1, 0.5f, 0.5f);
        return true;
    case 0x1FB69: // left+lower+right (upper tri cut)
        addTriangle(g, 0, 0, 0.5f, 0.5f, 0, 1);
        addTriangle(g, 0, 1, 0.5f, 0.5f, 1, 1);
        addTriangle(g, 1, 1, 0.5f, 0.5f, 1, 0);
        return true;
    case 0x1FB6A: // upper+left+lower (right tri cut)
        addTriangle(g, 0, 0, 1, 0, 0.5f, 0.5f);
        addTriangle(g, 0, 0, 0.5f, 0.5f, 0, 1);
        addTriangle(g, 0, 1, 0.5f, 0.5f, 1, 1);
        return true;
    case 0x1FB6B: // left+upper+right (lower tri cut)
        addTriangle(g, 0, 0, 1, 0, 0.5f, 0.5f);
        addTriangle(g, 0, 0, 0.5f, 0.5f, 0, 1);
        addTriangle(g, 1, 0, 1, 1, 0.5f, 0.5f);
        return true;
    // ---- One-quarter triangular blocks (apex at the cell centre). ---
    case 0x1FB6C:
        addTriangle(g, 0, 0, 0.5f, 0.5f, 0, 1);
        return true; // left
    case 0x1FB6D:
        addTriangle(g, 0, 0, 1, 0, 0.5f, 0.5f);
        return true; // upper
    case 0x1FB6E:
        addTriangle(g, 1, 0, 0.5f, 0.5f, 1, 1);
        return true; // right
    case 0x1FB6F:
        addTriangle(g, 0, 1, 0.5f, 0.5f, 1, 1);
        return true; // lower
    default:
        return false;
    }
}

// Vertical / horizontal one-eighth fills and assorted eighth combinations
// U+1FB70..1FB8B, plus two-thirds blocks and centred quarters.
bool decomposeLegacyBlock(unsigned codePoint, Glyph* g)
{
    const float e = 1.0f / 8;
    // Vertical one-eighth block-N: the N-th of eight columns (N=2..7).
    if (codePoint >= 0x1FB70 && codePoint <= 0x1FB75) {
        int n = static_cast<int>(codePoint - 0x1FB70) + 2; // 2..7
        addRect(g, (n - 1) * e, 0, n * e, 1);
        return true;
    }
    // Horizontal one-eighth block-N: the N-th of eight rows (N=2..7).
    if (codePoint >= 0x1FB76 && codePoint <= 0x1FB7B) {
        int n = static_cast<int>(codePoint - 0x1FB76) + 2; // 2..7
        addRect(g, 0, (n - 1) * e, 1, n * e);
        return true;
    }
    switch (codePoint) {
    // L-shaped one-eighth edge pairs.
    case 0x1FB7C: // left + lower
        addRect(g, 0, 0, e, 1);
        addRect(g, 0, 7 * e, 1, 1);
        return true;
    case 0x1FB7D: // left + upper
        addRect(g, 0, 0, e, 1);
        addRect(g, 0, 0, 1, e);
        return true;
    case 0x1FB7E: // right + upper
        addRect(g, 7 * e, 0, 1, 1);
        addRect(g, 0, 0, 1, e);
        return true;
    case 0x1FB7F: // right + lower
        addRect(g, 7 * e, 0, 1, 1);
        addRect(g, 0, 7 * e, 1, 1);
        return true;
    case 0x1FB80: // upper + lower
        addRect(g, 0, 0, 1, e);
        addRect(g, 0, 7 * e, 1, 1);
        return true;
    case 0x1FB81: // horiz eighths 1,3,5,8
        addRect(g, 0, 0, 1, e);
        addRect(g, 0, 2 * e, 1, 3 * e);
        addRect(g, 0, 4 * e, 1, 5 * e);
        addRect(g, 0, 7 * e, 1, 1);
        return true;
    // Upper N-eighths blocks (top band).
    case 0x1FB82:
        addRect(g, 0, 0, 1, 2 * e);
        return true; // upper 1/4
    case 0x1FB83:
        addRect(g, 0, 0, 1, 3 * e);
        return true; // upper 3/8
    case 0x1FB84:
        addRect(g, 0, 0, 1, 5 * e);
        return true; // upper 5/8
    case 0x1FB85:
        addRect(g, 0, 0, 1, 6 * e);
        return true; // upper 3/4
    case 0x1FB86:
        addRect(g, 0, 0, 1, 7 * e);
        return true; // upper 7/8
    // Right N-eighths blocks.
    case 0x1FB87:
        addRect(g, 6 * e, 0, 1, 1);
        return true; // right 1/4
    case 0x1FB88:
        addRect(g, 5 * e, 0, 1, 1);
        return true; // right 3/8
    case 0x1FB89:
        addRect(g, 3 * e, 0, 1, 1);
        return true; // right 5/8
    case 0x1FB8A:
        addRect(g, 2 * e, 0, 1, 1);
        return true; // right 3/4
    case 0x1FB8B:
        addRect(g, e, 0, 1, 1);
        return true; // right 7/8
    // Two-thirds / one-third blocks.
    case 0x1FBCE:
        addRect(g, 0, 0, 2.0f / 3, 1);
        return true; // left 2/3
    case 0x1FBCF:
        addRect(g, 0, 0, 1.0f / 3, 1);
        return true; // left 1/3
    // Centred quarter blocks.
    case 0x1FBE4:
        addRect(g, 0.25f, 0, 0.75f, 0.5f);
        return true; // upper-centre 1/4
    case 0x1FBE5:
        addRect(g, 0.25f, 0.5f, 0.75f, 1);
        return true; // lower-centre 1/4
    case 0x1FBE6:
        addRect(g, 0, 0.25f, 0.5f, 0.75f);
        return true; // middle-left 1/4
    case 0x1FBE7:
        addRect(g, 0.5f, 0.25f, 1, 0.75f);
        return true; // middle-right 1/4
    default:
        return false;
    }
}

// Fills / shades / checker U+1FB8C..1FB9F.
bool decomposeLegacyShade(unsigned codePoint, Glyph* g)
{
    switch (codePoint) {
    case 0x1FB8C:
        addStipple(g, 0, 0, 0.5f, 1, 0.5f);
        return true; // left half med shade
    case 0x1FB8D:
        addStipple(g, 0.5f, 0, 1, 1, 0.5f);
        return true; // right half
    case 0x1FB8E:
        addStipple(g, 0, 0, 1, 0.5f, 0.5f);
        return true; // upper half
    case 0x1FB8F:
        addStipple(g, 0, 0.5f, 1, 1, 0.5f);
        return true; // lower half
    case 0x1FB90:
        addInverseStipple(g, 0, 0, 1, 1);
        return true; // inverse med shade
    case 0x1FB91: // upper solid + lower inverse shade
        addRect(g, 0, 0, 1, 0.5f);
        addInverseStipple(g, 0, 0.5f, 1, 1);
        return true;
    case 0x1FB92: // upper inverse shade + lower solid
        addInverseStipple(g, 0, 0, 1, 0.5f);
        addRect(g, 0, 0.5f, 1, 1);
        return true;
    case 0x1FB94: // left inverse shade + right solid
        addInverseStipple(g, 0, 0, 0.5f, 1);
        addRect(g, 0.5f, 0, 1, 1);
        return true;
    case 0x1FB95:
        addStipple(g, 0, 0, 1, 1, 0.5f);
        return true; // checker board
    case 0x1FB96:
        addStipple(g, 0, 0, 1, 1, 0.5f);
        return true; // inverse checker
    case 0x1FB97: // heavy horizontal fill
        addRect(g, 0, 0.25f, 1, 0.5f);
        addRect(g, 0, 0.75f, 1, 1);
        return true;
    case 0x1FB9A: // upper+lower triangular halves
        addTriangle(g, 0, 0, 1, 0, 0.5f, 0.5f);
        addTriangle(g, 0, 1, 1, 1, 0.5f, 0.5f);
        return true;
    case 0x1FB9B: // left+right triangular halves
        addTriangle(g, 0, 0, 0, 1, 0.5f, 0.5f);
        addTriangle(g, 1, 0, 1, 1, 0.5f, 0.5f);
        return true;
    // Triangular medium shades: the named corner triangle, dithered 50%.
    case 0x1FB9C: // upper-left
        addTriangle(g, 0, 0, 1, 0, 0, 1);
        g->prim[g->count - 1].alpha = 0.5f;
        return true;
    case 0x1FB9D: // upper-right
        addTriangle(g, 0, 0, 1, 0, 1, 1);
        g->prim[g->count - 1].alpha = 0.5f;
        return true;
    case 0x1FB9E: // lower-right
        addTriangle(g, 1, 0, 1, 1, 0, 1);
        g->prim[g->count - 1].alpha = 0.5f;
        return true;
    case 0x1FB9F: // lower-left
        addTriangle(g, 0, 0, 1, 1, 0, 1);
        g->prim[g->count - 1].alpha = 0.5f;
        return true;
    default:
        return false;
    }
}

// Box-drawing light diagonals U+1FBA0..1FBAF and U+1FBD0..1FBDF: thin
// strokes between named edge-midpoints / corners. Stroked, not filled.
bool decomposeLegacyDiagonal(unsigned codePoint, Glyph* g)
{
    // Anchors used by the diagonal names.
    const float UCx = 0.5f, UCy = 0, LCx = 0.5f, LCy = 1;
    const float MLx = 0, MLy = 0.5f, MRx = 1, MRy = 0.5f;
    const float ULx = 0, ULy = 0, URx = 1, URy = 0;
    const float LLx = 0, LLy = 1, LRx = 1, LRy = 1;
    const float MCx = 0.5f, MCy = 0.5f;
    switch (codePoint) {
    case 0x1FBA0:
        addDiagonal(g, UCx, UCy, MLx, MLy);
        return true;
    case 0x1FBA1:
        addDiagonal(g, UCx, UCy, MRx, MRy);
        return true;
    case 0x1FBA2:
        addDiagonal(g, MLx, MLy, LCx, LCy);
        return true;
    case 0x1FBA3:
        addDiagonal(g, MRx, MRy, LCx, LCy);
        return true;
    case 0x1FBA4: // UC-ML-LC (V open right)
        addDiagonal(g, UCx, UCy, MLx, MLy);
        addDiagonal(g, MLx, MLy, LCx, LCy);
        return true;
    case 0x1FBA5: // UC-MR-LC (V open left)
        addDiagonal(g, UCx, UCy, MRx, MRy);
        addDiagonal(g, MRx, MRy, LCx, LCy);
        return true;
    case 0x1FBA6: // ML-LC-MR (down chevron)
        addDiagonal(g, MLx, MLy, LCx, LCy);
        addDiagonal(g, LCx, LCy, MRx, MRy);
        return true;
    case 0x1FBA7: // ML-UC-MR (up chevron)
        addDiagonal(g, MLx, MLy, UCx, UCy);
        addDiagonal(g, UCx, UCy, MRx, MRy);
        return true;
    case 0x1FBA8: // UC-ML and MR-LC
        addDiagonal(g, UCx, UCy, MLx, MLy);
        addDiagonal(g, MRx, MRy, LCx, LCy);
        return true;
    case 0x1FBA9: // UC-MR and ML-LC
        addDiagonal(g, UCx, UCy, MRx, MRy);
        addDiagonal(g, MLx, MLy, LCx, LCy);
        return true;
    case 0x1FBAA: // UC-MR-LC-ML (diamond minus left)
        addDiagonal(g, UCx, UCy, MRx, MRy);
        addDiagonal(g, MRx, MRy, LCx, LCy);
        addDiagonal(g, LCx, LCy, MLx, MLy);
        return true;
    case 0x1FBAB: // UC-ML-LC-MR
        addDiagonal(g, UCx, UCy, MLx, MLy);
        addDiagonal(g, MLx, MLy, LCx, LCy);
        addDiagonal(g, LCx, LCy, MRx, MRy);
        return true;
    case 0x1FBAC: // ML-UC-MR-LC
        addDiagonal(g, MLx, MLy, UCx, UCy);
        addDiagonal(g, UCx, UCy, MRx, MRy);
        addDiagonal(g, MRx, MRy, LCx, LCy);
        return true;
    case 0x1FBAD: // MR-UC-ML-LC
        addDiagonal(g, MRx, MRy, UCx, UCy);
        addDiagonal(g, UCx, UCy, MLx, MLy);
        addDiagonal(g, MLx, MLy, LCx, LCy);
        return true;
    case 0x1FBAE: // diamond UC-MR-LC-ML-UC
        addDiagonal(g, UCx, UCy, MRx, MRy);
        addDiagonal(g, MRx, MRy, LCx, LCy);
        addDiagonal(g, LCx, LCy, MLx, MLy);
        addDiagonal(g, MLx, MLy, UCx, UCy);
        return true;
    case 0x1FBAF: // horizontal w/ vertical stroke
        addStub(g, Direction::Left, Weight::Light);
        addStub(g, Direction::Right, Weight::Light);
        addStub(g, Direction::Up, Weight::Light, 0, 0.5f);
        addStub(g, Direction::Down, Weight::Light, 0, 0.5f);
        return true;
    // Corner-to-edge diagonals U+1FBD0..1FBDF.
    case 0x1FBD0:
        addDiagonal(g, MRx, MRy, LLx, LLy);
        return true;
    case 0x1FBD1:
        addDiagonal(g, URx, URy, MLx, MLy);
        return true;
    case 0x1FBD2:
        addDiagonal(g, ULx, ULy, MRx, MRy);
        return true;
    case 0x1FBD3:
        addDiagonal(g, MLx, MLy, LRx, LRy);
        return true;
    case 0x1FBD4:
        addDiagonal(g, ULx, ULy, LCx, LCy);
        return true;
    case 0x1FBD5:
        addDiagonal(g, UCx, UCy, LRx, LRy);
        return true;
    case 0x1FBD6:
        addDiagonal(g, URx, URy, LCx, LCy);
        return true;
    case 0x1FBD7:
        addDiagonal(g, UCx, UCy, LLx, LLy);
        return true;
    case 0x1FBD8: // UL-MC-UR (down-V)
        addDiagonal(g, ULx, ULy, MCx, MCy);
        addDiagonal(g, MCx, MCy, URx, URy);
        return true;
    case 0x1FBD9: // UR-MC-LR
        addDiagonal(g, URx, URy, MCx, MCy);
        addDiagonal(g, MCx, MCy, LRx, LRy);
        return true;
    case 0x1FBDA: // LL-MC-LR (up-V)
        addDiagonal(g, LLx, LLy, MCx, MCy);
        addDiagonal(g, MCx, MCy, LRx, LRy);
        return true;
    case 0x1FBDB: // UL-MC-LL
        addDiagonal(g, ULx, ULy, MCx, MCy);
        addDiagonal(g, MCx, MCy, LLx, LLy);
        return true;
    case 0x1FBDC: // UL-LC-UR (down chevron)
        addDiagonal(g, ULx, ULy, LCx, LCy);
        addDiagonal(g, LCx, LCy, URx, URy);
        return true;
    case 0x1FBDD: // UR-ML-LR
        addDiagonal(g, URx, URy, MLx, MLy);
        addDiagonal(g, MLx, MLy, LRx, LRy);
        return true;
    case 0x1FBDE: // LL-UC-LR (up chevron)
        addDiagonal(g, LLx, LLy, UCx, UCy);
        addDiagonal(g, UCx, UCy, LRx, LRy);
        return true;
    case 0x1FBDF: // UL-MR-LL
        addDiagonal(g, ULx, ULy, MRx, MRy);
        addDiagonal(g, MRx, MRy, LLx, LLy);
        return true;
    default:
        return false;
    }
}

// The simple Powerline separators U+E0B0..E0BF. Their meaning is the cell-edge
// geometry itself, so they are always drawn procedurally and bleed exactly to
// the cell edges. The icons and complex Powerline Extra artwork stay in the
// bundled Symbols Nerd Font rather than being approximated here.
// The four corner triangles: a cell cut along one diagonal, the half named by
// the character filled.
//
// Cell geometry rather than text, on the same rule as the quadrants beside them
// in every table: a font draws these as small centred marks with bearings, so a
// run of them leaves a gap at every boundary where a reader is drawing a shape.
// The case that shows it puts them next to the blocks and shades, and its own
// doc says all of those fill their cells.
//
// The diagonal each one keeps is the one its glyph is named for. U+25E2 is the
// lower RIGHT triangle, so the kept half is below the leading diagonal, and so
// on around the cell.
//
// - Parameter codePoint: One of U+25E2..U+25E5.
// - Parameter g: The glyph to fill.
void decomposeCornerTriangle(unsigned codePoint, Glyph* g)
{
    switch (codePoint) {
    case 0x25E2: // Black lower right triangle
        addTriangle(g, 1, 0, 1, 1, 0, 1);
        return;
    case 0x25E3: // Black lower left triangle
        addTriangle(g, 0, 0, 1, 1, 0, 1);
        return;
    case 0x25E4: // Black upper left triangle
        addTriangle(g, 0, 0, 1, 0, 0, 1);
        return;
    case 0x25E5: // Black upper right triangle
        addTriangle(g, 0, 0, 1, 0, 1, 1);
        return;
    default:
        return;
    }
}

bool decomposePowerline(unsigned codePoint, Glyph* g)
{
    switch (codePoint) {
    // --- Filled right-pointing separator (classic prompt arrow), edge to edge. ---
    case 0xE0B0:
        addTriangle(g, 0, 0, 1, 0.5f, 0, 1);
        return true;
    // --- Thin right angle: the same chevron as an outline. ---
    case 0xE0B1:
        addDiagonal(g, 0, 0, 1, 0.5f);
        addDiagonal(g, 1, 0.5f, 0, 1);
        return true;
    // --- Filled left-pointing separator. ---
    case 0xE0B2:
        addTriangle(g, 1, 0, 0, 0.5f, 1, 1);
        return true;
    // --- Thin left angle. ---
    case 0xE0B3:
        addDiagonal(g, 1, 0, 0, 0.5f);
        addDiagonal(g, 0, 0.5f, 1, 1);
        return true;
    // --- Rounded half-circle separators E0B4..E0B7: flat side is a full cell
    //     edge (so segments tile); the bulge is a half-disc approximated by a
    //     triangle fan (filled) or polyline (thin), sampled at y = 0..1 by 1/4. ---
    case 0xE0B4: // filled, bulge right
        addTriangle(g, 0, 0, 0.866f, 0.25f, 0, 0.5f);
        addTriangle(g, 0, 0.5f, 0.866f, 0.25f, 1.0f, 0.5f);
        addTriangle(g, 0, 0.5f, 1.0f, 0.5f, 0.866f, 0.75f);
        addTriangle(g, 0, 0.5f, 0.866f, 0.75f, 0, 1);
        return true;
    case 0xE0B5: // thin, bulge right
        addDiagonal(g, 0, 0, 0.866f, 0.25f);
        addDiagonal(g, 0.866f, 0.25f, 1.0f, 0.5f);
        addDiagonal(g, 1.0f, 0.5f, 0.866f, 0.75f);
        addDiagonal(g, 0.866f, 0.75f, 0, 1);
        return true;
    case 0xE0B6: // filled, bulge left
        addTriangle(g, 1, 0, 0.134f, 0.25f, 1, 0.5f);
        addTriangle(g, 1, 0.5f, 0.134f, 0.25f, 0.0f, 0.5f);
        addTriangle(g, 1, 0.5f, 0.0f, 0.5f, 0.134f, 0.75f);
        addTriangle(g, 1, 0.5f, 0.134f, 0.75f, 1, 1);
        return true;
    case 0xE0B7: // thin, bulge left
        addDiagonal(g, 1, 0, 0.134f, 0.25f);
        addDiagonal(g, 0.134f, 0.25f, 0.0f, 0.5f);
        addDiagonal(g, 0.0f, 0.5f, 0.134f, 0.75f);
        addDiagonal(g, 0.134f, 0.75f, 1, 1);
        return true;
    // --- Bottom/top triangles E0B8..E0BF (lower/upper corner wedges). ---
    case 0xE0B8:
        addTriangle(g, 0, 0, 0, 1, 1, 1);
        return true; // filled ◣
    case 0xE0B9: // thin ◣
        addDiagonal(g, 0, 0, 1, 1);
        return true;
    case 0xE0BA:
        addTriangle(g, 1, 0, 1, 1, 0, 1);
        return true; // filled ◢
    case 0xE0BB: // thin ◢
        addDiagonal(g, 1, 0, 0, 1);
        return true;
    case 0xE0BC:
        addTriangle(g, 0, 0, 1, 0, 0, 1);
        return true; // filled ◤
    case 0xE0BD: // thin ◤
        addDiagonal(g, 1, 0, 0, 1);
        return true;
    case 0xE0BE:
        addTriangle(g, 0, 0, 1, 0, 1, 1);
        return true; // filled ◥
    case 0xE0BF: // thin ◥
        addDiagonal(g, 0, 0, 1, 1);
        return true;
    default:
        return false;
    }
}

// Fill a SolidTriangle shape from three unit corners.
void plTri(PowerlineShape* s, float ax, float ay, float bx, float by, float cx, float cy)
{
    s->kind = PowerlineShapeKind::SolidTriangle;
    s->p[0] = ax;
    s->p[1] = ay;
    s->p[2] = bx;
    s->p[3] = by;
    s->p[4] = cx;
    s->p[5] = cy;
    s->p[6] = 0.0f;
    s->p[7] = 0.0f;
}

// strokeHalf for the thin separators, in unit-Y; the renderer scales it to
// device pixels (so it tracks the box-line light weight).
constexpr float plStrokeHalfY = 1.0f / 24.0f; // ≈ half of light (cellHeight/12)

// Fill an OutlineWedge (thin chevron): the solid triangle's corners with b the
// apex. The band runs from the two apex edges inward, so the corners and apex
// stay on the stroke's outer boundary and the cell clip finishes the ends as
// points — the bundled face's own construction.
void plChevron(PowerlineShape* s, float ax, float ay, float bx, float by, float cx, float cy)
{
    s->kind = PowerlineShapeKind::OutlineWedge;
    s->p[0] = ax;
    s->p[1] = ay;
    s->p[2] = bx;
    s->p[3] = by;
    s->p[4] = cx;
    s->p[5] = cy;
    s->p[6] = plStrokeHalfY;
    s->p[7] = 0.0f;
}

// Fill an OutlineDiagonal (thin corner wedge): one stroke along a→b, banded
// inward toward the witness point — a corner of the solid sibling's filled
// side — so the corner-to-corner line is the stroke's outer boundary.
void plDiagonal(PowerlineShape* s, float ax, float ay, float bx, float by, float wx, float wy)
{
    s->kind = PowerlineShapeKind::OutlineDiagonal;
    s->p[0] = ax;
    s->p[1] = ay;
    s->p[2] = bx;
    s->p[3] = by;
    s->p[4] = wx;
    s->p[5] = wy;
    s->p[6] = plStrokeHalfY;
    s->p[7] = 0.0f;
}

// Fill a half-disc: centre (cx,cy), unit radii (rx,ry), sideSign (+1 keeps the
// x≥cx half, −1 keeps x≤cx). outline=false → filled; true → arc stroke.
void plHalfDisc(PowerlineShape* s, float cx, float cy, float rx, float ry, float sideSign, bool outline)
{
    s->kind = outline ? PowerlineShapeKind::OutlineHalfDisc : PowerlineShapeKind::SolidHalfDisc;
    s->p[0] = cx;
    s->p[1] = cy;
    s->p[2] = rx;
    s->p[3] = ry;
    s->p[4] = sideSign;
    s->p[5] = outline ? plStrokeHalfY : 0.0f;
    s->p[6] = 0.0f;
    s->p[7] = 0.0f;
}

} // namespace

bool powerlineShape(unsigned codePoint, PowerlineShape* out)
{
    out->kind = PowerlineShapeKind::None;
    for (float& coordinate : out->p)
        coordinate = 0.0f;
    switch (codePoint) {
    // Right / left solid arrows.
    case 0xE0B0:
        plTri(out, 0, 0, 1, 0.5f, 0, 1);
        return true;
    case 0xE0B2:
        plTri(out, 1, 0, 0, 0.5f, 1, 1);
        return true;
    // Thin right / left angle (open chevron).
    case 0xE0B1:
        plChevron(out, 0, 0, 1, 0.5f, 0, 1);
        return true;
    case 0xE0B3:
        plChevron(out, 1, 0, 0, 0.5f, 1, 1);
        return true;
    // Rounded half-disc separators: flat diameter on a cell edge, bulging
    // toward the far edge. Centre on the flat edge, unit radii (1, 0.5).
    case 0xE0B4:
        plHalfDisc(out, 0, 0.5f, 1.0f, 0.5f, 1.0f, false);
        return true;
    case 0xE0B5:
        plHalfDisc(out, 0, 0.5f, 1.0f, 0.5f, 1.0f, true);
        return true;
    case 0xE0B6:
        plHalfDisc(out, 1, 0.5f, 1.0f, 0.5f, -1.0f, false);
        return true;
    case 0xE0B7:
        plHalfDisc(out, 1, 0.5f, 1.0f, 0.5f, -1.0f, true);
        return true;
    // Corner wedges ◣ ◢ ◤ ◥ (solid) and their thin single-diagonal forms.
    case 0xE0B8:
        plTri(out, 0, 0, 0, 1, 1, 1);
        return true;
    case 0xE0BA:
        plTri(out, 1, 0, 1, 1, 0, 1);
        return true;
    case 0xE0BC:
        plTri(out, 0, 0, 1, 0, 0, 1);
        return true;
    case 0xE0BE:
        plTri(out, 0, 0, 1, 0, 1, 1);
        return true;
    case 0xE0B9:
        plDiagonal(out, 0, 0, 1, 1, 0, 1);
        return true;
    case 0xE0BB:
        plDiagonal(out, 1, 0, 0, 1, 1, 1);
        return true;
    case 0xE0BD:
        plDiagonal(out, 1, 0, 0, 1, 0, 0);
        return true;
    case 0xE0BF:
        plDiagonal(out, 0, 0, 1, 1, 1, 0);
        return true;
    default:
        return false;
    }
}

bool analyticCellShape(unsigned codePoint, PowerlineShape* out)
{
    // The corner triangles resolve through the wedge rather than as cases of
    // `powerlineShape`, which keeps that function's shape under the PGO profile.
    switch (codePoint) {
    case 0x25E2: // Black lower right triangle
        return powerlineShape(0xE0BA, out);
    case 0x25E3: // Black lower left triangle
        return powerlineShape(0xE0B8, out);
    case 0x25E4: // Black upper left triangle
        return powerlineShape(0xE0BC, out);
    case 0x25E5: // Black upper right triangle
        return powerlineShape(0xE0BE, out);
    default:
        return powerlineShape(codePoint, out);
    }
}

bool isPowerlineGlyph(unsigned codePoint)
{
    return (codePoint >= 0xE0A0 && codePoint <= 0xE0A3)
        || (codePoint >= 0xE0B0 && codePoint <= 0xE0C8)
        || codePoint == 0xE0CA
        || (codePoint >= 0xE0CC && codePoint <= 0xE0D2)
        || codePoint == 0xE0D4
        || (codePoint >= 0xE0D6 && codePoint <= 0xE0D7);
}

bool isDash(unsigned codePoint)
{
    // Triple / quadruple dashes (2504..250B) and double dashes (254C..254F),
    // both weights, both orientations.
    return (codePoint >= 0x2504 && codePoint <= 0x250B) || (codePoint >= 0x254C && codePoint <= 0x254F);
}

bool covers(unsigned codePoint)
{
    if (codePoint < lowestCoveredCodePoint)
        return false;
    // The corner triangles U+25E2..25E5: half-cell fills a reader draws shapes
    // with, and the only members of Geometric Shapes here — the rest of that
    // block is text, and belongs to whatever face the reader configured.
    if (codePoint >= 0x25E2 && codePoint <= 0x25E5)
        return true;
    // Octants U+1CD00..1CDE5: 2×4 sub-cell fills, no font nuance, always drawn.
    if (codePoint >= 0x1CD00 && codePoint <= 0x1CDE5)
        return true;
    if (codePoint >= 0x2500 && codePoint <= 0x257F) {
        // The whole range, dashes included. A dash still renders from the
        // font glyph (prefersProcedural declines it, and the builder demotes
        // it to the text lane when no box font can draw it) and is never
        // edge-stretched (boxReachEdges refuses it — the stretch is what
        // staggers a dash under fractional advance and closes its gaps).
        // Covering it is what puts it in the box FAMILY: the family's
        // centring shift is a uniform whole-pixel translate, and a dash
        // outside the family rides the text baseline instead — under a line
        // spacing the dashed continuation of a solid line then sits rows
        // away from the line it continues.
        return true;
    }
    if (codePoint >= 0x2580 && codePoint <= 0x259F)
        return true;
    // Legacy computing: a probe decompose tells covered (>0) from the
    // pictographic gaps (stick figures, arrows, circle slices, digits).
    if (codePoint >= 0x1FB00 && codePoint <= 0x1FBFF) {
        Glyph g;
        return decompose(codePoint, &g) > 0;
    }
    // Terminal graphics: vertical box lines, the VT100 scan lines and the
    // dentistry corners CLI trees use as elbows. Probe-decomposed like the
    // families below so the covered set and the drawn set cannot drift.
    if (codePoint >= 0x23B8 && codePoint <= 0x23CC) {
        Glyph g;
        return decompose(codePoint, &g) > 0;
    }
    // Powerline PUA: same probe — covered codePoints decompose, the rest
    // fall through to the font.
    if (isPowerlineGlyph(codePoint)) {
        Glyph g;
        return decompose(codePoint, &g) > 0;
    }
    return false;
}

bool prefersProcedural(unsigned codePoint)
{
    // An analytic Powerline separator is cell geometry rather than text. Its
    // SDF quad lands on the snapped cell rect, independent of the text pen's
    // fractional phase, and reaches the segment background on every edge.
    if (codePoint >= 0xE0B0 && codePoint <= 0xE0BF) {
        return true;
    }
    // Solid blocks / shades / quadrants and the legacy area-fill families:
    // pure area fills with no font nuance, so draw them procedurally for
    // gapless tiling. Box-drawing lines (2500..257F except the diagonals)
    // keep the font's weight / dashes via edge-stretch (font-first;
    // procedural only when the font lacks the glyph).
    if (codePoint >= 0x25E2 && codePoint <= 0x25E5)
        return true; // corner triangles: half-cell area fills, gapless tiling
    if (codePoint >= 0x1CD00 && codePoint <= 0x1CDE5)
        return true; // octants: pure 2×4 area fills, gapless tiling
    if (codePoint >= 0x2580 && codePoint <= 0x259F)
        return true;
    if (codePoint >= 0x1FB00 && codePoint <= 0x1FB9F)
        return true; // sextants..shades/wedges
    if (codePoint == 0x1FBCE || codePoint == 0x1FBCF)
        return true; // two-thirds / one-third
    if (codePoint >= 0x1FBE4 && codePoint <= 0x1FBE7)
        return true; // centred quarters
    // The diagonals (and the legacy half-diagonal / chevron families): font
    // glyphs neither reach the cell corners nor pixel-align, and unlike the
    // orthogonal lines a diagonal stroke cannot be edge-stretched to meet its
    // neighbours (boxEdges does not model it) — so a run of U+2571/2572 (or a
    // lattice of U+2573) drawn from the font shows a notch at every cell
    // boundary. The procedural capsule runs exactly corner-to-corner of the
    // snapped cell rect, so adjacent cells tile with no gap.
    if (codePoint >= 0x2571 && codePoint <= 0x2573)
        return true;
    if (codePoint >= 0x1FBA0 && codePoint <= 0x1FBAF)
        return true; // legacy half-diagonals / chevron combinations
    if (codePoint >= 0x1FBD0 && codePoint <= 0x1FBDF)
        return true; // corner-to-edge diagonals
    return false;
}

int centerShiftY(int inkMinY, int inkMaxY, int cellTop, int cellBottom)
{
    if (inkMaxY < inkMinY)
        return 0; // no ink
    const int cellCy2 = cellTop + cellBottom; // 2 × cell-box centre
    const int inkCy2 = inkMinY + inkMaxY; // 2 × ink-box centre
    const int diff = cellCy2 - inkCy2;
    // Round half away from zero to a whole row.
    return (diff + (diff >= 0 ? 1 : -1)) / 2;
}

bool takesFamilyCentring(unsigned codePoint)
{
    return !(codePoint >= 0x23B8 && codePoint <= 0x23BD);
}

bool mosaicMaskForCodePoint(unsigned codePoint, MosaicMask* out)
{
    out->mask = 0;
    out->columns = 0;
    out->rows = 0;

    // Sextants U+1FB00..1FB3B: a 2-column × 3-row grid. Unicode assigns the
    // 60 codePoints to bit-patterns 1..62 in ascending order, OMITTING the
    // four that already exist as other glyphs — empty (space), 21 (left half
    // U+258C), 42 (right half U+2590), and 63 (full block U+2588). This is the
    // same index→pattern remap decomposeSextant uses.
    if (codePoint >= 0x1FB00 && codePoint <= 0x1FB3B) {
        int pattern = static_cast<int>(codePoint - 0x1FB00) + 1;
        if (pattern >= 21)
            pattern += 1;
        if (pattern >= 42)
            pattern += 1;
        out->mask = static_cast<unsigned>(pattern);
        out->columns = 2;
        out->rows = 3;
        return true;
    }

    // Quadrants U+2596..259F: a 2-column × 2-row grid (bit 0 = upper-left,
    // 1 = upper-right, 2 = lower-left, 3 = lower-right).
    out->columns = 2;
    out->rows = 2;
    switch (codePoint) {
    case 0x2596:
        out->mask = 1u << 2;
        return true; // lower-left
    case 0x2597:
        out->mask = 1u << 3;
        return true; // lower-right
    case 0x2598:
        out->mask = 1u << 0;
        return true; // upper-left
    case 0x2599:
        out->mask = (1u << 0) | (1u << 2) | (1u << 3);
        return true; // upper-left + lower-left + lower-right
    case 0x259A:
        out->mask = (1u << 0) | (1u << 3);
        return true; // upper-left + lower-right
    case 0x259B:
        out->mask = (1u << 0) | (1u << 1) | (1u << 2);
        return true; // upper-left + upper-right + lower-left
    case 0x259C:
        out->mask = (1u << 0) | (1u << 1) | (1u << 3);
        return true; // upper-left + upper-right + lower-right
    case 0x259D:
        out->mask = 1u << 1;
        return true; // upper-right
    case 0x259E:
        out->mask = (1u << 1) | (1u << 2);
        return true; // upper-right + lower-left
    case 0x259F:
        out->mask = (1u << 1) | (1u << 2) | (1u << 3);
        return true; // upper-right + lower-left + lower-right
    default:
        break;
    }

    out->mask = 0;
    out->columns = 0;
    out->rows = 0;
    return false;
}

// The terminal-graphics family: U+23B8..U+23BD (vertical box lines and the
// horizontal scan lines the VT100 special set drew at scans 1/3/7/9; scan 5
// is U+2500 itself). Every stroke is an EdgeLine at Light weight, so a healed
// character takes the same band the font's own rules measure into the stroke
// profile — the family's whole reason to be procedural is that fonts rarely
// ship it and the system cascade substitutes a hairline from a face that matches
// nothing else on screen.
static void decomposeTerminalGraphics(unsigned codePoint, Glyph* out)
{
    auto vertical = [](float x, float y0, float y1) {
        Primitive p {};
        p.kind = PrimitiveKind::EdgeLine;
        p.weight = Weight::Light;
        p.direction = Direction::Up;
        p.x0 = x;
        p.y0 = y0;
        p.y1 = y1;
        p.alpha = 1.0f;
        return p;
    };
    auto horizontal = [](float y, float x0, float x1) {
        Primitive p {};
        p.kind = PrimitiveKind::EdgeLine;
        p.weight = Weight::Light;
        p.direction = Direction::Right;
        p.y0 = y;
        p.x0 = x0;
        p.x1 = x1;
        p.alpha = 1.0f;
        return p;
    };
    switch (codePoint) {
    case 0x23B8: // left vertical box line
        out->prim[0] = vertical(0.0f, 0.0f, 1.0f);
        out->count = 1;
        break;
    case 0x23B9: // right vertical box line
        out->prim[0] = vertical(1.0f, 0.0f, 1.0f);
        out->count = 1;
        break;
    case 0x23BA: // horizontal scan line 1
        out->prim[0] = horizontal(0.0f, 0.0f, 1.0f);
        out->count = 1;
        break;
    case 0x23BB: // horizontal scan line 3
        out->prim[0] = horizontal(0.25f, 0.0f, 1.0f);
        out->count = 1;
        break;
    case 0x23BC: // horizontal scan line 7
        out->prim[0] = horizontal(0.75f, 0.0f, 1.0f);
        out->count = 1;
        break;
    case 0x23BD: // horizontal scan line 9
        out->prim[0] = horizontal(1.0f, 0.0f, 1.0f);
        out->count = 1;
        break;
    default:
        break;
    }
}

int decompose(unsigned codePoint, Glyph* out)
{
    out->count = 0;
    if (codePoint >= 0x25E2 && codePoint <= 0x25E5) {
        decomposeCornerTriangle(codePoint, out);
    } else if (codePoint >= 0x1CD00 && codePoint <= 0x1CDE5) {
        decomposeOctant(codePoint, out);
    } else if (codePoint >= 0x2500 && codePoint <= 0x257F) {
        decomposeBox(codePoint, out);
    } else if (codePoint >= 0x2580 && codePoint <= 0x259F) {
        decomposeBlock(codePoint, out);
    } else if (codePoint >= 0x1FB00 && codePoint <= 0x1FBFF) {
        if (codePoint <= 0x1FB3B)
            decomposeSextant(codePoint, out);
        else if (codePoint <= 0x1FB6F)
            decomposeWedge(codePoint, out);
        else if (codePoint <= 0x1FB8B)
            decomposeLegacyBlock(codePoint, out);
        else if (codePoint <= 0x1FB9F)
            decomposeLegacyShade(codePoint, out);
        else if (codePoint <= 0x1FBAF)
            decomposeLegacyDiagonal(codePoint, out);
        else if (codePoint == 0x1FBCE || codePoint == 0x1FBCF)
            decomposeLegacyBlock(codePoint, out);
        else if (codePoint >= 0x1FBD0 && codePoint <= 0x1FBDF)
            decomposeLegacyDiagonal(codePoint, out);
        else if (codePoint >= 0x1FBE4 && codePoint <= 0x1FBE7)
            decomposeLegacyBlock(codePoint, out);
        // else pictographic → font.
    } else if (codePoint >= 0xE0B0 && codePoint <= 0xE0BF) {
        decomposePowerline(codePoint, out);
    } else if ((codePoint >= 0x23B8 && codePoint <= 0x23BD)) {
        decomposeTerminalGraphics(codePoint, out);
    }
    return out->count;
}

} // namespace Ubiquitty::Rendering::BoxDrawing
