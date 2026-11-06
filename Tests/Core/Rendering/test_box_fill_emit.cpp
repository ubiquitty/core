// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_box_fill_emit — box-drawing geometry as overlay quads.
//
// The reason this lane exists instead of the glyph atlas is that neighbouring
// cells must MEET. A font's █ carries bearing and a font's ─ carries side
// bearings, so a run of either shows the gaps; the quads emitted here are
// placed against the cell rect so a row of blocks is one unbroken band.
//
// So the assertions here are mostly about edges rather than about counts. A
// quad one pixel short on the right reads as a stripe every cell width, which
// looks like a font or a scale problem and not like this function; the same
// mistake at every cell simultaneously is the one that gets rationalised.
//
// The caller integer-snaps the cell rect before calling — the header says so,
// and the reason is that snapping afterwards moves a shared edge on one side of
// it only, which reopens the seam this lane exists to close. These cases pass
// pre-snapped rects, which is the contract, and assert that what comes back
// lands exactly on them.

#include "Harness.h"

#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxFillEmit.h"
#include "Core/Rendering/BoxStrokeProfile.h"

#include <cmath>
#include <vector>

using Ubiquitty::Rendering::BoxFillStats;
using Ubiquitty::Rendering::emitBoxFill;
using Ubiquitty::Rendering::OverlayInstance;
using Ubiquitty::Rendering::Vec4;

namespace {

constexpr std::uint32_t fullBlock = 0x2588; // █
constexpr std::uint32_t horizontalLine = 0x2500; // ─
constexpr std::uint32_t verticalLine = 0x2502; // │
constexpr std::uint32_t cross = 0x253C; // ┼
constexpr std::uint32_t lightShade = 0x2591; // ░
constexpr std::uint32_t letterA = 0x0041;

const Vec4 white { 1.0f, 1.0f, 1.0f, 1.0f };

bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// One cell's worth of emission, with room to spare so a capacity clamp is never
// what a geometry case is measuring.
struct Emission {
    std::vector<OverlayInstance> quads { 64 };
    BoxFillStats stats;
    int count = 0;

    Emission(std::uint32_t codePoint, float left, float top, float right, float bottom)
    {
        count = emitBoxFill(codePoint, left, top, right, bottom, white, quads.data(),
            static_cast<int>(quads.size()), nullptr, &stats);
    }

    // The union of every quad, which is what the eye sees as the glyph's extent.
    void bounds(float& minX, float& minY, float& maxX, float& maxY) const
    {
        minX = minY = 1e9f;
        maxX = maxY = -1e9f;
        for (int i = 0; i < count; ++i) {
            minX = std::fmin(minX, quads[static_cast<std::size_t>(i)].topLeftInPixels.x);
            minY = std::fmin(minY, quads[static_cast<std::size_t>(i)].topLeftInPixels.y);
            maxX = std::fmax(maxX, quads[static_cast<std::size_t>(i)].bottomRightInPixels.x);
            maxY = std::fmax(maxY, quads[static_cast<std::size_t>(i)].bottomRightInPixels.y);
        }
    }
};

} // namespace

TEST(boxFillEmit)
{
    // --- A full block is its cell. -----------------------------------------
    //
    // The strictest form of the tiling contract: no inset anywhere, so two of
    // them side by side leave nothing between.
    {
        const Emission block(fullBlock, 8.0f, 16.0f, 16.0f, 32.0f);
        CHECK(block.count > 0);
        float minX, minY, maxX, maxY;
        block.bounds(minX, minY, maxX, maxY);
        CHECK_MSG(approx(minX, 8.0f) && approx(maxX, 16.0f), "a full block spans its cell's full width");
        CHECK_MSG(approx(minY, 16.0f) && approx(maxY, 32.0f), "and its full height");
        CHECK(block.stats.sourceCells == 1);
    }

    // --- Two adjacent cells meet exactly. ----------------------------------
    //
    // The property the whole lane exists for, asserted as the thing a reader
    // would see: the right edge of one cell's ink is the left edge of the
    // next's. A gap here is a visible stripe down every column boundary; an
    // overlap doubles the alpha and reads as a brighter seam.
    {
        const Emission left(fullBlock, 0.0f, 0.0f, 8.0f, 16.0f);
        const Emission right(fullBlock, 8.0f, 0.0f, 16.0f, 16.0f);
        float lx0, ly0, lx1, ly1, rx0, ry0, rx1, ry1;
        left.bounds(lx0, ly0, lx1, ly1);
        right.bounds(rx0, ry0, rx1, ry1);
        CHECK_MSG(approx(lx1, rx0), "one cell's right edge is the next cell's left edge");
        CHECK_MSG(approx(lx0, 0.0f) && approx(rx1, 16.0f), "and the pair covers both cells end to end");
    }

    // --- A rule reaches both cell edges. -----------------------------------
    //
    // A font's ─ stops short by its side bearings, which is why this is drawn
    // rather than delegated. The stroke is thin, so only the SPAN is asserted:
    // the thickness is the decomposition's business and the reach is not.
    {
        const Emission rule(horizontalLine, 0.0f, 0.0f, 8.0f, 16.0f);
        CHECK(rule.count > 0);
        float minX, minY, maxX, maxY;
        rule.bounds(minX, minY, maxX, maxY);
        CHECK_MSG(approx(minX, 0.0f) && approx(maxX, 8.0f), "a horizontal rule reaches both side edges");
        CHECK_MSG(minY > 0.0f && maxY < 16.0f, "and is a stroke rather than a fill");
    }
    {
        const Emission rule(verticalLine, 0.0f, 0.0f, 8.0f, 16.0f);
        CHECK(rule.count > 0);
        float minX, minY, maxX, maxY;
        rule.bounds(minX, minY, maxX, maxY);
        CHECK_MSG(approx(minY, 0.0f) && approx(maxY, 16.0f), "a vertical rule reaches top and bottom");
        CHECK_MSG(minX > 0.0f && maxX < 8.0f, "and is a stroke rather than a fill");
    }

    // --- A shade is counted as a shade. ------------------------------------
    //
    // The stats exist to tell a reader whether a scene's box cost is geometry
    // or shading, so a shade landing in the plain-rect bucket would misreport
    // exactly the thing they are for.
    {
        const Emission shade(lightShade, 0.0f, 0.0f, 8.0f, 16.0f);
        CHECK(shade.count > 0);
        CHECK_MSG(shade.stats.shadeQuads > 0, "a shade block is counted as a shade");
        float minX, minY, maxX, maxY;
        shade.bounds(minX, minY, maxX, maxY);
        CHECK_MSG(approx(minX, 0.0f) && approx(maxX, 8.0f), "and still fills its cell, since it tiles like a block");
    }

    // --- A non-box code point emits nothing. -------------------------------
    //
    // Checked against a buffer pre-filled with a tell-tale rather than zeros: a
    // quad written with zeroed corners is a degenerate quad, and a caller
    // reading a cleared buffer cannot tell that from nothing written at all.
    {
        std::vector<OverlayInstance> quads(4);
        for (auto& quad : quads)
            quad.topLeftInPixels = { -1234.0f, -1234.0f };
        BoxFillStats stats;
        const int needed = emitBoxFill(letterA, 0.0f, 0.0f, 8.0f, 16.0f, white,
            quads.data(), static_cast<int>(quads.size()), nullptr, &stats);
        CHECK_MSG(needed == 0, "a letter has no box geometry");
        CHECK_MSG(approx(quads[0].topLeftInPixels.x, -1234.0f), "and the refusal leaves the caller's buffer alone");
    }

    // --- Capacity stops the writes and never the count. --------------------
    //
    // Zero capacity is the case a `< maxOut` written as `<= maxOut` still
    // passes for every other size, and it is the one that corrupts the caller's
    // memory rather than dropping a quad. The count must come back anyway: it
    // is what sizes the caller's regrow, and a count that stopped at the
    // capacity could never exceed it, which makes every regrow check that
    // compares against the capacity true by construction.
    {
        std::vector<OverlayInstance> quads(8);
        for (auto& quad : quads)
            quad.topLeftInPixels = { -1234.0f, -1234.0f };
        BoxFillStats stats;
        const int needed = emitBoxFill(fullBlock, 0.0f, 0.0f, 8.0f, 16.0f, white,
            quads.data(), 0, nullptr, &stats);
        CHECK_MSG(needed > 0, "a full block reports the quads it needs with no room to write them");
        CHECK_MSG(approx(quads[0].topLeftInPixels.x, -1234.0f), "and nothing is written past the limit");
    }

    // --- A short buffer reports the full need and fills what it has. -------
    //
    // The cross's strokes are more quads than one, so capacity one forces the
    // split this contract exists for: the return names the whole glyph, the
    // buffer holds the first quad, and the slot past the capacity is untouched.
    // Sizing a retry from that return must reproduce the unconstrained count.
    {
        const Emission unconstrained(cross, 0.0f, 0.0f, 8.0f, 16.0f);
        CHECK_MSG(unconstrained.count > 1, "the cross emits more than one quad, or this case measures nothing");

        std::vector<OverlayInstance> quads(2);
        for (auto& quad : quads)
            quad.topLeftInPixels = { -1234.0f, -1234.0f };
        const int needed = emitBoxFill(cross, 0.0f, 0.0f, 8.0f, 16.0f, white,
            quads.data(), 1, nullptr, nullptr);
        CHECK_MSG(needed == unconstrained.count, "the need is the glyph's, not the buffer's");
        CHECK_MSG(!approx(quads[0].topLeftInPixels.x, -1234.0f), "the one slot that exists is filled");
        CHECK_MSG(approx(quads[1].topLeftInPixels.x, -1234.0f), "and the slot past the capacity is untouched");
    }

    // --- No buffer at all still counts. -------------------------------------
    //
    // A caller sizing a first allocation has nothing to hand over yet; the
    // count must not depend on the room this call was given.
    {
        const Emission unconstrained(fullBlock, 0.0f, 0.0f, 8.0f, 16.0f);
        const int needed = emitBoxFill(fullBlock, 0.0f, 0.0f, 8.0f, 16.0f, white,
            nullptr, 64, nullptr, nullptr);
        CHECK_MSG(needed == unconstrained.count, "a null buffer changes nothing about the need");
    }

    // --- Stats accumulate across cells rather than reset. ------------------
    //
    // The caller passes one counter block for a whole frame, so a function that
    // assigned instead of adding would report the last cell as the frame.
    {
        std::vector<OverlayInstance> quads(64);
        BoxFillStats stats;
        emitBoxFill(fullBlock, 0.0f, 0.0f, 8.0f, 16.0f, white, quads.data(), 64, nullptr, &stats);
        emitBoxFill(fullBlock, 8.0f, 0.0f, 16.0f, 16.0f, white, quads.data(), 64, nullptr, &stats);
        CHECK_MSG(stats.sourceCells == 2, "two cells accumulate into one frame's counters");
    }
}

// The two single-quad specials, which resolve before the decomposition: a
// Powerline separator becomes one SDF quad, a Legacy Computing mosaic one
// dither-mask quad. Both were dark — the decomposition cases above never
// reach them.
TEST(boxFillSpecials)
{
    // --- Every analytic Powerline kind emits exactly one SDF quad. ----------
    //
    // 0xE0B0/0xE0B1 are the solid/outline triangles, 0xE0B4/0xE0B5 the solid/
    // outline half-discs. The quad spans the whole cell rect and its shape
    // parameters ride in plA/plB, so shapeKind must be non-zero — a zero would
    // send the fragment stage down the dither path with SDF parameters.
    for (const unsigned codePoint : { 0xE0B0u, 0xE0B1u, 0xE0B4u, 0xE0B5u }) {
        Ubiquitty::Rendering::BoxDrawing::PowerlineShape shape;
        CHECK_MSG(Ubiquitty::Rendering::BoxDrawing::powerlineShape(codePoint, &shape),
            "the separator resolves to an analytic shape");
        std::vector<OverlayInstance> quads(4);
        BoxFillStats stats;
        const int needed = emitBoxFill(codePoint, 0.0f, 0.0f, 8.0f, 16.0f, white,
            quads.data(), 4, &shape, &stats);
        CHECK_MSG(needed == 1, "one analytic quad, not a strip stack");
        CHECK_MSG(approx(quads[0].topLeftInPixels.x, 0.0f) && approx(quads[0].bottomRightInPixels.x, 8.0f),
            "the quad spans the cell");
        CHECK_MSG(quads[0].shapeKind != 0.0f, "the SDF path, not the dither path");
        CHECK_MSG(stats.sourceCells == 1, "the cell counts once");

        // The counting contract holds here too: no buffer, same need.
        CHECK_MSG(emitBoxFill(codePoint, 0.0f, 0.0f, 8.0f, 16.0f, white, nullptr, 0, &shape, nullptr) == 1,
            "a null buffer changes nothing about the need");
    }

    // --- A sextant mosaic becomes one dither-mask quad. ---------------------
    {
        std::vector<OverlayInstance> quads(4);
        BoxFillStats stats;
        const int needed = emitBoxFill(0x1FB00, 0.0f, 0.0f, 8.0f, 16.0f, white,
            quads.data(), 4, nullptr, &stats);
        CHECK_MSG(needed == 1, "one mosaic quad");
        CHECK_MSG(quads[0].shapeKind == -1.0f, "the fragment's mask sampler, not a shape or a strip");
        CHECK_MSG(stats.mosaicQuads == 1, "counted as a mosaic");
        CHECK_MSG(stats.sourceCells == 1, "the cell counts once");
        CHECK_MSG(emitBoxFill(0x1FB00, 0.0f, 0.0f, 8.0f, 16.0f, white, nullptr, 0, nullptr, nullptr) == 1,
            "a null buffer changes nothing about the need");
    }

    // --- A diagonal decomposes to a round-capped capsule: one SDF quad ------
    // per stroke, bleeding past the cell so stepped runs join with no gap.
    {
        std::vector<OverlayInstance> quads(8);
        BoxFillStats stats;
        CHECK_MSG(emitBoxFill(0x2571, 0.0f, 0.0f, 8.0f, 16.0f, white, quads.data(), 8, nullptr, &stats) == 1,
            "one capsule for the rising diagonal");
        CHECK_MSG(quads[0].shapeKind != 0.0f && quads[0].shapeKind != -1.0f, "an SDF stroke");
        CHECK_MSG(emitBoxFill(0x2573, 0.0f, 0.0f, 8.0f, 16.0f, white, quads.data(), 8, nullptr, nullptr) == 2,
            "the X is two capsules");
    }

    // --- A triangular block scan-converts per device row. -------------------
    {
        std::vector<OverlayInstance> quads(64);
        BoxFillStats stats;
        const int needed = emitBoxFill(0x1FB68, 0.0f, 0.0f, 8.0f, 16.0f, white, quads.data(), 64, nullptr, &stats);
        CHECK_MSG(needed > 1, "a triangle is a stack of row spans, not one quad");
        CHECK_MSG(stats.triangleRects > 0, "counted as triangle rows");
    }

    // --- A degenerate rect emits nothing, whatever the special. -------------
    {
        Ubiquitty::Rendering::BoxDrawing::PowerlineShape shape;
        CHECK(Ubiquitty::Rendering::BoxDrawing::powerlineShape(0xE0B0, &shape));
        CHECK_MSG(emitBoxFill(0xE0B0, 8.0f, 16.0f, 8.0f, 16.0f, white, nullptr, 0, &shape, nullptr) == 0,
            "an empty rect has no quad to emit");
    }
}

TEST(boxFillStrokeFollowsTheReferenceHeightNotTheCell)
{
    // A rule's stroke thickness keys off the basis emitBoxFill is handed. The
    // default zero falls back to the cell height, the historical behaviour; a
    // reference below the cell height — the natural line height before the user's
    // spacing widened the cell — draws the same rule thinner, which is what keeps
    // a box rule from thickening as line spacing grows.
    //
    // A tall cell so the two strokes stay clear of any single-pixel floor, and a
    // horizontal rule so the measured extent is the stroke thickness itself.
    const float left = 0.0f, top = 0.0f, right = 16.0f, bottom = 64.0f;
    const float cellHeight = bottom - top;

    auto strokeThickness = [&](float referenceHeight) {
        std::vector<OverlayInstance> quads(64);
        const int count = emitBoxFill(horizontalLine, left, top, right, bottom, white,
            quads.data(), static_cast<int>(quads.size()), nullptr, nullptr, referenceHeight);
        CHECK(count > 0);
        float minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < count; ++i) {
            minY = std::fmin(minY, quads[static_cast<std::size_t>(i)].topLeftInPixels.y);
            maxY = std::fmax(maxY, quads[static_cast<std::size_t>(i)].bottomRightInPixels.y);
        }
        return maxY - minY;
    };

    const float cellBasis = strokeThickness(0.0f); // fallback: the cell height
    const float referenceEqualsCell = strokeThickness(cellHeight); // reference == the cell
    const float referenceQuarterCell = strokeThickness(cellHeight * 0.25f); // a line-spaced cell

    CHECK_MSG(approx(referenceEqualsCell, cellBasis),
        "a reference equal to the cell height reproduces the cell-basis stroke exactly");
    CHECK_MSG(referenceQuarterCell < cellBasis,
        "a reference below the cell height draws the rule thinner, so it holds its weight as spacing grows");
}

TEST(powerlineOutlineStrokeFollowsTheReferenceHeightNotTheCell)
{
    Ubiquitty::Rendering::BoxDrawing::PowerlineShape chevron;
    CHECK(Ubiquitty::Rendering::BoxDrawing::powerlineShape(0xE0B1, &chevron));
    std::vector<OverlayInstance> natural(1);
    std::vector<OverlayInstance> spaced(1);

    CHECK(emitBoxFill(0xE0B1, 0.0f, 0.0f, 8.0f, 16.0f, white,
              natural.data(), 1, &chevron, nullptr, 0.0f)
        == 1);
    CHECK(emitBoxFill(0xE0B1, 0.0f, 0.0f, 8.0f, 32.0f, white,
              spaced.data(), 1, &chevron, nullptr, 16.0f)
        == 1);

    CHECK_MSG(approx(spaced[0].plB.z * 32.0f, natural[0].plB.z * 16.0f),
        "line spacing stretches the separator silhouette without thickening its outline");
    CHECK_MSG(approx(spaced[0].topLeftInPixels.y, 0.0f)
            && approx(spaced[0].bottomRightInPixels.y, 32.0f),
        "the Powerline quad still spans the full spaced cell");
}

/// A fill's partial coverage takes the text composite's curve; its solid
/// coverage does not move.
///
/// A procedural box fill stands in for a glyph the font lacks, and the
/// strokes beside it are font glyphs whose coverage the composite curves. A
/// fill emitted raw lands heavier than the rules it joins by exactly that
/// curve — a healed elbow draws thicker than the vertical it continues, and
/// the knob that thins the font does not reach it.
///
/// Identity is the control: an arm with no curve passes 1.0 and every quad
/// is emitted as its raw fill.

TEST(clippedAnalyticBoxesKeepTheirOriginalShapeCoordinates)
{
    using namespace Ubiquitty::Rendering::BoxDrawing;
    const Ubiquitty::FloatRect clip { { 12.0f, 16.0f }, { 8.0f, 8.0f } };
    for (std::uint32_t codePoint : { 0xE0B0u, 0xE0B1u, 0xE0B4u, 0xE0B5u, 0xE0B9u }) {
        PowerlineShape shape {};
        REQUIRE(analyticCellShape(codePoint, &shape));
        OverlayInstance whole {}, cut {};
        REQUIRE(emitBoxFill(codePoint, 8, 8, 24, 40, white, &whole, 1, &shape) == 1);
        REQUIRE(emitBoxFill(codePoint, 8, 8, 24, 40, white, &cut, 1, &shape, nullptr, 0, nullptr, clip) == 1);
        CHECK(approx(cut.topLeftInPixels.x, 12));
        CHECK(approx(cut.topLeftInPixels.y, 16));
        CHECK(approx(cut.bottomRightInPixels.x, 20));
        CHECK(approx(cut.bottomRightInPixels.y, 24));
        CHECK(approx(8 + whole.plA.x * 16, 12 + cut.plA.x * 8));
        CHECK(approx(8 + whole.plA.y * 32, 16 + cut.plA.y * 8));
        const bool isDisc = shape.kind == PowerlineShapeKind::SolidHalfDisc || shape.kind == PowerlineShapeKind::OutlineHalfDisc;
        if (isDisc) {
            CHECK(approx(whole.plA.z * 16, cut.plA.z * 8));
            CHECK(approx(whole.plA.w * 32, cut.plA.w * 8));
        } else {
            CHECK(approx(8 + whole.plA.z * 16, 12 + cut.plA.z * 8));
            CHECK(approx(8 + whole.plA.w * 32, 16 + cut.plA.w * 8));
            CHECK(approx(whole.plB.z * 32, cut.plB.z * 8));
        }
        CHECK(emitBoxFill(codePoint, 8, 8, 24, 40, white, nullptr, 0, &shape, nullptr, 0, nullptr, clip) == 1);
    }
}

TEST(clippedMosaicsKeepTheirTilePositionsAndCapacityCounts)
{
    const Ubiquitty::FloatRect clip { { 8, 16 }, { 16, 16 } };
    for (std::uint32_t codePoint : { 0x1FB00u, 0x1FB19u, 0x1FB3Bu }) {
        OverlayInstance quads[32] {};
        const int count = emitBoxFill(codePoint, 8, 8, 24, 40, white, quads, 32, nullptr, nullptr, 0, nullptr, clip);
        REQUIRE(count > 0 && count <= 32);
        CHECK(emitBoxFill(codePoint, 8, 8, 24, 40, white, nullptr, 0, nullptr, nullptr, 0, nullptr, clip) == count);
        for (int index = 0; index < count; ++index) {
            CHECK(quads[index].shapeKind == 0);
            CHECK(quads[index].topLeftInPixels.x >= 8 && quads[index].bottomRightInPixels.x <= 24);
            CHECK(quads[index].topLeftInPixels.y >= 16 && quads[index].bottomRightInPixels.y <= 32);
        }
        if (codePoint == 0x1FB00u) {
            REQUIRE(count == 1);
            CHECK(approx(quads[0].bottomRightInPixels.x, 16));
            CHECK(approx(quads[0].bottomRightInPixels.y, 8 + 32.0f / 3.0f));
        }
    }
}

UBIQUITTY_TEST_MAIN()
