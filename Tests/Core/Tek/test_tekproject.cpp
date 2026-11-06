// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tekproject.cpp — host tests for the portable Tek 4014 -> device
// projection (Source/Core/Tek/Project.{h,cpp}).
//
// Pins: corner + centre projection, the Y-flip direction, the uniform-
// scale letterbox/pillarbox centring, project/unproject round-trip, the
// four char-cell sizes, and the line-style dash patterns. No platform
// types, links only Tek/Project.cpp.

#include "Core/Tek/BeamRaster.h"
#include "Core/Tek/Project.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>

using namespace Ubiquitty::Tek;

static bool approx(float a, float b) { return fabsf(a - b) < 1e-3f; }

/// Standard Tek tube dims (mirrors tekGridWidth x tekGridHeight).
static const int TW = 4096;
static const int TH = 3120;

/// A projected quantity, the value a case requires of it, and what the
/// requirement means when it fails.
struct Expectation {
    float actual;
    float expected;
    const char* meaning;
};

/// Checks every expectation to the shared tolerance.
static void checkApproximately(std::initializer_list<Expectation> expectations)
{
    for (const Expectation& expectation : expectations)
        CHECK_MSG(approx(expectation.actual, expectation.expected), expectation.meaning);
}

/// ---- 1:1-friendly viewport. A device rect exactly proportional to the
/// Tek space (4096:3120) has zero letterbox; scale is exact. ------------
TEST(testProportionalNoLetterbox)
{
    // Half-scale device rect: 2048 x 1560 keeps the 4096:3120 ratio.
    Projection p = makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f);
    checkApproximately({
        { p.scale, 0.5f, "half the Tek size in each axis is a scale of one half" },
        { p.offsetX, 0.0f, "a rect with Tek's aspect needs no pillars" },
        { p.offsetY, 0.0f, "and no letterbox bars" },
    });
}

/// ---- Alpha glyphs stroke inside their cell. ---------------------------
/// The tube lettered with the beam, and a Glyph primitive that no arm drew
/// left every alpha character invisible: a title typed after US, a GIN
/// prompt, a label beside a plot.
TEST(testGlyphStrokesInsideItsCell)
{
    Projection p = makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f);
    Primitive glyph {};
    glyph.kind = PrimitiveKind::Glyph;
    glyph.charSize = CharSize::Large;
    glyph.x0 = 1000;
    glyph.y0 = 1000;
    glyph.codePoint = 'H';
    StrokeRun runs[maxStrokeRunsPerGlyph];
    const int count = glyphStrokeRuns(p, glyph, runs);
    CHECK_MSG(count == 3, "an H is two uprights and a bar");

    const DevicePoint origin = project(p, 1000, 1000);
    const DeviceSize cell = charCellSize(p, 0);
    for (int i = 0; i < count; ++i) {
        for (const DevicePoint& end : { runs[i].start, runs[i].end }) {
            CHECK_MSG(end.x >= origin.x - 0.01f && end.x <= origin.x + cell.w + 0.01f, "a stroke stays inside the cell's width");
            CHECK_MSG(end.y <= origin.y + 0.01f && end.y >= origin.y - cell.h - 0.01f, "a stroke stays inside the cell's height, above the baseline");
        }
    }

    Primitive space = glyph;
    space.codePoint = ' ';
    CHECK_MSG(glyphStrokeRuns(p, space, runs) == 0, "a space draws nothing");
    Primitive foreign = glyph;
    foreign.codePoint = 0x2603;
    CHECK_MSG(glyphStrokeRuns(p, foreign, runs) == 4, "a code point with no pattern draws the box");
    Primitive line = glyph;
    line.kind = PrimitiveKind::Line;
    CHECK_MSG(glyphStrokeRuns(p, line, runs) == 0, "only a glyph letters");

    // The walk carries the glyph's runs, so a frontend drawing the walk draws text.
    const Primitive list[] = { glyph };
    StrokeRun walked[maxStrokeRunsPerGlyph];
    CHECK_MSG(buildStrokeRuns(list, p, walked) == count, "the stroke walk letters the glyph");
    StrokeRun tight[2];
    CHECK_MSG(buildStrokeRuns(list, p, tight) == 2, "a full output stops the walk rather than overrunning it");
}

TEST(testPointStrokeIsTheBeamSpot)
{
    const auto projection = makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f);
    Primitive point {};
    point.kind = PrimitiveKind::Point;
    point.x0 = 1000;
    point.y0 = 1000;
    Primitive line = point;
    line.kind = PrimitiveKind::Line;
    line.x1 = 1002;
    line.y1 = 1000;
    const Primitive primitives[] = { point, line };
    StrokeRun runs[2];
    CHECK_EQ(buildStrokeRuns(primitives, projection, runs), 2);
    const auto position = project(projection, point.x0, point.y0);
    CHECK_MSG(approx(runs[0].start.x, position.x) && approx(runs[0].end.x, position.x), "a point is a degenerate run at its position");
    CHECK(approx(runs[0].start.y, position.y) && approx(runs[0].end.y, position.y));
    CHECK_MSG(runs[1].end.x > runs[1].start.x, "a short line is not");

    // The spot is round: the raster lights it the same at every angle, full in
    // the middle and nothing past the half-width and its feather.
    const BeamRun spot(10.5f, 10.5f, 10.5f, 10.5f);
    const int halfThickness = BeamRun::units(1.0f);
    CHECK_EQ(beamCoverage(spot.distanceToPixel(10, 10), halfThickness), beamCoverageScale());
    CHECK_EQ(spot.distanceToPixel(11, 10), spot.distanceToPixel(10, 11));
    CHECK_EQ(spot.distanceToPixel(9, 10), spot.distanceToPixel(10, 9));
    CHECK_EQ(spot.distanceToPixel(11, 11), spot.distanceToPixel(9, 9));
    CHECK_EQ(beamCoverage(spot.distanceToPixel(12, 10), halfThickness), 0);
}

/// ---- The beam raster: an integer distance from a pixel's centre to a run's
/// capsule, which the Metal pass computes identically from the same header. ----
TEST(testBeamRasterDistanceIsTheCapsuleDistance)
{
    const StrokeRun runs[] = {
        { { 10.3f, 20.2f }, { 200.7f, 31.9f } },
        { { 50.0f, 50.0f }, { 50.0f, 50.0f } },
        { { 80.25f, 10.5f }, { 30.75f, 90.125f } },
        { { 0.0f, 0.0f }, { 4000.0f, 3.0f } },
    };
    for (const StrokeRun& run : runs) {
        const BeamRun raster(run.start.x, run.start.y, run.end.x, run.end.y);
        for (int y = -5; y < 100; y += 3) {
            for (int x = -5; x < 220; x += 7) {
                const float exact = distanceToRun(run, { static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f });
                if (exact > 200.0f)
                    continue;
                const float rastered = static_cast<float>(raster.distanceToPixel(x, y)) / static_cast<float>(BeamRun::unitsPerPixel());
                // The endpoints snap to a 16th of a pixel, so up to a 32nd
                // either way, and the distance floors to a 256th.
                CHECK_MSG(fabsf(rastered - exact) <= 1.0f / 16.0f, "the raster's distance is the capsule's");
            }
        }
    }
    const BeamRun far(0.0f, 0.0f, 10.0f, 0.0f);
    CHECK_EQ(far.distanceToPixel(5, 1000), BeamRun::farDistance());
    CHECK_EQ(far.distanceToPixel(5000, 0), BeamRun::farDistance());
}

TEST(testBeamCoverageFeathersOverOnePixel)
{
    const int halfThickness = BeamRun::units(1.0f);
    const int unit = BeamRun::unitsPerPixel();
    CHECK_EQ(beamCoverage(0, halfThickness), beamCoverageScale());
    CHECK_EQ(beamCoverage(halfThickness - unit / 2, halfThickness), beamCoverageScale());
    CHECK_EQ(beamCoverage(halfThickness, halfThickness), beamCoverageScale() / 2);
    CHECK_EQ(beamCoverage(halfThickness + unit / 2, halfThickness), 0);
    int previous = beamCoverageScale();
    for (int distance = 0; distance <= halfThickness + unit; ++distance) {
        const int coverage = beamCoverage(distance, halfThickness);
        CHECK_MSG(coverage <= previous, "coverage never rises with distance");
        previous = coverage;
    }
    CHECK_EQ(beamCoverageByte(beamCoverageScale()), 255);
    CHECK_EQ(beamCoverageByte(0), 0);
    CHECK_EQ(beamCoverageByte(beamCoverageScale() / 2), 128);
    static_assert(beamCoverage(0, 256) == 1 << 24);
    static_assert(BeamRun(0.0f, 0.0f, 0.0f, 0.0f).distanceToPixel(0, 0) == 181, "half a pixel's diagonal, floored");
}

TEST(testHaloTableIsTheKernelInSRGBBytes)
{
    const BeamGeometry beam = beamGeometry(2.0f);
    const int count = haloLevelCount(beam);
    CHECK_MSG(count == static_cast<int>(std::ceil(beam.haloReach * haloLevelsPerPixel())) + 1, "one entry per level step out to the reach");
    HaloLevel levels[1024] {};
    CHECK_EQ(fillHaloLevels(beam, 0x33, 0xff, 0x66, levels), count);
    // The centre lifts green by the peak in linear light; nothing at the reach.
    const double peakGreen = std::pow(static_cast<double>(beam.haloPeak), 1.0 / 2.4) * 1.055 - 0.055;
    CHECK_MSG(std::abs(static_cast<int>(levels[0].green) - static_cast<int>(std::lround(peakGreen * 255.0))) <= 1, "the centre is the peak");
    CHECK_EQ(levels[count - 1].green, 0);
    for (int index = 1; index < count; ++index) {
        CHECK_MSG(levels[index].red <= levels[index - 1].red && levels[index].green <= levels[index - 1].green
                && levels[index].blue <= levels[index - 1].blue,
            "the bloom falls with distance on every channel, so a maximum over runs is the nearest run's level");
    }
    CHECK_EQ(haloLevelIndex(BeamRun::unitsPerPixel()), haloLevelsPerPixel());
    HaloLevel tight[3] {};
    CHECK_MSG(fillHaloLevels(beam, 0x33, 0xff, 0x66, tight) == 3, "a short output stops the table");
}

/// ---- Corners + centre + the Y-FLIP. ----------------------------------
TEST(testCornersAndYflip)
{
    Projection p = makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f);

    // The rect keeps Tek's aspect, so the picture fills [0,0]..[2048,1560].
    // Tek's y axis points up and the device's down, so Tek y 0 is the device
    // bottom.
    const struct {
        int tekX, tekY;
        float deviceX, deviceY;
        const char* meaning;
    } landmarks[] = {
        { 0, 0, 0.0f, 1560.0f, "Tek's bottom-left is the device's bottom-left" },
        { 0, TH, 0.0f, 0.0f, "Tek's top-left is the device's top-left" },
        { TW, 0, 2048.0f, 1560.0f, "Tek's bottom-right is the device's bottom-right" },
        { TW, TH, 2048.0f, 0.0f, "Tek's top-right is the device's top-right" },
        { TW / 2, TH / 2, 1024.0f, 780.0f, "Tek's centre is the device's centre" },
    };
    for (const auto& landmark : landmarks) {
        const DevicePoint landed = project(p, landmark.tekX, landmark.tekY);
        CHECK_MSG(approx(landed.x, landmark.deviceX), landmark.meaning);
        CHECK_MSG(approx(landed.y, landmark.deviceY), landmark.meaning);
    }

    const float nearTekBottom = project(p, 0, 100).y;
    const float nearTekTop = project(p, 0, 3000).y;
    CHECK_MSG(nearTekTop < nearTekBottom, "climbing in Tek y moves up the device");
}

/// ---- Letterbox: device rect WIDER than the Tek aspect gets pillarbox
/// margins (slack on X); the fitted rect is centred. --------------------
TEST(testPillarboxWideViewport)
{
    // 4000 wide x 1560 tall. Tek aspect needs 4096:3120; height is the
    // binding axis: scale = 1560/3120 = 0.5. Fitted width = 4096*0.5 =
    // 2048, leaving 4000-2048 = 1952 of slack split into two 976 pillars.
    Projection p = makeProjection(TW, TH, 0, 0, 4000.0f, 1560.0f);
    checkApproximately({
        { p.scale, 0.5f, "the height, the shorter fit, sets the scale" },
        { p.offsetX, 976.0f, "the slack splits into equal pillars at the sides" },
        { p.offsetY, 0.0f, "so nothing is left above or below" },
        { project(p, TW / 2, TH / 2).x, 2000.0f, "and Tek's centre lands on the device's centre line" },
    });
}

/// ---- Letterbox: device rect TALLER than the Tek aspect gets letterbox
/// margins (slack on Y). ------------------------------------------------
TEST(testLetterboxTallViewport)
{
    // 2048 wide x 3000 tall. Width is binding: scale = 2048/4096 = 0.5.
    // Fitted height = 3120*0.5 = 1560, slack = 3000-1560 = 1440 split
    // into two 720 bars.
    Projection p = makeProjection(TW, TH, 0, 0, 2048.0f, 3000.0f);
    checkApproximately({
        { p.scale, 0.5f, "the width, the shorter fit, sets the scale" },
        { p.offsetX, 0.0f, "so nothing is left at the sides" },
        { p.offsetY, 720.0f, "and the slack splits into equal bars above and below" },
        { project(p, 0, TH).y, 720.0f, "the picture's top sits one bar below the device top" },
        { project(p, 0, 0).y, 2280.0f, "and its bottom one picture height lower" },
    });
}

/// ---- Device-rect ORIGIN offset (e.g. a top margin) is honoured. ------
TEST(testDeviceOriginOffset)
{
    Projection p = makeProjection(TW, TH, 100.0f, 50.0f, 2048.0f, 1560.0f);
    const DevicePoint topLeft = project(p, 0, TH);
    checkApproximately({
        { topLeft.x, 100.0f, "the rect's left edge moves with the device origin" },
        { topLeft.y, 50.0f, "and so does its top edge" },
    });
}

/// ---- project / unproject round-trip (within rounding). ---------------
TEST(testUnprojectRoundtrip)
{
    Projection p = makeProjection(TW, TH, 30.0f, 40.0f, 1000.0f, 900.0f);
    const struct {
        int x, y;
    } samples[] = { { 0, 0 }, { 1000, 500 }, { 2048, 1560 }, { 4095, 3119 } };
    for (const auto& sample : samples) {
        const DevicePoint device = project(p, sample.x, sample.y);
        int backX = -1, backY = -1;
        unproject(p, device.x, device.y, &backX, &backY);
        // Each axis may come back one Tek unit off through rounding.
        CHECK_MSG(std::abs(backX - sample.x) <= 1, "x survives a trip to the device and back");
        CHECK_MSG(std::abs(backY - sample.y) <= 1, "y survives a trip to the device and back");
    }
}

/// ---- unproject clamps out-of-rect device points into Tek range. ------
TEST(testUnprojectClamp)
{
    Projection p = makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f);
    // Device y grows downward and Tek y upward, so the device's top-left
    // corner is Tek's top-left and its bottom-right is Tek's bottom-right.
    const struct {
        float deviceX, deviceY;
        int tekX, tekY;
        const char* meaningX;
        const char* meaningY;
    } outside[] = {
        { -1000.0f, -1000.0f, 0, TH - 1, "far left of the rect clamps to Tek x 0", "far above it clamps to the top Tek row" },
        { 99999.0f, 99999.0f, TW - 1, 0, "far right clamps to the last Tek column", "far below clamps to Tek y 0" },
    };
    for (const auto& point : outside) {
        int clampedX = -1, clampedY = -1;
        unproject(p, point.deviceX, point.deviceY, &clampedX, &clampedY);
        CHECK_MSG(clampedX == point.tekX, point.meaningX);
        CHECK_MSG(clampedY == point.tekY, point.meaningY);
    }
}

/// ---- The four char-cell sizes: larger CharSize index = more columns =
/// SMALLER glyph cell. Ordering + magnitude pinned. ---------------------
TEST(testCharCellSizes)
{
    Projection p = makeProjection(TW, TH, 0, 0, 4096.0f, 3120.0f); // scale 1
    CHECK_MSG(approx(p.scale, 1.0f), "native dimensions project one to one");

    // Largest first; across the tube the four sizes fit 74 x 35, 81 x 38,
    // 121 x 58, and 133 x 64 characters.
    const DeviceSize cells[] = { charCellSize(p, 0), charCellSize(p, 1), charCellSize(p, 2), charCellSize(p, 3) };
    const DeviceSize& large = cells[0];
    const DeviceSize& small = cells[3];
    checkApproximately({
        { large.w, 4096.0f / 74.0f, "a large cell is a 74th of the tube's width" },
        { large.h, 3120.0f / 35.0f, "and a 35th of its height" },
        { small.w, 4096.0f / 133.0f, "a small cell is a 133rd of the width" },
        { small.h, 3120.0f / 64.0f, "and a 64th of the height" },
    });
    for (std::size_t size = 1; size < std::size(cells); ++size) {
        CHECK_MSG(cells[size - 1].w > cells[size].w, "each smaller character size has a narrower cell");
        CHECK_MSG(cells[size - 1].h > cells[size].h, "and a shorter one");
    }

    // A size index past the four folds onto the large size.
    CHECK_MSG(approx(charCellSize(p, 99).w, large.w), "an unknown character size draws large");
}

/// ---- Dash patterns: solid has no runs; others have positive runs that
/// scale with the projection. -------------------------------------------
TEST(testDashPatterns)
{
    Projection p = makeProjection(TW, TH, 0, 0, 4096.0f, 3120.0f); // scale 1
    const struct {
        unsigned lineStyle;
        int runs;
        const char* meaning;
    } styles[] = {
        { 0, 0, "a solid line has no dash runs" },
        { 1, 1, "a dotted line repeats one on/off pair" },
        { 2, 2, "a dot-dash line repeats two" },
        // A reserved style draws as solid rather than reading past the table.
        { 250, 0, "a style outside the table folds onto solid" },
    };
    for (const auto& style : styles)
        CHECK_MSG(dashForLineStyle(p, style.lineStyle).count == style.runs, style.meaning);

    const DashPattern dotted = dashForLineStyle(p, 1);
    CHECK_MSG(dotted.on[0] > 0.0f && dotted.off[0] > 0.0f, "both halves of a dotted run have length");
    // A run is a device length, so half the scale draws half the run.
    const DashPattern halfScaleDotted = dashForLineStyle(makeProjection(TW, TH, 0, 0, 2048.0f, 1560.0f), 1);
    CHECK_MSG(approx(halfScaleDotted.on[0], dotted.on[0] * 0.5f), "dash runs shrink with the projection");
}

/// ---- Aspect invariant across a viewport sweep. For any device rect the
/// fitted Tek rect must (a) keep the 4096:3120 aspect, (b) be centred, and
/// (c) show exactly one boxing pair — pillarbox (offsetX>0, offsetY~0) when the
/// view is wider than Tek, letterbox (offsetY>0, offsetX~0) when taller, and
/// neither offset when the view matches the Tek aspect. The Tek picture fills
/// the pixel view independent of cols/rows: no stray second gap, no wrong-axis
/// shrink. ------------------------------------------------------------------
TEST(testAspectOneBoxingPair)
{
    enum class Boxing : uint8_t {
        None,
        Pillarbox,
        Letterbox,
        Both,
    };
    const float tekAspect = static_cast<float>(TW) / static_cast<float>(TH); // ~1.313
    const float deviceHeight = 1000.0f;
    // Device rects from very tall to very wide at a fixed height; the middle
    // one has exactly the Tek aspect, so the unboxed case is reached.
    const float widths[] = { 300.0f, 800.0f, tekAspect * deviceHeight, 1500.0f, 4000.0f };
    for (const float deviceWidth : widths) {
        Projection p = makeProjection(TW, TH, 0, 0, deviceWidth, deviceHeight);
        const float fittedWidth = p.scale * static_cast<float>(TW);
        const float fittedHeight = p.scale * static_cast<float>(TH);
        const float slackX = deviceWidth - fittedWidth;
        const float slackY = deviceHeight - fittedHeight;

        CHECK_MSG(approx(fittedWidth / fittedHeight, tekAspect), "the fitted picture keeps the 4096:3120 aspect");
        CHECK_MSG(approx(p.offsetX, slackX * 0.5f), "half the horizontal slack sits on each side");
        CHECK_MSG(approx(p.offsetY, slackY * 0.5f), "half the vertical slack sits above and below");
        CHECK_MSG(fittedWidth <= deviceWidth + 1e-2f && fittedHeight <= deviceHeight + 1e-2f, "the fitted picture never overflows the device rect");

        // An offset counts as a bar once it exceeds the tolerance.
        const bool hasSideBars = p.offsetX > 1e-2f;
        const bool hasTopBars = p.offsetY > 1e-2f;
        const Boxing observed = hasSideBars ? (hasTopBars ? Boxing::Both : Boxing::Pillarbox) : (hasTopBars ? Boxing::Letterbox : Boxing::None);
        CHECK_MSG(observed != Boxing::Both, "a viewport is never pillarboxed and letterboxed at once");

        const float viewAspect = deviceWidth / deviceHeight;
        const Boxing expected = viewAspect > tekAspect + 1e-3f ? Boxing::Pillarbox
            : viewAspect < tekAspect - 1e-3f                   ? Boxing::Letterbox
                                                               : Boxing::None;
        const char* const meaning = expected == Boxing::Pillarbox ? "a view wider than Tek is pillarboxed and nothing else"
            : expected == Boxing::Letterbox                       ? "a view taller than Tek is letterboxed and nothing else"
                                                                  : "a view with Tek's aspect has no bars at all";
        CHECK_MSG(observed == expected, meaning);
    }
}

/// ---- The fitted rect is independent of the cell grid: two device rects of
/// the same pixel size yield the same projection regardless of how many Tek
/// columns/rows a caller might (wrongly) have folded in. The case models that
/// coupling — shrinking the device rect to a narrower grid width at a fixed
/// height — and confirms that only the binding axis drives the scale.
/// A WIDER rect at the same height must scale >= a narrower one. --------------
TEST(testWidthDrivesScaleNotGrid)
{
    const float deviceHeight = 1000.0f;
    Projection narrow = makeProjection(TW, TH, 0, 0, 400.0f, deviceHeight);
    Projection wide = makeProjection(TW, TH, 0, 0, 1200.0f, deviceHeight);
    // Both rects are narrower than the Tek aspect allows at this height
    // (1313 wide), so width binds both: the wider one scales larger, neither
    // opens a side gap, and the slack goes to the top and bottom alone.
    CHECK_MSG(wide.scale > narrow.scale, "more width at the same height scales the picture up");
    for (const Projection* widthBound : { &narrow, &wide })
        CHECK_MSG(approx(widthBound->offsetX, 0.0f), "a width-bound rect leaves no gap at its sides");
    CHECK_MSG(narrow.offsetY > 0.0f && wide.offsetY > 0.0f, "both put their slack above and below the picture");
}

/// ---- The bloom kernel: a radial falloff, round at a run's ends and at a
/// corner. A halo measured only across the run ends square at its endpoints,
/// and one summed over the two runs of a corner doubles where they meet. ----
TEST(testHaloKernelIsRadialAtEndsAndCorners)
{
    const BeamGeometry beam = beamGeometry(2.0f);
    CHECK_MSG(beam.haloReach > 0.0f && beam.haloSigma > 0.0f, "the halo has extent");

    // The profile: the peak on the line, strictly falling, zero at the reach
    // and nothing past it, with no step where it meets zero.
    CHECK_MSG(approx(haloIntensity(beam, 0.0f), beam.haloPeak), "the peak sits on the centre line");
    float previous = haloIntensity(beam, 0.0f);
    for (float distance = 0.25f; distance < beam.haloReach; distance += 0.25f) {
        const float intensity = haloIntensity(beam, distance);
        CHECK_MSG(intensity < previous, "the falloff is strictly decreasing inside the reach");
        previous = intensity;
    }
    CHECK_MSG(haloIntensity(beam, beam.haloReach) == 0.0f, "zero at the reach");
    CHECK_MSG(haloIntensity(beam, beam.haloReach + 5.0f) == 0.0f, "nothing past the reach");
    CHECK_MSG(haloIntensity(beam, beam.haloReach - 0.01f) < 1e-3f, "the falloff meets zero without a step");

    // Round ends: the same distance from a run reads the same strength beside
    // its middle, straight past its end, and diagonally off its end.
    const StrokeRun run { { 100.0f, 100.0f }, { 200.0f, 100.0f } };
    for (float radius = 1.0f; radius < beam.haloReach; radius += 1.0f) {
        const float beside = haloIntensity(beam, distanceToRun(run, { 150.0f, 100.0f + radius }));
        const float pastEnd = haloIntensity(beam, distanceToRun(run, { 200.0f + radius, 100.0f }));
        const float beforeStart = haloIntensity(beam, distanceToRun(run, { 100.0f - radius, 100.0f }));
        const float diagonal = haloIntensity(beam, distanceToRun(run, { 200.0f + radius * 0.6f, 100.0f - radius * 0.8f }));
        CHECK_MSG(beside > 0.0f, "the halo is lit beside the run");
        CHECK_MSG(approx(pastEnd, beside), "the halo continues past the end as far as it reaches across");
        CHECK_MSG(approx(beforeStart, beside), "and before the start");
        CHECK_MSG(approx(diagonal, beside), "the end cap is round, not square");
    }

    // Round corners: around the outside of an L joint the halos' maximum is
    // the kernel of the distance to the joint itself at every angle, and on the
    // inside it never exceeds the peak a single run gives.
    const StrokeRun across { { 100.0f, 100.0f }, { 200.0f, 100.0f } };
    const StrokeRun down { { 200.0f, 100.0f }, { 200.0f, 200.0f } };
    const float radius = beam.haloReach * 0.5f;
    for (int step = 0; step <= 8; ++step) {
        const float angle = -1.5707964f + 1.5707964f * static_cast<float>(step) / 8.0f;
        const DevicePoint point { 200.0f + radius * std::cos(angle), 100.0f + radius * std::sin(angle) };
        const float combined = std::max(haloIntensity(beam, distanceToRun(across, point)), haloIntensity(beam, distanceToRun(down, point)));
        CHECK_MSG(approx(combined, haloIntensity(beam, radius)), "the outside of a corner is a round arc of the falloff");
    }
    const DevicePoint inside { 195.0f, 105.0f };
    const float insideCombined = std::max(haloIntensity(beam, distanceToRun(across, inside)), haloIntensity(beam, distanceToRun(down, inside)));
    CHECK_MSG(insideCombined <= beam.haloPeak, "no hot spot inside the joint");
}

/// ---- The beam scales with the backing scale, so points and pixels agree. --
TEST(testBeamGeometryScalesWithTheBackingScale)
{
    const BeamGeometry points = beamGeometry(1.0f);
    const BeamGeometry pixels = beamGeometry(2.0f);
    CHECK_MSG(approx(pixels.edgeInset, points.edgeInset * 2.0f), "the inset is proportional");
    CHECK_MSG(approx(pixels.haloReach, points.haloReach * 2.0f), "the reach is proportional");
    CHECK_MSG(points.edgeInset >= points.haloReach + points.halfThickness, "the inset holds the halo and the stroke");
    CHECK_MSG(beamGeometry(0.5f).halfThickness >= 0.5f, "the stroke is at least one device pixel wide");

    const Projection inset = makeBeamProjection(TW, TH, 0, 0, 2048.0f, 1560.0f, 1.0f);
    const DevicePoint bottomLeft = project(inset, 0, 0);
    CHECK_MSG(bottomLeft.y <= 1560.0f - points.edgeInset + 1e-3f, "the origin stands clear of the bottom edge");
    CHECK_MSG(bottomLeft.x >= points.edgeInset - 1e-3f, "and of the left edge");
}

/// ---- Degenerate viewport: scale 0, everything pins to device origin. -
TEST(testDegenerateViewport)
{
    const float originX = 5.0f, originY = 7.0f;
    Projection p = makeProjection(TW, TH, originX, originY, 0.0f, 0.0f);
    const DevicePoint anywhere = project(p, 2000, 1000);
    checkApproximately({
        { p.scale, 0.0f, "a rect with no area has no scale" },
        { anywhere.x, originX, "so every Tek point lands on the device origin's x" },
        { anywhere.y, originY, "and on its y" },
    });
    // With no scale to divide by, unproject still answers inside Tek space.
    int tekX = -1, tekY = -1;
    unproject(p, 100.0f, 100.0f, &tekX, &tekY);
    CHECK_MSG(tekX == 0 && tekY == 0, "an empty rect unprojects to the Tek origin");
}

UBIQUITTY_TEST_MAIN()
