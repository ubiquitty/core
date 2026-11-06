// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// test_styledclipboard.cpp — the portable selection → styled (RTF/HTML) copy
// generator (Source/Core/Selection/StyledClipboard.{h,cpp}) plus the shell-escape
// (Source/Core/Input/PasteNormalize.cpp Ubiquitty::Input::shellEscape).
//
// The generator is fed a synthetic styled grid (no Terminal / Cell), so the test
// pins exactly what survives in each StyleColorMode (Full / NoBackground /
// StylesOnly), a colored-bold run, an inverse-resolved (swapped) run, and the
// RTF color table + HTML span structure.
//

#include "Core/Input/PasteNormalize.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/StyledClipboard.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::Input::shellEscape;
using Ubiquitty::Selection::buildStyledClipboard;
using Ubiquitty::Selection::SoftWrapNewline;
using Ubiquitty::Selection::StyleColorMode;
using Ubiquitty::Selection::StyledCell;
using Ubiquitty::Selection::StyledClipboard;
using Ubiquitty::Selection::StyledFont;

// A synthetic styled grid: per (row, column) a StyledCell. Mirrors what the
// macOS copy path feeds after resolving each cell through CellPalette::resolve.
struct StyledGrid {
    int rows;
    int columns;
    std::vector<StyledCell> cells;
    int textReadCount = 0;
    int styleReadCount = 0;

    StyledGrid(int r, int c)
        : rows(r)
        , columns(c)
        , cells(static_cast<std::size_t>(r) * c)
    {
        for (auto& cell : cells)
            cell = StyledCell {};
    }
    [[nodiscard]] std::size_t index(int row, int column) const { return static_cast<std::size_t>(row) * columns + column; }
    StyledCell& at(int row, int column) { return cells[index(row, column)]; }
};

static std::uint32_t gridCellAt(void* context, int row, int column)
{
    const StyledGrid* g = static_cast<const StyledGrid*>(context);
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return 0;
    return g->cells[g->index(row, column)].codePoint;
}

static StyledCell gridStyledAt(void* context, int row, int column)
{
    StyledGrid* g = static_cast<StyledGrid*>(context);
    ++g->styleReadCount;
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return StyledCell {};
    return g->cells[g->index(row, column)];
}

static Ubiquitty::Selection::TextCell gridTextAt(void* context, int row, int column)
{
    StyledGrid& grid = *static_cast<StyledGrid*>(context);
    ++grid.textReadCount;
    if (row < 0 || row >= grid.rows || column < 0 || column >= grid.columns) {
        return {};
    }
    const StyledCell& styled = grid.cells[grid.index(row, column)];
    return { styled.codePoint, styled.isContinuation, styled.codePoints, styled.codePointCount };
}

static StyledCell makeCell(std::uint32_t codePoint, std::uint8_t fr, std::uint8_t fg, std::uint8_t fb, std::uint8_t br, std::uint8_t bg, std::uint8_t bb, bool bold, bool italic, bool underline) { return StyledCell { codePoint, false, { fr, fg, fb }, { br, bg, bb }, bold, italic, underline, {}, 0 }; } // bool-ok: the flags carry StyledCell's own field names, and a fixture should name the type it builds

static const std::array<std::uint8_t, 3> defaultForeground = { 192, 192, 192 };
static const std::array<std::uint8_t, 3> defaultBackground = { 0, 0, 0 };
static const StyledFont font { "TestMono", 13.0 };

TEST(voidTailTrimmingReadsNeitherClustersNorStyles)
{
    StyledGrid grid(1, 1024);
    const Ubiquitty::SelectionBounds bounds { 0, 0, 0, grid.columns - 1 };
    const auto copied = buildStyledClipboard(&bounds, Ubiquitty::SelectionMode::Character, grid.columns,
        gridCellAt, &grid, gridTextAt, &grid, gridStyledAt, &grid, SoftWrapNewline::Join, nullptr, nullptr,
        StyleColorMode::Full, font, defaultForeground, defaultBackground);
    CHECK(copied.plain.empty());
    CHECK_EQ(grid.textReadCount, 0);
    CHECK_EQ(grid.styleReadCount, 0);
}

TEST(spaceTailTrimmingReadsClustersWithoutResolvingStyles)
{
    StyledGrid grid(1, 1024);
    for (auto& cell : grid.cells) {
        cell.codePoint = ' ';
        cell.codePoints = { ' ', ' ' };
        cell.codePointCount = 2;
    }
    const Ubiquitty::SelectionBounds bounds { 0, 0, 0, grid.columns - 1 };
    const auto copied = buildStyledClipboard(&bounds, Ubiquitty::SelectionMode::Character, grid.columns,
        gridCellAt, &grid, gridTextAt, &grid, gridStyledAt, &grid, SoftWrapNewline::Join, nullptr, nullptr,
        StyleColorMode::Full, font, defaultForeground, defaultBackground);
    CHECK(copied.plain.empty());
    CHECK_EQ(grid.textReadCount, grid.columns);
    CHECK_EQ(grid.styleReadCount, 0);
}

TEST(spaceLeadingTextClusterResolvesOnlyItsEmittedStyle)
{
    StyledGrid grid(1, 1024);
    StyledCell& cluster = grid.at(0, 0);
    cluster.codePoint = ' ';
    cluster.codePoints = { ' ', 'j', ' ', 'W' };
    cluster.codePointCount = 4;
    const Ubiquitty::SelectionBounds bounds { 0, 0, 0, grid.columns - 1 };
    const auto copied = buildStyledClipboard(&bounds, Ubiquitty::SelectionMode::Character, grid.columns,
        gridCellAt, &grid, gridTextAt, &grid, gridStyledAt, &grid, SoftWrapNewline::Join, nullptr, nullptr,
        StyleColorMode::Full, font, defaultForeground, defaultBackground);
    CHECK_STR_EQ(copied.plain, " j W");
    CHECK_CONTAINS(copied.html, " j W");
    CHECK_CONTAINS(copied.rtf, " j W");
    CHECK_EQ(grid.textReadCount, 1);
    CHECK_EQ(grid.styleReadCount, 1);
}

// --- Full mode: a colored-bold run keeps fg + bg + bold -------------------

TEST(testFullColoredBold)
{
    StyledGrid g(1, 3);
    // "Hi" in red-on-blue bold, then a default-style space-less char.
    g.at(0, 0) = makeCell('H', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 1) = makeCell('i', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 2) = makeCell('!', 192, 192, 192, 0, 0, 0, false, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 2 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);

    CHECK_STR_EQ(out.plain, "Hi!");

    // RTF: header, font face, color table carries red + blue; the "Hi" run is
    // bold with a fg + a highlight bg.
    CHECK_CONTAINS(out.rtf, "{\\rtf1");
    CHECK_CONTAINS(out.rtf, "TestMono");
    CHECK_CONTAINS(out.rtf, "\\red255\\green0\\blue0;");
    CHECK_CONTAINS(out.rtf, "\\red0\\green0\\blue255;");
    CHECK_CONTAINS(out.rtf, "\\b");
    CHECK_CONTAINS(out.rtf, "\\highlight"); // a non-default bg becomes a highlight

    // HTML: a span with color + background-color + font-weight:bold around "Hi".
    CHECK_CONTAINS(out.html, "<pre");
    CHECK_CONTAINS(out.html, "color:#ff0000;");
    CHECK_CONTAINS(out.html, "background-color:#0000ff;");
    CHECK_CONTAINS(out.html, "font-weight:bold;");
    CHECK_CONTAINS(out.html, ">Hi</span>");
    // The body <pre> paints the default paper in Full mode.
    CHECK_CONTAINS(out.html, "background-color:#000000;");
}

// --- NoBackground mode: fg + attrs survive, bg dropped --------------------

TEST(testNoBackgroundDropsBackground)
{
    StyledGrid g(1, 2);
    g.at(0, 0) = makeCell('X', 0, 255, 0, 255, 0, 0, false, false, true); // green-on-red underline
    g.at(0, 1) = makeCell('Y', 0, 255, 0, 255, 0, 0, false, false, true);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::NoBackground, font, defaultForeground, defaultBackground);

    // Foreground green present, underline present; no background anywhere.
    CHECK_CONTAINS(out.html, "color:#00ff00;");
    CHECK_CONTAINS(out.html, "text-decoration:underline;");
    CHECK_NOT_CONTAINS(out.html, "background-color:#ff0000;");
    CHECK_NOT_CONTAINS(out.html, "background-color"); // not even the <pre> paper

    CHECK_CONTAINS(out.rtf, "\\red0\\green255\\blue0;");
    CHECK_CONTAINS(out.rtf, "\\ul");
    CHECK_NOT_CONTAINS(out.rtf, "\\highlight"); // no bg → no highlight control
    CHECK_NOT_CONTAINS(out.rtf, "\\red255\\green0\\blue0;"); // red bg never enters the table
}

// --- StylesOnly mode: attrs only, both colors dropped ---------------------

TEST(testStylesOnlyDropsAllColor)
{
    StyledGrid g(1, 2);
    g.at(0, 0) = makeCell('B', 255, 0, 255, 0, 255, 0, true, true, false); // magenta/green bold-italic
    g.at(0, 1) = makeCell('I', 255, 0, 255, 0, 255, 0, true, true, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::StylesOnly, font, defaultForeground, defaultBackground);

    // AttributeSet survive, no color of any kind (no #rrggbb, no \red/\highlight).
    CHECK_CONTAINS(out.html, "font-weight:bold;");
    CHECK_CONTAINS(out.html, "font-style:italic;");
    CHECK_NOT_CONTAINS(out.html, "color:#");
    CHECK_NOT_CONTAINS(out.html, "background-color");

    CHECK_CONTAINS(out.rtf, "\\b");
    CHECK_CONTAINS(out.rtf, "\\i");
    CHECK_NOT_CONTAINS(out.rtf, "\\highlight");
    // The magenta/green never enter the color table (only the default fg/bg seed it).
    CHECK_NOT_CONTAINS(out.rtf, "\\red255\\green0\\blue255;");
    CHECK_NOT_CONTAINS(out.rtf, "\\red0\\green255\\blue0;");
}

// --- Inverse: the caller resolves the swap; the generator emits swapped RGB --

TEST(testInverseResolvedColors)
{
    // Inverse is baked by CellPalette::resolve before the generator sees the
    // cell: an inverse white-on-black cell arrives as fg=black bg=white. The
    // generator just emits those resolved colors — proving it never re-derives.
    StyledGrid g(1, 1);
    g.at(0, 0) = makeCell('Z', 0, 0, 0, 255, 255, 255, false, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 0 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);

    CHECK_CONTAINS(out.html, "color:#000000;");
    CHECK_CONTAINS(out.html, "background-color:#ffffff;");
    CHECK_CONTAINS(out.rtf, "\\red255\\green255\\blue255;"); // the swapped (white) bg
}

// --- Default-background runs omit a redundant bg in Full mode -------------

TEST(testDefaultBackgroundOmitted)
{
    StyledGrid g(1, 1);
    // HTML inherits the theme paper; RTF must specify it on the text run.
    g.at(0, 0) = makeCell('p', 255, 255, 0, defaultBackground[0], defaultBackground[1], defaultBackground[2], false, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 0 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);

    CHECK_CONTAINS(out.html, "color:#ffff00;");
    // The run carries no background-color span (only the <pre> paper does).
    CHECK_NOT_CONTAINS(out.html, "<span style=\"color:#ffff00;background-color");
    CHECK_CONTAINS(out.rtf, "\\highlight2");
    CHECK_CONTAINS(out.rtf, "\\cb2");
    CHECK_CONTAINS(out.rtf, "\\expandedcolortbl");
}

TEST(testZWJClusterPreservedInAllFlavors)
{
    StyledGrid g(1, 2);
    StyledCell& lead = g.at(0, 0);
    lead = makeCell(0x1F468, 192, 192, 192, 0, 0, 0, false, false, false);
    lead.codePoints = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
    lead.codePointCount = 7;
    g.at(0, 1) = makeCell(0, 192, 192, 192, 0, 0, 0, false, false, false);
    g.at(0, 1).isContinuation = true;

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";

    CHECK_STR_EQ(out.plain, family);
    CHECK_CONTAINS(out.html, family);
    CHECK_CONTAINS(out.rtf, "\\u8205?");
}

// --- Multi-row join + HTML/RTF escaping -----------------------------------

TEST(testEscapingAndNewlines)
{
    StyledGrid g(2, 3);
    g.at(0, 0) = makeCell('<', 192, 192, 192, 0, 0, 0, false, false, false);
    g.at(0, 1) = makeCell('&', 192, 192, 192, 0, 0, 0, false, false, false);
    g.at(0, 2) = makeCell('{', 192, 192, 192, 0, 0, 0, false, false, false); // RTF metachar
    g.at(1, 0) = makeCell('a', 192, 192, 192, 0, 0, 0, false, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 1, 0 };
    StyledClipboard out = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);

    CHECK_STR_EQ(out.plain, "<&{\na");
    CHECK_CONTAINS(out.html, "&lt;&amp;{"); // < and & escaped; { is literal in HTML
    CHECK_CONTAINS(out.rtf, "<&\\{"); // { escaped in RTF; < and & literal
    CHECK_CONTAINS(out.rtf, "\\line"); // the hard newline between the two rows
}

// --- Ubiquitty::Input::shellEscape ----------------------------------------------------------

TEST(testShellEscape)
{
    // Plain word: still single-quoted (a safe, single shell argument).
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("hello"), "'hello'");
    // Spaces and shell metacharacters: all inert inside single quotes.
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("a b c"), "'a b c'");
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("rm -rf /; echo $HOME && `id`"), "'rm -rf /; echo $HOME && `id`'");
    // A single quote becomes the close/escape/reopen idiom '\''.
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("it's"), "'it'\\''s'");
    // Multiple quotes.
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("'a'"), "''\\''a'\\'''");
    // Empty input is the explicit empty argument.
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape(""), "''");
    // Backslash is literal inside single quotes (no double-escaping).
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("a\\b"), "'a\\b'");
    // Newline survives literally inside the quotes.
    CHECK_STR_EQ(Ubiquitty::Input::shellEscape("a\nb"), "'a\nb'");
}

TEST(reversedCopyPreservesClustersAcrossStyleBoundaries)
{
    using namespace Ubiquitty::Selection;
    StyledRuns walked;
    walked.text = U"Ae\u0301B";
    walked.runs = {
        { {}, 0, 2, false },
        { { .isBold = true }, 2, 1, false },
        { { .isItalic = true }, 3, 1, false },
    };
    reverseCopyLines(walked);
    CHECK(walked.text == U"Be\u0301A");
    CHECK(walked.plain == "Be\xCC\x81"
                          "A");
    REQUIRE(walked.runs.size() == 4);
    CHECK(walked.runs[0].style.isItalic);
    CHECK(walked.runs[2].style.isBold);
    CHECK(walked.runs[2].textOffset == 2);
    CHECK(walked.runs[2].textLength == 1);
}

UBIQUITTY_TEST_MAIN()

// The deferred path: a frontend that promises RTF and HTML rather than building
// them holds the walk's result and serializes a flavor when one is asked for.
// Three properties make that safe, and none is visible from the eager call.
TEST(testWalkThenEmitMatchesTheEagerBuild)
{
    StyledGrid g(2, 3);
    g.at(0, 0) = makeCell('H', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 1) = makeCell('i', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 2) = makeCell('!', 192, 192, 192, 0, 0, 0, false, true, false);
    g.at(1, 0) = makeCell('<', 0, 255, 0, 255, 0, 0, false, false, true);
    g.at(1, 1) = makeCell('&', 0, 255, 0, 255, 0, 0, false, false, true);
    g.at(1, 2) = makeCell(0x1F600, 10, 20, 30, 40, 50, 60, false, false, false);

    // Every mode, because the color mode reaches the framing as well as the runs.
    for (StyleColorMode mode : { StyleColorMode::Full, StyleColorMode::NoBackground, StyleColorMode::StylesOnly }) {
        Ubiquitty::SelectionBounds b { 0, 0, 1, 2 };
        const StyledClipboard eager = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, mode, font, defaultForeground, defaultBackground);
        const Ubiquitty::Selection::StyledRuns walked = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, mode, font, defaultForeground, defaultBackground);
        const StyledClipboard deferred = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::All);

        CHECK_STR_EQ(walked.plain, eager.plain);
        CHECK_STR_EQ(deferred.plain, eager.plain);
        CHECK_STR_EQ(deferred.rtf, eager.rtf);
        CHECK_STR_EQ(deferred.html, eager.html);
    }
}

// A promise is redeemed one flavor at a time, and the one nobody asked for is the
// work being skipped — so an unasked flavor has to come back empty rather than
// half-built, and the asked one identical to what the eager build would have made.
TEST(testEachFlavorEmitsAloneAndTheOtherStaysEmpty)
{
    StyledGrid g(1, 2);
    g.at(0, 0) = makeCell('O', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 1) = makeCell('k', 255, 0, 0, 0, 0, 255, true, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
    const StyledClipboard eager = buildStyledClipboard(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);
    const Ubiquitty::Selection::StyledRuns walked = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, font, defaultForeground, defaultBackground);

    const StyledClipboard rtfOnly = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::RTF);
    CHECK_STR_EQ(rtfOnly.rtf, eager.rtf);
    CHECK(rtfOnly.html.empty());
    CHECK_STR_EQ(rtfOnly.plain, eager.plain);

    const StyledClipboard htmlOnly = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::HTML);
    CHECK_STR_EQ(htmlOnly.html, eager.html);
    CHECK(htmlOnly.rtf.empty());
    CHECK_STR_EQ(htmlOnly.plain, eager.plain);

    const StyledClipboard neither = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::None);
    CHECK(neither.rtf.empty());
    CHECK(neither.html.empty());
    CHECK_STR_EQ(neither.plain, eager.plain);
}

// The walk's result owns everything it names. A promise is redeemed after the
// selection is gone and often after the buffer has moved, and the flavor built
// then has only this value to read — an empty or stale paste is worse than a slow
// copy, which is the whole reason the eager build is worth leaving behind.
TEST(testEmittingOutlivesTheGridAndTheFontItWasWalkedFrom)
{
    Ubiquitty::Selection::StyledRuns walked;
    std::string expectedRTF;
    std::string expectedHTML;
    {
        StyledGrid g(1, 2);
        g.at(0, 0) = makeCell('O', 255, 0, 0, 0, 0, 255, true, false, false);
        g.at(0, 1) = makeCell('k', 255, 0, 0, 0, 0, 255, true, false, false);
        // Long enough to defeat the small-string optimisation: a borrowed face name
        // would dangle into freed heap here rather than into intact stack bytes.
        std::string face = "EphemeralMonospaceFaceWithALongName";
        StyledFont borrowed { face.c_str(), 13.0 };

        Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };
        walked = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::Full, borrowed, defaultForeground, defaultBackground);
        const StyledClipboard immediate = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::All);
        expectedRTF = immediate.rtf;
        expectedHTML = immediate.html;
        // The grid is overwritten and the face name freed before either flavor is
        // asked for again.
        g.at(0, 0) = makeCell('X', 0, 0, 0, 0, 0, 0, false, false, false);
        g.at(0, 1) = makeCell('X', 0, 0, 0, 0, 0, 0, false, false, false);
    }

    const StyledClipboard later = Ubiquitty::Selection::emitStyledFlavors(walked, Ubiquitty::Selection::StyledFlavors::All);
    CHECK_STR_EQ(later.rtf, expectedRTF);
    CHECK_STR_EQ(later.html, expectedHTML);
    CHECK_CONTAINS(later.rtf, "EphemeralMonospaceFaceWithALongName");
    CHECK_CONTAINS(later.html, "EphemeralMonospaceFaceWithALongName");
    CHECK_STR_EQ(walked.plain, "Ok");
}

// Runs merge on style, and a style is compared as bytes — which is only exact
// while a colour the mode discards is left zero rather than stale. Two cells
// differing in nothing but a colour the mode drops have to land in one run: under
// byte compare they do exactly when the discard zeroes, so this pins the
// invariant the comparison rests on rather than the comparison itself.
TEST(testAColourTheModeDropsDoesNotSplitARun)
{
    // Same weight, with both foreground and background differing, so every mode has
    // something to discard.
    StyledGrid g(1, 2);
    g.at(0, 0) = makeCell('A', 255, 0, 0, 0, 0, 255, true, false, false);
    g.at(0, 1) = makeCell('B', 0, 255, 0, 255, 255, 0, true, false, false);

    Ubiquitty::SelectionBounds b { 0, 0, 0, 1 };

    // StylesOnly drops both colours: the two cells differ in nothing that survives.
    const Ubiquitty::Selection::StyledRuns stylesOnly = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::StylesOnly, font, defaultForeground, defaultBackground);
    CHECK(stylesOnly.runs.size() == 1);
    CHECK_STR_EQ(stylesOnly.plain, "AB");

    // NoBackground drops the background alone, and the foregrounds still differ,
    // so this one must stay two — the guard above would pass on a comparison
    // that called everything equal.
    const Ubiquitty::Selection::StyledRuns noBackground = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, g.columns, gridCellAt, &g, gridTextAt, &g, gridStyledAt, &g, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::NoBackground, font, defaultForeground, defaultBackground);
    CHECK(noBackground.runs.size() == 2);

    // Same foreground, differing only in the dropped background: merges again.
    StyledGrid sameForeground(1, 2);
    sameForeground.at(0, 0) = makeCell('A', 255, 0, 0, 0, 0, 255, true, false, false);
    sameForeground.at(0, 1) = makeCell('B', 255, 0, 0, 255, 255, 0, true, false, false);
    const Ubiquitty::Selection::StyledRuns dropped = Ubiquitty::Selection::buildStyledRuns(&b, Ubiquitty::SelectionMode::Character, sameForeground.columns, gridCellAt, &sameForeground, gridTextAt, &sameForeground, gridStyledAt, &sameForeground, SoftWrapNewline::Join, nullptr, nullptr, StyleColorMode::NoBackground, font, defaultForeground, defaultBackground);
    CHECK(dropped.runs.size() == 1);
}
