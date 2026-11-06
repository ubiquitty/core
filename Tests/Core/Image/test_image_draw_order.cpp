// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_image_draw_order.cpp — host tests for the Kitty graphics draw order
// (Source/Core/Image/ImagePlacement.h orderImageQuadsForDrawing).
//
// An ascending sort satisfies most of what these cases assert; which of two
// placements sharing a zIndex ends up on top is the part it does not decide.
// That is observable only when a case puts the equal pair in an incoming order
// the sort would otherwise be free to change, so every case here does.

#include "Core/Image/ImagePlacement.h"

using namespace Ubiquitty::Image;

/// A quad carrying its incoming position in `imageID`, so a reordering can be
/// read back afterwards. Nothing else about the quad matters to the ordering.
static ImageQuad quadAt(std::int32_t zIndex, std::uint32_t marker)
{
    ImageQuad quad {};
    quad.zIndex = zIndex;
    quad.imageID = marker;
    return quad;
}

/// ---- Ascending by zIndex, and the split lands where the text draws. -----
TEST(testOrdersAscendingAndSplitsAtZero)
{
    ImageQuad quads[] = { quadAt(3, 0), quadAt(-2, 1), quadAt(0, 2), quadAt(-1, 3) };
    const int split = orderImageQuadsForDrawing(quads);

    CHECK(quads[0].zIndex == -2);
    CHECK(quads[1].zIndex == -1);
    CHECK(quads[2].zIndex == 0);
    CHECK(quads[3].zIndex == 3);
    // Two negatives, so the text draws after them and before z=0.
    CHECK(split == 2);
}

/// ---- Equal zIndex keeps incoming order: the later one draws on top. -----
///
/// The case an unstable sort passes anyway is one where the equal pair happens
/// to start in the order the sort would produce. These start reversed against
/// their markers, so only a stable pass leaves them 10, 11, 12.
TEST(testEqualDepthKeepsIncomingOrder)
{
    ImageQuad quads[] = { quadAt(5, 10), quadAt(5, 11), quadAt(5, 12) };
    const int split = orderImageQuadsForDrawing(quads);

    CHECK(quads[0].imageID == 10);
    CHECK(quads[1].imageID == 11);
    CHECK(quads[2].imageID == 12);
    CHECK(split == 0); // nothing negative: every placement draws over the text
}

/// ---- Stability holds across a move, not only among untouched neighbours. -
///
/// The equal pair here is separated by a placement that sorts ahead of both, so
/// a correct pass has to shift them and still not swap them. An implementation
/// that shifts on `>=` reverses this pair while ordering the zIndexes just as
/// correctly.
TEST(testEqualDepthSurvivesAShift)
{
    ImageQuad quads[] = { quadAt(2, 20), quadAt(-4, 21), quadAt(2, 22) };
    const int split = orderImageQuadsForDrawing(quads);

    CHECK(quads[0].zIndex == -4);
    CHECK(quads[1].imageID == 20);
    CHECK(quads[2].imageID == 22);
    CHECK(split == 1);
}

/// ---- All under, and all over: the split reaches both ends. --------------
TEST(testSplitReachesBothEnds)
{
    ImageQuad allUnder[] = { quadAt(-1, 30), quadAt(-9, 31) };
    CHECK(orderImageQuadsForDrawing(allUnder) == 2);

    ImageQuad allOver[] = { quadAt(0, 32), quadAt(7, 33) };
    CHECK(orderImageQuadsForDrawing(allOver) == 0);
}

/// ---- Zero is over the text, not under it. ------------------------------
///
/// Pinned on its own because z=0 is the value sixel is given, so a `<= 0`
/// split would put every sixel image behind the cells and still order
/// correctly.
TEST(testZeroDrawsOverTheText)
{
    ImageQuad quads[] = { quadAt(0, 40) };
    CHECK(orderImageQuadsForDrawing(quads) == 0);
}

/// ---- An empty span orders nothing and splits at nothing. ---------------
TEST(testEmptySpanIsSafe)
{
    CHECK(orderImageQuadsForDrawing({}) == 0);
}

UBIQUITTY_TEST_MAIN()
