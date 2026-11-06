// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The batched text run must not depend on where a read landed.
//
// emitTextRun decides how much to place by looking ahead inside the buffer it
// was handed: it absorbs an ASCII gap only when more multibyte text follows,
// and it batches a pictograph only when the code point after it does not join.
// Both questions have a different answer when the deciding byte is in the next
// chunk, so the run a caller gets depends on how the bytes arrived.
//
// That must not reach the grid. A terminal fed one byte at a time and a
// terminal fed the whole buffer are the same terminal, and the boundary cases
// are exactly the ones a PTY produces: a read ends mid-word, mid-sequence, or
// between a pictograph and its joiner.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/TerminalOperations.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

// The visible grid as code points, so two feeds can be compared without
// knowing what the correct grid is — only that the two agree.
std::vector<std::uint32_t> gridCodePoints(const Ubiquitty::Terminal& t)
{
    std::vector<std::uint32_t> points;
    points.reserve(static_cast<std::size_t>(t.rows * t.columns));
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Ubiquitty::Cell* c = UbiquittyTest::cell(t, row, column);
            points.push_back(c ? c->codePoint : 0u);
        }
    }
    return points;
}

std::vector<std::uint32_t> feedInChunks(std::string_view bytes, int chunkSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t take = std::min(static_cast<std::size_t>(chunkSize), bytes.size() - offset);
        Ubiquitty::processBytes(&t, bytes.substr(offset, take));
        offset += take;
    }
    return gridCodePoints(t);
}

// Every case pairs a lookahead the batcher makes with a boundary that can fall
// inside it.
const char* const cases[] = {
    "\xC3\xA0\xC3\xA9 \xC3\xAE\xC3\xB5 \xC3\xBC", // ASCII gaps between multibyte words
    "\xC3\xA0 \xC3\xA9 \xC3\xAE", // one-byte gaps
    "\xC3\xA0\xC3\xA9 plain ascii tail", // a tail the run must not take
    "\xC3\xA0   \xC3\xA9", // a wider gap
    "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80", // adjacent pictographs, no joiner
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", // pictograph, ZWJ, pictograph
    "\xF0\x9F\x98\x80\xE2\x9D\xA4\xEF\xB8\x8F", // pictograph then VS16 sequence
    "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87\xAB", // three regional indicators
    "\xC3\xA0 \xF0\x9F\x98\x80 \xE4\xBD\xA0\xE5\xA5\xBD", // gaps around emoji and CJK
    "a\xCC\x81 \xC3\xA9\xCC\x80", // decomposed marks either side of a gap
    "\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C", // wide glyphs with a gap
    "\xC3\xA0\xC3\xA9\xC3\xAE\xC3\xB5\xC3\xBC", // no gap at all
};

} // namespace

TEST(testTextRunPlacesTheSameGridAtEveryChunkSize)
{
    for (const char* const text : cases) {
        const std::string_view bytes { text };
        const std::vector<std::uint32_t> whole = feedInChunks(bytes, static_cast<int>(bytes.size()));
        // 1 splits every sequence; the rest walk the boundary across each
        // lookahead the batcher performs.
        for (const int chunkSize : { 1, 2, 3, 4, 5, 7, 11 }) {
            const std::vector<std::uint32_t> split = feedInChunks(bytes, chunkSize);
            CHECK(split == whole);
        }
    }
}

TEST(testTextRunChunkingComparisonSeesADifference)
{
    // The comparison above is only worth its runtime if an actual difference
    // reaches it: two different feeds must not compare equal.
    const std::vector<std::uint32_t> one = feedInChunks("\xC3\xA0\xC3\xA9", 4);
    const std::vector<std::uint32_t> other = feedInChunks("\xC3\xA0\xC3\xAE", 4);
    CHECK(!(one == other));
}

UBIQUITTY_TEST_MAIN()
