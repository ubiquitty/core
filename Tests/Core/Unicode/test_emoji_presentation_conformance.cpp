// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_emoji_presentation_conformance.cpp — the Emoji_Presentation property
// against the Unicode Consortium's emoji-data.txt.
//
// isEmojiPresentation drives font and colour-path selection (the Metal and web
// glyph rasterizers), not cell geometry: the generated wideRanges makes an emoji
// two cells regardless, so a code point missing the property renders at the
// right width in a text-presentation font — monochrome, or a fallback box. Width
// cannot see that; only the property tells the colour path the cell is emoji, so
// this asserts the predicate, not the width.
//
// build-grapheme-tables.py emits emojiPresentationRanges from emoji-data.txt.
// The test holds that generated table against the same authority, so an emit bug
// or a skew between the table's UCD version and the vendored corpus is a build
// failure rather than a silent narrow glyph in a text font.
//
// The corpus (emoji-data.txt) is vendored beside this file and its path is
// handed in as EMOJI_DATA_FILE by the build (same reason as the grapheme
// corpus: the compiler cache rewrites __FILE__). It must track the tables'
// version (Width.tables.inc — UCD 17.0.0). Skin-tone modifiers
// (U+1F3FB..U+1F3FF) are Emoji_Presentation but zero-width, so the width overlay
// excludes them to keep the zero-width contract; the test excludes them the same way.

#include "Harness.h"

#include "Core/Unicode/Width.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef EMOJI_DATA_FILE
#error "EMOJI_DATA_FILE must name the vendored emoji-data.txt; see Tests/CMakeLists.txt"
#endif

using namespace Ubiquitty::Unicode;

namespace {

constexpr std::uint32_t skinToneFirst = 0x1F3FB;
constexpr std::uint32_t skinToneLast = 0x1F3FF;

// The Emoji_Presentation=Yes code points from emoji-data.txt, minus the
// zero-width skin-tone modifiers.
std::vector<std::uint32_t> emojiPresentationCodePoints()
{
    std::vector<std::uint32_t> out;
    std::ifstream input(EMOJI_DATA_FILE);
    std::string raw;
    while (std::getline(input, raw)) {
        const std::string body = raw.substr(0, raw.find('#'));
        if (body.find("; Emoji_Presentation") == std::string::npos)
            continue;
        const std::string field = body.substr(0, body.find(';'));
        const auto dots = field.find("..");
        const std::uint32_t first = static_cast<std::uint32_t>(std::strtoul(field.c_str(), nullptr, 16));
        const std::uint32_t last = dots == std::string::npos
            ? first
            : static_cast<std::uint32_t>(std::strtoul(field.c_str() + dots + 2, nullptr, 16));
        for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint) {
            if (codePoint >= skinToneFirst && codePoint <= skinToneLast)
                continue;
            out.push_back(codePoint);
        }
    }
    return out;
}

} // namespace

// Every Emoji_Presentation code point the property covers must carry the bit,
// so the colour path routes it to an emoji font. A code point the generated
// table drops still passes every width check — the width is right and the
// presentation is wrong — so a width assertion cannot see it; only this can.
TEST(everyEmojiPresentationCodePointCarriesTheBit)
{
    const std::vector<std::uint32_t> points = emojiPresentationCodePoints();
    REQUIRE(points.size() > 1000); // the corpus parsed and yielded the set

    int missing = 0;
    for (std::uint32_t codePoint : points) {
        if (!Width::isEmojiPresentation(codePoint)) {
            if (missing < 20)
                std::printf("FAIL U+%04X: Emoji_Presentation in the UCD, isEmojiPresentation() is false\n", codePoint);
            ++missing;
        }
    }
    std::printf("emoji-presentation: %zu code points scored, %d missing the bit\n", points.size(), missing);
    CHECK_MSG(missing == 0, "every Emoji_Presentation code point sets the isEmojiPresentation bit");

    // The documented exclusion holds in both directions: a skin-tone modifier
    // is Emoji_Presentation in the UCD but stays zero-width and unmarked here.
    CHECK(!Width::isEmojiPresentation(skinToneFirst));
    CHECK_EQ(Width::forCodePoint(skinToneFirst, Width::AmbiguousWidth::Narrow), 0);
}

// The property bit and the cell width are emitted as two arrays from one parse
// of emoji-data.txt. A code point the bit calls emoji must measure two cells;
// this catches a bit set on a narrow cell without needing the corpus, and it
// fails the moment the two arrays disagree in that direction.
TEST(theEmojiPresentationBitImpliesTwoCells)
{
    int inconsistent = 0;
    for (std::uint32_t codePoint = 0; codePoint < 0x110000; ++codePoint) {
        if (Width::isEmojiPresentation(codePoint) && Width::forCodePoint(codePoint, Width::AmbiguousWidth::Narrow) != 2) {
            if (inconsistent < 20)
                std::printf("FAIL U+%04X: isEmojiPresentation but width %d\n", codePoint, Width::forCodePoint(codePoint, Width::AmbiguousWidth::Narrow));
            ++inconsistent;
        }
    }
    CHECK_MSG(inconsistent == 0, "the emoji-presentation bit never sits on a narrow cell");
}

UBIQUITTY_TEST_MAIN()
