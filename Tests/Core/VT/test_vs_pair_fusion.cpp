// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// The fused base+VS emit closes its cluster when the decode batch proves no
// extender follows. These cases pin what that must not change: a keycap or
// ZWJ arriving next -- in the same chunk or split across a byte-wise feed --
// still extends the pair, and the pair cell itself is identical either way.

#include "Harness.h"

#include <memory>
#include <string>

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

namespace {

using namespace Ubiquitty;

std::unique_ptr<Terminal> makeTerminal()
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, 6, 40);
    return t;
}

void feed(Terminal& t, const std::string& bytes) { Ubiquitty::processBytes(&t, bytes); }

void feedBytewise(Terminal& t, const std::string& bytes)
{
    for (char byte : bytes)
        Ubiquitty::processBytes(&t, std::string_view(&byte, 1));
}

const Cell* cell(Terminal& t, int row, int column) { return t.grid->cellAt(Row(row), Column(column)); }

// A pair followed by a plain base: the pair keeps its fused shape -- wide,
// emoji, the selector inline -- and the base lands two columns on as its own
// narrow cell.
TEST(testAPairThenABaseKeepsBothShapes)
{
    auto t = makeTerminal();
    feed(*t, "\xE2\x98\x80\xEF\xB8\x8F"
             "A"); // U+2600 + VS16, then 'A'
    const Cell* lead = cell(*t, 0, 0);
    CHECK(lead && lead->codePoint == 0x2600);
    CHECK_MSG(cellPen(*lead).attributes.contains(VT::Attribute::Wide), "VS16 upgraded the base to a wide emoji cell");
    CHECK_MSG(lead->combiningLength() == 1 && lead->combining[0] == 0xFE0F, "the selector rides the lead cell");
    const Cell* after = cell(*t, 0, 2);
    CHECK_MSG(after && after->codePoint == 'A', "the following base is its own cell past the continuation");
    t.reset();
}

// Two pairs back to back: the second pair must be as fused as the first --
// the early close must not leak state into the next emit.
TEST(testTwoPairsBackToBackBothFuse)
{
    auto t = makeTerminal();
    feed(*t, "\xE2\x98\x80\xEF\xB8\x8F\xE2\x98\x81\xEF\xB8\x8F"); // sun+VS16, cloud+VS16
    const Cell* first = cell(*t, 0, 0);
    const Cell* second = cell(*t, 0, 2);
    CHECK(first && first->codePoint == 0x2600 && cellPen(*first).attributes.contains(VT::Attribute::Wide));
    CHECK(second && second->codePoint == 0x2601 && cellPen(*second).attributes.contains(VT::Attribute::Wide));
    CHECK(second->combiningLength() == 1 && second->combining[0] == 0xFE0F);
    t.reset();
}

// A keycap sequence -- base, VS16, U+20E3 -- in one chunk: the enclosing
// keycap must still extend the fused pair into one wide emoji cluster.
TEST(testAKeycapStillExtendsTheFusedPair)
{
    auto t = makeTerminal();
    feed(*t, "1\xEF\xB8\x8F\xE2\x83\xA3"); // '1' + VS16 + COMBINING ENCLOSING KEYCAP
    const Cell* lead = cell(*t, 0, 0);
    CHECK(lead && lead->codePoint == '1');
    CHECK_MSG(cellPen(*lead).attributes.contains(VT::Attribute::Wide), "the completed keycap is wide");
    CHECK_MSG(cellPen(*lead).attributes.contains(VT::Attribute::Emoji), "and emoji-presented");
    t.reset();
}

// The same keycap fed one byte at a time: every code point arrives in its own
// chunk, the batch never sees a successor, and the boundary rule keeps the
// cluster open so the keycap still attaches.
TEST(testAKeycapSplitAcrossFeedsStillExtends)
{
    auto whole = makeTerminal();
    auto split = makeTerminal();
    const std::string bytes = "1\xEF\xB8\x8F\xE2\x83\xA3";
    feed(*whole, bytes);
    feedBytewise(*split, bytes);
    for (int column = 0; column < 4; ++column) {
        const Cell* a = cell(*whole, 0, column);
        const Cell* b = cell(*split, 0, column);
        CHECK_MSG(a && b && a->codePoint == b->codePoint
                && cellPen(*a).attributes == cellPen(*b).attributes,
            "byte-split and whole-chunk keycaps produce identical cells");
    }
    whole.reset();
    split.reset();
}

// A ZWJ continuation after the pair, split across feeds: the pair's cluster
// must still be live when the ZWJ arrives in its own chunk.
TEST(testAZwjAfterThePairSplitAcrossFeedsStillJoins)
{
    auto whole = makeTerminal();
    auto split = makeTerminal();
    // U+2764 U+FE0F U+200D U+1F525: heart + VS16, ZWJ, fire.
    const std::string bytes = "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5";
    feed(*whole, bytes);
    feedBytewise(*split, bytes);
    for (int column = 0; column < 4; ++column) {
        const Cell* a = cell(*whole, 0, column);
        const Cell* b = cell(*split, 0, column);
        CHECK_MSG(a && b && a->codePoint == b->codePoint
                && a->combiningLength() == b->combiningLength(),
            "byte-split and whole-chunk ZWJ sequences produce identical cells");
    }
    whole.reset();
    split.reset();
}

} // namespace

UBIQUITTY_TEST_MAIN()
