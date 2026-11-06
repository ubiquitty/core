// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_fontmetrics.cpp — host tests for the portable vertical cell layout
// (Source/Core/Rendering/FontMetrics.{h,cpp}).
//
// Pins the half-leading rule: the font line gap is split EVENLY above and
// below the glyph, so a face with a built-in leading (Monaspace) sits
// centered instead of riding low. No platform types, links only
// FontMetrics.cpp.

#include "Core/Rendering/Font/FontRoute.h"
#include "Core/Rendering/FontMetrics.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace Ubiquitty;

static bool approx(double a, double b) { return fabs(a - b) < 1e-6; }

/// One quantity a layout produced, the value the case requires of it, and what
/// that requirement means when it fails.
struct Measurement {
    double actual;
    double expected;
    const char* meaning;
};

/// Checks every measurement to the shared tolerance, reporting each failure by
/// its meaning.
static void checkMeasurements(std::initializer_list<Measurement> measurements)
{
    for (const Measurement& measurement : measurements)
        CHECK_MSG(approx(measurement.actual, measurement.expected), measurement.meaning);
}

/// ---- leading 0 (SF Mono / Menlo): no slack, baseline pins at descent. With
/// no spacing and no snap the gap is empty, so the result equals a naive
/// ascent+descent box. ------------------------------------------------------
TEST(testNoLeadingNoGap)
{
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(/*asc*/ 13.0, /*desc*/ 3.0,
        /*leading*/ 0.0, /*spacing*/ 0.0,
        /*snap*/ 0.0);
    checkMeasurements({
        { m.lineHeight, 16.0, "with no slack the cell is the ascent plus the descent" },
        { m.gapTotal, 0.0, "and nothing is left over to place" },
        { m.gapTop, 0.0, "so no space opens above the ascent" },
        { m.baselineFromBottom, 3.0, "and the baseline rests on the descent" },
    });
}

/// ---- the core fix: a font leading is split half above / half below, not
/// dumped above. Putting 100% above leaves the baseline at descent; the glyph
/// must rise by ~leading/2. ------------------------------------------------
TEST(testLeadingSplitEvenly)
{
    const double asc = 13.0, desc = 3.0, leading = 4.0;
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(asc, desc, leading, 0.0, 0.0);
    // The slack under the descent is what remains once the top share is taken;
    // equal shares are what centres the ink box in the cell.
    const double slackBelow = m.gapTotal - m.gapTop;
    checkMeasurements({
        { m.lineHeight, 20.0, "the cell holds the ink and the whole leading" },
        { m.gapTotal, 4.0, "all of the leading becomes slack" },
        { m.gapTop, 2.0, "half of that slack opens above the ascent" },
        { m.baselineFromBottom, 5.0, "the baseline sits half the leading above the descent, at 5 rather than 3" },
        { m.gapTop, slackBelow, "the slack above the ink equals the slack below it" },
    });
}

/// ---- regression guard: the baseline must sit strictly higher than a
/// one-sided baseline (== descent) whenever leading > 0, by at least
/// floor(leading/2) within rounding. This is the "text does not ride low"
/// invariant. -------------------------------------------------------------
TEST(testLeadingRaisesBaseline)
{
    for (double leading = 0.5; leading <= 6.0; leading += 0.5) {
        Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(13.0, 3.0, leading, 0.0, 0.0);
        // Where the baseline would sit if every unit of slack went above the ink.
        const double descentOnly = 3.0;
        CHECK_MSG(m.baselineFromBottom >= descentOnly, "the baseline never falls below the descent");
        CHECK_MSG(m.baselineFromBottom >= descentOnly + leading * 0.5 - 1.0, "and climbs by about half the leading");
    }
}

/// ---- Monaspace Neon @14pt on a 2x display (metrics in atlas px = pt*2):
/// asc=26.46 desc=5.60 leading=2.80, snap to a whole point (2 atlas px). The
/// ink box must end up vertically centered to within the snap rounding (a gap
/// not split evenly leaves it ~3 atlas px / 1.5pt low). --------------------
TEST(testMonaspaceCentered)
{
    const double asc = 26.46, desc = 5.60, leading = 2.80;
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(asc, desc, leading,
        /*spacing*/ 0.0, /*snap*/ 2.0);
    // Snapping rounds the 34.86 unit box up to a whole number of 2-unit steps:
    // 18 steps, 36 atlas pixels.
    CHECK_MSG(approx(m.lineHeight, 36.0), "the snapped Monaspace cell is 36 atlas pixels, 18 points");
    const double slackBelow = m.gapTotal - m.gapTop;
    // The top share is rounded to the snap unit, which may leave the two
    // shares a unit apart and no more.
    CHECK_MSG(fabs(m.gapTop - slackBelow) <= 1.0 + 1e-9, "the shares above and below the ink differ by at most one unit");
    CHECK_MSG(m.baselineFromBottom > desc + 1.0, "the baseline clears the descent by more than a unit");
}

/// ---- snapUnit ceils the cell height up to a whole unit; a sub-unit line
/// never rounds down (rows must not lose height). -------------------------
TEST(testSnapCeilsUp)
{
    // ink 16.4 + no leading; snap to 3 -> ceil(16.4/3)*3 = 18.
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(13.2, 3.2, 0.0, 0.0, 3.0);
    checkMeasurements({
        { m.lineHeight, 18.0, "a 16.4 unit box grows to the next multiple of 3" },
        { m.gapTotal, 1.6, "and the growth is slack" },
    });
}

/// ---- a large negative line spacing floors the cell at half the ink box so
/// glyphs can't overprint neighbours into illegibility. -------------------
TEST(testNegativeSpacingFloored)
{
    const double asc = 13.0, desc = 3.0, inkHeight = asc + desc;
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(asc, desc, 0.0,
        /*spacing*/ -100.0, /*snap*/ 0.0);
    checkMeasurements({ { m.lineHeight, inkHeight * 0.5, "spacing far below zero stops at half the ink height" } });
}

/// ---- user line spacing is slack too: it splits evenly like leading. -----
TEST(testUserSpacingSplits)
{
    Rendering::FontMetrics::CellVerticalMetrics m = Rendering::FontMetrics::layoutCell(13.0, 3.0, /*leading*/ 0.0,
        /*spacing*/ 6.0, /*snap*/ 0.0);
    checkMeasurements({
        { m.lineHeight, 22.0, "the configured spacing adds to the ink height" },
        { m.gapTop, 3.0, "half of it opens above the ascent" },
        { m.baselineFromBottom, 6.0, "and the other half lifts the baseline off the descent" },
    });
}

// A uniform face: the widest is the representative, so this returns exactly what
// probing 'M' alone returned. The whole fix rests on costing nothing here.
TEST(testUniformFaceMatchesSingleProbe)
{
    const double uniform[] = { 8.0, 8.0, 8.0, 8.0, 8.0 };
    CHECK_MSG(approx(Ubiquitty::Rendering::FontMetrics::cellAdvance(Rendering::FontMetrics::FacePitch::Fixed, uniform), 8.0), "uniform face keeps the single-probe width");
}

// The bug this exists for: a face that declares monospace but carries one wider
// glyph. Probing 'M' returns 8 and that glyph draws into the next column; the
// widest sample gives it a cell it fits in.
TEST(testOutlierGlyphWidensTheCell)
{
    // Söhne Mono, the least uniform face in the installed population that really
    // is monospace: its space sits one 600th of an em off its digits. Probing
    // one glyph returns whichever of the two it happened to ask for.
    const double sohneMono[] = { 1200.0, 1200.0, 1202.0, 1200.0 };
    CHECK_MSG(approx(Ubiquitty::Rendering::FontMetrics::cellAdvance(Rendering::FontMetrics::FacePitch::Fixed, sohneMono), 1202.0), "one wide glyph sets the cell");
}

// A face missing a glyph advances 0 for it. Skipped, not treated as a zero-width
// cell — reading it as one would collapse the grid.
TEST(testMissingGlyphsIgnored)
{
    const double gaps[] = { 0.0, 7.0, 0.0, 0.0 };
    CHECK_MSG(approx(Ubiquitty::Rendering::FontMetrics::cellAdvance(Rendering::FontMetrics::FacePitch::Fixed, gaps), 7.0), "absent glyphs do not win");
}

// Nothing usable sampled: report 0 and leave the caller's fallback in charge
// rather than inventing a width.
TEST(testEmptySampleDefersToCaller)
{
    CHECK_MSG(approx(Ubiquitty::Rendering::FontMetrics::cellAdvance(Rendering::FontMetrics::FacePitch::Fixed, {}), 0.0), "empty sample yields 0");
    const double allZero[] = { 0.0, 0.0 };
    CHECK_MSG(approx(Ubiquitty::Rendering::FontMetrics::cellAdvance(Rendering::FontMetrics::FacePitch::Fixed, allZero), 0.0), "all-absent yields 0");
}

// The sample range is the seam's to state: a caller sizing its own buffer from
// its own copy of 0x20..0x7E is a second source of truth for the same fact.
TEST(testProbeRangeIsPrintableASCII)
{
    CHECK_MSG(Ubiquitty::Rendering::FontMetrics::firstProbeCharacter == U' ', "the sample opens at space");
    CHECK_MSG(Ubiquitty::Rendering::FontMetrics::lastProbeCharacter == U'~', "and closes at tilde");
    CHECK_MSG(Ubiquitty::Rendering::FontMetrics::probeCharacterCount == 95, "95 printable ASCII characters");
}

// Tracking moves the pitch, and the two directions are one setting: a reader
// tightening a wide face uses the same control as one loosening a tight one.
TEST(testTrackingMovesThePitchBothWays)
{
    using Ubiquitty::Rendering::FontMetrics::trackedCellAdvance;
    CHECK_MSG(approx(trackedCellAdvance(10.0, 0.0), 10.0), "no tracking leaves the face's advance");
    CHECK_MSG(approx(trackedCellAdvance(10.0, 2.5), 12.5), "positive tracking widens the cell");
    CHECK_MSG(approx(trackedCellAdvance(10.0, -2.5), 7.5), "and negative tracking tightens it");
}

// The floor is what keeps a grid drawable at the control's end. Without it the
// cell reaches zero and then goes negative, which is a window with no columns
// rather than a tight one.
TEST(testTrackingCannotCollapseTheCell)
{
    using Ubiquitty::Rendering::FontMetrics::trackedCellAdvance;
    CHECK_MSG(approx(trackedCellAdvance(10.0, -10.0), 1.0), "tracking to zero stops at the floor");
    CHECK_MSG(approx(trackedCellAdvance(10.0, -40.0), 1.0), "and past it stays there");
    // A face that measured nothing keeps measuring nothing: widening zero
    // would hand back a width derived from a probe that never happened, and
    // the caller's own fallback owns that case.
    CHECK_MSG(approx(trackedCellAdvance(0.0, 3.0), 0.0), "an unmeasured face is left to its caller");
}

// Whether a sampled advance set is monospace: all usable advances equal.
TEST(testIsMonospaceUniformIsMono)
{
    const double uniform[] = { 8.0, 8.0, 8.0, 8.0 };
    CHECK_MSG(Ubiquitty::Rendering::FontMetrics::isMonospace(uniform), "uniform advances are monospace");
}

TEST(testIsMonospaceOutlierIsNotMono)
{
    const double varied[] = { 8.0, 8.0, 9.0, 8.0 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(varied), "one differing advance is not monospace");
}

TEST(testIsMonospaceRefusesMissingGlyphs)
{
    // A face that does not draw printable ASCII is not a terminal face: a gap
    // is a refusal, not a sample to skip. Otherwise a braille or emoji face
    // with one ASCII glyph would be admitted as a primary font.
    const double withGaps[] = { 0.0, 7.0, 0.0, 7.0 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(withGaps), "a missing glyph refuses the face");
    double oneGlyph[95] = {};
    oneGlyph[0] = 7.0;
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(oneGlyph), "one present glyph is not evidence");
    const double negative[] = { 7.0, -1.0, 7.0 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(negative), "a negative advance refuses the face");
}

TEST(testIsMonospaceEmptyIsNotMono)
{
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace({}), "empty sample is not monospace");
    const double allZero[] = { 0.0, 0.0 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(allZero), "all missing is not monospace");
}

TEST(testIsMonospaceProportionalIsNotMono)
{
    const double helvetica[] = { 8.42, 9.1, 8.42, 12.3 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(helvetica), "proportional advances are not monospace");
}

TEST(testIsMonospaceEpsilonTolerance)
{
    const double within[] = { 8.0, 8.0005, 8.0 };
    CHECK_MSG(Ubiquitty::Rendering::FontMetrics::isMonospace(within), "within 0.001 is still monospace");
    const double outside[] = { 8.0, 8.002, 8.0 };
    CHECK_MSG(!Ubiquitty::Rendering::FontMetrics::isMonospace(outside), "outside 0.001 is not monospace");
}

// Zero is the absence of a weight, not a light one — the distinction the two
// font-weight settings are built on.
TEST(testConfiguredWeightKeepsZeroMeaningTheFacesOwn)
{
    using Ubiquitty::Rendering::Font::variationWeightForSetting;
    CHECK_MSG(approx(variationWeightForSetting(0), 0.0), "zero names no axis");
    CHECK_MSG(approx(variationWeightForSetting(-200), 0.0), "and neither does a negative");
    CHECK_MSG(approx(variationWeightForSetting(400), 400.0), "a weight in range is the axis value");
    CHECK_MSG(approx(variationWeightForSetting(1200), 900.0), "above the axis clamps to its end");
    CHECK_MSG(approx(variationWeightForSetting(30), 100.0), "and below it clamps to the other");
}

// The width mapping is the OS/2 table's, not a curve: the anchors are the
// nine usWidthClass percents, and the trait convention (class - 5) / 10 is
// checkable against the system faces — SF Condensed (class 3) reports -0.2.
TEST(testWidthClassFollowsTheOS2Anchors)
{
    using Ubiquitty::Rendering::Font::widthClassFromPercent;
    CHECK_MSG(approx(widthClassFromPercent(50.0), 1.0), "ultra-condensed is class 1");
    CHECK_MSG(approx(widthClassFromPercent(75.0), 3.0), "condensed is class 3");
    CHECK_MSG(approx(widthClassFromPercent(100.0), 5.0), "normal is class 5");
    CHECK_MSG(approx(widthClassFromPercent(112.5), 6.0), "semi-expanded is class 6");
    CHECK_MSG(approx(widthClassFromPercent(150.0), 8.0), "extra-expanded is class 8");
    CHECK_MSG(approx(widthClassFromPercent(200.0), 9.0), "ultra-expanded is class 9");
    // Between anchors the class interpolates, so the matcher prefers the
    // nearer member; the spans are unequal (12.5 up to 50 percent wide), so a
    // linear-in-percent curve would land these on the wrong side.
    CHECK_MSG(approx(widthClassFromPercent(106.25), 5.5), "halfway to semi-expanded is class 5.5");
    CHECK_MSG(approx(widthClassFromPercent(175.0), 8.5), "halfway to ultra-expanded is class 8.5");
    CHECK_MSG(approx(widthClassFromPercent(40.0), 1.0), "below the table clamps to its first row");
    CHECK_MSG(approx(widthClassFromPercent(300.0), 9.0), "and above it to its last");
}

TEST(testWidthTraitFollowsTheCoreTextConvention)
{
    using Ubiquitty::Rendering::Font::normalisedWidthTraitForSetting;
    CHECK_MSG(approx(normalisedWidthTraitForSetting(75.0), -0.2), "condensed reports -0.2, as SF Condensed does");
    CHECK_MSG(approx(normalisedWidthTraitForSetting(62.5), -0.3), "extra-condensed -0.3, as SF Compressed does");
    CHECK_MSG(approx(normalisedWidthTraitForSetting(100.0), 0.0), "normal is zero");
    CHECK_MSG(approx(normalisedWidthTraitForSetting(112.5), 0.1), "semi-expanded is 0.1");
    CHECK_MSG(approx(normalisedWidthTraitForSetting(125.0), 0.2), "expanded 0.2, as SF Expanded does");
}

TEST(testFacePitchReadsTheSampleItsCellIsMeasuredFrom)
{
    using Ubiquitty::Rendering::FontMetrics::AdvanceSpan;
    using Ubiquitty::Rendering::FontMetrics::FacePitch;
    using Ubiquitty::Rendering::FontMetrics::facePitch;
    const double fixed[] = { 8.4, 8.4, 8.4, 8.4 };
    const double proportional[] = { 4.6, 8.4, 3.5, 13.2 };
    // A symbol face as primary: absent letters advance 0 and are not unequal ones.
    const double sparse[] = { 0.0, 10.0, 0.0, -1.0, 10.0 };
    CHECK_MSG(facePitch(AdvanceSpan(fixed)) == FacePitch::Fixed, "equal advances are fixed pitch");
    CHECK_MSG(facePitch(AdvanceSpan(proportional)) == FacePitch::Proportional, "unequal advances are proportional");
    CHECK_MSG(facePitch(AdvanceSpan(sparse)) == FacePitch::Fixed, "missing glyphs do not make a face proportional");
    CHECK_MSG(facePitch(AdvanceSpan()) == FacePitch::Fixed, "an empty sample is fixed pitch");
}

TEST(testAProportionalFaceIsCutToItsDigit)
{
    using namespace Ubiquitty::Rendering::FontMetrics;
    std::vector<double> advances(probeCharacterCount, 10.0);
    const auto at = [](char32_t character) { return static_cast<std::size_t>(character - firstProbeCharacter); };
    advances[at(U'0')] = 17.36;
    advances[at(U'W')] = 26.25;
    advances[at(U'i')] = 6.1;
    CHECK_MSG(facePitch(AdvanceSpan(advances)) == FacePitch::Proportional, "unequal advances are proportional");
    CHECK_MSG(approx(cellAdvance(FacePitch::Proportional, AdvanceSpan(advances)), 17.36), "a proportional face's cell is the advance of 0, not of W");
    advances[at(U'0')] = 0.0;
    for (char32_t letter = U'a'; letter <= U'z'; ++letter)
        advances[at(letter)] = letter == U'm' ? 23.0 : 13.0;
    CHECK_MSG(approx(cellAdvance(FacePitch::Proportional, AdvanceSpan(advances)), (25.0 * 13.0 + 23.0) / 26.0),
        "a face without 0 takes the mean lowercase advance");
    const double shortSample[] = { 8.9, 17.79, 32.0, 12.0 };
    CHECK_MSG(approx(cellAdvance(FacePitch::Proportional, shortSample), 32.0), "a sample too short to reach 0 keeps the widest advance it holds");
}

// The pitch is the caller's, read from unhinted advances: a monospace face
// whose hinted sample rounds a pixel apart keeps its widest advance rather
// than being cut to a digit the hinter happened to round down.
TEST(testAHintedMonospaceSampleKeepsItsWidestAdvanceWhenTheFaceIsFixed)
{
    using namespace Ubiquitty::Rendering::FontMetrics;
    std::vector<double> advances(probeCharacterCount, 10.0);
    const auto at = [](char32_t character) { return static_cast<std::size_t>(character - firstProbeCharacter); };
    advances[at(U'0')] = 9.0;
    advances[at(U'W')] = 10.0;
    CHECK_MSG(facePitch(AdvanceSpan(advances)) == FacePitch::Proportional, "the hinted sample alone reads as proportional");
    CHECK_MSG(approx(cellAdvance(FacePitch::Fixed, AdvanceSpan(advances)), 10.0), "told the face is fixed, the cell is the widest hinted advance");
    CHECK_MSG(approx(cellAdvance(FacePitch::Proportional, AdvanceSpan(advances)), 9.0), "told it is proportional, the cell is the digit's");
}

TEST(testFallbackFontsMatchTextMetrics)
{
    using namespace Rendering::FontMetrics;
    const FallbackMetrics primary { 0.55, 0.73, 1.32 };
    CHECK(approx(fallbackScale(primary, { 0.40, 0.55, 0.90 }), 1.375));
    CHECK(approx(fallbackScale(primary, { 0.70, 0.90, 1.50 }), 0.55 / 0.70));
    CHECK(approx(fallbackScale(primary, primary), 1.0));
    CHECK(approx(fallbackScale(primary, { 0, 0.55, 0.90 }), 0.73 / 0.55));
    CHECK(approx(fallbackScale(primary, { 0, 0, 0.90 }), 1.32 / 0.90));
    CHECK(approx(fallbackScale({}, {}), 1.0));
}

TEST(testInvalidFallbackMetricsCannotExpandTheRasterWithoutBound)
{
    using namespace Rendering::FontMetrics;
    const FallbackMetrics primary { 0.55, 0.73, 1.32 };
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK(approx(fallbackScale(primary, { invalid, 0.55, 0.90 }), 0.73 / 0.55));
    CHECK(approx(fallbackScale(primary, { infinity, -1, 0.90 }), 1.32 / 0.90));
    CHECK(approx(fallbackScale(primary, { -1, invalid, 0 }), 1.0));
    CHECK(approx(fallbackScale({ invalid, 0, -1 }, primary), 1.0));
    CHECK(approx(fallbackScale(primary, { 0.001, 0.55, 0.90 }), 2.0));
    CHECK(approx(fallbackScale(primary, { 1000, 0.55, 0.90 }), 0.5));
}

UBIQUITTY_TEST_MAIN()
