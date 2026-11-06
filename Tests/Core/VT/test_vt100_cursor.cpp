// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Where the cursor is, and every control that moves it.
//
// Absolute and relative motion, tab stops, the scrolling region and the
// horizontal margins that bound it, origin mode, save and restore, and the
// index family that moves and scrolls in one step.
//
// Motion is where off-by-one errors hide, because a cursor one column wrong
// produces correct-looking output until something is written. The cases sweep
// the edges rather than the middle: column zero, the last column, the margin boundary,
// and the parameter defaults that a program omits far more often than it
// supplies.

#include "Core/Screen/Addressing.h"
#include "Core/VT/Internal.h"
#include "Harness.h"
#include "VT100Fixture.h"

TEST(testCUUCUDRespectDECSTBMMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // DECSTBM region rows 5..15 (1-based 6..16), origin mode OFF (default).
    feed(t, "\033[6;16r");
    // Inside the region (row 10): CUU 999 stops at the top margin (row 5).
    feed(t, "\033[11;1H\033[999A");
    CHECK(t.screens.active->cursorY == 5);
    // Inside again: CUD 999 stops at the bottom margin (row 15).
    feed(t, "\033[11;1H\033[999B");
    CHECK(t.screens.active->cursorY == 15);
    // Above the region (row 2): CUU 999 reaches the screen top (margin N/A).
    feed(t, "\033[3;1H\033[999A");
    CHECK(t.screens.active->cursorY == 0);
}

TEST(test_modifyCursorKeys_set_and_query)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // Set: modifyCursorKeys = 2 (category 1, value 2).
    feed(t, "\033[>1;2m");
    CHECK(t.xtermKeyModifiers[1] == 2);
    // Query via CSI ? 1 m → reply ESC [ > 1 ; 2 m
    feed(t, "\033[?1m");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033[>1;2m") != nullptr);
}

TEST(testDECSLRMInitialState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Default: DECLRMM off, scrollLeft=0, scrollRight=columns-1.
    CHECK(t.modes.leftRightMarginMode == false);
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
}

TEST(testDECSLRMRequiresDecset69)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Without DECLRMM, CSI Pl ; Pr s is SCOSC (save cursor). The
    // margins should not change.
    feed(t, "\033[5;15s");
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
    // Enable DECLRMM then try again.
    feed(t, "\033[?69h");
    CHECK(t.modes.leftRightMarginMode == true);
    feed(t, "\033[5;15s");
    CHECK(t.screens.active->scrollLeft == 4); // 1-based 5 → 0-based 4
    CHECK(t.screens.active->scrollRight == 14); // 1-based 15 → 0-based 14
}

TEST(testDECSLRMResetsOnDecrst69)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[10;30s");
    CHECK(t.screens.active->scrollLeft == 9);
    CHECK(t.screens.active->scrollRight == 29);
    feed(t, "\033[?69l");
    CHECK(t.modes.leftRightMarginMode == false);
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
}

TEST(testICHDCHRespectDECSLRMRightMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 10);
    feed(t, "ABCDEFGHIJ"); // row 0 = A..J across all ten columns
    feed(t, "\033[?69h\033[3;8s"); // DECLRMM on; DECSLRM left=3 right=8 (0-based 2..7)
    feed(t, "\033[1;4H"); // cursor → col 4 (1-based) = col 3 (0-based), inside
    feed(t, "\033[@"); // ICH 1: insert one blank
    CHECK(cell(t, 0, 2)->codePoint == 'C'); // left of the cursor: untouched
    CHECK(cell(t, 0, 4)->codePoint == 'D'); // shifted right by one within the margin
    CHECK(cell(t, 0, 7)->codePoint == 'G'); // last cell inside the right margin
    CHECK(cell(t, 0, 8)->codePoint == 'I'); // past the margin: UNTOUCHED ('H' if the shift ignores it)
    CHECK(cell(t, 0, 9)->codePoint == 'J'); // untouched
    feed(t, "\033[P"); // DCH 1 at the same cursor: pulls within the margin
    CHECK(cell(t, 0, 3)->codePoint == 'D'); // margin content pulled left by one
    CHECK(cell(t, 0, 8)->codePoint == 'I'); // still untouched past the margin
}

TEST(testDECSLRMClampsInvalidRegion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h");
    // Degenerate: left > right → reset to full screen per spec.
    feed(t, "\033[20;10s");
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
}

TEST(testDECSLRMCursorHomeAfterSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h");
    // Place cursor far away before DECSLRM.
    feed(t, "\033[10;40H");
    // DECSLRM should reset cursor to home (0,0) with DECOM off.
    feed(t, "\033[5;20s");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);
}

TEST(testCrHonorsLeftMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[10;60s"); // DECLRMM + left margin col 10 (0-based 9)
    feed(t, "\033[1;30H\r"); // cursor col 30 (>= margin) then CR
    CHECK(t.screens.active->cursorX == 9); // left margin, not 0
    feed(t, "\033[1;5H\r"); // cursor col 5 (left of margin) then CR
    CHECK(t.screens.active->cursorX == 0); // column 0
}

TEST(testNELHonorsLeftMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[10;60s");
    feed(t, "\033[3;30H\033E"); // row 3 col 30, then NEL
    CHECK(t.screens.active->cursorX == 9); // left margin
    CHECK(t.screens.active->cursorY == 3); // advanced one row
}

TEST(testBareCSISUnderDECLRMMResetsMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[10;60s");
    CHECK(t.screens.active->scrollLeft == 9);
    CHECK(t.screens.active->scrollRight == 59);
    feed(t, "\033[5;5H\033[s"); // move cursor, then bare CSI s
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
}

TEST(testDECSLRMCUFClipsToRightMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[5;15s");
    // CUF beyond scrollRight should clip to column 14 (0-based 15-1).
    feed(t, "\033[100C");
    CHECK(t.screens.active->cursorX == 14);
}

TEST(testDECSLRMCUBClipsToLeftMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[5;15s");
    // Move to column 14 then CUB 100 should clip to scrollLeft=4.
    feed(t, "\033[1;15H"); // column 15 = 0-based 14
    feed(t, "\033[100D");
    CHECK(t.screens.active->cursorX == 4);
}

TEST(testDECSLRMAutowrapAtRightMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[1;6s"); // columns 1..6 (0-based 0..5)
    // Write 7 chars; the 6th should wrap.
    feed(t, "ABCDEFG");
    // A..F land in row 0; G wraps to row 1 column 0 (leftMargin).
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 5)->codePoint == 'F');
    CHECK(cell(t, 1, 0)->codePoint == 'G');
}

TEST(testDECSLRMAutowrapLandsAtLeftMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[3;6s"); // columns 3..6 = 0-based 2..5
    // Move to column 2 (scrollLeft), write 5 chars — the 5th wraps to
    // row 1 column 2 (not column 0).
    feed(t, "\033[1;3HABCDE");
    CHECK(cell(t, 0, 2)->codePoint == 'A');
    CHECK(cell(t, 0, 5)->codePoint == 'D');
    CHECK(cell(t, 1, 2)->codePoint == 'E');
}

TEST(testDECSLRMSuPreservesOutsideCols)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    // Paint a sentinel column at column 0 and column 19 across all rows.
    feed(t, "\033[H");
    for (int r = 0; r < 10; r++) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\033[%d;1HL\033[%d;20HR", r + 1, r + 1);
        feed(t, buffer);
    }
    // Set margins to columns 5..15 and SU 1 — sentinels must survive.
    feed(t, "\033[?69h\033[6;16s");
    feed(t, "\033[3S");
    for (int r = 0; r < 10; r++) {
        CHECK(cell(t, r, 0)->codePoint == 'L');
        CHECK(cell(t, r, 19)->codePoint == 'R');
    }
}

TEST(testDECSLRMSdPreservesOutsideCols)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    for (int r = 0; r < 10; r++) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\033[%d;1HL\033[%d;20HR", r + 1, r + 1);
        feed(t, buffer);
    }
    feed(t, "\033[?69h\033[6;16s");
    feed(t, "\033[2T");
    for (int r = 0; r < 10; r++) {
        CHECK(cell(t, r, 0)->codePoint == 'L');
        CHECK(cell(t, r, 19)->codePoint == 'R');
    }
}

TEST(testDECSLRMIlNoopWhenCursorOutsideMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "\033[?69h\033[10;30s");
    // Cursor at column 0 — OUTSIDE [9..29]. IL must be a no-op.
    feed(t, "\033[1;1H");
    feed(t, "AAAAAAAA"); // write 8 chars to row 0
    feed(t, "\033[1;1H\033[5L"); // IL 5 — cursor outside, no-op
    CHECK(cell(t, 0, 0)->codePoint == 'A'); // row 0 unchanged
    CHECK(cell(t, 0, 7)->codePoint == 'A');
}

TEST(testDECSLRMHtClampsToRightMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[1;10s");
    feed(t, "\033[1;1H");
    feed(t, "\t\t\t\t\t");
    // Default tab stops at columns 8/16/24/...; the first stop inside
    // [0..9] is column 8. Subsequent tabs should clip to column 9 (right
    // margin), not jump to column 16.
    CHECK(t.screens.active->cursorX == 9);
}

TEST(testDECSLRMBsClampsToLeftMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[5;15s");
    feed(t, "\033[1;5H"); // column 5 = scrollLeft (0-based 4)
    feed(t, "\b\b\b");
    CHECK(t.screens.active->cursorX == 4);
}

TEST(testDECSLRMCHTCBTRespectMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[1;20s");
    feed(t, "\033[1;1H\033[100I"); // CHT 100 — clamps to column 19
    CHECK(t.screens.active->cursorX == 19);
    feed(t, "\033[100Z"); // CBT 100 — clamps to column 0 (leftMargin)
    CHECK(t.screens.active->cursorX == 0);
}

TEST(testDECFIDECBIIndexAndPan)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDEF\r\nGHIJKL");
    // Mid-row: DECFI is just cursor-right; DECBI cursor-left.
    feed(t, "\033[1;3H\0339");
    CHECK(t.screens.active->cursorX == 3);
    CHECK(cell(t, 0, 0)->codePoint == 'A'); // no pan
    feed(t, "\0336");
    CHECK(t.screens.active->cursorX == 2);

    // At the right margin: DECFI pans the region left one column.
    char lastColumn[16];
    std::snprintf(lastColumn, sizeof(lastColumn), "\033[1;%dH", t.columns);
    feed(t, lastColumn);
    feed(t, "\0339");
    CHECK(t.screens.active->cursorX == t.columns - 1); // cursor stays at the margin
    CHECK(cell(t, 0, 0)->codePoint == 'B'); // row 1 panned left
    CHECK(cell(t, 1, 0)->codePoint == 'H'); // row 2 panned left too

    // At the left margin (column 1): DECBI pans the region right one column.
    feed(t, "\033[1;1H\0336");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0); // blank revealed at the left margin
    CHECK(cell(t, 0, 1)->codePoint == 'B'); // content panned back right
    CHECK(cell(t, 1, 1)->codePoint == 'H');
}

TEST(testHorizontalIndexAtPageEdgesOutsideMarginsIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5);
    feed(t, "ABCDE\r\nFGHIJ\033[?69h\033[2;4s");
    feed(t, "\033[1;5H\0339");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    CHECK(cell(t, 1, 0)->codePoint == 'F');
    CHECK(cell(t, 1, 4)->codePoint == 'J');
    feed(t, "\033[1;1H\0336");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    CHECK(cell(t, 1, 0)->codePoint == 'F');
    CHECK(cell(t, 1, 4)->codePoint == 'J');
}

TEST(testDECSCDECRCSavesGr)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033-A"); // Latin-1 → G1
    feed(t, "\033~"); // LS1R: GR = 1
    CHECK(t.characterSetGR == 1);
    feed(t, "\0337"); // DECSC
    feed(t, "\033}"); // LS2R: GR = 2
    CHECK(t.characterSetGR == 2);
    feed(t, "\0338"); // DECRC: GR restored to 1
    CHECK(t.characterSetGR == 1);
}

TEST(testDECSCPreservesWrapPending)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5); // 2 rows x 5 cols
    feed(t, "ABCDE"); // 'E' at the last column arms the wrap
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "\0337\0338"); // DECSC then DECRC
    CHECK(t.screens.active->wrapPending == true); // the latch survives the round-trip
    feed(t, "F"); // wraps to row 1, col 0
    CHECK(cell(t, 1, 0)->codePoint == 'F');
    CHECK(cell(t, 0, 4)->codePoint == 'E'); // last cell not overwritten
}

TEST(testDECUDKPayloadInSessionLog)
{
    Ubiquitty::Terminal t;
    char logPath[256];
    Ubiquitty::hardReset(t);
    snprintf(logPath, sizeof(logPath), "/tmp/ubiquitty_decudk_test_%d.log", (int)(size_t)&t);
    CHECK(Ubiquitty::setSessionLogPath(&t, logPath));
    /// DECUDK: DCS Pid;Pmod | <payload> ST. Use 1|17/foo for keypress.
    const char* wire = "\033P1;1|17/666f6f\033\\";
    feed(t, wire);
    /// Close the log so contents flush.
    Ubiquitty::setSessionLogPath(&t, nullptr);
    FILE* f = fopen(logPath, "rb");
    CHECK(f != nullptr);
    char buffer[256];
    size_t n = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);
    remove(logPath);
    CHECK(n == strlen(wire));
    CHECK(memcmp(buffer, wire, n) == 0);
}

TEST(testCursorNextChangeMatchesResolver)
{
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    term.windowFocused = true;
    term.lastActivityTick = 0;
    term.cursorStyle = Ubiquitty::Cursor::Style::BlinkingBlock;
    term.cursorBlinkPref = true;

    const Ubiquitty::Cursor::Blink::Animation originalStyle = Ubiquitty::Cursor::Blink::animation();
    for (Ubiquitty::Cursor::Blink::Animation style : { Ubiquitty::Cursor::Blink::Animation::Fade, Ubiquitty::Cursor::Blink::Animation::OnOff }) {
        Ubiquitty::Cursor::Blink::setAnimation(style);
        for (uint32_t tick = 0; tick < Ubiquitty::Cursor::Blink::caretPeriodTicks; ++tick) {
            const float alpha = Ubiquitty::Cursor::resolve(&term, tick).blinkAlpha;
            const uint32_t delay = Ubiquitty::Cursor::ticksUntilAlphaChange(&term, tick);
            CHECK(delay > 0);
            for (uint32_t before = 1; before < delay; ++before) {
                CHECK(Ubiquitty::Cursor::resolve(&term, tick + before).blinkAlpha == alpha);
            }
            CHECK(Ubiquitty::Cursor::resolve(&term, tick + delay).blinkAlpha != alpha);
        }
    }

    Ubiquitty::Cursor::Blink::setAnimation(Ubiquitty::Cursor::Blink::Animation::Fade);
    term.lastActivityTick = 100;
    term.cursorHoldFrames = 10;
    CHECK(Ubiquitty::Cursor::ticksUntilAlphaChange(&term, 105) == 5);
    term.windowFocused = false;
    CHECK(Ubiquitty::Cursor::ticksUntilAlphaChange(&term, 105) == 0);
    Ubiquitty::Cursor::Blink::setAnimation(originalStyle);
}

TEST(testFreshTerminalHoldsSolidBeforeItsFirstBlink)
{
    // A terminal that has seen no activity phases its blink from tick 0 the
    // way one that just saw activity does: the hold, then the fade. The
    // snapshot arms pin their clock at tick 0, so this is what makes a
    // capture through the live session draw the same solid block the
    // clockless capture path pins by construction.
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    term.windowFocused = true;
    term.lastActivityTick = 0;
    term.cursorStyle = Ubiquitty::Cursor::Style::BlinkingBlock;
    term.cursorBlinkPref = true;
    term.cursorHoldFrames = 10;
    const Ubiquitty::Cursor::Blink::Animation originalStyle = Ubiquitty::Cursor::Blink::animation();
    Ubiquitty::Cursor::Blink::setAnimation(Ubiquitty::Cursor::Blink::Animation::Fade);
    CHECK(Ubiquitty::Cursor::resolve(&term, 0).blinkAlpha == 1.0f);
    CHECK(Ubiquitty::Cursor::resolve(&term, 9).blinkAlpha == 1.0f);
    CHECK(Ubiquitty::Cursor::resolve(&term, 10).blinkAlpha == 0.75f);
    CHECK(Ubiquitty::Cursor::ticksUntilAlphaChange(&term, 0) == 10);
    Ubiquitty::Cursor::Blink::setAnimation(originalStyle);
}

TEST(testCursorOnlyMoveDoesNotDirtyRows)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int r;
    Ubiquitty::hardReset(t);
    feed(t, "hello world");
    drainPaintCycle(t, nullptr);
    // CUP: row 5, column 10 (1-indexed). Pure cursor moves must not
    // queue any paint work.
    feed(t, "\033[5;10H");
    peekPaintTrace(t, &tr);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
    CHECK(tr.pendingScrolls == 0);
    CHECK(!t.grid->isFullRedrawRequested());
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 9);
    // CUF / CUB / CUU / CUD must also stay clean.
    drainPaintCycle(t, nullptr);
    feed(t, "\033[3C");
    peekPaintTrace(t, &tr);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
    drainPaintCycle(t, nullptr);
    feed(t, "\033[2A");
    peekPaintTrace(t, &tr);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
}

TEST(testLsEnterAdvancesCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ls\r\n");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 1);
}

TEST(testBackspaceThenTypingDoesNotDoubleCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "abc");
    CHECK(t.screens.active->cursorX == 3);
    feed(t, "\b\b");
    CHECK(t.screens.active->cursorX == 1);
    feed(t, "X");
    CHECK(t.screens.active->cursorX == 2);
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    CHECK(cell(t, 0, 1)->codePoint == 'X');
    CHECK(cell(t, 0, 2)->codePoint == 'c');
}

TEST(testCrLfAtTopDoesNotDirtyCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    drainPaintCycle(t, nullptr);
    CHECK(!t.grid->hasUnpaintedWork());
    // CR by itself: cursorX = 0, no cell mutation.
    feed(t, "\r");
    CHECK(!t.grid->hasUnpaintedWork());
    // LF at non-bottom row: just cursorY++, no scroll, no cells.
    feed(t, "\n");
    CHECK(!t.grid->hasUnpaintedWork());
    // CR LF together — also no cell mutation.
    drainPaintCycle(t, nullptr);
    feed(t, "\r\n");
    CHECK(!t.grid->hasUnpaintedWork());
}

TEST(testDecst8cRestoresEvery8Stops)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Wipe all stops, then ask for the default grid back.
    feed(t, "\033[3g");
    for (int i = 0; i < t.columns; i++)
        CHECK(!t.tabStops[i]);
    feed(t, "\033[?5W");
    CHECK(!t.tabStops[0]);
    CHECK(t.tabStops[8]);
    CHECK(t.tabStops[16]);
    CHECK(t.tabStops[24]);
    CHECK(!t.tabStops[7]);
    CHECK(!t.tabStops[9]);
}

TEST(testDECSTBMIgnoresInvalidMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;15r"); // valid region rows 5..15 (0-based 4..14); homes cursor
    feed(t, "\033[10;10H"); // move cursor to row 10, col 10 (0-based 9,9)

    // Inverted margins → ignored: region + cursor unchanged.
    feed(t, "\033[12;3r");
    CHECK(t.screens.active->scrollTop == 4);
    CHECK(t.screens.active->scrollBottom == 14);
    CHECK(t.screens.active->cursorY == 9); // Not homed
    CHECK(t.screens.active->cursorX == 9);

    // Equal margins (single line) → also ignored.
    feed(t, "\033[8;8r");
    CHECK(t.screens.active->scrollTop == 4);
    CHECK(t.screens.active->scrollBottom == 14);

    // A bottom margin past the last line → ignored too (DEC STD 070), not
    // taken as the last line.
    feed(t, "\033[2;25r");
    CHECK(t.screens.active->scrollTop == 4);
    CHECK(t.screens.active->scrollBottom == 14);
    CHECK(t.screens.active->cursorY == 9);

    // A valid no-parameter CSI r resets to full page and homes.
    feed(t, "\033[r");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);
}

TEST(testLfWithinDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[4;8r"); // scrollTop=3, scrollBottom=7
    CHECK(t.screens.active->scrollTop == 3);
    CHECK(t.screens.active->scrollBottom == 7);

    // Paint markers on every row to detect spurious scrolls.
    feed(t, "\033[1;1H");
    feed(t, "ROW0\033[2;1HROW1\033[3;1HROW2\033[4;1HROW3");
    feed(t, "\033[5;1HROW4\033[6;1HROW5\033[7;1HROW6\033[8;1HROW7");
    feed(t, "\033[9;1HROW8");

    // Move cursor to the region's bottom (row 8 1-based → index 7).
    feed(t, "\033[8;1H");
    CHECK(t.screens.active->cursorY == 7);

    // Bare LF at the region bottom must scroll [3,7] up by one.
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 7);
    // Row 3 (region top) gets row 4's content.
    CHECK(cell(t, 3, 0)->codePoint == 'R');
    CHECK(cell(t, 3, 3)->codePoint == '4');
    // Region bottom is now blank.
    CHECK(cell(t, 7, 0)->codePoint == 0);
    // Rows above and below the region are untouched.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == '0');
    CHECK(cell(t, 1, 3)->codePoint == '1');
    CHECK(cell(t, 2, 3)->codePoint == '2');
    CHECK(cell(t, 8, 3)->codePoint == '8');
}

TEST(testIndWithinDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[4;8r");
    feed(t, "\033[1;1HABOVE");
    feed(t, "\033[4;1HTOP");
    feed(t, "\033[8;1HBOT");
    feed(t, "\033[9;1HBELOW");
    feed(t, "\033[8;1H");
    CHECK(t.screens.active->cursorY == 7);

    // IND at bottom of region → scroll region up by 1.
    feed(t, "\033D");
    CHECK(t.screens.active->cursorY == 7);
    // "TOP" should have scrolled out; region bottom is blank.
    CHECK(cell(t, 7, 0)->codePoint == 0);
    // Above + below the region untouched.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 8, 0)->codePoint == 'B');
}

TEST(testSaveRestoreCarriesDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[4;8r"); // scrollTop=3, scrollBottom=7
    CHECK(t.screens.active->scrollTop == 3);
    CHECK(t.screens.active->scrollBottom == 7);

    // DECSC saves cursor + pen + origin/characterSet state.
    feed(t, "\033[5;10H");
    feed(t, "\033[31m");
    feed(t, "\0337");
    CHECK(t.screens.active->hasSavedCursor);
    CHECK(t.screens.active->savedX == 9);
    CHECK(t.screens.active->savedY == 4);

    /// Resize must reclamp the (live) scroll region into the new bounds.
    Ubiquitty::resize(t, 10, 60);
    CHECK(t.screens.active->scrollTop >= 0);
    CHECK(t.screens.active->scrollBottom < 10);
    CHECK(t.screens.active->scrollTop < t.screens.active->scrollBottom);

    // DECRC restores cursor + pen; margins are untouched (not part of the
    // saved-cursor record), so they're whatever the resize-reclamp left.
    feed(t, "\0338");
    CHECK(t.screens.active->cursorY >= 0);
    CHECK(t.screens.active->cursorY < 10);
    CHECK(t.screens.active->pen.foregroundIndex == 1);
}

TEST(testTBCCSIGNoParameterDefaultsZero)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // column 8 has a default stop. Park the cursor on it and feed CSI g
    // (no parameters).
    CHECK(t.tabStops[8]);
    feed(t, "\033[9G"); // 1-based: column 9 → cursorX=8
    feed(t, "\033[g");
    CHECK(!t.tabStops[8]);
    // Neighbouring stops are untouched.
    CHECK(t.tabStops[16]);
}

TEST(testTabulationClearKeepsLineAndCharacterStopsIndependent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[3;9H\033J\033[5;1H\033J\033[3;9H");
    feed(t, "\033[1g");
    CHECK(t.tabStops[8]);
    CHECK(!t.lineTabStops[2]);
    CHECK(t.lineTabStops[4]);
    feed(t, "\033[2g");
    CHECK(!t.tabStops[8]);
    CHECK(t.lineTabStops[4]);
    feed(t, "\033H\033[4g");
    CHECK(t.tabStops[8]);
    CHECK(t.lineTabStops.none());
    feed(t, "\033J\033[3g");
    CHECK(t.lineTabStops[2]);
    for (int column = 0; column < t.columns; ++column)
        CHECK(!t.tabStops[static_cast<std::size_t>(column)]);
}

TEST(testDecst8cCol0HasNoStop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[3g"); // clear all
    feed(t, "\033[?5W"); // DECST8C
    CHECK(!t.tabStops[0]);
    CHECK(t.tabStops[8]);
}

/// With reverse-wrap off, a backspace at column 0 stays put. With it on,
/// the cursor wraps to the right margin of the previous row.
TEST(testReverseWrapBackspaceBehavior)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Cursor at row 2, column 0.
    feed(t, "\033[3;1H");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 0);
    // Reverse-wrap OFF: BS holds column 0.
    feed(t, "\b");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 0);
    // Mode 45 alone pins here — row 1 did not wrap into row 2.
    feed(t, "\033[?45h");
    feed(t, "\b");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 0);
    // Geometric reverse-wrap (1045) ON: BS at column 0 wraps to (row-1, columns-1).
    feed(t, "\033[?1045h");
    feed(t, "\b");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == t.columns - 1);
}

/// Geometric reverse-wrap (1045) off the TOP of the scrolling region lands on its
/// bottom row, not above the region: the step is a cell offset taken modulo the
/// margin rectangle. With no DECSTBM the region is the whole page, so home wraps
/// to the last cell.
TEST(testReverseWrapAtHomeWrapsToBottom)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1045h");
    feed(t, "\033[1;1H");
    feed(t, "\b");
    CHECK(t.screens.active->cursorY == t.rows - 1);
    CHECK(t.screens.active->cursorX == t.columns - 1);
}

/// CUB (CSI D) with geometric reverse-wrap (1045) walks back across the row
/// boundary, consuming the count one cell at a time.
TEST(testReverseWrapCUBCrossesRows)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1045h");
    // Row 2, column 1 (1-based 3;2). CUB by 3 → column 0, then wrap to
    // (row 1, columns-1), then one more left.
    feed(t, "\033[3;2H");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 1);
    feed(t, "\033[3D");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == t.columns - 2);
}

/// Mode 45 goes back only onto a row that soft-wrapped into the current one,
/// and stops at the left margin of the first row of the wrapped text.
TEST(testReverseWrapReentersOnlyAWrappedRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033[?45h\033[2;1HABCDEFG"); // row 1 wraps into row 2
    feed(t, "\b\b\b\b\b\b\b\b\b\b\b\b");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\033[3;3H\033[4D"); // across the wrap into row 1
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 3);
    feed(t, "\033[1;1HAB\r\n\b"); // row 0 ended with a line break
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 0);
}

/// Cursor motion — CUP basic + default parameters.
TEST(testCUPBasic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033[5;10H");
    CHECK(t.screens.active->cursorX == 9);
    CHECK(t.screens.active->cursorY == 4);
    feed(t, "\033[5;10H\033[H");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
}

/// CUP must clamp out-of-range coordinates to the visible grid.
TEST(testCUPClampsToGrid)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[99;99H");
    CHECK(t.screens.active->cursorY == t.rows - 1);
    CHECK(t.screens.active->cursorX == t.columns - 1);
}

/// HVP (CSI f) must behave identically to CUP (CSI H).
TEST(testHVPEqualsCUP)
{
    Ubiquitty::Terminal t1, t2;
    Ubiquitty::hardReset(t1);
    Ubiquitty::hardReset(t2);
    feed(t1, "\033[3;4H");
    feed(t2, "\033[3;4f");
    CHECK(t1.screens.active->cursorX == t2.screens.active->cursorX);
    CHECK(t1.screens.active->cursorY == t2.screens.active->cursorY);
    CHECK(t2.screens.active->cursorX == 3);
    CHECK(t2.screens.active->cursorY == 2);
}

/// CUU/CUD/CUF/CUB default-parameter semantics.
TEST(testCUFCUBCUUCUDDefaultParameter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;5H");
    CHECK(t.screens.active->cursorX == 4 && t.screens.active->cursorY == 4);
    feed(t, "\033[C"); // CUF default 1
    CHECK(t.screens.active->cursorX == 5);
    feed(t, "\033[D"); // CUB default 1
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\033[A"); // CUU default 1
    CHECK(t.screens.active->cursorY == 3);
    feed(t, "\033[B"); // CUD default 1
    CHECK(t.screens.active->cursorY == 4);
}

/// Edge clamps for CUU/CUD/CUF/CUB.
TEST(testCursorMotionClampsAtEdges)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // CUF past right edge clamps to columns-1.
    feed(t, "\033[1;1H\033[999C");
    CHECK(t.screens.active->cursorX == t.columns - 1);
    // CUB from column 0 stays at 0.
    feed(t, "\033[1;1H\033[10D");
    CHECK(t.screens.active->cursorX == 0);
    // CUU from row 0 stays at 0.
    feed(t, "\033[1;1H\033[10A");
    CHECK(t.screens.active->cursorY == 0);
    // CUD past bottom clamps to rows-1.
    feed(t, "\033[1;1H\033[999B");
    CHECK(t.screens.active->cursorY == t.rows - 1);
}

/// CHA (CSI Ps G) — column absolute.
TEST(testCHAColumnAbsolute)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[10;10H\033[5G");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 9); // row unchanged
    feed(t, "\033[G");
    CHECK(t.screens.active->cursorX == 0);
}

/// HPA (CSI Ps `) — alias for CHA. xterm supports it; Ubiquitty may not.
TEST(testHPAAliasForCHA)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[10;10H\033[5`");
    CHECK_EQ(t.screens.active->cursorX, 4);
    CHECK(t.screens.active->cursorY == 9);
}

/// VPA (CSI Ps d) — row absolute.
TEST(testVPARowAbsolute)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[10;10H\033[5d");
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 9); // column unchanged
    feed(t, "\033[d");
    CHECK(t.screens.active->cursorY == 0);
}

/// CHT (CSI Ps I) — advance N tab stops (default 8-grid).
TEST(testCHTAdvancesToNextTabStop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033[I");
    CHECK(t.screens.active->cursorX == 8);
    feed(t, "\033[1;1H\033[2I");
    CHECK(t.screens.active->cursorX == 16);
}

/// CBT (CSI Ps Z) — walk back N tab stops.
TEST(testCBTWalksBackThroughTabStops)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;25H\033[Z");
    CHECK(t.screens.active->cursorX == 16);
    feed(t, "\033[1;25H\033[2Z");
    CHECK(t.screens.active->cursorX == 8);
}

/// TAB (0x09) advances cursor to the next horizontal tab stop. With
/// the default every-8 grid: column 0 -> column 8 -> column 16.
TEST(testTabAdvancesToNextStop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\t");
    CHECK(t.screens.active->cursorX == 8);
    feed(t, "\t");
    CHECK(t.screens.active->cursorX == 16);
}

/// TAB at the right margin must not advance past columns-1. With no tab
/// stop at columns-1, the search runs off the row and the handler clamps
/// back to columns-1 (current behaviour).
TEST(testTabAtLastStopClampsToRightMargin)
{
    Ubiquitty::Terminal t;
    char buffer[16];
    Ubiquitty::hardReset(t);
    // Park cursor at the last column via CHA.
    snprintf(buffer, sizeof(buffer), "\033[%dG", Ubiquitty::defaultColumns);
    feed(t, buffer);
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    feed(t, "\t");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
}

/// With every stop cleared and a single explicit stop at column 10,
/// TAB from column 0 lands on column 10, and a second TAB walks all the
/// way to columns-1 (clamped to right margin).
TEST(testTabPastExplicitStopsFallsToRightMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // CSI 3 g - clear every tab stop, then HTS at column 11 (1-based).
    feed(t, "\033[3g");
    feed(t, "\033[11G\033H");
    CHECK(t.tabStops[10]);
    feed(t, "\033[1;1H");
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\t");
    CHECK(t.screens.active->cursorX == 10);
    feed(t, "\t");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
}

/// CSI 0 I (CHT with explicit zero parameter). Ubiquitty's parameterOr collapses
/// a zero parameter to the default of 1, so this advances one stop.
/// This pins current behaviour - the spec allows treating 0 as 1.
TEST(testCHTZeroCountIsNoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    feed(t, "\033[0I");
    // Current Ubiquitty behaviour: CHT(0) -> CHT(1) -> next stop = column 8.
    // If a future change starts treating 0 literally, swap the
    // expected value to 0 and update this comment.
    CHECK(t.screens.active->cursorX == 8);
}

/// BS at column 0 must not move cursor off the row. The current row's
/// cell content is also untouched (BS is non-destructive per VT spec).
TEST(testBsAtColZeroStaysAtColZero)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\b");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    // No write happened.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

/// BS in the middle of a row moves cursor one column left without
/// erasing the cell at the old cursor column.
TEST(testBsInMiddleMovesLeft)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    CHECK(t.screens.active->cursorX == 5);
    // Cell at column 4 holds 'E'; column 5 is still blank.
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    feed(t, "\b");
    CHECK(t.screens.active->cursorX == 4);
    // BS is non-destructive: 'E' must still be there.
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    // And column 5 unchanged.
    CHECK(cell(t, 0, 5)->codePoint == 0);
}

/// BS after a write that triggered the deferred-wrap state must clear
/// wrapPending and move the cursor left by one: BS "unwraps" the pending
/// newline rather than spending it.
TEST(testBsWithPendingWrap)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    // Fill the row to the right margin so the next character would
    // have wrapped. After the final write, cursor is parked at
    // columns-1 with wrapPending = true.
    for (i = 0; i < Ubiquitty::defaultColumns; i++)
        feed(t, "X");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->wrapPending);
    feed(t, "\b");
    // wrapPending cleared, cursor moved one column left.
    CHECK(!t.screens.active->wrapPending);
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 2);
    CHECK(t.screens.active->cursorY == 0);
}

/// CR (0x0D) returns cursor to column 0 of the current row, never
/// advances a line.
TEST(testCrReturnsToColZero)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    CHECK(t.screens.active->cursorX == 5);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\r");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    // Existing row content untouched.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
}

/// LF (0x0A), VT (0x0B) and FF (0x0C) all behave as IND under the
/// default LNM=off: cursor advances one row, column unchanged.
TEST(testLfVTFfAllActAsIndex)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Park cursor at (row 5, column 5) 1-based -> indices (4,4).
    feed(t, "\033[5;5H");
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 5);
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\013");
    CHECK(t.screens.active->cursorY == 6);
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\014");
    CHECK(t.screens.active->cursorY == 7);
    CHECK(t.screens.active->cursorX == 4);
}

/// Autowrap: after writing the columns-th character at column=columns-1, the
/// cursor stays at columns-1 with wrapPending set. The next printable
/// character wraps to the next row, column 0.
TEST(testAutowrapPendingWrapState)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    // Fill exactly columns characters on row 0.
    for (i = 0; i < Ubiquitty::defaultColumns; i++)
        feed(t, "A");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->wrapPending);
    // Last cell on row 0 holds the most recent write.
    CHECK(cell(t, 0, Ubiquitty::defaultColumns - 1)->codePoint == 'A');
    // Next char triggers the deferred wrap.
    feed(t, "B");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'B');
    CHECK(!t.screens.active->wrapPending);
}

/// CR while wrapPending is true must clear the pending-wrap state so
/// that the next printable lands at column 0 of the same row (not column 1
/// as it would after a normal wrap+LF).
TEST(testCrAfterPendingWrapResets)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H");
    for (i = 0; i < Ubiquitty::defaultColumns; i++)
        feed(t, "X");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    CHECK(t.screens.active->wrapPending);
    // CR - must reset both wrapPending and cursorX.
    feed(t, "\r");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(!t.screens.active->wrapPending);
    // Next printable lands at column 0 (overwriting the X already there).
    feed(t, "Y");
    CHECK(cell(t, 0, 0)->codePoint == 'Y');
    CHECK(t.screens.active->cursorX == 1);
    CHECK(t.screens.active->cursorY == 0);
}

TEST(test_decaln_fills_screen_with_E)
{
    Ubiquitty::Terminal t;
    int r, c;
    Ubiquitty::hardReset(t);
    // Write a smattering of non-E content first; DECALN must
    // obliterate it.
    feed(t, "abc\r\n123\r\nxyz");
    feed(t, "\033#8");
    for (r = 0; r < t.rows; r++)
        for (c = 0; c < t.columns; c++)
            CHECK(cell(t, r, c)->codePoint == 'E');
}

// DECALN puts the rendition back to normal; a character printed after it
// is plain (xterm charproc.c, CASE_DECALN: resetRendition).
TEST(testDECALNResetsTheRendition)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;7m\033#8X");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(t.screens.active->pen.attributes.isEmpty());
}

// DECALN discards the margins origin mode is relative to, and with them
// origin mode (xterm charproc.c, CASE_DECALN: UIntClr ORIGIN).
TEST(testDECALNResetsOriginMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?6h\033#8\033[2;3r\033[1;1HX");
    CHECK(!t.modes.originMode);
    CHECK(cell(t, 0, 0)->codePoint == 'X');
}

// DECSC saves a pending single shift and DECRC puts it back (VT510 DECSC: any
// single shift 2 or 3 functions sent).
TEST(testDECRCRestoresAPendingSingleShift)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033*0\033N\0337");
    CHECK(t.singleShift == 2);
    feed(t, "q");
    CHECK(t.singleShift == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    feed(t, "\0338");
    CHECK(t.singleShift == 2);
    feed(t, "q");
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    feed(t, "q");
    CHECK(cell(t, 0, 1)->codePoint == 'q');
}

// At the right margin DECFI moves the data and not the cursor, so a pending
// wrap stays pending; on a double-width line it stops at the last column
// shown (xterm util.c, xtermColScroll; cursor.c, CursorForward).
TEST(testDECFIKeepsAPendingWrapAndStopsAtADoubleWidthLinesEdge)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const int last = t.columns - 1;
    feed(t, "\033[1;1H");
    for (int column = 0; column < t.columns; ++column)
        feed(t, "A");
    CHECK(t.screens.active->wrapPending);
    feed(t, "\0339X");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'X');
    CHECK(cell(t, 0, last)->codePoint == 0);

    Ubiquitty::hardReset(t);
    const int lastShown = t.columns / 2 - 1;
    char position[32];
    std::snprintf(position, sizeof(position), "\033#6\033[1;%dH\0339X", lastShown + 1);
    feed(t, position);
    CHECK(cell(t, 0, lastShown)->codePoint == 'X');
    CHECK(t.screens.active->cursorX == lastShown);
}

// HP memory lock makes the cursor's line the top margin and unlock puts it
// back on the first line; the bottom margin stays (xterm ctlseqs, ESC l and
// ESC m).
TEST(testHPMemoryLockAndUnlockMoveTheTopMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[3;1H\033l");
    CHECK(t.screens.active->scrollTop == 2);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
    CHECK(t.screens.active->cursorY == 2);
    feed(t, "\033[2;5r\033[3;1H\033m");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == 4);
}

// A line made double width or double height loses the text past its new end;
// back at single width, that text stays lost (DEC STD 070, 5.6.1).
TEST(testDoubledLineLosesItsRightHalf)
{
    for (const char* attribute : { "\033#6", "\033#3", "\033#4" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        const int half = t.columns / 2;
        char position[32];
        std::snprintf(position, sizeof(position), "A\033[1;%dHJ", t.columns);
        feed(t, position);
        feed(t, attribute);
        feed(t, "\033#5");
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(cell(t, 0, t.columns - 1)->codePoint == 0);
        CHECK(t.screens.active->cursorX == half - 1);
    }
}

/// DECSC / DECRC — cursor position round-trip.
///
/// Per VT220 spec, DECSC (ESC 7) snapshots cursor position, current SGR pen,
/// characterSet designations (G0..G3 + active/single-shift), autowrap and origin
/// mode. Ubiquitty saves all of these but autowrap, and that one omission is the
/// spec's rather than a gap: testDECSCDECRCDoesNotRestoreAutowrap and
/// testDECSCDECRCSavesOriginMode pin each half.
TEST(testDECSCDECRCSavesCursorPosition)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;7H"); // CUP row=5,column=7 → (x=6,y=4)
    CHECK(t.screens.active->cursorX == 6);
    CHECK(t.screens.active->cursorY == 4);
    feed(t, "\0337"); // DECSC
    feed(t, "\033[1;1H"); // CUP home
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\0338"); // DECRC
    CHECK(t.screens.active->cursorX == 6);
    CHECK(t.screens.active->cursorY == 4);
}

/// DECSC / DECRC — autowrap (DECAWM, mode 7) is not part of the saved cursor.
/// DEC STD 070 p.5-139: what DECSC snapshots is the last-column flag, not DECAWM,
/// and resetting DECAWM clears that flag. xterm agrees; restoring the mode itself
/// would undo a DECRST the program issued after the save.
TEST(testDECSCDECRCDoesNotRestoreAutowrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.autowrap == true);
    feed(t, "\033[?7l"); // DECRST 7 — autowrap off
    feed(t, "\0337"); // DECSC
    feed(t, "\033[?7h"); // DECSET 7 — autowrap on
    feed(t, "\0338"); // DECRC
    CHECK(t.modes.autowrap == true);
}

/// DECRC on a fresh terminal — no DECSC was issued, so DEC STD 070's defaults
/// apply: the cursor goes home, origin mode and the character attributes reset,
/// and G0..G3 hold ASCII with G0 in GL and GR at its default.
TEST(testDECRCWithoutDECSCGoesToHome)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Move cursor away from home and dirty the pen so we can confirm
    // DECRC's home-fallback fires.
    feed(t, "\033[10;20H");
    feed(t, "\033[31m");
    CHECK(t.screens.active->cursorX == 19);
    CHECK(t.screens.active->cursorY == 9);
    CHECK(t.screens.active->hasSavedCursor == false);
    feed(t, "\033)0\016\033~"); // G1 special graphics in GL and in GR
    feed(t, "\0338"); // DECRC with no prior DECSC
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(t.characterSetG[1] == 0); // US-ASCII
    CHECK(t.characterSetActive == 0);
    CHECK(t.characterSetGR == 0);
}

/// A DECRC with nothing saved also drops a pending SS2 or SS3, so the
/// character-set state is wholly back at its defaults.
TEST(testDECRCWithoutDECSCDropsAPendingSingleShift)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033*0\033N");
    CHECK(t.singleShift == 2);
    feed(t, "\0338");
    CHECK(t.singleShift == 0);
    feed(t, "q");
    CHECK(cell(t, 0, 0)->codePoint == 'q');
}

/// DECSC / DECRC — saved state is per-screen (primary vs alternate
/// each carry their own savedX/Y/pen). Switch screens between saves
/// and confirm the restores don't bleed across.
TEST(testDECSCPerScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Primary: park cursor at (3,4), DECSC.
    feed(t, "\033[4;5H"); // CUP row=4,column=5 → (x=4,y=3)
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 3);
    feed(t, "\0337"); // DECSC on primary
    // Switch to alternate (1047); park cursor at (7,8), DECSC.
    feed(t, "\033[?1047h");
    CHECK(t.screens.isUsingAlternateScreen == true);
    feed(t, "\033[8;9H"); // CUP row=8,column=9 → (x=8,y=7)
    CHECK(t.screens.active->cursorX == 8);
    CHECK(t.screens.active->cursorY == 7);
    feed(t, "\0337"); // DECSC on alternate
    // Trash alternate cursor, DECRC — should land at alternate's
    // saved (8,7), not primary's (4,3).
    feed(t, "\033[1;1H");
    feed(t, "\0338");
    CHECK(t.screens.active->cursorX == 8);
    CHECK(t.screens.active->cursorY == 7);
    // Back to primary; DECRC there must land at primary's (4,3).
    feed(t, "\033[?1047l");
    CHECK(t.screens.isUsingAlternateScreen == false);
    feed(t, "\033[1;1H");
    feed(t, "\0338");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 3);
}

/// HTS (ESC H) — sets a horizontal tab stop at the current column.
TEST(testHTSSetsStopAtCurrentColumn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // The default 8-column tab grid leaves column 5 clear. Park there
    // and HTS — that exact column must now be a stop.
    feed(t, "\033[1;6H"); // CUP column=6 → cursorX=5
    CHECK(t.screens.active->cursorX == 5);
    CHECK(t.tabStops[5] == false);
    feed(t, "\033H"); // HTS
    CHECK(t.tabStops[5] == true);
}

/// HTS — idempotent. A second HTS at the same column changes nothing.
TEST(testHTSIdempotent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;6H\033H"); // set stop at column 5
    CHECK(t.tabStops[5] == true);
    feed(t, "\033H"); // second HTS — no-op
    CHECK(t.tabStops[5] == true);
}

/// HTS alongside TBC — set, clear-at-cursor, re-set: TBC must clear
/// the stop HTS placed, and a follow-up HTS must put it back.
TEST(testHTSAlongsideTBC)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;6H\033H"); // HTS at column 5
    CHECK(t.tabStops[5] == true);
    feed(t, "\033[g"); // TBC parameter=0 — clear at cursor
    CHECK(t.tabStops[5] == false);
    feed(t, "\033H"); // HTS again — re-set
    CHECK(t.tabStops[5] == true);
}

TEST(testDoubleWidthTabClampsToVisibleWidth)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033#6"); // DECDWL on row 0
    feed(t, "x"); // column 0
    feed(t, "\t\t\t\t\t"); // tabs reach 8,16,24,32, then clamp to 39 (was 40)
    feed(t, "x"); // visible right edge
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 39)->codePoint == 'x');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 40)->codePoint == 0);
    // A normal line is unaffected: the same tabs run on to column 40.
    feed(t, "\033[2;1Hx\t\t\t\t\tx");
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 1, 40)->codePoint == 'x');
}

// Absolute positioning clips to the full screen on every axis: a row/column
// parameter of 0 lands one before the page, a large parameter past the end —
// both must clamp rather than index out of range. DECLRMM margins do not bind
// absolute motion.
TEST(testAbsolutePositionClampsToScreenEdges)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[0;1H"); // CUP row 0 → cursorY = -1 → pinned to 0
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033[999;1H"); // CUP row 999 → pinned to the last row
    CHECK(t.screens.active->cursorY == 23);
    feed(t, "\033[1;0H"); // CUP column 0 → cursorX = -1 → pinned to 0
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\033[1;999H"); // CUP column 999 → pinned to the last column
    CHECK(t.screens.active->cursorX == 79);
    feed(t, "\033[0G"); // CHA 0: same column-only clamp
    CHECK(t.screens.active->cursorX == 0);
}

// moreFix (DECSET 41): HT at an armed deferred wrap commits the wrap first, so
// the tab lands on the next row's first stop. Without the mode the latch stays
// armed and the tab pins at the right margin (the covered default branch).
TEST(testMoreFixHTCommitsDeferredWrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    feed(t, "\033[?41h"); // moreFix on
    feed(t, "ABCDEFGHIJ"); // 'J' at the last column arms the wrap
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "\t"); // HT commits the wrap, then steps to the first stop
    CHECK(t.screens.active->wrapPending == false);
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 8); // default stop, 8-wide spacing
    feed(t, "k");
    CHECK(cell(t, 1, 8)->codePoint == 'k'); // text lands at the stop
}

// moreFix off (the default): HT at an armed wrap pins at the right margin and
// the latch survives — the same sequence, different destination.
TEST(testPlainHTAtArmedWrapPinsAtMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    feed(t, "ABCDEFGHIJ");
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "\t");
    CHECK(t.screens.active->cursorY == 0); // no wrap commit
    CHECK(t.screens.active->cursorX == 9); // pinned at the right margin
    CHECK(t.screens.active->wrapPending == true); // latch stays armed
}

// LNM (DECSET 20): a line feed moves the cursor to column 0 as well.
TEST(testLineFeedWithLNMResetsColumn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "abc");
    CHECK(t.screens.active->cursorX == 3);
    feed(t, "\033[20h"); // LNM on
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\033[20l"); // LNM off
    feed(t, "d");
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 1); // column preserved
}

// The same LNM column reset applies to the VT/FF line-feed controls, which
// route through the DFA rather than the inline ground fast path.
TEST(testVTAndFFLineFeedHonorLNM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "abc");
    feed(t, "\033[20h"); // LNM on
    feed(t, "\x0B"); // VT
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "d");
    feed(t, "\x0C"); // FF
    CHECK(t.screens.active->cursorY == 2);
    CHECK(t.screens.active->cursorX == 0);
}

// A double-width or double-height line shows only its left half, so text
// wraps at its middle and the cursor stops there, as DEC STD 070 and xterm
// have it; text written past the middle would be drawn off the screen.
TEST(testDoubleWidthLineWrapsAtItsMiddle)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    feed(t, "\033#6ABCDEFGHIJ");
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    CHECK(cell(t, 0, 5)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 'F');
    CHECK(t.screens.active->cursorY == 1 && t.screens.active->cursorX == 5);

    Ubiquitty::hardReset(t);
    feed(t, "\033#3\033[1;10H");
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\033[1;1H\033[20C");
    CHECK(t.screens.active->cursorX == 4);

    Ubiquitty::hardReset(t);
    feed(t, "\033[1;10H\033#6");
    CHECK(t.screens.active->cursorX == 4);
}

TEST(testSingletonASCIIPlacementMatchesGraphicPathAtVisibleBounds)
{
    const auto compare = [](const Ubiquitty::Terminal& actual, const Ubiquitty::Terminal& expected) {
        CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
        CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
        CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
        for (int row = 0; row < actual.rows; ++row) {
            CHECK_EQ(actual.screens.active->lineAttribute[row], expected.screens.active->lineAttribute[row]);
            for (int column = 0; column < actual.columns; ++column)
                CHECK(std::memcmp(cell(actual, row, column), cell(expected, row, column), sizeof(Ubiquitty::Cell)) == 0);
        }
    };
    for (const int columns : { 2, 3, 5, 8, 9, 10 }) {
        for (const char* lineSize : { "\033#5", "\033#6", "\033#3", "\033#4" }) {
            for (const bool isMarginNarrowed : { false, true }) {
                // DECLRMM resets doubled rows to normal and refuses new sizes.
                if (isMarginNarrowed && (columns < 4 || lineSize[2] != '5'))
                    continue;
                for (const bool isAlternate : { false, true }) {
                    for (int column = 0; column < columns; ++column) {
                        Ubiquitty::Terminal actual;
                        Ubiquitty::Terminal expected;
                        char setup[96];
                        std::snprintf(setup, sizeof(setup), "%s\033[2;%dH\033[38;5;2m",
                            isAlternate ? "\033[?1049h" : "", column + 1);
                        for (auto* terminal : { &actual, &expected }) {
                            Ubiquitty::hardReset(*terminal);
                            Ubiquitty::resize(*terminal, 3, columns, Ubiquitty::Modes::ReflowMode::Fixed);
                            feed(*terminal, setup);
                            // Select the target row before changing its line size.
                            feed(*terminal, lineSize);
                            if (isMarginNarrowed) {
                                char bounds[48];
                                std::snprintf(bounds, sizeof(bounds), "\033[?69h\033[1;%ds\033[2;%dH", columns / 2, column + 1);
                                feed(*terminal, bounds);
                            }
                        }
                        // A successor CSI makes this a singleton ASCII write.
                        // The direct graphic entry uses the general placement path.
                        feed(actual, "X\033[0m");
                        Ubiquitty::VT::emitGraphicCodePoint(expected, 'X');
                        feed(expected, "\033[0m");
                        compare(actual, expected);

                        // SGR keeps the open cluster, including at a pending wrap.
                        feed(actual, "\xCC\x81\033[0m");
                        Ubiquitty::VT::emitGraphicCodePoint(expected, 0x0301);
                        feed(expected, "\033[0m");
                        compare(actual, expected);
                    }
                }
            }
        }
    }
}

TEST(testSingletonASCIIRepaintKeepsContentGeneration)
{
    for (const int columns : { 2, 3, 10 }) {
        for (const char* lineSize : { "\033#5", "\033#6", "\033#3", "\033#4" }) {
            const bool isDoubled = lineSize[2] != '5';
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            Ubiquitty::resize(t, 3, columns, Ubiquitty::Modes::ReflowMode::Fixed);
            feed(t, lineSize);
            const int visibleColumns = isDoubled ? columns / 2 : columns;
            std::string text;
            for (int column = 0; column < visibleColumns; ++column)
                text += "\033[48;5;23mX";
            feed(t, text);
            CHECK_EQ(t.screens.active->cursorX, visibleColumns - 1);
            CHECK(t.screens.active->wrapPending);
            const auto generation = t.grid->contentGeneration();
            feed(t, "\033[H");
            feed(t, text);
            CHECK_EQ(t.grid->contentGeneration(), generation);
            CHECK_EQ(t.screens.active->cursorY, 0);
            CHECK(t.screens.active->wrapPending);
            for (int column = 0; column < columns; ++column) {
                CHECK_EQ(cell(t, 0, column)->codePoint, column < visibleColumns ? uint32_t { 'X' } : 0u);
                if (column < visibleColumns)
                    CHECK_EQ(cell(t, 0, column)->pen.backgroundIndex, 23);
                CHECK_EQ(cell(t, 1, column)->codePoint, 0u);
            }
        }
    }
}

// While DECLRMM allows left and right margins, the line-size requests are
// ignored and the line stays single-width.
TEST(testLineSizeIsRefusedWhileMarginsAreAllowed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    feed(t, "\033[?69h\033#6ABCDEFGHIJ");
    CHECK(cell(t, 0, 9)->codePoint == 'J');
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeNormal);
}

// DECSLRM with a left margin not left of its right is ignored, as DEC STD 070
// ignores the same DECSTBM: the margins keep their values and the cursor does
// not move.
TEST(testDegenerateDECSLRMIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h\033[2;10s\033[2;5H\033[3;3s");
    CHECK(t.screens.active->scrollLeft == 1);
    CHECK(t.screens.active->scrollRight == 9);
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\033[5;4s");
    CHECK(t.screens.active->scrollLeft == 1);
    CHECK(t.screens.active->cursorX == 4);
}

// Only the bare `CSI s` saves the cursor while DECLRMM is reset; with a
// parameter it is a DECSLRM the terminal ignores.
TEST(testSCOSCWithAParameterSavesNothing)
{
    for (const char* save : { "\033[0s", "\033[1s", "\033[1;2s" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 3, 5);
        feed(t, "\033[2;3H");
        feed(t, save);
        feed(t, "\033[1;1H\033[u");
        CHECK(t.screens.active->cursorY == 0);
        CHECK(t.screens.active->cursorX == 0);
    }
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033[2;3H\033[s\033[1;1H\033[u");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 2);
}

// HPR and VPR are bounded by the page, not the margins, unless origin mode
// holds the cursor inside them.
TEST(testHPRAndVPRMovePastTheMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 5);
    feed(t, "\033[?69h\033[2;3s\033[1;2H\033[3a");
    CHECK(t.screens.active->cursorX == 4);
    feed(t, "\033[?69l\033[1;2r\033[1;3H\033[3e");
    CHECK(t.screens.active->cursorY == 3);
    feed(t, "\033[?6h\033[1;1H\033[9e");
    CHECK(t.screens.active->cursorY == 1);
}

// DECSCPP moves the cursor in to the new last column; Pn left out or 0 is 80
// and a width other than 80 or 132 is ignored.
TEST(testDECSCPPMovesTheCursorInToTheNewWidth)
{
    for (const char* select : { "\033[80$|", "\033[$|", "\033[0$|", "\033[80;132$|" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 2, 82);
        feed(t, "\033[1;82H");
        feed(t, select);
        CHECK(t.screens.active->cursorX == 79);
    }
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 82);
    feed(t, "\033[1;82H\033[100$|");
    CHECK(t.screens.active->cursorX == 81);
    feed(t, "\033[132$|");
    CHECK(t.screens.active->cursorX == 81);
}

// DECST8C is `CSI ? 5 W` alone; with a second parameter it is ignored.
TEST(testDECST8CWithASecondParameterIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 1, 20);
    feed(t, "\033[3g\033[?5;1W\tX");
    CHECK(t.screens.active->cursorX == 19);
    feed(t, "\r\033[?5W\tY");
    CHECK(t.screens.active->cursorX == 9);
}

TEST(testHPBAndVPBUseDataPositionBounds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "\033[?69h\033[3;8s\033[2;5r\033[4;6H\033[j\033[k");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 2);
    feed(t, "\033[0j\033[0k");
    CHECK(t.screens.active->cursorX == 3);
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "\033[99j\033[99k");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033[?6h\033[4;6H\033[99j\033[99k");
    CHECK(t.screens.active->cursorX == 2);
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "\033[?6l\033[?69l\033[r\033[1;10HX\033[jY");
    CHECK(!t.screens.active->wrapPending);
    CHECK(cell(t, 0, 8)->codePoint == 'Y');
    CHECK(cell(t, 0, 9)->codePoint == 'X');
}

UBIQUITTY_TEST_MAIN()
TEST(testLineTabStopsMoveClearAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 12);
    feed(t, "\033[3;4H\033J\033[6;4H\033J");
    for (const char* sequence : { "\033[Y", "\033[0Y", "\033[1Y" }) {
        feed(t, "\033[1;4H");
        feed(t, sequence);
        CHECK_EQ(t.screens.active->cursorY, 2);
        CHECK_EQ(t.screens.active->cursorX, 3);
    }
    feed(t, "\033[1;4H\033[2Y");
    CHECK_EQ(t.screens.active->cursorY, 5);
    feed(t, "\033[1;4H\033[9999Y");
    CHECK_EQ(t.screens.active->cursorY, 5);
    feed(t, "\033[3;1H\033[1g\033[H\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 5);
    feed(t, "\033[4g\033[H\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 0);
    feed(t, "\033[4;1H\033J\033[!p\033[H\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 3);
    Ubiquitty::hardReset(t);
    feed(t, "\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 0);
    CHECK(t.lineTabStops.none());
}

TEST(testLineTabsPreserveTheColumnAndCancelWrapOnlyOnMovement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 8);
    feed(t, "\033[3;1H\033J\033[H12345678\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 2);
    CHECK_EQ(t.screens.active->cursorX, 7);
    CHECK(!t.screens.active->wrapPending);
    feed(t, "X\033[Y");
    CHECK(t.screens.active->wrapPending);
    feed(t, "Y");
    CHECK_EQ(t.screens.active->cursorY, 3);
    CHECK_EQ(t.screens.active->cursorX, 1);
    feed(t, "\033[4;1H\033#6\033J\033[1;8H\033[2Y");
    CHECK_EQ(t.screens.active->cursorY, 3);
    CHECK_EQ(t.screens.active->cursorX, 3);
}

TEST(testEightBitLineTabStopAndResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 8);
    feed(t, "\033%@\033[5;2H\x8a\033[1;2H\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 4);
    CHECK_EQ(t.screens.active->cursorX, 1);
    Ubiquitty::resize(t, 3, 8);
    Ubiquitty::resize(t, 6, 8);
    feed(t, "\033[H\033[Y");
    CHECK_EQ(t.screens.active->cursorY, 0);
}

TEST(testCursorTabulationControlAppliesEverySelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 12);
    feed(t, "\033[5;6W\033[3;4H\033[0;1W\033[H\t\033[Y");
    CHECK_EQ(t.screens.active->cursorX, 3);
    CHECK_EQ(t.screens.active->cursorY, 2);
    feed(t, "\033[2;3W\033[H\t\033[Y");
    CHECK_EQ(t.screens.active->cursorX, 11);
    CHECK_EQ(t.screens.active->cursorY, 0);
    feed(t, "\033[1;5H\033[W\033[H\t");
    CHECK_EQ(t.screens.active->cursorX, 4);
    feed(t, "\033[4W\033[H\t");
    CHECK_EQ(t.screens.active->cursorX, 11);
    feed(t, "\033[3;4H\033[0;1W\033[5g\033[H\t\033[Y");
    CHECK_EQ(t.screens.active->cursorX, 11);
    CHECK_EQ(t.screens.active->cursorY, 0);
    feed(t, "\033[3;4H\033[0;1W\033[2g\033[H\t\033[Y");
    CHECK_EQ(t.screens.active->cursorX, 11);
    CHECK_EQ(t.screens.active->cursorY, 2);
}
