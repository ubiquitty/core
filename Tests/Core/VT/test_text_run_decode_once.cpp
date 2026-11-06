// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What a batched text run decodes past a stop does not grow with the row, and
// placing text that way leaves the same grid as placing it a byte at a time.
//
// emitTextRun decodes ahead and places what it can, stopping at the first code
// point the cluster path owns, and the next run decodes again from there. The
// decode budget keeps what a stop costs to a little beyond what was placed, so
// text dense with combining marks does not decode the rest of the row once per
// mark. The count is every byte utf8DecodeRun consumed while the terminal parsed
// the input.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/Cell.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint32_t nextRandom(std::uint64_t& state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 33);
}

// Accented prose: letters from Latin-1 and Cyrillic in words, with a combining
// mark on about one letter in six, the way decomposed text carries them.
std::string markedProse(std::uint64_t seed, int letters)
{
    std::uint64_t state = seed;
    std::string text;
    for (int letter = 0; letter < letters; ++letter) {
        const std::uint32_t base = nextRandom(state) % 2 ? 0xE0 + nextRandom(state) % 0x1F : 0x430 + nextRandom(state) % 0x20;
        Ubiquitty::Unicode::appendUTF8(text, base);
        if (nextRandom(state) % 6 == 0)
            Ubiquitty::Unicode::appendUTF8(text, 0x300 + nextRandom(state) % 0x10);
        if (nextRandom(state) % 7 == 0)
            text.push_back(' ');
    }
    return text;
}

// Everything the batched writers and the cluster path hand to one another:
// stacked marks, wide glyphs, joined and lone pictographs, flags, selectors,
// gaps, ASCII words, pen changes, line breaks, and malformed bytes.
std::string mixedText(std::uint64_t seed, int pieces)
{
    std::uint64_t state = seed;
    std::string text;
    for (int piece = 0; piece < pieces; ++piece) {
        switch (nextRandom(state) % 14) {
        case 0:
            text.push_back(' ');
            break;
        case 1:
            text += "ascii";
            break;
        case 2:
            Ubiquitty::Unicode::appendUTF8(text, 0x300 + nextRandom(state) % 0x70);
            break;
        case 3:
            Ubiquitty::Unicode::appendUTF8(text, 0x300 + nextRandom(state) % 0x70);
            Ubiquitty::Unicode::appendUTF8(text, 0x300 + nextRandom(state) % 0x70);
            break;
        case 4:
            Ubiquitty::Unicode::appendUTF8(text, 0x4E00 + nextRandom(state) % 0x5000);
            break;
        case 5:
            Ubiquitty::Unicode::appendUTF8(text, 0x1F600 + nextRandom(state) % 0x50);
            break;
        case 6:
            text += "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB"; // man, ZWJ, laptop
            break;
        case 7:
            text += "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"; // a flag
            break;
        case 8:
            text += "\xE2\x9D\xA4\xEF\xB8\x8F"; // heart, VS16
            break;
        case 9:
            text += "\x1B[1;3" + std::to_string(nextRandom(state) % 8) + "m";
            break;
        case 10:
            text += nextRandom(state) % 3 ? "\r\n" : "\x1B[0m";
            break;
        case 11:
            text.push_back(static_cast<char>(nextRandom(state) % 2 ? 0x80 + nextRandom(state) % 0x40 : 0xE2));
            break;
        default:
            Ubiquitty::Unicode::appendUTF8(text, 0x80 + nextRandom(state) % 0x780);
            break;
        }
    }
    return text;
}

struct GridCell {
    std::vector<std::uint32_t> cluster;
    std::uint64_t attributes { 0 };
    std::uint32_t linkID { 0 };
    bool operator==(const GridCell&) const = default;
};

struct Grid {
    std::vector<GridCell> cells;
    int cursorRow { 0 };
    int cursorColumn { 0 };
    bool wrapPending { false };
    bool operator==(const Grid&) const = default;
};

// Every visible cell with its whole cluster, so a mark attached to the wrong
// base, or dropped, shows up where a base-only comparison would miss it.
Grid snapshot(const Ubiquitty::Terminal& t)
{
    Grid grid;
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Ubiquitty::Cell* cell = UbiquittyTest::cell(t, row, column);
            GridCell entry;
            if (cell) {
                std::uint32_t codePoints[Ubiquitty::clusterPoolMaximumCodePoints + 1];
                const int count = Ubiquitty::cellClusterCodePoints(&t, cell, codePoints, static_cast<int>(std::size(codePoints)));
                entry.cluster.assign(codePoints, codePoints + count);
                entry.attributes = Ubiquitty::cellPen(*cell).attributes.toRaw();
                entry.linkID = cell->linkID();
            }
            grid.cells.push_back(std::move(entry));
        }
    }
    grid.cursorRow = t.screens.active->cursorY;
    grid.cursorColumn = t.screens.active->cursorX;
    grid.wrapPending = t.screens.active->wrapPending;
    return grid;
}

Grid feed(std::string_view bytes, int rows, int columns, int chunkSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);
    for (std::size_t offset = 0; offset < bytes.size(); offset += static_cast<std::size_t>(chunkSize))
        Ubiquitty::processBytes(&t, bytes.substr(offset, static_cast<std::size_t>(chunkSize)));
    return snapshot(t);
}

} // namespace

// The bytes decoded while parsing `text` on a grid `columns` wide.
std::uint64_t decodedBytes(std::string_view text, int columns)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 40, columns);
    const std::uint64_t before = Ubiquitty::Unicode::decodedBytesForTest;
    Ubiquitty::processBytes(&t, text);
    return Ubiquitty::Unicode::decodedBytesForTest - before;
}

TEST(testMarkedProseDecodesNoMoreOnWiderRows)
{
    // Decoding a row's tail again after every mark costs in proportion to the
    // row, so five times the columns would decode several times the bytes. At
    // most a third more means a stop's cost does not follow the width.
    const std::string text = markedProse(11, 6000);
    const std::uint64_t narrow = decodedBytes(text, 40);
    const std::uint64_t wide = decodedBytes(text, 200);
    CHECK_MSG(wide <= narrow + narrow / 3,
        ("decoded " + std::to_string(wide) + " bytes at 200 columns and " + std::to_string(narrow) + " at 40, of a "
            + std::to_string(text.size()) + "-byte input")
            .c_str());
}

TEST(testBatchedTextPlacesTheSameGridAsBytesOneAtATime)
{
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        const std::string text = mixedText(seed, 900);
        for (const int columns : { 9, 40, 200 }) {
            const Grid whole = feed(text, 12, columns, static_cast<int>(text.size()));
            CHECK(whole == feed(text, 12, columns, 1));
            CHECK(whole == feed(text, 12, columns, 7));
        }
    }
}

UBIQUITTY_TEST_MAIN()
