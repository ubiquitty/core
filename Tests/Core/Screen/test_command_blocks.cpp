// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_command_blocks.cpp — enumerating the OSC 133 command history.
//
// Every case drives a real Terminal with real OSC 133 sequences rather than
// writing marks onto rows directly. The marks are what the parser puts there,
// so a walk tested against hand-placed ones keeps passing after the parser
// stops placing them, and the frontend that reads the walk goes blank with
// nothing failing.
//
// What is pinned here is the set of decisions a second frontend would otherwise
// make for itself: the scan direction, the bound, how several rows become one
// label, what the whitespace between them becomes, that an executed prompt with
// no text is absent rather than blank, and how many rows the alternate screen
// addresses.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Addressing.h"
#include "Core/Screen/CommandBlocks.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <string>

namespace {

using Ubiquitty::Screen::CommandBlockList;
using Ubiquitty::Screen::commandBlockOutput;
using Ubiquitty::Screen::commandBlocks;
using Ubiquitty::Screen::CommandOutputRows;
using Ubiquitty::Screen::lastCommandOutputRows;
using Ubiquitty::Screen::maxCommandBlocks;
using Ubiquitty::Screen::maxCommandOutputRows;
using Ubiquitty::Screen::maxCommandTitleRows;
using UbiquittyTest::feed;

// A terminal sized for the case, reset the way a session starts.
void startTerminal(Ubiquitty::Terminal& t, int rows, int columns)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);
}

// One command with its output, in the byte order a shell integration emits:
// the previous command ends, the prompt starts, the command line is echoed, the
// output begins.
void runCommand(Ubiquitty::Terminal& t, const char* commandLine, const char* output)
{
    feed(t, "\033]133;D;0\007\033]133;A\007");
    feed(t, commandLine);
    feed(t, "\r\n\033]133;C\007");
    feed(t, output);
    feed(t, "\r\n");
}

// The command labels, newest first, as one line — the report a failure prints.
std::string describe(const CommandBlockList& blocks)
{
    std::string text;
    for (const Ubiquitty::Screen::CommandBlock& block : blocks) {
        if (!text.empty())
            text += " | ";
        text += block.command;
    }
    return text;
}

// The newest command is the first entry, not the last: a palette lists what the
// user just ran at the top, and a walk that reads the buffer forwards puts it
// at the bottom of a 300-entry list.
TEST(testBlocksComeNewestFirst)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo one", "one");
    runCommand(t, "$ echo two", "two");
    runCommand(t, "$ echo three", "three");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testBlocksComeNewestFirst: %s\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 3);
    CHECK_STR_EQ(blocks[0].command, "$ echo three");
    CHECK_STR_EQ(blocks[1].command, "$ echo two");
    CHECK_STR_EQ(blocks[2].command, "$ echo one");

    // The prompt rows are the jump targets, and they descend with the list.
    CHECK_EQ(blocks[0].promptRow, 4);
    CHECK_EQ(blocks[1].promptRow, 2);
    CHECK_EQ(blocks[2].promptRow, 0);
}

// The bound keeps the newest and drops the oldest. A walk that stopped at the
// bound while reading forwards would keep the oldest instead, which is the half
// of the history a palette never wants.
TEST(testMaxBlocksKeepsTheNewest)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 24, 40);
    for (int i = 0; i < 8; ++i) {
        char commandLine[64];
        snprintf(commandLine, sizeof commandLine, "$ echo %d", i);
        runCommand(t, commandLine, "out");
    }

    const CommandBlockList limited = commandBlocks(&t, 3);
    printf("testMaxBlocksKeepsTheNewest: %s\n", describe(limited).c_str());
    REQUIRE(limited.size() == 3);
    CHECK_STR_EQ(limited[0].command, "$ echo 7");
    CHECK_STR_EQ(limited[1].command, "$ echo 6");
    CHECK_STR_EQ(limited[2].command, "$ echo 5");

    // Without a bound below the history size, every block is listed.
    CHECK_EQ(static_cast<int>(commandBlocks(&t, maxCommandBlocks).size()), 8);
    // A bound of zero or less asks for nothing.
    CHECK_EQ(static_cast<int>(commandBlocks(&t, 0).size()), 0);
    CHECK_EQ(static_cast<int>(commandBlocks(&t, -1).size()), 0);
}

// A command occupying more than one row is one label. The prompt here prints an
// information line above the command, so the region spans two rows, and the
// spaces the shell laid out inside the command line collapse to one.
TEST(testMultiRowCommandJoinsWithSingleSpaces)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 60);
    feed(t, "\033]133;D;0\007\033]133;A\007[user@host ~]\r\n> echo   a    b\r\n\033]133;C\007out\r\n");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testMultiRowCommandJoinsWithSingleSpaces: %s\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 1);
    CHECK_STR_EQ(blocks[0].command, "[user@host ~] > echo a b");
    CHECK_EQ(blocks[0].promptRow, 0);
}

// The label is bounded by rows, not by where the program puts its marks.
//
// The span from the prompt row to the output mark is delimited by two things a
// program prints, so a program that emits `OSC 133;A`, a scrollback's worth of
// text, and only then `OSC 133;C` names a region as large as the scrollback. The
// label reads at most maxCommandTitleRows of it whatever the marks claim, and the
// palette re-reads that label on each keystroke. The output half of the same
// block is bounded by maxCommandOutputRows for the same reason.
//
// Fails-without-it: the label carries every row of the span, so it grows with the
// text the program printed rather than stopping at maxCommandTitleRows.
TEST(testCommandLabelStopsAtTheRowMaximum)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 8, 40);

    // A prompt mark, far more rows than the bound, and only then the output mark.
    feed(t, "\033]133;D;0\007\033]133;A\007");
    const int rowsPrinted = maxCommandTitleRows * 3;
    for (int i = 0; i < rowsPrinted; ++i)
        feed(t, "wwwwwwwwww\r\n"); // ten non-space bytes, so no row collapses away
    feed(t, "\033]133;C\007out\r\n");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    REQUIRE(blocks.size() >= 1);

    // Every row contributes its ten bytes plus one joining space, so the label's
    // length says how many rows were read. Asserted as a bound on rows rather
    // than a byte count, so a change to the row text does not silently retune it.
    const std::size_t bytesPerRow = 11;
    const std::size_t rowsRead = (blocks[0].command.size() + 1) / bytesPerRow;
    printf("testCommandLabelStopsAtTheRowMaximum: printed %d rows, label reads %zu\n", rowsPrinted, rowsRead);
    CHECK(rowsRead <= static_cast<std::size_t>(maxCommandTitleRows));
    CHECK(static_cast<int>(rowsRead) < rowsPrinted); // the bound is reached, so it is doing work

    // And a command inside the bound is untouched: it must not truncate the
    // ordinary multi-row case the label exists to render.
    Ubiquitty::Terminal small;
    startTerminal(small, 8, 60);
    feed(small, "\033]133;D;0\007\033]133;A\007[user@host ~]\r\n> echo a b\r\n\033]133;C\007out\r\n");
    const CommandBlockList smallBlocks = commandBlocks(&small, maxCommandBlocks);
    REQUIRE(smallBlocks.size() == 1);
    CHECK_STR_EQ(smallBlocks[0].command, "[user@host ~] > echo a b");
}

// The separator set is the Unicode whitespace property, not its ASCII subset. A
// command pasted out of a document carries a no-break space where a plain one
// belongs, and a label that keeps it shows the user a gap that no other command
// in the list has.
TEST(testNonASCIIWhitespaceCollapsesToo)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    // U+00A0 no-break space between the two operands, twice over.
    runCommand(t, "$ echo a\xc2\xa0\xc2\xa0", "out");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testNonASCIIWhitespaceCollapsesToo: [%s]\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 1);
    CHECK_STR_EQ(blocks[0].command, "$ echo a");
}

// An executed prompt whose rows hold no text is absent from the list, not
// present as a blank row — and it does not spend the caller's bound, so a
// shell that marks prompts it never prints cannot push real commands off the
// end.
// A leading run of whitespace is trimmed rather than emitted as one space. A
// prompt indented by its shell, or a command whose first row is padding, would
// otherwise show a label starting with a gap while every other row in the list
// starts at the margin — and the collapse that produces the gap and the collapse
// that joins the rows are one loop, so the trim is not implied by the join.
TEST(testLeadingWhitespaceIsTrimmedNotCollapsed)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 60);
    feed(t, "\033]133;D;0\007\033]133;A\007    $ echo indented\r\n\033]133;C\007out\r\n");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testLeadingWhitespaceIsTrimmedNotCollapsed: [%s]\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 1);
    CHECK_STR_EQ(blocks[0].command, "$ echo indented");
}

// The label is read at the terminal's full width. A read one column short drops
// the last character of any command that fills the row, which is the case a
// narrow window produces and a wide one hides — so the width is pinned against a
// command that ends exactly at the margin.
TEST(testLabelIsReadAtTheFullWidth)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 20);
    // Exactly twenty characters, so the final 'X' sits in the last column and a
    // width of columns - 1 drops it. A payload one short leaves that column
    // blank, and the shortened read then returns the same string — which is the
    // form of this case that passed while the width was unpinned.
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo abcdefghijkXY\r\n\033]133;C\007out\r\n");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testLabelIsReadAtTheFullWidth: [%s]\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 1);
    CHECK_STR_EQ(blocks[0].command, "$ echo abcdefghijkXY");
}

TEST(testEmptyExecutedPromptIsAbsent)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo one", "one"); // rows 0, 1
    // A and C on the same untouched row: the prompt executes with nothing typed
    // and nothing printed, then output arrives on the row below.
    feed(t, "\033]133;D;0\007\033]133;A\007\033]133;C\007\r\nstray output\r\n"); // rows 2, 3
    runCommand(t, "$ echo two", "two"); // rows 4, 5

    // The blank prompt really is marked as executed — the case would prove
    // nothing if the walk had no candidate to reject.
    CHECK_MSG((Ubiquitty::Screen::rowMark(&t, 2) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0,
        "row 2 carries CMD_EXECUTED, so the walk has to decide about it");

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testEmptyExecutedPromptIsAbsent: %s\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 2);
    CHECK_STR_EQ(blocks[0].command, "$ echo two");
    CHECK_STR_EQ(blocks[1].command, "$ echo one");

    // The same two under a bound of two: a blank entry between them would
    // have taken the second slot and hidden "$ echo one".
    const CommandBlockList limited = commandBlocks(&t, 2);
    REQUIRE(limited.size() == 2);
    CHECK_STR_EQ(limited[1].command, "$ echo one");
}

// A non-zero exit status is reported as a flag on the block, and it lands on
// the command that failed rather than on the prompt that followed it.
TEST(testFailedCommandIsFlagged)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo ok", "ok"); // rows 0, 1
    feed(t, "\033]133;D;0\007\033]133;A\007$ false\r\n\033]133;C\007"); // row 2
    feed(t, "\033]133;D;1\007\033]133;A\007$ "); // D;1 fails the prompt at row 2

    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testFailedCommandIsFlagged: %s\n", describe(blocks).c_str());
    REQUIRE(blocks.size() == 2);
    CHECK_STR_EQ(blocks[0].command, "$ false");
    CHECK_MSG(blocks[0].didFail, "the command that exited non-zero is flagged");
    CHECK_STR_EQ(blocks[1].command, "$ echo ok");
    CHECK_MSG(!blocks[1].didFail, "the command that exited zero is not");
}

// The alternate screen addresses its own rows and nothing above them: history
// counts as zero while it is up, so the walk is bounded at the visible row
// count instead of at history plus the visible rows.
//
// The history here opens with unmarked filler, and that choice is the case:
// row addressing keeps its own history whatever the bound says, so the rows
// left inside the bound are the oldest retained lines. Filler there is what
// makes the bound observable — covering history plus the screen finds the
// commands, covering the screen's worth of rows alone finds only filler — and
// what keeps the case from asserting anything about which rows lie under it.
TEST(testAlternateScreenCountsNoHistory)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 6, 40);
    for (int i = 0; i < 20; ++i) {
        char line[32];
        snprintf(line, sizeof line, "filler %d\r\n", i);
        feed(t, line);
    }
    runCommand(t, "$ echo one", "one");
    runCommand(t, "$ echo two", "two");
    runCommand(t, "$ echo three", "three");

    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    printf("testAlternateScreenCountsNoHistory: history=%d\n", history);
    REQUIRE_MSG(history > t.rows, "the filler must outrun the screen, or the bound changes nothing");

    const CommandBlockList onPrimary = commandBlocks(&t, maxCommandBlocks);
    printf("  primary: %s\n", describe(onPrimary).c_str());
    CHECK_EQ(static_cast<int>(onPrimary.size()), 3);
    CHECK_MSG(lastCommandOutputRows(&t).isFound, "the primary screen has a newest block");

    feed(t, "\033[?1049h"); // a full-screen program takes over
    const CommandBlockList onAlternate = commandBlocks(&t, maxCommandBlocks);
    printf("  alternate: %s\n", describe(onAlternate).c_str());
    CHECK_MSG(onAlternate.empty(), "the walk stops at a screen's worth of rows, and the commands sit past that bound");
    CHECK_MSG(!lastCommandOutputRows(&t).isFound, "and reports no newest block either");

    feed(t, "\033[?1049l"); // and hands the screen back
    CHECK_EQ(static_cast<int>(commandBlocks(&t, maxCommandBlocks).size()), 3);
}

// The output span starts one row past the command line and runs to the end of
// the buffer: output belongs to the newest command until another prompt claims
// it, and none has.
TEST(testLastOutputSpansToTheEndOfTheBuffer)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo one", "one"); // rows 0, 1
    runCommand(t, "$ echo two", "two"); // rows 2, 3
    runCommand(t, "$ echo three", "three"); // rows 4, 5
    feed(t, "\033]133;D;0\007\033]133;A\007$ "); // row 6, a bare prompt that never ran

    const CommandOutputRows span = lastCommandOutputRows(&t);
    printf("testLastOutputSpansToTheEndOfTheBuffer: found=%d from=%d to=%d\n",
        static_cast<int>(span.isFound), span.from, span.to);
    CHECK_MSG(span.isFound, "a marked command block exists");
    CHECK_EQ(span.from, 5); // the row "three" printed on
    CHECK_EQ(span.to, 11); // the last row of the buffer
}

// Only the newest block is consulted. An older command's output is not the
// newest one's, so a scan that fell through to the previous prompt would scope
// a search to the wrong rows.
TEST(testLastOutputIgnoresOlderBlocks)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo one", "one"); // rows 0, 1
    runCommand(t, "$ echo two", "two"); // rows 2, 3

    const CommandOutputRows span = lastCommandOutputRows(&t);
    printf("testLastOutputIgnoresOlderBlocks: from=%d to=%d\n", span.from, span.to);
    CHECK_MSG(span.isFound, "a marked command block exists");
    CHECK_EQ(span.from, 3); // "two", not "one" on row 1
}

// No shell integration is a different answer from an empty span: a frontend
// hides its scope control on the first and shows an empty result on the second.
// A command whose OUTPUT_START lands on the buffer's final row has no output
// rows at all, and the span must say so rather than describing one backwards.
//
// The arithmetic produces from = commandLastRow + 1 and to = totalRows - 1, so
// this case makes from exceed to. A caller reading `isFound` and building a
// range out of the pair gets an inverted one — and Swift's ClosedRange TRAPS on
// lowerBound > upperBound rather than yielding something empty, so the frontend
// crashes on a terminal that has merely filled its last row.
TEST(testACommandOnTheFinalRowHasNoOutputSpan)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 4, 40);
    // Fill to the last row, then mark a command whose output would start past it.
    feed(t, "\033]133;D;0\007a\r\nb\r\nc\r\n");
    feed(t, "\033]133;A\007$ echo tail\033]133;C\007");

    const CommandOutputRows span = lastCommandOutputRows(&t);
    printf("testACommandOnTheFinalRowHasNoOutputSpan: found=%d from=%d to=%d\n",
        static_cast<int>(span.isFound), span.from, span.to);
    CHECK_MSG(!span.isFound || span.from <= span.to,
        "a found span never describes its rows backwards");
}

// A block's output stops at the NEXT executed prompt. Without that bound the
// oldest block would claim every row to the end of the buffer and carry every
// later command's output with it — and the newest block would still look right,
// which is what makes the defect survive a casual check.
TEST(testBlockOutputStopsAtTheNextPrompt)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo one", "one"); // rows 0, 1
    runCommand(t, "$ echo two", "two"); // rows 2, 3

    CHECK_STR_EQ(commandBlockOutput(&t, 0, maxCommandOutputRows), "one");
    CHECK_STR_EQ(commandBlockOutput(&t, 2, maxCommandOutputRows), "two");
}

// Trailing blank rows are the unwritten remainder of the buffer rather than
// output. A block ending mid-screen would otherwise carry the rest of the screen.
TEST(testBlockOutputDropsTheBlankRemainder)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    runCommand(t, "$ echo tail", "tail");

    const std::string output = commandBlockOutput(&t, 0, maxCommandOutputRows);
    printf("testBlockOutputDropsTheBlankRemainder: [%s]\n", output.c_str());
    CHECK_STR_EQ(output, "tail");
}

// The row bound is honoured, so a command that printed more than the palette can
// show cannot make the read proportional to a million-line scrollback.
TEST(testBlockOutputHonoursItsRowBound)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    // The newline after OUTPUT_START matters: without it the command and its
    // first line share a row, output begins at the SECOND line, and this case
    // would assert a bound against rows it had miscounted.
    feed(t, "\033]133;D;0\007\033]133;A\007$ seq\033]133;C\007\r\n");
    feed(t, "one\r\ntwo\r\nthree\r\n");

    CHECK_STR_EQ(commandBlockOutput(&t, 0, 2), "one\ntwo");
    CHECK_STR_EQ(commandBlockOutput(&t, 0, 0), "");
}

// A row naming no block, and a null terminal, both yield nothing rather than
// reading whatever sits at that address.
TEST(testBlockOutputRefusesARowWithNoBlock)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    feed(t, "plain text, no marks\r\n");

    CHECK_STR_EQ(commandBlockOutput(&t, 0, maxCommandOutputRows), "");
    CHECK_STR_EQ(commandBlockOutput(&t, -1, maxCommandOutputRows), "");
    CHECK_STR_EQ(commandBlockOutput(&t, 9999, maxCommandOutputRows), "");
    CHECK_STR_EQ(commandBlockOutput(nullptr, 0, maxCommandOutputRows), "");
}

TEST(testNoShellIntegrationFindsNoSpan)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 12, 40);
    feed(t, "$ echo one\r\none\r\n"); // the same text, with no marks behind it

    CHECK_MSG(commandBlocks(&t, maxCommandBlocks).empty(), "text alone is not a command block");
    CHECK_MSG(!lastCommandOutputRows(&t).isFound, "and there is no span to report");
}

// A frontend asks before it has a terminal — a palette opened with no pane
// focused, a window closing under a query. Neither entry point reads through
// the null.
TEST(testNullTerminalEnumeratesNothing)
{
    CHECK_MSG(commandBlocks(nullptr, maxCommandBlocks).empty(), "no terminal, no blocks");
    CHECK_MSG(!lastCommandOutputRows(nullptr).isFound, "no terminal, no span");
}

// A command that scrolled into history is still enumerated, and its prompt row
// is the absolute one — the identity that survives the rows ageing out of the
// live screen, which is what a caller jumps to.
TEST(testBlocksInHistoryKeepAbsoluteRows)
{
    Ubiquitty::Terminal t;
    startTerminal(t, 6, 40);
    for (int i = 0; i < 8; ++i) {
        char commandLine[64];
        snprintf(commandLine, sizeof commandLine, "$ echo %d", i);
        runCommand(t, commandLine, "out");
    }

    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    const CommandBlockList blocks = commandBlocks(&t, maxCommandBlocks);
    printf("testBlocksInHistoryKeepAbsoluteRows: history=%d %s\n", history, describe(blocks).c_str());
    REQUIRE_MSG(history > 0, "the screen must have overflowed, or nothing aged out");
    REQUIRE(blocks.size() == 8);
    CHECK_STR_EQ(blocks[0].command, "$ echo 7");
    CHECK_STR_EQ(blocks[7].command, "$ echo 0");
    // Two rows per command, oldest at absolute row 0.
    CHECK_EQ(blocks[7].promptRow, 0);
    CHECK_EQ(blocks[0].promptRow, 14);
    CHECK_MSG(blocks[7].promptRow < history, "the oldest block's prompt really is in history");
}

} // namespace

UBIQUITTY_TEST_MAIN()
