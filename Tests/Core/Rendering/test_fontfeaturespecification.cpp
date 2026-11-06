// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <algorithm>

// test_fontfeaturespecification.cpp — host tests for the portable font.features
// allow/block parser (Source/Core/Rendering/FontFeatureSpecification.{h,cpp}).
//
// Pins the grammar -> resolved (tag -> value) mapping the macOS CoreText path
// feeds to kCTFontFeatureSettingsAttribute: bare/+tag = on (1), -tag = off (0),
// tag=N = explicit value, `ssN` zero-padding, comma/space splitting, last-wins
// de-dup, the bare comma list, and the canonical round-trip. No platform types,
// links only FontFeatureSpecification.cpp.

#include "Core/Rendering/FontFeatureSpecification.h"

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

using Ubiquitty::Rendering::canonicalFontFeatureSpecification;
using Ubiquitty::Rendering::FontFeatureSetting;
using Ubiquitty::Rendering::parseFontFeatureSpecification;
using Ubiquitty::Rendering::serializeFontFeatureSettings;

// Look up the resolved value for `tag` (-1 if the tag is absent).
static int valueOf(const std::vector<FontFeatureSetting>& v, const std::string& tag)
{
    for (const auto& s : v)
        if (s.tag == tag)
            return s.value;
    return -1;
}

/// A tag a specification must resolve and the value it must resolve to, with
/// -1 meaning the tag must be absent, and what the expectation demonstrates.
struct Resolution {
    const char* tag;
    int value;
    const char* meaning;
};

/// Checks each expected resolution against the parsed settings, one assertion
/// per tag so a failure names the tag that went wrong.
static void checkResolutions(const std::vector<FontFeatureSetting>& settings, std::span<const Resolution> expected)
{
    for (const Resolution& resolution : expected)
        CHECK_MSG(valueOf(settings, resolution.tag) == resolution.value, resolution.meaning);
}

/// ---- empty / whitespace yields no settings: an unset preference leaves the
/// faces' own default features in force. ------------------------------------
TEST(testEmpty)
{
    for (const char* tagless : { "", "   \t \n", ",, , " })
        CHECK_MSG(parseFontFeatureSpecification(tagless).empty(), "nothing, blanks, or bare separators resolve to no settings");
}

/// ---- a plain comma list of bare tags resolves to all-ENABLED, since a bare
/// tag is on. ---------------------------------------------------------------
TEST(testLegacyEnableOnly)
{
    auto v = parseFontFeatureSpecification("zero,ss01");
    CHECK_EQ(v.size(), 2u);
    const Resolution expected[] = {
        { "zero", 1, "the first tag of a comma list is enabled" },
        { "ss01", 1, "and so is the second" },
    };
    checkResolutions(v, expected);
}

/// ---- the allow/block grammar: bare and +tag enable, -tag disables. The
/// disabled tag rides into CoreText as value 0 (feature OFF). ---------------
TEST(testAllowBlock)
{
    auto v = parseFontFeatureSpecification("calt liga -dlig ss01");
    CHECK_EQ(v.size(), 4u);
    const Resolution expected[] = {
        { "calt", 1, "a bare calt is allowed" },
        { "liga", 1, "a bare liga is allowed" },
        { "dlig", 0, "a minus sign blocks dlig with value 0" },
        { "ss01", 1, "a bare stylistic set is allowed" },
    };
    checkResolutions(v, expected);
}

/// ---- explicit +tag enable form is equivalent to bare. -------------------
TEST(testPlusEnable)
{
    auto v = parseFontFeatureSpecification("+calt +zero");
    const Resolution expected[] = {
        { "calt", 1, "a plus sign enables calt as a bare tag would" },
        { "zero", 1, "and enables zero the same way" },
    };
    checkResolutions(v, expected);
}

/// ---- spaces, commas, and mixed whitespace all separate tokens. ----------
TEST(testSeparators)
{
    auto v = parseFontFeatureSpecification("calt, liga\tdlig\nzero");
    CHECK_EQ(v.size(), 4u);
    const Resolution expected[] = {
        { "calt", 1, "a token ended by a comma and a space is enabled" },
        { "liga", 1, "a token ended by a tab is enabled" },
        { "dlig", 1, "a token ended by a newline is enabled" },
        { "zero", 1, "the final token is enabled" },
    };
    checkResolutions(v, expected);
}

/// ---- case-insensitive: tags lower-case to their canonical form. ---------
TEST(testCaseInsensitive)
{
    auto v = parseFontFeatureSpecification("CALT Liga -DLIG");
    const Resolution expected[] = {
        { "calt", 1, "an upper-case tag resolves under its lower-case name" },
        { "liga", 1, "so does a mixed-case one" },
        { "dlig", 0, "and a blocked upper-case tag is blocked under its lower-case name" },
    };
    checkResolutions(v, expected);
}

/// ---- `ssN` shorthand zero-pads to `ssNN`; `ssNN` passes through. --------
TEST(testStylisticSetPadding)
{
    auto v = parseFontFeatureSpecification("ss1 ss20 -ss5");
    const Resolution expected[] = {
        { "ss01", 1, "a one-digit set number gains its leading zero" },
        { "ss20", 1, "a two-digit set number is kept as written" },
        { "ss05", 0, "padding applies to a blocked set too" },
    };
    checkResolutions(v, expected);
}

/// ---- explicit value: tag=N enables with N (alternate/parametric value). -
TEST(testExplicitValue)
{
    auto v = parseFontFeatureSpecification("cv01=2 aalt=3 +ss01=5");
    const Resolution expected[] = {
        { "cv01", 2, "an equals sign gives the tag its value" },
        { "aalt", 3, "for any tag" },
        { "ss01", 5, "with or without a leading plus sign" },
    };
    checkResolutions(v, expected);
}

/// ---- a disabled tag with an explicit value stays OFF: blocking wins. ----
TEST(testDisableOverridesValue)
{
    auto v = parseFontFeatureSpecification("-cv01=4");
    CHECK_EQ(valueOf(v, "cv01"), 0);
}

/// ---- duplicate tags collapse: the LAST token wins, so a spec can override
/// an earlier enable with a later block (or vice-versa). -------------------
TEST(testLastWins)
{
    const auto blockedLater = parseFontFeatureSpecification("calt -calt");
    CHECK_EQ(blockedLater.size(), 1u);
    CHECK_MSG(valueOf(blockedLater, "calt") == 0, "a later block overrides an earlier enable");
    CHECK_MSG(valueOf(parseFontFeatureSpecification("-dlig dlig"), "dlig") == 1, "a later enable overrides an earlier block");
    CHECK_MSG(valueOf(parseFontFeatureSpecification("ss01 ss01=7"), "ss01") == 7, "a later value overrides an earlier bare tag");
}

/// ---- malformed tokens are skipped, not fatal: non-4-char tags, bad numbers,
/// stray signs. Valid neighbors still parse. -------------------------------
TEST(testInvalidSkipped)
{
    auto v = parseFontFeatureSpecification("liga toolongtag ab calt cv01=x");
    const Resolution expected[] = {
        { "liga", 1, "a valid tag before the malformed ones is kept" },
        { "calt", 1, "a valid tag between them is kept" },
        { "ab", -1, "a tag shorter than four letters is discarded" },
        { "cv01", -1, "a tag whose value is not a number is discarded" },
    };
    checkResolutions(v, expected);
    // The over-long token leaves no entry, not even under a four-letter prefix of itself.
    CHECK_EQ(v.size(), 2u);
}

/// ---- first-appearance ordering is preserved (stable for persistence). ---
TEST(testOrderPreserved)
{
    auto v = parseFontFeatureSpecification("zero ss01 calt");
    REQUIRE(v.size() == 3);
    const char* const written[] = { "zero", "ss01", "calt" };
    bool isInWrittenOrder = true;
    for (std::size_t position = 0; position < v.size(); ++position)
        isInWrittenOrder = isInWrittenOrder && v[position].tag == written[position];
    CHECK_MSG(isInWrittenOrder, "settings come back in the order their tags were written");
}

/// ---- canonical form: stable, space-separated, normalized; -tag for off,
/// tag=N for explicit values > 1; round-trips back to the same settings. ---
TEST(testCanonical)
{
    const struct {
        const char* written;
        const char* canonical;
        const char* meaning;
    } forms[] = {
        { "CALT, liga ,-DLIG, ss1", "calt liga -dlig ss01", "case, separators, and set padding are normalised" },
        { "cv01=2 -zero", "cv01=2 -zero", "an explicit value and a block are kept as written" },
        // Value 1 is what a bare tag means, so it is written without `=1`.
        { "ss01=1", "ss01", "an explicit value of one is written as the bare tag" },
        { "", "", "an empty specification stays empty" },
    };
    for (const auto& form : forms)
        CHECK_MSG(canonicalFontFeatureSpecification(form.written) == form.canonical, form.meaning);

    // Round-trip: parse(canonical(x)) == parse(x).
    const std::string raw = "Calt, -DLIG, ss3, cv02=4";
    const std::string canonical = canonicalFontFeatureSpecification(raw);
    CHECK_MSG(parseFontFeatureSpecification(canonical) == parseFontFeatureSpecification(raw), "the canonical form parses to the settings it was made from");
}

/// ---- serializeFontFeatureSettings mirrors canonical for a hand-built vector
/// (the path the slashed-zero toggle uses). --------------------------------
TEST(testSerialize)
{
    std::vector<FontFeatureSetting> v = { { "zero", 1 }, { "dlig", 0 }, { "cv01", 3 } };
    CHECK_STR_EQ(serializeFontFeatureSettings(v), "zero -dlig cv01=3");
}

// The composition every frontend needs and none owns: a ligature policy and a
// specification resolved into one ordered list. Shared because the rule is
// portable and only the output is not — the Linux arm maps this to
// `hb_feature_t`, the browser to a CSS descriptor string. Two frontends
// composing it themselves agree until one of them is edited.
TEST(aLigaturePolicyAndASpecificationResolveToOneList)
{
    using Ubiquitty::Rendering::Ligatures;
    using Ubiquitty::Rendering::resolveFontFeatures;

    // Enabled and nothing named asks for nothing. That is not "no features" but
    // "whatever the shaper does by default", which is what enabled means.
    CHECK(resolveFontFeatures(Ligatures::Enabled, "").empty());

    // Disabled names all three tags a shaper turns on by default. One would not
    // do: a face can carry its operators under `calt` alone, so suppressing
    // `liga` by itself changes nothing and reads as a fix.
    const auto suppressed = resolveFontFeatures(Ligatures::Disabled, "");
    CHECK(suppressed.size() == 3);
    if (suppressed.size() == 3) {
        CHECK(suppressed[0].tag == "liga" && suppressed[0].value == 0);
        CHECK(suppressed[1].tag == "clig" && suppressed[1].value == 0);
        CHECK(suppressed[2].tag == "calt" && suppressed[2].value == 0);
    }

    // The specification comes second, after the suppression, which is what makes an
    // explicit feature beat the coarse toggle: both shapers take a later entry
    // as the winner. The duplicate tag is the override and must survive — a
    // resolver that merged here would hand the frontend a list in which the
    // reader had already lost.
    const auto overridden = resolveFontFeatures(Ligatures::Disabled, "+calt");
    CHECK(overridden.size() == 4);
    if (overridden.size() == 4) {
        CHECK(overridden[2].tag == "calt" && overridden[2].value == 0);
        CHECK(overridden[3].tag == "calt" && overridden[3].value == 1);
    }

    // Enabled keeps the specification and adds nothing to it.
    const auto plain = resolveFontFeatures(Ligatures::Enabled, "zero, ss01");
    CHECK(plain.size() == 2);
    if (plain.size() == 2)
        CHECK(plain[0].tag == "zero" && plain[1].tag == "ss01");
}

// The suppression list is not the candidate list, and the difference is the
// point: `dlig` is a ligature feature a classification should TRY, and one a
// suppression must not name, because a shaper does not enable it on its own and
// naming it adds a line to every resolved list to no effect.
TEST(theSuppressionListIsNotTheCandidateList)
{
    const auto suppression = Ubiquitty::Rendering::ligatureSuppressionFeatureTags();
    const auto candidates = Ubiquitty::Rendering::ligatureCandidateFeatureTags();
    CHECK(std::ranges::find(suppression, "dlig") == suppression.end());
    CHECK(std::ranges::find(candidates, "dlig") != candidates.end());
    // And `clig` the other way about: a shaper enables it, so a suppression must
    // name it; a classification has no use for trying it on its own.
    CHECK(std::ranges::find(suppression, "clig") != suppression.end());
}

UBIQUITTY_TEST_MAIN()
