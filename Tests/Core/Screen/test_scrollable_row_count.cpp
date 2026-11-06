// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_scrollable_row_count.cpp — how far the view may scroll, which is not how
// many rows the buffer retains.
//
// Spelled as one name, the two questions are told apart only by whether the
// caller writes `isUsingAlternateScreen ? 0 :` in front of it, and a caller that
// forgets lets a selection drag on the alternate screen scroll into the
// primary's history — the grid sliding under a selection anchored to a screen
// that has no history of its own. These cases pin the two answers apart so a
// caller has to choose a name that says which it is asking.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <string>

using UbiquittyTest::feed;

namespace {

// A terminal with real primary scrollback: the condition the reported bug
// reproduces under, since with an empty primary both answers agree at 0 and
// nothing moves.
std::unique_ptr<Ubiquitty::Terminal> withPrimaryScrollback(int rows, int columns, int extraLines)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    for (int i = 0; i < rows + extraLines; ++i)
        feed(*t, "line\r\n");
    return t;
}

} // namespace

TEST(testTheAlternateScreenCanScrollNowhere)
{
    auto t = withPrimaryScrollback(24, 80, 40);
    const int retained = Ubiquitty::Screen::scrollbackCount(t.get());
    CHECK(retained > 0); // the fixture is not vacuous

    // On the primary the two answers agree: the limit is the retained count.
    CHECK(Ubiquitty::Screen::scrollableRowCount(t.get()) == retained);

    feed(*t, "\033[?1049h"); // enter the alternate screen

    // The alternate screen keeps no history, so no offset into one is reachable.
    CHECK(Ubiquitty::Screen::scrollableRowCount(t.get()) == 0);

    // …and the STORAGE answer is unchanged, because the rows are still retained.
    // Zeroing this instead would stop the sync frame transmitting the primary's
    // history and misplace every search match, which is why the two are separate.
    CHECK(Ubiquitty::Screen::scrollbackCount(t.get()) == retained);

    feed(*t, "\033[?1049l"); // and back
    CHECK(Ubiquitty::Screen::scrollableRowCount(t.get()) == retained);
}

TEST(testTheLimitClampsAViewOffsetToNothingOnTheAlternateScreen)
{
    auto t = withPrimaryScrollback(24, 80, 40);
    const int retained = Ubiquitty::Screen::scrollbackCount(t.get());

    // The clamp the drag path applies. On the primary a scroll-up request is
    // honoured up to the retained depth.
    CHECK(Ubiquitty::Screen::clampViewOffset(10, Ubiquitty::Screen::scrollableRowCount(t.get())) == 10);
    CHECK(Ubiquitty::Screen::clampViewOffset(retained + 100, Ubiquitty::Screen::scrollableRowCount(t.get())) == retained);

    feed(*t, "\033[?1049h");

    // On the alternate screen every request clamps to zero — the reported bug is
    // this clamp being handed the retained count instead of the limit, which let
    // the view slide into history the screen does not have.
    CHECK(Ubiquitty::Screen::clampViewOffset(10, Ubiquitty::Screen::scrollableRowCount(t.get())) == 0);
    CHECK(Ubiquitty::Screen::clampViewOffset(retained, Ubiquitty::Screen::scrollableRowCount(t.get())) == 0);
}

TEST(testAnEmptyPrimaryHidesTheDefect)
{
    // Why the report says "reproduces if there was scrollback in the main
    // screen": with none, the two answers agree at 0 and the wrong one is
    // indistinguishable from the right one. Pinned so a future fixture that
    // forgets the scrollback does not look like it is testing this.
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    feed(*t, "\033[?1049h");
    CHECK(Ubiquitty::Screen::scrollbackCount(t.get()) == 0);
    CHECK(Ubiquitty::Screen::scrollableRowCount(t.get()) == 0);
}

TEST(testANullTerminalScrollsNowhere)
{
    CHECK(Ubiquitty::Screen::scrollableRowCount(nullptr) == 0);
}

UBIQUITTY_TEST_MAIN()
