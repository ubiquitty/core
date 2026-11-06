// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_range_map_write.cpp — writing `[[font.range_map]]` back to config.toml.
//
// Every case that claims the writer produced something usable proves it by
// parsing the result with the real reader rather than by comparing strings: the
// reader drops an entry failing isValidRangeMap without reporting it, so output
// that reads correctly and parses to nothing satisfies a string comparison and
// still routes no glyph.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/RangeMapWrite.h"

#include <sstream>
#include <string>

using Ubiquitty::Preferences::formatCodePointRange;
using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::removeRangeMap;
using Ubiquitty::Preferences::renderRangeMapBlock;
using Ubiquitty::Preferences::spliceRangeMap;
using Ubiquitty::Preferences::Values;

namespace {

Values parsed(const std::string& body)
{
    Values values;
    std::istringstream input(body);
    parse(input, values);
    return values;
}

// The routing the file actually expresses, which is the only thing a caller
// cares about: which family answers this code point, empty for none.
std::string familyFor(const Values& values, std::uint32_t codePoint)
{
    for (const auto& entry : values.rangeMap) {
        if (entry.firstCodePoint <= codePoint && codePoint <= entry.lastCodePoint)
            return entry.family;
    }
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

} // namespace

TEST(testRangeSpellingIsOneTheReaderAccepts)
{
    CHECK(formatCodePointRange(0x2318, 0x2318) == "U+2318");
    CHECK(formatCodePointRange(0x2500, 0x257F) == "U+2500-U+257F");
    // Beyond the BMP, where a four-digit format would truncate.
    CHECK(formatCodePointRange(0x1D400, 0x1D7FF) == "U+1D400-U+1D7FF");

    // Each spelling round-trips through the reader to the same numbers.
    const Values single = parsed(renderRangeMapBlock(0x2318, 0x2318, "Apple Symbols"));
    CHECK(single.rangeMap.size() == 1);
    if (single.rangeMap.size() == 1) {
        CHECK(single.rangeMap[0].firstCodePoint == 0x2318);
        CHECK(single.rangeMap[0].lastCodePoint == 0x2318);
    }
    const Values wide = parsed(renderRangeMapBlock(0x1D400, 0x1D7FF, "STIX Two Math"));
    CHECK(wide.rangeMap.size() == 1);
    if (wide.rangeMap.size() == 1) {
        CHECK(wide.rangeMap[0].firstCodePoint == 0x1D400);
        CHECK(wide.rangeMap[0].lastCodePoint == 0x1D7FF);
    }
}

TEST(testAWrittenOverrideParsesBackAsRouting)
{
    const std::string body = spliceRangeMap("[font]\nfamily = \"JetBrains Mono\"\n", 0x2500, 0x257F, "Apple Symbols");
    const Values values = parsed(body);

    // The unrelated key survives, and the override routes.
    CHECK(values.fontFamily.value_or("") == "JetBrains Mono");
    CHECK(values.rangeMap.size() == 1);
    CHECK(familyFor(values, 0x2502) == "Apple Symbols");
    // Outside the range, nothing claims it.
    CHECK(familyFor(values, 0x2318).empty());
}

TEST(testAFamilyNeedingEscapesSurvivesTheRoundTrip)
{
    // The family is not the writer's: it comes from a font catalogue, and a
    // quote in it would close the string and make the file mean something else.
    const std::string hostile = "Ev\"il\nFont";
    const Values values = parsed(spliceRangeMap("", 0x2318, 0x2318, hostile));
    CHECK(values.rangeMap.size() == 1);
    if (values.rangeMap.size() == 1)
        CHECK(values.rangeMap[0].family == hostile);
    // And the file still parses to exactly one entry — nothing was injected.
    CHECK(familyFor(values, 0x2318) == hostile);
}

TEST(testTheLastWriteWinsForTheSameRange)
{
    std::string body = spliceRangeMap("", 0x2500, 0x257F, "Apple Symbols");
    body = spliceRangeMap(std::move(body), 0x2500, 0x257F, "STIX Two Math");
    const Values values = parsed(body);
    CHECK(values.rangeMap.size() == 1);
    CHECK(familyFor(values, 0x2502) == "STIX Two Math");
}

TEST(testAPartialOverlapSplitsRatherThanDiscards)
{
    // A block-wide override, then one character inside it sent elsewhere.
    std::string body = spliceRangeMap("", 0x2500, 0x257F, "Apple Symbols");
    body = spliceRangeMap(std::move(body), 0x2502, 0x2502, "Menlo");
    const Values values = parsed(body);

    // The one character moved...
    CHECK(familyFor(values, 0x2502) == "Menlo");
    // ...and everything either side of it still routes where it did. Replacing
    // the overlapping entry instead of splitting it drops routing across
    // 0x2500-0x257F that the override never named.
    CHECK(familyFor(values, 0x2500) == "Apple Symbols");
    CHECK(familyFor(values, 0x2501) == "Apple Symbols");
    CHECK(familyFor(values, 0x2503) == "Apple Symbols");
    CHECK(familyFor(values, 0x257F) == "Apple Symbols");
    // Nothing leaked outside the original block.
    CHECK(familyFor(values, 0x24FF).empty());
    CHECK(familyFor(values, 0x2580).empty());
}

TEST(testAnOverlapAtAnEdgeLeavesOnlyOnePiece)
{
    std::string body = spliceRangeMap("", 0x2500, 0x257F, "Apple Symbols");
    body = spliceRangeMap(std::move(body), 0x2500, 0x2510, "Menlo");
    const Values values = parsed(body);
    CHECK(familyFor(values, 0x2500) == "Menlo");
    CHECK(familyFor(values, 0x2510) == "Menlo");
    CHECK(familyFor(values, 0x2511) == "Apple Symbols");
    CHECK(familyFor(values, 0x257F) == "Apple Symbols");
}

TEST(testAnOverrideCoveringAnEntryWhollyReplacesIt)
{
    std::string body = spliceRangeMap("", 0x2502, 0x2502, "Menlo");
    body = spliceRangeMap(std::move(body), 0x2500, 0x257F, "Apple Symbols");
    const Values values = parsed(body);
    CHECK(values.rangeMap.size() == 1);
    CHECK(familyFor(values, 0x2502) == "Apple Symbols");
}

TEST(testRemovalDeletesCoverageAndSplitsWhatExtendsBeyond)
{
    std::string body = spliceRangeMap("", 0x2500, 0x257F, "Apple Symbols");
    body = removeRangeMap(std::move(body), 0x2502, 0x2502);
    const Values values = parsed(body);

    CHECK(familyFor(values, 0x2502).empty()); // the override is gone
    CHECK(familyFor(values, 0x2501) == "Apple Symbols"); // its neighbours are not
    CHECK(familyFor(values, 0x2503) == "Apple Symbols");

    // Removing the whole thing leaves no entry at all rather than an empty one.
    const Values emptied = parsed(removeRangeMap(body, 0x2500, 0x257F));
    CHECK(emptied.rangeMap.empty());
}

TEST(testBlocksThatDoNotOverlapAreByteIdentical)
{
    // A hand-authored block with a comment and spelling the writer would not
    // produce: it must come back exactly as written.
    const std::string handAuthored = "\n[[font.range_map]]\n"
                                     "# powerline, kept by hand\n"
                                     "range = \"e0a0-e0d4\"\n"
                                     "mode = \"custom\"\n"
                                     "family = \"MesloLGS Nerd Font\"\n";
    const std::string body = "[font]\nfamily = \"JetBrains Mono\"\n" + handAuthored;
    const std::string after = spliceRangeMap(body, 0x2500, 0x257F, "Apple Symbols");

    CHECK(contains(after, handAuthored));
    CHECK(contains(after, "# powerline, kept by hand"));
    CHECK(contains(after, "range = \"e0a0-e0d4\"")); // lowercase, no U+, unpadded

    // And both routings are live.
    const Values values = parsed(after);
    CHECK(familyFor(values, 0xE0A0) == "MesloLGS Nerd Font");
    CHECK(familyFor(values, 0x2502) == "Apple Symbols");
}

TEST(testABlockTheWriterCannotReadIsLeftAlone)
{
    // An unparseable range is not this writer's to rewrite: it cannot know what
    // the entry claims, so it must not decide the entry does not overlap and
    // quietly cut it. The reader drops it; the file keeps it.
    const std::string opaque = "\n[[font.range_map]]\nrange = \"not-a-range\"\nmode = \"custom\"\nfamily = \"X\"\n";
    const std::string after = spliceRangeMap(opaque, 0x2500, 0x257F, "Apple Symbols");
    CHECK(contains(after, "range = \"not-a-range\""));
    CHECK(contains(after, "U+2500-U+257F"));
}

TEST(testAnEmptyFamilyOrInvertedRangeIsRefused)
{
    // Both would produce an entry the reader drops, so writing one would be a
    // silent no-op in the file rather than a refusal at the call.
    const std::string body = "[font]\nfamily = \"JetBrains Mono\"\n";
    CHECK(spliceRangeMap(body, 0x2500, 0x257F, "") == body);
    CHECK(spliceRangeMap(body, 0x257F, 0x2500, "Apple Symbols") == body);
    CHECK(removeRangeMap(body, 0x257F, 0x2500) == body);
}

TEST(rangeSizingSurvivesSplitsAndAnOverrideCanReturnToDefault)
{
    std::string body = spliceRangeMap("", 0x2400, 0x243F, "Apple Symbols", false);
    body = spliceRangeMap(std::move(body), 0x2426, 0x2426, "Apple Symbols", true);
    const Values values = parsed(body);
    REQUIRE(values.rangeMap.size() == 3);
    CHECK(values.rangeMap[0].shouldMatchSize == false);
    CHECK(values.rangeMap[1].shouldMatchSize == false);
    CHECK(values.rangeMap[2].shouldMatchSize == true);
    const Values inherited = parsed(spliceRangeMap(body, 0x2426, 0x2426, "Apple Symbols"));
    REQUIRE(inherited.rangeMap.size() == 3);
    CHECK(!inherited.rangeMap[2].shouldMatchSize.has_value());
    const Values removed = parsed(removeRangeMap(body, 0x2410, 0x2410));
    REQUIRE(removed.rangeMap.size() == 4);
    CHECK(removed.rangeMap[0].shouldMatchSize == false);
    CHECK(removed.rangeMap[1].shouldMatchSize == false);
}

UBIQUITTY_TEST_MAIN()
