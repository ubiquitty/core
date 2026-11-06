// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_xparsecolor_degenerate — the colour-space paths a client can steer into.
//
// `XParseColor.cpp` has the worst branch coverage of any wire-facing Core parser,
// and the missing branches are the degenerate arithmetic inside the Xcms
// conversions: a denominator that comes out zero, a luminance below the CIE
// knee. Those exist because libX11 has them, and the file's whole
// contract is to reproduce Xlib's answers rather than to compute correct ones.
//
// So the question is not whether a branch is covered but whether a client can
// reach it, and the two device-independent forms differ:
//
//     CIExyY   x, y and Y are each range-checked to [0, 1]
//     CIEuvY   only Y is; u' and v' are taken as given
//
// `xyyToXYZ`'s degenerate branch needs `-2x + 12y + 3 == 0`, so `y = (2x-3)/12`,
// which over x in [0, 1] is between -0.25 and -1/12 — always negative, never
// admitted. That branch is unreachable through the parser, and no case here
// pretends otherwise.
//
// `uvyToXYZ`'s needs `6u' - 16v' + 12 == 0`, and nothing checks u' or v'. That
// one a client reaches, so it gets cases.

#include "Core/OSC/XParseColor.h"

#include <cstdint>

using Ubiquitty::OSC::XParseColor::parseColorSpecification;

namespace {

struct Parsed {
    bool isAccepted;
    std::uint16_t red, green, blue;
};

/// Parses `spec`, reporting whether it was accepted and what it produced.
Parsed parse(const char* spec)
{
    Parsed result { false, 0, 0, 0 };
    const std::optional<Ubiquitty::PaletteRGB> parsed = parseColorSpecification(spec);
    result.isAccepted = parsed.has_value();
    if (parsed) {
        result.red = parsed->red;
        result.green = parsed->green;
        result.blue = parsed->blue;
    }
    return result;
}

} // namespace

TEST(aDegenerateChromaticitySubstitutesTheWhitePoint)
{
    // Two specifications that both zero the denominator, chosen from opposite
    // ends of the line `6u' - 16v' + 12 == 0`. Xcms substitutes the white point
    // rather than dividing, so both must land on the same colour — which is what
    // says the substitution happened and not that the arithmetic happened to
    // agree.
    const Parsed first = parse("CIEuvY:0.0/0.75/0.5");
    const Parsed second = parse("CIEuvY:0.2/0.825/0.5");
    CHECK(first.isAccepted);
    CHECK(second.isAccepted);
    CHECK_EQ(static_cast<int>(first.red), static_cast<int>(second.red));
    CHECK_EQ(static_cast<int>(first.green), static_cast<int>(second.green));
    CHECK_EQ(static_cast<int>(first.blue), static_cast<int>(second.blue));

    // And an ordinary chromaticity at the same luminance is a different colour,
    // so the pair above agree because of the substitution rather than because
    // every input at this luminance answers the same.
    const Parsed ordinary = parse("CIEuvY:0.2/0.3/0.5");
    CHECK(ordinary.isAccepted);
    CHECK(ordinary.red != first.red || ordinary.green != first.green || ordinary.blue != first.blue);

    // Every channel a client reads back is a replicated byte, which is the
    // file's stated contract and holds on this path too.
    CHECK_EQ(static_cast<int>(first.red & 0xFF), static_cast<int>(first.red >> 8));
    CHECK_EQ(static_cast<int>(first.green & 0xFF), static_cast<int>(first.green >> 8));
    CHECK_EQ(static_cast<int>(first.blue & 0xFF), static_cast<int>(first.blue >> 8));
}

TEST(theDegenerateBranchStillHonoursItsLuminance)
{
    // The substitution replaces the chromaticity and keeps Y, so a degenerate
    // specification at zero luminance is black rather than the white point.
    // Without this the case above would pass on a conversion that discarded Y.
    const Parsed dark = parse("CIEuvY:0.0/0.75/0.0");
    CHECK(dark.isAccepted);
    CHECK_EQ(static_cast<int>(dark.red), 0);
    CHECK_EQ(static_cast<int>(dark.green), 0);
    CHECK_EQ(static_cast<int>(dark.blue), 0);
}

TEST(theTwoDeviceIndependentFormsDisagreeAboutRangeChecking)
{
    // CIEuvY takes u' and v' as given, so a client can drive the conversion with
    // values no chromaticity diagram contains and still get a colour back.
    CHECK(parse("CIEuvY:-5.0/0.0/0.5").isAccepted);
    CHECK(parse("CIEuvY:99.0/99.0/0.5").isAccepted);

    // CIExyY range-checks all three, so the same shape is refused. Pinned
    // because it is what makes `xyyToXYZ`'s degenerate branch unreachable: it
    // needs a negative y, and a negative y never gets past here.
    CHECK(!parse("CIExyY:1.5/0.0/0.5").isAccepted);
    CHECK(!parse("CIExyY:0.0/-0.25/0.5").isAccepted);
    CHECK(parse("CIExyY:0.3/0.3/0.5").isAccepted);

    // Luminance is checked on both, so the difference above is about the
    // chromaticity pair and not about the form being unchecked in general.
    CHECK(!parse("CIEuvY:0.2/0.3/1.5").isAccepted);
    CHECK(!parse("CIExyY:0.3/0.3/1.5").isAccepted);
}

UBIQUITTY_TEST_MAIN()
