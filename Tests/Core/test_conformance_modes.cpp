// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::cell;
using UbiquittyTest::feed;

// test_conformance_modes — DECSET/DECRST pins for the mode layer
// (Source/Core/VT/Modes.cpp): the alternate-screen modes 47 / 1047 / 1049 / 1046
// (https://invisible-island.net/xterm/ctlseqs/ctlseqs.html), DECNKM (?66) and
// DECNCSM (?95) (https://vt100.net/docs/).

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>

int runModesConformanceTests();

// Leave the ALTERNATE buffer's own stored cursor far from the primary's, so a
// later "the cursor did not move" check cannot pass by coincidence.
static void poisonAlternateCursor(Ubiquitty::Terminal& t)
{
    feed(t, "\033[?47h\033[12;40H\033[?47l");
    CHECK(t.screens.alternate.cursorX == 39);
    CHECK(t.screens.alternate.cursorY == 11);
}

// The cursor is terminal state, not per-buffer state: a 47 flip must not move it
// in either direction. 47 also never clears the alternate buffer.
static void testAltbuf47DoesNotMoveCursor()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    poisonAlternateCursor(t);

    feed(t, "\033[1;1Habc\r\ndef");
    const int x = t.screens.active->cursorX, y = t.screens.active->cursorY;
    CHECK(x == 3);
    CHECK(y == 1);

    feed(t, "\033[?47h");
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == x);
    CHECK(t.screens.active->cursorY == y);

    feed(t, "\033[5;5Hz");
    feed(t, "\033[?47l");
    CHECK(!t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == 5);
    CHECK(t.screens.active->cursorY == 4);
    // The primary's own contents are intact underneath.
    CHECK(cell(t, 0, 0)->codePoint == 'a');

    // 47 never clears: re-entering shows the alternate's prior content.
    feed(t, "\033[?47h");
    CHECK(cell(t, 4, 4)->codePoint == 'z');
}

// Same cursor invariant as 47; DECRST 1047 additionally clears the alternate
// buffer before returning, so a re-entry shows a clean screen.
static void testAltbuf1047DoesNotMoveCursor()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    poisonAlternateCursor(t);

    feed(t, "\033[1;1Habc\r\ndef");
    const int x = t.screens.active->cursorX, y = t.screens.active->cursorY;

    feed(t, "\033[?1047h");
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == x);
    CHECK(t.screens.active->cursorY == y);

    feed(t, "\033[5;5Hz");
    feed(t, "\033[?1047l");
    CHECK(!t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == 5);
    CHECK(t.screens.active->cursorY == 4);

    feed(t, "\033[?1047h");
    CHECK(cell(t, 4, 4)->codePoint != 'z'); // cleared on exit
}

// The deferred-wrap latch is terminal state too, so a buffer flip must not
// swallow a pending wrap: the next glyph still wraps.
static void testAltbuf47CarriesDeferredWrap()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[1;80HX"); // arms wrapPending at the right margin
    CHECK(t.screens.active->wrapPending);
    feed(t, "\033[?47h");
    CHECK(t.screens.active->wrapPending);
    feed(t, "Y");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'Y');
}

// DECRST 1046 while on the alternate screen forces back to primary. That is a
// buffer flip like any other, so it obeys the same cursor invariant.
static void testAltbuf1046ForcedExitKeepsCursor()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?47h\033[7;9H");
    CHECK(t.screens.isUsingAlternateScreen);
    feed(t, "\033[?1046l");
    CHECK(!t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == 8);
    CHECK(t.screens.active->cursorY == 6);
}

// 1049 saves the cursor on entry and restores it on exit, and clears the
// alternate screen without moving the cursor, as xterm does.
static void testAltbuf1049StillSavesAndRestoresCursor()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[3;7H");
    feed(t, "\033[?1049h");
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == 6);
    CHECK(t.screens.active->cursorY == 2);
    feed(t, "\033[10;10H");
    feed(t, "\033[?1049l");
    CHECK(!t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active->cursorX == 6);
    CHECK(t.screens.active->cursorY == 2);
}

// CSI ?66 h/l and ESC = / ESC > select the same application-keypad state, in
// both directions.
static void testDecnkm66IsTheApplicationKeypadState()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!t.modes.appKeypad);

    feed(t, "\033[?66h");
    CHECK(t.modes.appKeypad);
    feed(t, "\033[?66l");
    CHECK(!t.modes.appKeypad);

    // ESC = (DECKPAM) must be visible through ?66, and ?66 l must undo it.
    feed(t, "\033=");
    CHECK(t.modes.appKeypad);
    feed(t, "\033[?66l");
    CHECK(!t.modes.appKeypad);

    // ...and the reverse: ?66 h must be undone by ESC > (DECKPNM).
    feed(t, "\033[?66h");
    CHECK(t.modes.appKeypad);
    feed(t, "\033>");
    CHECK(!t.modes.appKeypad);
}

// DECNCSM (?95) is a VT500 feature: below that operating level a DECCOLM column
// change erases the screen whatever the flag says.
static void testDECNCSMHonouredOnlyAtVT500()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[64;1\"p"); // DECSCL — VT400 operating level
    feed(t, "\033[?40h"); // permit the 80↔132 switch
    feed(t, "\033[?95h"); // DECNCSM set, but inert below VT500
    feed(t, "\033[?3l");
    feed(t, "\033[1;1H1");
    CHECK(cell(t, 0, 0)->codePoint == '1');
    feed(t, "\033[?3h");
    CHECK(t.columns == 132);
    CHECK(cell(t, 0, 0)->codePoint == 0); // erased despite DECNCSM

    feed(t, "\033[65;1\"p"); // DECSCL — VT500 operating level
    feed(t, "\033[?40h");
    feed(t, "\033[?95h");
    feed(t, "\033[?3l");
    feed(t, "\033[1;1HX");
    feed(t, "\033[?3h");
    CHECK(t.columns == 132);
    CHECK(cell(t, 0, 0)->codePoint == 'X'); // DECNCSM honoured at VT500
}

// The default operating level is VT500, so an unconfigured session keeps
// honouring DECNCSM — the level gate must not have moved the default.
static void testDECNCSMDefaultLevelStillKeepsScreen()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?40hAB");
    feed(t, "\033[?3h");
    CHECK(cell(t, 0, 0)->codePoint == 0);
    feed(t, "\033[?95h");
    feed(t, "\033[1;1HCD");
    feed(t, "\033[?3l");
    CHECK(t.columns == 80);
    CHECK(cell(t, 0, 0)->codePoint == 'C');
    CHECK(cell(t, 0, 1)->codePoint == 'D');
}

int runModesConformanceTests()
{
    testAltbuf47DoesNotMoveCursor();
    testAltbuf1047DoesNotMoveCursor();
    testAltbuf47CarriesDeferredWrap();
    testAltbuf1046ForcedExitKeepsCursor();
    testAltbuf1049StillSavesAndRestoresCursor();
    testDecnkm66IsTheApplicationKeypadState();
    testDECNCSMHonouredOnlyAtVT500();
    testDECNCSMDefaultLevelStillKeepsScreen();

    if (failures == 0)
        printf("test_conformance_modes: all passed\n");
    else
        printf("test_conformance_modes: %d FAILED\n", failures);
    return failures;
}

// Defined out when several cluster conformance files are linked into one
// binary; that driver supplies main and calls the run*Tests entry points.
#ifndef UBIQUITTY_CONFORMANCE_TESTS_COMBINED
int main() { return runModesConformanceTests() == 0 ? 0 : 1; }
#endif
