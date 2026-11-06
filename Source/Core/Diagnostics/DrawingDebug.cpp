// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The drawing-debug panel's readout. See DrawingDebug.h.

#include "Core/Diagnostics/DrawingDebug.h"

#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <format>

namespace Ubiquitty::Diagnostics {

void readTerminalFacts(DrawingDebugFacts& facts, const Terminal& terminal)
{
    facts.columns = terminal.columns;
    facts.rows = terminal.rows;
    facts.cursorColumn = terminal.screens.active ? terminal.screens.active->cursorX : 0;
    facts.cursorRow = terminal.screens.active ? terminal.screens.active->cursorY : 0;
    facts.scrollbackLines = Screen::scrollbackCount(&terminal);
    facts.searchMatches = Screen::ScrollSearch::matchCount(terminal);
    facts.bellMode = static_cast<int>(terminal.bellMode);
}

std::string drawingDebugText(const DrawingDebugFacts& facts)
{
    return std::format("grid        {}×{} cols×rows\n"
                       "cursor      ({}, {})\n"
                       "scrollback  {} lines\n"
                       "viewOffset  {}\n"
                       "\n"
                       "font        {} {:.0f}pt\n"
                       "theme       #{}\n"
                       "cell px     {:.2f} × {:.2f}\n"
                       "\n"
                       "atlas       {} / {} slots\n"
                       "cells/frame {}\n"
                       "overlays    {}\n"
                       "search      {} matches\n"
                       "bellMode    {}",
        facts.columns, facts.rows, facts.cursorColumn, facts.cursorRow, facts.scrollbackLines, facts.viewOffset,
        facts.fontName.empty() ? std::string("?") : facts.fontName, facts.fontSizeInPoints, facts.themeIndex,
        facts.cellWidthInPoints, facts.cellHeightInPoints, facts.atlasSlotsUsed, facts.atlasSlotCapacity,
        facts.cellsPerFrame, facts.overlaysPerFrame, facts.searchMatches, facts.bellMode);
}

} // namespace Ubiquitty::Diagnostics
