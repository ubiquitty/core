// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Diagnostics/DrawingDebug.cpp — the readout every
// shell's drawing-debug panel shows.
//
// Pins:
//   - the terminal's half of the facts is read from the terminal: grid,
//     cursor, history, bell mode;
//   - the text names each fact on its own line, in the panel's words.

#include "Core/Diagnostics/DrawingDebug.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <string>

namespace {

using namespace Ubiquitty;
using Ubiquitty::Diagnostics::DrawingDebugFacts;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

TEST(testTheTerminalHalfIsReadFromTheTerminal)
{
    auto terminal = std::make_unique<Terminal>();
    Ubiquitty::resize(*terminal, 4, 12);
    Ubiquitty::processBytes(terminal.get(), "hello\r\nab");
    for (int line = 0; line < 6; ++line)
        Ubiquitty::processBytes(terminal.get(), "\r\nline");

    DrawingDebugFacts facts;
    facts.fontName = "Menlo";
    Diagnostics::readTerminalFacts(facts, *terminal);
    CHECK(facts.columns == 12);
    CHECK(facts.rows == 4);
    CHECK(facts.cursorColumn == 4);
    CHECK(facts.cursorRow == 3);
    CHECK_MSG(facts.scrollbackLines == 4, "the lines scrolled off the top are history");
    CHECK_MSG(facts.fontName == "Menlo", "the renderer's half is left as the caller filled it");
}

TEST(testTheTextNamesEveryFact)
{
    DrawingDebugFacts facts;
    facts.columns = 80;
    facts.rows = 24;
    facts.cursorColumn = 3;
    facts.cursorRow = 7;
    facts.scrollbackLines = 120;
    facts.viewOffset = 2;
    facts.fontName = "SF Mono";
    facts.fontSizeInPoints = 13;
    facts.themeIndex = 4;
    facts.cellWidthInPoints = 7.5;
    facts.cellHeightInPoints = 16.25;
    facts.atlasSlotsUsed = 90;
    facts.atlasSlotCapacity = 1024;
    facts.cellsPerFrame = 1920;
    facts.overlaysPerFrame = 3;
    facts.searchMatches = 5;
    facts.bellMode = 1;
    const std::string text = Diagnostics::drawingDebugText(facts);
    CHECK(contains(text, "grid        80×24 cols×rows\n"));
    CHECK(contains(text, "cursor      (3, 7)\n"));
    CHECK(contains(text, "scrollback  120 lines\n"));
    CHECK(contains(text, "viewOffset  2\n"));
    CHECK(contains(text, "font        SF Mono 13pt\n"));
    CHECK(contains(text, "theme       #4\n"));
    CHECK(contains(text, "cell px     7.50 × 16.25\n"));
    CHECK(contains(text, "atlas       90 / 1024 slots\n"));
    CHECK(contains(text, "cells/frame 1920\n"));
    CHECK(contains(text, "overlays    3\n"));
    CHECK(contains(text, "search      5 matches\n"));
    CHECK(contains(text, "bellMode    1"));

    facts.fontName.clear();
    CHECK_MSG(contains(Diagnostics::drawingDebugText(facts), "font        ? 13pt"), "an unnamed face reads as unknown");
}

} // namespace

UBIQUITTY_TEST_MAIN()
