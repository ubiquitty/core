// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// East Asian Ambiguous width is a choice each terminal makes for itself.
//
// Two windows or two panes with different `[terminal] ambiguous_width` share
// one process, one set of Unicode tables, and nothing else. Each case here runs
// two terminals side by side and interleaves their writes, so a choice held
// anywhere but on the terminal would be the last writer's: it would move the
// other terminal's cursor, cells or ?7700 report, and the case reading them
// fails.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/Protocol/Tmux/Session.h"
#include "Core/Unicode/Width.h"

#include <cstring>
#include <span>
#include <string>

namespace {

using Ubiquitty::Unicode::Width::AmbiguousWidth;

// U+2192 RIGHTWARDS ARROW, East_Asian_Width=Ambiguous, as UTF-8.
constexpr const char* rightwardsArrow = "\xE2\x86\x92";

int cursorColumn(const Ubiquitty::Terminal& t) { return t.screens.active->cursorX; }

bool isWideLeadOf(const Ubiquitty::Terminal& t, int column, std::uint32_t codePoint)
{
    const Ubiquitty::Cell* lead = cell(t, 0, column);
    const Ubiquitty::Cell* continuation = cell(t, 0, column + 1);
    return lead && continuation && lead->codePoint == codePoint
        && Ubiquitty::cellPen(*lead).attributes.contains(Ubiquitty::VT::Attribute::Wide)
        && Ubiquitty::cellPen(*continuation).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
}

bool isNarrowCellOf(const Ubiquitty::Terminal& t, int column, std::uint32_t codePoint)
{
    const Ubiquitty::Cell* narrow = cell(t, 0, column);
    return narrow && narrow->codePoint == codePoint
        && !Ubiquitty::cellPen(*narrow).attributes.contains(Ubiquitty::VT::Attribute::Wide);
}

} // namespace

// One ambiguous code point, written to a narrow terminal and a wide one in
// turn: the narrow terminal places it in one cell, the wide one in a pair, and
// the text after it lands where each terminal's own arithmetic puts it.
TEST(twoTerminalsPlaceAnAmbiguousCodePointUnderTheirOwnChoice)
{
    Ubiquitty::Terminal narrow;
    Ubiquitty::Terminal wide;
    Ubiquitty::hardReset(narrow);
    Ubiquitty::hardReset(wide);
    wide.ambiguousWidth = AmbiguousWidth::Wide;

    feed(wide, rightwardsArrow);
    feed(narrow, rightwardsArrow);
    feed(wide, "x");
    feed(narrow, "x");

    CHECK_EQ(cursorColumn(narrow), 2);
    CHECK_EQ(cursorColumn(wide), 3);
    CHECK_MSG(isNarrowCellOf(narrow, 0, 0x2192), "the narrow terminal holds the arrow in one cell");
    CHECK_MSG(isNarrowCellOf(narrow, 1, 'x'), "and the next character beside it");
    CHECK_MSG(isWideLeadOf(wide, 0, 0x2192), "the wide terminal holds the arrow in a pair");
    CHECK_MSG(isNarrowCellOf(wide, 2, 'x'), "and the next character after the pair");

    // A later write to the narrow terminal leaves the wide one's arithmetic
    // where it was, and the other way round.
    feed(narrow, rightwardsArrow);
    feed(wide, rightwardsArrow);
    CHECK_EQ(cursorColumn(narrow), 3);
    CHECK_EQ(cursorColumn(wide), 5);
}

// A run of ambiguous code points takes the batched text writer rather than
// the per-character one, and the batch measures under the terminal's choice
// too.
TEST(aBatchedRunOfAmbiguousTextMeasuresUnderEachTerminalsChoice)
{
    Ubiquitty::Terminal narrow;
    Ubiquitty::Terminal wide;
    Ubiquitty::hardReset(narrow);
    Ubiquitty::hardReset(wide);
    wide.ambiguousWidth = AmbiguousWidth::Wide;

    // U+00A7 SECTION SIGN, U+2026 HORIZONTAL ELLIPSIS, U+2605 BLACK STAR and
    // U+0410 CYRILLIC CAPITAL A: four ambiguous code points from four blocks.
    const char* run = "\xC2\xA7\xE2\x80\xA6\xE2\x98\x85\xD0\x90"
                      "\xC2\xA7\xE2\x80\xA6\xE2\x98\x85\xD0\x90";
    feed(narrow, run);
    feed(wide, run);

    CHECK_EQ(cursorColumn(narrow), 8);
    CHECK_EQ(cursorColumn(wide), 16);
    CHECK_MSG(isNarrowCellOf(narrow, 3, 0x0410), "the narrow terminal's fourth glyph is in column 3");
    CHECK_MSG(isWideLeadOf(wide, 6, 0x0410), "the wide terminal's fourth glyph leads a pair at column 6");
}

// Moving one terminal's choice is heard by the program in that terminal and
// by no other: each terminal announces its own choice under ?7700.
TEST(aChoiceMovedOnOneTerminalIsReportedByThatTerminalAlone)
{
    Ubiquitty::Terminal still;
    Ubiquitty::Terminal moved;
    char outputBuffer[64];
    Ubiquitty::hardReset(still);
    Ubiquitty::hardReset(moved);
    feed(still, "\033[?7700h");
    feed(moved, "\033[?7700h");
    feed(still, "a");
    feed(moved, "a");
    drain(still, outputBuffer);
    CHECK(strcmp(outputBuffer, "") == 0);
    drain(moved, outputBuffer);
    CHECK(strcmp(outputBuffer, "") == 0);

    moved.ambiguousWidth = AmbiguousWidth::Wide;
    feed(moved, "b");
    feed(still, "b");
    drain(moved, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[2W") == 0);
    drain(still, outputBuffer);
    CHECK_MSG(strcmp(outputBuffer, "") == 0, "the terminal whose choice did not move reports nothing");
}

// A tmux -CC pane's terminal is minted by the session, which reads no config,
// so the choice the session was handed is the only route to a pane that
// measures ambiguous text wide.
TEST(aTmuxPaneIsBornWithTheSessionsAmbiguousWidth)
{
    const std::string arrowThenX = std::string(rightwardsArrow) + "x";
    const std::span<const unsigned char> bytes { reinterpret_cast<const unsigned char*>(arrowThenX.data()), arrowThenX.size() };

    Ubiquitty::Protocol::Tmux::Session wide;
    wide.setPaneAmbiguousWidth(AmbiguousWidth::Wide);
    wide.applyLayout(0, "ef01,80x24,0,0,7");
    Ubiquitty::Protocol::Tmux::Pane* widePane = wide.pane(7);
    CHECK(widePane != nullptr && widePane->term() != nullptr);
    if (!widePane || !widePane->term())
        return;
    wide.deliverOutput(7, bytes);
    CHECK_EQ(cursorColumn(*widePane->term()), 3);
    CHECK_MSG(isWideLeadOf(*widePane->term(), 0, 0x2192), "the pane born under the wide choice holds the arrow in a pair");

    // A session handed no choice mints a pane that measures it narrow.
    Ubiquitty::Protocol::Tmux::Session narrow;
    narrow.applyLayout(0, "ef01,80x24,0,0,3");
    Ubiquitty::Protocol::Tmux::Pane* narrowPane = narrow.pane(3);
    CHECK(narrowPane != nullptr && narrowPane->term() != nullptr);
    if (!narrowPane || !narrowPane->term())
        return;
    narrow.deliverOutput(3, bytes);
    CHECK_EQ(cursorColumn(*narrowPane->term()), 2);
    CHECK_MSG(isNarrowCellOf(*narrowPane->term(), 0, 0x2192), "the pane born under the default holds it in one cell");
}

UBIQUITTY_TEST_MAIN()
