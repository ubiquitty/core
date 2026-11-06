// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/ClearBuffer.h"

#include "Core/Screen/Margins.h"
#include "Core/Screen/Marks.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/Selection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>

namespace Ubiquitty::Screen {

namespace {
// Ctrl-L: clear-screen at a shell, redraw in a TUI. The byte a frontend forwards
// so the foreground process re-syncs its prompt row.
constexpr std::array<char, 1> redrawRequest { '\x0c' };

// Everything a row-dropping command invalidates.
void dropStaleState(Terminal* t, SelectionState* selection, const ResetViewOffset& resetViewOffset)
{
    if (selection)
        Selection::clear(selection);
    if (resetViewOffset)
        resetViewOffset();
    // A mirrored grid is the far end's difference target, so dropping rows makes
    // every later frame a difference against a screen this one no longer holds.
    // Those frames still arrive in sequence, leaving nothing downstream able to
    // detect it.
    if (t && t->mirrorClient)
        t->mirrorClient->invalidateBaseline();
}

// A program has told us it owns the screen's layout, so the terminal cannot
// tell which rows are finished output and which are still being drawn. The
// alternate screen says so outright; a scroll region or left/right margins say
// it just as plainly, because the rows outside them are chrome the program
// painted once and will not repaint on its own.
bool programOwnsLayout(const Terminal& t)
{
    const Ubiquitty::Screen::State* s = t.screens.active;
    if (!s)
        return true; // nothing to reason about, so reason about nothing
    return t.screens.isUsingAlternateScreen || s->scrollTop != 0 || s->scrollBottom != t.rows - 1
        || Ubiquitty::Screen::isRowNarrowedByMargins(t, s);
}

// Screen-relative row the clear lifts to the top.
int boundaryRow(const Terminal& t, ClearBoundary boundary)
{
    const Ubiquitty::Screen::State* s = t.screens.active;
    int row = s->cursorY;
    // pendingPromptRow is absolute — history plus a screen row — so the screen
    // starts where history ends. It is live only between OSC 133 A and C, which
    // is exactly the window where "the prompt" is a thing that exists; during a
    // command it reads -1 and the cursor row is the boundary, as it is for a
    // shell with no integration at all.
    if (boundary == ClearBoundary::CurrentPrompt && t.pendingPromptRow >= 0) {
        const int promptRow = t.pendingPromptRow - scrollbackCount(&t);
        // A prompt that has scrolled above the viewport describes no row the
        // user can see; the cursor row is the boundary that still means something.
        if (promptRow >= 0)
            row = promptRow;
    }
    if (row < 0)
        return 0;
    if (row > t.rows - 1)
        return t.rows - 1;
    return row;
}
}

ClearOutcome clearScreen(Terminal* t, ClearBoundary boundary, SelectionState* selection,
    const ResetViewOffset& resetViewOffset, const SendToProcess& sendToProcess)
{
    if (!t || !t->screens.active || t->rows <= 0)
        return ClearOutcome::NoOp;

    if (programOwnsLayout(*t)) {
        if (sendToProcess)
            sendToProcess(redrawRequest);
        return ClearOutcome::HandedOff;
    }

    // Also the number of rows to scroll: lifting row N to the top moves N rows
    // off it. Named for the row so it does not read as t->rows.
    const int topRow = boundaryRow(*t, boundary);
    if (topRow <= 0)
        return ClearOutcome::NoOp;

    // The same call an overflowing line of output makes, so the rows land in
    // scrollback with their marks, images and line attributes intact.
    Ubiquitty::Screen::scrollViewportUp(t, topRow);
    // scrollRegionUp rotates the grid and leaves the cursor index alone; the
    // content under it moved, so it follows. pendingPromptRow needs no such fix:
    // it is absolute, and history grew by exactly what the screen row lost.
    t->screens.active->cursorY -= topRow;
    if (t->screens.active->cursorY < 0)
        t->screens.active->cursorY = 0;

    // dropStaleState is for rows that went: its baseline reset exists because a
    // mirror cannot difference against a screen the near end no longer holds.
    // A scroll leaves every row somewhere, so the far end tracks it as one more
    // frame the way it tracks a line of output. The selection goes because a
    // highlight the user can no longer see surviving a "clear" is surprising,
    // not because it became invalid.
    if (selection)
        Selection::clear(selection);
    if (resetViewOffset)
        resetViewOffset();
    return ClearOutcome::Scrolled;
}

void clearBuffer(Terminal* t, SelectionState* selection, const ResetViewOffset& resetViewOffset, const SendToProcess& sendToProcess)
{
    if (!t)
        return;
    if (programOwnsLayout(*t)) {
        if (sendToProcess)
            sendToProcess(redrawRequest);
        return;
    }

    clearBufferPreserveInput(t);
    dropStaleState(t, selection, resetViewOffset);
    if (sendToProcess)
        sendToProcess(redrawRequest);
}

bool runClearCommand(ClearCommand command, Terminal* t, SelectionState* selection, const SendToProcess& sendToProcess)
{
    if (!t)
        return false;
    // Set by the commands that moved the view's ground out from under it. The
    // viewport itself is the frontend's — it is a property of looking, not of
    // the buffer — so this reports rather than performs.
    bool returnsViewToTail = false;
    const ResetViewOffset resetViewOffset = [&returnsViewToTail] { returnsViewToTail = true; };
    switch (command) {
    case ClearCommand::Buffer:
        clearBuffer(t, selection, resetViewOffset, sendToProcess);
        break;
    case ClearCommand::Scrollback:
        clearScrollback(t, selection, resetViewOffset);
        break;
    case ClearCommand::Screen:
        (void)clearScreen(t, ClearBoundary::CurrentPrompt, selection, resetViewOffset, sendToProcess);
        break;
    case ClearCommand::ToPreviousMark:
        (void)clearToPreviousMark(t, selection, resetViewOffset, sendToProcess);
        break;
    }
    return returnsViewToTail;
}

void clearScrollback(Terminal* t, SelectionState* selection, const ResetViewOffset& resetViewOffset)
{
    if (!t)
        return;

    dropScrollback(t);
    // The user asked for a clean screen, not only a clean history: a
    // double-height banner surviving here renders the next application's top
    // rows at twice the size. Line attributes are live-row state though, so a
    // program that owns the screen keeps the ones it drew — dropping history
    // never needed the screen's cooperation, and this is the one step that does.
    if (!programOwnsLayout(*t))
        resetLiveRowMetadata(t);
    dropStaleState(t, selection, resetViewOffset);
}

ClearToMarkOutcome clearToPreviousMark(Terminal* t, SelectionState* selection, const ResetViewOffset& resetViewOffset, const SendToProcess& sendToProcess)
{
    if (!t)
        return ClearToMarkOutcome::FullClear;
    // Eating a command block deletes live rows, so it needs the same handoff
    // ``clearScreen`` makes. The marks it searches are the primary screen's in
    // any case: on the alternate screen the anchors would be another screen's.
    if (programOwnsLayout(*t)) {
        if (sendToProcess)
            sendToProcess(redrawRequest);
        return ClearToMarkOutcome::HandedOff;
    }

    // The row cleared to, so zero is a success and only -1 is "no mark above".
    if (Ubiquitty::Screen::dropRowsToPreviousMark(t) >= 0) {
        dropStaleState(t, selection, resetViewOffset);
        return ClearToMarkOutcome::ClearedToMark;
    }
    // No executed prompt above: clear the buffer instead, which invalidates the
    // same state and moves the screen, so it needs the redraw request too.
    clearBuffer(t, selection, resetViewOffset, sendToProcess);
    return ClearToMarkOutcome::FullClear;
}

ClearToMarkOutcome clearToMarkOrFull(Terminal* t, int* viewOffset)
{
    if (!t)
        return ClearToMarkOutcome::FullClear;
    return clearToPreviousMark(t, nullptr, [viewOffset] {
        if (viewOffset)
            *viewOffset = 0;
    },
        {});
}

} // namespace Ubiquitty::Screen
