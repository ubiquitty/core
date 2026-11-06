// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SearchFixture.h"

// test_overlay_span_gather.cpp — the terminal-state → visible-row span gathers
// (Core/Rendering/OverlaySpanGather.cpp). Each gather maps one highlight tier
// through the absolute-to-visible conversion: the focused search match, every
// other match, the trigger-highlight ring, and the DEC 1001 hilite band. Every
// frontend maps its highlights through these gathers, so this file is where the
// mapping is tested.

#include "Core/Rendering/OverlaySpanGather.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Triggers.h"
#include "TerminalFixture.h"

#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;
using UbiquittyTest::feed;
namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;

TEST(testSearchSpansSplitFocusedFromOthers)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "alpha beta alpha\r\ngamma\r\nalpha");

    UbiquittyTest::SearchFixture search(t);
    CHECK_EQ(search.find("alpha"), 3);

    // Which match gains focus first is the controller's policy;
    // asserted here are the relations the two gathers guarantee together.
    std::vector<SelectionRowSpan> focused;
    collectSearchMatchRowSpans(t, 0, focused);
    CHECK_EQ(focused.size(), 1u);
    CHECK(focused[0].visibleRow >= 0 && focused[0].visibleRow < t.rows);
    CHECK(focused[0].column1 >= focused[0].column0);

    // The complement: every segment that is not the focused match, keyed on
    // match ID so nothing lands in both lists.
    std::vector<SelectionRowSpan> others;
    collectOtherSearchMatchRowSpans(t, 0, others);
    CHECK_EQ(static_cast<int>(focused.size() + others.size()), ScrollSearch::segmentCount(t));
    CHECK_EQ(others.size(), 2u);
    for (const SelectionRowSpan& span : others)
        CHECK(!(span.visibleRow == focused[0].visibleRow && span.column0 == focused[0].column0));

    // Stepping the active match moves the emphasis, not the total.
    const SelectionRowSpan before = focused[0];
    CHECK(search.step(Search::StepDirection::Older));
    collectSearchMatchRowSpans(t, 0, focused);
    collectOtherSearchMatchRowSpans(t, 0, others);
    CHECK_EQ(focused.size(), 1u);
    CHECK(!(focused[0].visibleRow == before.visibleRow && focused[0].column0 == before.column0));
    CHECK_EQ(others.size(), 2u);
}

TEST(testSearchSpansEmptyWithoutASearch)
{
    Terminal t;
    hardReset(t);
    feed(t, "text with no search");

    std::vector<SelectionRowSpan> spans { { 1, 2, 3 } }; // pre-filled: must be cleared
    collectSearchMatchRowSpans(t, 0, spans);
    CHECK(spans.empty());
    spans = { { 1, 2, 3 } };
    collectOtherSearchMatchRowSpans(t, 0, spans);
    CHECK(spans.empty());
}

// A match's rows count after the primary screen's history on either screen,
// as the gathers convert them, and the alternate screen keeps no history of
// its own: a match there lands on its own rows, across a soft wrap too,
// however much history the shell behind the program kept.
TEST(aFocusedMatchOnTheAlternateScreenLandsOnItsOwnRows)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    for (int line = 0; line < 20; ++line)
        feed(t, "shell line " + std::to_string(line) + "\r\n");
    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    REQUIRE(history > t.rows);
    feed(t, "\033[?1049h\033[4;27Hneedle"); // row 3 from column 26, wrapping onto row 4

    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("needle") == 1);
    const ScrollSearch::Match* active = ScrollSearch::activeMatch(t);
    REQUIRE(active != nullptr);
    CHECK_EQ(active->absoluteRow, history + 3);

    std::vector<SelectionRowSpan> spans;
    collectSearchMatchRowSpans(t, 0, spans);
    REQUIRE(spans.size() == 2u);
    CHECK_EQ(spans[0].visibleRow, 3);
    CHECK_EQ(spans[0].column0, 26);
    CHECK_EQ(spans[0].column1, 29);
    CHECK_EQ(spans[1].visibleRow, 4);
    CHECK_EQ(spans[1].column0, 0);
    CHECK_EQ(spans[1].column1, 1);
}

TEST(theGatherResolvesTheViewOffsetTheWayAddressingDoes)
{
    // visibleCell forces the view offset to zero on the alternate screen: an
    // application drawing there has the whole screen, and nothing from the
    // scrollback is on it. The gather takes the frontend's stored offset,
    // which is not reset when the alternate screen comes up — so the same
    // (terminal, offset) pair that addresses no scrollback row can still map
    // a highlight from history into the viewport, and the frame paints a
    // quad over the application's own output.
    Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    CHECK(Triggers::addRuleSpec(&t, "ERROR", "highlight:red", CaseSensitivity::Sensitive));
    feed(t, "an ERROR appeared\r\n");
    for (int i = 0; i < 10; ++i)
        feed(t, "filler\r\n");
    CHECK(Triggers::highlightCount(&t) > 0);
    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(history > 0);

    // The offset the user left behind when the application took the screen.
    const int staleOffset = history;
    t.screens.activate(Ubiquitty::Modes::ScreenBuffer::Alternate);
    CHECK(t.screens.isUsingAlternateScreen);

    std::vector<SelectionRowSpan> spans;
    collectTriggerHighlightRowSpans(t, staleOffset, spans);
    CHECK_MSG(spans.empty(), "a highlight from the scrollback landed on the alternate screen");

    // The control: on the primary screen the same offset is a real scroll
    // position, and the same highlight maps into the viewport as before.
    t.screens.activate(Ubiquitty::Modes::ScreenBuffer::Primary);
    collectTriggerHighlightRowSpans(t, staleOffset, spans);
    CHECK_EQ(spans.size(), 1u);
}

TEST(testTriggerHighlightSpans)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 40);

    // No highlight fired: cleared and empty.
    std::vector<SelectionRowSpan> spans { { 1, 2, 3 } };
    collectTriggerHighlightRowSpans(t, 0, spans);
    CHECK(spans.empty());

    // A highlight rule fires on output and lands in the ring; the gather maps
    // its absolute row into the viewport.
    CHECK(Triggers::addRuleSpec(&t, "ERROR", "highlight:red", CaseSensitivity::Sensitive));
    feed(t, "an ERROR appeared\r\n");
    CHECK(Triggers::highlightCount(&t) > 0);

    collectTriggerHighlightRowSpans(t, 0, spans);
    CHECK_EQ(spans.size(), 1u);
    CHECK_EQ(spans[0].visibleRow, 0);
    CHECK_EQ(spans[0].column0, 3); // "an " precedes the match
    CHECK(spans[0].column1 > spans[0].column0);
}

TEST(testHiliteRegionSpans)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);

    // No band set: cleared and empty.
    std::vector<SelectionRowSpan> spans { { 1, 2, 3 } };
    collectHiliteRegionRowSpans(t, 0, spans);
    CHECK(spans.empty());

    // The four scalars resolve through the shared geometry into one span per
    // covered row.
    t.modes.hiliteRegionValid = true;
    t.modes.hiliteStartX = 2;
    t.modes.hiliteStartY = 1;
    t.modes.hiliteFirstRow = 0;
    t.modes.hiliteLastRow = 3;
    collectHiliteRegionRowSpans(t, 0, spans);
    CHECK(!spans.empty());
    for (const SelectionRowSpan& span : spans) {
        CHECK(span.visibleRow >= 0 && span.visibleRow < t.rows);
        CHECK(span.column0 >= 0 && span.column1 < t.columns);
    }
}

UBIQUITTY_TEST_MAIN()
