// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/OpenTypeShapingFilter.h"

#include <array>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

using Bytes = std::vector<std::uint8_t>;

void appendWord(Bytes& bytes, unsigned value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void setWord(Bytes& bytes, std::size_t offset, unsigned value)
{
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

std::array<std::uint16_t, 128> nominalGlyphs()
{
    std::array<std::uint16_t, 128> result;
    for (unsigned index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint16_t>(index + 500);
    }
    return result;
}

Bytes coverage(unsigned character)
{
    Bytes result;
    for (auto value : { 1u, 1u, character + 500 }) {
        appendWord(result, value);
    }
    return result;
}

Bytes single(unsigned character)
{
    Bytes result;
    for (auto value : { 1u, 6u, 1u }) {
        appendWord(result, value);
    }
    auto covered = coverage(character);
    result.insert(result.end(), covered.begin(), covered.end());
    return result;
}

Bytes chained(unsigned before, unsigned character, unsigned after)
{
    Bytes result;
    for (auto value : { 3u, 1u, 16u, 1u, 22u, 1u, 28u, 0u }) {
        appendWord(result, value);
    }
    for (auto value : { before, character, after }) {
        auto covered = coverage(value);
        result.insert(result.end(), covered.begin(), covered.end());
    }
    return result;
}

Bytes table(unsigned type, const Bytes& subtable, unsigned flags = 0, bool isRequired = false) // bool-ok: encodes the required-feature field in synthetic font bytes.
{
    Bytes result;
    for (auto value : { 1u, 0u, 10u, isRequired ? 32u : 12u, isRequired ? 46u : 26u }) {
        appendWord(result, value);
    }
    if (isRequired) {
        for (auto value : { 1u, 0x6c61u, 0x746eu, 8u, 4u, 0u, 0u, 0u, 1u, 0u, 0u }) {
            appendWord(result, value);
        }
    } else {
        appendWord(result, 0);
    }
    for (auto value : { 1u, 0x6361u, 0x6c74u, 8u, 0u, 1u, 0u }) {
        appendWord(result, value);
    }
    for (auto value : { 1u, 4u, type, flags, 1u, 8u }) {
        appendWord(result, value);
    }
    result.insert(result.end(), subtable.begin(), subtable.end());
    return result;
}

bool candidate(const OpenTypeShapingFilter& filter, std::string_view text)
{
    std::vector<std::uint32_t> characters(text.begin(), text.end());
    return filter.mayChange(characters);
}

}

TEST(singleSubstitutionUsesResolvedNominalGlyphs)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(1, single('m')), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "tmux"));
    CHECK(!candidate(filter, "12345"));
    CHECK(!candidate(filter, "ttt"));
}

TEST(disabledFeaturesStayInactiveExceptWhenRequired)
{
    std::array<std::uint32_t, 1> disabled { 0x63616c74 };
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(1, single('m')), disabled, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(!candidate(filter, "tmux"));
    OpenTypeShapingFilter required;
    CHECK(required.addTable(table(1, single('m'), 0, true), disabled, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(required, "tmux"));
}

TEST(contextNeedsBothNeighborsAndRespectsEdges)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(6, chained('m', 'i', 'm')), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "mim"));
    CHECK(!candidate(filter, "mi"));
    CHECK(!candidate(filter, "im"));
    CHECK(!candidate(filter, "mii"));
    CHECK(!candidate(filter, "mmm"));
}

TEST(ignoreFlagsBroadenContextInsteadOfMissingSkippedGlyphs)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(6, chained('m', 'i', 'm'), 8), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "m i m"));
    CHECK(candidate(filter, "i"));
    CHECK(!candidate(filter, "mmm"));
}

TEST(firstSubstitutionPreventsBypassOfLaterContext)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(6, chained('m', 'i', 'm')), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(!candidate(filter, "aia"));
    CHECK(filter.addTable(table(1, single('a')), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "aia"));
}

TEST(glyphDefinitionClassesProveWhenIgnoreFlagsCannotSkipASCII)
{
    Bytes definition;
    for (auto value : { 1u, 0u, 12u, 0u, 0u, 0u, 2u, 1u, 532u, 626u, 1u }) {
        appendWord(definition, value);
    }
    OpenTypeShapingFilter filter;
    CHECK(filter.setGlyphClasses(definition, nominalGlyphs()));
    CHECK(filter.addTable(table(6, chained('m', 'i', 'm'), 8), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "mim"));
    CHECK(!candidate(filter, "m i m"));
    OpenTypeShapingFilter skipping;
    setWord(definition, definition.size() - 2, 3);
    CHECK(skipping.setGlyphClasses(definition, nominalGlyphs()));
    CHECK(skipping.addTable(table(6, chained('m', 'i', 'm'), 8), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(skipping, "i"));
    OpenTypeShapingFilter invalid;
    definition.pop_back();
    CHECK(!invalid.setGlyphClasses(definition, nominalGlyphs()));
    CHECK(candidate(invalid, "abc"));
}

TEST(explicitGlyphContextAndLigatureRulesNeedTheirComponents)
{
    for (unsigned type : { 4u, 5u, 6u }) {
        Bytes bytes;
        // Coverage follows the fixed header, then a single rule set.
        for (auto value : { 1u, 8u, 1u, 14u }) {
            appendWord(bytes, value);
        }
        auto covered = coverage('f');
        bytes.insert(bytes.end(), covered.begin(), covered.end());
        appendWord(bytes, 1);
        appendWord(bytes, 4);
        if (type == 6) {
            appendWord(bytes, 1);
            appendWord(bytes, 500 + 'a');
        } else if (type == 4) {
            appendWord(bytes, 900);
        }
        appendWord(bytes, 2);
        if (type == 5) {
            appendWord(bytes, 0);
        }
        appendWord(bytes, 500 + 'i');
        if (type == 6) {
            appendWord(bytes, 1);
            appendWord(bytes, 500 + 'z');
            appendWord(bytes, 0);
        }
        OpenTypeShapingFilter filter;
        CHECK(filter.addTable(table(type, bytes), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
        CHECK(candidate(filter, "afiz"));
        CHECK(!candidate(filter, "afaz"));
        CHECK(!candidate(filter, "f"));
        CHECK(candidate(filter, "fi") == (type != 6));
        for (std::size_t end = 1; end < bytes.size(); ++end) {
            OpenTypeShapingFilter truncated;
            auto shortened = Bytes(bytes.begin(), bytes.begin() + end);
            CHECK(!truncated.addTable(table(type, shortened), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
            CHECK(candidate(truncated, "fi"));
        }
    }
}

TEST(positioningCoverageParticipates)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(1, single('A')), {}, nominalGlyphs(), FontLayoutTableKind::Positioning));
    CHECK(candidate(filter, "AV"));
    CHECK(!candidate(filter, "XX"));
    CHECK(filter.addTable(table(8, chained('m', 'i', 'm')), {}, nominalGlyphs(), FontLayoutTableKind::Positioning));
    CHECK(candidate(filter, "mim"));
}

TEST(classContextsIncludeDefaultClassZeroAndBothFlanks)
{
    for (unsigned type : { 5u, 6u }) {
        bool isChained = type == 6;
        Bytes bytes(isChained ? 16 : 12);
        setWord(bytes, 0, 2);
        setWord(bytes, 2, bytes.size());
        auto covered = coverage('f');
        bytes.insert(bytes.end(), covered.begin(), covered.end());
        auto classOffset = bytes.size();
        setWord(bytes, 4, classOffset);
        if (isChained) {
            setWord(bytes, 6, classOffset);
            setWord(bytes, 8, classOffset);
        }
        appendWord(bytes, 1);
        appendWord(bytes, 500 + 'a');
        appendWord(bytes, 26);
        for (unsigned character = 'a'; character <= 'z'; ++character) {
            appendWord(bytes, character == 'f' ? 1 : character == 'a' ? 2
                    : character == 'z'                                ? 3
                                                                      : 0);
        }
        setWord(bytes, isChained ? 10 : 6, 2);
        setWord(bytes, isChained ? 14 : 10, bytes.size());
        appendWord(bytes, 1);
        appendWord(bytes, 4);
        if (isChained) {
            appendWord(bytes, 1);
            appendWord(bytes, 2);
        }
        appendWord(bytes, 2);
        if (!isChained) {
            appendWord(bytes, 0);
        }
        appendWord(bytes, 0);
        if (isChained) {
            appendWord(bytes, 1);
            appendWord(bytes, 3);
            appendWord(bytes, 0);
        }
        OpenTypeShapingFilter filter;
        CHECK(filter.addTable(table(type, bytes), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
        CHECK(candidate(filter, "afiz"));
        CHECK(!candidate(filter, "affz"));
        CHECK(!candidate(filter, "f"));
        CHECK(candidate(filter, "fi") == !isChained);
        CHECK(candidate(filter, "ifii") == !isChained);
        for (std::size_t end = 1; end < bytes.size(); ++end) {
            OpenTypeShapingFilter truncated;
            CHECK(!truncated.addTable(table(type, Bytes(bytes.begin(), bytes.begin() + end)), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
        }
    }
}

TEST(extensionLookupUsesItsOwnOffsetBase)
{
    Bytes extension;
    for (auto value : { 1u, 6u, 0u, 8u }) {
        appendWord(extension, value);
    }
    auto contextual = chained('m', 'i', 'm');
    extension.insert(extension.end(), contextual.begin(), contextual.end());
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(7, extension), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "mim"));
    CHECK(!candidate(filter, "mii"));
}

TEST(coverageRangesProjectIntoCharacterMasks)
{
    Bytes ranges;
    for (auto value : { 1u, 6u, 1u, 2u, 1u, 500u + 'a', 500u + 'c', 0u }) {
        appendWord(ranges, value);
    }
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable(table(1, ranges), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(filter, "b"));
    CHECK(!candidate(filter, "d"));
}

TEST(truncatedOrUnsupportedTablesDecline)
{
    auto bytes = table(6, chained('m', 'i', 'm'));
    for (std::size_t size = 1; size < bytes.size(); ++size) {
        OpenTypeShapingFilter filter;
        CHECK(!filter.addTable(std::span(bytes).first(size), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
        CHECK(candidate(filter, "12345"));
    }
    OpenTypeShapingFilter unsupported;
    CHECK(!unsupported.addTable(table(99, single('m')), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(candidate(unsupported, "12345"));
}

TEST(variationsAndInvalidCoverageDecline)
{
    auto bytes = table(1, single('m'));
    setWord(bytes, 2, 1);
    setWord(bytes, 10, 1);
    OpenTypeShapingFilter variations;
    CHECK(!variations.addTable(bytes, {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    auto invalid = single('m');
    setWord(invalid, 2, 0xffff);
    OpenTypeShapingFilter coverageFilter;
    CHECK(!coverageFilter.addTable(table(1, invalid), {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
}

TEST(nonASCIIAndUnboundedRunsKeepTheShaper)
{
    OpenTypeShapingFilter filter;
    CHECK(filter.addTable({}, {}, nominalGlyphs(), FontLayoutTableKind::Substitution));
    CHECK(!candidate(filter, "ascii"));
    CHECK(candidate(filter, ""));
    CHECK(candidate(filter, std::string(129, 'a')));
    std::array<std::uint32_t, 1> devanagari { 0x915 };
    CHECK(filter.mayChange(devanagari));
}

TEST(featureTagsAreReadWithoutAssumingDefaults)
{
    auto tags = OpenTypeShapingFilter::featureTags(table(1, single('m')));
    CHECK(tags.size() == 1 && tags[0] == 0x63616c74);
    CHECK(OpenTypeShapingFilter::featureTags({}).empty());
}

UBIQUITTY_TEST_MAIN()
