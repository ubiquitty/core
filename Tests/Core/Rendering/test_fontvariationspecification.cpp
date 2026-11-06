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
