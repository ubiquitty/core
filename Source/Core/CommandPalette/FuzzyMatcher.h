// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// FuzzyMatcher.h — the palette's tiered fuzzy matcher (Sublime / fzf feel).
// Lower tier = stronger, in order:
//   1 prefix · 2 word-boundary prefix · 3 contiguous substring · 4 constrained
//   subsequence (every matched char at a word boundary or contiguous with the
//   previous match). Fully-scattered subsequences are rejected, so only
//   intuitive hits surface. Pure — no state, one function.
//

#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::CommandPalette {

// The outcome of a match. `matched` false is "no match"; an empty query is a
// match at tier 0 with no highlighted indices. `position` is the first matched
// code-point index (earlier ranks higher); `matchedIndices` are ascending
// code-point indices into the title.
struct FuzzyMatch {
    bool matched = false;
    int tier = 0;
    int position = 0;
    std::vector<int> matchedIndices;
};

// Match `query` against `title`, both ASCII-case-folded. Word boundaries are the
// string start or a char just after space / '-' / '/' / ':' / '▸'.
FuzzyMatch fuzzyMatch(std::string_view query, std::string_view title);

// Decode + case-fold a query once for a whole ranking pass; feed the result to
// fuzzyMatchDecoded per candidate. The engine matches every command title and
// every keyword per keystroke — per-candidate query re-decoding is pure waste.
std::vector<char32_t> decodeQuery(std::string_view query);
FuzzyMatch fuzzyMatchDecoded(const std::vector<char32_t>& queryNeedles, std::string_view title);

/// Decodes and case-folds a candidate string into the form the matcher scans.
///
/// A candidate is matched against once per keystroke and does not change between them, so a caller
/// holding a fixed set of them decodes each once rather than on every pass. The query side already
/// works this way through ``decodeQuery``; this is the same for the side that is N times larger.
///
/// - Parameter title: The candidate's text.
/// - Returns: Its code points, case-folded.
[[nodiscard]] std::vector<char32_t> decodeCandidate(std::string_view title);

/// Matches a pre-decoded query against a pre-decoded candidate.
///
/// - Parameters:
///   - queryNeedles: From ``decodeQuery``.
///   - haystack: From ``decodeCandidate``.
/// - Returns: The match, whose indices are into the candidate's code points.
[[nodiscard]] FuzzyMatch fuzzyMatchDecoded(const std::vector<char32_t>& queryNeedles, const std::vector<char32_t>& haystack);

} // namespace Ubiquitty::CommandPalette
