// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_contrast.cpp — pins the portable APCA perceptual contrast floor
// (Source/Core/Rendering/PerceptualContrast.cpp): apcaLc() and contrastFloor(), and the two
// chrome answers built on them — a chrome badge's ink over the title row's wash
// and a filled chip's label.
//
// apcaLc is checked against the canonical APCA-W3 0.1.9 reference pairs (the
// black/white asymmetry ~106 / ~-108 is the signature that distinguishes APCA
// from the WCAG-2 ratio, which would rate both at 21). contrastFloor is checked
// for its three contract properties: it is a no-op when the pair already clears
// the floor; when it does not, it RAISES |Lc| to at least the target; and it
// preserves hue (HSL lightness-only move). A silent regression in the color math
// shows here, not as an illegible hint on a light theme.

#include "Core/Themes/Contrast.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using Ubiquitty::Themes::Contrast::apcaLc;
using Ubiquitty::Themes::Contrast::chromeBadgeInk;
using Ubiquitty::Themes::Contrast::contrastFloor;
using Ubiquitty::Themes::Contrast::labelColorOnFill;
using Ubiquitty::Themes::Contrast::labelDark;
using Ubiquitty::Themes::Contrast::labelLight;
using Ubiquitty::Themes::Contrast::lcBody;
using Ubiquitty::Themes::Contrast::lcSecondary;
using Ubiquitty::Themes::Contrast::minReadingCoverage;
using Ubiquitty::Themes::Contrast::wcagContrastRatio;
using Ubiquitty::Themes::Contrast::wcagMinimumRatio;

// APCA-W3 0.1.9 canonical reference pairs (text/background luminance → Lc). The exact decimals
// are version-pinned; ±1.0 tolerance proves the algorithm is genuinely APCA (and
// not WCAG-2, which is off by tens) without over-fitting a build's last digit.
TEST(testApcaReferenceValues)
{
    // Black text on white: the maximum positive polarity.
    CHECK_NEAR(apcaLc(0x000000, 0xFFFFFF), 106.04, 1.0);
    // White text on black: the maximum negative polarity — note it is not the
    // negation of the above. That asymmetry is APCA's signature.
    CHECK_NEAR(apcaLc(0xFFFFFF, 0x000000), -107.88, 1.0);
    // Mid grays on white / white on mid gray — the dim-metadata regime.
    CHECK_NEAR(apcaLc(0x888888, 0xFFFFFF), 63.06, 1.0);
    CHECK_NEAR(apcaLc(0xFFFFFF, 0x888888), -68.54, 1.0);
    // A designed dark-on-light pair.
    CHECK_NEAR(apcaLc(0x112233, 0xDDEEFF), 91.66, 1.0);
    CHECK_NEAR(apcaLc(0xDDEEFF, 0x112233), -93.07, 1.0);
}

// Identical colors and pairs inside the APCA low-contrast clamp region collapse
// to exactly 0 (no separation to report).
TEST(testApcaZeroRegion)
{
    CHECK(apcaLc(0x808080, 0x808080) == 0.0);
    CHECK(apcaLc(0xFFFFFF, 0xFFFFFF) == 0.0);
    // A pair one step apart is below the low-contrast clip floor → clamped to 0.
    CHECK(apcaLc(0x808080, 0x818181) == 0.0);
}

// Polarity sign: dark-on-light is positive, light-on-dark is negative.
TEST(testApcaPolaritySign)
{
    CHECK(apcaLc(0x222222, 0xEEEEEE) > 0.0);
    CHECK(apcaLc(0xEEEEEE, 0x222222) < 0.0);
}

// contrastFloor is a strict no-op when the pair already clears the floor:
// black-on-white sails past any chrome floor, so the foreground comes back byte-identical.
TEST(testFloorNoopWhenAlreadyLegible)
{
    CHECK(contrastFloor(0x000000, 0xFFFFFF, lcSecondary) == 0x000000u);
    CHECK(contrastFloor(0x000000, 0xFFFFFF, lcBody) == 0x000000u);
    // White-on-black clears on |Lc| too (polarity-agnostic floor).
    CHECK(contrastFloor(0xFFFFFF, 0x000000, lcBody) == 0xFFFFFFu);
}

// The core rescue: a dim gray hint on a light panel that falls BELOW the
// secondary floor is darkened until it clears — and the result actually clears.
TEST(testFloorRescuesDimOnLight)
{
    const std::uint32_t panelBackground = 0xF2F2F2; // light chrome panel
    const std::uint32_t dimHint = 0xB8B8B8; // ~.28 gray, illegible metadata
    CHECK(std::fabs(apcaLc(dimHint, panelBackground)) < lcSecondary); // precondition: it fails
    const std::uint32_t fixed = contrastFloor(dimHint, panelBackground, lcSecondary);
    CHECK(fixed != dimHint); // it moved
    CHECK(std::fabs(apcaLc(fixed, panelBackground)) >= lcSecondary - 0.5); // it now clears (± search step)
    // Darkened, not lightened: on a light background the pole that raises |Lc| is black.
    CHECK(((fixed >> 16) & 0xFF) <= ((dimHint >> 16) & 0xFF));
}

// Same rescue on the dark side: a too-dim gray on a dark panel is lightened.
TEST(testFloorRescuesDimOnDark)
{
    const std::uint32_t panelBackground = 0x1A1A1A; // dark chrome panel
    const std::uint32_t dimHint = 0x555555; // illegible on dark
    CHECK(std::fabs(apcaLc(dimHint, panelBackground)) < lcSecondary);
    const std::uint32_t fixed = contrastFloor(dimHint, panelBackground, lcSecondary);
    CHECK(std::fabs(apcaLc(fixed, panelBackground)) >= lcSecondary - 0.5);
    // Lightened: red channel goes up (toward the white pole).
    CHECK(((fixed >> 16) & 0xFF) >= ((dimHint >> 16) & 0xFF));
}

// Hue is preserved: rescuing a colored hint keeps its hue angle. A muted blue
// hint stays blue (blue remains the dominant channel; it is not desaturated into
// gray).
TEST(testFloorPreservesHue)
{
    const std::uint32_t panelBackground = 0xF2F2F2;
    const std::uint32_t blueHint = 0x9AB0D8; // muted blue, too dim on light
    const std::uint32_t fixed = contrastFloor(blueHint, panelBackground, lcSecondary);
    const int red = (fixed >> 16) & 0xFF, green = (fixed >> 8) & 0xFF, blue = fixed & 0xFF;
    // Blue stays the max channel, red the min — the blue hue ordering survives.
    CHECK(blue >= green && green >= red);
    CHECK(blue > red); // still recognizably blue, not collapsed to gray
    CHECK(std::fabs(apcaLc(fixed, panelBackground)) >= lcSecondary - 0.5);
}

// Monotonic in the right direction: asking for a HIGHER floor never yields less
// contrast than a lower one on the same pair.
TEST(testFloorMonotoneInTarget)
{
    const std::uint32_t panelBackground = 0xF2F2F2;
    const std::uint32_t dimHint = 0xB8B8B8;
    const double atSecondary = std::fabs(apcaLc(contrastFloor(dimHint, panelBackground, lcSecondary), panelBackground));
    const double atBody = std::fabs(apcaLc(contrastFloor(dimHint, panelBackground, lcBody), panelBackground));
    CHECK(atBody >= atSecondary - 0.5);
}

// The WCAG 2 ratio against its own published anchors: black on white is 21,
// and #767676 on white is the canonical "just AA" grey at 4.54. Symmetric,
// which is the property that separates it from APCA above.
TEST(testWcagReferenceValues)
{
    CHECK_NEAR(wcagContrastRatio(0x000000, 0xFFFFFF), 21.0, 0.01);
    CHECK_NEAR(wcagContrastRatio(0xFFFFFF, 0x000000), 21.0, 0.01);
    CHECK_NEAR(wcagContrastRatio(0x767676, 0xFFFFFF), 4.54, 0.01);
    CHECK_NEAR(wcagContrastRatio(0x777777, 0xFFFFFF), 4.48, 0.01);
    CHECK(wcagContrastRatio(0x808080, 0x808080) == 1.0);
}

// The least coverage that reads: the composite at that coverage clears both
// floors and the composite one step under it does not.
TEST(testMinimumReadingCoverageIsTheFirstStepThatReads)
{
    const std::uint32_t ink = 0x32363C, fill = 0xE7E8EB; // Noir Clair's foreground on its band
    const double coverage = minReadingCoverage(ink, fill, lcSecondary, wcagMinimumRatio);
    CHECK(coverage > 0.0 && coverage < 1.0);
    const auto blend = [&](double c) -> std::uint32_t {
        const auto channel = [&](int shift) {
            const int i = (ink >> shift) & 0xFF, f = (fill >> shift) & 0xFF;
            return static_cast<std::uint32_t>(std::lround(i * c + f * (1.0 - c)));
        };
        return (channel(16) << 16) | (channel(8) << 8) | channel(0);
    };
    const auto reads = [&](std::uint32_t candidate) {
        return std::fabs(apcaLc(candidate, fill)) >= lcSecondary && wcagContrastRatio(candidate, fill) >= wcagMinimumRatio;
    };
    CHECK(reads(blend(coverage)));
    CHECK(!reads(blend(coverage - 1.0 / 255.0)));
}

// An ink that does not read at full coverage answers 1.0: all of it, and no more
// is on offer here.
TEST(testMinimumReadingCoverageOfAnInkThatCannotRead)
{
    const std::uint32_t ink = 0x839496, fill = 0x153A45; // Solarized Dark's foreground on its band
    CHECK(std::fabs(apcaLc(ink, fill)) < lcSecondary); // precondition: it fails
    CHECK(minReadingCoverage(ink, fill, lcSecondary, wcagMinimumRatio) == 1.0);
}

// Both floors bind: the answer is the coverage the stricter demands.
TEST(testMinimumReadingCoverageHonoursTheStricterFloor)
{
    const std::uint32_t ink = 0xFFFFFF, fill = 0x1A1A1A;
    const double byBoth = minReadingCoverage(ink, fill, lcSecondary, wcagMinimumRatio);
    const double byRatioOnly = minReadingCoverage(ink, fill, 0.0, wcagMinimumRatio);
    const double byLcOnly = minReadingCoverage(ink, fill, lcSecondary, 1.0);
    // On a dark ground APCA is the stricter: the ratio alone is met sooner, and
    // the Lc alone lands where both do.
    CHECK(byRatioOnly < byBoth);
    CHECK(byLcOnly == byBoth);
}

// A chrome badge keeps the theme's own foreground wherever it reads on the
// wash: a dark theme's grey on its ground and a light theme's charcoal on its
// paper come back byte-identical.
TEST(testChromeBadgeInkKeepsAForegroundThatReads)
{
    CHECK(chromeBadgeInk(0xC0C0C0, 0x1E1E1E) == 0xC0C0C0u);
    CHECK(chromeBadgeInk(0x333333, 0xFAFAFA) == 0x333333u);
}

// A letterbox fill the foreground cannot be told from: a grey foreground on a
// grey wash one step off it has no contrast at all, and the badge's ink is
// walked to the secondary floor — darkened, the pole that reads on that grey.
TEST(testChromeBadgeInkClearsTheFloorOnALetterboxFill)
{
    const std::uint32_t foreground = 0xC0C0C0, letterbox = 0xB4B4B4;
    CHECK(std::fabs(apcaLc(foreground, letterbox)) < lcSecondary); // precondition: it fails
    const std::uint32_t ink = chromeBadgeInk(foreground, letterbox);
    CHECK(ink != foreground);
    CHECK(std::fabs(apcaLc(ink, letterbox)) >= lcSecondary - 0.5);
    CHECK(((ink >> 16) & 0xFF) < ((foreground >> 16) & 0xFF));
}

// The walk keeps the theme's hue: Solarized's blue-grey foreground on a fill
// of its own tone lightens toward white and stays blue-grey — its blue and
// green channels still above its red.
TEST(testChromeBadgeInkKeepsTheForegroundsHue)
{
    const std::uint32_t foreground = 0x839496, letterbox = 0x7A8A8C;
    const std::uint32_t ink = chromeBadgeInk(foreground, letterbox);
    const int red = (ink >> 16) & 0xFF, green = (ink >> 8) & 0xFF, blue = ink & 0xFF;
    CHECK(std::fabs(apcaLc(ink, letterbox)) >= lcSecondary - 0.5);
    CHECK(red > 0x83);
    CHECK(blue >= green && green > red);
}

// The theme's own ground is a wash like any other: Solarized Dark's foreground
// sits under the secondary floor on its background, and the badge lifts it.
TEST(testChromeBadgeInkFloorsAgainstTheThemesOwnBackground)
{
    const std::uint32_t foreground = 0x839496, background = 0x002B36;
    CHECK(std::fabs(apcaLc(foreground, background)) < lcSecondary); // precondition: it fails
    const std::uint32_t ink = chromeBadgeInk(foreground, background);
    CHECK(ink != foreground);
    CHECK(std::fabs(apcaLc(ink, background)) >= lcSecondary - 0.5);
}

// A chip's label is the pole with the greater |Lc| on its fill, and on the
// saturated mid-tones that is not what a weighted-channel sum says: blue,
// purple and pink each weigh in above half on Rec. 601 luma, which would set
// them in near-black, and each carries white. Orange and yellow go the other
// way and carry near-black.
TEST(testLabelOnFillPartsCompanyWithALumaSum)
{
    const auto luma = [](std::uint32_t fill) {
        return (0.299 * ((fill >> 16) & 0xFF) + 0.587 * ((fill >> 8) & 0xFF) + 0.114 * (fill & 0xFF)) / 255.0;
    };
    for (const std::uint32_t fill : { 0x5291E6u, 0x9475DBu, 0xDE75B3u }) {
        CHECK(luma(fill) > 0.5); // precondition: a luma threshold picks the dark ink
        CHECK(labelColorOnFill(fill) == labelLight);
        CHECK(std::fabs(apcaLc(labelLight, fill)) > std::fabs(apcaLc(labelDark, fill)));
    }
    CHECK(labelColorOnFill(0xF08F38) == labelDark);
    CHECK(labelColorOnFill(0xE6C247) == labelDark);
    CHECK(labelColorOnFill(0xDB4D57) == labelLight);
}

TEST(aLoudTagWashesTheChromeWithoutTurningItsTheme)
{
    using Ubiquitty::Themes::Contrast::loudTagWash;
    const auto luminance = [](std::uint32_t rgb) {
        return 0.2126 * ((rgb >> 16) & 0xFF) + 0.7152 * ((rgb >> 8) & 0xFF) + 0.0722 * (rgb & 0xFF);
    };
    const std::uint32_t red = 0xE05050;
    const std::uint32_t darkGround = 0x15171C;
    const std::uint32_t lightGround = 0xF4F5F8;
    const std::uint32_t onDark = loudTagWash(red, darkGround);
    const std::uint32_t onLight = loudTagWash(red, lightGround);
    CHECK_MSG(luminance(onDark) < 80, "a dark theme's chrome stays dark under a loud tag");
    CHECK_MSG(luminance(onLight) > 180, "and a light theme's stays light");
    CHECK_MSG(((onDark >> 16) & 0xFF) > ((onDark >> 8) & 0xFF) + 20, "while the tag's colour still shows in it");
    CHECK_MSG(((onLight >> 16) & 0xFF) > ((onLight >> 8) & 0xFF) + 20, "on either ground");
    CHECK_EQ(loudTagWash(red, red), red);
    CHECK_EQ(loudTagWash(0x000000, 0x000000), 0x000000U);
    CHECK_EQ(loudTagWash(0xFFFFFF, 0x000000), 0x4D4D4DU);
}

UBIQUITTY_TEST_MAIN()
