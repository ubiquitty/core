// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_bidi_conformance.cpp — the Unicode Bidirectional Algorithm (UAX #9)
// against the Unicode Consortium's own corpus.
//
// Ubiquitty's bidi today is cursive joining plus reversal of each maximal
// strong-RTL run (Core/Unicode/Joining.cpp), which is not the algorithm.
// Naming it "minimal BiDi" in a header describes the gap; this measures it,
// and a measurement is what an implementation can be held to while it closes.
// The case passes at today's distance from the identity baseline and fails
// when that distance shrinks — see minOverIdentity below for why the gate is
// the distance and not the share.
//
// The corpus (BidiCharacterTest.txt) is vendored beside this file on the same
// terms as GraphemeBreakTest.txt: a fixture the test needs in order to run
// cannot be an optional download, or the check never runs. It must track the
// version the tables were generated from (UCD 17.0.0).
//
// Corpus format, one case per line, five semicolon-separated fields:
//   codepoints ; paragraph-direction ; paragraph-level ; levels ; visual-order
// Field 1 is 0=LTR, 1=RTL, 2=auto. Field 4 is the indices of the input in
// visual order, with the characters UAX #9 removes omitted entirely — so a
// length difference is itself a conformance failure and is counted apart from a
// wrong ordering, because the two are different bugs.
//
// What is scored here is field 4 against shapeVisualRow's visual->logical
// permutation, which is the same question a terminal asks: what does the row
// look like, left to right. Levels (field 3) are not scored: this code resolves
// none, so scoring them would report 100% failure and say nothing that the
// ordering does not already say more usefully.

#include "Harness.h"

#include "Core/Unicode/Joining.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef BIDI_CHARACTER_TEST_CORPUS
#error "BIDI_CHARACTER_TEST_CORPUS must name the vendored corpus; see Tests/CMakeLists.txt"
#endif

namespace {

// A conformance score is meaningless without its null, and this corpus has a
// large one: many of its cases need no reordering at all, so an
// implementation that returned the identity permutation and did nothing else
// would score that. A floor on the share alone sits so close to that null
// that it survives deleting the RTL run reversal it guards.
//
// So both numbers are recorded, and the gate is the DISTANCE between them.
// `identityBaseline` is measured by the case below rather than asserted, so it
// cannot rot; `minOverIdentity` is what the implementation must earn on top
// of it: what run reversal is worth today, which is the size of "minimal
// BiDi" measured against the algorithm rather than described. Every step
// toward UAX #9 raises it, and the commit that earns the rise raises the
// constant with it.
constexpr int minOverIdentity = 7;

std::vector<std::string> splitOn(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, separator))
        parts.push_back(part);
    return parts;
}

std::vector<std::uint32_t> parseCodePoints(const std::string& field)
{
    std::vector<std::uint32_t> out;
    std::istringstream in(field);
    std::string token;
    while (in >> token)
        out.push_back(static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
    return out;
}

std::vector<int> parseIndices(const std::string& field)
{
    std::vector<int> out;
    std::istringstream in(field);
    std::string token;
    while (in >> token)
        out.push_back(std::atoi(token.c_str()));
    return out;
}

} // namespace

TEST(testTheVisualOrderMatchesTheBidiCorpusAtOrAboveTheRecordedFloor)
{
    const char* override = std::getenv("BIDI_CHARACTER_TEST");
    const std::string path = override ? override : BIDI_CHARACTER_TEST_CORPUS;
    std::ifstream corpus(path);
    CHECK_MSG(corpus.is_open(), path.c_str());
    if (!corpus.is_open())
        return;

    // Counted per paragraph direction because shapeVisualRow accepts none: it
    // is one behaviour being asked three different questions, and a single
    // total hides which of the three it answers. The RTL column is the one that
    // shows a missing paragraph level rather than a wrong reordering.
    int scored[3] = { 0, 0, 0 };
    int passed[3] = { 0, 0, 0 };
    int wrongOrder = 0;
    int wrongLength = 0;
    int shown = 0;
    // Cases the identity permutation would satisfy — the score of doing
    // nothing. Counted from the same lines in the same pass so the two numbers
    // cannot drift apart or be compared across corpus versions.
    int identityPasses = 0;

    std::string line;
    while (std::getline(corpus, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = splitOn(line, ';');
        if (fields.size() < 5)
            continue;

        const std::vector<std::uint32_t> logical = parseCodePoints(fields[0]);
        const int direction = std::atoi(fields[1].c_str());
        const std::vector<int> expected = parseIndices(fields[4]);
        if (logical.empty() || direction < 0 || direction > 2)
            continue;

        const int length = static_cast<int>(logical.size());
        std::vector<std::uint32_t> visual(static_cast<std::size_t>(length));
        std::vector<std::uint16_t> permutation(static_cast<std::size_t>(length));
        (void)Ubiquitty::Unicode::shapeVisualRow(logical.data(), visual.data(), permutation.data(), length);

        scored[direction]++;

        if (static_cast<int>(expected.size()) != length) {
            wrongLength++;
            continue;
        }
        bool isIdentityOrder = true;
        for (int i = 0; i < length; ++i) {
            if (expected[static_cast<std::size_t>(i)] != i) {
                isIdentityOrder = false;
                break;
            }
        }
        if (isIdentityOrder)
            identityPasses++;
        bool isExpectedOrder = true;
        for (int i = 0; i < length; ++i) {
            if (static_cast<int>(permutation[static_cast<std::size_t>(i)]) != expected[static_cast<std::size_t>(i)]) {
                isExpectedOrder = false;
                break;
            }
        }
        if (isExpectedOrder) {
            passed[direction]++;
            continue;
        }
        wrongOrder++;
        if (shown < 3) {
            shown++;
            std::printf("  e.g. paragraph dir %d, input:", direction);
            for (std::uint32_t codePoint : logical)
                std::printf(" %04X", codePoint);
            std::printf("\n       want:");
            for (int index : expected)
                std::printf(" %d", index);
            std::printf("\n       got :");
            for (int i = 0; i < length; ++i)
                std::printf(" %d", static_cast<int>(permutation[static_cast<std::size_t>(i)]));
            std::printf("\n");
        }
    }

    const int total = scored[0] + scored[1] + scored[2];
    const int pass = passed[0] + passed[1] + passed[2];
    CHECK_MSG(total > 0, "the corpus produced no scorable cases — check the vendored file");
    if (total == 0)
        return;

    const double share = static_cast<double>(pass) / static_cast<double>(total);
    const int overIdentity = pass - identityPasses;
    std::printf("\nBidiCharacterTest: %d cases, %d pass (%.1f%%)\n", total, pass, 100.0 * share);
    std::printf("  identity would pass %d (%.2f%%) — the score of doing nothing\n",
        identityPasses, 100.0 * static_cast<double>(identityPasses) / static_cast<double>(total));
    std::printf("  earned over identity %d   (floor %d)\n", overIdentity, minOverIdentity);
    std::printf("  wrong order  %d\n", wrongOrder);
    std::printf("  wrong length %d  (characters UAX #9 removes; this code removes none)\n", wrongLength);
    std::printf("  by paragraph direction — LTR %d/%d, RTL %d/%d, auto %d/%d\n",
        passed[0], scored[0], passed[1], scored[1], passed[2], scored[2]);
    std::printf("  RTL is the paragraph level this implementation does not have:\n"
                "  it assumes an LTR base and reverses RTL runs inside it.\n");

    // The gate is the distance from doing nothing, not the raw share. A share
    // floor passes an implementation that has been deleted.
    CHECK_MSG(overIdentity >= minOverIdentity, "bidi reordering earned less over the identity baseline than it did before");
}

UBIQUITTY_TEST_MAIN()
