// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OpenTypeShapingFilter.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace Ubiquitty::Rendering {

class OpenTypeShapingFilter::TableReader {
public:
    explicit TableReader(FontTableBytes bytes)
        : m_bytes(bytes)
    {
        if (bytes.size() > 16 * 1024 * 1024) {
            m_valid = false;
        }
    }

    bool valid() const { return m_valid; }
    void fail() { m_valid = false; }

    unsigned word(std::size_t offset)
    {
        if (!m_valid || !m_budget || offset > m_bytes.size() || m_bytes.size() - offset < 2) {
            m_valid = false;
            return 0;
        }
        --m_budget;
        return (static_cast<unsigned>(m_bytes[offset]) << 8) | m_bytes[offset + 1];
    }

    std::uint32_t doubleWord(std::size_t offset)
    {
        return (static_cast<std::uint32_t>(word(offset)) << 16) | word(offset + 2);
    }

    std::size_t relative(std::size_t base, std::uint32_t offset)
    {
        if (!offset || base > m_bytes.size() || offset > m_bytes.size() - base) {
            fail();
            return 0;
        }
        return base + offset;
    }

    std::size_t features()
    {
        if (word(0) != 1 || word(2) > 1) {
            fail();
        }
        if (word(2) == 1 && doubleWord(10)) {
            // Feature variation selection belongs to the shaper.
            fail();
        }
        return relative(0, word(6));
    }

    std::array<unsigned, 128> classes(std::size_t offset, NominalFontGlyphs nominal)
    {
        std::array<unsigned, 128> result {};
        if (!offset) {
            return result;
        }
        auto format = word(offset);
        if (format == 1) {
            auto start = word(offset + 2);
            auto count = word(offset + 4);
            if (start + count > 65536) {
                fail();
            }
            for (unsigned index = 0; valid() && index < count; ++index) {
                auto value = word(offset + 6 + 2 * index);
                for (unsigned character = 32; character < 127; ++character) {
                    if (nominal[character] == start + index) {
                        result[character] = value;
                    }
                }
            }
        } else if (format == 2) {
            auto count = word(offset + 2);
            unsigned previous = 0;
            for (unsigned index = 0; valid() && index < count; ++index) {
                auto record = offset + 4 + 6 * index;
                auto first = word(record);
                auto last = word(record + 2);
                auto value = word(record + 4);
                if (last < first || (index && first <= previous)) {
                    fail();
                }
                previous = last;
                for (unsigned character = 32; character < 127; ++character) {
                    if (nominal[character] >= first && nominal[character] <= last) {
                        result[character] = value;
                    }
                }
            }
        } else {
            fail();
        }
        return result;
    }

    CharacterSet coverage(std::size_t offset, NominalFontGlyphs nominal,
        std::array<unsigned, 128>* indices = nullptr)
    {
        CharacterSet result;
        auto format = word(offset);
        auto count = word(offset + 2);
        std::vector<std::pair<unsigned, unsigned>> ranges;
        std::vector<unsigned> starts;
        ranges.reserve(count);
        unsigned previous = 0;
        unsigned coverageIndex = 0;
        for (unsigned index = 0; valid() && index < count; ++index) {
            unsigned first;
            unsigned last;
            if (format == 1) {
                first = last = word(offset + 4 + 2 * index);
            } else if (format == 2) {
                auto record = offset + 4 + 6 * index;
                first = word(record);
                last = word(record + 2);
                if (word(record + 4) != coverageIndex) {
                    fail();
                }
            } else {
                fail();
                break;
            }
            if (last < first || (index && first <= previous)) {
                fail();
            }
            previous = last;
            starts.push_back(coverageIndex);
            coverageIndex += last - first + 1;
            ranges.emplace_back(first, last);
        }
        if (format != 1 && format != 2) {
            fail();
        }
        for (unsigned character = 32; valid() && character < 127; ++character) {
            unsigned glyph = nominal[character];
            auto found = std::lower_bound(ranges.begin(), ranges.end(), glyph,
                [](const auto& range, unsigned value) { return range.second < value; });
            if (glyph && found != ranges.end() && found->first <= glyph) {
                result.add(character);
                if (indices) {
                    (*indices)[character] = starts[found - ranges.begin()] + glyph - found->first;
                }
            }
        }
        return result;
    }

    void subtable(OpenTypeShapingFilter& filter, std::size_t offset, unsigned type,
        unsigned flags, NominalFontGlyphs nominal, bool isPositioning, unsigned depth = 0) // bool-ok: internal grammar flag derived from FontLayoutTableKind.
    {
        if (!valid()) {
            return;
        }
        if (type == (isPositioning ? 9u : 7u)) {
            if (depth || word(offset) != 1) {
                fail();
                return;
            }
            auto extensionType = word(offset + 2);
            auto extension = relative(offset, doubleWord(offset + 4));
            subtable(filter, extension, extensionType, flags, nominal, isPositioning, depth + 1);
            return;
        }
        if (!type || type > 8) {
            fail();
            return;
        }
        bool isContextual = type == (isPositioning ? 7u : 5u);
        bool isChained = type == (isPositioning ? 8u : 6u);
        unsigned format = word(offset);
        bool isLigature = !isPositioning && type == 4;
        if (((isContextual || isChained) && (format == 1 || format == 2)) || (isLigature && format == 1)) {
            std::array<unsigned, 128> indices {};
            auto covered = coverage(relative(offset, word(offset + 2)), nominal, &indices);
            std::array<unsigned, 128> backtrackClasses {};
            std::array<unsigned, 128> inputClasses {};
            std::array<unsigned, 128> lookaheadClasses {};
            std::size_t setHeader = offset + 4;
            if (format == 2) {
                if (isChained) {
                    backtrackClasses = classes(relative(offset, word(offset + 4)), nominal);
                    inputClasses = classes(relative(offset, word(offset + 6)), nominal);
                    lookaheadClasses = classes(relative(offset, word(offset + 8)), nominal);
                    setHeader = offset + 10;
                } else {
                    inputClasses = classes(relative(offset, word(offset + 4)), nominal);
                    setHeader = offset + 6;
                }
                indices = inputClasses;
            }
            auto setCount = word(setHeader);
            for (unsigned setIndex = 0; valid() && setIndex < setCount; ++setIndex) {
                CharacterSet first;
                for (unsigned character = 32; character < 127; ++character) {
                    if (covered.contains(character) && indices[character] == setIndex) {
                        first.add(character);
                    }
                }
                auto setOffset = word(setHeader + 2 + 2 * setIndex);
                if (!setOffset) {
                    continue;
                }
                auto set = relative(offset, setOffset);
                auto count = word(set);
                for (unsigned index = 0; valid() && index < count; ++index) {
                    auto rule = relative(set, word(set + 2 + 2 * index));
                    auto cursor = rule;
                    std::vector<Condition> conditions;
                    auto readGlyphs = [&](unsigned glyphCount, int start, int direction,
                                          const std::array<unsigned, 128>* glyphClasses) {
                        if (glyphCount > 32) {
                            fail();
                            return;
                        }
                        for (unsigned glyphIndex = 0; valid() && glyphIndex < glyphCount; ++glyphIndex) {
                            auto glyph = word(cursor);
                            cursor += 2;
                            CharacterSet characters;
                            for (unsigned character = 32; character < 127; ++character) {
                                if ((glyphClasses ? (*glyphClasses)[character] : nominal[character]) == glyph) {
                                    characters.add(character);
                                }
                            }
                            conditions.push_back({ start + direction * static_cast<int>(glyphIndex), characters });
                        }
                    };
                    if (isChained) {
                        auto backtrack = word(cursor);
                        cursor += 2;
                        readGlyphs(backtrack, -1, -1, format == 2 ? &backtrackClasses : nullptr);
                    }
                    if (isLigature) {
                        word(cursor); // The output glyph does not affect the match predicate.
                        cursor += 2;
                    }
                    auto inputs = word(cursor);
                    cursor += 2;
                    unsigned records = 0;
                    if (isContextual) {
                        records = word(cursor);
                        cursor += 2;
                    }
                    if (!inputs || inputs > 32) {
                        fail();
                    } else {
                        readGlyphs(inputs - 1, 1, 1, format == 2 ? &inputClasses : nullptr);
                    }
                    if (isChained) {
                        auto lookahead = word(cursor);
                        cursor += 2;
                        readGlyphs(lookahead, static_cast<int>(inputs), 1, format == 2 ? &lookaheadClasses : nullptr);
                        records = word(cursor);
                        cursor += 2;
                    }
                    for (unsigned record = 0; valid() && record < records; ++record) {
                        if (word(cursor + 4 * record) >= inputs || word(cursor + 4 * record + 2) >= m_lookupCount) {
                            fail();
                        }
                    }
                    if (filter.flagsMaySkip(flags)) {
                        conditions.clear();
                    }
                    if (valid()) {
                        filter.addRule(first, std::move(conditions));
                    }
                }
            }
            // Missing rule sets cannot establish the absence of a lookup.
            for (unsigned character = 32; character < 127; ++character) {
                if (covered.contains(character) && indices[character] >= setCount) {
                    fail();
                }
            }
            return;
        }
        if ((isContextual || isChained) && format == 3) {
            std::vector<Condition> conditions;
            CharacterSet first;
            std::size_t cursor = offset + 2;
            auto readConditions = [&](unsigned count, int start, int direction) {
                if (count > 32) {
                    fail();
                    return;
                }
                for (unsigned index = 0; valid() && index < count; ++index) {
                    auto covered = coverage(relative(offset, word(cursor)), nominal);
                    cursor += 2;
                    int relativeOffset = start + direction * static_cast<int>(index);
                    if (!relativeOffset) {
                        first = covered;
                    } else {
                        conditions.push_back({ relativeOffset, covered });
                    }
                }
            };
            unsigned records = 0;
            if (isChained) {
                auto backtrack = word(cursor);
                cursor += 2;
                readConditions(backtrack, -1, -1);
            }
            auto inputs = word(cursor);
            cursor += 2;
            if (!inputs) {
                fail();
            }
            if (isContextual) {
                records = word(cursor);
                cursor += 2;
            }
            readConditions(inputs, 0, 1);
            if (isChained) {
                auto lookahead = word(cursor);
                cursor += 2;
                readConditions(lookahead, static_cast<int>(inputs), 1);
                records = word(cursor);
                cursor += 2;
            }
            for (unsigned index = 0; valid() && index < records; ++index) {
                if (word(cursor + 4 * index) >= inputs || word(cursor + 4 * index + 2) >= m_lookupCount) {
                    fail();
                }
            }
            if (filter.flagsMaySkip(flags)) {
                // Ignored glyph classes can make non-adjacent glyphs participate.
                conditions.clear();
            }
            if (valid()) {
                filter.addRule(first, std::move(conditions));
            }
            return;
        }
        if ((isContextual || isChained) && format != 1 && format != 2) {
            fail();
            return;
        }
        if (!isContextual && !isChained && format != 1
            && !((type == 1 || (isPositioning && type == 2)) && format == 2)) {
            fail();
            return;
        }
        // First-glyph coverage is a necessary condition for the remaining
        // lookup formats. Their full action need not be interpreted to reject
        // a run with no covered glyphs.
        auto first = coverage(relative(offset, word(offset + 2)), nominal);
        if (valid()) {
            filter.addRule(first, {});
        }
    }

    void compile(OpenTypeShapingFilter& filter, FontFeatureTags disabled, NominalFontGlyphs nominal, bool isPositioning) // bool-ok: internal grammar flag derived from FontLayoutTableKind.
    {
        auto featureList = features();
        auto lookupList = relative(0, word(8));
        m_lookupCount = word(lookupList);
        auto featureCount = word(featureList);
        if (featureCount > 1024 || m_lookupCount > 4096) {
            fail();
            return;
        }
        std::vector<bool> selected(m_lookupCount);
        std::vector<bool> required(featureCount);
        auto scriptList = relative(0, word(4));
        auto scriptCount = word(scriptList);
        for (unsigned index = 0; valid() && index < scriptCount; ++index) {
            auto script = relative(scriptList, word(scriptList + 2 + 6 * index + 4));
            auto languageCount = word(script + 2);
            auto inspectLanguage = [&](unsigned languageOffset) {
                if (!languageOffset) {
                    return;
                }
                auto language = relative(script, languageOffset);
                auto requiredIndex = word(language + 2);
                if (requiredIndex != 0xffff) {
                    if (requiredIndex >= required.size()) {
                        fail();
                    } else {
                        required[requiredIndex] = true;
                    }
                }
            };
            inspectLanguage(word(script));
            for (unsigned language = 0; valid() && language < languageCount; ++language) {
                inspectLanguage(word(script + 4 + 6 * language + 4));
            }
        }
        for (unsigned index = 0; valid() && index < featureCount; ++index) {
            auto record = featureList + 2 + 6 * index;
            auto tag = doubleWord(record);
            auto feature = relative(featureList, word(record + 4));
            auto count = word(feature + 2);
            bool enabled = required[index] || std::find(disabled.begin(), disabled.end(), tag) == disabled.end();
            for (unsigned item = 0; valid() && item < count; ++item) {
                auto lookup = word(feature + 4 + 2 * item);
                if (lookup >= selected.size()) {
                    fail();
                    break;
                }
                selected[lookup] = selected[lookup] || enabled;
            }
        }
        for (unsigned index = 0; valid() && index < m_lookupCount; ++index) {
            if (!selected[index]) {
                continue;
            }
            auto lookup = relative(lookupList, word(lookupList + 2 + 2 * index));
            auto type = word(lookup);
            auto flags = word(lookup + 2);
            auto count = word(lookup + 4);
            for (unsigned item = 0; valid() && item < count; ++item) {
                subtable(filter, relative(lookup, word(lookup + 6 + 2 * item)), type, flags, nominal, isPositioning);
            }
        }
    }

private:
    FontTableBytes m_bytes;
    unsigned m_lookupCount { 0 };
    unsigned m_budget { 1024 * 1024 };
    bool m_valid { true };
};

FontFeatureTagList OpenTypeShapingFilter::featureTags(FontTableBytes bytes)
{
    if (bytes.empty()) {
        return {};
    }
    TableReader reader(bytes);
    auto list = reader.features();
    auto count = reader.word(list);
    if (count > 1024) {
        return {};
    }
    FontFeatureTagList tags;
    tags.reserve(count);
    for (unsigned index = 0; reader.valid() && index < count; ++index) {
        tags.push_back(reader.doubleWord(list + 2 + 6 * index));
    }
    return reader.valid() ? tags : FontFeatureTagList {};
}

void OpenTypeShapingFilter::addRule(CharacterSet first, std::vector<Condition>&& conditions)
{
    if (first.empty() || std::any_of(conditions.begin(), conditions.end(), [](const auto& condition) { return condition.characters.empty(); })) {
        return;
    }
    if (conditions.empty() || m_rules.size() >= maxRules) {
        for (unsigned word = 0; word < 2; ++word) {
            m_unconditional.words[word] |= first.words[word];
        }
        return;
    }
    auto index = m_rules.size();
    for (unsigned character = 32; character < 127; ++character) {
        if (first.contains(character)) {
            m_candidates[character][index / 64] |= std::uint64_t { 1 } << (index % 64);
        }
    }
    m_rules.push_back({ std::move(conditions) });
}

bool OpenTypeShapingFilter::setGlyphClasses(FontTableBytes bytes, NominalFontGlyphs nominal)
{
    if (nominal.size() != 128 || !m_rules.empty() || !m_unconditional.empty()) {
        m_valid = false;
        return false;
    }
    if (!bytes.empty()) {
        TableReader reader(bytes);
        if (reader.word(0) != 1 || reader.word(2) > 3) {
            reader.fail();
        }
        auto offset = reader.word(4);
        m_glyphClasses = reader.classes(offset ? reader.relative(0, offset) : 0, nominal);
        m_valid = m_valid && reader.valid()
            && std::all_of(m_glyphClasses.begin(), m_glyphClasses.end(), [](unsigned value) { return value <= 4; });
    }
    m_hasGlyphClasses = m_valid;
    return m_valid;
}

bool OpenTypeShapingFilter::flagsMaySkip(unsigned flags) const
{
    if (!flags) {
        return false;
    }
    if (!m_hasGlyphClasses || (flags & 0xe0)) {
        return true;
    }
    for (auto glyphClass : m_glyphClasses) {
        if ((glyphClass == 1 && (flags & 2)) || (glyphClass == 2 && (flags & 4))
            || (glyphClass == 3 && (flags & 0xff18))) {
            return true;
        }
    }
    return false;
}

bool OpenTypeShapingFilter::addTable(FontTableBytes bytes, FontFeatureTags disabled,
    NominalFontGlyphs nominal, FontLayoutTableKind tableKind)
{
    if (nominal.size() != 128) {
        m_valid = false;
    }
    if (!m_valid || bytes.empty()) {
        return m_valid;
    }
    TableReader reader(bytes);
    reader.compile(*this, disabled, nominal, tableKind == FontLayoutTableKind::Positioning);
    m_valid = reader.valid();
    return m_valid;
}

bool OpenTypeShapingFilter::mayChange(ShapingCodePoints text) const
{
    if (!m_valid || text.empty() || text.size() > 128) {
        return true;
    }
    for (auto character : text) {
        if (character < 32 || character >= 127 || m_unconditional.contains(character)) {
            return true;
        }
    }
    for (std::size_t position = 0; position < text.size(); ++position) {
        auto masks = m_candidates[text[position]];
        for (unsigned word = 0; word < masks.size(); ++word) {
            while (masks[word]) {
                auto bit = std::countr_zero(masks[word]);
                masks[word] &= masks[word] - 1;
                bool matches = true;
                for (const auto& condition : m_rules[word * 64 + bit].conditions) {
                    auto neighbor = static_cast<int>(position) + condition.offset;
                    if (neighbor < 0 || static_cast<std::size_t>(neighbor) >= text.size()
                        || !condition.characters.contains(text[neighbor])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return true;
                }
            }
        }
    }
    return false;
}

}
