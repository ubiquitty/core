// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// FuzzyMatcher.cpp — the command palette's fuzzy match. Titles are decoded to
// code points so a match index lines up with a frontend's per-character
// highlight (1:1 for the palette's Latin command text).
//

#include "Core/CommandPalette/FuzzyMatcher.h"

#include <cstddef>

namespace Ubiquitty::CommandPalette {

namespace {

// U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE — the group-path separator, kept a
// word boundary so a query can jump across it.
constexpr char32_t groupSeparatorCodePoint = 0x25B8;

// ASCII case-fold (the palette's command text is Latin; non-ASCII passes
// through, matching the frontend's lowercased() on the same run).
char32_t foldCase(char32_t codePoint)
{
    if (codePoint >= U'A' && codePoint <= U'Z')
        return codePoint + 32;
    return codePoint;
}

// Lenient UTF-8 → code points: a malformed lead / truncated tail yields the raw
// byte as one code point, so an odd byte can never desync the index stream.
std::vector<char32_t> decode(std::string_view text)
{
    std::vector<char32_t> out;
    out.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t codePoint = lead;
        std::size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            extra = 3;
        } else {
            out.push_back(lead);
            ++i;
            continue;
        }
        if (i + extra >= n) {
            out.push_back(lead);
            ++i;
            continue;
        }
        bool ok = true;
        char32_t assembled = codePoint;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            assembled = (assembled << 6) | (cont & 0x3F);
        }
        if (!ok) {
            out.push_back(lead);
            ++i;
            continue;
        }
        out.push_back(assembled);
        i += extra + 1;
    }
    return out;
}

bool isBoundaryCharacter(char32_t codePoint) { return codePoint == U' ' || codePoint == U'-' || codePoint == U'/' || codePoint == U':' || codePoint == groupSeparatorCodePoint; }

bool isBoundary(const std::vector<char32_t>& characters, int index) { return index == 0 || isBoundaryCharacter(characters[index - 1]); }

bool substringMatches(const std::vector<char32_t>& haystack, int start, const std::vector<char32_t>& needles)
{
    if (start + static_cast<int>(needles.size()) > static_cast<int>(haystack.size()))
        return false;
    for (int offset = 0; offset < static_cast<int>(needles.size()); ++offset) {
        if (haystack[start + offset] != needles[offset])
            return false;
    }
    return true;
}

FuzzyMatch rangeMatch(int tier, int start, int count)
{
    FuzzyMatch match;
    match.matched = true;
    match.tier = tier;
    match.position = start;
    match.matchedIndices.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        match.matchedIndices.push_back(start + i);
    return match;
}

// The tier-4 anchor rule, shared by the greedy pass and the feasibility table
// below: a needle may land on `position` only at a word boundary or flush
// against the previous match. One definition — the greedy≡table equivalence
// depends on the two passes judging positions identically.
bool anchorsAt(const std::vector<char32_t>& haystack, const std::vector<char32_t>& needles, int needleIndex, int position, int previous) { return haystack[position] == needles[needleIndex] && (isBoundary(haystack, position) || position == previous + 1); }

// Tier-4 fast path: a single greedy left-to-right pass, allocation-free and
// O(haystack). When it completes, its choices are exactly the leftmost-first
// alignment (each needle takes the first anchorable position, and completion
// means no choice ever needs revisiting), so the table pass below runs only
// on greedy failure.
bool subsequenceGreedy(const std::vector<char32_t>& haystack, const std::vector<char32_t>& needles, std::vector<int>& chosen)
{
    const int needleCount = static_cast<int>(needles.size());
    const int haystackCount = static_cast<int>(haystack.size());
    int needleIndex = 0;
    // Root sentinel -1: its contiguity slot is position 0, which is always a
    // word boundary anyway, so the first needle is boundary-gated either way.
    int previous = -1;
    for (int position = 0; position < haystackCount && needleIndex < needleCount; ++position) {
        if (anchorsAt(haystack, needles, needleIndex, position, previous)) {
            chosen.push_back(position);
            previous = position;
            ++needleIndex;
        }
    }
    if (needleIndex == needleCount)
        return true;
    chosen.clear();
    return false;
}

// Work cap for the table pass: needles × haystack cells. 1M cells ≈ a
// sub-millisecond fill; a pasted-novel query against an OSC-inflated title
// lands above it and keeps the greedy answer instead. Any real command
// title × query is thousands of cells.
constexpr long long subsequenceMaxCells = 1'000'000;

// Tier-4 completion when greedy dead-ends (an early anchor can starve a later
// needle: "newta" against "Ne New Tab" locks n@0,e@1 and then finds no
// boundary `w`). feasible[ni][p+1] answers "can needles[ni..] be matched with
// the previous match sitting at p?" — computed bottom-up in O(needles ×
// haystack) with a running any-boundary-anchor-to-the-right accumulator, no
// recursion. The leftmost alignment then reads off forward: each needle takes
// the first anchorable position whose suffix stays feasible.
bool subsequenceTable(const std::vector<char32_t>& haystack, const std::vector<char32_t>& needles, std::vector<int>& chosen)
{
    const int needleCount = static_cast<int>(needles.size());
    const int haystackCount = static_cast<int>(haystack.size());
    // Row layout: feasible[ni * width + (p + 1)] for p in [-1, haystackCount-1];
    // p = -1 is the root "no match yet" state — its contiguity slot is
    // position 0, a word boundary by definition, so the root admits exactly
    // the boundary-anchored set (same sentinel semantics as the greedy pass).
    // The row fill decomposes anchorsAt into its two disjuncts: the boundary
    // half rides the right-to-left accumulator, the contiguity half is the
    // single explicit probe at p + 1.
    const int width = haystackCount + 1;
    std::vector<unsigned char> feasible(static_cast<std::size_t>(needleCount + 1) * static_cast<std::size_t>(width), 0);
    for (int p = 0; p < width; ++p)
        feasible[static_cast<std::size_t>(needleCount) * width + p] = 1; // nothing left
    for (int needleIndex = needleCount - 1; needleIndex >= 0; --needleIndex) {
        unsigned char* row = feasible.data() + static_cast<std::size_t>(needleIndex) * width;
        const unsigned char* next = row + width;
        // Right-to-left: boundaryRight = "some boundary-anchorable position for
        // this needle exists strictly right of p whose suffix is feasible".
        unsigned char boundaryRight = 0;
        for (int p = haystackCount - 1; p >= -1; --p) {
            const int contiguous = p + 1;
            unsigned char ok = boundaryRight;
            if (!ok && contiguous < haystackCount && haystack[contiguous] == needles[needleIndex] && next[contiguous + 1])
                ok = 1; // flush against the previous match
            row[p + 1] = ok;
            if (p >= 0 && haystack[p] == needles[needleIndex] && isBoundary(haystack, p) && next[p + 1])
                boundaryRight = 1; // p itself serves states left of p
        }
    }
    if (!feasible[0 * width + 0]) // root: ni = 0, previous = -1
        return false;
    int previous = -1;
    for (int needleIndex = 0; needleIndex < needleCount; ++needleIndex) {
        const unsigned char* next = feasible.data() + static_cast<std::size_t>(needleIndex + 1) * width;
        int pick = -1;
        for (int p = previous + 1; p < haystackCount; ++p) {
            if (anchorsAt(haystack, needles, needleIndex, p, previous) && next[p + 1]) {
                pick = p;
                break;
            }
        }
        if (pick < 0) {
            chosen.clear();
            return false; // unreachable when the root is feasible
        }
        chosen.push_back(pick);
        previous = pick;
    }
    return true;
}

} // namespace

std::vector<char32_t> decodeQuery(std::string_view query)
{
    std::vector<char32_t> needles = decode(query);
    for (char32_t& character : needles)
        character = foldCase(character);
    return needles;
}

FuzzyMatch fuzzyMatch(std::string_view query, std::string_view title) { return fuzzyMatchDecoded(decodeQuery(query), title); }

std::vector<char32_t> decodeCandidate(std::string_view title)
{
    std::vector<char32_t> folded = decode(title);
    for (char32_t& character : folded)
        character = foldCase(character);
    return folded;
}

FuzzyMatch fuzzyMatchDecoded(const std::vector<char32_t>& needles, std::string_view title)
{
    if (needles.empty()) {
        FuzzyMatch empty;
        empty.matched = true;
        return empty; // tier 0, no indices — an empty query matches everything
    }
    return fuzzyMatchDecoded(needles, decodeCandidate(title));
}

FuzzyMatch fuzzyMatchDecoded(const std::vector<char32_t>& needles, const std::vector<char32_t>& haystack)
{
    if (needles.empty()) {
        FuzzyMatch empty;
        empty.matched = true;
        return empty; // tier 0, no indices — an empty query matches everything
    }

    if (needles.size() > haystack.size())
        return {}; // matched == false

    const int needleCount = static_cast<int>(needles.size());
    const int haystackCount = static_cast<int>(haystack.size());

    // Tier 1 — the query is a prefix of the title.
    if (substringMatches(haystack, 0, needles))
        return rangeMatch(1, 0, needleCount);

    // Tier 2 — a word in the title starts with the query (earliest boundary).
    for (int start = 0; start < haystackCount; ++start) {
        if (isBoundary(haystack, start) && substringMatches(haystack, start, needles))
            return rangeMatch(2, start, needleCount);
    }

    // Tier 3 — contiguous substring anywhere (earliest position).
    for (int start = 0; start < haystackCount; ++start) {
        if (substringMatches(haystack, start, needles))
            return rangeMatch(3, start, needleCount);
    }

    // Tier 4 — subsequence where every matched char is at a word boundary or
    // contiguous with the previous match; scattered mid-word hits are rejected.
    // Greedy first (allocation-free, covers virtually all real matches); the
    // O(needles × haystack) feasibility table completes the search when greedy
    // dead-ends, under a work cap that only pathological input can reach.
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(needleCount));
    bool matched = subsequenceGreedy(haystack, needles, chosen);
    if (!matched && static_cast<long long>(needleCount) * static_cast<long long>(haystackCount) <= subsequenceMaxCells) {
        matched = subsequenceTable(haystack, needles, chosen);
    }
    if (!matched)
        return {}; // matched == false
    FuzzyMatch match;
    match.matched = true;
    match.tier = 4;
    match.matchedIndices = std::move(chosen);
    match.position = match.matchedIndices.empty() ? 0 : match.matchedIndices.front();
    return match;
}

} // namespace Ubiquitty::CommandPalette
