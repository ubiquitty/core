// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

/*
 * FontFeatureSpecification.h — portable parser for the `font.features` allow/block
 * grammar. Resolves a user specification string into an ordered list of OpenType
 * feature tags each paired with an integer value (0 = off, 1 = on, N = an
 * explicit alternate/parametric value). Frontends map the result onto their
 * local shaper feature mechanism.
 *
 * Grammar (space- and/or comma-separated tokens, case-insensitive):
 *   tag      enable  (value 1)   e.g. liga, calt, dlig, ss01, zero
 *   +tag     enable  (value 1)   explicit-enable form
 *   -tag     disable (value 0)   block the feature
 *   tag=N    enable with the explicit value N (alternate/parametric)
 * A tag must be exactly four ASCII graphic characters; the `ssN` shorthand is
 * zero-padded to `ssNN`. Later tokens for the same tag win (override). A plain
 * comma list of bare tags ("zero,ss01") therefore resolves to all-on, matching
 * the bare enable-only form.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Rendering {

struct FontFeatureSetting {
    std::string tag; // canonical 4-char lower-case OpenType tag
    int value; // 0 = off, 1 = on, N = explicit value

    bool operator==(const FontFeatureSetting&) const = default;
};

// Resolved settings in first-appearance order.
//
// Named because a vector's element type is a template argument with no Swift
// spelling, so an unnamed one drops the declaration from the generated
// interface rather than failing to compile.
using FontFeatureSettings = std::vector<FontFeatureSetting>;

/// Parses a `font.features` specification into resolved settings.
///
/// Invalid tokens are skipped rather than refused, and a duplicate tag
/// collapses to the last value seen for it: a reader typing a specification by
/// hand gets the features they spelled correctly instead of none of them.
/// An empty or whitespace specification names no feature.
///
/// - Parameter specification: The specification, in the grammar above.
/// - Returns: The settings, in first-appearance order.
FontFeatureSettings parseFontFeatureSpecification(const std::string& specification);

/// Whether a face's own ligatures may apply.
///
/// A named argument rather than a bare bool: a call site reads
/// `Ligatures::Disabled` where it would otherwise read `false` and leave the
/// reader to work out what is false about it.
enum class Ligatures : std::uint8_t {
    Enabled,
    Disabled,
};

/// The tags a ligature suppression must name, in the order it names them.
///
/// Three rather than one, because a face may carry its programming ligatures
/// under any of them and turning off only `liga` leaves the rest drawing —
/// measured against JetBrains Mono, `=>` lives in `calt` alone, so a
/// `liga`-only suppression looks like a fix and changes nothing.
///
/// `dlig` is absent on purpose. A suppression has to name the features a shaper
/// applies by DEFAULT, and discretionary ligatures are off until asked for, so
/// naming it would add a line to every resolved list to no effect. That is why
/// this is not ``ligatureCandidateFeatureTags``, which is the opposite
/// question — what a classification should TRY, including features nothing
/// enables on its own.
///
/// - Returns: The tags, `liga`, `clig`, `calt`.
[[nodiscard]] std::vector<std::string> ligatureSuppressionFeatureTags();

/// Resolves a ligature policy and a `font.features` specification into one list.
///
/// The composition rule every frontend needs and none should own: suppression
/// first, the reader's specification after, so a later entry wins and a feature
/// named explicitly beats the coarse toggle. Reversing that order makes
/// `font.features` unreliable in a way nothing reports.
///
/// Shared because the rule is portable and only the OUTPUT is not: a frontend
/// maps this to `hb_feature_t`, to a CSS `font-feature-settings` descriptor, or
/// to whatever its shaper takes. Two frontends composing the list themselves
/// agree until one of them is edited.
///
/// - Parameters:
///   - ligatures: Whether the face's own ligatures may apply.
///   - specification: The `font.features` specification, in the grammar
///     ``parseFontFeatureSpecification`` reads.
/// - Returns: The settings to request, in the order to request them. Empty when
///   ligatures are enabled and the specification names nothing — which is not
///   "no features" but "whatever the shaper does by default".
[[nodiscard]] FontFeatureSettings resolveFontFeatures(Ligatures ligatures, const std::string& specification);

/// Serialises resolved settings to the canonical form.
///
/// Space-separated, lower-cased, a disabled tag prefixed `-`, an explicit
/// value above one rendered `tag=N`. Round-trips through
/// ``parseFontFeatureSpecification``, which is what lets a stored value be
/// compared against a freshly composed one without a second spelling of the
/// grammar deciding they differ.
///
/// - Parameter settings: The settings to write.
/// - Returns: The canonical specification.
std::string serializeFontFeatureSettings(const FontFeatureSettings& settings);

// Convenience: parse `specification` then serialize it back to canonical form.
std::string canonicalFontFeatureSpecification(const std::string& specification);

// The probes a ligature classification renders under each candidate feature.
// A feature qualifies when it changes the operator probe's glyphs but leaves
// the alphanumeric probe untouched — ligatures turn on, letter alternates stay
// off. The classification itself is each frontend's (it needs the platform
// shaper), but two frontends spelling the probes separately drift them apart
// and classify one font two ways.
inline constexpr const char* ligatureAlphanumericProbe = "Hello there 0123456789 the quick brown fox jumps lazily";
// Broad operator probe so per-language stylistic sets each get exercised.
inline constexpr const char* ligatureOperatorProbe = "-> => <- <-> != !== >= <= == === <==> |> <| <|> >>= =<< "
                                                     "++ -- ** // /* */ :: := =~ << >> <<< >>> && || ?? ?. ?: "
                                                     "<> <~ ~> |- -| .. ... www 0x ## #! /\\ \\/ |||";

/// The features a ligature classification tries.
///
/// The ligature triple, then the twenty stylistic sets that bury per-language
/// ligatures behind letter alternates.
///
/// - Returns: The candidate tags, in trial order.
std::vector<std::string> ligatureCandidateFeatureTags();

// The healing probe: plain letters and digits a texture-healing font swaps
// for contextual alternates — adjacent wides (m, w), adjacent narrows (i, l),
// and digit pairs, the populations Monaspace-class faces adjust. No operator
// anywhere: an operator would qualify the probe under the gated walk, and the
// question is about text that walk would never shape — exactly the text whose
// rendering must not depend on an invisible neighbour.
//
// Shaped under the STRIP font — default features included, since a
// default-on calt is how healing arrives without any classified feature —
// and compared against the same characters mapped one at a time, which is
// what the per-cell mono path draws. The two disagreeing on any glyph is the
// finding; deciding what follows from it is decideLigatureParticipation's,
// so both arms reach one verdict from one criterion.
inline constexpr const char* ligatureHealingProbe = "mmm www nnn iii lll 111 000 illmw";

} // namespace Ubiquitty::Rendering
