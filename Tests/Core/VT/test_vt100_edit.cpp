// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The controls that change what is already on screen without moving the cursor
// through it.
//
// Erase (ECH, EL, ED), insert and delete of characters and lines, the scroll
// controls, and the VT400 rectangle operations that apply the same edits to a
// region rather than a row. Alongside them the guarded-area machinery — DECSCA,
// DECSEL, DECSED, DECSERA — which decides for every one of the above whether a
// cell survives.
//
// Protection belongs with editing rather than beside it: a protected cell is
// defined entirely by what an erase does when it reaches one, so the two are a
// single rule seen from two directions.

#include "Core/Screen/Addressing.h"
#include "Core/Triggers/Engine.h"
#include "Harness.h"
#include "VT100Fixture.h"

/// Empty lists (the default) pass every OSC: an OSC 2 title still lands. A
/// deny list blocks just the named code; an allow list makes the set
/// exclusive (only listed codes run). A blocked code is consumed cleanly and
/// leaves the title unchanged, so a filtered sequence costs nothing downstream.
/// parseOSCList: comma/space separated decimal codes; junk tokens skipped.
/// parseDCSList: case-insensitive kind names; unknown names skipped.
/// DCS gate: deny DECRQSS → a DECRQSS query yields no host reply; with the
/// gate cleared the same query replies.
/// DECUDK payload travelling through a session log: the log must
/// capture the entire wire (DCS + payload + ST) verbatim, with all
/// ESC bytes intact.
/// csi-miss diag hook: an unknown prefixed CSI ("CSI ? 9999 h") must
/// emit one csi-miss event encoding (priv<<16)|(int<<8)|final. A plain
/// unknown CSI emits csi-miss with just `final` in detail.
/// DECSET 44 (margin bell): fires once when the cursor lands on the
/// threshold column, then disarms until the cursor moves back to a
/// strictly lower X (CR, BS, NEL, linefeed wrap). Overwriting the
/// threshold cell repeatedly via BS must not re-fire the bell.
/// C0 control characters embedded inside a CSI sequence must execute in
/// place (cursor motion etc.) while the CSI continues accumulating +
/// dispatches normally — not abort the sequence (vttest cursor-control-
/// in-ESC + insert/delete).
/// Under a legacy (non-UTF-8) input encoding, raw C1 controls must
/// still reach the parser instead of being reinterpreted as characterSet
/// characters (Latin-1's 0x80-0x9F range maps to real accented/graphic
/// characters, unlike its identity-mapped 0x00-0x7F range that C0 controls
/// already survive). 0x9B is the 8-bit form of CSI.
/// A C1 control EMBEDDED mid-buffer must split the batched transcode run and
/// reach the parser raw; characterSet bytes on both sides of it still decode. One
/// processBytes call so the whole sequence rides a single batched entry.
/// Multibyte legacy encodings keep their 0x80-0x9F lead/shift bytes: C1
/// routing applies to single-byte encodings only, or a Shift-JIS lead
/// (0x81-0x9F) / EUC single-shift (0x8E/0x8F) would be stolen mid-sequence.
/// The other C1 controls (IND/NEL/HTS/RI/DCS/OSC/SS2/SS3) reach the parser
/// under a legacy encoding too, not just CSI.
static void writeDisplayProbeRow(Ubiquitty::CellGrid* display, int row, int seq)
{
    Ubiquitty::Cell cells[8];
    Ubiquitty::Cell blank = *display->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(0));
    const int width = (seq % 6) + 1;
    for (int c = 0; c < width; ++c) {
        cells[c] = blank;
        cells[c].codePoint = static_cast<uint32_t>('A' + ((seq + c) % 26));
    }
    Ubiquitty::RowView view = { cells, width };
    display->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(0), view);
    display->setLineAttribute(row, static_cast<uint8_t>(seq % 4));
    display->setRowMark(row, static_cast<uint8_t>((seq % 7) + 1));
    display->setRowWrapped(row, static_cast<bool>(seq % 2));
}

TEST(testDECRARAOutOfBoundsRectClamped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // The crashing input: DECRARA (CSI Pt;Pl;Pb;Pr $ t) with top far past the
    // last row. Reaching the next line at all means it did not abort.
    feed(t, "\033[999;1;1;1$t");
    // DECOM origin mode pushing top past the scroll region's bottom.
    feed(t, "\033[?6h\033[5;10r\033[99;1;1;1$t\033[r\033[?6l");
    // Over-large left column, and a fully out-of-range rect.
    feed(t, "\033[1;999;5;999$t");
    feed(t, "\033[200;200;255;255$t");
    // Sibling rect operations through the same helper: DECFRA fill, DECERA erase.
    feed(t, "\033[66;500;1;5;5$x"); // DECFRA fill 'B' into an over-range rect
    feed(t, "\033[900;1;1;1$z"); // DECERA erase an out-of-range rect
    // The term survived and is still usable.
    feed(t, "\033[1;1HOK");
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 'O');
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == 'K');
}

TEST(testSlSrScroll)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[2 @"); // SL 2: shift left, blank the right
    CHECK(cell(t, 0, 0)->codePoint == 'C');
    CHECK(cell(t, 0, 1)->codePoint == 'D');
    CHECK(cell(t, 0, 2)->codePoint == 'E');
    CHECK(cell(t, 0, 3)->codePoint == 0);

    Ubiquitty::Terminal t2;
    Ubiquitty::hardReset(t2);
    feed(t2, "ABCDE");
    feed(t2, "\033[2 A"); // SR 2: shift right, blank the left
    CHECK(cell(t2, 0, 0)->codePoint == 0);
    CHECK(cell(t2, 0, 1)->codePoint == 0);
    CHECK(cell(t2, 0, 2)->codePoint == 'A');
    CHECK(cell(t2, 0, 3)->codePoint == 'B');
    CHECK(cell(t2, 0, 4)->codePoint == 'C');
}

// The run batching paths bail to the per-byte driver when the cursor sits
// beyond the narrowed right margin: each fast path refuses the span, the
// per-byte emit still writes at the cursor, and the row holds the text.
TEST(testRunBatchBailsOutsideNarrowedMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 15);
    feed(t, "\033[?69h\033[2;8s"); // DECSLRM cols 2..8
    feed(t, "\033[1;10H"); // cursor at index 9 — beyond the right margin
    feed(t, "ab"); // ASCII run refuses the span; 'b' wraps to the margin row
    CHECK(cell(t, 0, 9)->codePoint == 'a');
    CHECK(cell(t, 1, 1)->codePoint == 'b');
    feed(t, "\xE2\x98\xBA"); // cluster run refuses the span
    CHECK(cell(t, 1, 2)->codePoint == 0x263A);
    feed(t, "\xC3\xA9"); // text run refuses the span
    CHECK(cell(t, 1, 3)->codePoint == 0xE9);
    feed(t, "a\xCC\x81"); // ASCII-combining run refuses the span
    CHECK(cell(t, 1, 4)->codePoint == 'a');
}

// A full-width row evaluated at fill (autowrap) re-evaluates on a genuine
// rewrite: the fill hook arms the guard, a CR + rewrite resets it, so the row
// can match a trigger rule again once its content changes.
TEST(testFilledRowRewriteResetsTriggerGuard)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    auto engine = std::make_unique<Ubiquitty::Triggers::Engine>();
    engine->addRule("nomatch", Ubiquitty::Triggers::Action::Highlight, nullptr, Ubiquitty::CaseSensitivity::Sensitive);
    t.triggersEngine = std::move(engine);

    feed(t, "xxxx"); // fill row 0 to the margin -> fill hook evaluates
    CHECK(t.triggerWrapEvaluatedRow == 0);
    feed(t, "\r"); // clear the pending wrap
    CHECK(t.screens.active->wrapPending == false);
    feed(t, "yy"); // fast-path rewrite of row 0 drops the guard
    CHECK(t.triggerWrapEvaluatedRow == -1);
}

TEST(testDECSLRMIlDlWithinRect)
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
    feed(t, "\033[3;8H\033[2L"); // IL 2 — cursor inside margins
    feed(t, "\033[3;8H\033[1M"); // DL 1
    for (int r = 0; r < 10; r++) {
        CHECK(cell(t, r, 0)->codePoint == 'L');
        CHECK(cell(t, r, 19)->codePoint == 'R');
    }
}

TEST(testDECSLRMScrollPreservesOutsideColumns)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    for (int r = 0; r < 5; r++) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\033[%d;1HL\033[%d;10HR", r + 1, r + 1);
        feed(t, buffer);
    }
    feed(t, "\033[?69h\033[2;4r\033[3;8s\033[4;3H\n");

    for (int r = 0; r < 5; r++) {
        CHECK(cell(t, r, 0)->codePoint == 'L');
        CHECK(cell(t, r, 9)->codePoint == 'R');
    }
}

TEST(testDECSLRMScrollPreservesRowMetadata)
{
    // Setting DECLRMM makes every line single width (VT510 DECLRMM), so the
    // metadata a margin scroll carries is the OSC 133 mark; the line size is
    // single width throughout.
    // --- Partial-width: metadata kept on its row, region-outside rows safe.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 6, 12);
        // Mark row 1 with a prompt mark + DECDWL; it sits inside the vertical
        // region [1..3] (DECSTBM 2;4r) but the scroll is column-restricted.
        feed(t, "\033[2;1H\033]133;A\007\033#6"); // row 1: PromptStart + DWL
        CHECK(t.screens.active->mark[1] == Ubiquitty::Screen::osc133MarkPromptStart);
        CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeDoubleWidth);
        // Row 5 (outside the region) is marked too — it must never move.
        feed(t, "\033[6;1H\033]133;A\007\033#6");
        CHECK(t.screens.active->mark[5] == Ubiquitty::Screen::osc133MarkPromptStart);
        // DECSTBM rows 2..4 (0-based 1..3), DECSLRM partial cols 4..9, then a
        // region scroll up via LF at the bottom margin.
        feed(t, "\033[?69h");
        CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeNormal);
        CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeNormal);
        feed(t, "\033[2;4r\033[4;10s\033[4;6H\n");
        // Partial-width policy: row 1's mark stays on row 1 (not dropped).
        CHECK(t.screens.active->mark[1] == Ubiquitty::Screen::osc133MarkPromptStart);
        // Row 5, outside the vertical region, is wholly untouched.
        CHECK(t.screens.active->mark[5] == Ubiquitty::Screen::osc133MarkPromptStart);
    }

    // --- Full-width DECLRMM scroll: metadata follows the cells. With margins
    // spanning the whole row the scroll guard takes the CellGrid::scrollRegion
    // path (the same RowMeta rotation scrollRect's full-width branch reuses), so
    // a prompt mark inside the region moves with its cells.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 6, 12);
        feed(t, "\033[4;1H\033]133;A\007");
        // DECLRMM on, but margins span the whole row (1..columns).
        feed(t, "\033[?69h\033[1;12s");
        CHECK(t.modes.leftRightMarginMode == true);
        CHECK(t.screens.active->scrollLeft == 0);
        CHECK(t.screens.active->scrollRight == 11);
        // Then scroll the whole screen up 2.
        CHECK(t.screens.active->mark[3] == Ubiquitty::Screen::osc133MarkPromptStart);
        CHECK(t.screens.active->lineAttribute[3] == Ubiquitty::lineAttributeNormal);
        feed(t, "\033[6;1H\n\n"); // LF x2 at bottom → scroll up 2
        // The mark followed the row up by two: now on row 1, gone from row 3.
        CHECK(t.screens.active->mark[1] == Ubiquitty::Screen::osc133MarkPromptStart);
        CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeNormal);
        CHECK(t.screens.active->mark[3] == 0);
        // Revealed bottom rows carry no stale mark.
        CHECK(t.screens.active->mark[5] == 0);
    }
}

TEST(testScrollUpShiftsLineattr)
{
    /// Row 5 DWL, scroll up by 2 — DWL attribute lands on row 3.
    /// New rows revealed at the bottom (22..23) reset to NORMAL.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6;1H\033#6"); // row 5 (0-based) DWL
    CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeDoubleWidth);
    // Trigger Ubiquitty::Screen::scrollUp(2) by writing 2 LFs at the scroll bottom.
    feed(t, "\033[24;1H");
    feed(t, "\n\n");
    CHECK(t.screens.active->lineAttribute[3] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.active->lineAttribute[22] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.active->lineAttribute[23] == Ubiquitty::lineAttributeNormal);
}

TEST(testScrollDownShiftsLineattr)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Row 5 DWL, scroll down 2 via RI from row 0.
    feed(t, "\033[6;1H\033#6");
    CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeDoubleWidth);
    feed(t, "\033[1;1H");
    feed(t, "\033M\033M"); // RI twice → Ubiquitty::Screen::scrollDown(2) at top
    CHECK(t.screens.active->lineAttribute[7] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[5] != Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeNormal);
}

TEST(testEd2ClearsAllLineattr)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#6");
    feed(t, "\033[5;1H\033#3");
    feed(t, "\033[10;1H\033#4");
    feed(t, "\033[2J"); // ED 2 — clear entire screen
    for (int r = 0; r < t.rows; r++)
        CHECK(t.screens.active->lineAttribute[r] == Ubiquitty::lineAttributeNormal);
}

TEST(testEd0ClearsLineattrBelowCursorOnly)
{
    /// ED 0 erases [cursor..end]. The cursor's row is only partially
    /// erased (Ubiquitty::Screen::eraseInLine handles the right half), so its line attribute
    /// survives — only rows STRICTLY BELOW the cursor are fully
    /// cleared, and those reset to NORMAL.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#6"); // row 0 DWL
    feed(t, "\033[5;1H\033#3"); // row 4 DHL_TOP
    feed(t, "\033[10;1H\033#4"); // row 9 DHL_BOT
    feed(t, "\033[5;1H"); // cursor to row 4
    feed(t, "\033[J"); // ED 0 — cursor row + everything below
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth); // above
    CHECK(t.screens.active->lineAttribute[4] == Ubiquitty::lineAttributeDoubleHeightTop); // kept
    CHECK(t.screens.active->lineAttribute[9] == Ubiquitty::lineAttributeNormal); // below
}

TEST(testDECALNClearsAllLineattr)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033#6");
    feed(t, "\033[5;1H\033#3");
    feed(t, "\033#8"); // DECALN
    for (int r = 0; r < t.rows; r++)
        CHECK(t.screens.active->lineAttribute[r] == Ubiquitty::lineAttributeNormal);
}

TEST(testInsertlinesShiftsLineattrDown)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6;1H\033#6"); // row 5 DWL
    feed(t, "\033[3;1H\033[2L"); // IL 2 at row 2
    CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.active->lineAttribute[7] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[2] == Ubiquitty::lineAttributeNormal); // freshly blank
    CHECK(t.screens.active->lineAttribute[3] == Ubiquitty::lineAttributeNormal);
}

TEST(testDeletelinesShiftsLineattrUp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6;1H\033#6"); // row 5 DWL
    feed(t, "\033[3;1H\033[2M"); // DL 2 at row 2
    CHECK(t.screens.active->lineAttribute[5] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.active->lineAttribute[3] == Ubiquitty::lineAttributeDoubleWidth);
}

TEST(testDECFRAFillRectangleWithCharacter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Fill rows 2..4 columns 3..6 with 'X' (ch=88).
    feed(t, "\033[88;2;3;4;6$x");
    // Verify a sampling of cells inside + outside the rect.
    CHECK(cell(t, 1, 2)->codePoint == 'X'); // top-left
    CHECK(cell(t, 1, 5)->codePoint == 'X'); // top-right
    CHECK(cell(t, 3, 2)->codePoint == 'X'); // bot-left
    CHECK(cell(t, 3, 5)->codePoint == 'X'); // bot-right
    CHECK(cell(t, 0, 2)->codePoint == 0); // above
    CHECK(cell(t, 4, 2)->codePoint == 0); // below
    CHECK(cell(t, 1, 1)->codePoint == 0); // left of
    CHECK(cell(t, 1, 6)->codePoint == 0); // right of
}

TEST(testDECFRAFillCharacterRange)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Pch=183 = U+00B7 MIDDLE DOT, a legal GR graphic — fills as that code point.
    feed(t, "\033[183;1;1;1;5$x");
    CHECK(cell(t, 0, 0)->codePoint == 0x00B7);
    CHECK(cell(t, 0, 4)->codePoint == 0x00B7);

    // Out-of-range Pch (0x7F DEL) leaves the rect untouched — not space-filled.
    feed(t, "\033[2;1HVWXYZ"); // row 2 (1-based) content
    feed(t, "\033[127;2;1;2;5$x"); // Pch=127 out of range → no-op
    CHECK(cell(t, 1, 0)->codePoint == 'V');
    CHECK(cell(t, 1, 4)->codePoint == 'Z');
}

TEST(testDECFRAResolvesTheFillCharacterThroughTheInvokedSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // 'q' in DEC Special Graphics is the horizontal line.
    feed(t, "\033(0\033[113;1;1;1;3$x\033(B");
    CHECK_EQ(cell(t, 0, 0)->codePoint, 0x2500u);
    CHECK_EQ(cell(t, 0, 2)->codePoint, 0x2500u);
    CHECK_EQ(cell(t, 0, 3)->codePoint, 0u);
    // With ASCII invoked again the same code is the letter.
    feed(t, "\033[113;2;1;2;3$x");
    CHECK_EQ(cell(t, 1, 0)->codePoint, static_cast<uint32_t>('q'));
    // GL follows a locking shift: G1 holds the graphics set, and SO invokes it.
    feed(t, "\033)0\016\033[113;3;1;3;2$x\017");
    CHECK_EQ(cell(t, 2, 1)->codePoint, 0x2500u);
}

TEST(testDECICDECDCInsertDeleteColumns)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDEF\r\nGHIJKL");
    // Cursor to row 1, column 3 (1-based) = 0-based col 2, then DECIC 2.
    feed(t, "\033[1;3H\033[2'}");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0); // inserted blank
    CHECK(cell(t, 0, 3)->codePoint == 0); // inserted blank
    CHECK(cell(t, 0, 4)->codePoint == 'C'); // shifted right
    // The whole region shifts — row 2 too.
    CHECK(cell(t, 1, 2)->codePoint == 0);
    CHECK(cell(t, 1, 4)->codePoint == 'I');
    // The cursor did not move.
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 2);

    // DECDC 2 undoes the shift: both rows return to the original content.
    feed(t, "\033[2'~");
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    CHECK(cell(t, 0, 5)->codePoint == 'F');
    CHECK(cell(t, 1, 2)->codePoint == 'I');
}

TEST(testDECICRegionBounds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AAAA\r\nBBBB\r\nCCCC");
    feed(t, "\033[1;2r"); // region rows 1..2; row 3 is outside
    // Cursor inside the region: shift rows 1..2 only.
    feed(t, "\033[1;1H\033[1'}");
    CHECK(cell(t, 0, 0)->codePoint == 0); // row 1 shifted
    CHECK(cell(t, 1, 0)->codePoint == 0); // row 2 shifted
    CHECK(cell(t, 2, 0)->codePoint == 'C'); // row 3 untouched
    // Cursor OUTSIDE the vertical margins: DECIC is a no-op — the region's rows
    // must not shift. Region rows 3..15 (0-based 2..14), cursor parked at row 1.
    feed(t, "\033[r\033[3;15r\033[1;1H\033[1'}");
    CHECK(cell(t, 2, 0)->codePoint == 'C'); // region row untouched: no-op held
}

TEST(testDECERAErasesRect)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Pre-fill row 0 with 'A' then erase a sub-rect.
    feed(t, "ABCDEFGHIJ");
    feed(t, "\033[1;3;1;6$z"); // erase row 1, columns 3..6 (1-based)
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0); // erased
    CHECK(cell(t, 0, 5)->codePoint == 0); // erased
    CHECK(cell(t, 0, 6)->codePoint == 'G');
}

TEST(testRectangularEraseRendition)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // DECERA over reverse-video text.
    feed(t, "\033[H\033[7mABCDE"); // home, SGR reverse, text on row 0
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    feed(t, "\033[1;1;1;5$z"); // DECERA row 1, cols 1..5 (1-based)
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse)); // not leaked
    CHECK(!cell(t, 0, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));

    // DECSERA preserves the erased positions' rendition.
    feed(t, "\033[2;1H\033[7mFGHIJ"); // row 2 (1-based), still reverse
    CHECK(cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    feed(t, "\033[2;1;2;5${"); // DECSERA row 2, cols 1..5
    CHECK(cell(t, 1, 0)->codePoint == 0);
    CHECK(cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 1, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
}

TEST(testDECCRACopiesRect)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDEF");
    // Copy row 1 columns 1..3 (1-based: ABC) → row 2 column 1.
    feed(t, "\033[1;1;1;3;1;2;1;1$v");
    CHECK(cell(t, 1, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 1)->codePoint == 'B');
    CHECK(cell(t, 1, 2)->codePoint == 'C');
    // Source unchanged.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
}

TEST(testDECCRAOverlapPreservesSource)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDEF");
    // Copy row 1 columns 1..4 (ABCD) → row 1 column 2. Without a snapshot
    // the result reads as A A A A E F. With one, A A B C D F.
    feed(t, "\033[1;1;1;4;1;1;2;1$v");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'A');
    CHECK(cell(t, 0, 2)->codePoint == 'B');
    CHECK(cell(t, 0, 3)->codePoint == 'C');
    CHECK(cell(t, 0, 4)->codePoint == 'D');
    CHECK(cell(t, 0, 5)->codePoint == 'F');
}

TEST(testDECSACEStoresExtent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2*x");
    CHECK(t.modes.sace == 2);
    feed(t, "\033[1*x");
    CHECK(t.modes.sace == 1);
}

TEST(testDECSACEStreamExtentDECRARA)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Set stream extent.
    feed(t, "\033[1*x");
    CHECK(t.modes.sace == 1);

    // DECRARA on rows 1..3 columns 3..5 (1-based) with stream extent.
    // Expect: row 0 (top) reversed from column 2 to columns-1; row 1 (middle)
    // reversed from column 0 to columns-1; row 2 (bottom) reversed from
    // column 0 to column 4.
    feed(t, "\033[1;3;3;5;7$t");
    int columns = t.columns;

    // Top row: cells before r.left untouched, cells from r.left
    // onward reversed.
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 0, columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));

    // Middle row: every column reversed.
    CHECK(cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 1, columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));

    // Bottom row: cells through r.right reversed, beyond untouched.
    CHECK(cell(t, 2, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 2, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(!cell(t, 2, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(!cell(t, 2, columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
}

TEST(testDECSACERectangleExtentDECRARA)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2*x");
    CHECK(t.modes.sace == 2);

    feed(t, "\033[1;3;3;5;7$t");
    // Outside the rect: untouched.
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(!cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(!cell(t, 1, t.columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    // Inside the rect: reversed.
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 1, 3)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 2, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
}

TEST(testDECSACEStreamSingleRowMatchesRectangle)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1*x");
    feed(t, "\033[1;3;1;5;7$t");
    CHECK((cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse)) == 0);
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 0, 4)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK((cell(t, 0, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse)) == 0);
}

TEST(testDECSCATogglesPenProtection)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"q"); // Ps=1 → protect
    feed(t, "X");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Protected));
    feed(t, "\033[0\"q"); // Ps=0 → unprotect
    feed(t, "Y");
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Protected));
}

TEST(testDECSELPreservesProtectedCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Write 'P' as protected, then 'U' as unprotected on the same row.
    feed(t, "\033[1\"qP");
    feed(t, "\033[0\"qU");
    // Cursor now at column 2; back up to column 0 + selective erase line.
    feed(t, "\033[H\033[?2K");
    CHECK(cell(t, 0, 0)->codePoint == 'P'); // protected — survives
    CHECK(cell(t, 0, 1)->codePoint == 0); // unprotected — erased
}

TEST(testDECSEDPreservesProtectedCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"qP\r\n"); // protected P at (0,0)
    feed(t, "\033[0\"qU"); // unprotected U at (1,0)
    feed(t, "\033[H\033[?2J"); // home + selective erase screen
    CHECK(cell(t, 0, 0)->codePoint == 'P');
    CHECK(cell(t, 1, 0)->codePoint == 0);
}

/// ED with several selective parameters applies each in turn (DEC STD 070
/// erase_in_display): ED 1 then ED 2 leaves nothing, where the first alone
/// would keep the text after the cursor.
TEST(testEDAppliesEachSelectiveParameter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "abcde\r\nfghij\r\nklmno");
    feed(t, "\033[2;3H\033[1;2J");
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 5; column++)
            CHECK(cell(t, row, column)->codePoint == 0);
}

/// EL with several selective parameters applies each in turn (DEC STD 070
/// erase_in_line): EL 1 then EL 2 erases the whole line.
TEST(testELAppliesEachSelectiveParameter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "abcde\r\nfghij");
    feed(t, "\033[1;3H\033[1;2K");
    for (int column = 0; column < 5; column++)
        CHECK(cell(t, 0, column)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 'f');
}

/// DECSEL applies each selective parameter in turn (DEC STD 070
/// selective_erase_in_line) and still steps over protected cells.
TEST(testDECSELAppliesEachSelectiveParameter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ab\033[1\"qP\033[0\"qcde");
    feed(t, "\033[1;2H\033[?1;2K");
    CHECK(cell(t, 0, 2)->codePoint == 'P');
    for (int column : { 0, 1, 3, 4, 5 })
        CHECK(cell(t, 0, column)->codePoint == 0);
}

/// DECSED applies each selective parameter in turn (DEC STD 070
/// selective_erase_in_display) and still steps over protected cells.
TEST(testDECSEDAppliesEachSelectiveParameter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "abcde\r\nf\033[1\"qP\033[0\"qhij\r\nklmno");
    feed(t, "\033[2;3H\033[?1;2J");
    CHECK(cell(t, 1, 1)->codePoint == 'P');
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 5; column++)
            if (row != 1 || column != 1)
                CHECK(cell(t, row, column)->codePoint == 0);
}

TEST(testDecrst1047ClearsAlternate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Enter alternate (?1047h), write 'Z', exit (?1047l), re-enter (?1047h).
    feed(t, "\033[?1047hZ\033[?1047l\033[?1047h");
    CHECK(t.screens.isUsingAlternateScreen == true);
    CHECK(cell(t, 0, 0)->codePoint != 'Z'); // 1047 cleared alternate on exit
    /// Contrast: 47 never clears — the stale 'Q' survives a round-trip.
    Ubiquitty::hardReset(t);
    feed(t, "\033[?47hQ\033[?47l\033[?47h");
    CHECK(cell(t, 0, 0)->codePoint == 'Q');
}

TEST(testAlternateScrollMode1007)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Default: off, and not active (primary screen).
    CHECK(t.modes.alternateScrollMode.asRequested() == false);
    CHECK(Ubiquitty::Modes::alternateScrollActive(&t) == false);

    // DECRQM ?1007 before set: reset (2).
    feed(t, "\033[?1007$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1007;2$y") == 0);

    // DECSET 1007 flips the flag; DECRQM now reports set (1).
    feed(t, "\033[?1007h");
    CHECK(t.modes.alternateScrollMode.asRequested() == true);
    feed(t, "\033[?1007$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1007;1$y") == 0);

    // Set but primary screen active → not yet active.
    CHECK(Ubiquitty::Modes::alternateScrollActive(&t) == false);

    // Switch to the alternate screen → now active.
    feed(t, "\033[?1049h");
    CHECK(Ubiquitty::Modes::alternateScrollActive(&t) == true);

    // DECCKM off: ESC [ A / ESC [ B.
    CHECK(t.modes.appCursorKeys == false);
    int n = Ubiquitty::Modes::alternateScrollKeys(t.modes.appCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Modes::ScrollDirection::Up, outputBuffer);
    CHECK(n == 3);
    CHECK(memcmp(outputBuffer, "\033[A", 3) == 0);
    n = Ubiquitty::Modes::alternateScrollKeys(t.modes.appCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Modes::ScrollDirection::Down, outputBuffer);
    CHECK(n == 3);
    CHECK(memcmp(outputBuffer, "\033[B", 3) == 0);

    // DECCKM on (application cursor keys): ESC O A / ESC O B.
    feed(t, "\033[?1h");
    CHECK(t.modes.appCursorKeys == true);
    n = Ubiquitty::Modes::alternateScrollKeys(t.modes.appCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Modes::ScrollDirection::Up, outputBuffer);
    CHECK(n == 3);
    CHECK(memcmp(outputBuffer, "\033OA", 3) == 0);
    n = Ubiquitty::Modes::alternateScrollKeys(t.modes.appCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Modes::ScrollDirection::Down, outputBuffer);
    CHECK(memcmp(outputBuffer, "\033OB", 3) == 0);

    // Too-small buffer writes nothing.
    CHECK(Ubiquitty::Modes::alternateScrollKeys(t.modes.appCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Modes::ScrollDirection::Up, { outputBuffer, 2 }) == 0);

    // Leave alternate screen → inactive again though flag stays set.
    feed(t, "\033[?1049l");
    CHECK(t.modes.alternateScrollMode.asRequested() == true);
    CHECK(Ubiquitty::Modes::alternateScrollActive(&t) == false);

    // DECRST 1007 clears the flag.
    feed(t, "\033[?1007l");
    CHECK(t.modes.alternateScrollMode.asRequested() == false);
}

TEST(testDeleteSendsDelMode1037)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Power-on default: reset (Delete sends CSI 3 ~).
    CHECK(t.modes.deleteKeySendsDelete == false);
    feed(t, "\033[?1037$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1037;2$y") == 0);

    // DECSET 1037 → field sets; DECRQM reports set (1).
    feed(t, "\033[?1037h");
    CHECK(t.modes.deleteKeySendsDelete == true);
    feed(t, "\033[?1037$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1037;1$y") == 0);

    // DECRST 1037 → back to reset (2).
    feed(t, "\033[?1037l");
    CHECK(t.modes.deleteKeySendsDelete == false);
    feed(t, "\033[?1037$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1037;2$y") == 0);
}

TEST(testDECUDKProgramClearLock)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Defaults: no keys, unlocked.
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 0);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).locked == false);

    // Program F6 (DEC code 17) → "AB" (hex 4142), Pl=1 (do not lock).
    feed(t, "\033P1;1|17/4142\033\\");
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).locked == false);
    int slot = -1;
    for (int i = 0; i < Ubiquitty::ensureUserDefinedKeys(t).count; ++i)
        if (Ubiquitty::ensureUserDefinedKeys(t).entries[i].keyCode == 17)
            slot = i;
    CHECK(slot >= 0);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[slot].length == 2);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[slot].value[0] == 'A');
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[slot].value[1] == 'B');

    // Program F7 (18) with Pc=1 (keep existing) → F6 survives.
    feed(t, "\033P1;1|18/58\033\\"); // "X"
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 2);

    // Pc=0 clears all before loading: only F8 (19) remains.
    feed(t, "\033P0;1|19/59\033\\"); // "Y"
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 1);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].keyCode == 19);

    // Lock with Pl=0: the keys can no longer be reprogrammed.
    feed(t, "\033P1;0|17/5A\033\\"); // program F6 then lock
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).locked == true);
    int countAfterLock = Ubiquitty::ensureUserDefinedKeys(t).count;
    feed(t, "\033P0;1|20/5B\033\\"); // ignored while locked
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == countAfterLock);

    // RIS clears + unlocks.
    feed(t, "\033c");
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 0);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).locked == false);
}

TEST(testDECUDKOverlongKeyCode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Key codes past 16 bits clamp to 0xFFFF instead of wrapping.
    feed(t, "\033P1;1|999999/41\033\\"); // "A"
    int slot = -1;
    for (int i = 0; i < Ubiquitty::ensureUserDefinedKeys(t).count; ++i)
        if (Ubiquitty::ensureUserDefinedKeys(t).entries[i].keyCode == 0xFFFF)
            slot = i;
    CHECK(slot >= 0);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[slot].length == 1);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[slot].value[0] == 'A');
}

TEST(testDECUDKBadHexAndGarbage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // A non-hex nibble aborts the pair's value, the parser skips past the
    // garbage to the next `;`-separated pair, and the good pair still loads.
    feed(t, "\033P1;1|17/4Z;18/5858\033\\"); // F6 bad hex, F7 → "XX"
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 1);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].keyCode == 18);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].length == 2);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].value[0] == 'X');
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].value[1] == 'X');
}

TEST(testScrollSetsCellsModifiedFlag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    drainPaintCycle(t, nullptr);
    CHECK(!t.grid->hasUnpaintedWork());
    // LF at the bottom of the scroll region triggers a Ubiquitty::Screen::scrollUp; that
    // memcpys rows + blanks the newly-revealed row → cells changed.
    feed(t, "\033[24H");
    CHECK(!t.grid->hasUnpaintedWork()); // CUP alone
    feed(t, "\n");
    CHECK(t.grid->hasUnpaintedWork());
}

TEST(testPureCursorMovesKeepFlagClear)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    drainPaintCycle(t, nullptr);
    CHECK(!t.grid->hasUnpaintedWork());
    // CUP, CUF, CUB, CUU, CUD must all keep the flag clear so the
    // cursor-only-skip can fire on these movements.
    feed(t, "\033[10;10H");
    CHECK(!t.grid->hasUnpaintedWork());
    feed(t, "\033[3C"); // CUF
    CHECK(!t.grid->hasUnpaintedWork());
    feed(t, "\033[3D"); // CUB
    CHECK(!t.grid->hasUnpaintedWork());
    feed(t, "\033[3A"); // CUU
    CHECK(!t.grid->hasUnpaintedWork());
    feed(t, "\033[3B"); // CUD
    CHECK(!t.grid->hasUnpaintedWork());
}

TEST(testFullScreenScrollOriginPreservesRowRecords)
{
    Ubiquitty::CellGrid display;
    display.init(5, 8);
    Ubiquitty::Cell blank = *display.cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0));

    for (int row = 0; row < 5; ++row)
        writeDisplayProbeRow(&display, row, row);
    for (int seq = 5; seq < 105; ++seq) {
        display.scrollRegion(Ubiquitty::Row(0), Ubiquitty::Row(4), 1, blank);
        writeDisplayProbeRow(&display, 4, seq);
    }

    for (int row = 0; row < 5; ++row) {
        const int seq = 100 + row;
        const int width = (seq % 6) + 1;
        const Ubiquitty::Cell* first = display.cellAt(Ubiquitty::Row(row), Ubiquitty::Column(0));
        CHECK(first != nullptr);
        if (first)
            CHECK(first->codePoint == static_cast<uint32_t>('A' + (seq % 26)));
        CHECK(display.rowContentWidth(Ubiquitty::Row(row)) == width);
        CHECK(display.rowMeta(row).lineAttribute == static_cast<uint8_t>(seq % 4));
        CHECK(display.rowMeta(row).oscMark == static_cast<uint8_t>((seq % 7) + 1));
        CHECK(display.rowMeta(row).has(Ubiquitty::RowFlag::Wrapped) == static_cast<bool>(seq % 2));
        if (width < 8) {
            const Ubiquitty::Cell* tail = display.cellAt(Ubiquitty::Row(row), Ubiquitty::Column(width));
            CHECK(tail != nullptr);
            if (tail)
                CHECK(tail->codePoint == 0);
        }
    }
}

TEST(testClearPreservesUserTrailingSpaces)
{
    Ubiquitty::Terminal t;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    // "user@host % " = 12 chars (prompt), then "hello  " = 7 chars
    // (input with two trailing spaces). Cursor lands at column 19.
    feed(t, "user@host % hello  ");
    CHECK(t.screens.active->cursorX == 19);
    Ubiquitty::clearBufferPreserveInput(&t);
    CHECK(t.screens.active->cursorX == 19);
    CHECK(t.screens.active->cursorY == 0);
    // Row 0 must read "user@host % hello  " verbatim — including
    // both trailing spaces at columns 17 and 18 (the cells immediately
    // to the left of cursorX=19).
    for (c = 0; c < 12; c++) {
        const char* expect = "user@host % ";
        CHECK(cell(t, 0, c)->codePoint == static_cast<uint16_t>(expect[c]));
    }
    for (c = 0; c < 5; c++) {
        const char* expect = "hello";
        CHECK(cell(t, 0, 12 + c)->codePoint == static_cast<uint16_t>(expect[c]));
    }
    // The two trailing spaces.
    CHECK(cell(t, 0, 17)->codePoint == ' ');
    CHECK(cell(t, 0, 18)->codePoint == ' ');
}

TEST(testClearPreservesPromptTrailingSpace)
{
    Ubiquitty::Terminal t;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    feed(t, "user@host % ");
    CHECK(t.screens.active->cursorX == 12);
    Ubiquitty::clearBufferPreserveInput(&t);
    CHECK(t.screens.active->cursorX == 12);
    CHECK(t.screens.active->cursorY == 0);
    for (c = 0; c < 12; c++) {
        const char* expect = "user@host % ";
        CHECK(cell(t, 0, c)->codePoint == static_cast<uint16_t>(expect[c]));
    }
    // column 11 is the trailing space — must be preserved verbatim.
    CHECK(cell(t, 0, 11)->codePoint == ' ');
}

TEST(testClearMarksOldRowDirty)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Push the cursor down to row N>0 by emitting several lines of
    // output followed by a prompt on the new row.
    feed(t, "line one\r\nline two\r\nline three\r\n$ hello");
    CHECK(t.screens.active->cursorY > 0);
    /// Snapshot row N before clear; clear-buffer-preserve-input requests
    /// a full redraw so every row must be repainted.
    Ubiquitty::clearBufferPreserveInput(&t);
    CHECK(t.grid->isFullRedrawRequested());
    // hasUnpaintedWork is true — full-redraw or cell deltas after the
    // clear.
    CHECK(t.grid->hasUnpaintedWork());
}

TEST(testConsecutiveScrollsMergeDelta)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    drainPaintCycle(t, nullptr);
    // Two LFs at the bottom margin: each triggers a Ubiquitty::Screen::scrollUp. The
    // pending-scroll slot must coalesce same-direction same-range
    // scrolls into delta=2.
    feed(t, "\033[5;1H\n\n");
    peekPaintTrace(t, &tr);
    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollTop == 0);
    CHECK(tr.scrollBottom == 4);
    CHECK(tr.scrollShift == 2);
}

TEST(testPendingScrollExposesDirtyRowsAfterAck)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feedNumberedLines(t, 5);
    drainPaintCycle(t, nullptr);
    feed(t, "\033[32m\033[5;1H\n");
    drainPaintCycle(t, &tr);

    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollShift == 1);
    CHECK(tr.isCellYielded[4][0]);
    CHECK(tr.isRowYielded[4]);
    CHECK(!tr.isRowYielded[3]);

    peekPaintTrace(t, &tr);
    CHECK(tr.pendingScrolls == 0);
    CHECK(tr.runs == 0);
}

TEST(testPendingScrollPostScrollWriteStaysDirty)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feedNumberedLines(t, 5);
    drainPaintCycle(t, nullptr);
    feed(t, "\033[32m\033[5;1H\n\033[3;1HX");
    drainPaintCycle(t, &tr);

    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollShift == 1);
    CHECK(tr.isCellYielded[2][0]);
    CHECK(tr.isRowYielded[2]);
    CHECK(tr.isCellYielded[4][0]);
    CHECK(tr.isRowYielded[4]);
    CHECK(!tr.isRowYielded[3]);
    CHECK(cell(t, 2, 0)->codePoint == 'X');
}

TEST(testPendingScrollSelectionDirtySurvivesAck)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feedNumberedLines(t, 5);
    drainPaintCycle(t, nullptr);
    feed(t, "\033[5;1H\n");
    t.grid->markSelectionFlipped(Ubiquitty::Row(1), Ubiquitty::Column(0), Ubiquitty::Column(0));
    drainPaintCycle(t, &tr);

    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollShift == 1);
    CHECK(tr.isCellYielded[1][0]);
    CHECK(tr.isRowYielded[1]);
}

TEST(testScrollDirectionChangeReplacesPendingScroll)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    drainPaintCycle(t, nullptr);
    // LF at bottom (scroll up) then RI at top (scroll down). The second
    // scroll has opposite direction — CellGrid::scrollRegion currently
    // only merges same-direction scrolls; opposite direction overwrites
    // the slot (after a paint-mid the older scroll has already been
    // acked). Since no paint runs between them, the result depends on
    // CellGrid's merge rules. CellGrid.cpp checks (m_pendingScroll.delta
    // > 0 && n > 0) || both negative — opposite directions take the
    // else branch which OVERWRITES with the new scroll.
    feed(t, "\033[5;1H\n\033[1;1H\033M");
    peekPaintTrace(t, &tr);
    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollTop == 0);
    CHECK(tr.scrollBottom == 4);
    CHECK(tr.scrollShift == -1);
}

TEST(testScrollRangeChangeReplacesPendingScroll)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    drainPaintCycle(t, nullptr);
    // First scroll: LF at row 6, full-screen scroll region (top=0, bot=5,
    // delta=+1). Then DECSTBM to rows 2..5 (top=1, bot=4), cursor to
    // row 5, LF — scroll up by 1 in the new region. Because top/bot
    // differ, the merge fails and the slot is overwritten with the
    // new scroll.
    feed(t, "\033[6;1H\n\033[2;5r\033[5;1H\n");
    peekPaintTrace(t, &tr);
    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollTop == 1);
    CHECK(tr.scrollBottom == 4);
    CHECK(tr.scrollShift == 1);
}

TEST(testMidScrollDirtyPropagatesToUnsafeRows)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    drainPaintCycle(t, nullptr);
    // First LF: scroll up by 1 (delta=+1). Then write 'X' at row 2 —
    // that cell-mutation happens between scrolls. Second LF: would
    // merge to delta=+2 (same direction, same range). CellGrid
    // surfaces the cells written between scrolls as plain dirty runs
    // that the paint cycle resolves after the scroll drain.
    // After a drain+repaint, the displayed state matches the model.
    feed(t, "\033[5;1H\n\033[2;1HX\033[5;1H\n");
    peekPaintTrace(t, &tr);
    // Merged scroll delta is 2.
    CHECK(tr.pendingScrolls == 1);
    CHECK(tr.scrollShift == 2);
    // Now drain. The mid-scroll write surfaces through dirty runs;
    // after the full cycle, hasUnpaintedWork is false.
    drainPaintCycle(t, nullptr);
    CHECK(!t.grid->hasUnpaintedWork());
}

TEST(testTBCClearsStopAtCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Move to column 4 and set an HTS there.
    feed(t, "\033[5G\033H");
    CHECK(t.tabStops[4]);
    // CSI g (default Ps=0) clears the stop at the cursor column.
    feed(t, "\033[g");
    CHECK(!t.tabStops[4]);
    // Other default stops must survive.
    CHECK(t.tabStops[8]);
    CHECK(t.tabStops[16]);
}

TEST(testTBCClearsAllStops)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Set explicit stops at columns 5 and 10 via HTS.
    feed(t, "\033[6G\033H\033[11G\033H");
    CHECK(t.tabStops[5]);
    CHECK(t.tabStops[10]);
    // CSI 3 g — wipe all stops.
    feed(t, "\033[3g");
    for (int i = 0; i < t.columns; i++)
        CHECK(!t.tabStops[i]);
}

TEST(testDECRCDoesNotRestoreScrollMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    feed(t, "\033[4;8r"); // scrollTop=3, scrollBottom=7
    CHECK(t.screens.active->scrollTop == 3);
    CHECK(t.screens.active->scrollBottom == 7);

    feed(t, "\0337"); // DECSC: saves cursor/pen/characterSet — not the margins
    CHECK(t.screens.active->hasSavedCursor);

    feed(t, "\033[2;10r"); // scrollTop=1, scrollBottom=9 — changed after the save
    CHECK(t.screens.active->scrollTop == 1);
    CHECK(t.screens.active->scrollBottom == 9);

    feed(t, "\0338"); // DECRC: must leave the CURRENT (post-save) margins alone
    CHECK(t.screens.active->scrollTop == 1);
    CHECK(t.screens.active->scrollBottom == 9);
}

TEST(test_eraseInDisplay_skips_already_blank_rows)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int r;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Fresh terminal — every cell already matches the blank template.
    // Sync the shadow, then ED 2 must produce zero dirty runs.
    drainPaintCycle(t, nullptr);
    feed(t, "\033[2J");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
}

TEST(test_eraseInDisplay_dirties_only_changed_range)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Put "hi" at row 5 columns 0..1, leave the rest of row 5 blank,
    // and have row 6 fully blank. Sync the shadow with these cells.
    // Then ED 2 should dirty only row 5 columns 0..1; row 6 untouched.
    feed(t, "\033[6;1H");
    feed(t, "hi");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[2J");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[5]);
    CHECK(tr.isCellYielded[5][0]);
    CHECK(tr.isCellYielded[5][1]);
    for (c = 2; c < t.columns; c++)
        CHECK(!tr.isCellYielded[5][c]);
    CHECK(!tr.isRowYielded[6]);
    CHECK(!tr.isRowYielded[0]);
}

TEST(test_eraseInLine_skips_already_blank)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int r;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Cursor at start of row 4 (blank row). EL 0 must yield nothing.
    feed(t, "\033[4;1H");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[K");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
}

TEST(test_eraseInLine_dirties_only_changed_range)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Put "abcde" at row 4 columns 0..4 then move cursor to (4, 3) and
    // EL 0. Only columns 2..4 actually change to blank. Columns 5..79 were
    // already blank and must not surface.
    feed(t, "\033[4;1H");
    feed(t, "abcde");
    feed(t, "\033[4;3H");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[K");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[3]);
    CHECK(tr.isCellYielded[3][2]);
    CHECK(tr.isCellYielded[3][3]);
    CHECK(tr.isCellYielded[3][4]);
    for (c = 0; c <= 1; c++)
        CHECK(!tr.isCellYielded[3][c]);
    for (c = 5; c < t.columns; c++)
        CHECK(!tr.isCellYielded[3][c]);
}

TEST(test_eraseInLine_sparse_does_not_dirty_blank_gaps)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Sparse-content row: "foo" at columns 0..2, an UNWRITTEN gap 3..9 (the
    // cursor jumps over it, so those cells hold the blank sentinel, not
    // typed spaces), "bar" at columns 10..12. Move cursor to (4, 1) and EL 0.
    // Cells 0..2 + 10..12 change to blank — those surface as dirty runs.
    // Cells 3..9 stay the sentinel (blank → blank) and must not surface.
    feed(t, "\033[4;1H");
    feed(t, "foo");
    feed(t, "\033[4;11Hbar");
    feed(t, "\033[4;1H");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[K");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[3]);
    // The cells that actually changed:
    for (c = 0; c <= 2; c++)
        CHECK(tr.isCellYielded[3][c]);
    for (c = 10; c <= 12; c++)
        CHECK(tr.isCellYielded[3][c]);
    // The blank gap in the middle must not have been yielded as
    // dirty — CellGrid compares cells_ against m_bbShadow, and these
    // cells didn't change.
    for (c = 3; c <= 9; c++)
        CHECK(!tr.isCellYielded[3][c]);
}

TEST(testDECCKMSetClearsFlipsFlag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1h");
    CHECK(t.modes.appCursorKeys == true);
    feed(t, "\033[?1l");
    CHECK(t.modes.appCursorKeys == false);
    // And again.
    feed(t, "\033[?1h");
    CHECK(t.modes.appCursorKeys == true);
}

TEST(testDECPNMClearsKeypadApp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033=");
    CHECK(t.modes.appKeypad == true);
    feed(t, "\033>");
    CHECK(t.modes.appKeypad == false);
}

TEST(testSyncOutputSetClearsVia2026)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.syncOutput == false);
    feed(t, "\033[?2026h");
    CHECK(t.modes.syncOutput == true);
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == true);
    feed(t, "\033[?2026l");
    CHECK(t.modes.syncOutput == false);
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == false);
}

TEST(testDECSCLMSmoothScrollMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.smoothScroll == false);
    testStoredModeRoundtrip(t, 4, &t.modes.smoothScroll, "\033[?4h", "\033[?4l", "\033[?4;1$y", "\033[?4;2$y");
}

TEST(testScrollOnOutputMode1010)
{
    using Ubiquitty::Modes::shouldSnapToTail;
    using Ubiquitty::Modes::SnapTrigger;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // xterm default: scroll-on-output is OFF.
    CHECK(t.modes.scrollOnOutput == false);
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Output) == false);
    testStoredModeRoundtrip(t, 1010, &t.modes.scrollOnOutput, "\033[?1010h", "\033[?1010l", "\033[?1010;1$y", "\033[?1010;2$y");
    // Predicate tracks the flag after set/reset.
    feed(t, "\033[?1010h");
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Output) == true);
    // 1010 governs Output only; 1011 (keypress) stays default-on.
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress) == true);
    feed(t, "\033[?1010l");
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Output) == false);
}

TEST(testScrollOnKeypressMode1011)
{
    using Ubiquitty::Modes::shouldSnapToTail;
    using Ubiquitty::Modes::SnapTrigger;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // xterm default: scroll-on-keypress is ON.
    CHECK(t.modes.scrollOnKeypress == true);
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress) == true);
    // DECRQM reports the default-on state as set (Pm=1) before any toggle.
    {
        char outputBuffer[64];
        feed(t, "\033[?1011$p");
        int n = drain(t, outputBuffer);
        CHECK(n > 0);
        CHECK(strcmp(outputBuffer, "\033[?1011;1$y") == 0);
    }
    feed(t, "\033[?1011l");
    CHECK(t.modes.scrollOnKeypress == false);
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress) == false);
    feed(t, "\033[?1011h");
    CHECK(t.modes.scrollOnKeypress == true);
    CHECK(shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress) == true);
    // Null term guards to false for both triggers.
    CHECK(shouldSnapToTail(nullptr, Ubiquitty::Modes::SnapTrigger::Output) == false);
    CHECK(shouldSnapToTail(nullptr, Ubiquitty::Modes::SnapTrigger::Keypress) == false);
}

/// EL Ps=0 — erase from cursor to end of line.
TEST(testElPs0ClearsFromCursorToEndOfLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;3H\033[K"); // CUP to column 3 (index 2), then EL
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 0, 3)->codePoint == 0);
    CHECK(cell(t, 0, 4)->codePoint == 0);
    CHECK(cell(t, 0, t.columns - 1)->codePoint == 0);
}

/// EL Ps=1 — erase from start of line to cursor (inclusive).
TEST(testElPs1ClearsFromStartToCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;3H\033[1K");
    // columns 0..2 blanked; column 3 still 'D'.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 0, 3)->codePoint == 'D');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
}

/// EL Ps=2 — erase whole line.
TEST(testElPs2ClearsWholeLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[2K");
    for (int i = 0; i < t.columns; i++)
        CHECK(cell(t, 0, i)->codePoint == 0);
}

/// ED Ps=0 — erase from cursor to end of screen.
TEST(testEdPs0ClearsFromCursorToEndOfScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ROW0\r\nROW1\r\nROW2");
    feed(t, "\033[2;1H\033[J"); // CUP row 2 column 1, then ED
    // Row 0 intact.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == '0');
    // Row 1 blanked.
    for (int i = 0; i < 4; i++)
        CHECK(cell(t, 1, i)->codePoint == 0);
    // Row 2 blanked.
    for (int i = 0; i < 4; i++)
        CHECK(cell(t, 2, i)->codePoint == 0);
    // Bottom row blanked.
    CHECK(cell(t, t.rows - 1, 0)->codePoint == 0);
}

/// ED Ps=1 — erase from start of screen to cursor (inclusive).
TEST(testEdPs1ClearsTopToCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ROW0\r\nROW1\r\nROW2");
    feed(t, "\033[2;3H\033[1J"); // CUP row 2 column 3, then ED 1
    // Row 0 blanked.
    for (int i = 0; i < 4; i++)
        CHECK(cell(t, 0, i)->codePoint == 0);
    // Row 1 columns 0..2 blanked; column 3 ('1') blanked too (inclusive).
    CHECK(cell(t, 1, 0)->codePoint == 0);
    CHECK(cell(t, 1, 1)->codePoint == 0);
    CHECK(cell(t, 1, 2)->codePoint == 0);
    // Row 2 must still be intact (below cursor row).
    CHECK(cell(t, 2, 0)->codePoint == 'R');
    CHECK(cell(t, 2, 3)->codePoint == '2');
}

/// ED Ps=2 — erase entire visible screen; cursor unchanged.
TEST(testEdPs2ClearsFullScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ROW0\r\nROW1\r\nROW2");
    feed(t, "\033[5;7H"); // park cursor somewhere
    int cx = t.screens.active->cursorX, cy = t.screens.active->cursorY;
    feed(t, "\033[2J");
    for (int r = 0; r < t.rows; r++)
        for (int c = 0; c < t.columns; c++)
            CHECK(cell(t, r, c)->codePoint == 0);
    CHECK(t.screens.active->cursorX == cx);
    CHECK(t.screens.active->cursorY == cy);
}

/// ICH (CSI Ps @) — insert N blanks at cursor; chars shift right, last fall off.
TEST(testICHInsertCharacterShiftsRight)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;2H\033[2@"); // CUP column 2 (index 1), insert 2 blanks
    // Expect: "A  BCD" — E shifted off the row (since columns-1 is the limit).
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 0, 3)->codePoint == 'B');
    CHECK(cell(t, 0, 4)->codePoint == 'C');
    CHECK(cell(t, 0, 5)->codePoint == 'D');
    // Original 'E' at column 4 is now at column 6 (5+1) or shifted off? With columns=80
    // and only 5 chars at the front, E moves to column 6.
    CHECK(cell(t, 0, 6)->codePoint == 'E');
}

/// DCH (CSI Ps P) — delete N chars at cursor; chars shift left, blanks at right.
TEST(testDCHDeleteCharacterShiftsLeft)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;2H\033[2P"); // CUP column 2 (index 1), delete 2
    // Expect: "ADE  " starting at column 0.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'D');
    CHECK(cell(t, 0, 2)->codePoint == 'E');
    CHECK(cell(t, 0, 3)->codePoint == 0);
    CHECK(cell(t, 0, 4)->codePoint == 0);
}

/// IRM (CSI 4 h / 4 l) — printable chars insert and shift the line right
/// while set; resetting returns to overwrite.
TEST(testIRMInsertModeShiftsLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;2H"); // cursor to column 2 (index 1, on 'B')
    feed(t, "\033[4h"); // IRM set
    feed(t, "XY"); // insert two chars
    // "A" stays, "XY" inserted, "BCDE" pushed right → "AXYBCDE".
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'X');
    CHECK(cell(t, 0, 2)->codePoint == 'Y');
    CHECK(cell(t, 0, 3)->codePoint == 'B');
    CHECK(cell(t, 0, 4)->codePoint == 'C');
    CHECK(cell(t, 0, 5)->codePoint == 'D');
    CHECK(cell(t, 0, 6)->codePoint == 'E');
    CHECK(t.modes.insertMode == true);

    // Reset IRM → overwrite resumes. Cursor now at index 3 ('B').
    feed(t, "\033[4l");
    feed(t, "Z");
    CHECK(t.modes.insertMode == false);
    CHECK(cell(t, 0, 3)->codePoint == 'Z'); // overwrote 'B', no shift
    CHECK(cell(t, 0, 4)->codePoint == 'C');
}

/// ECH (CSI Ps X) — erase N chars in place (no shift).
TEST(testECHEraseCharacterNoShift)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDE");
    feed(t, "\033[1;2H\033[2X"); // CUP column 2 (index 1), erase 2
    // Expect: "A  DE" — blanks in place, D and E unchanged.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 0, 3)->codePoint == 'D');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
}

/// IL (CSI Ps L) — insert blank line at cursor row; rows below shift down.
TEST(testIlInsertLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ROW0\r\nROW1\r\nROW2");
    feed(t, "\033[2;1H\033[L"); // CUP row 2, insert 1 line
    // Row 0 intact.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == '0');
    // Row 1 (cursor row) now blank.
    CHECK(cell(t, 1, 0)->codePoint == 0);
    CHECK(cell(t, 1, 3)->codePoint == 0);
    // Row 2 has the old "ROW1".
    CHECK(cell(t, 2, 0)->codePoint == 'R');
    CHECK(cell(t, 2, 3)->codePoint == '1');
    // Row 3 has the old "ROW2".
    CHECK(cell(t, 3, 0)->codePoint == 'R');
    CHECK(cell(t, 3, 3)->codePoint == '2');
}

/// DL (CSI Ps M) — delete line at cursor row; rows below shift up, last blanks.
TEST(testDlDeleteLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ROW0\r\nROW1\r\nROW2");
    feed(t, "\033[2;1H\033[M"); // CUP row 2, delete 1 line
    // Row 0 intact.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == '0');
    // Row 1 now has old "ROW2".
    CHECK(cell(t, 1, 0)->codePoint == 'R');
    CHECK(cell(t, 1, 3)->codePoint == '2');
    // Last row blanks.
    CHECK(cell(t, t.rows - 1, 0)->codePoint == 0);
}

/// SU/SD (CSI Ps S/T) — pan scroll within DECSTBM.
TEST(testSuSdScrollUpDown)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Set DECSTBM 1..5, then fill rows 0..4 with R0..R4.
    feed(t, "\033[1;5r");
    feed(t, "\033[1;1H");
    feed(t, "R0\r\nR1\r\nR2\r\nR3\r\nR4");
    // SU 3 — content moves up by 3 within the scroll region.
    // Row 0 should become R3, row 1 R4, rows 2..4 blanks.
    feed(t, "\033[3S");
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 1)->codePoint == '3');
    CHECK(cell(t, 1, 0)->codePoint == 'R');
    CHECK(cell(t, 1, 1)->codePoint == '4');
    CHECK(cell(t, 2, 0)->codePoint == 0);
    CHECK(cell(t, 3, 0)->codePoint == 0);
    CHECK(cell(t, 4, 0)->codePoint == 0);
    // SD 3 — content moves down by 3.
    // Rows 0..2 blanks, row 3 = old row-0 (R3), row 4 = old row-1 (R4).
    feed(t, "\033[3T");
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 0);
    CHECK(cell(t, 2, 0)->codePoint == 0);
    CHECK(cell(t, 3, 0)->codePoint == 'R');
    CHECK(cell(t, 3, 1)->codePoint == '3');
    CHECK(cell(t, 4, 0)->codePoint == 'R');
    CHECK(cell(t, 4, 1)->codePoint == '4');
}

/// LF at the bottom of a DECSTBM region scrolls the region up - top
/// row's content disappears, bottom row is blank, cursor stays put.
TEST(testLfAtBottomOfDECSTBMScrolls)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // DECSTBM 1;5 (1-based) -> indices [0,4].
    feed(t, "\033[1;5r");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == 4);
    // Paint each row of the region with a unique marker.
    feed(t, "\033[1;1HROWA");
    feed(t, "\033[2;1HROWB");
    feed(t, "\033[3;1HROWC");
    feed(t, "\033[4;1HROWD");
    feed(t, "\033[5;1HROWE");
    // CUP to (row 5, column 1) -> cursor at region bottom.
    feed(t, "\033[5;1H");
    CHECK(t.screens.active->cursorY == 4);
    // LF at region bottom -> scroll up, cursor stays on row 4.
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 4);
    // Row 0 lost (was ROWA); row 0 now shows ROWB's content.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == 'B');
    // Region bottom is now blank.
    CHECK(cell(t, 4, 0)->codePoint == 0);
}

/// Wrap at the bottom of a DECSTBM region must scroll the region (not
/// the whole screen) up by one row.
TEST(testWrapAtDECSTBMBottomScrolls)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    // Region [0,4] then park on row 4 (region bottom).
    feed(t, "\033[1;5r");
    feed(t, "\033[1;1HROW0");
    feed(t, "\033[2;1HROW1");
    feed(t, "\033[3;1HROW2");
    feed(t, "\033[4;1HROW3");
    feed(t, "\033[5;1H");
    CHECK(t.screens.active->cursorY == 4);
    // Fill the bottom row to the right margin. wrapPending should
    // be set after the last write.
    for (i = 0; i < Ubiquitty::defaultColumns; i++)
        feed(t, "Z");
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    CHECK(t.screens.active->wrapPending);
    // Write one more char - should scroll the region up by one and
    // land on the new bottom row at column 1.
    feed(t, "W");
    // Cursor remains in region; the next-char-after-wrap lands at
    // (row 4, column 1) after the scroll.
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 1);
    // Row 0 should now hold what was row 1 before the scroll.
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 3)->codePoint == '1');
    // The new bottom row contains 'W' at column 0 (the wrap-target).
    CHECK(cell(t, 4, 0)->codePoint == 'W');
}

/// A soft-wrap on the BOTTOM row scrolls the screen up; the join between the
/// wrapped row and its continuation must still be recorded as a wrapped flag,
/// or a URL/link spanning the wrap can't be stitched back into one logical line
/// guards a wrapped URL from `claude /login`, printed near the bottom,
/// highlighting on only one row. After the scroll the wrapped content lands at
/// cursorY-1, so that's where the flag belongs — not the stale pre-scroll
/// index, which a scroll skips entirely.
TEST(testWrapJoinPreservedOnBottomScroll)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    // Park on the bottom row (row 2), then write > columns chars so the line
    // autowraps WITH a scroll (cursor is at the screen bottom).
    feed(t, "\n\n");
    CHECK(t.screens.active->cursorY == 2);
    feed(t, "ABCDEFGHIJKLMN"); // 10 fill row 2; the 11th wraps+scrolls
    // After the scroll: the first 10 chars sit on row 1 and continue onto
    // row 2, so row 1 is soft-wrapped into row 2.
    CHECK(cell(t, 1, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 9)->codePoint == 'J');
    CHECK(cell(t, 2, 0)->codePoint == 'K');
    CHECK(t.screens.active->lineWrapped[1]);
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&t, 0, 1));
    // The non-continuation bottom row is not marked wrapped.
    CHECK(!t.screens.active->lineWrapped[2]);
}

/// RIS (ESC c) is a hard reset. It must clear screen content.
TEST(testRISClearsScreen)
{
    Ubiquitty::Terminal t;
    int r, c;
    Ubiquitty::hardReset(t);
    feed(t, "Hello\r\nWorld");
    CHECK(cell(t, 0, 0)->codePoint == 'H');
    feed(t, "\033c");
    // Sweep the full grid - every cell must be blank.
    for (r = 0; r < t.rows; r++)
        for (c = 0; c < t.columns; c++)
            CHECK(cell(t, r, c)->codePoint == 0);
}

TEST(testDECALNClearsDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6;16r");
    CHECK(t.screens.active->scrollTop == 5);
    CHECK(t.screens.active->scrollBottom == 15);
    feed(t, "\033#8");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
}

TEST(testDECALNHomesCursorAndClearsPendingWrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[10;40H");
    t.screens.active->wrapPending = true;
    CHECK(t.screens.active->cursorY == 9);
    CHECK(t.screens.active->cursorX == 39);

    feed(t, "\033#8");

    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);
    CHECK(!t.screens.active->wrapPending);
}

TEST(testDECSCLVT100GatesDECSCA)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // VT100 level: DECSCA 1 is inert → the written cell is not protected, so a
    // selective erase-in-line (DECSEL, CSI ? 2 K) clears it.
    feed(t, "\033[61\"p");
    feed(t, "\033[1\"q"); // DECSCA 1 — try to protect
    CHECK(!t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Protected));
    feed(t, "X");
    feed(t, "\033[H"); // home
    feed(t, "\033[?2K"); // DECSEL all
    CHECK(cell(t, 0, 0)->codePoint != 'X');

    // VT200 level: DECSCA 1 now sets the protect bit; a protected cell
    // survives DECSEL.
    feed(t, "\033[62\"p");
    feed(t, "\033[2J\033[H"); // clear + home
    feed(t, "\033[1\"q"); // DECSCA 1 — protect
    CHECK(t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Protected));
    feed(t, "Y");
    feed(t, "\033[H");
    feed(t, "\033[?2K"); // DECSEL all — protected cell survives
    CHECK(cell(t, 0, 0)->codePoint == 'Y');
}

TEST(testDECSERAPreservesProtectedCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Row 0: protected P at col 0, unprotected U at col 1, plain Z at col 2.
    feed(t, "\033[1\"qP"); // protected
    feed(t, "\033[0\"qUZ"); // unprotected U, Z
    // Selective erase rect over row 1, columns 1..3 (1-based).
    feed(t, "\033[1;1;1;3${");
    CHECK(cell(t, 0, 0)->codePoint == 'P'); // protected — survives
    CHECK(cell(t, 0, 1)->codePoint == 0); // unprotected — erased
    CHECK(cell(t, 0, 2)->codePoint == 0); // unprotected — erased
}

TEST(testDECSERAErasesUnprotectedRect)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABCDEFGHIJ");
    feed(t, "\033[1;3;1;6${"); // erase row 1, columns 3..6 (1-based)
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0); // erased
    CHECK(cell(t, 0, 5)->codePoint == 0); // erased
    CHECK(cell(t, 0, 6)->codePoint == 'G');
    // VT100 conformance level gates DECSERA off (a VT400 op).
    feed(t, "\033[61\"p"); // DECSCL → VT100
    feed(t, "\033[H"); // home the cursor
    feed(t, "KLMNOP");
    feed(t, "\033[1;1;1;3${"); // should be inert at VT100
    CHECK(cell(t, 0, 0)->codePoint == 'K');
}

TEST(testShowScrollbarMode30)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(t.scrollbarVisible == true);
    feed(t, "\033[?30$p"); // DECRQM: default set (1)
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?30;1$y") == 0);
    feed(t, "\033[?30l"); // hide
    CHECK(t.scrollbarVisible == false);
    feed(t, "\033[?30$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?30;2$y") == 0);
    feed(t, "\033[?30h"); // show again
    CHECK(t.scrollbarVisible == true);
}

// A UTF-8 text run starting at the continuation half of a wide pair breaks the
// orphaned lead: the run overwrites the cont, so the lead at column-1 must be
// blanked or the row renders a dangling wide start.
TEST(testTextRunClearsLeftEdgeOrphanLead)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 12);

    feed(t, "\033[1;3H\xE6\x97\xA5"); // 日: wide lead at col 2, cont at col 3
    feed(t, "\033[1;4H\xC3\xA9\xC3\xA9"); // run starts on the cont column
    CHECK(cell(t, 0, 2)->codePoint == 0); // orphaned lead blanked
    CHECK(cell(t, 0, 3)->codePoint == 0xE9);
    CHECK(cell(t, 0, 4)->codePoint == 0xE9);
}

// A UTF-8 text run ending on the lead half of a wide pair leaves the cont
// half orphaned past the run's last column: the cont must be blanked.
TEST(testTextRunClearsRightEdgeOrphanCont)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 12);

    feed(t, "\033[1;8H\xE6\x97\xA5"); // 日: wide lead at col 7, cont at col 8
    feed(t, "\033[1;4H\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"); // run ends at col 7
    CHECK(cell(t, 0, 7)->codePoint == 0xE9); // the lead was overwritten
    CHECK(!cell(t, 0, 8)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(cell(t, 0, 8)->codePoint == 0); // orphaned cont blanked
}

// SL and SR act only while the cursor is inside the scrolling margins, as IL
// and DL do; outside them the region is left alone.
TEST(testHorizontalScrollOutsideTheMarginsDoesNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 6);
    feed(t, "ABCDEF\r\nGHIJKL");
    feed(t, "\033[?69h\033[3;5s\033[1;1H\033[ @"); // left of the left margin
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    feed(t, "\033[?69l\033[2;3r\033[1;3H\033[ @"); // above the top margin
    CHECK(cell(t, 1, 0)->codePoint == 'G');
    feed(t, "\033[2;1H\033[ @"); // inside: the row shifts
    CHECK(cell(t, 1, 0)->codePoint == 'H');
}

// CBT walks back through stops to column 1 even inside the left margin, but
// in origin mode the cursor cannot leave the margins, so it stops at the left.
TEST(testBackTabStopsAtTheLeftMarginInOriginMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    feed(t, "\033[?5W\033[?6h\033[?69h\033[3;6s\033[1;2H\033[Z");
    CHECK(t.screens.active->cursorX == 2);
    feed(t, "\033[?6l\033[1;4H\033[Z");
    CHECK(t.screens.active->cursorX == 0);
}

// Every editing and erasing control resets a pending wrap, so the next
// character overwrites the last column; CHT, like HT, keeps it armed.
TEST(testEditingControlsCancelAPendingWrap)
{
    const char* controls[] = { "\033[@", "\033[P", "\033[X", "\033[K", "\033[J", "\033[?K", "\033[?J" };
    for (const char* control : controls) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 2, 5);
        feed(t, "ABCDE");
        feed(t, control);
        feed(t, "X");
        CHECK(cell(t, 0, 4)->codePoint == 'X');
        CHECK(t.screens.active->cursorY == 0);
    }
    for (const char* control : { "\033[L", "\033[M" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 3, 5);
        feed(t, "ABCDE");
        feed(t, control);
        feed(t, "X");
        CHECK(cell(t, 0, 0)->codePoint == 'X');
        CHECK(t.screens.active->cursorY == 0);
    }
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "ABCDE\033[2dX");
    CHECK(cell(t, 1, 4)->codePoint == 'X');
    feed(t, "\033[1;1HABCDE\033[IX");
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    CHECK(cell(t, 1, 0)->codePoint == 'X');
}

// CSI T with five or more parameters is xterm's XTHIMOUSE form, which with no
// highlight pending scrolls nothing. SD takes one parameter, so with two the
// second is ignored and SD scrolls; a lone 0 is SD's default of one line.
TEST(testMultiParameterCSITScrollsNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "1\r\n2\r\n3\033[1;1;1;1;3T\033[1;1;1;1;3;9T");
    CHECK(cell(t, 0, 0)->codePoint == '1');
    CHECK(cell(t, 2, 0)->codePoint == '3');
    feed(t, "\033[1;2T");
    CHECK(cell(t, 1, 0)->codePoint == '1');
    feed(t, "\033[0T");
    CHECK(cell(t, 2, 0)->codePoint == '1');
}

// DECSCA takes 0, 1 and 2; another value leaves the pen's protection as it was.
TEST(testDECSCAUndefinedValuesChangeNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 1, 5);
    feed(t, "\033[1\"q\033[3\"qAB\033[0\"q\033[9\"qCD\033[?2K");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0);
}

UBIQUITTY_TEST_MAIN()

TEST(testSelectiveErasesPreserveEachCellsRendition)
{
    for (const char* command : { "\033[?2K", "\033[?2J", "\033[1;1;1;3${" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[1;31mA\033[0;4;38;2;18;52;86mB\033[1\"qC\033[0\"q\033[0;32m\033[H");
        feed(t, command);
        CHECK(cell(t, 0, 0)->codePoint == 0);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
        CHECK(cell(t, 0, 0)->pen.foregroundIndex == 1);
        CHECK(cell(t, 0, 1)->codePoint == 0);
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
        CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorRGB);
        CHECK(cell(t, 0, 1)->pen.foregroundRed == 18);
        CHECK(cell(t, 0, 1)->pen.foregroundGreen == 52);
        CHECK(cell(t, 0, 1)->pen.foregroundBlue == 86);
        CHECK(cell(t, 0, 2)->codePoint == 'C');
        CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Protected));
    }
}

TEST(testSelectiveErasesClearWholeWideCharactersAndTheirClusters)
{
    for (const char* command : { "\033[?0K", "\033[?0J", "\033[1;3;1;3${" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[1;31mA界\u0301\033[0m\033[1;3H");
        feed(t, command);
        for (int column = 1; column <= 2; ++column) {
            const auto* cleared = cell(t, 0, column);
            CHECK(cleared->codePoint == 0);
            CHECK(cleared->clusterPacked == 0);
            CHECK(!cleared->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
            CHECK(!cleared->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
            CHECK(cleared->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
            CHECK(cleared->pen.foregroundIndex == 1);
        }
        CHECK(cell(t, 0, 0)->codePoint == 'A');
    }
}

TEST(testEraseInFieldUsesBothTabBoundaries)
{
    for (int selector = 0; selector <= 2; ++selector) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 12);
        feed(terminal, "\033[3g\033[3G\033H\033[9G\033H\033[1GABCDEFGHIJKL\033[5G");
        const char* commands[] { "\033[N", "\033[1N", "\033[2N" };
        feed(terminal, commands[selector]);
        for (int column = 0; column < 12; ++column) {
            const bool shouldErase = column >= (selector == 0 ? 4 : 2) && column <= (selector == 1 ? 4 : 7);
            CHECK_EQ(cell(terminal, 0, column)->codePoint, shouldErase ? 0u : static_cast<uint32_t>('A' + column));
        }
        CHECK_EQ(terminal.screens.active->cursorX, 4);
        CHECK_EQ(terminal.screens.active->cursorY, 0);
    }
}

TEST(testEraseInFieldRetainsGuardedCellsAndWholeWideCharacters)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 12);
    feed(terminal, "\033[3g\033[1G\033H\033[9G\033H\033[1G\033VAB\033WCD\r\033[2N");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    feed(terminal, "\033[5G界\033[6G\033[N");
    CHECK_EQ(cell(terminal, 0, 4)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, 0u);
    CHECK(!cell(terminal, 0, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

TEST(testEraseInFieldRejectsUnknownSelectorsAndPreservesPendingWrapWithoutAField)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "\033[3g\033[1G\033H\033[5G\033H\033[1GABCD\033[3G\033[99N");
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, static_cast<uint32_t>('C'));
    feed(terminal, "\033[1;2N");
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 3)->codePoint, static_cast<uint32_t>('D'));
    feed(terminal, "\033[3g\rABCDEF\033[N");
    CHECK(terminal.screens.active->wrapPending);
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, static_cast<uint32_t>('F'));
    feed(terminal, "G");
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, static_cast<uint32_t>('G'));
}

TEST(testHTJMovesTheWholeFieldAndPreservesExplicitSpacesAndRendition)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 12);
    feed(terminal, "\033[3g\033[1G\033H\033[9G\033H\033[1G\033[31mAB \033[0m\033[9GXY\033[2G\033");
    feed(terminal, "I");
    for (int column = 0; column < 5; ++column)
        CHECK_EQ(cell(terminal, 0, column)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, static_cast<uint32_t>(' '));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, static_cast<uint32_t>('X'));
    CHECK_EQ(cell(terminal, 0, 9)->codePoint, static_cast<uint32_t>('Y'));
    CHECK_EQ(cell(terminal, 0, 5)->pen.foregroundIndex, 1u);
    CHECK_EQ(cell(terminal, 0, 7)->pen.foregroundIndex, 1u);
    CHECK_EQ(cell(terminal, 0, 8)->pen.foregroundIndex, Ubiquitty::colorDefault);
    CHECK_EQ(terminal.screens.active->cursorX, 8);
}

TEST(testHTJMovesToTheFollowingStopWhenTheFieldIsFull)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 12);
    feed(terminal, "\033[3g\033[1G\033H\033[9G\033H\033[1GABCDEFGH\033[3G\033I");
    CHECK_EQ(terminal.screens.active->cursorX, 8);
    feed(terminal, "X");
    for (int column = 0; column < 8; ++column)
        CHECK_EQ(cell(terminal, 0, column)->codePoint, static_cast<uint32_t>('A' + column));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, static_cast<uint32_t>('X'));
    CHECK_EQ(terminal.screens.active->cursorX, 9);
}

TEST(testHTJPreservesWideClustersAndSupportsEightBitControls)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 12);
    feed(terminal, "\033[3g\033[1G\033H\033[9G\033H\033[1G界e\u0301\033I");
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, 0x754cu);
    CHECK(cell(terminal, 0, 6)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, static_cast<uint32_t>('e'));
    CHECK_EQ(cell(terminal, 0, 7)->combining[0], 0x301u);
    feed(terminal, "\033[2;1H\033%@AB\x89");
    CHECK_EQ(cell(terminal, 1, 6)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 1, 7)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(terminal.screens.active->cursorX, 8);
}

TEST(testFieldOperationsRespectDoubleWidthAndMissingBoundaries)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 12);
    feed(terminal, "\033[3g\033[1G\033H\033[9G\033H\033[1G\033#6AB\033I\033[2N");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    feed(terminal, "\033[5G\033H\033[3G\033I");
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 0, 3)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(terminal.screens.active->cursorX, 4);
}
