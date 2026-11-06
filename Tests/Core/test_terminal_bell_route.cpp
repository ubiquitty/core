// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_terminal_bell_route.cpp — a bell reaches the terminal that rang it.
//
// Two panes in one process — two sessions in one daemon, two splits in one
// window — must not share one bell: whatever plays it, badges it or carries it
// to a client has to know which pane rang. So a terminal's bell goes to that
// terminal's own effect route, and a BEL on one terminal rings its route and no
// other. Every producer of a bell is driven here: the C0 BEL in the ANSI and
// VT52 grammars, the margin bell and the Tektronix bell.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <memory>
#include <string_view>

using UbiquittyTest::feed;

namespace {

class CountingRoute final : public Ubiquitty::EffectRoute {
public:
    void ringBell() override { ++rings; }
    int rings = 0;
};

struct TwoPanes {
    std::unique_ptr<Ubiquitty::Terminal> left = std::make_unique<Ubiquitty::Terminal>();
    std::unique_ptr<Ubiquitty::Terminal> right = std::make_unique<Ubiquitty::Terminal>();
    CountingRoute leftRoute;
    CountingRoute rightRoute;

    TwoPanes()
    {
        Ubiquitty::resize(*left, 10, 40);
        Ubiquitty::resize(*right, 10, 40);
        left->effectRoute = &leftRoute;
        right->effectRoute = &rightRoute;
    }

    ~TwoPanes()
    {
        left->effectRoute = nullptr;
        right->effectRoute = nullptr;
    }
};

} // namespace

TEST(aBellRingsOnlyTheRouteOfTheTerminalThatRangIt)
{
    TwoPanes panes;
    feed(*panes.left, "\x07");
    CHECK(panes.leftRoute.rings == 1);
    CHECK(panes.rightRoute.rings == 0);
    feed(*panes.right, "text\x07\x07");
    CHECK(panes.leftRoute.rings == 1);
    CHECK(panes.rightRoute.rings == 2);
}

TEST(theVT52BellRingsItsOwnRoute)
{
    TwoPanes panes;
    feed(*panes.left, "\x1b[?2l\x07");
    CHECK(panes.leftRoute.rings == 1);
    CHECK(panes.rightRoute.rings == 0);
}

TEST(theMarginBellRingsItsOwnRoute)
{
    TwoPanes panes;
    feed(*panes.right, "\x1b[?44h\x1b[H");
    panes.right->marginBellColumn = 4;
    panes.right->marginBellArmed = true;
    feed(*panes.right, "abcd");
    CHECK(panes.rightRoute.rings == 1);
    CHECK(panes.leftRoute.rings == 0);
}

TEST(theTektronixBellRingsItsOwnRoute)
{
    TwoPanes panes;
    Ubiquitty::hardReset(*panes.left);
    feed(*panes.left, "\x1b[?38h\x07");
    CHECK(panes.leftRoute.rings == 1);
    CHECK(panes.rightRoute.rings == 0);
}

// RIS wipes the program's state, not the frontend's wiring: a program that
// resets its terminal still rings its own pane's route.
TEST(aHardResetKeepsTheRoute)
{
    TwoPanes panes;
    feed(*panes.left, "\033c\x07");
    CHECK(panes.leftRoute.rings == 1);
}

// A BEL inside an OSC string terminates the string rather than ringing; only a
// bell outside one is a bell.
TEST(aBellThatTerminatesAnOSCStringRingsNothing)
{
    TwoPanes panes;
    feed(*panes.left, "\x1b]2;title\x07");
    CHECK(panes.leftRoute.rings == 0);
}

// The pull state frontends read on their drain is still set: a route is where
// the bell goes as an event, and the frontend's coalescer still decides what
// plays.
TEST(aRoutedBellStillLeavesItsTerminalPending)
{
    TwoPanes panes;
    feed(*panes.left, "\x07");
    CHECK(panes.left->bellPending);
    CHECK(!panes.right->bellPending);
}

// A terminal with no route rings nothing and still records the bell.
TEST(aTerminalWithoutARouteKeepsItsPendingBell)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 10, 40);
    feed(terminal, "\x07");
    CHECK(terminal.bellPending);
}

UBIQUITTY_TEST_MAIN()
