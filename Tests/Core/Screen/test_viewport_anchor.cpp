// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Addressing.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <string>

namespace {

void fillUnevenHistory(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::resize(terminal, 4, 12);
    for (int line = 0; line < 40; ++line)
        UbiquittyTest::feed(terminal, std::to_string(line) + ":" + std::string(1 + line % 7 * 11, 'x') + "\r\n");
}

} // namespace

TEST(aLocalViewportKeepsItsLogicalCellThroughUnevenRewraps)
{
    Ubiquitty::Terminal terminal;
    fillUnevenHistory(terminal);
    Ubiquitty::Selection::Session selection;
    int offset = Ubiquitty::Screen::scrollbackCount(&terminal) - 5;
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, offset);
    REQUIRE(selection.viewportAnchor);
    const auto original = *selection.viewportAnchor;
    for (const int columns : { 7, 23, 5, 12 }) {
        Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, offset);
        Ubiquitty::resize(terminal, 4, columns);
        offset = Ubiquitty::Selection::viewportAfterResize(selection, terminal, offset);
        const auto place = Ubiquitty::Screen::resolvePosition(terminal, original);
        REQUIRE(place);
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&terminal) - offset, place->absoluteRow);
        REQUIRE(selection.viewportAnchor);
        CHECK(*selection.viewportAnchor == original);
        CHECK(!selection.usesLogicalViewport);
    }
}

TEST(explicitViewportMovementReplacesTheResizeAnchor)
{
    Ubiquitty::Terminal terminal;
    fillUnevenHistory(terminal);
    Ubiquitty::Selection::Session selection;
    int offset = Ubiquitty::Screen::scrollbackCount(&terminal) - 5;
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, offset);
    REQUIRE(selection.viewportAnchor);
    const auto original = *selection.viewportAnchor;
    Ubiquitty::resize(terminal, 4, 7);
    offset = Ubiquitty::Selection::viewportAfterResize(selection, terminal, offset);
    offset /= 2;
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, offset);
    REQUIRE(selection.viewportAnchor);
    const auto moved = *selection.viewportAnchor;
    CHECK(moved != original);
    Ubiquitty::resize(terminal, 4, 12);
    offset = Ubiquitty::Selection::viewportAfterResize(selection, terminal, offset);
    const auto place = Ubiquitty::Screen::resolvePosition(terminal, moved);
    REQUIRE(place);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&terminal) - offset, place->absoluteRow);
}

TEST(aResizeLeavesTheTailAndPulledBackTextAtTheTail)
{
    Ubiquitty::Terminal terminal;
    fillUnevenHistory(terminal);
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, 0);
    Ubiquitty::resize(terminal, 5, 9);
    CHECK_EQ(Ubiquitty::Selection::viewportAfterResize(selection, terminal, 0), 0);
    CHECK(!selection.viewportAnchor);
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, 1);
    REQUIRE(selection.viewportAnchor);
    Ubiquitty::resize(terminal, 10, 9);
    CHECK_EQ(Ubiquitty::Selection::viewportAfterResize(selection, terminal, 1), 0);
}

TEST(anEvictedResizeAnchorClampsToTheOldestSurvivor)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::setScrollbackLimit(&terminal, 10);
    Ubiquitty::resize(terminal, 3, 12);
    for (int line = 0; line < 15; ++line)
        UbiquittyTest::feed(terminal, std::to_string(line) + "\r\n");
    Ubiquitty::Selection::Session selection;
    const int offset = Ubiquitty::Screen::scrollbackCount(&terminal);
    Ubiquitty::Selection::noteViewportBeforeResize(selection, terminal, offset);
    REQUIRE(selection.viewportAnchor);
    for (int line = 0; line < 20; ++line)
        UbiquittyTest::feed(terminal, "replacement\r\n");
    Ubiquitty::resize(terminal, 4, 9);
    CHECK_EQ(Ubiquitty::Selection::viewportAfterResize(selection, terminal, offset), Ubiquitty::Screen::scrollbackCount(&terminal));
}

UBIQUITTY_TEST_MAIN()
