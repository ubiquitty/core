// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_rule_geometry.cpp — the size-relative rule geometry.
//
// The floor is one device pixel, which is the thinnest thing a display can
// draw. Not one point, for the reason below.
//
// A point floor looks like the display-independent choice and is the opposite:
// at 2x it binds for every size below 16pt, so 6pt and 14pt text both get a
// rule of exactly two device pixels and the weight stops tracking the type it
// sits under. The symptom it was reaching for — an 11pt rule reading light at
// 2x — is the builder rounding 1.375 device pixels to 1, and that rounding is
// where it is fixed: the builder snaps at 1x only, where a whole pixel is the
// grid rather than a choice.

#include "Harness.h"

#include "Core/Rendering/RuleGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using Ubiquitty::Rendering::doubledRuleGap;
using Ubiquitty::Rendering::RuleGeometry;
using Ubiquitty::Rendering::ruleGeometry;

bool close(float a, float b)
{
    return std::fabs(a - b) < 0.0001f;
}

TEST(strokeFloorIsOneDevicePixel)
{
    // Never thinner than the display can draw, at any scale.
    CHECK(ruleGeometry(11.0f, 1.0f).underlineThickness >= 1.0f);
    CHECK(ruleGeometry(22.0f, 2.0f).underlineThickness >= 1.0f);
    CHECK(ruleGeometry(33.0f, 3.0f).underlineThickness >= 1.0f);
}

// And never thicker than the size asks for, which is the half a point floor
// cannot express. A floor of `scale` would put all three of these at exactly
// `scale` device pixels — one weight for the range a terminal is read at.
TEST(smallTextKeepsAThinRuleOnADenseDisplay)
{
    CHECK(close(ruleGeometry(12.0f, 2.0f).underlineThickness, 1.0f)); // 6pt at 2x
    CHECK(close(ruleGeometry(16.0f, 2.0f).underlineThickness, 1.0f)); // 8pt at 2x
    CHECK(close(ruleGeometry(22.0f, 2.0f).underlineThickness, 1.375f)); // 11pt at 2x
    CHECK(close(ruleGeometry(28.0f, 2.0f).underlineThickness, 1.75f)); // 14pt at 2x
    // Monotone in the type size, which is the property a floor destroys where
    // it binds: the three above are 1.0, 1.375, 1.75 and not one value repeated.
    CHECK(ruleGeometry(12.0f, 2.0f).underlineThickness < ruleGeometry(22.0f, 2.0f).underlineThickness);
    CHECK(ruleGeometry(22.0f, 2.0f).underlineThickness < ruleGeometry(28.0f, 2.0f).underlineThickness);
}

// Above the floor, the weight is display-invariant point-for-point: both terms
// are linear in the scale, so the same type prints the same rule on every
// screen. Asserted rather than assumed, because the floor and the size term
// change places as the type grows.
TEST(weightInPointsDoesNotDependOnTheDisplayAboveTheFloor)
{
    for (const float pointSize : { 16.0f, 24.0f, 48.0f }) {
        const RuleGeometry at1x = ruleGeometry(pointSize, 1.0f);
        const RuleGeometry at2x = ruleGeometry(pointSize * 2.0f, 2.0f);
        CHECK_MSG(close(at1x.underlineThickness, at2x.underlineThickness / 2.0f),
            "underline weight in points drifts with the display scale");
    }
}

// Below it the printed weights differ, and that is the floor doing its job
// rather than a defect. 9pt wants 0.5625 device pixels at 1x; a display with
// whole pixels cannot draw that, so it draws one. At 2x the same text wants
// 1.125 and gets it. The 1x rule is heavier in points because 1x cannot go
// finer, which is a statement about the display and not about the type.
TEST(belowTheFloorTheDisplayDecidesHowFineARuleCanBe)
{
    const RuleGeometry at1x = ruleGeometry(9.0f, 1.0f);
    const RuleGeometry at2x = ruleGeometry(18.0f, 2.0f);
    CHECK(close(at1x.underlineThickness, 1.0f)); // floored: 0.5625 is unreachable
    CHECK(close(at2x.underlineThickness, 1.125f)); // reachable, so drawn
    CHECK(at2x.underlineThickness / 2.0f < at1x.underlineThickness);
}

// At 1x the rule is the point size over 16, floored at one pixel: every
// portable golden and every container render is a 1x frame. This case is what
// licenses changing the 2x weight without re-recording the 1x tiers.
TEST(oneXGeometryKeepsThePixelFloor)
{
    for (const float pointSize : { 8.0f, 11.0f, 16.0f, 32.0f }) {
        const float expected = pointSize / 16.0f > 1.0f ? pointSize / 16.0f : 1.0f;
        CHECK(close(ruleGeometry(pointSize, 1.0f).underlineThickness, expected));
    }
}

// The curl and the strike track the solid rule's weight, floor included: a
// heavier underline with a hairline strike beside it reads as two pens in one
// paragraph.
TEST(curlAndStrikeTrackTheUnderline)
{
    const RuleGeometry geometry = ruleGeometry(22.0f, 2.0f);
    CHECK(close(geometry.curlyThickness, geometry.underlineThickness));
    CHECK(close(geometry.strikeThickness, geometry.underlineThickness));
}

// The gap of a doubled rule is what makes it read as two, so it is held in
// proportion to the strokes rather than fixed in device pixels. A constant gap
// survives every thin case and fails the heavy one: the strokes grow, the space
// between them does not, and at the maximum thickness the pair prints as one bar
// with a scratch through it — which is the one thing a doubled rule must not be.
TEST(aDoubledRuleGapTracksItsStrokeSoThePairNeverMerges)
{
    // Proportional, not constant. A rule four times as heavy is four times as
    // far from its partner, so the pair reads the same at every weight: the
    // same share of a stroke as the rule keeps from the baseline, floored at a
    // point.
    CHECK(close(doubledRuleGap(1.0f, 1.0f), 1.0f));
    CHECK(close(doubledRuleGap(4.0f, 1.0f), 3.4f));
    CHECK(close(doubledRuleGap(7.0f, 2.0f), 5.95f));

    // Monotone, which is the property a constant gap destroys. Asserted rather
    // than assumed: the failure it guards is a gap that stops growing while the
    // strokes keep going.
    CHECK(doubledRuleGap(7.0f, 1.0f) > doubledRuleGap(4.0f, 1.0f));
    CHECK(doubledRuleGap(4.0f, 1.0f) > doubledRuleGap(1.0f, 1.0f));

    // A gap under a device pixel is one the paint pass rounds away, and the pair
    // it was separating becomes a single mark of twice the weight.
    CHECK(close(doubledRuleGap(0.4f, 1.0f), 1.0f));
    CHECK(close(doubledRuleGap(0.0f, 1.0f), 1.0f));
    // And the floor is a POINT: a thin pair on a dense display keeps the
    // separation it shows on a coarse one instead of halving it.
    CHECK(close(doubledRuleGap(1.0f, 2.0f), 2.0f));
    CHECK(close(doubledRuleGap(1.0f, 3.0f), 3.0f));
}

// A face's rule is its underscore as the engine draws it: a bar two rows
// solid and a third half covered weighs two and a half pixels, whatever its
// width, and ends a face rounds or tapers do not thin it, because the bar is
// read across its middle third.
TEST(theUnderscoreIsReadAtItsMiddleWeight)
{
    constexpr int width = 20;
    constexpr int height = 8;
    std::vector<std::uint8_t> bar(static_cast<std::size_t>(width * height), 0);
    const auto paint = [&](int left, int right, int row, std::uint8_t value) {
        for (int column = left; column < right; ++column)
            bar[static_cast<std::size_t>(row * width + column)] = value;
    };
    paint(4, 16, 3, 255);
    paint(4, 16, 4, 255);
    paint(4, 16, 5, 128);
    CHECK(std::fabs(Ubiquitty::Rendering::measuredUnderscoreWeight(bar.data(), width, height) - (2.0f + 128.0f / 255.0f)) < 1e-4f);
    // Rounded ends: the outer columns carry a lighter bar.
    paint(4, 6, 3, 64);
    paint(14, 16, 3, 64);
    CHECK(std::fabs(Ubiquitty::Rendering::measuredUnderscoreWeight(bar.data(), width, height) - (2.0f + 128.0f / 255.0f)) < 1e-4f);
    const std::vector<std::uint8_t> blank(static_cast<std::size_t>(width * height), 0);
    CHECK(Ubiquitty::Rendering::measuredUnderscoreWeight(blank.data(), width, height) == 0.0f);
}

// A face's measured underscore is its rule, ahead of the weight it declares:
// the declaration is a legacy number, the underscore what its letters match.
TEST(theUnderscoreOutranksTheDeclaredUnderline)
{
    Ubiquitty::Rendering::RuleFaceMetrics face;
    face.underlineThickness = 1.4f;
    face.underscoreWeight = 2.7f;
    face.descent = 8.0f;
    CHECK(std::fabs(ruleGeometry(28.0f, 2.0f, face).underlineThickness - 3.0f) < 1e-4f);
    face.underscoreWeight = 0.0f;
    CHECK(std::fabs(ruleGeometry(28.0f, 2.0f, face).underlineThickness - 2.0f) < 1e-4f);
}

// A face's rule is never thinner than a point. Hack's underscore at 12 points
// on a 2x display measures under one and a half pixels and rounds to one, a
// rule lighter than the letters it sits under; TextEdit draws two. At 1x a
// point is one pixel, and nothing changes there.
TEST(aFacesRuleIsAtLeastAPoint)
{
    Ubiquitty::Rendering::RuleFaceMetrics face;
    face.underscoreWeight = 1.3f;
    face.descent = 7.2f;
    CHECK(ruleGeometry(24.0f, 2.0f, face).underlineThickness == 2.0f);
    CHECK(ruleGeometry(12.0f, 1.0f, face).underlineThickness == 1.0f);
    face.underscoreWeight = 2.6f;
    CHECK(ruleGeometry(24.0f, 2.0f, face).underlineThickness == 3.0f);
}

// A rule is drawn in whole device rows. SF Mono's underscore at 12 points on a
// 2x display is 1.72 pixels, which drew as a solid row and a faint one where
// TextEdit draws two solid rows; a weight under one row still draws one.
TEST(aRuleIsDrawnInWholeRows)
{
    using Ubiquitty::Rendering::ruleWeight;
    CHECK(ruleWeight(1.72f) == 2.0f);
    CHECK(ruleWeight(1.80f) == 2.0f);
    CHECK(ruleWeight(2.32f) == 2.0f);
    CHECK(ruleWeight(2.5f) == 3.0f);
    CHECK(ruleWeight(1.29f) == 1.0f);
    CHECK(ruleWeight(0.4f) == 1.0f);
}

} // namespace

UBIQUITTY_TEST_MAIN()

// The face policy's drop: a rule's top sits a clear gap below the baseline,
// 0.85 of the rule's weight in whole rows and never under one. The face's
// declared position is not folded into the drop, which knows no baseline; the
// builder lowers the rule to it where it is lower
// (underlineTopNoHigherThanDeclared, tested below). The weight is the face's own
// in whole rows. Faces as measured on macOS 26.4 at 2x.
TEST(faceRuleSitsAGapBelowTheBaseline)
{
    using Ubiquitty::Rendering::RuleFaceMetrics;
    using Ubiquitty::Rendering::underlineStrokeLiftForEm;
    struct Case {
        const char* face;
        float em;
        float thickness;
        float descent;
        float leading;
        int top;
    };
    // em, thickness, descent, leading in device pixels (2x); the top row below
    // the baseline, round(0.85 * weight) floored at one, where the weight is
    // the thickness in whole rows and at least a point (two rows at 2x).
    const Case cases[] = {
        { "Menlo 24pt", 48.0f, 2.109f, 11.32f, 0.0f, 2 },
        { "Menlo 12pt", 24.0f, 1.055f, 5.66f, 0.0f, 2 },
        { "System UI 24pt", 48.0f, 2.812f, 10.12f, 0.0f, 3 },
        { "JetBrains Mono 24pt", 48.0f, 2.40f, 14.40f, 0.0f, 2 },
        { "JetBrains Mono 12pt", 24.0f, 1.20f, 7.20f, 0.0f, 2 },
        { "Monaspace Neon 24pt", 48.0f, 1.728f, 9.60f, 4.80f, 2 },
        { "Monaspace Neon 12pt", 24.0f, 0.864f, 4.80f, 2.40f, 2 },
        { "System UI 12pt", 24.0f, 1.406f, 5.06f, 0.0f, 2 },
    };
    for (const Case& c : cases) {
        RuleFaceMetrics face;
        face.underlineThickness = c.thickness;
        face.descent = c.descent;
        face.leading = c.leading;
        const RuleGeometry g = ruleGeometry(c.em, 2.0f, face);
        // The face's own weight in whole rows, at least a point.
        CHECK_MSG(close(g.underlineThickness, std::max(2.0f, std::round(c.thickness))), c.face);
        // The top row: the drop with the builder's lift taken back off.
        const int top = static_cast<int>(std::round(g.underlineDrop - underlineStrokeLiftForEm(c.em)));
        CHECK_MSG(top == c.top, c.face);
        // Curly and strike take the face's stroke too.
        CHECK(close(g.curlyThickness, g.underlineThickness));
        CHECK(close(g.strikeThickness, g.underlineThickness));
    }
}

// A face that answers nothing keeps the size-only geometry, field by field: an
// engine that knows the descent but not the weight (the web arm) takes the
// placement and keeps the size-only stroke.
TEST(faceRuleFallsBackFieldByField)
{
    using Ubiquitty::Rendering::RuleFaceMetrics;
    const RuleGeometry sizeOnly = ruleGeometry(24.0f, 2.0f);
    const RuleGeometry empty = ruleGeometry(24.0f, 2.0f, RuleFaceMetrics {});
    CHECK(close(empty.underlineThickness, sizeOnly.underlineThickness));
    CHECK(close(empty.underlineDrop, sizeOnly.underlineDrop));
    RuleFaceMetrics descentOnly;
    descentOnly.descent = 7.2f;
    const RuleGeometry placed = ruleGeometry(24.0f, 2.0f, descentOnly);
    CHECK(close(placed.underlineThickness, sizeOnly.underlineThickness));
    CHECK(!close(placed.underlineDrop, sizeOnly.underlineDrop));
    // A hairline face never draws thinner than the display can: the floor
    // holds under the face policy too.
    RuleFaceMetrics hairline;
    hairline.underlineThickness = 0.3f;
    CHECK(ruleGeometry(12.0f, 1.0f, hairline).underlineThickness >= 1.0f);
}

// The face's declared underline passes through the face policy untouched, for
// the builder to read: its top below the baseline and its thickness. A face
// that declares no position, or one at or above the baseline, declares none,
// and the thickness then goes too, so a font rule with nowhere to stand falls
// back to ours.
TEST(theDeclaredUnderlinePassesThroughTheFacePolicy)
{
    using Ubiquitty::Rendering::RuleFaceMetrics;
    RuleFaceMetrics face;
    face.underscoreWeight = 1.78f;
    face.descent = 7.2f;
    face.underlineThickness = 1.2f;
    face.underlinePosition = 3.72f;
    const RuleGeometry declared = ruleGeometry(24.0f, 2.0f, face);
    CHECK(close(declared.fontUnderlineTop, 3.72f));
    CHECK(close(declared.fontUnderlineThickness, 1.2f));
    CHECK_MSG(close(declared.underlineThickness, 2.0f), "the weight stays the underscore's");

    face.underlinePosition = 0.0f;
    const RuleGeometry undeclared = ruleGeometry(24.0f, 2.0f, face);
    CHECK(undeclared.fontUnderlineTop == 0.0f);
    CHECK_MSG(undeclared.fontUnderlineThickness == 0.0f, "a thickness with no position to hang from is not passed on");
    CHECK_MSG(close(undeclared.underlineDrop, declared.underlineDrop), "the declaration never moves the drop");

    face.underlinePosition = -1.5f;
    CHECK_MSG(ruleGeometry(24.0f, 2.0f, face).fontUnderlineTop == 0.0f, "a rule declared above the baseline counts as none");
}

namespace {

/// Returns the cell metrics an engine reports for a face at 12 points on a 2x
/// display whose underscore draws 1.78 pixels, declaring the given underline.
///
/// - Parameters:
///   - position: The declared top below the baseline, in device pixels; zero
///     for none.
///   - thickness: The declared thickness, in device pixels.
///   - source: The `[underline] metrics` choice.
/// - Returns: The metrics, as an engine copies them from the face policy.
Ubiquitty::Rendering::CellMetrics twelvePointMetrics(float position, float thickness, Ubiquitty::Rendering::RuleMetricsSource source)
{
    Ubiquitty::Rendering::RuleFaceMetrics face;
    face.underscoreWeight = 1.78f;
    face.descent = 7.2f;
    face.underlineThickness = thickness;
    face.underlinePosition = position;
    const RuleGeometry rules = ruleGeometry(24.0f, 2.0f, face);
    Ubiquitty::Rendering::CellMetrics metrics;
    metrics.deviceScale = 2.0f;
    metrics.underlineThickness = rules.underlineThickness;
    metrics.underlineDrop = rules.underlineDrop;
    metrics.underlineStrokeLift = Ubiquitty::Rendering::underlineStrokeLiftForEm(24.0f);
    metrics.fontUnderlineTop = rules.fontUnderlineTop;
    metrics.fontUnderlineThickness = rules.fontUnderlineThickness;
    metrics.underlineMetrics = source;
    return metrics;
}

/// Returns the single underline's rows below a baseline on row 30.
///
/// - Parameter metrics: The cell metrics to place the rule by.
/// - Returns: The rule's top as rows below the baseline, and its weight.
Ubiquitty::Rendering::RuleRows rowsBelowTheBaseline(const Ubiquitty::Rendering::CellMetrics& metrics)
{
    constexpr float baseline = 30.0f;
    const float ruleLine = baseline + metrics.underlineDrop - metrics.underlineStrokeLift;
    Ubiquitty::Rendering::RuleRows rows = Ubiquitty::Rendering::singleUnderlineRows(metrics, baseline, ruleLine, metrics.underlineThickness);
    rows.top -= baseline;
    return rows;
}

} // namespace

// The ubiquitty rule sits the larger of its own clear gap and the face's
// declared position below the baseline, at its own weight. At 12 points on a
// 2x display: JetBrains Mono declares 3.72 pixels, and gets TextEdit's three
// clear rows under the stems where the gap alone leaves two; SF Mono declares
// 2.34, which the gap already reaches; Monaspace Neon declares 0.84, a rule
// crowding its letters, and keeps the gap. A face that declares nothing keeps
// the gap.
TEST(theUbiquittyRuleSitsNoHigherThanTheFaceDeclares)
{
    using Ubiquitty::Rendering::RuleMetricsSource;
    struct Case {
        const char* face;
        float position;
        float thickness;
        float top;
    };
    const Case cases[] = {
        { "none declared", 0.0f, 0.0f, 2.0f },
        { "SF Mono", 2.34f, 1.17f, 2.0f },
        { "JetBrains Mono", 3.72f, 1.20f, 3.0f },
        { "Monaspace Neon", 0.84f, 0.864f, 2.0f },
    };
    for (const Case& c : cases) {
        const auto rows = rowsBelowTheBaseline(twelvePointMetrics(c.position, c.thickness, RuleMetricsSource::Ubiquitty));
        CHECK_MSG(close(rows.top, c.top), c.face);
        CHECK_MSG(close(rows.weight, 2.0f), c.face);
    }

    // The offset moves the declared top as it moves ours: three rows down from
    // a face whose declared top already wins puts the rule six rows down.
    auto offset = twelvePointMetrics(3.72f, 1.20f, RuleMetricsSource::Ubiquitty);
    offset.underlineOffset = 3.0f;
    offset.underlineDrop += 3.0f;
    CHECK(close(rowsBelowTheBaseline(offset).top, 6.0f));
}

// The font rule is the face's declared rectangle snapped outward to whole rows,
// as TextEdit draws it: SF Mono's 2.34 down and 1.17 thick at 12 points on a
// 2x display covers rows 2 and 3 below the baseline, JetBrains Mono's 3.72 and
// 1.20 rows 3 and 4. A face that declares no position keeps ours.
TEST(theFontRuleIsTheDeclaredRuleSnappedOutward)
{
    using Ubiquitty::Rendering::RuleMetricsSource;
    const auto sfMono = rowsBelowTheBaseline(twelvePointMetrics(2.34f, 1.17f, RuleMetricsSource::Font));
    CHECK(close(sfMono.top, 2.0f));
    CHECK(close(sfMono.weight, 2.0f));
    const auto jetBrains = rowsBelowTheBaseline(twelvePointMetrics(3.72f, 1.20f, RuleMetricsSource::Font));
    CHECK(close(jetBrains.top, 3.0f));
    CHECK(close(jetBrains.weight, 2.0f));
    const auto monaspace = rowsBelowTheBaseline(twelvePointMetrics(0.84f, 0.864f, RuleMetricsSource::Font));
    CHECK_MSG(close(monaspace.top, 0.0f), "the font rule follows the face even where it crowds the letters");
    CHECK(close(monaspace.weight, 2.0f));

    const auto undeclared = rowsBelowTheBaseline(twelvePointMetrics(0.0f, 1.2f, RuleMetricsSource::Font));
    const auto ours = rowsBelowTheBaseline(twelvePointMetrics(0.0f, 1.2f, RuleMetricsSource::Ubiquitty));
    CHECK_MSG(close(undeclared.top, ours.top) && close(undeclared.weight, ours.weight), "a face declaring no position keeps ours");

    // A thin declaration still draws a row, and an edge a rounding error past
    // a row's boundary does not take another row.
    const auto hairline = rowsBelowTheBaseline(twelvePointMetrics(2.0f, 0.1f, RuleMetricsSource::Font));
    CHECK(close(hairline.top, 2.0f));
    CHECK(close(hairline.weight, 1.0f));
    const auto exact = rowsBelowTheBaseline(twelvePointMetrics(2.0f + 1e-5f, 1.0f, RuleMetricsSource::Font));
    CHECK(close(exact.top, 2.0f));
    CHECK(close(exact.weight, 1.0f));
}

// The declared position is measured from the baseline the glyphs stand on.
// The atlas rounds every glyph's pen to a whole row, so measuring from a
// layout baseline 0.4 of a row below the glyphs' would set JetBrains Mono's
// 3.72-pixel rule a row under TextEdit's.
TEST(theDeclaredUnderlineIsMeasuredFromTheGlyphsBaseline)
{
    using Ubiquitty::Rendering::RuleMetricsSource;
    for (const auto source : { RuleMetricsSource::Ubiquitty, RuleMetricsSource::Font }) {
        const auto metrics = twelvePointMetrics(3.72f, 1.2f, source);
        for (const float layoutBaseline : { 29.6f, 30.0f, 30.4f }) {
            const float ruleLine = layoutBaseline + metrics.underlineDrop - metrics.underlineStrokeLift;
            const auto rows = Ubiquitty::Rendering::singleUnderlineRows(metrics, layoutBaseline, ruleLine, metrics.underlineThickness);
            // The glyphs stand on row boundary 30 in every case; three clear
            // rows (30, 31, 32) and the rule from row 33.
            CHECK_MSG(rows.top == 33.0f, source == RuleMetricsSource::Font ? "font" : "ubiquitty");
        }
    }
}
