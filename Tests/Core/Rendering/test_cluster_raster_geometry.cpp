// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/ClusterRasterGeometry.h"
#include "Core/Rendering/ClusterWindows.h"

#include <array>
#include <cmath>
#include <limits>

using namespace Ubiquitty::Rendering;

TEST(clusterFitEligibilityCoversIndicAndRelatedSyllabicScripts)
{
    constexpr std::array clusters {
        std::array<std::uint32_t, 2> { 0x915, 0x94d },
        std::array<std::uint32_t, 2> { 0x995, 0x9cd },
        std::array<std::uint32_t, 2> { 0xa15, 0xa4d },
        std::array<std::uint32_t, 2> { 0xa95, 0xacd },
        std::array<std::uint32_t, 2> { 0xb15, 0xb4d },
        std::array<std::uint32_t, 2> { 0xb95, 0xbcd },
        std::array<std::uint32_t, 2> { 0xc15, 0xc4d },
        std::array<std::uint32_t, 2> { 0xc95, 0xccd },
        std::array<std::uint32_t, 2> { 0xd15, 0xd4d },
        std::array<std::uint32_t, 2> { 0xd9a, 0xdca },
        std::array<std::uint32_t, 2> { 0xe01, 0xe48 },
        std::array<std::uint32_t, 2> { 0xe81, 0xec8 },
        std::array<std::uint32_t, 2> { 0xf40, 0xf71 },
        std::array<std::uint32_t, 2> { 0x1000, 0x1039 },
        std::array<std::uint32_t, 2> { 0x1780, 0x17d2 },
        std::array<std::uint32_t, 2> { 0x1b13, 0x1b44 },
        std::array<std::uint32_t, 2> { 0xa984, 0xa9c0 },
        std::array<std::uint32_t, 2> { 0x11005, 0x11046 },
        std::array<std::uint32_t, 2> { 0x11083, 0x110b9 },
        std::array<std::uint32_t, 2> { 0x11315, 0x1134d },
        std::array<std::uint32_t, 2> { 0x11415, 0x11442 },
        std::array<std::uint32_t, 2> { 0x11f12, 0x11f41 },
    };
    for (const auto& cluster : clusters) {
        CHECK(clusterHorizontalFitEligible(cluster));
        CHECK(clusterHorizontalFitEligible(cluster.data(), static_cast<int>(cluster.size())));
    }
}

TEST(clusterFitEligibilityPreservesOtherScriptsAndPictographs)
{
    constexpr std::array clusters {
        std::array<std::uint32_t, 2> { 'A', 0x301 },
        std::array<std::uint32_t, 2> { 0x250, 0x35d },
        std::array<std::uint32_t, 2> { 0x3b1, 0x301 },
        std::array<std::uint32_t, 2> { 0x436, 0x301 },
        std::array<std::uint32_t, 2> { 0x628, 0x64e },
        std::array<std::uint32_t, 2> { 0x5d0, 0x5b0 },
        std::array<std::uint32_t, 2> { 0x531, 0x301 },
        std::array<std::uint32_t, 2> { 0x10d0, 0x301 },
        std::array<std::uint32_t, 2> { 0x1200, 0x301 },
        std::array<std::uint32_t, 2> { 0x6f22, 0x301 },
        std::array<std::uint32_t, 2> { 0x1100, 0x1161 },
        std::array<std::uint32_t, 2> { 0x2764, 0x301 },
        std::array<std::uint32_t, 2> { 0x1f469, 0x1f3fd },
        std::array<std::uint32_t, 2> { 0x915, 0x6f22 },
        std::array<std::uint32_t, 2> { 0x915, 0x1f600 },
    };
    for (const auto& cluster : clusters)
        CHECK(!clusterHorizontalFitEligible(cluster));
}

TEST(clusterFitEligibilityUsesTheBaseAfterPrepend)
{
    constexpr std::array indic { 0x600u, 0xd4eu, 0x915u, 0x94du };
    constexpr std::array greek { 0xd4eu, 0x3b1u, 0x301u };
    constexpr std::array prefixOnly { 0x600u, 0xd4eu };
    constexpr std::array prefixAndBase { 0x600u, 0x915u };
    CHECK(clusterHorizontalFitEligible(indic));
    CHECK(!clusterHorizontalFitEligible(greek));
    CHECK(!clusterHorizontalFitEligible(prefixOnly));
    CHECK(!clusterHorizontalFitEligible(prefixAndBase));
}

TEST(clusterFitEligibilityRequiresVisibleMarksAndAValidNarrowBase)
{
    constexpr std::array base { 0x915u };
    constexpr std::array selectors { 0x915u, 0x200du, 0xfe0fu };
    constexpr std::array marked { 0x915u, 0x94du, 0x200du, 0xfe0fu };
    constexpr std::array zeroWidthBase { 0x94du, 0x915u };
    CHECK(!clusterHorizontalFitEligible(std::span<const std::uint32_t> {}));
    CHECK(!clusterHorizontalFitEligible(base));
    CHECK(!clusterHorizontalFitEligible(selectors));
    CHECK(clusterHorizontalFitEligible(marked));
    CHECK(!clusterHorizontalFitEligible(zeroWidthBase));
    CHECK(!clusterHorizontalFitEligible(nullptr, 0));
    CHECK(!clusterHorizontalFitEligible(nullptr, 2));
    CHECK(!clusterHorizontalFitEligible(base.data(), -1));
}

TEST(clusterFitPreservesContainedGlyphPositions)
{
    const auto fit = fitClusterHorizontal(4.25, 11.5, { 4, 8 });
    CHECK_EQ(fit.scaleX, 1);
    CHECK_EQ(fit.translationX, 0);
}

TEST(clusterFitMovesBearingsWithoutEnlargingOrChangingSpacing)
{
    const auto left = fitClusterHorizontal(-2, 4, { 1, 8 });
    CHECK_EQ(left.scaleX, 1);
    CHECK_EQ(left.translationX, 3);
    const auto right = fitClusterHorizontal(9, 15, { 1, 8 });
    CHECK_EQ(right.scaleX, 1);
    CHECK_EQ(right.translationX, -6);
}

TEST(clusterFitContractsTheCompleteGroupIntoItsCellInterval)
{
    const auto fit = fitClusterHorizontal(-2, 18, { 3, 8 });
    CHECK(std::abs(fit.scaleX - 0.4) < 1e-12);
    const auto placed = [&](double source) { return source * fit.scaleX + fit.translationX; };
    CHECK(std::abs(placed(-2) - 3) < 1e-12);
    CHECK(std::abs(placed(18) - 11) < 1e-12);
    CHECK(std::abs((placed(8) - placed(3)) - 2) < 1e-12);
    const auto wide = fitClusterHorizontal(-2, 18, { 3, 16 });
    CHECK(std::abs(wide.scaleX - 0.8) < 1e-12);
}

TEST(clusterFitRejectsInvalidMetricsAndHandlesEmptyInk)
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double maximum = std::numeric_limits<double>::max();
    for (const auto fit : { fitClusterHorizontal(infinity, 8, { 0, 8 }), fitClusterHorizontal(0, infinity, { 0, 8 }),
             fitClusterHorizontal(8, 0, { 0, 8 }), fitClusterHorizontal(0, 8, { 0, 0 }),
             fitClusterHorizontal(0, 8, { infinity, 8 }), fitClusterHorizontal(maximum, maximum, { -maximum, 1 }) }) {
        CHECK_EQ(fit.scaleX, 1);
        CHECK_EQ(fit.translationX, 0);
    }
    const auto empty = fitClusterHorizontal(20, 20, { 0, 8 });
    CHECK_EQ(empty.scaleX, 1);
    CHECK_EQ(empty.translationX, -12);
}

// A cluster that fits its slot takes no window on either side, whether its
// quad is the whole slot or the cell box inside the margins: only ink past
// the rows the own quad draws asks for one.
TEST(aClusterInsideTheRowsItsQuadDrawsTakesNoWindow)
{
    constexpr int slotHeight = 20;
    constexpr int margin = 2;
    const ClusterWindowCounts whole = clusterWindowCounts({ true, 0, slotHeight }, ClusterOwnQuad::WholeSlot, slotHeight, margin);
    CHECK_EQ(whole.above, 0);
    CHECK_EQ(whole.below, 0);
    const ClusterWindowCounts box = clusterWindowCounts({ true, margin, slotHeight - margin }, ClusterOwnQuad::CellBox, slotHeight, margin);
    CHECK_EQ(box.above, 0);
    CHECK_EQ(box.below, 0);
    const ClusterWindowCounts blank = clusterWindowCounts({ false, -40, 60 }, ClusterOwnQuad::WholeSlot, slotHeight, margin);
    CHECK_MSG(blank.above == 0 && blank.below == 0, "a cluster that drew nothing takes nothing");
    const ClusterWindowCounts margins = clusterWindowCounts({ true, 1, slotHeight - 1 }, ClusterOwnQuad::CellBox, slotHeight, margin);
    CHECK_MSG(margins.above == 1 && margins.below == 1, "ink in the margins a baked cell does not draw takes a window each side");
}

// The windows a measured stack takes cover its ink at the window pitch, out
// to the limit and no further.
TEST(aStackTakesWindowsAtThePitchUpToTheLimit)
{
    constexpr int slotHeight = 20;
    constexpr int pitch = slotHeight - 2 * clusterWindowGuardRows;
    CHECK_EQ(clusterWindowPitch(slotHeight), pitch);
    // The own quad keeps rows [1, 19) beside windows; the first window above
    // covers the pitch of rows before row 1.
    const ClusterWindowCounts one = clusterWindowCounts({ true, 1 - pitch, slotHeight }, ClusterOwnQuad::WholeSlot, slotHeight, 0);
    CHECK_EQ(one.above, 1);
    const ClusterWindowCounts two = clusterWindowCounts({ true, -pitch, slotHeight }, ClusterOwnQuad::WholeSlot, slotHeight, 0);
    CHECK_EQ(two.above, 2);
    const ClusterWindowCounts below = clusterWindowCounts({ true, 0, slotHeight + 1 }, ClusterOwnQuad::WholeSlot, slotHeight, 0);
    CHECK_EQ(below.below, 1);
    CHECK_EQ(below.above, 0);
    const ClusterWindowCounts zalgo = clusterWindowCounts({ true, -10 * slotHeight, 11 * slotHeight }, ClusterOwnQuad::WholeSlot, slotHeight, 0);
    CHECK_MSG(zalgo.above == clusterWindowLimitPerSide && zalgo.below == clusterWindowLimitPerSide,
        "a pathological stack is cut at the limit, which bounds what it spends of the sheet");
    CHECK_MSG(clusterWindowReach(slotHeight) >= clusterWindowLimitPerSide * pitch,
        "a measurement looks far enough to count every window the limit allows");
}

// The windows tile one tall drawing: each window's drawn rows end where the
// next one's begin, the first meets the rows the own quad keeps, and every
// window's slot carries a guard row of its neighbour each side.
TEST(windowsTileTheTallDrawingWithAGuardRowEachSide)
{
    constexpr int slotHeight = 20;
    constexpr int margin = 2;
    for (const ClusterOwnQuad quad : { ClusterOwnQuad::WholeSlot, ClusterOwnQuad::CellBox }) {
        const ClusterSlotRows beside = clusterOwnRowsBesideWindows(quad, slotHeight, margin);
        const ClusterSlotRows drawn = clusterWindowRows(slotHeight);
        CHECK_EQ(drawn.top, clusterWindowGuardRows);
        CHECK_EQ(drawn.bottom, slotHeight - clusterWindowGuardRows);
        int edge = beside.top;
        for (int index = 1; index <= clusterWindowLimitPerSide; ++index) {
            const int offset = clusterWindowRowOffset(beside, slotHeight, ClusterWindowSide::Above, index);
            CHECK_MSG(offset + drawn.bottom == edge, "a window above ends where the rows below it begin");
            edge = offset + drawn.top;
        }
        edge = beside.bottom;
        for (int index = 1; index <= clusterWindowLimitPerSide; ++index) {
            const int offset = clusterWindowRowOffset(beside, slotHeight, ClusterWindowSide::Below, index);
            CHECK_MSG(offset + drawn.top == edge, "a window below begins where the rows above it end");
            edge = offset + drawn.bottom;
        }
    }
    CHECK_MSG(clusterOwnRowsBesideWindows(ClusterOwnQuad::WholeSlot, slotHeight, margin).top == clusterWindowGuardRows,
        "a slot-sized quad gives a guard row up to the window above it");
    CHECK_MSG(clusterOwnRowsBesideWindows(ClusterOwnQuad::CellBox, slotHeight, margin).top == margin,
        "a baked cell gives nothing up: the window meets it at the cell's edge");
}

// Only a cluster that might overflow is measured: two marks or more, or ink at
// the slot's edge. An accented letter inside its slot is not.
TEST(onlyAClusterThatMightOverflowIsMeasured)
{
    constexpr int slotHeight = 20;
    const GlyphInkBox inside { { 1.0f, 3.0f }, { 7.0f, 17.0f } };
    const GlyphInkBox atTop { { 1.0f, 0.0f }, { 7.0f, 17.0f } };
    const GlyphInkBox atBottom { { 1.0f, 3.0f }, { 7.0f, 20.0f } };
    CHECK(!clusterMayOverflowItsSlot(2, inside, slotHeight));
    CHECK(clusterMayOverflowItsSlot(3, inside, slotHeight));
    CHECK(clusterMayOverflowItsSlot(2, atTop, slotHeight));
    CHECK(clusterMayOverflowItsSlot(2, atBottom, slotHeight));
}

// A window's quad narrows to the ink its raster measured, and a blank window
// draws nothing; an unmeasured one keeps every row it may draw.
TEST(aWindowQuadNarrowsToItsMeasuredInk)
{
    constexpr int slotHeight = 20;
    const ClusterSlotRows rows = clusterWindowRows(slotHeight);
    const ClusterSlotRows unmeasured = clusterWindowInkRows(rows, unmeasuredGlyphInkBox());
    CHECK(unmeasured.top == rows.top && unmeasured.bottom == rows.bottom);
    const ClusterSlotRows blank = clusterWindowInkRows(rows, emptyGlyphInkBox());
    CHECK(blank.bottom <= blank.top);
    const ClusterSlotRows inked = clusterWindowInkRows(rows, { { 0.0f, 9.0f }, { 8.0f, 20.0f } });
    CHECK_EQ(inked.top, 9);
    CHECK_EQ(inked.bottom, slotHeight - clusterWindowGuardRows);
    CHECK_MSG(clusterWindowWord(-18) > 0x10FFFFu && clusterWindowWord(-18) != clusterWindowWord(18),
        "a window's contents end in a word no cluster holds, distinct per window");
}

UBIQUITTY_TEST_MAIN()
