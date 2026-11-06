// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Selection/Selection.h"
#include "Core/Selection/SelectionText.h"
#include "Core/Unicode/UTF8Encoder.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::Selection::appendSelectionText;
using Ubiquitty::Selection::selectionText;
using Ubiquitty::Selection::SoftWrapNewline;
using Ubiquitty::Selection::TextCell;

// A tiny synthetic grid: per (row, column) a code point (0 = void / blank) and
// a wide-continuation flag, plus a per-row soft-wrap flag. Mirrors what the
// frontends feed the policy without any Ubiquitty::Terminal dependency.
struct Grid {
    int rows;
    int columns;
    std::vector<std::uint32_t> codePoints;
    std::vector<std::uint8_t> isContinuation;
    std::vector<std::array<std::uint32_t, 16>> clusterCodePoints;
    std::vector<int> clusterLengths;
    std::vector<std::uint8_t> wrapped;

    Grid(int r, int c)
        : rows(r)
        , columns(c)
        , codePoints(static_cast<std::size_t>(r) * c, 0)
        , isContinuation(static_cast<std::size_t>(r) * c, 0)
        , clusterCodePoints(static_cast<std::size_t>(r) * c)
        , clusterLengths(static_cast<std::size_t>(r) * c, 0)
        , wrapped(static_cast<std::size_t>(r), 0)
    {
    }
    [[nodiscard]] std::size_t index(int row, int column) const { return static_cast<std::size_t>(row) * columns + column; }
    // Fill row `row` from column 0 with a run of single-width code points.
    void setRow(int row, const std::vector<std::uint32_t>& run)
    {
        for (std::size_t i = 0; i < run.size() && static_cast<int>(i) < columns; ++i)
            codePoints[index(row, static_cast<int>(i))] = run[i];
    }
};

static std::uint32_t gridCellAt(void* context, int row, int column)
{
    const Grid* g = static_cast<const Grid*>(context);
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return 0;
    return g->codePoints[g->index(row, column)];
}

static TextCell gridTextCellAt(void* context, int row, int column)
{
    const Grid* g = static_cast<const Grid*>(context);
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return TextCell {};
    const std::size_t index = g->index(row, column);
    TextCell cell { g->codePoints[index], g->isContinuation[index] != 0, {}, 0 };
    cell.codePointCount = g->clusterLengths[index];
    cell.codePoints = g->clusterCodePoints[index];
    return cell;
}

static int gridRowWrapped(void* context, int row)
{
    const Grid* g = static_cast<const Grid*>(context);
    if (row < 0 || row >= g->rows)
        return 0;
    return g->wrapped[static_cast<std::size_t>(row)] ? 1 : 0;
}

// "ab" over two single-width cells.
static std::vector<std::uint32_t> letters(const char* s)
{
    std::vector<std::uint32_t> run;
    for (const char* p = s; *p; ++p)
        run.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(*p)));
    return run;
}

// --- Soft-wrap newline suppression (Join) --------------------------------

// The trailing-blank scan decides where a row's text ends by reading each cell's
// code point. Read sixteen bits wide, U+10020 is indistinguishable from a space
// and U+10000 from an empty cell, so a row ending in either loses its last
// character to the trim rather than copying it.
TEST(testARowEndingAboveTheBasicPlaneKeepsItsLastCharacter)
{
    for (const std::uint32_t codePoint : { 0x10020u, 0x10000u }) {
        Grid g(1, 8);
        g.setRow(0, { 'h', 'i', codePoint });
        Ubiquitty::SelectionBounds b { 0, 0, 0, 7 };
        std::string expected = "hi";
        Ubiquitty::Unicode::appendUTF8(expected, codePoint);
        CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g), expected);
    }

    // The control: a cell that really is a space is still trimmed, so the case
    // above is not passing because the trim stopped working.
    Grid blank(1, 8);
    blank.setRow(0, { 'h', 'i', ' ' });
    Ubiquitty::SelectionBounds b { 0, 0, 0, 7 };
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, blank.columns, gridCellAt, &blank, gridTextCellAt, &blank, SoftWrapNewline::Keep, gridRowWrapped, &blank), "hi");
}
static int gridRowExtent(void* context, int row)
{
    const Grid* g = static_cast<const Grid*>(context);
    if (row < 0 || row >= g->rows)
        return 0;
    // The fixture's own stored width: the columns actually written.
    int extent = 0;
    for (int column = 0; column < g->columns; ++column) {
        if (g->codePoints[g->index(row, column)] != 0)
            extent = column + 1;
    }
    return extent;
}

static int gridRowExtentTooShort(void* context, int row)
{
    const int honest = gridRowExtent(context, row);
    return honest > 1 ? honest - 1 : honest;
}

// The trailing-blank scan may start at the row's stored end instead of the
// screen's. The contract is one-sided: an extent past the content only costs a
// longer scan, while one short of it trims content away.
TEST(testARowExtentStartsTheTrailingScanWithoutMovingIt)
{
    Grid g(2, 16);
    g.setRow(0, letters("hi there"));
    g.setRow(1, letters("second"));
    Ubiquitty::SelectionBounds b { 0, 0, 1, 15 };

    const std::string withoutExtent = selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g);
    const std::string withExtent = selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g, gridRowExtent, &g);
    CHECK_STR_EQ(withExtent, "hi there\nsecond");
    CHECK_STR_EQ(withoutExtent, withExtent);

    // The full width is always a correct answer, and must not change the text.
    const std::string wideExtent = selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g, [](void* context, int) { return static_cast<const Grid*>(context)->columns; }, &g);
    CHECK_STR_EQ(wideExtent, withExtent);

    // The control: an extent one short of the content loses a character, which
    // is what the one-sided contract forbids and what this test would miss if
    // the extent were ignored rather than honoured.
    const std::string shortExtent = selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g, gridRowExtentTooShort, &g);
    CHECK_STR_EQ(shortExtent, "hi ther\nsecon");
}

TEST(testSoftWrapJoin)
{
    // Two-row char selection. Row 0 is FULL (8 cols of content) and soft-wraps
    // into row 1, so the reflowed logical line copies back as one line.
    Grid g(2, 8);
    g.setRow(0, letters("hello wo")); // fills all 8 columns
    g.setRow(1, letters("rld")); // 3 columns, rest void
    g.wrapped[0] = 1; // row 0 wraps into row 1

    Ubiquitty::SelectionBounds b { 0, 0, 1, 2 }; // through "rld" on row 1

    // Join: no newline at the soft-wrap boundary → one logical line.
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g), "hello world");

    // Keep: a newline at every row boundary regardless of wrap (block copy).
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g), "hello wo\nrld");

    // A null rowWrapped callback forces a newline between every row even in Join.
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, nullptr, nullptr), "hello wo\nrld");
}

TEST(testHardNewlineKept)
{
    // Row 0 not wrapped — a real line break survives the Join policy.
    Grid g(2, 8);
    g.setRow(0, letters("alpha")); // 5 cols, trailing void trimmed
    g.setRow(1, letters("beta")); // 4 cols
    g.wrapped[0] = 0;

    Ubiquitty::SelectionBounds b { 0, 0, 1, 3 };
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g), "alpha\nbeta");
}

// --- Rectangular block extraction ----------------------------------------

TEST(testRectangularBlock)
{
    // 3 rows × 6 cols. A rectangular selection clips every row to the same
    // [column1, column3] span and emits each as its own physical line — no
    // reflow, even though the per-row content differs in width.
    Grid g(3, 6);
    g.setRow(0, letters("ABCDEF"));
    g.setRow(1, letters("abcdef"));
    g.setRow(2, letters("012345"));

    Ubiquitty::SelectionBounds b { 0, 1, 2, 3 }; // columns 1..3 on each row

    // Rect always keeps row newlines (block copy) regardless of wrap state.
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Rectangle, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g), "BCD\nbcd\n123");

    // Even a wrapped row inside the block keeps its newline under Keep.
    g.wrapped[0] = 1;
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Rectangle, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g), "BCD\nbcd\n123");
}

TEST(testRectangularTrimsTrailingBlanks)
{
    // The block span runs past the content on a short row; trailing blanks
    // within the span are trimmed per row.
    Grid g(2, 6);
    g.setRow(0, letters("ABCDEF"));
    g.setRow(1, letters("ab")); // columns 2..3 are void on this row

    Ubiquitty::SelectionBounds b { 0, 0, 1, 4 }; // columns 0..4
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Rectangle, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g), "ABCDE\nab");
}

// --- Wide-glyph continuation + astral code points ------------------------

TEST(testWideContinuationSkipped)
{
    // A wide glyph occupies two cells: the lead carries the code point, the
    // continuation half is flagged and must not be emitted again.
    Grid g(1, 4);
    g.codePoints[g.index(0, 0)] = 0x4E16; // 世 (lead)
    g.codePoints[g.index(0, 1)] = 0x4E16; // continuation carries the same cp
    g.isContinuation[g.index(0, 1)] = 1;
    g.codePoints[g.index(0, 2)] = 0x754C; // 界 (lead)
    g.codePoints[g.index(0, 3)] = 0x754C;
    g.isContinuation[g.index(0, 3)] = 1;

    Ubiquitty::SelectionBounds b { 0, 0, 0, 3 };
    // Expect 世界 once each (6 UTF-8 bytes), not duplicated.
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g),
        std::string("\xE4\xB8\x96\xE7\x95\x8C")); // 世界
}

// The consequence of resolving bounds without `attributesAt`, stated where it
// is visible: a `column0` on a wide glyph's continuation half drops that glyph
// from the text entirely.
//
// The continuation cell repeats the lead's code point — see the case above —
// and the walk skips it so a wide glyph is not emitted twice. A bound
// that starts there therefore starts one column past the only cell that would
// have produced the character. Nothing reports it: the row is not short, no
// count is wrong, and the bytes that do come back are valid text.
//
// `Selection::getBoundsWide` with an `attributesAt` snaps such an anchor to the
// leading half and this cannot arise; `Selection::getBounds` performs no snap
// and it can. Both entry points are called from frontends today, so this is the
// gap between them measured in what a user would paste.
TEST(boundsAnchoredOnAContinuationDropTheGlyphItBelongsTo)
{
    Grid g(1, 4);
    g.codePoints[g.index(0, 0)] = 0x4E16; // 世, lead
    g.codePoints[g.index(0, 1)] = 0x4E16; // its continuation, same code point
    g.isContinuation[g.index(0, 1)] = 1;
    g.codePoints[g.index(0, 2)] = 0x754C; // 界, lead
    g.codePoints[g.index(0, 3)] = 0x754C;
    g.isContinuation[g.index(0, 3)] = 1;

    // The snapped bound, which is what a wide-aware caller resolves.
    Ubiquitty::SelectionBounds snapped { 0, 0, 0, 3 };
    CHECK_STR_EQ(selectionText(&snapped, Ubiquitty::SelectionMode::Character, g.columns,
                     gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g),
        std::string("\xE4\xB8\x96\xE7\x95\x8C")); // 世界

    // The same gesture resolved without the snap: one column right, and 世 is
    // gone rather than clipped. Six bytes become three.
    Ubiquitty::SelectionBounds unsnapped { 0, 1, 0, 3 };
    CHECK_STR_EQ(selectionText(&unsnapped, Ubiquitty::SelectionMode::Character, g.columns,
                     gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g),
        std::string("\xE7\x95\x8C")); // 界 alone
}

TEST(testAstralCodePoints)
{
    // Astral / emoji code points must emit their 4-byte form, which a BMP-only
    // encoder drops.
    Grid g(1, 3);
    g.codePoints[g.index(0, 0)] = static_cast<std::uint32_t>('x');
    g.codePoints[g.index(0, 1)] = 0x1F600; // 😀
    g.codePoints[g.index(0, 2)] = 0x1F4A9; // 💩

    Ubiquitty::SelectionBounds b { 0, 0, 0, 2 };
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g), std::string("x\xF0\x9F\x98\x80\xF0\x9F\x92\xA9"));
}

TEST(testZWJClusterCopiedAsFullSequence)
{
    Grid g(1, 2);
    const std::array<std::uint32_t, 16> family { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
    const std::size_t lead = g.index(0, 0);
    g.codePoints[lead] = family[0];
    g.clusterCodePoints[lead] = family;
    g.clusterLengths[lead] = 7;
    g.codePoints[g.index(0, 1)] = 0x4E16;
    g.isContinuation[g.index(0, 1)] = 1;

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g),
        std::string("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6"));
}

// --- appendSelectionText accumulates into an existing buffer --------------

TEST(testAppendAccumulates)
{
    Grid g(1, 3);
    g.setRow(0, letters("hey"));
    Ubiquitty::SelectionBounds b { 0, 0, 0, 2 };
    std::string out = "prefix:";
    appendSelectionText(out, &b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Join, gridRowWrapped, &g);
    CHECK_STR_EQ(out, "prefix:hey");
}

TEST(testNullBoundsNoOp)
{
    std::string out = "keep";
    appendSelectionText(out, nullptr, Ubiquitty::SelectionMode::Character, 8, gridCellAt, nullptr, gridTextCellAt, nullptr, SoftWrapNewline::Join, gridRowWrapped, nullptr);
    CHECK_STR_EQ(out, "keep");
}

// --- rectangular-extraction lock: a rectangular block over a tiled multi-pane window row -------

TEST(test759RectangularPaneColumnsNoInterleave)
{
    // In tmux NORMAL mode a single WINDOW row spans tiled panes: here cols 0..3
    // are pane A, cols 4..7 are pane B. A rectangular block over pane B's
    // columns must extract only pane B's cells, on every row — the Core never
    // interleaves panes GIVEN the right column bounds. The garble therefore
    // originates upstream (the copy path feeding window-composite bounds/Terminal),
    // not in this extraction.
    Grid g(2, 8);
    g.setRow(0, letters("AAAABBBB")); // paneA=AAAA, paneB=BBBB
    g.setRow(1, letters("aaaabbbb")); // paneA=aaaa, paneB=bbbb

    Ubiquitty::SelectionBounds b { 0, 4, 1, 7 }; // pane B's columns 4..7, both rows
    CHECK_STR_EQ(selectionText(&b, Ubiquitty::SelectionMode::Rectangle, g.columns, gridCellAt, &g, gridTextCellAt, &g, SoftWrapNewline::Keep, gridRowWrapped, &g),
        "BBBB\nbbbb"); // pane B only — no "AAAABBBB" interleave
}

UBIQUITTY_TEST_MAIN()
