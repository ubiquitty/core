// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Screen/Marks.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

TEST(testConEmuPromptMarksTheCursorRowWithoutNotification)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 6);
    feed(t, "\033[2;3H\033]9;12\a$");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 3);
    CHECK(Ubiquitty::Screen::rowMark(&t, 1) & Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == 0);
    CHECK(!t.hasUnseenNotification);
    feed(t, "\033[3;1H\n");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t)) & Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\033[2J");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t)) == 0);
}

// OSC 133 prompt-mark placement + the mark-consuming clears (Cmd-K
// clearBufferPreserveInput, Cmd-L clearToPreviousMark). Pins that a clean
// session marks each executed prompt, that Cmd-L keeps the LAST executed
// command (never the first), and that neither Cmd-K nor scrollback drifts the
// pendingPromptRow so a later command's bracket lands off its prompt.
#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>

// Cmd-L runs the same Core entry point the macOS frontend and snapshot harness
// call — the frontend's clear-to-previous-mark command is just this under the
// model lock plus the UI follow-up — so the test exercises the real decision,
// not a copy of it.
static int cmdL(Ubiquitty::Terminal& t) { return Ubiquitty::Screen::dropRowsToPreviousMark(&t); }

static char cellCh(Ubiquitty::Terminal& t, int row, int column)
{
    const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, row, column);
    return c ? (char)c->codePoint : '?';
}

// Row 0 text matches s (first strlen(s) cells).
static bool rowIs(Ubiquitty::Terminal& t, int row, const char* s)
{
    for (int i = 0; s[i]; ++i)
        if (cellCh(t, row, i) != s[i])
            return false;
    return true;
}

// A clean 3-command session ending at a bare prompt: echo one / echo two / false.
static void feedThreeCommands(Ubiquitty::Terminal& t)
{
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo two\r\n\033]133;C\007two\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ false\r\n\033]133;C\007");
    feed(t, "\033]133;D;1\007\033]133;A\007$ ");
}

// Cmd-L eats the bottom-most executed command ($ false) and slides the current
// prompt up into its place — the earlier commands stay put.
// Repeated Cmd-L keeps eating from the bottom, one command per press.
// The user's actual sequence: run commands, Cmd-K, run a command, then the next
// command's bracket must land on the preserved prompt row (no drift), and Cmd-L
// must still keep the last command.
// Compounding: two Cmd-K cycles leave "$ bbb" as the only executed command
// above the bare prompt; Cmd-L eats it, leaving just the bare prompt.
// Cmd-L on a full screen (history present) keeps the screen full: the prompt
// stays anchored at the BOTTOM and the scrollback SHRINKS by the eaten command's
// height — history slides back into view at the top, not dead blanks below the
// prompt — never floating up and leaving history stale.
// Regression: on a full screen, repeated Cmd-L must keep the prompt pinned at
// the BOTTOM and shrink the scrollback each time — never let the prompt float to
// the top with stale history left scrollable above it.
// Two-line prompt (info line carries A; command typed on line 2). Cmd-L eats the
// whole last command block — both prompt lines + output — and the current
// two-line prompt rides up to where it started.
// Cmd-K must reset per-row metadata —
// DECDWL/DECDHL lineAttribute, OSC 133 marks, soft-wrap joins — on every
// cleared row, not just the cells. Stale meta is the root of the "bracket
// appears on the wrong row after Cmd-K" corruption in the manual test plan.
// Same contract for the plain (non-preserving) clearBuffer: cells, marks,
// line attributes, and wrap joins all go; a pending prompt (A fired, C not
// yet) must not promote a bracket onto a stale row afterwards.
// The host `clear` command (ESC[2J + home) must also drop per-row marks on
// every erased row, not just the cells — otherwise the renderer keeps drawing
// the OSC 133 prompt-mark edge brackets ([ … ]) on now-blank rows. Same
// contract clearBuffer enforces for Cmd-K, but driven through Ubiquitty::Screen::eraseInDisplay.
// Realistic cycle: Cmd-L eats "$ false", run a new command, Cmd-L again eats that
// freshly-run command — the anchor tracks the live prompt across the new run.
// OSC 633 ; E (explicit command line) + OSC 633 ; P ; Cwd (working dir).
// E carries the exact command the shell will run, with VS Code's escapes:
// "\xAB" for any byte (esp. ';' as "\x3b" and '\n' as "\x0a") and "\\" for a
// literal backslash. P;Cwd reports a bare path (the OSC-7 analog). Both must
// land on the Ubiquitty::Terminal so the command-event consumers (Copy Command / host) read
// the precise value, not a screen-scraped guess.
TEST(testCleanMarks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(rowIs(t, 0, "$ echo one"));
    CHECK(rowIs(t, 2, "$ echo two"));
    CHECK(rowIs(t, 4, "$ false"));
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 2) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 4) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 4) & Ubiquitty::Screen::osc133MarkCmdFailed) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 5) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0); // bare prompt
    printf("testCleanMarks done\n");
}

TEST(testOSC633CommandLineAndCwd)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 80);

    // A/B/E/C ordering VS Code emits. E carries an escaped semicolon ("\x3b"),
    // an escaped newline ("\x0a"), and an escaped backslash ("\\"), plus a
    // trailing ";<nonce>" the parser must drop.
    feed(t, "\033]633;A\007\033]633;B\007");
    feed(t, "\033]633;E;echo a\\x3b ls\\x0acd\\\\tmp;abc123nonce\007");
    feed(t, "\033]633;C\007");

    // Decoded: "echo a; ls\ncd\tmp" — ';' from \x3b, '\n' from \x0a, '\' from \\.
    const char* expected = "echo a; ls\ncd\\tmp";
    printf("test_osc633: commandLine=\"%s\"\n", t.commandLine.c_str());
    CHECK(t.commandLine == expected);
    CHECK(t.commandLineGeneration == 1);

    // 633;P;Cwd routes to the same currentDirectory sink as OSC 7 (bare path,
    // no file:// URL). Unknown P keys are ignored gracefully (no crash, no
    // clobber).
    feed(t, "\033]633;P;Cwd=/Users/x/proj\007");
    CHECK(t.currentDirectory == "/Users/x/proj");
    feed(t, "\033]633;P;IsWindows=False\007"); // accepted + ignored
    feed(t, "\033]633;P;Bogus=1\007"); // unknown key — ignored
    CHECK(t.currentDirectory == "/Users/x/proj");

    // A/B/C/D still drive the prompt marks (E/P didn't disturb them).
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);

    // A second E bumps the generation and replaces the text (no trailing nonce
    // this time — a literal ';' inside would be "\x3b", so the last literal ';'
    // is always the nonce delimiter; here there's none).
    feed(t, "\033]633;E;git status\007");
    CHECK(t.commandLine == "git status");
    CHECK(t.commandLineGeneration == 2);

    printf("testOSC633CommandLineAndCwd done\n");
}

TEST(test_cmdL_eats_last_command)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    // Before: row4 "$ false", row5 bare "$ ".
    const int prev = cmdL(t);
    printf("test_cmdL_eats_last_command: prev=%d (expect 4)\n", prev);
    CHECK(prev == 4);
    CHECK(rowIs(t, 0, "$ echo one")); // earlier commands untouched
    CHECK(rowIs(t, 1, "one"));
    CHECK(rowIs(t, 2, "$ echo two"));
    CHECK(rowIs(t, 3, "two"));
    CHECK(rowIs(t, 4, "$ ")); // the bare prompt rode up into "$ false"'s slot
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 2) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 4) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0); // bare prompt
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    printf("test_cmdL_eats_last_command done\n");
}

TEST(test_cmdL_eats_repeatedly)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    CHECK(cmdL(t) == 4); // eat "$ false"  -> prompt at row 4
    CHECK(rowIs(t, 4, "$ "));
    CHECK(cmdL(t) == 2); // eat "$ echo two" -> prompt at row 2
    CHECK(rowIs(t, 0, "$ echo one"));
    CHECK(rowIs(t, 1, "one"));
    CHECK(rowIs(t, 2, "$ "));
    CHECK(cmdL(t) == 0); // eat "$ echo one" -> prompt at row 0
    CHECK(rowIs(t, 0, "$ "));
    CHECK(cmdL(t) == -1); // nothing left to eat
    printf("test_cmdL_eats_repeatedly done\n");
}

TEST(test_cmdK_then_command_no_drift)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t); // ends at bare "$ " with A fired (pendingPromptRow set)
    // Cmd-K: preserve the input line, clear the rest.
    Ubiquitty::clearBufferPreserveInput(&t);
    CHECK(rowIs(t, 0, "$ "));
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    // Type + run the next command. C promotes the pending prompt -> must be row 0.
    feed(t, "ls\r\n\033]133;C\007out\r\n\033]133;D;0\007\033]133;A\007$ ");
    printf("test_cmdK: row0 mark=%d (expect CMD_EXECUTED set)\n", Ubiquitty::Screen::rowMark(&t, 0));
    CHECK(rowIs(t, 0, "$ ls"));
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    // No stray bracket on the output row.
    CHECK((Ubiquitty::Screen::rowMark(&t, 1) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
    printf("test_cmdK_then_command_no_drift done\n");
}

TEST(test_repeated_cmdK_then_cmdL)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    Ubiquitty::clearBufferPreserveInput(&t);
    feed(t, "aaa\r\n\033]133;C\007o\r\n\033]133;D;0\007\033]133;A\007$ ");
    Ubiquitty::clearBufferPreserveInput(&t);
    feed(t, "bbb\r\n\033]133;C\007o\r\n\033]133;D;0\007\033]133;A\007$ ");
    // Before: row0 "$ bbb" (executed), row1 "o", row2 bare "$ ".
    const int prev = cmdL(t);
    printf("test_repeated: prev=%d row0='%c%c%c'\n", prev, cellCh(t, 0, 0), cellCh(t, 0, 1), cellCh(t, 0, 2));
    CHECK(prev == 0);
    CHECK(rowIs(t, 0, "$ ")); // "$ bbb" eaten; the bare prompt rode up to row 0
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
    printf("test_repeated_cmdK_then_cmdL done\n");
}

TEST(test_cmdL_with_scrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);
    // 8 two-row commands so early ones scroll into history on a 6-row screen.
    for (int i = 0; i < 8; ++i) {
        char buffer[96];
        snprintf(buffer, sizeof buffer, "\033]133;D;0\007\033]133;A\007$ cmd%d\r\n\033]133;C\007o%d\r\n", i, i);
        feed(t, buffer);
    }
    feed(t, "\033]133;D;0\007\033]133;A\007$ "); // bare prompt at the bottom row
    const int hist = Ubiquitty::Screen::scrollbackCount(&t);
    const int prev = cmdL(t);
    printf("test_cmdL_with_scrollback: hist=%d prev=%d newHist=%d\n", hist, prev, Ubiquitty::Screen::scrollbackCount(&t));
    CHECK(prev == hist + 3); // "$ cmd7" was the live row at offset 3 (abs hist+3)
    CHECK(rowIs(t, 5, "$ ")); // prompt re-anchored at the bottom row
    CHECK(rowIs(t, 3, "$ cmd6")); // cmd6 still visible just above
    // "$ cmd7" + "o7" (2 rows) were eaten → history shrank by 2, two older lines
    // ("o4", "$ cmd5") slid back onto the top of the screen.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == hist - 2);
    CHECK(rowIs(t, 1, "$ cmd5"));
    printf("test_cmdL_with_scrollback done\n");
}

TEST(test_cmdL_scrollback_shrinks_each_eat)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);
    for (int i = 0; i < 8; ++i) {
        char buffer[96];
        snprintf(buffer, sizeof buffer, "\033]133;D;0\007\033]133;A\007$ cmd%d\r\n\033]133;C\007o%d\r\n", i, i);
        feed(t, buffer);
    }
    feed(t, "\033]133;D;0\007\033]133;A\007$ "); // bare prompt at the bottom
    int hist = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(hist >= 4);
    // Eat three commands; each is 2 rows, so history must drop by 2 each time and
    // the prompt must stay on the bottom row.
    for (int eat = 0; eat < 3; ++eat) {
        const int before = Ubiquitty::Screen::scrollbackCount(&t);
        CHECK(cmdL(t) >= 0);
        const int after = Ubiquitty::Screen::scrollbackCount(&t);
        printf("test_cmdL_scrollback_shrinks: eat %d  hist %d -> %d\n", eat, before, after);
        CHECK(after == before - 2); // history shrank by the eaten command's height
        CHECK(rowIs(t, 5, "$ ")); // prompt stayed pinned to the bottom row
    }
    printf("test_cmdL_scrollback_shrinks_each_eat done\n");
}

TEST(testMultilinePrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    // info-line\n  "> cmd"  (A fires before the info line prints)
    feed(t, "\033]133;D;0\007\033]133;A\007[user@host]\r\n> echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007[user@host]\r\n> echo two\r\n\033]133;C\007two\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007[user@host]\r\n> "); // bare 2-line prompt
    // Marks land on the info line of each prompt: rows 0 and 3.
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK((Ubiquitty::Screen::rowMark(&t, 3) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    const int prev = cmdL(t);
    printf("testMultilinePrompt: prev=%d (expect 3)\n", prev);
    CHECK(prev == 3); // info line of the eaten "echo two" block
    CHECK(rowIs(t, 0, "[user@host]")); // echo one's block kept
    CHECK(rowIs(t, 1, "> echo one"));
    CHECK(rowIs(t, 2, "one"));
    CHECK(rowIs(t, 3, "[user@host]")); // the current 2-line prompt rode up here
    CHECK(rowIs(t, 4, "> "));
    CHECK((Ubiquitty::Screen::rowMark(&t, 3) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0); // bare prompt
    printf("testMultilinePrompt done\n");
}

TEST(test_cmdK_resets_row_meta)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 20);
    // Row 0: executed command (mark). Row 2: DECDWL banner. Row 3: a 26-char
    // line that soft-wraps into row 4 on a 20-column screen. Bare prompt last.
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033#6BANNER\r\n");
    feed(t, "abcdefghijklmnopqrstuvwxyz\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ ");
    // Sanity: the metadata is in place before the clear.
    CHECK((Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted) != 0);
    CHECK(t.screens.active->lineAttribute[2] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineWrapped[3]);
    Ubiquitty::clearBufferPreserveInput(&t);
    CHECK(rowIs(t, 0, "$ ")); // input row preserved
    for (int r = 0; r < 12; ++r) {
        if (t.screens.active->lineAttribute[r] != Ubiquitty::lineAttributeNormal)
            printf("  stale lineAttribute on row %d\n", r);
        CHECK(t.screens.active->lineAttribute[r] == Ubiquitty::lineAttributeNormal);
        CHECK(!t.screens.active->lineWrapped[r]);
        // No executed-command bracket anywhere on a freshly cleared screen;
        // rows past the preserved input line carry no mark at all.
        CHECK((Ubiquitty::Screen::rowMark(&t, r) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
        if (r > 0)
            CHECK(Ubiquitty::Screen::rowMark(&t, r) == 0);
    }
    printf("test_cmdK_resets_row_meta done\n");
}

TEST(test_clearBuffer_resets_row_meta)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 20);
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033#6BANNER\r\n");
    feed(t, "abcdefghijklmnopqrstuvwxyz\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ "); // A fired, C pending
    Ubiquitty::clearBuffer(&t);
    for (int r = 0; r < 12; ++r) {
        CHECK(t.screens.active->lineAttribute[r] == Ubiquitty::lineAttributeNormal);
        CHECK(!t.screens.active->lineWrapped[r]);
        CHECK(Ubiquitty::Screen::rowMark(&t, r) == 0);
    }
    // The pending prompt died with the screen — a C arriving now must not
    // resurrect a bracket on whatever row the old prompt occupied.
    feed(t, "\033]133;C\007");
    for (int r = 0; r < 12; ++r)
        CHECK((Ubiquitty::Screen::rowMark(&t, r) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
    printf("test_clearBuffer_resets_row_meta done\n");
}

TEST(test_eraseDisplay_clears_marks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo two\r\n\033]133;C\007two\r\n");
    // Sanity: two executed-command rows would draw brackets before the clear.
    int marked = 0;
    for (int r = 0; r < 12; ++r)
        if (Ubiquitty::Screen::rowMark(&t, r) & Ubiquitty::Screen::osc133MarkCmdExecuted)
            ++marked;
    CHECK(marked == 2);
    // The `clear` command: Erase CellGrid 2 (whole screen) + cursor home.
    feed(t, "\033[2J\033[H");
    for (int r = 0; r < 12; ++r)
        CHECK(Ubiquitty::Screen::rowMark(&t, r) == 0);
    printf("test_eraseDisplay_clears_marks done\n");
}

TEST(test_run_after_cmdL)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    CHECK(cmdL(t) == 4); // eat "$ false"; bare prompt rides up to row 4
    // Run a new command from the bare prompt at row 4.
    feed(t, "echo three\r\n\033]133;C\007three\r\n\033]133;D;0\007\033]133;A\007$ ");
    const int prev = cmdL(t);
    printf("test_run_after_cmdL: prev=%d (expect 4)\n", prev);
    CHECK(prev == 4); // "$ echo three" was at row 4; it gets eaten
    CHECK(rowIs(t, 0, "$ echo one")); // older commands still present
    CHECK(rowIs(t, 2, "$ echo two"));
    CHECK(rowIs(t, 4, "$ ")); // bare prompt back at row 4, "echo three" gone
    CHECK((Ubiquitty::Screen::rowMark(&t, 4) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
    printf("test_run_after_cmdL done\n");
}

UBIQUITTY_TEST_MAIN()
