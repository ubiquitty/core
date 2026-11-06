// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// PaletteFrecency.h — the palette's recency+frequency boost (the VSCode-style
// "commands you actually use float up" behavior). The FRONTEND stores raw use
// timestamps per command id and feeds them per summon; the decay math and the
// ranking effect live here so every frontend orders identically.
//
// Ranking effect (see PaletteEngine):
//   - blank query: commands with a score sort first (by score), the rest keep
//     the scope's blank-query order after them.
//   - ranked query: score breaks ties INSIDE (tier, position) — frequency
//     never beats match quality — ahead of the shorter-title tiebreak.
//

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::CommandPalette {

// One command's use history, newest-first not required (order is irrelevant).
// `timestamps` are seconds in any monotonic-enough epoch the frontend likes
// (wall clock is fine; only differences matter).
struct UsageHistory {
    std::string id;
    std::vector<std::int64_t> timestamps;
};

// Retention policy for the frontend's store, shared so both sides agree:
// keep at most this many stamps per id (drop oldest).
inline constexpr int maxFrecencyStampsPerID = 10;

// Half-life of one use, in seconds (7 days): a use contributes 2^(-age/halfLife)
// to the score, so ten uses last week ≈ five uses today, and anything untouched
// for months decays to noise.
inline constexpr double frecencyHalfLifeSeconds = 7.0 * 24.0 * 60.0 * 60.0;

// The decayed score of one history at `now`. Future-dated stamps (clock skew)
// count as `now`. An empty history scores 0.
double frecencyScore(const UsageHistory& history, std::int64_t now);

} // namespace Ubiquitty::CommandPalette
