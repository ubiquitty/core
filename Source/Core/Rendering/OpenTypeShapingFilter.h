// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

using FontTableBytes = std::span<const std::uint8_t>;
using FontFeatureTags = std::span<const std::uint32_t>;
using NominalFontGlyphs = std::span<const std::uint16_t>;
using ShapingCodePoints = std::span<const std::uint32_t>;
using FontFeatureTagList = std::vector<std::uint32_t>;

/// Identifies the layout table’s lookup grammar.
enum class FontLayoutTableKind {
    Substitution,
    Positioning,
};

/// Rejects ASCII runs with no possible first OpenType lookup application.
///
/// The caller supplies the resolved font's nominal ASCII glyphs and only
/// features the platform has established are disabled. Every other feature
/// participates. A malformed or unsupported table makes every run a candidate.
class UBIQUITTY_NONCOPYABLE OpenTypeShapingFilter {
public:
    OpenTypeShapingFilter() = default;
    OpenTypeShapingFilter(OpenTypeShapingFilter&&) = default;
    OpenTypeShapingFilter& operator=(OpenTypeShapingFilter&&) = default;
    OpenTypeShapingFilter(const OpenTypeShapingFilter&) = delete;
    OpenTypeShapingFilter& operator=(const OpenTypeShapingFilter&) = delete;

    /// Reads feature tags from a GSUB or GPOS table.
    ///
    /// - Parameter table: The complete layout-table bytes.
    /// - Returns: The declared tags, or an empty list if the directory is invalid.
    static FontFeatureTagList featureTags(FontTableBytes);
    /// Loads GDEF classes for the nominal ASCII glyphs.
    ///
    /// - Parameters:
    ///   - table: The complete GDEF table, or an empty span if absent.
    ///   - nominal: The resolved font’s 128 nominal ASCII glyph IDs.
    /// - Returns: Whether glyph-class interpretation remains supported.
    bool setGlyphClasses(FontTableBytes, NominalFontGlyphs);
    /// Adds conservative candidates from one layout table.
    ///
    /// - Parameters:
    ///   - table: The complete layout-table bytes.
    ///   - disabledFeatures: Tags the platform has verified are disabled.
    ///   - nominal: The resolved font’s 128 nominal ASCII glyph IDs.
    ///   - tableKind: Whether to interpret GSUB or GPOS lookup types.
    /// - Returns: Whether the accumulated analysis remains supported.
    bool addTable(FontTableBytes, FontFeatureTags disabledFeatures, NominalFontGlyphs, FontLayoutTableKind tableKind);
    /// Tests whether a lookup could affect the complete ASCII shaping context.
    ///
    /// - Parameter text: The context, including shaping flanks.
    /// - Returns: True when shaping is required, including unsupported input.
    bool mayChange(ShapingCodePoints) const;
    /// Reports whether all supplied tables support conservative analysis.
    ///
    /// - Returns: Whether direct mapping can be considered.
    bool valid() const { return m_valid; }

private:
    static constexpr unsigned maxRules = 512;
    /// Stores membership for the 128 nominal ASCII characters.
    struct CharacterSet {
        std::array<std::uint64_t, 2> words {};
        /// Includes an ASCII character in the set.
        ///
        /// - Parameter character: An ASCII value below 128.
        void add(unsigned character) { words[character / 64] |= std::uint64_t { 1 } << (character % 64); }
        /// Tests membership of an ASCII character.
        ///
        /// - Parameter character: An ASCII value below 128.
        /// - Returns: Whether the set includes the character.
        bool contains(unsigned character) const { return (words[character / 64] >> (character % 64)) & 1; }
        /// Reports whether the set contains no characters.
        ///
        /// - Returns: Whether both membership words are zero.
        bool empty() const { return !(words[0] | words[1]); }
    };
    /// Constrains a character at an offset from a candidate start.
    struct Condition {
        int offset;
        CharacterSet characters;
    };
    /// Owns the contextual conditions for one possible lookup application.
    struct Rule {
        std::vector<Condition> conditions;
    };
    class TableReader;
    /// Indexes a contextual candidate or broadens it when storage is exhausted.
    ///
    /// - Parameters:
    ///   - first: Characters that can start the candidate.
    ///   - conditions: Remaining contextual requirements, transferred to the filter.
    void addRule(CharacterSet first, std::vector<Condition>&&);
    /// Determines whether lookup flags can skip a nominal glyph.
    ///
    /// - Parameter flags: The OpenType lookup flags.
    /// - Returns: Whether skipping is possible or class information is insufficient.
    bool flagsMaySkip(unsigned) const;

    CharacterSet m_unconditional;
    std::array<std::array<std::uint64_t, maxRules / 64>, 128> m_candidates {};
    std::array<unsigned, 128> m_glyphClasses {};
    std::vector<Rule> m_rules;
    bool m_valid { true };
    bool m_hasGlyphClasses { false };
};

}
