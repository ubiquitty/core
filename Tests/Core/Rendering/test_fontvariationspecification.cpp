// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_fontvariationspecification.cpp — host tests for the portable
// font.variations axis parser and the precedence fold
// (Source/Core/Rendering/FontVariationSpecification.{h,cpp}).
//
// Pins the grammar -> resolved (tag -> value) mapping both shells feed to
// their variation dictionaries: `tag=N` with a signed decimal value, comma
// and space splitting, last-wins de-dup, case preservation, the canonical
// round-trip, and the fold where a user's tag beats a derived one. No
// platform types, links only FontVariationSpecification.cpp.

#include "Core/Rendering/FontVariationSpecification.h"

#include <string>

using Ubiquitty::Rendering::FontVariationSetting;
using Ubiquitty::Rendering::FontVariationSettings;
using Ubiquitty::Rendering::mergeFontVariationSettings;
using Ubiquitty::Rendering::parseFontVariationSpecification;
using Ubiquitty::Rendering::serializeFontVariationSettings;

// Look up the resolved value for `tag`; absence answers a sentinel no axis
// scale uses.
static double valueOf(const FontVariationSettings& v, const std::string& tag)
{
    for (const auto& s : v)
        if (s.tag == tag)
            return s.value;
    return -99999.0;
}

TEST(testEmptyNamesNoAxis)
{
    CHECK_MSG(parseFontVariationSpecification("").empty(), "empty spec -> {}");
    CHECK_MSG(parseFontVariationSpecification("  \t \n").empty(), "whitespace spec -> {}");
}

TEST(testGrammar)
{
    const auto v = parseFontVariationSpecification("slnt=-6 opsz=14.5, wght=450");
    CHECK(v.size() == 3);
    CHECK(valueOf(v, "slnt") == -6.0);
    CHECK(valueOf(v, "opsz") == 14.5);
    CHECK(valueOf(v, "wght") == 450.0);

    // Case is preserved: registered axes are lower-case by the OpenType
    // specification and custom axes upper-case, so `GRAD` names a different
    // axis than `grad` would.
    const auto custom = parseFontVariationSpecification("GRAD=88");
    CHECK(custom.size() == 1 && custom[0].tag == "GRAD" && custom[0].value == 88.0);

    // Later tokens for one tag win, in first-appearance order.
    const auto dup = parseFontVariationSpecification("slnt=-6 slnt=-11");
    CHECK(dup.size() == 1 && dup[0].value == -11.0);
}

TEST(testMalformedTokensAreSkippedNotRefused)
{
    // A reader typing by hand keeps the axes they spelled correctly.
    const auto v = parseFontVariationSpecification("slnt opsz= =14 sl=1 wght=45x toolong=1 wdth=125");
    CHECK(v.size() == 1);
    CHECK(valueOf(v, "wdth") == 125.0);
    // Exponents and second points are not part of the grammar.
    CHECK(parseFontVariationSpecification("opsz=1e2").empty());
    CHECK(parseFontVariationSpecification("opsz=1.2.3").empty());
}

// The refusal paths of the value grammar that testMalformedTokensAreSkippedNotRefused
// does not reach (it pins trailing characters). Each one rejects rather than
// accepts a number it half-read: a parse that fell through would hand the face
// an axis value assembled from the digits it happened to consume.
TEST(malformedValuesAndTagsAreRefusedRatherThanHalfRead)
{
    CHECK_MSG(parseFontVariationSpecification("wght=+").empty(), "a sign with no digits after it");
    CHECK_MSG(parseFontVariationSpecification("wght=-").empty(), "and the same for a minus");
    CHECK_MSG(parseFontVariationSpecification("wght=1234567").empty(), "an integral part past any axis scale");
    CHECK_MSG(parseFontVariationSpecification("opsz=1.234567").empty(), "more fractional digits than the scale carries");
    CHECK_MSG(parseFontVariationSpecification("opsz=.").empty(), "a point with digits on neither side");
    CHECK_MSG(parseFontVariationSpecification("wght=1.").empty(), "integral digits followed by a bare point");

    // A tag is four ASCII graphic characters; a control byte inside one is the
    // case a length check alone would pass.
    CHECK_MSG(parseFontVariationSpecification(std::string("wg\x01t=400")).empty(), "a control byte inside the tag");

    // And the good value beside them, so the test cannot pass by refusing
    // everything.
    const auto good = parseFontVariationSpecification("wght=400");
    CHECK(good.size() == 1 && good[0].value == 400.0);
}

// Each limit of the value grammar is accepted at the limit and refused one step
// past it.
TEST(valueLimitsAreInclusiveAtTheEdge)
{
    CHECK_MSG(valueOf(parseFontVariationSpecification("wght=100000"), "wght") == 100000.0,
        "the largest integral part is accepted");
    CHECK_MSG(parseFontVariationSpecification("wght=100001").empty(), "and one past it is refused");
    CHECK_MSG(valueOf(parseFontVariationSpecification("opsz=1.2345"), "opsz") == 1.2345,
        "four fractional digits fill the scale exactly");
    CHECK_MSG(parseFontVariationSpecification("opsz=1.23456").empty(), "and a fifth is refused");
}

TEST(testCanonicalRoundTrip)
{
    const auto v = parseFontVariationSpecification("slnt=-6,opsz=14.50 , wght=450");
    const std::string canonical = serializeFontVariationSettings(v);
    CHECK_MSG(canonical == "slnt=-6 opsz=14.5 wght=450", "canonical spelling is stable");
    CHECK_MSG(parseFontVariationSpecification(canonical) == v, "canonical form round-trips");
}

TEST(testUserAxisBeatsDerivedAxis)
{
    // font.weight and font.width derive wght and wdth; a variation naming the
    // same tag is the more specific instruction and wins — the precedence CSS
    // gives font-variation-settings over its high-level properties.
    FontVariationSettings derived { { "wdth", 112.5 }, { "wght", 400.0 } };
    const auto merged = mergeFontVariationSettings(derived, parseFontVariationSpecification("wght=450 slnt=-6"));
    CHECK(merged.size() == 3);
    CHECK(valueOf(merged, "wdth") == 112.5);
    CHECK(valueOf(merged, "wght") == 450.0);
    CHECK(valueOf(merged, "slnt") == -6.0);
    // The fold mutates neither input.
    CHECK(derived.size() == 2 && valueOf(derived, "wght") == 400.0);
}

UBIQUITTY_TEST_MAIN()
