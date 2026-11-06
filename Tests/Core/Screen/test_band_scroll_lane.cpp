// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// The band lane defers DECSLRM band scrolls; the classic rectangle path is the
// oracle. Every case feeds two terminals the same bytes -- one with the lane,
// one without -- and requires byte-identical grids, wrap flags and cursors.
// The lane is a pure reordering of the same moves, so any divergence is a
// defect by construction, whatever the content.

#include "Harness.h"

#include <cstring>
#include <memory>
#include <string>

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

namespace {

using namespace Ubiquitty;

std::unique_ptr<Terminal> makeTerminal(int rows, int columns, bool bandLane)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    t->isScrollLaneEnabled = false; // isolate: only the band lane differs
    t->isBandScrollLaneEnabled = bandLane;
    return t;
}

void feedBoth(Terminal& a, Terminal& b, const std::string& bytes)
{
    Ubiquitty::processBytes(&a, bytes);
    Ubiquitty::processBytes(&b, bytes);
}

void feedBothBytewise(Terminal& a, Terminal& b, const std::string& bytes)
{
    for (char byte : bytes) {
        Ubiquitty::processBytes(&a, std::string_view(&byte, 1));
        Ubiquitty::processBytes(&b, std::string_view(&byte, 1));
    }
}

[[nodiscard]] bool gridsMatch(Terminal& a, Terminal& b)
{
    if (a.rows != b.rows || a.columns != b.columns)
        return false;
    for (int row = 0; row < a.rows; ++row) {
        const Cell* rowA = a.grid->cellAt(Row(row), Column(0));
        const Cell* rowB = b.grid->cellAt(Row(row), Column(0));
        if (!rowA || !rowB)
            return false;
        if (std::memcmp(rowA, rowB, static_cast<std::size_t>(a.columns) * sizeof(Cell)) != 0)
            return false;
    }
    const Screen::State* sa = a.screens.active;
    const Screen::State* sb = b.screens.active;
    if (sa->cursorX != sb->cursorX || sa->cursorY != sb->cursorY)
        return false;
    for (int row = 0; row < a.rows; ++row) {
        if (sa->lineWrapped[row] != sb->lineWrapped[row])
            return false;
    }
    return true;
}

// The tmux shape: margins on, a band armed, the cursor parked at the band's
// bottom, and lines arriving as text CR LF.
std::string bandSetup(int bottomRow, int left1, int right1)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "\033[?69h\033[%d;%ds\033[1;%dr\033[%d;%dH", left1, right1, bottomRow, bottomRow, left1);
    return buffer;
}

std::string burst(int lines, int width, const char* tag)
{
    std::string out;
    for (int line = 0; line < lines; ++line) {
        char body[64];
        std::snprintf(body, sizeof(body), "%03d %s", line, tag);
        std::string text(body);
        if (static_cast<int>(text.size()) > width)
            text.resize(static_cast<std::size_t>(width));
        out += text;
        out += "\r\n";
    }
    return out;
}

TEST(testABandBurstMatchesTheClassicPath)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    // Neighbour text that must not move: the right pane and the divider.
    const std::string neighbour = "\033[1;21H\xE2\x94\x82 right pane\033[2;21H\xE2\x94\x82 stays put";
    feedBoth(*lane, *classic, neighbour + bandSetup(7, 1, 20) + burst(40, 20, "pane"));
    CHECK_MSG(gridsMatch(*lane, *classic), "a 40-line band burst (through pool overflow) matches the classic rectangle path");
    lane.reset();
    classic.reset();
}

TEST(testPartialWidthLinesBlankTheTail)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    feedBoth(*lane, *classic, bandSetup(7, 1, 20) + burst(12, 7, "x"));
    CHECK_MSG(gridsMatch(*lane, *classic), "short lines leave the band's tail blank exactly as the classic path does");
    lane.reset();
    classic.reset();
}

TEST(testAnSGRMidBurstKeepsEquivalence)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    std::string bytes = bandSetup(7, 1, 20);
    bytes += burst(5, 20, "a");
    bytes += "\033[33m"; // fg only: blank-invariant, the run may survive
    bytes += burst(5, 20, "b");
    bytes += "\033[41m"; // bg change: the BCE blank changes, the run must flush
    bytes += burst(5, 20, "c");
    bytes += "\033[0m";
    feedBoth(*lane, *classic, bytes);
    CHECK_MSG(gridsMatch(*lane, *classic), "fg and bg SGRs mid-burst leave both paths identical");
    lane.reset();
    classic.reset();
}

TEST(testACupMidBurstFlushesAndMatches)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    std::string bytes = bandSetup(7, 1, 20) + burst(6, 20, "a");
    bytes += "\033[2;3Hpeek"; // a dispatch that leaves the pattern
    bytes += bandSetup(7, 1, 20) + burst(6, 20, "b");
    feedBoth(*lane, *classic, bytes);
    CHECK_MSG(gridsMatch(*lane, *classic), "a cursor move mid-burst flushes the run and the grids agree");
    lane.reset();
    classic.reset();
}

TEST(testTheAlternateScreenIsEligibleAndEquivalent)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    feedBoth(*lane, *classic, "\033[?1049h" + bandSetup(7, 1, 20) + burst(20, 20, "alt") + "\033[?1049l");
    CHECK_MSG(gridsMatch(*lane, *classic), "an alt-screen band burst matches, and leaving the screen restores identically");
    lane.reset();
    classic.reset();
}

TEST(testBytewiseChunkingChangesNothing)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    feedBothBytewise(*lane, *classic, bandSetup(7, 1, 20) + burst(8, 20, "byte"));
    CHECK_MSG(gridsMatch(*lane, *classic), "one-byte feeds (every chunk boundary flushes) still match");
    lane.reset();
    classic.reset();
}

TEST(testTheSecondPaneBandMatchesToo)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    std::string bytes = bandSetup(7, 1, 20) + burst(9, 20, "L");
    bytes += bandSetup(7, 22, 40) + burst(9, 19, "R"); // re-arm to the right pane: geometry change flushes
    bytes += bandSetup(7, 1, 20) + burst(9, 20, "L2");
    feedBoth(*lane, *classic, bytes);
    CHECK_MSG(gridsMatch(*lane, *classic), "alternating pane bands (tmux's burst switching) match the classic path");
    lane.reset();
    classic.reset();
}

// A run left open across a processBytes return -- which the main path allows
// by design -- must survive whatever comes next from outside the byte stream.
// A resize is the sharpest such visitor: it can reallocate the storage the
// stash points into. The runtime flushes before resizing; this drives the
// same order and requires the classic grids to agree afterwards.
TEST(testAResizeAfterAnOpenRunMatchesTheClassicPath)
{
    auto lane = makeTerminal(8, 40, true);
    auto classic = makeTerminal(8, 40, false);
    // A burst with no trailing dispatch: the run is open when feed returns.
    feedBoth(*lane, *classic, bandSetup(7, 1, 20) + burst(5, 20, "live"));
    // What the runtime does before any resize reaches the grid.
    Ubiquitty::termFlushScrollLane(*lane);
    Ubiquitty::termFlushScrollLane(*classic);
    Ubiquitty::resize(*lane, 10, 44);
    Ubiquitty::resize(*classic, 10, 44);
    CHECK_MSG(gridsMatch(*lane, *classic), "an open run flushed at the resize boundary leaves both paths identical");
    // And the band still works at the new geometry.
    feedBoth(*lane, *classic, bandSetup(9, 1, 22) + burst(12, 22, "after"));
    CHECK_MSG(gridsMatch(*lane, *classic), "a fresh burst after the resize matches too");
    lane.reset();
    classic.reset();
}

} // namespace

UBIQUITTY_TEST_MAIN()
