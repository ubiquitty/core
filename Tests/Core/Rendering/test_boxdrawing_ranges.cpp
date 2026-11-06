// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_boxdrawing_ranges.cpp — decomposes every code point in the blocks
// BoxDrawing serves rather than the representatives the per-glyph tests pick.
//
// A sample leaves whole contiguous runs undecomposed: the dashed box-drawing
// lines, the partial and quadrant blocks, and the Legacy Computing sextants and
// octants are regular enough that a few members stand in for the rest. An arm
// that nothing reaches can draw nothing without any test noticing.
//
// The assertion is weak per code point — covers() and decompose() must
// agree, and a claimed glyph must emit at least one primitive. Pinning exact
// geometry across thousands of code points would freeze the drawing and fail on
// every legitimate tweak; agreement plus a non-empty result still executes every
// arm and catches the failure that matters: a code point the renderer routes to
// the procedural path which then draws nothing, leaving a blank cell where a box
// character belongs.

#include "Harness.h"

#include "Core/Rendering/BoxDrawing.h"

namespace {

using Ubiquitty::Rendering::BoxDrawing::covers;
using Ubiquitty::Rendering::BoxDrawing::decompose;
using Ubiquitty::Rendering::BoxDrawing::Direction;
using Ubiquitty::Rendering::BoxDrawing::Glyph;
using Ubiquitty::Rendering::BoxDrawing::lowestCoveredCodePoint;
using Ubiquitty::Rendering::BoxDrawing::maxPrimitives;
using Ubiquitty::Rendering::BoxDrawing::Primitive;
using Ubiquitty::Rendering::BoxDrawing::PrimitiveKind;

/// An inclusive run of code points and the name a failure reports it under.
struct CodePointBlock {
    unsigned low;
    unsigned high;
    const char* name;
};

// Every range BoxDrawing participates in — whole blocks, not just the arms that
// measured as uncovered: a range that is entirely handled today should stay
// handled, and enumerating the whole block is how a newly-added arm gets
// exercised for free rather than waiting for someone to extend a sample list.
constexpr CodePointBlock blocks[] = {
    { .low = 0x2500, .high = 0x257F, .name = "U+2500..U+257F, Box Drawing" },
    { .low = 0x2580, .high = 0x259F, .name = "U+2580..U+259F, Block Elements" },
    { .low = 0x25E2, .high = 0x25E5, .name = "U+25E2..U+25E5, the corner triangles" },
    { .low = 0x1FB00, .high = 0x1FBAF, .name = "U+1FB00..U+1FBAF, Legacy Computing sextants, octants, and wedges" },
};

/// Whether a primitive carries the geometry its kind needs to draw anything.
///
/// The check is per kind rather than one rule, because zero is a legal
/// coordinate for some kinds and a degenerate shape for others. A decompose
/// that set its count and left the array zeroed would satisfy every bound on
/// the count and still reach the screen as a blank cell where a box character
/// belongs; this is what catches it.
bool hasGeometry(const Primitive& primitive)
{
    switch (primitive.kind) {
    case PrimitiveKind::Stub:
        // Direction and weight are both enums with no absent value, so a Stub
        // has nothing that can be left unset, and its coordinates are unused.
        return true;
    case PrimitiveKind::Arc:
        return primitive.x1 > 0.0f; // x1 is the radius
    case PrimitiveKind::Rect:
    case PrimitiveKind::Stipple:
        return primitive.x1 > primitive.x0 && primitive.y1 > primitive.y0;
    case PrimitiveKind::Diagonal:
        return primitive.x1 != primitive.x0 || primitive.y1 != primitive.y0;
    case PrimitiveKind::Triangle:
        return (primitive.x1 != primitive.x0 || primitive.y1 != primitive.y0)
            && (primitive.x2 != primitive.x0 || primitive.y2 != primitive.y0);
    case PrimitiveKind::EdgeLine:
        // The span along the stroke's axis is the geometry; the centreline may
        // legally sit at zero, on a left or top edge.
        return primitive.direction == Direction::Up ? primitive.y1 > primitive.y0 : primitive.x1 > primitive.x0;
    }
    return false;
}

TEST(testEveryClaimedCodePointDecomposes)
{
    int claimedCount = 0;
    int drawnCount = 0;
    for (const CodePointBlock& block : blocks) {
        for (unsigned codePoint = block.low; codePoint <= block.high; ++codePoint) {
            Glyph decomposition {};
            const int primitiveCount = decompose(codePoint, &decomposition);
            // Only the claimed side is held to anything: covers() is a routing
            // policy and decompose() a capability. A dash decomposes, yet
            // covers() leaves it to the glyph atlas (see isDash in BoxDrawing.h).
            if (!covers(codePoint))
                continue;
            ++claimedCount;
            // The renderer draws a claimed code point procedurally, so a
            // decomposition with no primitives is a blank cell.
            CHECK_MSG(primitiveCount > 0, block.name);
            if (primitiveCount > 0)
                ++drawnCount;
            CHECK_MSG(primitiveCount <= maxPrimitives, block.name);
            for (int index = 0; index < primitiveCount; ++index)
                CHECK_MSG(hasGeometry(decomposition.prim[index]), block.name);
        }
    }

    // A covers() that answered false everywhere would pass the loop above
    // without testing anything, so the blocks must claim a substantial set.
    CHECK_MSG(claimedCount > 200, "the enumerated blocks claim a substantial set");
    CHECK_MSG(drawnCount == claimedCount, "every claimed code point drew at least one primitive");
}

// A decomposition depends on its code point alone. An arm that kept state in a
// static, or that left part of the previous glyph behind, would answer
// differently the second time.
TEST(testDecompositionIsStable)
{
    for (const CodePointBlock& block : blocks) {
        for (unsigned codePoint = block.low; codePoint <= block.high; ++codePoint) {
            Glyph once {};
            Glyph again {};
            const int firstCount = decompose(codePoint, &once);
            const int secondCount = decompose(codePoint, &again);
            CHECK_MSG(firstCount == secondCount, block.name);
        }
    }
}

TEST(theCornerTrianglesAreCellGeometry)
{
    // A cell cut on one diagonal with the named half filled. Claimed and drawn
    // by the renderer, on the rule the quadrants beside them follow: a font
    // draws these as small centred marks with bearings, so a reader building a
    // shape out of them gets a gap at every cell boundary.
    for (unsigned codePoint = 0x25E2; codePoint <= 0x25E5; ++codePoint) {
        Glyph glyph {};
        CHECK_MSG(covers(codePoint), "the corner triangles are claimed");
        CHECK_MSG(Ubiquitty::Rendering::BoxDrawing::prefersProcedural(codePoint), "and drawn even where the font ships a glyph");
        CHECK_MSG(decompose(codePoint, &glyph) == 1, "as one primitive");
        CHECK_MSG(glyph.prim[0].kind == Ubiquitty::Rendering::BoxDrawing::PrimitiveKind::Triangle, "and that primitive is a triangle");
    }

    // Each keeps the half its name gives it, which is the claim a count cannot
    // make: all four decompose to one triangle whichever corners they carry.
    // Read as the two corners away from the diagonal both triangles share.
    Glyph lowerRight {};
    decompose(0x25E2, &lowerRight);
    CHECK_MSG(lowerRight.prim[0].x0 == 1.0f && lowerRight.prim[0].y0 == 0.0f,
        "the lower right triangle reaches the top right corner");
    CHECK_MSG(lowerRight.prim[0].x2 == 0.0f && lowerRight.prim[0].y2 == 1.0f,
        "and the bottom left, so its filled half is below the leading diagonal");

    Glyph upperLeft {};
    decompose(0x25E4, &upperLeft);
    CHECK_MSG(upperLeft.prim[0].x0 == 0.0f && upperLeft.prim[0].y0 == 0.0f,
        "the upper left triangle reaches the top left corner");
    CHECK_MSG(upperLeft.prim[0].x2 == 0.0f && upperLeft.prim[0].y2 == 1.0f,
        "and the bottom left, which is the other side of that diagonal");
}

// Past the edges of every block, and in the text blocks around them, nothing is
// claimed; decomposing there must still be safe, whatever it returns.
TEST(testOutsideRangesIsUnclaimed)
{
    // 0x25A0 and 0x2600 stay out: Geometric Shapes is a text block but for the
    // four corner triangles above, and its rest belongs to the face the reader
    // configured rather than to the renderer.
    constexpr unsigned outside[] = { 0x0041, 0x24FF, 0x25A0, 0x25E1, 0x25E6, 0x2600, 0x1FAFF, 0x1FC00 };
    for (const unsigned codePoint : outside) {
        CHECK_MSG(!covers(codePoint), "a code point outside the blocks is never claimed");
        Glyph discarded {};
        static_cast<void>(decompose(codePoint, &discarded));
    }
}

TEST(testNothingBelowTheLowestCoveredCodePointIsClaimed)
{
    // The builder skips the call below this bound, so a range added under it
    // would draw from the font instead of as geometry.
    for (unsigned codePoint = 0; codePoint < lowestCoveredCodePoint; ++codePoint)
        CHECK_MSG(!covers(codePoint), "every code point below the bound is text");
    CHECK_MSG(covers(lowestCoveredCodePoint), "the bound itself is the first covered code point");
}

} // namespace

UBIQUITTY_TEST_MAIN()
