// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What a resize does to content that no longer fits the way it did.
//
// Rewrapping a logical line across a new width, the scrollback that receives
// what falls off the top, the cursor's position through it, and the row
// attributes that have to travel with the line rather than the row.
//
// Reflow is the operation with the most state in flight: cell contents, wrap
// joins, marks, line attributes and the cursor all move at once, and each is
// owned by different code. The cases resize repeatedly rather than once, because
// the second reflow reads what the first wrote.

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "VT100Fixture.h"

TEST(testResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.columns == Ubiquitty::defaultColumns);
    CHECK(t.rows == Ubiquitty::defaultRows);
    Ubiquitty::resize(t, 40, 100);
    CHECK(t.columns == 100);
    CHECK(t.rows == 40);
    CHECK(t.screens.active->scrollBottom == 39);
}

TEST(testResizeGrowthUsesDefaultBlankPen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033[?1049h\033[44m\033[2;5H");

    Ubiquitty::resize(t, 3, 8);

    const Ubiquitty::Cell* exposed = cell(t, 1, 5);
    CHECK(exposed != nullptr);
    if (exposed != nullptr) {
        CHECK(exposed->codePoint == 0);
        CHECK(exposed->pen.foregroundIndex == Ubiquitty::colorDefault);
        CHECK(exposed->pen.backgroundIndex == Ubiquitty::colorDefault);
    }
    CHECK(t.screens.active->pen.backgroundIndex == 4);
}

TEST(testPrimaryReflowResizeGrowthUsesDefaultBlankPen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033[44m\033[2;5H");

    Ubiquitty::resize(t, 3, 8);

    const Ubiquitty::Cell* exposed = cell(t, 1, 5);
    CHECK(exposed != nullptr);
    if (exposed != nullptr) {
        CHECK(exposed->codePoint == 0);
        CHECK(exposed->pen.foregroundIndex == Ubiquitty::colorDefault);
        CHECK(exposed->pen.backgroundIndex == Ubiquitty::colorDefault);
    }
    CHECK(t.screens.active->pen.backgroundIndex == 4);
}

TEST(testHeightResizeGrowthUsesDefaultBlankPen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033[44m\033[3;5H");

    Ubiquitty::resize(t, 5, 5);

    const Ubiquitty::Cell* exposed = cell(t, 3, 0);
    CHECK(exposed != nullptr);
    if (exposed != nullptr) {
        CHECK(exposed->codePoint == 0);
        CHECK(exposed->pen.foregroundIndex == Ubiquitty::colorDefault);
        CHECK(exposed->pen.backgroundIndex == Ubiquitty::colorDefault);
    }
    CHECK(t.screens.active->pen.backgroundIndex == 4);
}

TEST(testArabicDisplayRowInScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // "بسم" on the top row, then scroll the screen until that row ages out.
    feed(t, "\xD8\xA8\xD8\xB3\xD9\x85");
    for (int i = 0; i < t.rows + 2; ++i)
        feed(t, "\r\n");

    const int hist = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(hist >= 1);
    // viewOffset == hist, row 0 resolves to the oldest history line (the
    // Arabic row). The flag must be set on that Ubiquitty::Screen::ScrollLine.
    CHECK(Ubiquitty::Screen::rowMayNeedShaping(t, hist, 0));
    // Logical store in history keeps the original codePoints.
    CHECK(Ubiquitty::Screen::visibleCell(&t, hist, 0, 0)->codePoint == 0x0628);

    // Same shaped + RTL-reversed visual order as the live assertion.
    Ubiquitty::Cell row[Ubiquitty::maxColumns];
    Ubiquitty::Screen::getDisplayRow(t, hist, 0, row, t.columns);
    CHECK(row[0].codePoint == 0xFEE2); // MEEM final
    CHECK(row[1].codePoint == 0xFEB4); // SEEN medial
    CHECK(row[2].codePoint == 0xFE91); // BEH initial
}

TEST(testClearScrollbackResetsLineattr)
{
    /// Cmd-K maps to Terminal::clearScrollback. It must leave a pristine
    /// screen: every visible row back to single-width/-height + no
    /// soft-wrap join. A DECDHL/DECDWL banner's attributes surviving this is
    /// the fzf-renders-at-2x ghost bug.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#3"); // row 0 DHL top
    feed(t, "\033[2;1H\033#4"); // row 1 DHL bottom
    feed(t, "\033[3;1H\033#6"); // row 2 DWL
    feed(t, "hello"); // soft-wrap-eligible content on a DWL row
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeDoubleHeightBottom);
    CHECK(t.screens.active->lineAttribute[2] == Ubiquitty::lineAttributeDoubleWidth);
    Ubiquitty::Screen::dropScrollback(&t);
    Ubiquitty::Screen::resetLiveRowMetadata(&t); // the second half of the user's clear
    // clearScrollback owns history + per-row line attributes, not cell content
    // (the frontend repaints the visible grid). Every visible row must be
    // back to single-size with no soft-wrap join. The protocol's ED 3 drops
    // history alone and leaves the live rows to their own metadata, which is
    // why the reset is its own verb here.
    for (int r = 0; r < t.rows; r++) {
        CHECK(t.screens.active->lineAttribute[r] == Ubiquitty::lineAttributeNormal);
        CHECK(t.screens.active->lineWrapped[r] == false);
    }
}

TEST(testResizePreservesLineattrInRange)
{
    /// Resize to larger rows: existing per-row attributes in [0..oldRows-1]
    /// survive. Newly-revealed rows are NORMAL (memset-zeroed extras).
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#6");
    feed(t, "\033[10;1H\033#3");
    Ubiquitty::resize(t, 40, 80);
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[9] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.active->lineAttribute[39] == Ubiquitty::lineAttributeNormal);
}

TEST(testXtwinops8ResizesTermModel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.rows == 24);
    CHECK(t.columns == 80);
    feed(t, "\033[8;30;100t");
    CHECK(t.rows == 30);
    CHECK(t.columns == 100);
}

TEST(testFullScreenScrollbackCaptures100Lines)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Screen::ScrollLine* first;
    const Ubiquitty::Screen::ScrollLine* last;

    Ubiquitty::hardReset(t);
    feedNumberedLines(t, 100);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 76);
    first = Ubiquitty::Screen::scrollbackLine(&t, 0);
    last = Ubiquitty::Screen::scrollbackLine(&t, 75);
    CHECK(first != nullptr);
    CHECK(last != nullptr);
    if (first) {
        CHECK(first->cells[0].codePoint == 'L');
        CHECK(first->cells[1].codePoint == '0');
        CHECK(first->cells[2].codePoint == '0');
        CHECK(first->cells[3].codePoint == '0');
    }
    if (last) {
        CHECK(last->cells[0].codePoint == 'L');
        CHECK(last->cells[1].codePoint == '0');
        CHECK(last->cells[2].codePoint == '7');
        CHECK(last->cells[3].codePoint == '5');
    }
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 3)->codePoint == '6');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 1, 0, 3)->codePoint == '5');
}

TEST(testBottomMarginScrollCapturesScrollbackAndBlankPen)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Screen::ScrollLine* first;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "\033[32mL000\r\nL001\r\nL002\r\nL003\r\nL004");
    feed(t, "\033[32m\033[5;1H\n");

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 1);
    first = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(first != nullptr);
    if (first) {
        CHECK(first->cells[0].codePoint == 'L');
        CHECK(first->cells[1].codePoint == '0');
        CHECK(first->cells[2].codePoint == '0');
        CHECK(first->cells[3].codePoint == '0');
        CHECK(!first->wrapped);
    }
    CHECK(cell(t, 4, 0)->codePoint == 0);
    CHECK(cell(t, 4, 0)->pen.foregroundIndex == 2);
}

TEST(testAlternateScreenDoesNotPopulateScrollback)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033[?1049h");
    feedNumberedLines(t, 100);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    feed(t, "\033[?1049l");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
}

TEST(testPartialScrollRegionDoesNotPopulateScrollback)
{
    Ubiquitty::Terminal t;
    int i;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feed(t, "\033[2;4r\033[4;1H");
    for (i = 0; i < 20; i++)
        feed(t, "\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
}

TEST(testVisibleCellMapsHistoryAndLiveRows)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 30);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 25);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 3)->codePoint == '5');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 3, 0, 3)->codePoint == '2');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 3, 2, 3)->codePoint == '4');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 3, 3, 3)->codePoint == '5');
}

TEST(testClearBufferClearsScrollbackAndScreen)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feedNumberedLines(t, 30);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=12;/wAA\033\\");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);
    CHECK(findKittyImage(t, 12) != nullptr);
    CHECK(cell(t, 4, 0)->codePoint != ' ');

    Ubiquitty::clearBuffer(&t);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(findKittyImage(t, 12) == nullptr);
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 4, 0)->codePoint == 0);
}

TEST(testResizeReclampsScrollRegion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 30, 80);
    // DECSTBM 20..30 — region in the lower portion of the screen.
    feed(t, "\033[20;30r");
    CHECK(t.screens.active->scrollTop == 19);
    CHECK(t.screens.active->scrollBottom == 29);

    /// Shrink to 10 rows: the whole region is past the new bottom. The
    /// resize path must recover with a full-screen region.
    Ubiquitty::resize(t, 10, 80);
    CHECK(t.screens.active->scrollTop >= 0);
    CHECK(t.screens.active->scrollBottom >= 0);
    CHECK(t.screens.active->scrollTop < t.screens.active->scrollBottom);
    CHECK(t.screens.active->scrollBottom <= 9);

    // Linefeed at the (new) bottom must still scroll instead of getting
    // stuck.
    t.screens.active->cursorX = 0;
    t.screens.active->cursorY = 9;
    feed(t, "BOTTOM\n");
    CHECK(t.screens.active->cursorY == 9);
}

TEST(testResizePreservesExplicitTabStops)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // Clear all default tab stops and set one custom column.
    for (i = 0; i < Ubiquitty::maxColumns; i++)
        t.tabStops[i] = false;
    t.tabStops[3] = true;

    /// Grow the terminal — the custom tab stop on column 3 must survive,
    /// and new columns get the every-8 default.
    Ubiquitty::resize(t, 24, 120);
    CHECK(t.tabStops[3]);
    CHECK(!t.tabStops[7]); // old column stayed cleared
    CHECK(t.tabStops[88]); // new column, multiple of 8
    CHECK(!t.tabStops[81]); // new column, not on the grid
}

TEST(testReflowNarrowToWideRejoins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    // 15 chars on a 10-column screen: wraps to row 0 (10) + row 1 (5).
    feed(t, "ABCDEFGHIJKLMNO");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 9)->codePoint == 'J');
    CHECK(t.screens.active->lineWrapped[0]); // row 0 soft-wraps into row 1
    CHECK(cell(t, 1, 0)->codePoint == 'K');
    CHECK(!t.screens.active->lineWrapped[1]);

    /// Widen to 20 columns: the whole logical line fits on one row.
    Ubiquitty::resize(t, 6, 20);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 14)->codePoint == 'O');
    CHECK(!t.screens.active->lineWrapped[0]); // no longer wrapped
    CHECK(cell(t, 1, 0)->codePoint == 0); // row 1 now blank
}

TEST(testReflowWideToNarrowResplits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);
    feed(t, "ABCDEFGHIJKLMNO"); // one row, no wrap
    CHECK(!t.screens.active->lineWrapped[0]);

    Ubiquitty::resize(t, 6, 10);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 9)->codePoint == 'J');
    CHECK(t.screens.active->lineWrapped[0]); // re-split: row 0 wraps
    CHECK(cell(t, 1, 0)->codePoint == 'K');
    CHECK(cell(t, 1, 4)->codePoint == 'O');
    CHECK(!t.screens.active->lineWrapped[1]); // final row of the logical line
}

TEST(testReflowHardBreakStays)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "ABC\r\nDEF");
    CHECK(!t.screens.active->lineWrapped[0]); // hard break after ABC

    Ubiquitty::resize(t, 6, 20);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    CHECK(cell(t, 0, 3)->codePoint == 0); // DEF did not join row 0
    CHECK(!t.screens.active->lineWrapped[0]);
    CHECK(cell(t, 1, 0)->codePoint == 'D');
    CHECK(cell(t, 1, 2)->codePoint == 'F');
}

TEST(testReflowWideCharacterStraddleWrapsWhole)
{
    Ubiquitty::Terminal t;
    // U+4E00 is width-2. Feed "AAAA" + one wide char on a wide screen so
    // it lives unwrapped, then narrow to a width where the wide pair
    // would straddle column boundary.
    unsigned char wide[] = { 0xE4, 0xB8, 0x80 }; // U+4E00
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "AAAA");
    feed(t, wide); // lead at column 4, cont at 5
    CHECK(cell(t, 0, 4)->codePoint == 0x4E00);
    CHECK(cell(t, 0, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));

    /// Narrow to 5 columns: "AAAA" fills columns 0-3; the wide char needs columns
    /// 4-5 but only column 4 remains on row 0, so it wraps WHOLE to row 1.
    Ubiquitty::resize(t, 6, 5);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 3)->codePoint == 'A');
    CHECK(t.screens.active->lineWrapped[0]);
    // The wide pair sits intact at the start of row 1, not split.
    CHECK(cell(t, 1, 0)->codePoint == 0x4E00);
    CHECK(cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 1, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

TEST(testReflowWideWrapRoundtripNoStraySpace)
{
    Ubiquitty::Terminal t;
    unsigned char wide[] = { 0xE4, 0xB8, 0x80 }; // U+4E00, width-2
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "AAAA");
    feed(t, wide); // row 0: A A A A 中(4,5)

    // Narrow so 中 wraps whole to row 1, then widen back several times.
    for (int i = 0; i < 4; i++) {
        Ubiquitty::resize(t, 6, 5);
        Ubiquitty::resize(t, 6, 10);
    }

    // Back at width 10 the logical line is exactly "AAAA中" — no stray blank
    // between the 'A' run and the wide char, whatever the resize history.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'A');
    CHECK(cell(t, 0, 2)->codePoint == 'A');
    CHECK(cell(t, 0, 3)->codePoint == 'A');
    CHECK(cell(t, 0, 4)->codePoint == 0x4E00); // wide char immediately follows
    CHECK(cell(t, 0, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

TEST(testReflowCursorStaysOnCharacter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "ABCDEFGHIJKLMNO"); // cursor sits after 'O'
    // Park the cursor ON 'L' (logical index 11 → row 1, column 1 at width 10).
    feed(t, "\033[2;2H");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 1);
    CHECK(cell(t, t.screens.active->cursorY, t.screens.active->cursorX)->codePoint == 'L');

    Ubiquitty::resize(t, 6, 20); // logical line now one row
    CHECK(cell(t, t.screens.active->cursorY, t.screens.active->cursorX)->codePoint == 'L');
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 11);
}

TEST(testReflowScrollbackRewraps)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    // Push a long wrapped line, then enough hard lines to scroll the
    // wrapped line into scrollback.
    feed(t, "ABCDEFGHIJKLMNO\r\n"); // wraps row0+row1
    feed(t, "p1\r\np2\r\np3\r\np4");
    int before = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(before > 0);

    /// Widen: the wrapped logical line in history must rejoin onto one
    /// scrollback row; its cells stay in order.
    Ubiquitty::resize(t, 3, 20);
    /// Find the scrollback line that starts with 'A' and verify it now
    /// holds the full 15-char logical line on a single line.
    int found = -1;
    for (int i = 0; i < Ubiquitty::Screen::scrollbackCount(&t); i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (line && line->length > 0 && line->cells[0].codePoint == 'A') {
            found = i;
            break;
        }
    }
    CHECK(found >= 0);
    if (found >= 0) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, found);
        CHECK(line->cells[0].codePoint == 'A');
        CHECK(line->cells[14].codePoint == 'O');
        CHECK(!line->wrapped); // rejoined: no soft-wrap continuation
    }
}

TEST(testReflowAlternateScreenNotRewrapped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    // Enter the alternate screen (DECSET 1049) and draw a wrapped line.
    feed(t, "\033[?1049h");
    feed(t, "ABCDEFGHIJKLMNO");
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 0)->codePoint == 'K');

    /// Widen: the alternate line is not rejoined — row 1 keeps 'K' where
    /// a reflow would have pulled it up onto row 0.
    Ubiquitty::resize(t, 6, 20);
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(cell(t, 1, 0)->codePoint == 'K'); // still on row 1: no rewrap
}

TEST(testReflowHeightOnlyNoRewrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "ABCDEFGHIJKLMNO");
    CHECK(t.screens.active->lineWrapped[0]);
    CHECK(cell(t, 1, 0)->codePoint == 'K');

    Ubiquitty::resize(t, 12, 10); // rows change, columns stay
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 0)->codePoint == 'K'); // split point unchanged
    CHECK(t.screens.active->lineWrapped[0]);
}

TEST(testReflowTrimsTrailingBlanks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    // "AB" then a hard break: the rest of row 0 is blank padding.
    feed(t, "AB\r\nCD");
    Ubiquitty::resize(t, 6, 20);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    // CD stays its own logical line; AB's trailing blanks were trimmed so
    // CD did not get pulled onto row 0.
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 'C');
}

TEST(testClearScrollbackBeforeDropsRowsAndKeepsMarks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// Feed enough rows that scrollback fills up. Mark a few specific
    /// rows with OSC 133 A; the absolute row index for each marked row
    /// equals the line index because the first line is index 0 and rows
    /// only ever scroll forward.
    const int totalLines = 40;
    const int markRows[3] = { 3, 7, 12 };
    int mi = 0;
    for (int i = 0; i < totalLines; i++) {
        if (mi < 3 && i == markRows[mi]) {
            feed(t, "\033]133;A\007");
            mi++;
        }
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "R%03d", i);
        feed(t, buffer);
        if (i + 1 < totalLines)
            feed(t, "\r\n");
    }
    const int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);
    // All three mark rows must have landed in scrollback (they're all
    // below the live-screen top).
    CHECK(historyBefore > markRows[2]);
    CHECK(Ubiquitty::Screen::rowMark(&t, markRows[0]) == Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(Ubiquitty::Screen::rowMark(&t, markRows[1]) == Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(Ubiquitty::Screen::rowMark(&t, markRows[2]) == Ubiquitty::Screen::osc133MarkPromptStart);
    /// Snapshot a live-screen cell so we can confirm it's preserved.
    const uint32_t liveCodePoint = cell(t, 0, 0)->codePoint;
    /// Drop rows 0..6.
    const int dropTo = 7;
    Ubiquitty::Screen::dropScrollbackBefore(&t, dropTo);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == historyBefore - dropTo);
    // Marks ride along: old row 7 → new row 0; old row 12 → new row 5.
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(Ubiquitty::Screen::rowMark(&t, 12 - dropTo) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Unmarked rows stay unmarked.
    CHECK(Ubiquitty::Screen::rowMark(&t, 1) == 0);
    CHECK(Ubiquitty::Screen::rowMark(&t, 4) == 0);
    // Live screen content unchanged.
    CHECK(cell(t, 0, 0)->codePoint == liveCodePoint);
    /// Content of the now-oldest scrollback line matches what was at the
    /// old absolute row 7 — i.e. its first cell still says 'R'.
    const Ubiquitty::Screen::ScrollLine* l0 = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(l0 != nullptr);
    if (l0)
        CHECK(l0->cells[0].codePoint == 'R');
}

TEST(testClearScrollbackBeforeEdges)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// No scrollback yet: any call is a no-op.
    Ubiquitty::Screen::dropScrollbackBefore(&t, 5);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    // Seed history.
    for (int i = 0; i < t.rows + 10; i++)
        feed(t, "X\r\n");
    const int before = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(before > 0);
    /// absoluteRow <= 0 drops nothing.
    Ubiquitty::Screen::dropScrollbackBefore(&t, 0);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == before);
    Ubiquitty::Screen::dropScrollbackBefore(&t, -3);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == before);
    /// absoluteRow > count clamps to count → full drain.
    Ubiquitty::Screen::dropScrollbackBefore(&t, before + 100);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
}

TEST(testDropRowsBeforeLiveScreenShift)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Three prompt-marked rows on the live screen, no scrollback.
    feed(t, "\033]133;A\007P1\r\n");
    feed(t, "out1\r\n");
    feed(t, "\033]133;A\007P2\r\n");
    feed(t, "out2\r\n");
    feed(t, "\033]133;A\007P3");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    const int p3Row = t.screens.active->cursorY;
    /// Previous-prompt search with ceiling = cursorY finds P2's row.
    const int prev = Ubiquitty::Screen::findPromptMarkBefore(&t, p3Row, Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(prev == 2);
    const int cursorYBefore = t.screens.active->cursorY;
    Ubiquitty::Screen::dropRowsBefore(&t, prev);
    // No scrollback created; the rows above prev were destructively
    // discarded, not captured.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    // The P2 row is now at row 0 of the live screen.
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    CHECK(cell(t, 0, 0)->codePoint == 'P');
    CHECK(cell(t, 0, 1)->codePoint == '2');
    // P3 row remains marked but shifted up by `prev`.
    CHECK(Ubiquitty::Screen::rowMark(&t, p3Row - prev) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Cursor followed the shift.
    CHECK(t.screens.active->cursorY == cursorYBefore - prev);
}

TEST(testDropRowsBeforeScrollbackOnly)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int i = 0; i < t.rows + 10; i++)
        feed(t, "X\r\n");
    const int before = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(before >= 5);
    const int cyBefore = t.screens.active->cursorY;
    Ubiquitty::Screen::dropRowsBefore(&t, 5);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == before - 5);
    CHECK(t.screens.active->cursorY == cyBefore);
}

/// Enabling 2048 emits an immediate report; a subsequent resize emits
/// another. With a cell pixel size published, the pixel fields are
/// cellPixel × dims. Format: CSI 48 ; rows ; columns ; pixh ; pixw t.
TEST(testInBandResizeEmitsReport)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    // Publish a cell pixel size so the report carries real pixel dims.
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    // Default size is 80x24. Enable 2048 → immediate report.
    feed(t, "\033[?2048h");
    CHECK(t.modes.inBandResize == true);
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    // rows=24 columns=80 pixh=16*24=384 pixw=8*80=640
    CHECK(strcmp(outputBuffer, "\033[48;24;80;384;640t") == 0);
    /// A resize emits another report with the new geometry.
    Ubiquitty::resize(t, 30, 100);
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[48;30;100;480;800t") == 0);
}

/// With 2048 disabled, a resize emits nothing.
TEST(testInBandResizeSilentWhenOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    Ubiquitty::resize(t, 30, 100);
    int n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testGroundLfFastPathBottomScrollAppendsScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 8);

    feed(t, "TOP\r\nMID\r\nBOT");
    CHECK(t.screens.active->cursorY == 2);
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 2);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 1);

    const Ubiquitty::Screen::ScrollLine* first = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(first != nullptr);
    if (first) {
        CHECK(first->cells[0].codePoint == 'T');
        CHECK(first->cells[1].codePoint == 'O');
        CHECK(first->cells[2].codePoint == 'P');
        CHECK(!first->wrapped);
    }
    CHECK(cell(t, 2, 0)->codePoint == 0);
}

UBIQUITTY_TEST_MAIN()