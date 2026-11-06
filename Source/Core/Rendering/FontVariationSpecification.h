// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

/*
 * FontVariationSpecification.h — portable parser for the `font.variations`
 * axis grammar. Resolves a user specification string into an ordered list of
 * OpenType variation axis tags each paired with a design-space value.
 * Frontends map the result onto their local variable-font mechanism, writing
 * every axis in ONE variation dictionary: a second write is entitled to
 * restate the first axis's default, which is a measured failure mode.
 *
 * Grammar (space- and/or comma-separated tokens):
 *   tag=N    set the axis to N   e.g. slnt=-6, opsz=14.5, wght=450, GRAD=88
 * A tag must be exactly four ASCII graphic characters and keeps its case:
 * registered axes are lower-case by the OpenType specification and custom
 * axes upper-case, so folding case would alias a font's `GRAD` with a
 * nonexistent `grad`. Later tokens for the same tag win (override). Values
 * are locale-independent decimals with an optional sign and at most four
 * fractional digits of precision, which covers every published axis scale.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Rendering {

/// The OpenType `wght` axis tag as a big-endian four-character code.
inline constexpr std::uint32_t variationAxisTagWeight = 0x77676874;
/// The OpenType `wdth` axis tag as a big-endian four-character code.
inline constexpr std::uint32_t variationAxisTagWidth = 0x77647468;

/// One variation axis a specification sets.
struct FontVariationSetting {
    std::string tag; // 4-char OpenType axis tag, case preserved
    double value; // design-space value, clamped by the face at application

    bool operator==(const FontVariationSetting&) const = default;
};

// Resolved settings in first-appearance order.
//
// Named because a vector's element type is a template argument with no Swift
// spelling, so an unnamed one drops the declaration from the generated
// interface rather than failing to compile.
using FontVariationSettings = std::vector<FontVariationSetting>;

/// Parses a `font.variations` specification into resolved settings.
///
/// Invalid tokens are skipped rather than refused, and a duplicate tag
/// collapses to the last value seen for it: a reader typing a specification
/// by hand gets the axes they spelled correctly instead of none of them.
/// An empty or whitespace specification names no axis.
///
/// - Parameter specification: The specification, in the grammar above.
/// - Returns: The settings, in first-appearance order.
FontVariationSettings parseFontVariationSpecification(const std::string& specification);

/// Serialises resolved settings to the canonical form.
///
/// Space-separated `tag=N`, the value printed without a trailing fractional
/// zero. Round-trips through ``parseFontVariationSpecification``, which is
/// what lets a stored value be compared against a freshly composed one
/// without a second spelling of the grammar deciding they differ.
///
/// - Parameter settings: The settings to write.
/// - Returns: The canonical specification.
std::string serializeFontVariationSettings(const FontVariationSettings& settings);

/// Folds a user's axes over the ones a named setting derived.
///
/// `font.weight` and `font.width` derive `wght` and `wdth`; a variation
/// naming the same tag is the more specific instruction and wins, the same
/// precedence CSS gives `font-variation-settings` over its high-level
/// properties. Kept in Core so both shells fold identically — two folds are
/// two answers to which value an axis takes.
///
/// - Parameters:
///   - derived: The axes named settings derived, in their order.
///   - user: The axes the user's specification names.
/// - Returns: The merged settings; a tag in both takes the user's value.
[[nodiscard]] FontVariationSettings mergeFontVariationSettings(
    const FontVariationSettings& derived, const FontVariationSettings& user);

} // namespace Ubiquitty::Rendering
