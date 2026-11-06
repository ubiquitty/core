// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The classification loop, run against a fake shaper whose rules are the
// test's to state. The loop is Core policy — which candidates are tried, what
// disqualifies one, the defaults fallback — and it existed once per arm until
// the arms disagreed: one ran all of it, one none, and every Monaspace-class
// face rendered differently per window kind.

#include "Harness.h"

#include "Core/Rendering/LigatureClassification.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

/// A shaper whose faces are rule tables: glyph id == code point, except where
/// a rule moves it. The rules name real font shapes — a stylistic-set face
/// (Monaspace: ligatures behind ssNN), an alternates set that also touches
/// letters (disqualified), and a default-on calt face (Fira).
struct FakeShaper {
    // Which tag turns operators into ligature forms without touching letters.
    std::string ligatureTag;
    // Which tag substitutes letters besides, so it must be rejected.
    std::string letterAlternatesTag;
    // Whether the face ligates by default (calt-style), detected by the
    // all-off fallback rather than by any qualifying tag.
    bool doesLigateByDefault = false;

    static int shape(void* context, const char* text, const char* featureTag, int featureValue, ProbeGlyphSpan outGlyphs)
    {
        const FakeShaper& shaper = *static_cast<const FakeShaper*>(context);
        const std::string_view probe { text };
        const std::string_view tag { featureTag ? featureTag : "" };
        int count = 0;
        const bool isOperatorProbe = probe.find("=>") != std::string_view::npos
            || probe.find("==") != std::string_view::npos;
        // The face's default rendering already carries ligature forms when the
        // face is calt-style; a tag explicitly OFF removes them again.
        const bool isLigated = shaper.doesLigateByDefault
            ? !(tag == "calt" && featureValue == 0)
            : (tag == shaper.ligatureTag && featureValue == 1);
        const bool didMoveLetters = tag == shaper.letterAlternatesTag && featureValue == 1;
        for (const char c : probe) {
            if (count >= static_cast<int>(outGlyphs.size()))
                return -1;
            std::uint32_t glyph = static_cast<std::uint32_t>(c);
            if (isLigated && isOperatorProbe && (c == '=' || c == '<' || c == '>'))
                glyph += 1000;
            if (didMoveLetters && c >= 'a' && c <= 'z')
                glyph += 2000;
            outGlyphs[static_cast<std::size_t>(count++)] = glyph;
        }
        return count;
    }
};

} // namespace

// A stylistic-set face: the set that moves operators and leaves letters alone
// qualifies; the set that also moves letters is rejected, whatever it does to
// operators.
TEST(aStylisticSetFaceQualifiesTheOperatorOnlySet)
{
    FakeShaper monaspaceLike;
    monaspaceLike.ligatureTag = "ss02";
    monaspaceLike.letterAlternatesTag = "ss01";
    const LigatureClassification found = classifyLigatureFeatures(&FakeShaper::shape, &monaspaceLike);
    CHECK_MSG(found.doesLigate, "the face ligates through its set");
    CHECK_MSG(found.features.size() == 1, "exactly one set qualified");
    CHECK_MSG(found.features[0].tag == "ss02" && found.features[0].value == 1,
        "and it is the operator-only one");
}

// A calt face ligates by default, so no candidate qualifies — the all-off
// fallback is what reports that the face ligates at all.
TEST(aDefaultOnFaceIsDetectedByTheAllOffFallback)
{
    FakeShaper firaLike;
    firaLike.doesLigateByDefault = true;
    const LigatureClassification found = classifyLigatureFeatures(&FakeShaper::shape, &firaLike);
    CHECK_MSG(found.features.empty(), "no candidate moved the probes");
    CHECK_MSG(found.doesLigate, "yet the face is known to ligate");
}

// A face that never ligates answers empty both ways.
TEST(aPlainFaceClassifiesEmpty)
{
    FakeShaper plain;
    const LigatureClassification found = classifyLigatureFeatures(&FakeShaper::shape, &plain);
    CHECK_MSG(found.features.empty() && !found.doesLigate, "nothing qualified and nothing ligates");
}

// The merge: an override naming a probed tag wins, a new tag appends, and the
// base order survives — one spelling of the fold both arms read.
TEST(theFeatureMergeLetsTheReaderOverrideTheProbe)
{
    FontFeatureSettings probed;
    probed.push_back({ "ss01", 1 });
    probed.push_back({ "ss02", 1 });
    FontFeatureSettings user;
    user.push_back({ "ss01", 0 });
    user.push_back({ "zero", 1 });
    const FontFeatureSettings folded = mergeFontFeatureSettings(probed, user);
    CHECK_MSG(folded.size() == 3, "two probed, one appended");
    CHECK_MSG(folded[0].tag == "ss01" && folded[0].value == 0, "the reader's off wins over the probe's on");
    CHECK_MSG(folded[1].tag == "ss02" && folded[1].value == 1, "an untouched probe entry survives");
    CHECK_MSG(folded[2].tag == "zero" && folded[2].value == 1, "and the reader's new tag appends");
}

UBIQUITTY_TEST_MAIN()
