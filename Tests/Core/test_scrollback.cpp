// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>

// Drain a paint cycle the way the PAL would: ack pending scrolls (shifts the
// shadow / BB), then every dirty run (copies changed cells into the shadow).
// Leaves the shadow == what the back buffer would show.
static void drainPaint(Ubiquitty::CellGrid* d)
{
    Ubiquitty::PaintSession ses = d->beginPaint();
    Ubiquitty::Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc))
        ses.ackScroll(sc);
    Ubiquitty::PaintRun run;
    while (ses.nextDirtyRun(&run))
        ses.ackRun(run);
}

// Every visible cell's back-buffer shadow must equal the live cell after a
// paint — a stale shadow is the "output gets eaten" symptom.
static void checkShadowMatchesScreen(Ubiquitty::Terminal& t)
{
    for (int r = 0; r < t.rows; r++) {
        for (int c = 0; c < t.columns; c++) {
            const Ubiquitty::Cell* shadow = t.grid->readShadowCell(r, c);
            const Ubiquitty::Cell* live = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            if (!shadow || !live || shadow->codePoint != live->codePoint) {
                printf("FAIL shadow!=screen at r=%d c=%d (shadow=%d live=%d)\n", r, c, shadow ? shadow->codePoint : -1, live ? live->codePoint : -1);
                failures++;
                return;
            }
        }
    }
}

static void feedNumberedLines(Ubiquitty::Terminal& t, int count)
{
    int i;
    char buffer[16];
    for (i = 0; i < count; i++) {
        snprintf(buffer, sizeof(buffer), "L%04d", i);
        feed(t, buffer);
        if (i + 1 < count)
            feed(t, "\r\n");
    }
}

static void checkLineNumber(const Ubiquitty::Cell* cells, int number)
{
    char expected[16];
    snprintf(expected, sizeof(expected), "L%04d", number);
    CHECK(cells[0].codePoint == (unsigned char)expected[0]);
    CHECK(cells[1].codePoint == (unsigned char)expected[1]);
    CHECK(cells[2].codePoint == (unsigned char)expected[2]);
    CHECK(cells[3].codePoint == (unsigned char)expected[3]);
    CHECK(cells[4].codePoint == (unsigned char)expected[4]);
}

// A configured scrollback limit (set before output) sizes the ring instead of
// the compile-time default, and the ring wraps at exactly that capacity.
static void checkVisLine(Ubiquitty::Terminal& t, int row, int number)
{
    char expected[16];
    snprintf(expected, sizeof(expected), "L%04d", number);
    for (int c = 0; c < 5; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, c);
        CHECK(cell && cell->codePoint == (unsigned char)expected[c]);
    }
}

// Height-only GROW with scrollback pulls history down into the new top rows and
// keeps the existing content anchored at the bottom (reflowHeightOnly fast path,
// matching the full-reflow distribution: last newRows physical rows stay visible).
// A grow large enough to fit all content pulls the whole history into view and
// empties scrollback; the tail rows below the content are blank.
// Grow then shrink back to the original height restores the exact prior visible
// screen and scrollback count — the boundary shift is its own inverse.
// A height resize taken while the alternate screen is active must rewrap the
// PRIMARY (the flip-off/flip-on bracket), leave the alternate content intact,
// and keep alt active across the resize.
// Growing the window while a full-screen app (alt screen) is up must move the
// SAVED primary cursor with the reflow, not just the live one — otherwise
// exiting the app (DECRST 1049 Ubiquitty::Screen::restoreCursor) drops the prompt back at its old
// row and the rest of the taller window reads as blank ("stuck at 24 rows").
// A height SHRINK with no scrollback scrolls the top rows into history and keeps
// the bottom rows + cursor — the inverse of the grow pull.
// Soft-wrapped logical lines survive a height grow/shrink: the fast path moves
// full-width wrapped rows and preserves the wrap-join flag, so a line that spans
// the visible/scrollback boundary rejoins intact and pushes back wrapped.
// Trailing blank rows below the cursor are screen padding: a grow drops them
// rather than pulling them in as content, so the prompt + history land correctly
// and the blank fills the new bottom.
// A shrink eats the blank space below the content first; it only scrolls text
// into history once the last content row has reached the screen bottom.
// `cat`-then-resize: an 80x50 screen with the prompt sitting just below the
// printed output leaves a band of blank rows at the bottom. A vertical shrink
// must consume that blank band first — the window only gets shorter into the
// empty space — and only spill real content into history once the viewport
// bottom reaches the cursor row.
// A width+height resize while a full-screen app is up rewraps the primary; the
// DECSET 1049 saved cursor must follow the live cursor through the rewrap, or
// exiting the app restores the prompt at the wrong row (stacked prompts on the
// next resize).
// After a height resize, normal scrolling (newline at the bottom) must keep the
// back buffer in sync — the reported "type a command, output gets eaten, only
// the last line blinks" bug after a vertical resize.
// A resize while a full-screen app holds the alt screen must update the SAVED
// scroll region (DECSET 1049 saved scrollTop/Bottom), not just the live one.
// Otherwise exiting the app restores a stale full-screen region sized to the old
// height — the screen acts shorter than the window ("still 24 lines after it
// grew"), and only a drastic resize recovers it.
// Per-row visible-window text matcher for the round-trip tests. number < 0 means
// the row must read blank (space or NUL in column 0).
static void checkVisLineOrBlank(Ubiquitty::Terminal& t, int row, int number)
{
    if (number < 0) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, 0);
        CHECK(cell && (cell->codePoint == ' ' || cell->codePoint == 0));
        return;
    }
    checkVisLine(t, row, number);
}

// ROUND-TRIP (bug B — duplication on re-grow): 50-row window, real content on
// rows 0..33, the prompt/cursor on row 34, rows 34..49 blank below. Shrink
// 50 -> 30 (spills the top into history) then grow 30 -> 50. The final visible
// window must reproduce the ORIGINAL screen exactly: content L0000..L0033 on
// rows 0..33, blank below, scrollback empty again. A grow that re-pulls history
// rows without dropping them (or a stale view) shows duplicated rows here.
// Over-shrink round-trip (bug B, extreme): shrink so far that only the blank
// band + the prompt/cursor are visible (50 -> 18, well past the content), then
// grow back to 50. No content row may appear twice in the visible window and the
// scrollback count must be consistent (all pulled back, none orphaned, none
// duplicated).
// LIVE-DRAG (bug A — persistent wrong shrink): a window drag fires one resize
// per pixel-row crossed, so the height steps down one row at a time rather than
// in a single jump. While the viewport bottom stays at or below the cursor row,
// each intermediate reflow consumes the blank band and spills nothing. Stepping
// 50→35 one row at a time (15 steps; the new bottom row 34 still equals the
// cursor row) must leave scrollback empty and the content + cursor untouched.
// A per-step divergence (the geometry-only path's cursor-latch dragging content
// into history) shows up as scrollback growing here.
// LIVE-DRAG round-trip: step 50→30 one row at a time, then 30→50 one row at a
// time. The final viewport must reproduce the original screen with no duplication
// and a drained scrollback — the stepwise inverse of the single-jump round-trip.
// Geometry-only path: when allowReflow=false the resize skips reflowHeightOnly
// and only commits the new geometry + clamps the cursor. This is the path the
// macOS frontend takes when a non-shell foreground app "owns the screen"
// (propagateGridResize's primaryAppOwnsScreen, allowReflow=false) and the path
// every tmux/agent/sync/daemon pane resize takes (Session::Pane::setGeometry —
// the peer, not this client, owns the authoritative content). It does not
// rewrap: on a shrink it top-anchors and clamps the cursor onto a content row
// (the blank band below the cursor is never consumed, content falls below the
// new bottom).
//
// Content below the shrunk bottom must round-trip correctly on a later grow:
// the rows never moved or reflowed, so growing back must
// reveal the same content again, not blank rows. Under
// Modes::ReflowMode::Fixed a grow does not blank newly revealed rows/columns:
// "newly revealed" does not mean "never seen", and blanking them would destroy
// content on a shrink-then-grow-back cycle, while a genuinely first-time-larger
// geometry is already blank from allocation.
// (C) WIDTH-RESIZE DROPS DECDWL/DECDHL: a column reflow rebuilds the visible
// rows from the rewrapped logical lines; resetting every dest row's
// lineAttribute to NORMAL would collapse a double-width (ESC #6) or double-height
// (ESC #3/#4) line back to single size. The reflow must carry each logical
// line's attribute onto the (first/only) physical row it lands on. These lines never
// soft-wrap, so a small ±columns change leaves them on their own row with the attribute
// intact.
// (D) lineAttribute + mark survive a row's trip into and back out of scrollback under
// a WIDTH change. The scrollback carrier (Ubiquitty::Screen::ScrollLine) holds its own
// lineAttribute/mark, so a DECDWL row carrying an OSC-133 prompt mark keeps both after
// it ages into history, a column reflow rewraps it, and a height grow pulls it
// back into the live screen.
// (D') The history-aware read accessors return the SCROLLED-OFF row's OWN attribute +
// mark, not the live row sitting at the same physical index: a DECDWL row
// scrolled into history must read DWL through rowLineAttribute even when a plain
// live row occupies the same screen index.
// (C, sprite squish): a short non-wrapping row of distinct cells that does not
// fill the width must survive a ±2-column change byte-identical — the column reflow
// must not compress, duplicate, or drop its cells. Uses distinct background colors so a
// horizontal squish would change the per-column content.
// NON-EMPTY SCROLLBACK shrink (the bug the empty-scrollback tests miss): with
// history present, folding historyRows into reflowHeightOnly's distribution
// makes a vertical shrink compute a spurious history PULL (grow behaviour)
// instead of letting the blank band below the cursor absorb the
// delta. Drive the live path with real escapes: 200 lines to fill scrollback,
// then ESC[2J ESC[H + 34 printed lines so the cursor sits on row 34 with a
// blank band on rows 35..49. Shrink 50->40 (delta 10 <= 15-row band): nothing
// must spill — scrollbackCount unchanged, top content + cursor put.
static void feedRepaintWithHistory(Ubiquitty::Terminal& t)
{
    char buffer[16];
    for (int i = 0; i < 200; i++) { // overflow scrollback so history > 195
        snprintf(buffer, sizeof(buffer), "P%04d\r\n", i);
        feed(t, buffer);
    }
    feed(t, "\x1b[2J\x1b[H"); // clear screen, cursor home
    for (int i = 0; i < 34; i++) { // print L0000..L0033, cursor -> row 34
        snprintf(buffer, sizeof(buffer), "L%04d\r\n", i);
        feed(t, buffer);
    }
}

// Round-trip with history: 50->40->50. The shrink is absorbed by the blank band
// (nothing spills); the grow back then reveals scrollback above the content, as a
// real terminal does. The invariant under test is no duplication and an intact
// total ordering: every L-content row appears exactly once, the 10 newly revealed
// rows are the 10 newest history lines, and the shrink itself spills nothing.
// Stepwise (one row per drag pixel) shrink with history: 50->40 one row at a
// time must absorb every step into the band and spill nothing.
// HEIGHT ROUND-TRIP (the canonical physical-row-as-truth case): a
// screen FULL of content with no blank band below the cursor — every row holds a
// line and the cursor sits on the last row. Unlike the blank-band round-trips
// above, a shrink here genuinely spills content into history (there is no padding
// to absorb the delta), so the grow back must pull those exact rows home. Shrink
// N->M then grow M->N must reproduce the original screen: every content row once,
// the cursor on its row, and the scrollback count back to its starting value.
TEST(testScrollbackRingWrapsAtCapacity)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Screen::ScrollLine* oldest;
    const Ubiquitty::Screen::ScrollLine* newest;
    int totalLines = Ubiquitty::VT::fallbackScrollbackLines + 22;
    int expectedFirstHistory = totalLines - 5 - Ubiquitty::VT::fallbackScrollbackLines;
    int expectedLastHistory = totalLines - 6;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, totalLines);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == Ubiquitty::VT::fallbackScrollbackLines);
    oldest = Ubiquitty::Screen::scrollbackLine(&t, 0);
    newest = Ubiquitty::Screen::scrollbackLine(&t, Ubiquitty::VT::fallbackScrollbackLines - 1);
    CHECK(oldest != nullptr);
    CHECK(newest != nullptr);
    if (oldest)
        checkLineNumber(oldest->cells, expectedFirstHistory);
    if (newest)
        checkLineNumber(newest->cells, expectedLastHistory);
    CHECK(Ubiquitty::Screen::scrollbackLine(&t, -1) == nullptr);
    CHECK(Ubiquitty::Screen::scrollbackLine(&t, Ubiquitty::VT::fallbackScrollbackLines) == nullptr);
}

TEST(testScrollbackLimitConfigurable)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::setScrollbackLimit(&t, 50);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 50 + 22);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 50);
    CHECK(t.scrollbackCapacity == 50);
    CHECK(Ubiquitty::Screen::scrollbackLine(&t, 50) == nullptr);

    // Below one screen is clamped up to the screen height.
    Ubiquitty::Terminal small;
    Ubiquitty::hardReset(small);
    Ubiquitty::setScrollbackLimit(&small, 1);
    Ubiquitty::resize(small, 5, 20);
    feedNumberedLines(small, 200);
    CHECK(t.scrollbackCapacity != 1);
    CHECK(Ubiquitty::Screen::scrollbackCount(&small) == small.scrollbackCapacity);
    CHECK(small.scrollbackCapacity >= 24); // Ubiquitty::defaultRows floor
}

TEST(testResizeUnderAlternateUpdatesSavedScrollRegion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feedNumberedLines(t, 10);
    feed(t, "\x1b[?1049h"); // enter alt: saves region [0,23]
    Ubiquitty::resize(t, 51, 80); // grow to 51 rows while alt is up
    feed(t, "\x1b[?1049l"); // quit: restore the saved region
    CHECK(t.screens.primary.scrollBottom == 50); // full region spans the new height
    CHECK(t.screens.primary.scrollTop == 0);
}

TEST(testScrollRendersAfterHeightGrow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feedNumberedLines(t, 20); // fill screen + scrollback
    drainPaint(t.grid.get()); // back buffer synced
    Ubiquitty::resize(t, 14, 20); // vertical grow -> reflowHeightOnly
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);

    for (int i = 0; i < 12; i++) // scroll past the bottom repeatedly
        feed(t, "scrolled\r\n");
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);
}

TEST(testScrollRendersAfterHeightShrink)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 14, 20);
    feedNumberedLines(t, 24);
    drainPaint(t.grid.get());
    Ubiquitty::resize(t, 8, 20); // vertical shrink -> reflowHeightOnly
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);

    for (int i = 0; i < 12; i++)
        feed(t, "scrolled\r\n");
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);
}

TEST(testScrollRendersAfterWidthResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedNumberedLines(t, 24);
    drainPaint(t.grid.get());
    Ubiquitty::resize(t, 20, 100); // width+height -> reflowPrimary
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);

    for (int i = 0; i < 12; i++)
        feed(t, "scrolled\r\n");
    drainPaint(t.grid.get());
    checkShadowMatchesScreen(t);
}

TEST(testVisibleCellClampsViewOffset)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Cell* cell;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 40);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 35);

    cell = Ubiquitty::Screen::visibleCell(&t, 1000, 0, 0);
    CHECK(cell->codePoint == 'L');
    cell = Ubiquitty::Screen::visibleCell(&t, 1000, 0, 4);
    CHECK(cell->codePoint == '0');

    cell = Ubiquitty::Screen::visibleCell(&t, -1000, 0, 0);
    CHECK(cell->codePoint == 'L');
    cell = Ubiquitty::Screen::visibleCell(&t, -1000, 0, 3);
    CHECK(cell->codePoint == '3');
    cell = Ubiquitty::Screen::visibleCell(&t, -1000, 0, 4);
    CHECK(cell->codePoint == '5');

    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, -1, 0)->codePoint == 0);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, -1)->codePoint == 0);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, t.rows, 0)->codePoint == 0);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, t.columns)->codePoint == 0);
}

TEST(testAlternateScreenClampsViewToLiveScreen)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Cell* cell;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 20);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 15);

    feed(t, "\033[?1049h\033[HALT");
    cell = Ubiquitty::Screen::visibleCell(&t, 15, 0, 0);
    CHECK(cell->codePoint == 'A');
    cell = Ubiquitty::Screen::visibleCell(&t, 15, 0, 1);
    CHECK(cell->codePoint == 'L');
    cell = Ubiquitty::Screen::visibleCell(&t, 15, 0, 2);
    CHECK(cell->codePoint == 'T');

    feed(t, "\033[?1049l");
}

TEST(testClearScrollbackDropsHistoryKeepsLive)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 40);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 35);

    /// Live screen content before the clear — must survive untouched.
    const Ubiquitty::Cell* liveBefore = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    uint16_t liveCh = liveBefore ? liveBefore->codePoint : 0;

    Ubiquitty::Screen::dropScrollback(&t);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(Ubiquitty::Screen::scrollbackLine(&t, 0) == nullptr);
    /// A view offset into the (now empty) history clamps to the live tail.
    const Ubiquitty::Cell* liveAfter = Ubiquitty::Screen::visibleCell(&t, 1000, 0, 0);
    CHECK(liveAfter != nullptr);
    CHECK(liveAfter->codePoint == liveCh);
}

TEST(testHeightGrowPullsHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 20); // visible L0015..L0019, history L0000..L0014
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 15);

    Ubiquitty::resize(t, 8, 20); // grow 5 -> 8 rows, same width
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 12); // 3 newest pulled up
    checkVisLine(t, 0, 12);
    checkVisLine(t, 2, 14);
    checkVisLine(t, 3, 15);
    checkVisLine(t, 7, 19);
}

TEST(testHeightGrowDrainsScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 8); // visible L0003..L0007, history L0000..L0002
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 3);

    Ubiquitty::resize(t, 10, 20); // total 8 rows < 10 -> all visible
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    checkVisLine(t, 0, 0);
    checkVisLine(t, 7, 7);
    const Ubiquitty::Cell* tail = Ubiquitty::Screen::visibleCell(&t, 0, 8, 0);
    CHECK(tail && (tail->codePoint == ' ' || tail->codePoint == 0));
}

TEST(testHeightGrowShrinkRoundtrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 20);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 15);

    Ubiquitty::resize(t, 8, 20);
    Ubiquitty::resize(t, 5, 20); // back to start
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 15);
    checkVisLine(t, 0, 15);
    checkVisLine(t, 4, 19);
}

TEST(testHeightResizeUnderAlternateKeepsAlternateIntact)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feedNumberedLines(t, 20); // primary: history + visible
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 10);

    feed(t, "\033[?1049h"); // enter alt screen
    feed(t, "\033[HALT"); // write "ALT" at alt home
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 'A');

    Ubiquitty::resize(t, 14, 20); // grow while alt is up
    // Alternate is still active and its content is untouched.
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 'A');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == 'L');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 2)->codePoint == 'T');

    feed(t, "\033[?1049l"); // exit alt -> primary restored
    CHECK(t.rows == 14);
}

TEST(testHeightGrowMovesSavedCursorUnderAlternate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feedNumberedLines(t, 30); // history 6, prompt row 23
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 6);
    CHECK(t.screens.primary.cursorY == 23);

    feed(t, "\x1b[?1049h"); // enter alt screen (mc): saves cursor
    Ubiquitty::resize(t, 51, 80); // grow while alt is up: 6 lines pulled
    feed(t, "\x1b[?1049l"); // quit: Ubiquitty::Screen::restoreCursor on the primary

    // The prompt followed the reflow down by the 6 pulled history lines.
    CHECK(t.screens.primary.cursorY == 29);
}

TEST(testHeightShrinkPushesToScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feedNumberedLines(t, 10); // fills rows 0..9, no scrollback
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    Ubiquitty::resize(t, 6, 20); // 4 top rows -> scrollback
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 4);
    checkVisLine(t, 0, 4); // L0004 now top of the visible screen
    checkVisLine(t, 5, 9); // L0009 at the bottom
    checkLineNumber(Ubiquitty::Screen::scrollbackLine(&t, 0)->cells, 0);
    checkLineNumber(Ubiquitty::Screen::scrollbackLine(&t, 3)->cells, 3);
}

TEST(testHeightResizePreservesSoftwrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 8);
    // "WWWW..."(12) wraps to 2 rows; enough following lines scroll the line's
    // first row (wrapped) into history while its tail stays on screen.
    feed(t, "L0\r\nL1\r\nWWWWWWWWWWWW\r\nM3\r\nM4\r\nM5\r\nM6");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 3); // L0, L1, W-head(wrapped)

    Ubiquitty::resize(t, 8, 8); // pull all 3 back
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 2, 0)->codePoint == 'W'); // W-head row
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 2, 7)->codePoint == 'W');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 3, 0)->codePoint == 'W'); // continuation
    CHECK(t.screens.primary.lineWrapped[2] == true);

    Ubiquitty::resize(t, 5, 8); // push back into history
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 3);
    const Ubiquitty::Screen::ScrollLine* head = Ubiquitty::Screen::scrollbackLine(&t, 2);
    CHECK(head && head->wrapped == true && head->cells[0].codePoint == 'W');
}

TEST(testHeightGrowTrimsTrailingBlanks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 20);
    feedNumberedLines(t, 30); // history 6, prompt row 23
    feed(t, "\x1b[10;1H\x1b[J"); // cursor to row 9 (0-based), erase below
    CHECK(t.screens.primary.cursorY == 9);

    Ubiquitty::resize(t, 40, 20); // grow: content is only rows 0..9
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // all 6 history pulled up
    checkVisLine(t, 0, 0); // oldest history at the top
    checkVisLine(t, 6, 6); // visible content shifted down by 6
    const Ubiquitty::Cell* tail = Ubiquitty::Screen::visibleCell(&t, 0, 39, 0);
    CHECK(tail && (tail->codePoint == ' ' || tail->codePoint == 0)); // blank fills the bottom
}

TEST(testHeightShrinkEatsBlankFirst)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 20);
    feedNumberedLines(t, 10); // content rows 0..9, blank 10..23
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    Ubiquitty::resize(t, 15, 20); // still taller than the 10 content rows
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // nothing scrolled off yet
    checkVisLine(t, 0, 0);
    checkVisLine(t, 9, 9); // content untouched at the top

    Ubiquitty::resize(t, 8, 20); // now shorter than content: text scrolls up
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 2); // top two rows into history
    checkVisLine(t, 0, 2);
    checkVisLine(t, 7, 9); // last line at the bottom
}

TEST(testHeightShrinkEatsBlankBelowCursorFirst)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34); // output on rows 0..33
    feed(t, "\x1b[35;1H"); // prompt/cursor to row 34 (0-based)
    CHECK(t.screens.primary.cursorY == 34);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // rows 34..49 blank below

    // Shrink 50 -> 40: delta 10, blank rows below the cursor cover it entirely,
    // so nothing scrolls off. Top content + cursor stay put; window just shortens.
    Ubiquitty::resize(t, 40, 80);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // no spill into history
    checkVisLine(t, 0, 0); // top content untouched
    checkVisLine(t, 33, 33); // last printed line untouched
    CHECK(t.screens.primary.cursorY == 34); // cursor unmoved
    CHECK(t.rows == 40);

    // Shrink 40 -> 30: the cursor row (34) is now past the new bottom (29). The
    // blank band 35..39 is gone; the remaining delta spills the top rows into
    // history and shifts the cursor up to the last visible row.
    Ubiquitty::resize(t, 30, 80);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 5); // 5 top rows into history
    CHECK(t.screens.primary.cursorY == 29); // cursor pinned to the new bottom
    checkVisLine(t, 0, 5); // L0005 now tops the visible screen
    checkVisLine(t, 28, 33); // L0033 just above the cursor row
    checkLineNumber(Ubiquitty::Screen::scrollbackLine(&t, 0)->cells, 0);
    checkLineNumber(Ubiquitty::Screen::scrollbackLine(&t, 4)->cells, 4);
    CHECK(t.rows == 30);
}

TEST(testHeightShrinkGrowRoundtripNoDup)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34); // content rows 0..33
    feed(t, "\x1b[35;1H"); // prompt/cursor to row 34 (0-based)
    CHECK(t.screens.primary.cursorY == 34);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    Ubiquitty::resize(t, 30, 80); // shrink: blank band gone, top spills
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 5); // 5 top rows into history

    Ubiquitty::resize(t, 50, 80); // grow back: pull the 5 rows home
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // history drained, no leftover
    CHECK(t.rows == 50);
    // The exact original screen is back: content top-anchored, blank below the
    // cursor, with no row appearing twice.
    for (int r = 0; r <= 33; r++)
        checkVisLine(t, r, r);
    for (int r = 34; r < 50; r++)
        checkVisLineOrBlank(t, r, -1);
    CHECK(t.screens.primary.cursorY == 34); // cursor back on its row
}

TEST(testHeightOverShrinkGrowRoundtripNoDup)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34); // content rows 0..33
    feed(t, "\x1b[35;1H"); // prompt/cursor to row 34 (0-based)
    CHECK(t.screens.primary.cursorY == 34);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    // 50 -> 18: cursor row 34 is far past the new bottom (17). The blank band is
    // gone and the top content spills into history. Content rows = 35 (0..34),
    // visible 18 -> 17 spill into scrollback.
    Ubiquitty::resize(t, 18, 80);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 17);
    CHECK(t.screens.primary.cursorY == 17); // cursor pinned to the new bottom

    // Grow straight back to 50.
    Ubiquitty::resize(t, 50, 80);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(t.rows == 50);
    // Original screen restored, every content row exactly once.
    for (int r = 0; r <= 33; r++)
        checkVisLine(t, r, r);
    for (int r = 34; r < 50; r++)
        checkVisLineOrBlank(t, r, -1);
    CHECK(t.screens.primary.cursorY == 34);
}

TEST(testHeightStepwiseShrinkEatsBlankNoSpill)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34); // content rows 0..33
    feed(t, "\x1b[35;1H"); // prompt/cursor to row 34 (0-based)
    CHECK(t.screens.primary.cursorY == 34);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    for (int r = 49; r >= 35; r--) // 50 -> 35, one row per step
        Ubiquitty::resize(t, r, 80);

    CHECK(t.rows == 35);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // blank band absorbed it all
    CHECK(t.screens.primary.cursorY == 34); // cursor unmoved (still last row)
    for (int r = 0; r <= 33; r++) // every content row still on screen, once
        checkVisLine(t, r, r);
}

TEST(testHeightStepwiseShrinkGrowRoundtripNoDup)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34);
    feed(t, "\x1b[35;1H");
    CHECK(t.screens.primary.cursorY == 34);

    for (int r = 49; r >= 30; r--) // shrink to 30, one row at a time
        Ubiquitty::resize(t, r, 80);
    for (int r = 31; r <= 50; r++) // grow back to 50, one row at a time
        Ubiquitty::resize(t, r, 80);

    CHECK(t.rows == 50);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    for (int r = 0; r <= 33; r++)
        checkVisLine(t, r, r);
    for (int r = 34; r < 50; r++)
        checkVisLineOrBlank(t, r, -1);
    CHECK(t.screens.primary.cursorY == 34);
}

TEST(testHeightGeometryOnlyShrinkDoesNotReflow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedNumberedLines(t, 34); // content rows 0..33
    feed(t, "\x1b[35;1H"); // cursor to row 34, blank 34..49
    CHECK(t.screens.primary.cursorY == 34);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    // Shrink 50 -> 30 with reflow DISABLED: geometry-only. No history spill
    // (cells aren't moved), but the cursor clamps to the new bottom and the
    // rows below it (34..49, incl. the cursor's own row 34) fall off-screen.
    Ubiquitty::resize(t, 30, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    CHECK(t.rows == 30);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // nothing pushed to history
    CHECK(t.screens.primary.cursorY == 29); // cursor clamped to bottom
    checkVisLine(t, 0, 0); // top stays put (top-anchored)
    checkVisLine(t, 29, 29); // row 29 still shows L0029

    // Grow back 30 -> 50, reflow DISABLED: geometry-only reveals the same
    // content that fell below row 29 on the shrink (L0030..L0033) — the rows
    // never moved, so there's nothing to reflow or restore, only reveal.
    Ubiquitty::resize(t, 50, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    CHECK(t.rows == 50);
    checkVisLine(t, 29, 29);
    checkVisLine(t, 30, 30); // L0030 — not blank
    checkVisLine(t, 33, 33); // L0033 — the last content row
    checkVisLineOrBlank(t, 34, -1); // 34..49 were always blank
}

TEST(testHeightShrinkEatsBlankWithHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedRepaintWithHistory(t);
    int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(historyBefore == 151); // 200 P-lines + L-feed trailing newline scroll history past the 50-row screen
    CHECK(t.screens.primary.cursorY == 34); // blank band on rows 35..49

    Ubiquitty::resize(t, 40, 80); // shrink: band (15) >= delta (10)
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == historyBefore); // Nothing spilled
    CHECK(t.screens.primary.cursorY == 34); // cursor unmoved
    checkVisLine(t, 0, 0); // L0000 still tops the screen
    checkVisLine(t, 33, 33); // last printed line untouched
    CHECK(t.rows == 40);
}

TEST(testHeightShrinkGrowRoundtripWithHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedRepaintWithHistory(t);
    int historyBefore = Ubiquitty::Screen::scrollbackCount(&t); // 151

    Ubiquitty::resize(t, 40, 80); // shrink absorbed by band
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == historyBefore); // nothing spilled
    CHECK(t.screens.primary.cursorY == 34);

    Ubiquitty::resize(t, 50, 80); // grow reveals history rows
    CHECK(t.rows == 50);
    // Content is L0000..L0033 (34 rows) with the cursor on row 34. Growing 40->50
    // frees 15 rows above the content's bottom; they fill, top-anchored, from the
    // newest history (P0136..P0150). So 15 history lines are pulled up, the
    // content sits on rows 15..48, and the cursor lands on row 49. No
    // duplication: each L row appears exactly once.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == historyBefore - 15); // 15 pulled up
    CHECK(t.screens.primary.cursorY == 49); // content shifted down by 15
    for (int r = 0; r <= 33; r++)
        checkVisLine(t, r + 15, r);
}

TEST(testHeightStepwiseShrinkWithHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 50, 80);
    feedRepaintWithHistory(t);
    int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);

    for (int r = 49; r >= 40; r--)
        Ubiquitty::resize(t, r, 80);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == historyBefore);
    CHECK(t.screens.primary.cursorY == 34);
    checkVisLine(t, 0, 0);
    checkVisLine(t, 33, 33);
    CHECK(t.rows == 40);
}

TEST(testReflowHeightRoundtrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feedNumberedLines(t, 10); // rows 0..9 all content, cursor row 9
    CHECK(t.screens.primary.cursorY == 9);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);

    Ubiquitty::resize(t, 6, 20); // shrink: 4 top rows truly spill
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 4);
    CHECK(t.screens.primary.cursorY == 5); // cursor pinned to the new bottom
    checkVisLine(t, 0, 4); // L0004 now tops the visible screen
    checkVisLine(t, 5, 9); // L0009 at the bottom

    Ubiquitty::resize(t, 10, 20); // grow back: the 4 rows come home
    CHECK(t.rows == 10);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0); // history drained, no leftover
    CHECK(t.screens.primary.cursorY == 9); // cursor back on its row
    for (int r = 0; r <= 9; r++) // every content row reproduced, once
        checkVisLine(t, r, r);
}

TEST(testWidthResizePreservesLineAttributes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 101);
    // Row 0: a DECDWL banner. Row 2/3: a DECDHL pair. Plain rows around them.
    feed(t, "PLAIN0\r\n");
    feed(t,
        "\x1b#6"
        "WIDE1\r\n"); // row 1 DECDWL
    feed(t,
        "\x1b#3"
        "TALL2\r\n"); // row 2 DECDHL top
    feed(t,
        "\x1b#4"
        "TALL3\r\n"); // row 3 DECDHL bottom
    feed(t, "PLAIN4"); // row 4 plain, cursor here
    CHECK(t.screens.primary.lineAttribute[1] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.primary.lineAttribute[2] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.primary.lineAttribute[3] == Ubiquitty::lineAttributeDoubleHeightBottom);
    CHECK(t.screens.primary.lineAttribute[0] == Ubiquitty::lineAttributeNormal);

    // Small column shrink: 101 -> 99. None of these short lines wrap, so they
    // keep their rows; the attributes must survive the rewrap.
    Ubiquitty::resize(t, 10, 99);
    CHECK(t.screens.primary.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.primary.lineAttribute[1] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.primary.lineAttribute[2] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.primary.lineAttribute[3] == Ubiquitty::lineAttributeDoubleHeightBottom);

    // Grow back 99 -> 101: still preserved (and no stale attribute leaks onto the
    // plain rows from a reused reflow-ring slot).
    Ubiquitty::resize(t, 10, 101);
    CHECK(t.screens.primary.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.primary.lineAttribute[1] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.primary.lineAttribute[2] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.primary.lineAttribute[3] == Ubiquitty::lineAttributeDoubleHeightBottom);
    CHECK(t.screens.primary.lineAttribute[4] == Ubiquitty::lineAttributeNormal);
}

TEST(testLineattrMarkRoundtripThroughScrollbackWidthChange)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    // Row 0: a DECDWL banner with an OSC-133 prompt mark. Then fill the screen and
    // push it off the top into history.
    feed(t, "\x1b]133;A\x1b\\"); // prompt-start mark on row 0
    feed(t,
        "\x1b#6"
        "BANNER"); // row 0 DECDWL
    feed(t, "\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5"); // scroll BANNER into history
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 2);
    // The banner is the oldest history row; it carries DWL + the prompt mark.
    const Ubiquitty::Screen::ScrollLine* hist0 = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(hist0 && hist0->cells[0].codePoint == 'B');
    CHECK(hist0 && hist0->lineAttribute == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(hist0 && (hist0->mark & Ubiquitty::Screen::osc133MarkPromptStart));

    // WIDTH reflow narrow then wide: the banner is short (6 cells) so it never
    // wraps; its attribute + mark must ride the rewrap ring both ways.
    Ubiquitty::resize(t, 4, 12);
    const Ubiquitty::Screen::ScrollLine* afterShrink = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(afterShrink && afterShrink->cells[0].codePoint == 'B');
    CHECK(afterShrink && afterShrink->lineAttribute == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(afterShrink && (afterShrink->mark & Ubiquitty::Screen::osc133MarkPromptStart));

    Ubiquitty::resize(t, 4, 20);
    const Ubiquitty::Screen::ScrollLine* afterGrow = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(afterGrow && afterGrow->cells[0].codePoint == 'B');
    CHECK(afterGrow && afterGrow->lineAttribute == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(afterGrow && (afterGrow->mark & Ubiquitty::Screen::osc133MarkPromptStart));

    // Grow the height enough to pull the banner back into the live screen; the
    // attribute + mark must land on its live row, not be reset to NORMAL/0.
    int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);
    Ubiquitty::resize(t, 4 + historyBefore + 2, 20);
    int liveRow = -1;
    for (int r = 0; r < t.rows; r++) {
        const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, r, 0);
        if (c && c->codePoint == 'B') {
            liveRow = r;
            break;
        }
    }
    CHECK(liveRow >= 0);
    CHECK(liveRow >= 0 && t.screens.primary.lineAttribute[liveRow] == Ubiquitty::lineAttributeDoubleWidth);
    // Read the live row's mark through the absolute accessor: live rows follow
    // the scrollback tail, so the live row's absolute index is scrollbackCount + r.
    CHECK(liveRow >= 0 && (Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + liveRow) & Ubiquitty::Screen::osc133MarkPromptStart));
}

TEST(testHistoryRowReadsOwnLineattrAndMark)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    feed(t, "\x1b]133;A\x1b\\"); // prompt mark on row 0
    feed(t,
        "\x1b#6"
        "WIDE"); // row 0 DECDWL
    feed(t, "\r\nP1\r\nP2\r\nP3\r\nP4"); // scroll WIDE into history; P-rows are plain
    int history = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(history >= 1);

    // Absolute row 0 is the WIDE banner in history: its own attribute is DWL + mark.
    CHECK(Ubiquitty::Screen::rowLineAttribute(&t, 0) == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkPromptStart);

    // Every live row is plain (NORMAL, unmarked) — the history read must not bleed
    // into the live index and the live read must not bleed into history.
    for (int r = 0; r < t.rows; r++) {
        CHECK(t.screens.primary.lineAttribute[r] == Ubiquitty::lineAttributeNormal);
        CHECK(Ubiquitty::Screen::rowLineAttribute(&t, history + r) == Ubiquitty::lineAttributeNormal);
    }
}

TEST(testWidthResizeShortRowByteIdentical)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);
    // Row 0: 12 distinct cells (a..l) with alternating SGR background, far short of 40.
    feed(t, "\x1b[41ma\x1b[42mb\x1b[43mc\x1b[44md\x1b[45me\x1b[46mf");
    feed(t, "\x1b[41mg\x1b[42mh\x1b[43mi\x1b[44mj\x1b[45mk\x1b[46ml\x1b[0m");
    // Snapshot the 12 cells before the resize.
    Ubiquitty::Cell before[12];
    for (int c = 0; c < 12; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, 0, c);
        CHECK(cell != nullptr);
        before[c] = *cell;
    }

    Ubiquitty::resize(t, 6, 38); // -2 columns (still far wider than 12)
    for (int c = 0; c < 12; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, 0, c);
        CHECK(cell && cell->codePoint == before[c].codePoint);
        CHECK(cell && cell->pen.backgroundIndex == before[c].pen.backgroundIndex);
    }
    // Column 12 stays blank — no cell shifted right into it.
    const Ubiquitty::Cell* gap = Ubiquitty::Screen::visibleCell(&t, 0, 0, 12);
    CHECK(gap && (gap->codePoint == ' ' || gap->codePoint == 0));

    Ubiquitty::resize(t, 6, 42); // +4 from 38 (grow past original)
    for (int c = 0; c < 12; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, 0, c);
        CHECK(cell && cell->codePoint == before[c].codePoint);
        CHECK(cell && cell->pen.backgroundIndex == before[c].pen.backgroundIndex);
    }
}

TEST(testWidthResizeCarriesSavedCursorUnderAlternate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feedNumberedLines(t, 30); // scrollback + prompt at row 23

    feed(t, "\x1b[?1049h"); // enter alt: saves the live cursor
    Ubiquitty::resize(t, 51, 196); // width change -> full rewrap path
    int reflowedRow = t.screens.primary.cursorY; // where the live cursor landed
    CHECK(reflowedRow != 23); // it did move
    feed(t, "\x1b[?1049l"); // quit -> Ubiquitty::Screen::restoreCursor

    CHECK(t.screens.primary.cursorY == reflowedRow); // restored at the reflowed row
}

UBIQUITTY_TEST_MAIN()
