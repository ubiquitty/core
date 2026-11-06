// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_background_image_layout.cpp — where a backdrop image lands.
//
// Five sizing modes, and two of them crop by narrowing texture coordinates
// rather than by shrinking the quad. That distinction is what a second
// derivation loses: a mode that scales and a mode that windows both "fit the
// image to the view", and reading only the rectangle cannot tell them apart.
// These cases pin the rectangle and the coordinates together for every mode,
// and Center per axis, because an image wider than the view but shorter than it
// is cropped on one axis and centred on the other in the same frame.

#include "Harness.h"

#include "Core/Rendering/BackgroundImageLayout.h"

using namespace Ubiquitty::Rendering;

namespace {

bool near(float actual, float expected)
{
    const float delta = actual - expected;
    return delta > -0.001f && delta < 0.001f;
}

} // namespace

// Stretch is the whole view showing the whole image: the one mode where neither
// the rectangle nor the coordinates depend on the image's shape.
TEST(testStretchFillsTheViewAndSamplesEverything)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Stretch, 800.0f, 600.0f, 100.0f, 400.0f);
    CHECK(near(p.leftInPixels, 0.0f));
    CHECK(near(p.topInPixels, 0.0f));
    CHECK(near(p.rightInPixels, 800.0f));
    CHECK(near(p.bottomInPixels, 600.0f));
    CHECK(near(p.u0, 0.0f) && near(p.v0, 0.0f));
    CHECK(near(p.u1, 1.0f) && near(p.v1, 1.0f));
    CHECK(!p.shouldRepeat);
}

// Fit letterboxes: the whole image is visible, so the coordinates stay full and
// the rectangle shrinks on the axis with slack.
TEST(testFitLetterboxesWithoutCroppingCoordinates)
{
    // 200x100 image into an 800x600 view: width-limited, scale 4, so 800x400
    // centred vertically with 100px bars.
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Fit, 800.0f, 600.0f, 200.0f, 100.0f);
    CHECK(near(p.leftInPixels, 0.0f));
    CHECK(near(p.rightInPixels, 800.0f));
    CHECK(near(p.topInPixels, 100.0f));
    CHECK(near(p.bottomInPixels, 500.0f));
    CHECK(near(p.u0, 0.0f) && near(p.u1, 1.0f));
    CHECK(near(p.v0, 0.0f) && near(p.v1, 1.0f));
}

// Fill covers the view and crops by narrowing the coordinates, leaving the
// rectangle at full view size. Reading only the rectangle cannot distinguish
// this from Stretch, which is why both are pinned.
TEST(testFillCoversTheViewAndCropsByCoordinates)
{
    // 200x100 into 800x600: height-limited, scale 6, cover 1200x600. The visible
    // window is 800/1200 = 2/3 of the width, centred.
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Fill, 800.0f, 600.0f, 200.0f, 100.0f);
    CHECK(near(p.leftInPixels, 0.0f) && near(p.topInPixels, 0.0f));
    CHECK(near(p.rightInPixels, 800.0f) && near(p.bottomInPixels, 600.0f));
    CHECK(near(p.u0, 0.5f - 1.0f / 3.0f));
    CHECK(near(p.u1, 0.5f + 1.0f / 3.0f));
    CHECK(near(p.v0, 0.0f) && near(p.v1, 1.0f));
    CHECK(!p.shouldRepeat);
}

// Tile asks the sampler for repetition by running the coordinates past one, and
// says so rather than leaving a backend to infer it from the numbers.
TEST(testTileRepeatsByCoordinatesPastOne)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Tile, 800.0f, 600.0f, 200.0f, 150.0f);
    CHECK(near(p.rightInPixels, 800.0f) && near(p.bottomInPixels, 600.0f));
    CHECK(near(p.u1, 4.0f));
    CHECK(near(p.v1, 4.0f));
    CHECK(p.shouldRepeat);
}

// Center, smaller than the view on both axes: native size, centred, full
// coordinates.
TEST(testCenterSmallerThanViewIsCentredAtNativeSize)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Center, 800.0f, 600.0f, 200.0f, 100.0f);
    CHECK(near(p.leftInPixels, 300.0f));
    CHECK(near(p.rightInPixels, 500.0f));
    CHECK(near(p.topInPixels, 250.0f));
    CHECK(near(p.bottomInPixels, 350.0f));
    CHECK(near(p.u0, 0.0f) && near(p.u1, 1.0f));
    CHECK(near(p.v0, 0.0f) && near(p.v1, 1.0f));
}

// Center, larger on both axes: the rectangle stays the whole view and both
// coordinate ranges narrow to the visible window.
TEST(testCenterLargerThanViewCropsBothAxes)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Center, 800.0f, 600.0f, 1600.0f, 1200.0f);
    CHECK(near(p.leftInPixels, 0.0f) && near(p.rightInPixels, 800.0f));
    CHECK(near(p.topInPixels, 0.0f) && near(p.bottomInPixels, 600.0f));
    CHECK(near(p.u0, 0.25f) && near(p.u1, 0.75f));
    CHECK(near(p.v0, 0.25f) && near(p.v1, 0.75f));
}

// The case a single branch on total size gets wrong: wider than the view but
// shorter, so one axis crops and the other centres in the same frame.
TEST(testCenterDecidesEachAxisIndependently)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Center, 800.0f, 600.0f, 1600.0f, 300.0f);
    // Wider: rectangle spans the view, coordinates narrow.
    CHECK(near(p.leftInPixels, 0.0f) && near(p.rightInPixels, 800.0f));
    CHECK(near(p.u0, 0.25f) && near(p.u1, 0.75f));
    // Shorter: rectangle centres at native height, coordinates stay full.
    CHECK(near(p.topInPixels, 150.0f) && near(p.bottomInPixels, 450.0f));
    CHECK(near(p.v0, 0.0f) && near(p.v1, 1.0f));
}

// Taller than the view but narrower — the same rule with the axes swapped, so a
// fix applied to one axis cannot pass while the other stays wrong.
TEST(testCenterDecidesEachAxisIndependentlyWithAxesSwapped)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Center, 800.0f, 600.0f, 400.0f, 1200.0f);
    CHECK(near(p.leftInPixels, 200.0f) && near(p.rightInPixels, 600.0f));
    CHECK(near(p.u0, 0.0f) && near(p.u1, 1.0f));
    CHECK(near(p.topInPixels, 0.0f) && near(p.bottomInPixels, 600.0f));
    CHECK(near(p.v0, 0.25f) && near(p.v1, 0.75f));
}

// A zero extent covers nothing rather than dividing by it, so a caller that
// draws the result unconditionally is safe before the image has loaded.
TEST(testNonPositiveExtentsYieldAnEmptyQuad)
{
    for (BackgroundImageMode mode : { BackgroundImageMode::Fill, BackgroundImageMode::Fit,
             BackgroundImageMode::Stretch, BackgroundImageMode::Tile, BackgroundImageMode::Center }) {
        const BackgroundImagePlacement noView = backgroundImagePlacement(mode, 0.0f, 600.0f, 200.0f, 100.0f);
        CHECK(near(noView.rightInPixels, 0.0f) && near(noView.bottomInPixels, 0.0f));
        const BackgroundImagePlacement noImage = backgroundImagePlacement(mode, 800.0f, 600.0f, 200.0f, 0.0f);
        CHECK(near(noImage.rightInPixels, 0.0f) && near(noImage.bottomInPixels, 0.0f));
    }
}

// An exact fit is the boundary between Center's two branches: equal extents take
// the centring arm, which leaves the coordinates full rather than windowing to
// exactly one.
TEST(testCenterAtExactlyViewSizeTakesTheCentringBranch)
{
    const BackgroundImagePlacement p = backgroundImagePlacement(BackgroundImageMode::Center, 800.0f, 600.0f, 800.0f, 600.0f);
    CHECK(near(p.leftInPixels, 0.0f) && near(p.rightInPixels, 800.0f));
    CHECK(near(p.topInPixels, 0.0f) && near(p.bottomInPixels, 600.0f));
    CHECK(near(p.u0, 0.0f) && near(p.u1, 1.0f));
    CHECK(near(p.v0, 0.0f) && near(p.v1, 1.0f));
}

UBIQUITTY_TEST_MAIN()
