// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::cell;
using UbiquittyTest::drain;
using UbiquittyTest::feed;

// RECTANGLES / PROTECTION / SELECTIVE ERASE (Source/Core/VT/Prefixed.cpp) —
// the DEC rectangle family, DECSCA and DECRQCRA (https://vt100.net/docs/,
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html). Three rules:
//
//   * DECRQCRA sums character values only: no video attribute may enter the
//     checksum, or a single-cell request stops identifying the character in it.
//   * Rectangle coordinates follow DECOM, and an out-of-order rectangle is
//     skipped rather than clamped into a sliver.
//   * DECSED / DECSEL / DECSERA erase only DECSCA-unprotected cells, and the
//     first two ignore the scrolling region entirely.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/TerminalOperations.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

static unsigned checksumOfRect(Ubiquitty::Terminal& t, int top, int left, int bot, int right)
{
    char request[64];
    std::snprintf(request, sizeof(request), "\033[1;1;%d;%d;%d;%d*y", top, left, bot, right);
    feed(t, request);
    char outputBuffer[64];
    drain(t, outputBuffer);
    const char* body = strstr(outputBuffer, "!~");
    if (!body)
        return 0xFFFFFFFFu; // no reply at all — never a legal checksum
    unsigned value = 0;
    if (std::sscanf(body + 2, "%4x", &value) != 1)
        return 0xFFFFFFFFu;
    return value;
}

// DECRQCRA reports the negated sum, so a single-cell request inverts straight
// back to that cell's character value. 1-based coordinates, like the wire.
static unsigned cellOrdinal(Ubiquitty::Terminal& t, int column, int row)
{
    unsigned checksum = checksumOfRect(t, row, column, row, column);
    return (0x10000u - checksum) & 0xFFFFu;
}

// The 8x8 block every rectangle test below is scored against.
static void prepareRectData(Ubiquitty::Terminal& t)
{
    feed(t, "\033[H");
    feed(t, "abcdefgh\r\n");
    feed(t, "ijklmnop\r\n");
    feed(t, "qrstuvwx\r\n");
    feed(t, "yz012345\r\n");
    feed(t, "ABCDEFGH\r\n");
    feed(t, "IJKLMNOP\r\n");
    feed(t, "QRSTUVWX\r\n");
    feed(t, "YZ6789!@\r\n");
}

static bool rectDataIntact(Ubiquitty::Terminal& t)
{
    static const char* rows[8] = { "abcdefgh", "ijklmnop", "qrstuvwx", "yz012345", "ABCDEFGH", "IJKLMNOP", "QRSTUVWX", "YZ6789!@" };
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            const Ubiquitty::Cell* c = cell(t, row, column);
            if (!c || c->codePoint != static_cast<uint32_t>(rows[row][column]))
                return false;
        }
    }
    return true;
}

// Every screen read below goes through DECRQCRA, so an attribute offset in the
// sum is indistinguishable from wrong screen content. A DECSCA-protected,
// fully-rendered cell must still check out as its own character.
static void testDECRQCRAIgnoresVideoAttributes()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "\033[1;4;5;7;8m"); // bold, underline, blink, reverse, invisible
    feed(t, "\033[1\"q"); // DECSCA — protect
    feed(t, "b");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c && c->codePoint == 'b');
    CHECK(c && Ubiquitty::cellPen(*c).attributes.contains(Ubiquitty::VT::Attribute::Protected));
    CHECK(c && Ubiquitty::cellPen(*c).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cellOrdinal(t, 1, 1) == 'b');
}

// A blank cell checksums as a space, whether it was never written or erased:
// the internal "no code point" sentinel must not reach the sum as 0.
static void testDECRQCRAUntouchedCellReadsAsSpace()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(cellOrdinal(t, 5, 5) == ' ');
    feed(t, "\033[5;5HX");
    CHECK(cellOrdinal(t, 5, 5) == 'X');
    feed(t, "\033[5;5H\033[K"); // EL — erase to end of line
    CHECK(cellOrdinal(t, 5, 5) == ' ');
}

// With DECOM set, the requested rectangle is relative to the scrolling region's
// origin — both axes.
static void testDECRQCRARespectsOriginMode()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // 'Y' sits where a row-only origin shift would land, so reading 'X' proves
    // the column shift happened too.
    feed(t, "\033[5;1HY");
    feed(t, "\033[5;5HX");
    feed(t, "\033[5;7r"); // DECSTBM 5..7
    feed(t, "\033[?69h"); // DECLRMM
    feed(t, "\033[5;7s"); // DECSLRM 5..7
    feed(t, "\033[?6h"); // DECOM
    CHECK(cellOrdinal(t, 1, 1) == 'X');
}

// Bottom above top (or right left of left) is not a 1x1 rectangle at the
// corner — the whole operation is skipped.
static void testDecRectOpsSkipInvalidRectangles()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    prepareRectData(t);
    feed(t, "\033[37;5;5;4;4$x"); // DECFRA '%' into top=5 left=5 bot=4 right=4
    CHECK(rectDataIntact(t));

    feed(t, "\033[5;5;4;4$z"); // DECERA
    CHECK(rectDataIntact(t));

    feed(t, "\033[5;5;4;4${"); // DECSERA
    CHECK(rectDataIntact(t));

    // Same rule for a DECCRA source rectangle.
    feed(t, "\033[2;2;1;1;1;5;5;1$v");
    CHECK(rectDataIntact(t));

    // A rectangle that is merely oversized is still ordered, so it acts on the
    // part that exists.
    feed(t, "\033[37;1;1;1;999$x");
    for (int column = 0; column < t.columns; column++)
        CHECK(cell(t, 0, column)->codePoint == '%');
}

// With margins at 2 and DECOM set, a 1,1..3,3 fill lands on 2,2..4,4.
static void testDecRectOpsRespectOriginMode()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    prepareRectData(t);
    feed(t, "\033[?69h"); // DECLRMM
    feed(t, "\033[2;9s"); // DECSLRM 2..9
    feed(t, "\033[2;9r"); // DECSTBM 2..9
    feed(t, "\033[?6h"); // DECOM
    feed(t, "\033[37;1;1;3;3$x"); // DECFRA '%'
    feed(t, "\033[?69l\033[r\033[?6l");

    static const char untouchedFirstColumn[3] = { 'i', 'q', 'y' };
    for (int row = 1; row <= 3; row++) {
        CHECK(cell(t, row, 0)->codePoint == static_cast<uint32_t>(untouchedFirstColumn[row - 1]));
        for (int column = 1; column <= 3; column++)
            CHECK(cell(t, row, column)->codePoint == '%');
        CHECK(cell(t, row, 4)->codePoint != '%');
    }
    // Row 1 and the row below the rectangle are untouched.
    CHECK(cell(t, 0, 1)->codePoint == 'b');
    CHECK(cell(t, 4, 1)->codePoint == 'B');
}

// The DECCRA destination corner is origin-relative too, so 1,1..3,3 -> 4,4
// copies 2,2..4,4 -> 5,5.
static void testDECCRADestinationRespectsOriginMode()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    prepareRectData(t);
    feed(t, "\033[?69h");
    feed(t, "\033[2;9s");
    feed(t, "\033[2;9r");
    feed(t, "\033[?6h");
    feed(t, "\033[1;1;3;3;1;4;4;1$v"); // DECCRA
    feed(t, "\033[?69l\033[r\033[?6l");

    static const char* expected[3] = { "ABCDjklH", "IJKLrstP", "QRSTz01X" };
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 8; column++)
            CHECK(cell(t, 4 + row, column)->codePoint == static_cast<uint32_t>(expected[row][column]));
}

// DECCRA whose destination runs off the grid clips the copy at the far edges
// instead of writing out of bounds: the rows and columns past the last one are
// dropped, the cells that fit still receive their source, and the row scrub
// skips the rows that do not exist. A destination edge left unclamped would
// write past the grid — this is the guard that stops it.
static void testDECCRAClipsDestinationAtGridEdge()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 10);
    prepareRectData(t);
    // Copy the 3x3 top-left block (rows 1..3, cols 1..3 — a b c / i j k / q r s)
    // to 9,9: its bottom row and right column fall off the 10x10 grid.
    feed(t, "\033[1;1;3;3;1;9;9;1$v");
    // The corner that fits took the source's top-left 2x2 — a b over i j.
    CHECK(cell(t, 8, 8)->codePoint == 'a');
    CHECK(cell(t, 8, 9)->codePoint == 'b');
    CHECK(cell(t, 9, 8)->codePoint == 'i');
    CHECK(cell(t, 9, 9)->codePoint == 'j');
    // The source block is read, not moved, so the original 8x8 stays intact.
    CHECK(rectDataIntact(t));
}

// DECSERA erases the rectangle but steps over every DECSCA-protected cell
// inside it.
static void testDECSERASkipsProtectedCells()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "\033[1\"qabcdefgh\r\n"); // row 1 protected
    feed(t, "\033[0\"qijklmnop\r\n"); // row 2 unprotected
    feed(t, "\033[1;3;2;6${"); // DECSERA rows 1..2, columns 3..6

    for (int column = 0; column < 8; column++)
        CHECK(cell(t, 0, column)->codePoint == static_cast<uint32_t>("abcdefgh"[column]));
    CHECK(cell(t, 1, 1)->codePoint == 'j');
    for (int column = 2; column <= 5; column++)
        CHECK(cell(t, 1, column)->codePoint == 0);
    CHECK(cell(t, 1, 6)->codePoint == 'o');
}

// DECSED erases forward from the cursor over the whole page — the scrolling
// region does not bound it — and leaves protected cells alone.
static void testDECSEDErasesOnlyUnprotectedCells()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"q"); // DECSCA — protect what follows
    feed(t, "\033[1;1Habcde");
    feed(t, "\033[2;1Hfghij");
    feed(t, "\033[3;1Hklmno");
    feed(t, "\033[0\"q"); // DECSCA off
    feed(t, "\033[3;1HX"); // unprotected X over the 'k'
    feed(t, "\033[?69h\033[2;4s"); // DECLRMM + DECSLRM 2..4
    feed(t, "\033[2;3r"); // DECSTBM 2..3
    feed(t, "\033[2;3H"); // row 2, column 3 — inside both margins
    feed(t, "\033[?0J"); // DECSED 0
    feed(t, "\033[?69l\033[r");

    for (int column = 0; column < 5; column++) {
        CHECK(cell(t, 0, column)->codePoint == static_cast<uint32_t>("abcde"[column]));
        CHECK(cell(t, 1, column)->codePoint == static_cast<uint32_t>("fghij"[column]));
    }
    CHECK(cell(t, 2, 0)->codePoint == 0); // X was unprotected — erased
    for (int column = 1; column < 5; column++)
        CHECK(cell(t, 2, column)->codePoint == static_cast<uint32_t>("klmno"[column]));
}

// DECSEL covers the whole line regardless of DECSLRM, and skips protected cells.
static void testDECSELIgnoresScrollRegionAndProtection()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"q");
    feed(t, "\033[1;1Habcdefghij");
    feed(t, "\033[0\"q");
    feed(t, "\033[1;1HX"); // unprotected X over the 'a'
    feed(t, "\033[?69h\033[2;4s"); // DECLRMM + DECSLRM 2..4
    feed(t, "\033[1;5H");
    feed(t, "\033[?2K"); // DECSEL 2 — whole line
    feed(t, "\033[?69l");

    CHECK(cell(t, 0, 0)->codePoint == 0);
    for (int column = 1; column < 10; column++)
        CHECK(cell(t, 0, column)->codePoint == static_cast<uint32_t>("abcdefghij"[column]));
}

// Only DECSCA Ps=1 protects; 0 and 2 both clear. And DECSED Ps=3 (erase
// scrollback) must not touch the screen.
static void testDecsca2ClearsProtectionAndDecsed3IsScreenInert()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "\033[1\"qP"); // protected
    feed(t, "\033[2\"qU"); // DECSCA 2 == DECSCA 0 → unprotected
    CHECK(!Ubiquitty::cellPen(*cell(t, 0, 1)).attributes.contains(Ubiquitty::VT::Attribute::Protected));
    feed(t, "\033[H\033[?2K"); // DECSEL 2
    CHECK(cell(t, 0, 0)->codePoint == 'P');
    CHECK(cell(t, 0, 1)->codePoint == 0);

    feed(t, "\033[H\033[1\"qQ\033[0\"q");
    feed(t, "\033[?3J"); // DECSED 3 — scrollback only
    CHECK(cell(t, 0, 0)->codePoint == 'Q');
}

static void testGuardedAreaProtectsFromEDButDECSCADoesNot()
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[H\033VG\033W"); // SPA, one guarded cell, EPA
        feed(t, "\033[2;1HU"); // an unguarded cell on the next row
        feed(t, "\033[H\033[J"); // ED 0 — erase to end of screen
        CHECK(cellOrdinal(t, 1, 1) == 'G'); // ISO-guarded: survives
        CHECK(cellOrdinal(t, 2, 1) == ' '); // unguarded: erased
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[H\033[1\"q"
                "D\033[0\"q");
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        CHECK(c && Ubiquitty::cellPen(*c).attributes.contains(Ubiquitty::VT::Attribute::Protected));
        feed(t, "\033[H\033[J");
        CHECK(cellOrdinal(t, 1, 1) == ' ');
    }
    {
        // DECSTR resets the pen without changing existing cell attributes.
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[H\033VG\033W");
        feed(t, "\033[!p"); // DECSTR — soft reset
        feed(t, "\033[H\033[J");
        CHECK(cellOrdinal(t, 1, 1) == 'G');
    }
}

// DECCARA under DECSACE 2 touches the block and nothing beside it.
//
// The two extents differ only in what the four coordinates MEAN, so a rectangle
// that leaked past its columns would look on screen exactly like a stream one —
// which is how the overflow gets read as a defect rather than as the default it
// is. Asserting the cells immediately outside the span is what tells them apart.
static void testDECCARARectangleExtentStaysInsideItsColumns()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 10);
    prepareRectData(t);

    feed(t, "\033[2*x"); // DECSACE 2 — rectangle
    feed(t, "\033[2;3;4;5;4$r"); // rows 2..4, columns 3..5, set underline

    for (int row = 1; row <= 3; row++) {
        for (int column = 2; column <= 4; column++)
            CHECK(cell(t, row, column) && Ubiquitty::cellPen(*cell(t, row, column)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
        CHECK(cell(t, row, 1) && !Ubiquitty::cellPen(*cell(t, row, 1)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
        CHECK(cell(t, row, 5) && !Ubiquitty::cellPen(*cell(t, row, 5)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    }
    for (int column = 0; column < 10; column++) {
        CHECK(cell(t, 0, column) && !Ubiquitty::cellPen(*cell(t, 0, column)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
        CHECK(cell(t, 4, column) && !Ubiquitty::cellPen(*cell(t, 4, column)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    }
    CHECK(rectDataIntact(t)); // attributes only: DECCARA never rewrites text
}

// DECSACE 1 is a paragraph, not a box, and that is the spec's default.
//
// The first row runs from the start column to the right margin, middle rows span
// the full width, and the last row runs from the left margin to the end column —
// so cells outside the column span, blank ones past the text included, are
// covered by design. Pinned because "the run extends outside the box on both
// sides" is a correct observation of a correct implementation, and without this
// there is nothing distinguishing it from a defect.
static void testDECCARAStreamExtentRunsInReadingOrder()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 10);
    prepareRectData(t);

    feed(t, "\033[1*x"); // DECSACE 1 — stream, the default
    feed(t, "\033[2;3;4;5;4$r"); // the same coordinates as the rectangle case

    CHECK(cell(t, 1, 1) && !Ubiquitty::cellPen(*cell(t, 1, 1)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 1, 2) && Ubiquitty::cellPen(*cell(t, 1, 2)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 1, 9) && Ubiquitty::cellPen(*cell(t, 1, 9)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 2, 0) && Ubiquitty::cellPen(*cell(t, 2, 0)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 2, 9) && Ubiquitty::cellPen(*cell(t, 2, 9)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 3, 0) && Ubiquitty::cellPen(*cell(t, 3, 0)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 3, 4) && Ubiquitty::cellPen(*cell(t, 3, 4)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 3, 5) && !Ubiquitty::cellPen(*cell(t, 3, 5)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    // Rows outside the extent stay clean, so "stream" does not mean "everything".
    CHECK(cell(t, 0, 5) && !Ubiquitty::cellPen(*cell(t, 0, 5)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 4, 5) && !Ubiquitty::cellPen(*cell(t, 4, 5)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(rectDataIntact(t));
}

// A single-row extent is the same block under either setting, which is why both
// callers special-case it rather than letting the stream walk reach the margins.
static void testDECCARASingleRowIsConfinedUnderStream()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 10);
    prepareRectData(t);

    feed(t, "\033[1*x"); // stream
    feed(t, "\033[3;3;3;5;4$r"); // row 3 only, columns 3..5

    CHECK(cell(t, 2, 1) && !Ubiquitty::cellPen(*cell(t, 2, 1)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    for (int column = 2; column <= 4; column++)
        CHECK(cell(t, 2, column) && Ubiquitty::cellPen(*cell(t, 2, column)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 2, 5) && !Ubiquitty::cellPen(*cell(t, 2, 5)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 2, 9) && !Ubiquitty::cellPen(*cell(t, 2, 9)).attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(rectDataIntact(t));
}

// DECCARA and DECRARA keep the underline shape with the underline bit, and
// reach Hidden through 8 and 28.
static void testRectangleRenditionKeepsUnderlineShapeAndHidden()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 6);
    feed(t, "abcd\033[2*x\033[1;1;1;1;4$r\033[1;2;1;2;8$r");
    CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*cell(t, 0, 0))) == Ubiquitty::VT::underlineSingle);
    CHECK(Ubiquitty::cellPen(*cell(t, 0, 1)).attributes.contains(Ubiquitty::VT::Attribute::Hidden));
    feed(t, "\033[1;1;1;2;24;28$r");
    CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*cell(t, 0, 0))) == Ubiquitty::VT::underlineNone);
    CHECK(!Ubiquitty::cellPen(*cell(t, 0, 1)).attributes.contains(Ubiquitty::VT::Attribute::Hidden));
    feed(t, "\033[1;3;1;3;4$t");
    CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*cell(t, 0, 2))) == Ubiquitty::VT::underlineSingle);
}

int runRectConformanceTests()
{
    testDECCARARectangleExtentStaysInsideItsColumns();
    testDECCARAStreamExtentRunsInReadingOrder();
    testDECCARASingleRowIsConfinedUnderStream();
    testDECRQCRAIgnoresVideoAttributes();
    testDECRQCRAUntouchedCellReadsAsSpace();
    testDECRQCRARespectsOriginMode();
    testDecRectOpsSkipInvalidRectangles();
    testDecRectOpsRespectOriginMode();
    testDECCRADestinationRespectsOriginMode();
    testDECCRAClipsDestinationAtGridEdge();
    testDECSERASkipsProtectedCells();
    testDECSEDErasesOnlyUnprotectedCells();
    testDECSELIgnoresScrollRegionAndProtection();
    testDecsca2ClearsProtectionAndDecsed3IsScreenInert();
    testGuardedAreaProtectsFromEDButDECSCADoesNot();
    testRectangleRenditionKeepsUnderlineShapeAndHidden();

    if (failures) {
        printf("%d conformance-rect test failure(s)\n", failures);
        return 1;
    }
    printf("conformance rect tests passed\n");
    return 0;
}

// Defined out when several cluster conformance files are linked into one
// binary; that driver supplies main and calls the run*Tests entry points.
#ifndef UBIQUITTY_CONFORMANCE_TESTS_COMBINED
int main() { return runRectConformanceTests() == 0 ? 0 : 1; }
#endif

// Rect.cpp coverage ledger. The lines these tests leave dark are guards that are
// unreachable by contract, not untested behaviour:
//
//   * The `if (!t.grid) return;` head of every rect op. A constructed Terminal
//     owns its grid for its whole life, so the pointer is never null here.
//   * The degenerate-extent returns (`width <= 0`, `h <= 0 || w <= 0`). The lone
//     parser that builds a RectArea — VT/Prefixed.cpp's decRectFromParameters —
//     returns false for an out-of-order rectangle rather than an empty one, so
//     every RectArea that arrives has right >= left and bot >= top.
//   * The maxColumns / maxRows clamps. That same parser clamps the far edges to
//     the grid, whose dimensions never exceed those caps, so the span cannot.
//   * rectCopy's `if (!buffer) return;` — an allocation failure the coverage
//     instrument cannot force.
//   * walkSaceExtent's `sace == 2 || single-row` branch. Its only callers,
//     rectReverseAttributes and rectChangeAttributes, handle that case inline
//     and reach the walker solely from their stream, multi-row else-path, so the
//     branch is dead from every call site.
//   * The `if (!codePoint) return;` guards in those two stream walks. The walk
//     visits only in-range coordinates, and cellAt returns null only out of one.
