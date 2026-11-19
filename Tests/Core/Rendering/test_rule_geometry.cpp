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
    CHECK(std::fabs(ruleGeometry(28.0f, 2.0f, face).underlineThickness - 1.0f) < 1e-4f);
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

// The face policy: a rule's top sits a clear gap below the baseline, 0.85 of
// the rule's weight in whole rows and never under one, whatever position the
// face declares — Monaspace Neon declares one that all but touches its letters
// and JetBrains Mono one a stroke and a half lower, so following them would
// set the same rule at a different height in every face. The weight is the
// face's own in whole rows. Faces as measured on macOS 26.4 at 2x.
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
    // the thickness in whole rows.
    const Case cases[] = {
        { "Menlo 24pt", 48.0f, 2.109f, 11.32f, 0.0f, 2 },
        { "Menlo 12pt", 24.0f, 1.055f, 5.66f, 0.0f, 1 },
        { "System UI 24pt", 48.0f, 2.812f, 10.12f, 0.0f, 3 },
        { "JetBrains Mono 24pt", 48.0f, 2.40f, 14.40f, 0.0f, 2 },
        { "JetBrains Mono 12pt", 24.0f, 1.20f, 7.20f, 0.0f, 1 },
        { "Monaspace Neon 24pt", 48.0f, 1.728f, 9.60f, 4.80f, 2 },
        { "Monaspace Neon 12pt", 24.0f, 0.864f, 4.80f, 2.40f, 1 },
        { "System UI 12pt", 24.0f, 1.406f, 5.06f, 0.0f, 1 },
    };
    for (const Case& c : cases) {
        RuleFaceMetrics face;
        face.underlineThickness = c.thickness;
        face.descent = c.descent;
        face.leading = c.leading;
        const RuleGeometry g = ruleGeometry(c.em, 2.0f, face);
        // The face's own weight in whole rows, at least one.
        CHECK_MSG(close(g.underlineThickness, std::max(1.0f, std::round(c.thickness))), c.face);
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
