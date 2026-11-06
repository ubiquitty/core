// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_clear_buffer.cpp — the shared "clear" operation.
//
// Clearing looks like a one-line model edit and is not: the step every frontend
// forgets is telling the process to redraw, without which the prompt sits at row
// 0 while the shell's line editor still echoes at the row it remembers. These
// cases pin the whole outcome, so a frontend that adopts this inherits the part
// that is easy to miss rather than rediscovering it from a bug report.

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include <memory>

#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Marks.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/Selection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty;

// Records what Core asked the frontend to do, so a test asserts the whole
// sequence rather than a description of it.
struct Recorder {
    int viewResets = 0;
    std::string sent;

    Screen::ResetViewOffset resetView()
    {
        return [this] { ++viewResets; };
    }
    Screen::SendToProcess send()
    {
        return [this](std::span<const char> bytes) { sent.append(bytes.data(), bytes.size()); };
    }
};

std::unique_ptr<Terminal> makeTerminal(int rows = 6, int columns = 20)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

void feed(Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

// One visible row as text, trailing blanks dropped, so a case compares what the
// grid shows against a string instead of counting cells that satisfied a
// predicate. Every case here writes ASCII, which is what makes the narrowing
// cast safe.
std::string visibleRow(const Terminal& t, int row)
{
    std::vector<Cell> cells(static_cast<std::size_t>(t.columns));
    Screen::getDisplayRow(t, 0, row, cells.data(), t.columns);
    std::string text;
    for (const Cell& cell : cells)
        text.push_back(cell.codePoint == 0 ? ' ' : static_cast<char>(cell.codePoint));
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

// One row of retained history, addressed from the newest scrolled-off line
// backwards, so a case can assert that a "clear" moved rows out of view rather
// than destroying them.
std::string historyRow(const Terminal& t, int linesBack)
{
    std::vector<Cell> cells(static_cast<std::size_t>(t.columns));
    Screen::getDisplayRow(t, linesBack, 0, cells.data(), t.columns);
    std::string text;
    for (const Cell& cell : cells)
        text.push_back(cell.codePoint == 0 ? ' ' : static_cast<char>(cell.codePoint));
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

TEST(testClearDropsScreenAndHistory)
{
    auto t = makeTerminal();
    for (int i = 0; i < 30; ++i)
        feed(*t, "history\r\n");
    feed(*t, "visible");
    CHECK_MSG(Screen::scrollbackCount(t.get()) > 0, "there is history to drop");

    Recorder target;
    Screen::clearBuffer(t.get(), nullptr, target.resetView(), target.send());

    CHECK_MSG(Screen::scrollbackCount(t.get()) == 0, "the scrollback is gone");
    CHECK_MSG(target.viewResets == 1, "the frontend is told to snap to the tail");

    // The screen goes too — this is what distinguishes it from
    // clear_scrollback, which leaves the grid alone. What survives is the
    // in-progress input line, lifted to row 0: clearBufferPreserveInput keeps
    // columns [0, cursorX) of the cursor row, so a user who clears mid-command
    // still has the half-typed command in front of them.
    CHECK_STR_EQ(visibleRow(*t, 0), "visible");
    for (int row = 1; row < t->rows; ++row)
        CHECK_MSG(visibleRow(*t, row).empty(), "and every row below it is blank");
    t.reset();
}

// The step a frontend forgets. Without these bytes the prompt is at row 0 and
// the shell still echoes where it thinks the prompt is.
TEST(testClearAsksTheProcessToRedraw)
{
    auto t = makeTerminal();
    Recorder target;
    Screen::clearBuffer(t.get(), nullptr, target.resetView(), target.send());

    CHECK_MSG(target.sent == "\x0c", "Ctrl-L reaches the process: clear-screen at a shell, redraw in a TUI");
    t.reset();
}

TEST(testClearWithoutASelectionModelIsFine)
{
    // A frontend with no selection passes null rather than inventing a state to
    // satisfy the signature.
    auto t = makeTerminal();
    Recorder target;
    Screen::clearBuffer(t.get(), nullptr, target.resetView(), target.send());
    CHECK(!target.sent.empty());
    t.reset();
}

TEST(testClearDropsTheSelection)
{
    // A selection names rows that clearing destroys; keeping it would leave a
    // highlight over unrelated text, and a copy would return the wrong bytes.
    auto t = makeTerminal();
    for (int i = 0; i < 10; ++i)
        feed(*t, "line\r\n");

    SelectionState selection {};
    Selection::init(&selection);
    Selection::begin(&selection, t->rows, t->columns, 1, 0, SelectionMode::Character);
    Selection::update(&selection, t->rows, t->columns, 2, 5);
    CHECK_MSG(selection.active, "a selection exists");

    Recorder target;
    Screen::clearBuffer(t.get(), &selection, target.resetView(), target.send());
    CHECK_MSG(!selection.active, "clearing drops it");
    t.reset();
}

TEST(testClearOnANullTerminalTouchesNothing)
{
    Recorder target;
    Screen::clearBuffer(nullptr, nullptr, target.resetView(), target.send());
    CHECK_MSG(target.sent.empty(), "nothing forwarded");
    CHECK_MSG(target.viewResets == 0, "and no view reset");
}

// The sequence is Core's, so a frontend cannot perform four of the five steps.
// Every mechanism the interface names is exercised on one ordinary clear.
TEST(testEveryFrontendStepIsDriven)
{
    auto t = makeTerminal();
    for (int i = 0; i < 10; ++i)
        feed(*t, "line\r\n");

    SelectionState selection {};
    Selection::init(&selection);
    Selection::begin(&selection, t->rows, t->columns, 1, 0, SelectionMode::Character);
    Selection::update(&selection, t->rows, t->columns, 2, 5);

    Recorder target;
    Screen::clearBuffer(t.get(), &selection, target.resetView(), target.send());

    CHECK_MSG(Screen::scrollbackCount(t.get()) == 0, "history cleared");
    CHECK_MSG(!selection.active, "selection dropped");
    CHECK_MSG(target.viewResets == 1, "view snapped to the tail");
    CHECK_MSG(target.sent == "\x0c", "process asked to redraw");
    t.reset();
}

// Clearing the scrollback shares everything with clearing the buffer except the
// screen. A selection naming rows that are gone is just as wrong here, and one
// frontend can drop the history without dropping the selection naming it.
TEST(testClearScrollbackDropsSelectionAndSnapsTheView)
{
    auto t = makeTerminal();
    for (int i = 0; i < 12; ++i)
        feed(*t, "history\r\n");
    feed(*t, "onscreen");
    CHECK(Screen::scrollbackCount(t.get()) > 0);

    SelectionState selection {};
    Selection::init(&selection);
    Selection::begin(&selection, t->rows, t->columns, 1, 0, SelectionMode::Character);
    Selection::update(&selection, t->rows, t->columns, 2, 5);

    Recorder target;
    Screen::clearScrollback(t.get(), &selection, target.resetView());

    CHECK_MSG(Screen::scrollbackCount(t.get()) == 0, "history gone");
    CHECK_MSG(!selection.active, "selection dropped — it named rows that no longer exist");
    CHECK_MSG(target.viewResets == 1, "view snapped to the tail");
    CHECK_MSG(target.sent.empty(), "no redraw request: the visible screen did not change");
    t.reset();
}

TEST(testClearScrollbackOnANullTerminalTouchesNothing)
{
    Recorder target;
    Screen::clearScrollback(nullptr, nullptr, target.resetView());
    CHECK(target.viewResets == 0);
}

// The mark clear drops the same stale state — but only when rows actually went.
// A search that finds no prompt changed nothing, so a selection that was valid
// before is still valid, and clearing it would surprise the user for no reason.
TEST(testClearToMarkDropsStaleStateOnlyWhenRowsWent)
{
    auto t = makeTerminal();

    SelectionState selection {};
    Selection::init(&selection);
    Selection::begin(&selection, t->rows, t->columns, 0, 0, SelectionMode::Character);
    Selection::update(&selection, t->rows, t->columns, 1, 3);
    CHECK(selection.active);

    // No prompt has ever been marked, so the mark path cannot run — and the
    // fallback that does run is a full clear, which invalidates everything.
    Recorder target;
    const Screen::ClearToMarkOutcome outcome = Screen::clearToPreviousMark(t.get(), &selection, target.resetView(), target.send());
    CHECK_MSG(outcome == Screen::ClearToMarkOutcome::FullClear, "no mark, so the buffer clear runs instead");
    CHECK_MSG(!selection.active, "the fallback cleared the buffer, so the selection is stale after all");
    CHECK_MSG(target.viewResets == 1, "and the view is snapped");
    CHECK_MSG(target.sent == "\x0c", "the fallback moved the screen, so the process is asked to redraw");

    t.reset();
}

TEST(testClearToMarkOnANullTerminalTouchesNothing)
{
    Recorder target;
    (void)Screen::clearToPreviousMark(nullptr, nullptr, target.resetView(), target.send());
    CHECK(target.viewResets == 0);
    CHECK(target.sent.empty());
}

// The mark path leaves the screen where it is, so unlike the fallback it needs
// no redraw request — the shell's prompt row is still correct.
//
// The marked prompt here is the FIRST row, which is the boundary the return
// value straddles: dropRowsToPreviousMark answers with the row it cleared to,
// so a mark at row 0 answers 0. A caller reading that as failure runs the full
// clear on top of rows the mark path has already dropped, and every freshly
// started shell is in exactly that state.
TEST(testClearToMarkKeepsTheScreenSoItAsksForNoRedraw)
{
    auto t = makeTerminal();
    feed(*t, "\033]133;A\033\\"); // prompt start
    feed(*t, "\033]133;C\033\\"); // command executed
    for (int i = 0; i < 8; ++i)
        feed(*t, "output\r\n");
    feed(*t, "\033]133;A\033\\");

    Recorder target;
    const Screen::ClearToMarkOutcome outcome = Screen::clearToPreviousMark(t.get(), nullptr, target.resetView(), target.send());
    CHECK_MSG(outcome == Screen::ClearToMarkOutcome::ClearedToMark,
        "a marked prompt above means the mark path runs, not the full-clear fallback");
    CHECK_MSG(target.sent.empty(), "the mark path moves no screen, so it asks for no redraw");
    CHECK_MSG(target.viewResets == 1, "but the view is still snapped: rows went");
    t.reset();
}

} // namespace

UBIQUITTY_TEST_MAIN()

// --- clearScreen: the gesture a "clear" key should reach for -------------------
//
// The distinction these cases pin is scroll versus erase. Erasing the visible
// grid is what forces the Ctrl-L above: the screen moved under a program that
// has no way of being told. Scrolling needs no such repair, because ordinary
// output performs the same scroll on every line that overflows the screen.

TEST(testClearScreenScrollsRatherThanDestroys)
{
    auto t = makeTerminal();
    for (int i = 0; i < 4; ++i)
        feed(*t, "old\r\n");
    feed(*t, "prompt");
    const int historyBefore = Screen::scrollbackCount(t.get());

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CursorRow, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::Scrolled, "rows moved rather than went");
    CHECK_STR_EQ(visibleRow(*t, 0), "prompt");
    for (int row = 1; row < t->rows; ++row)
        CHECK_MSG(visibleRow(*t, row).empty(), "the screen below the prompt is clean");

    // The point of the whole design: the rows are still there.
    CHECK_MSG(Screen::scrollbackCount(t.get()) == historyBefore + 4, "the four rows above went into history");
    CHECK_STR_EQ(historyRow(*t, 1), "old");
    t.reset();
}

TEST(testClearScreenSendsNothing)
{
    // No bytes, because nothing needs repairing: the prompt did not move
    // relative to its own content, so the shell's line editor is still right.
    auto t = makeTerminal();
    for (int i = 0; i < 3; ++i)
        feed(*t, "line\r\n");
    feed(*t, "prompt");

    Recorder target;
    Screen::clearScreen(t.get(), Screen::ClearBoundary::CursorRow, nullptr, target.resetView(), target.send());
    CHECK_MSG(target.sent.empty(), "a scroll asks the process for nothing");
    t.reset();
}

TEST(testClearScreenAtTheTopDoesNothing)
{
    auto t = makeTerminal();
    feed(*t, "prompt");

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CursorRow, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::NoOp, "the boundary is already row 0");
    CHECK_MSG(Screen::scrollbackCount(t.get()) == 0, "and nothing was scrolled to reach it");
    CHECK_MSG(target.viewResets == 0, "a no-op does not disturb the frontend");
    t.reset();
}

TEST(testClearScreenStopsAtThePromptRow)
{
    // With shell integration the boundary is exact rather than a guess. The
    // case only means something if the prompt row and the cursor row differ, so
    // the prompt spans two lines: CurrentPrompt must keep both, where CursorRow
    // would push the prompt's first line off the top.
    auto t = makeTerminal();
    for (int i = 0; i < 3; ++i)
        feed(*t, "output\r\n"); // rows 0-2
    feed(*t, "\x1b]133;A\x07"); // the prompt starts at row 3
    feed(*t, "user@host\r\n$ ");
    feed(*t, "\x1b]133;B\x07");
    feed(*t, "typed"); // the cursor is now on row 4

    CHECK_MSG(t->pendingPromptRow == 3, "the prompt was recorded a row above the cursor");

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CurrentPrompt, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::Scrolled, "the three output rows moved");
    CHECK_STR_EQ(visibleRow(*t, 0), "user@host"); // not "$ typed": the boundary was the prompt
    CHECK_STR_EQ(visibleRow(*t, 1), "$ typed");
    CHECK_MSG(Screen::scrollbackCount(t.get()) == 3, "exactly the rows above the prompt");
    CHECK_STR_EQ(historyRow(*t, 1), "output");
    t.reset();
}

TEST(testClearScreenStillStopsAtThePromptAfterAResizeRewrapsTheRowsAboveIt)
{
    // The prompt row is absolute, and a narrowing resize rewraps the output
    // above it onto more rows: the prompt's line moves down, and the clear
    // must keep it rather than the row its old number names now.
    auto t = makeTerminal(8, 20);
    feed(*t, "a long output line 1\r\n");
    feed(*t, "a long output line 2\r\n");
    feed(*t, "\x1b]133;A\x07");
    feed(*t, "user@host\r\n$ ");
    feed(*t, "\x1b]133;B\x07");
    feed(*t, "typed");
    CHECK_MSG(t->pendingPromptRow == 2, "the prompt was recorded below the two output lines");

    Ubiquitty::resize(*t, 8, 10);
    CHECK_MSG(t->pendingPromptRow == 4, "the prompt rode the rewrap down two rows");

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CurrentPrompt, nullptr, target.resetView(), target.send());
    CHECK_MSG(outcome == Screen::ClearOutcome::Scrolled, "the rewrapped output rows moved");
    CHECK_STR_EQ(visibleRow(*t, 0), "user@host");
    CHECK_STR_EQ(visibleRow(*t, 1), "$ typed");
    t.reset();
}

TEST(testClearScreenFallsBackToTheCursorDuringACommand)
{
    // OSC 133 C retires the pending prompt, so a clear fired while a command is
    // producing output has no prompt to aim at and uses the cursor row — the
    // same boundary a shell with no integration at all gets.
    auto t = makeTerminal();
    feed(*t, "\x1b]133;A\x07");
    feed(*t, "$ ");
    feed(*t, "\x1b]133;C\x07"); // the command starts running
    feed(*t, "\r\nout1\r\nout2");

    CHECK_MSG(t->pendingPromptRow < 0, "no prompt is pending mid-command");

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CurrentPrompt, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::Scrolled, "the cursor row became the boundary");
    CHECK_STR_EQ(visibleRow(*t, 0), "out2");
    t.reset();
}

TEST(testClearScreenHandsOffOnTheAlternateScreen)
{
    // A full-screen program owns every row, and there is no sequence telling it
    // to repaint. So the terminal touches nothing and forwards the request that
    // every shell, editor, pager and multiplexer reads as "redraw".
    auto t = makeTerminal();
    for (int i = 0; i < 3; ++i)
        feed(*t, "scrollback\r\n");
    feed(*t, "\x1b[?1049h\x1b[H"); // enter the alternate screen, cursor home
    feed(*t, "tui row");
    const int historyBefore = Screen::scrollbackCount(t.get());

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CurrentPrompt, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::HandedOff, "the program is asked instead");
    CHECK_MSG(target.sent == "\x0c", "and asked with Ctrl-L");
    CHECK_STR_EQ(visibleRow(*t, 0), "tui row");
    CHECK_MSG(Screen::scrollbackCount(t.get()) == historyBefore, "the primary screen's history is untouched");
    CHECK_MSG(target.viewResets == 0, "nothing moved, so nothing to snap");
    t.reset();
}

TEST(testClearScreenHandsOffUnderAScrollRegion)
{
    // DECSTBM is the same statement as the alternate screen made a different
    // way: the rows outside the region are chrome the program painted once and
    // will not repaint.
    auto t = makeTerminal();
    for (int i = 0; i < 3; ++i)
        feed(*t, "line\r\n");
    feed(*t, "\x1b[2;5r"); // DECSTBM
    feed(*t, "body");

    Recorder target;
    const Screen::ClearOutcome outcome
        = Screen::clearScreen(t.get(), Screen::ClearBoundary::CursorRow, nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearOutcome::HandedOff, "a scroll region is a program owning the layout");
    CHECK_MSG(target.sent == "\x0c", "and gets the redraw request");
    t.reset();
}

TEST(testClearBufferHandsOffOnTheAlternateScreen)
{
    // The destructive command inherits the same guard: erasing the grid behind
    // a full-screen program is precisely the damage nothing downstream repairs.
    auto t = makeTerminal();
    feed(*t, "\x1b[?1049h");
    feed(*t, "tui row");

    Recorder target;
    Screen::clearBuffer(t.get(), nullptr, target.resetView(), target.send());

    CHECK_STR_EQ(visibleRow(*t, 0), "tui row");
    CHECK_MSG(target.sent == "\x0c", "the request is forwarded, not performed");
    t.reset();
}

TEST(testClearToPreviousMarkHandsOffOnTheAlternateScreen)
{
    // Eating a command block deletes live rows, so Cmd-L needs the same
    // handoff. The outcome says so rather than reporting a clear that never ran.
    auto t = makeTerminal();
    feed(*t, "\x1b[?1049h");
    feed(*t, "tui row");

    Recorder target;
    const Screen::ClearToMarkOutcome outcome
        = Screen::clearToPreviousMark(t.get(), nullptr, target.resetView(), target.send());

    CHECK_MSG(outcome == Screen::ClearToMarkOutcome::HandedOff, "not FullClear: nothing was cleared");
    CHECK_MSG(target.sent == "\x0c", "the program got the redraw request");
    CHECK_STR_EQ(visibleRow(*t, 0), "tui row");
    t.reset();
}

TEST(testClearScrollbackStillWorksUnderAProgram)
{
    // History is never the program's, so this one needs no handoff at all —
    // which is the other half of the rule the handoff comes from.
    auto t = makeTerminal();
    for (int i = 0; i < 8; ++i)
        feed(*t, "history\r\n");
    CHECK_MSG(Screen::scrollbackCount(t.get()) > 0, "there is history to drop");
    feed(*t, "\x1b[?1049h\x1b[H");
    feed(*t, "tui row");

    Recorder target;
    Screen::clearScrollback(t.get(), nullptr, target.resetView());

    CHECK_MSG(Screen::scrollbackCount(t.get()) == 0, "the history goes");
    CHECK_STR_EQ(visibleRow(*t, 0), "tui row");
    CHECK_MSG(target.sent.empty(), "and the program is not disturbed");
    t.reset();
}
