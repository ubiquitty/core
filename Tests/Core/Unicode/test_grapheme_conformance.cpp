// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_grapheme_conformance.cpp — extended grapheme cluster boundaries
// (UAX #29), scored line by line against the Unicode Consortium's corpus.
//
// The hand-written grapheme tests pin the rules their authors had in mind. A
// conformance corpus pins the ones nobody had in mind, and every position in it
// is scored through both the full segmenter and its hot-path wrapper.
//
// The corpus (GraphemeBreakTest.txt) is vendored beside this file and the build
// hands its path in as GRAPHEME_BREAK_TEST_CORPUS. A conformance corpus is the
// test's fixture, not a generation input: table generation fetches UCD per
// build-grapheme-tables.py, but a fixture the test needs to run cannot be an
// optional download, or the check never runs. The corpus must track the version
// the tables were generated from (GraphemeBreak.tables.inc header — UCD 17.0.0);
// a mismatch reads version differences as failures, so refresh both together.
//
// $GRAPHEME_BREAK_TEST or argv[1] override the vendored corpus, so a newer one
// can be tried against the current tables without touching the checkout.
//
// Each corpus line is one case, with a mark between code points:
//
//   ÷ 000D ÷ 0308 × 200D ÷   # the rules that apply, as prose
//
// `÷` (U+00F7) marks a cluster boundary and `×` (U+00D7) marks a join.

#include "Harness.h"

#include "Core/Unicode/GraphemeBreak.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef GRAPHEME_BREAK_TEST_CORPUS
#error "GRAPHEME_BREAK_TEST_CORPUS must name the vendored corpus; see Tests/CMakeLists.txt"
#endif

namespace {

// The corpus marks, as UTF-8.
constexpr std::string_view boundaryMark = "\xc3\xb7";
constexpr std::string_view joinMark = "\xc3\x97";

/// Mismatches past this many are counted but not printed.
constexpr int maxReportedMismatches = 20;

// How a failure report spells the two answers a position can have.
constexpr const char* joinedText = "NO BREAK (x)";
constexpr const char* brokenText = "BREAK (/)";

/// One code point of a case, and whether the corpus puts a boundary before it.
struct CorpusPosition {
    std::uint32_t codePoint;
    bool isBoundaryBefore;
};

/// One scored corpus line.
///
/// The first position always opens a cluster, so its boundary is not scored.
struct CorpusCase {
    int lineNumber = 0;
    std::string uncommented;
    std::vector<CorpusPosition> positions;
};

/// Reads one corpus line into a case.
///
/// - Parameters:
///   - line: The line as read, comment included.
///   - lineNumber: The line's one-based position in the corpus.
/// - Returns: The case, or `std::nullopt` when the line names no code point
///   before its comment.
std::optional<CorpusCase> parseCorpusLine(const std::string& line, int lineNumber)
{
    CorpusCase parsed;
    parsed.lineNumber = lineNumber;
    parsed.uncommented = line.substr(0, line.find('#'));

    // A code point not preceded by a mark gets a boundary, as the first one does.
    bool isBoundaryPending = true;
    std::istringstream tokens(parsed.uncommented);
    for (std::string token; tokens >> token;) {
        if (token == boundaryMark || token == joinMark) {
            isBoundaryPending = token == boundaryMark;
        } else {
            const auto codePoint = static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16));
            parsed.positions.push_back({ codePoint, isBoundaryPending });
            isBoundaryPending = true;
        }
    }
    if (parsed.positions.empty())
        return std::nullopt;
    return parsed;
}

/// What one pass over the corpus scored.
struct CorpusTally {
    int scoredCases = 0;
    int unscoredMarkedLines = 0;
    int mismatches = 0;
};

/// Scores every position of a case through the full segmenter and the hot path.
///
/// Each path is driven the way the clusterer drives it: its state extends
/// across a position it joins and resets at one it breaks. After every step the
/// hot path's state must equal the full path's, field by field.
///
/// - Parameters:
///   - corpusCase: The case to score, which `parseCorpusLine` never leaves
///     without a position.
///   - tally: Counts the positions either path scored wrong.
void scoreCase(const CorpusCase& corpusCase, CorpusTally& tally)
{
    const std::vector<CorpusPosition>& positions = corpusCase.positions;
    assert(!positions.empty());

    Ubiquitty::Unicode::GraphemeState fullState {};
    Ubiquitty::Unicode::graphemeStateReset(&fullState, positions.front().codePoint);
    Ubiquitty::Unicode::GraphemeState hotState = fullState;

    for (std::size_t index = 1; index < positions.size(); ++index) {
        const std::uint32_t previous = positions[index - 1].codePoint;
        const std::uint32_t current = positions[index].codePoint;
        const bool shouldExtend = !positions[index].isBoundaryBefore;
        const bool doesFullExtend = Ubiquitty::Unicode::graphemeExtends(&fullState, previous, current);
        const bool doesHotExtend = Ubiquitty::Unicode::graphemeExtendsHot(&hotState, previous, current);

        if (doesFullExtend != shouldExtend || doesHotExtend != shouldExtend) {
            if (tally.mismatches < maxReportedMismatches) {
                std::printf("FAIL GraphemeBreakTest.txt:%d: U+%04X then U+%04X should be %s; "
                            "the full path says %s, the hot path %s\n    %s\n",
                    corpusCase.lineNumber, previous, current, shouldExtend ? joinedText : brokenText,
                    doesFullExtend ? joinedText : brokenText, doesHotExtend ? joinedText : brokenText,
                    corpusCase.uncommented.c_str());
            }
            ++tally.mismatches;
            ++failures;
        }

        CHECK(hotState.riCount == fullState.riCount);
        CHECK(hotState.pictExtend == fullState.pictExtend);
        CHECK(hotState.zwjAfterPict == fullState.zwjAfterPict);
        CHECK(hotState.prevRaw == fullState.prevRaw);
        CHECK(hotState.incbChain == fullState.incbChain);
        if (!doesFullExtend)
            Ubiquitty::Unicode::graphemeStateReset(&fullState, current);
        if (!doesHotExtend)
            Ubiquitty::Unicode::graphemeStateReset(&hotState, current);
    }
}

} // namespace

int main(int argumentCount, char** arguments)
{
    // An argument wins, then $GRAPHEME_BREAK_TEST, then the vendored corpus.
    // The vendored path is required when the test compiles, so no build yields
    // a binary that skips for want of a corpus.
    const char* corpusPath = argumentCount > 1 ? arguments[1] : std::getenv("GRAPHEME_BREAK_TEST");
    if (!corpusPath || !*corpusPath)
        corpusPath = GRAPHEME_BREAK_TEST_CORPUS;

    std::ifstream corpus(corpusPath);
    if (!corpus) {
        std::printf("FAIL: the corpus %s cannot be opened\n", corpusPath);
        return EXIT_FAILURE;
    }

    CorpusTally tally;
    std::string line;
    for (int lineNumber = 1; std::getline(corpus, line); ++lineNumber) {
        if (const std::optional<CorpusCase> parsed = parseCorpusLine(line, lineNumber)) {
            ++tally.scoredCases;
            scoreCase(*parsed, tally);
        } else if (line.find(boundaryMark) != std::string::npos) {
            ++tally.unscoredMarkedLines;
        }
    }

    // A corpus that scores nothing is the wrong file, or one in a format this
    // reader does not know, and must not pass as a clean run.
    if (!tally.scoredCases) {
        std::printf("FAIL: the corpus %s holds no case this reader can score\n", corpusPath);
        return EXIT_FAILURE;
    }

    std::printf("\nGraphemeBreakTest: %d cases scored, %d lines carrying a boundary mark "
                "but no code point left unscored, %d boundaries mismatched\n",
        tally.scoredCases, tally.unscoredMarkedLines, tally.mismatches);
    return UBIQUITTY_TEST_SUMMARY();
}
