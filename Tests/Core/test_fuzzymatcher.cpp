// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_fuzzymatcher.cpp — pins Core/CommandPalette/FuzzyMatcher's ranking:
// the four tiers, the scattered-
// subsequence rejection, and the canonical abbreviations (set→Settings, // preferences-vocabulary-ok: a command title being matched
// newt→New Tab, splv→Split Vertically). Header + one .cpp; bare libc++.

#include "Core/CommandPalette/FuzzyMatcher.h"

#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::CommandPalette::FuzzyMatch;
using Ubiquitty::CommandPalette::fuzzyMatch;

namespace {
void expectTier(const char* query, const char* title, int tier)
{
    FuzzyMatch match = fuzzyMatch(query, title);
    if (!match.matched || match.tier != tier) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "'%s' vs '%s' — want tier %d, got %s tier %d", query, title, tier, match.matched ? "matched" : "no-match", match.tier);
        CHECK_MSG(false, ubqMessage);
    }
}

void expectNoMatch(const char* query, const char* title)
{
    FuzzyMatch match = fuzzyMatch(query, title);
    if (match.matched) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "'%s' vs '%s' — want no match, got tier %d", query, title, match.tier);
        CHECK_MSG(false, ubqMessage);
    }
}

void expectIndices(const char* query, const char* title, const std::vector<int>& want)
{
    FuzzyMatch match = fuzzyMatch(query, title);
    if (!match.matched || match.matchedIndices != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "'%s' vs '%s' — indices mismatch (got %zu)", query, title, match.matchedIndices.size());
        CHECK_MSG(false, ubqMessage);
    }
}

} // namespace

TEST(fuzzymatcher)
{

    // Empty query matches everything at tier 0 with no highlighted indices.
    {
        FuzzyMatch match = fuzzyMatch("", "New Tab");
        if (!match.matched || match.tier != 0 || !match.matchedIndices.empty()) {
            CHECK_MSG(false, "empty query should match at tier 0 with no indices");
        }
    }

    // A query longer than the title never matches.
    expectNoMatch("toolong", "hi");

    // Tier 1 — the query is a prefix (case-insensitive).
    expectTier("set", "Settings", 1); // preferences-vocabulary-ok: a command title being matched
    expectTier("SET", "settings", 1);
    expectIndices("set", "Settings", { 0, 1, 2 }); // preferences-vocabulary-ok: a command title

    // Tier 2 — a word in the title starts with the query.
    expectTier("tab", "New Tab", 2);
    expectIndices("tab", "New Tab", { 4, 5, 6 });

    // Tier 3 — a contiguous substring that is neither a prefix nor word-start.
    expectTier("ertic", "Split Vertically", 3);
    expectIndices("ertic", "Split Vertically", { 7, 8, 9, 10, 11 });

    // Tier 4 — constrained subsequence (every char at a boundary or contiguous).
    expectTier("newt", "New Tab", 4);
    expectIndices("newt", "New Tab", { 0, 1, 2, 4 });
    expectTier("splv", "Split Vertically", 4);
    expectIndices("splv", "Split Vertically", { 0, 1, 2, 6 });

    // Word-boundary initials are a clean tier-4 hit …
    expectTier("nt", "New Tab", 4);
    expectIndices("nt", "New Tab", { 0, 4 });

    // … but a fully-scattered mid-word subsequence is REJECTED.
    expectNoMatch("ea", "New Tab"); // e and a both land mid-word
    expectNoMatch("st", "Settings"); // preferences-vocabulary-ok: a command title; the two t's are neither boundary nor contiguous
    expectNoMatch("pi", "Split"); // p is mid-word, not contiguous with the start

    // Tier 4 must BACKTRACK: the greedy leftmost anchor ("ne" at 0) starves
    // the rest of "newta"; the valid alignment starts at the second word.
    expectTier("newta", "Ne New Tab", 4);
    expectIndices("newta", "Ne New Tab", { 3, 4, 5, 7, 8 });
    // Same class with the dead-end anchor inside the first word.
    expectTier("newt", "Net New Tab", 4);
    expectIndices("newt", "Net New Tab", { 4, 5, 6, 8 });

    // The completion pass must never scan below position 0 (a start derived
    // from the root's previous-match sentinel would read haystack[-1] and
    // could admit a garbage match at index -1). "aaaa" cannot match a 3-a
    // title under any alignment.
    expectNoMatch("aaaa", "aaa-");
    expectNoMatch("aa", "-a"); // one 'a' in the title can't satisfy a two-'a' query

    // The completion pass covers titles of any realistic length — a
    // backtracking-needing alignment 300+ scalars in must still match (a
    // haystack-size cutoff here once silently dropped it).
    {
        std::string longTitle(300, 'x');
        longTitle += " Ne New Tab";
        FuzzyMatch match = fuzzyMatch("newta", longTitle);
        if (!match.matched || match.tier != 4 || match.matchedIndices.front() != 304) {
            char ubqMessage[512];
            std::snprintf(ubqMessage, sizeof(ubqMessage), "long-title backtracking alignment (matched=%d tier=%d)", match.matched, match.tier);
            CHECK_MSG(false, ubqMessage);
        }
    }

    // matchedIndices are Unicode-SCALAR ordinals: the two-scalar prefix "é~"
    // (U+00E9 + ~) shifts a following word match by exactly 2 — not by UTF-8
    // bytes, not by grapheme clusters.
    expectTier("tab", "\xC3\xA9~ Tab", 2);
    expectIndices("tab", "\xC3\xA9~ Tab", { 3, 4, 5 });

    // U+25B8 (the group-path separator) is a word boundary — a query lands on
    // the word after it — and counts as one scalar in the index stream.
    expectTier("theme", "View \xE2\x96\xB8 Theme", 2);
    expectIndices("theme", "View \xE2\x96\xB8 Theme", { 7, 8, 9, 10, 11 });
}

// A candidate decoded once and matched many times must answer what the same
// candidate decoded per call answered. The engine caches the decode across
// keystrokes, so every difference between the two forms would be a palette that
// ranks differently after the cache than before it.
//
// Held over the corpus where a decode can differ from itself: multi-byte text,
// case that folds, word boundaries the tiers turn on, and the empty query that
// matches everything.
TEST(aDecodedCandidateMatchesWhatTheStringDid)
{
    const char* const candidates[] = {
        "",
        "a",
        "Open Settings", // preferences-vocabulary-ok: a command title
        "open settings",
        "OPEN SETTINGS",
        "Split Pane Right",
        "split-pane-right",
        "Ubiquitty: Reload Configuration",
        "\xC3\x9C"
        "bersicht \xC3\xB6"
        "ffnen", // Ubersicht offnen, with folding umlauts
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", // Japanese, no case to fold
        "tab separated words",
        "  leading and trailing  ",
    };
    const char* const queries[] = {
        "",
        "o",
        "O",
        "os",
        "open",
        "OPEN",
        "opn",
        "settings",
        "spr",
        "pane",
        "\xC3\xBC",
        "\xC3\x9C",
        "\xE6\x97\xA5",
        "zzz",
        "reload config",
    };

    int comparisons = 0;
    int matches = 0;
    for (const char* candidate : candidates) {
        const std::vector<char32_t> decoded = Ubiquitty::CommandPalette::decodeCandidate(candidate);
        for (const char* query : queries) {
            const std::vector<char32_t> needles = Ubiquitty::CommandPalette::decodeQuery(query);
            const Ubiquitty::CommandPalette::FuzzyMatch fromString
                = Ubiquitty::CommandPalette::fuzzyMatchDecoded(needles, std::string_view(candidate));
            const Ubiquitty::CommandPalette::FuzzyMatch fromDecoded
                = Ubiquitty::CommandPalette::fuzzyMatchDecoded(needles, decoded);
            CHECK_MSG(fromString.matched == fromDecoded.matched, "the two forms disagree about whether it matched");
            CHECK_MSG(fromString.tier == fromDecoded.tier, "the two forms disagree about the tier");
            CHECK_MSG(fromString.position == fromDecoded.position, "the two forms disagree about the position");
            CHECK_MSG(fromString.matchedIndices == fromDecoded.matchedIndices, "the two forms disagree about the highlight");
            if (fromString.matched)
                ++matches;
            ++comparisons;
        }
    }
    // Guards a corpus that compares nothing, and one where nothing matches —
    // two forms that both refuse everything agree trivially.
    CHECK_EQ(comparisons, 12 * 15);
    CHECK_MSG(matches > 0, "no pair matched, so the agreement is about refusals only");
}

UBIQUITTY_TEST_MAIN()
