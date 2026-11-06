// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_rich_text_selection.cpp — selecting inside a rich text block and what
// a frame draws for one (Docs/rich-text.md, "Selection and copy",
// "Rendering" and "Performance"): the pointer-to-character rule through the
// selection's caret resolver, a copy across a block's edge in reading order,
// the highlights drawn on glyph bounds from the cut a copy reads, covered
// cells taking no cell highlight, and a static block costing a frame nothing
// but its quad.

#include "TerminalFixture.h"

#include "Core/Input/PointerLadder.h"
#include "Core/Rendering/RuleGeometry.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Drawing.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::RichText;
using UbiquittyTest::feed;

namespace {

constexpr std::uint16_t narrowAdvance = 400;
constexpr std::uint16_t letterAdvance = 1024;

/// One face, half an em a letter and `i` and `l` narrow, in an 8 by 16
/// point cell — which the bitmap tier's 8 by 16 pixel cell maps one to one.
TypographyFacts syntheticFacts()
{
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.faceCount = 1;
    FaceMetrics& face = facts.faces[0];
    face.ascent = 1900;
    face.descent = 500;
    face.lineHeight = 2400;
    face.averageAdvance = 1000;
    face.advances.fill(letterAdvance);
    face.advances['i' - firstTabledCodePoint] = narrowAdvance;
    face.advances['l' - firstTabledCodePoint] = narrowAdvance;
    face.advances[' ' - firstTabledCodePoint] = letterAdvance / 2;
    return facts;
}

void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    CHECK(adoptTypography(t, syntheticFacts()));
}

/// A point inside the screen, in cells across and rows down.
Ubiquitty::Selection::CaretResolution caretAt(Ubiquitty::Terminal& t, double xInCells, double yInRows)
{
    const int row = static_cast<int>(yInRows);
    return Ubiquitty::Selection::resolveSelectionCaret(t, 0, row, static_cast<int>(xInCells + 0.5), xInCells, yInRows);
}

std::string dragCopy(Ubiquitty::Terminal& t, double fromX, double fromY, double toX, double toY)
{
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, static_cast<int>(fromY), static_cast<int>(fromX), 1, 0, {});
    Ubiquitty::Selection::configureCharacterCaretGesture(selection, t, caretAt(t, fromX, fromY));
    Ubiquitty::Selection::dragCharacterCaretGesture(selection, t, caretAt(t, toX, toY), 0);
    return Ubiquitty::Selection::selectedText(selection, t);
}

/// A letter at 10 points is five points: in cells of eight, 5/8 of a cell.
constexpr double letter = 5.0 / 8.0;

/// The highlights a selection draws in the terminal's one block, captured as
/// a frame captures them: one cell is eight pixels across and sixteen down.
std::vector<HighlightQuad> highlightsOf(Ubiquitty::Selection::Session& selection, Ubiquitty::Terminal& t, BlockQuad& quad)
{
    std::array<BlockSelection, 4> blocks {};
    const int count = Ubiquitty::Selection::blockSelections(selection, t, blocks);
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    options.richTextSelections = blocks;
    options.richTextSelectionCount = count;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    if (!frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}) || frame.richTextQuads().empty())
        return {};
    quad = frame.richTextQuads()[0];
    const auto highlights = frame.richTextHighlights();
    return { highlights.begin(), highlights.end() };
}

} // namespace

TEST(aClickInsideABlockNamesTheCharacterItIsOn)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[3;3H\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20 il\\par second}\x1b\\");
    // `i` is 400/2048 em at ten points: 1.953 points, 0.244 cells. A point on
    // its last part is on `i`; just past it, `l`.
    const double iWidth = narrowAdvance / 2048.0 * 10.0 / 8.0;
    const Ubiquitty::Selection::CaretResolution onI = caretAt(t, 2.0 + iWidth - 0.01, 2.5);
    CHECK_EQ(onI.caret, 2);
    CHECK_EQ(onI.subCaret, 0);
    CHECK_EQ(onI.subCaretAfter, 1);
    const Ubiquitty::Selection::CaretResolution onL = caretAt(t, 2.0 + iWidth + 0.01, 2.5);
    CHECK_EQ(onL.subCaret, 1);
    CHECK_EQ(onL.subCaretAfter, 2);
    // The second line's top is 11.72 points down, inside the block's first
    // row: a point 13 points down is on it.
    const Ubiquitty::Selection::CaretResolution second = caretAt(t, 2.0 + letter * 1.5, 2.0 + 13.0 / 16.0);
    CHECK_EQ(second.subCaret, 4);
    // The caret stands at the block's top-left cell, whatever row it is on.
    CHECK_EQ(second.absoluteRow, onI.absoluteRow);
}

TEST(aDragAcrossABlocksEdgeCopiesInReadingOrder)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "above\r\n");
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa bbbb}\x1b\\");
    feed(t, "\x1b[4;1Hbelow");
    // From the second letter of `above` to the middle of `bbbb`: the rows
    // above the block, then the block's characters up to the pointer.
    const std::string into = dragCopy(t, 1.1, 0.5, 5.5 * letter, 1.5);
    CHECK_MSG(into == "bove\naaaa bb", into.c_str());
    // From inside the block to the row below it: the rest of the block, then
    // the rows below.
    const std::string out = dragCopy(t, 5.6 * letter, 1.5, 3.0, 3.5);
    CHECK_MSG(out == "bbb\n\nbel", out.c_str());
}

TEST(aSourceAnchoredViewportCopiesOnlyVisibleTextAcrossItsEdges)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "above\r\n\x1b]7404;w=20:h=1:g=1:v=7:C=1;{\\rtf1\\fs20 hidden\\par shown}\x1b\\\x1b[3;1Hbelow");
    const std::string into = dragCopy(t, 1.1, 0.5, 1.5 * letter, 1.5);
    CHECK_MSG(into == "bove\nsh", into.c_str());
    const std::string out = dragCopy(t, 1.6 * letter, 1.5, 3.0, 2.5);
    CHECK_MSG(out == "hown\nbel", out.c_str());

    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 1, 1, 0, {});
    Ubiquitty::Selection::configureCharacterCaretGesture(selection, t, caretAt(t, 1.1, 0.5));
    Ubiquitty::Selection::dragCharacterCaretGesture(selection, t, caretAt(t, 3.0, 2.5), 0);
    const std::string copied = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(copied == "bove\nshown\nbel", copied.c_str());
    const Ubiquitty::CellPalette palette = Ubiquitty::CellPalette::fromTerminalPalette(t.palette);
    const auto styled = Ubiquitty::Selection::styledClipboard(selection, t, Ubiquitty::Selection::StyleColorMode::StylesOnly, palette, { nullptr, 0 });
    CHECK(styled.plain == copied);
    CHECK(styled.html.find("hidden") == std::string::npos);
    CHECK(styled.rtf.find("hidden") == std::string::npos);
    std::array<BlockSelection, 1> blocks {};
    REQUIRE(Ubiquitty::Selection::blockSelections(selection, t, blocks) == 1);
    CHECK_EQ(blocks[0].first, 7u);
    CHECK_EQ(blocks[0].end, 12u);
}

// A line break inside a paragraph copies as a line feed, as a paragraph
// break does.
TEST(aLineBreakInsideAParagraphCopiesAsALineFeed)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20 aaaa\\line bbbb\\par cc}\x1b\\");
    const std::string copied = dragCopy(t, 0.05, 0.1, 3.0 * letter, 2.9);
    CHECK_MSG(copied == "aaaa\nbbbb\ncc", copied.c_str());
}

TEST(theHighlightIsOneRectanglePerLineOnGlyphBoundsAndMatchesTheCopy)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=4:h=4:C=1;{\\rtf1\\fs20 aaaa aaaa}\x1b\\");
    // Four cells are 32 points: six letters a line, so `aaaa ` and `aaaa`.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 0, 1, 0, {});
    Ubiquitty::Selection::configureCharacterCaretGesture(selection, t, caretAt(t, 1.5 * letter, 0.2));
    Ubiquitty::Selection::dragCharacterCaretGesture(selection, t, caretAt(t, 2.5 * letter, 0.8), 0);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "aaa aaa");

    std::array<BlockSelection, 4> blocks {};
    const int count = Ubiquitty::Selection::blockSelections(selection, t, blocks);
    CHECK_EQ(count, 1);
    CHECK_EQ(blocks[0].first, 1u);
    CHECK_EQ(blocks[0].end, 8u);

    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    options.richTextSelections = blocks;
    options.richTextSelectionCount = count;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    CHECK(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    CHECK_EQ(frame.richTextQuads().size(), std::size_t { 1 });
    const auto highlights = frame.richTextHighlights();
    CHECK_EQ(highlights.size(), std::size_t { 2 });
    if (highlights.size() == 2) {
        // Five points a letter at a point a pixel: the first line, which the
        // selection goes on past, from `a`'s right edge to the block's right
        // edge, the second over three letters, each a line of 2400/2048 em at
        // ten points tall.
        const float origin = frame.richTextQuads()[0].left;
        CHECK_EQ(highlights[0].left - origin, 5.0f);
        CHECK_EQ(highlights[0].width, 27.0f);
        CHECK_EQ(highlights[0].height, 11.71875f);
        CHECK_EQ(highlights[1].left - origin, 0.0f);
        CHECK_EQ(highlights[1].top - frame.richTextQuads()[0].top, 11.71875f);
        CHECK_EQ(highlights[1].width, 15.0f);
    }
}

TEST(aLineTheSelectionContinuesPastFillsToTheBlocksEdgeAndTheBandsTile)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=10:h=3:C=1;{\\rtf1\\fs20 aaaa bbbb\\par cc}\x1b\\");
    // From the second `a` to the first `c`: the first line from the press to
    // the block's right edge, the second from the block's left edge to the
    // release; the first band reaches down to the second line's top.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 1.5 * letter, 0.3, 0.8 * letter, 0.9);
    BlockQuad quad;
    const std::vector<HighlightQuad> highlights = highlightsOf(selection, t, quad);
    REQUIRE(highlights.size() == 2);
    CHECK_EQ(highlights[0].left - quad.left, 5.0f);
    CHECK_EQ(highlights[0].left + highlights[0].width, quad.left + quad.width);
    CHECK_EQ(highlights[0].top, quad.top);
    CHECK_EQ(highlights[1].left, quad.left);
    CHECK_EQ(highlights[1].width, 5.0f);
    CHECK_EQ(highlights[1].top, highlights[0].top + highlights[0].height);
}

TEST(aSelectionLeavingABlockIsOneTextViewShapeWithTheGridAroundIt)
{
    // The rich_text_selection_across_edge case: a drag from inside the
    // block's first line to the row below it. Each block line the selection
    // continues past fills to the block's right edge, where the grid's fill
    // of the cells right of it begins; the second line fills from the block's
    // left edge, where the cells left of it end; the bands tile with no gap,
    // the first from the block's top and the last to its bottom, which are
    // the selected grid rows' edges.
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "above the block\r\nleft \x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs24 inside the block\\par second line}\x1b\\\x1b[2;27Hright\x1b[4;1Hbelow the block");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 11.0, 1.5, 9.5, 3.5);
    BlockQuad quad;
    const std::vector<HighlightQuad> highlights = highlightsOf(selection, t, quad);
    REQUIRE(highlights.size() == 2);
    CHECK_EQ(quad.top, 16.0f);
    CHECK_EQ(quad.height, 32.0f);
    CHECK(highlights[0].left > quad.left);
    CHECK_EQ(highlights[0].left + highlights[0].width, quad.left + quad.width);
    CHECK_EQ(highlights[0].top, quad.top);
    CHECK_EQ(highlights[1].left, quad.left);
    CHECK_EQ(highlights[1].width, quad.width);
    CHECK_EQ(highlights[1].top, highlights[0].top + highlights[0].height);
    CHECK_EQ(highlights[1].top + highlights[1].height, quad.top + quad.height);
}

TEST(aSelectionEnteringABlockFillsFromItsTopAndLeadingEdge)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "above\r\n\x1b]7404;w=10:h=3:j=c:C=1;{\\rtf1\\fs20 aa\\par bbbb}\x1b\\");
    // From the grid above into the second line of a centred block: the first
    // line, entered from before, fills the block's width from its top; the
    // second fills from the block's left edge to the release.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 1.0, 0.5, 5.0, 1.9);
    BlockQuad quad;
    const std::vector<HighlightQuad> highlights = highlightsOf(selection, t, quad);
    REQUIRE(highlights.size() == 2);
    CHECK_EQ(highlights[0].left, quad.left);
    CHECK_EQ(highlights[0].width, quad.width);
    CHECK_EQ(highlights[0].top, quad.top);
    CHECK_EQ(highlights[1].left, quad.left);
    CHECK_EQ(highlights[1].top, highlights[0].top + highlights[0].height);
}

TEST(aPartlySelectedAlignedLineFillsOnlyFromThePressAndAWholeOneToBothEdges)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=10:h=3:j=r:C=1;{\\rtf1\\fs20 aa\\par bb\\par cc}\x1b\\");
    // A right-aligned block: `aa` sits flush right. From inside the first
    // line to inside the third, the middle line is crossed whole and fills
    // the block's width; the first fills from the press to the right edge.
    BlockQuad quad;
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    const double right = 10.0;
    Ubiquitty::Selection::selectBetweenPoints(selection, t, right - 0.8 * letter, 0.3, right - 1.6 * letter, 1.6);
    const std::vector<HighlightQuad> highlights = highlightsOf(selection, t, quad);
    REQUIRE(highlights.size() == 3);
    CHECK_EQ(highlights[0].left - quad.left, quad.width - 5.0f);
    CHECK_EQ(highlights[0].left + highlights[0].width, quad.left + quad.width);
    CHECK_EQ(highlights[1].left, quad.left);
    CHECK_EQ(highlights[1].width, quad.width);
    CHECK_EQ(highlights[2].left, quad.left);
    CHECK_EQ(highlights[2].left + highlights[2].width, quad.left + quad.width - 5.0f);
}

TEST(aNativelyShapedLineKeepsItsVisualPiecesUntilTheSelectionGoesOnPastIt)
{
    // A shaper lays `abcd` with `cd` a right-to-left run, so `d` stands left
    // of `c`, and `ef` on the next line; every letter is five points.
    Ubiquitty::Terminal t;
    reset(t);
    auto facts = syntheticFacts();
    facts.shaping = 1;
    CHECK(adoptTypography(t, facts));
    t.typography.shaper = { [](void*, const Document* document, const ShapeRequest*, const TypographyFacts*, ShapedText* output) {
                               constexpr std::int32_t letterUnits = 5 * 4096;
                               constexpr std::uint32_t lineUnits = 48000;
                               if (document->text.size() != 7)
                                   return false;
                               output->lines.push_back({ 0, 5, 0, lineUnits, 38000, 0, 4 * letterUnits });
                               output->lines.push_back({ 5, 7, lineUnits, lineUnits, 38000, 0, 2 * letterUnits });
                               output->spans.push_back({ 0, 1, 0, 0, letterUnits });
                               output->spans.push_back({ 1, 2, 0, letterUnits, 2 * letterUnits });
                               output->spans.push_back({ 2, 3, 0, 4 * letterUnits, 3 * letterUnits });
                               output->spans.push_back({ 3, 4, 0, 3 * letterUnits, 2 * letterUnits });
                               output->spans.push_back({ 4, 5, 0, 4 * letterUnits, 4 * letterUnits });
                               output->spans.push_back({ 5, 6, 1, 0, letterUnits });
                               output->spans.push_back({ 6, 7, 1, letterUnits, 2 * letterUnits });
                               return true;
                           },
        nullptr };
    feed(t, "\x1b]7404;w=10:h=3:C=1;{\\rtf1\\fs20 abcd\\par ef}\x1b\\");
    REQUIRE(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
    const Layout& layout = layoutOf(t, t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary)[0]);
    REQUIRE(layout.isShaped());
    BlockQuad quad;
    quad.pixelsPerUnitX = 1.0f / 4096.0f;
    quad.pixelsPerUnitY = 1.0f / 4096.0f;
    const float blockWidth = static_cast<float>(layout.blockWidth()) / 4096.0f;

    // `bc` inside the line: `b` and `c` are apart on screen, so two pieces.
    std::vector<HighlightQuad> inside;
    appendHighlights(layout, { .serial = 1, .first = 1, .end = 3 }, quad, inside);
    REQUIRE(inside.size() == 2);
    CHECK_EQ(inside[0].left, 5.0f);
    CHECK_EQ(inside[0].width, 5.0f);
    CHECK_EQ(inside[1].left, 15.0f);
    CHECK_EQ(inside[1].width, 5.0f);

    // From `b` on to `e`: the first line from the leftmost selected edge to
    // the block's edge, the second from the block's edge to `e`'s, tiled.
    std::vector<HighlightQuad> across;
    appendHighlights(layout, { .serial = 1, .first = 1, .end = 6 }, quad, across);
    REQUIRE(across.size() == 2);
    CHECK_EQ(across[0].left, 5.0f);
    CHECK_EQ(across[0].left + across[0].width, blockWidth);
    CHECK_EQ(across[1].left, 0.0f);
    CHECK_EQ(across[1].width, 5.0f);
    CHECK_EQ(across[1].top, across[0].top + across[0].height);
}

TEST(aBlocksHighlightBehindTheTextIsTheToneOfASelectedCellBesideIt)
{
    // Behind the text a selected cell's background is the selection washed
    // over its own; the block's bands take the same wash over the block's
    // background, opaque, so the two meet in one tone.
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "left \x1b]7404;w=10:h=2:C=1;{\\rtf1\\fs20 aaaa\\par bb}\x1b\\\x1b[3;1Hbelow");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 1.0, 0.5, 3.0, 2.5);
    std::array<BlockSelection, 4> blocks {};
    const int count = Ubiquitty::Selection::blockSelections(selection, t, blocks);
    std::array<Ubiquitty::Rendering::SelectionRowSpan, 16> spans {};
    const int spanCount = Ubiquitty::Selection::viewportSpans(selection, t, 0, spans);
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    options.style.selectionStyle = Ubiquitty::Rendering::SelectionStyle::Native;
    options.style.platformSelectionColor = { 0.7f, 0.84f, 1.0f, 0.5f };
    options.selectionSpans = spans.data();
    options.selectionSpanCount = spanCount;
    options.richTextSelections = blocks;
    options.richTextSelectionCount = count;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    REQUIRE(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.inputs().isSelectionBehindText);
    const auto highlights = frame.richTextHighlights();
    REQUIRE(!highlights.empty());
    const auto washed = [&](std::uint8_t ground, float selected) {
        const float base = static_cast<float>(ground) / 255.0f;
        return base + (selected - base) * frame.inputs().selectionColor.w;
    };
    const Ubiquitty::Rendering::Vec4 color = frame.inputs().selectionColor;
    for (const HighlightQuad& highlight : highlights) {
        CHECK_EQ(highlight.alpha, 1.0f);
        CHECK(std::fabs(highlight.red - washed(Ubiquitty::defaultBackgroundRGB[0], color.x)) < 1.0e-5f);
        CHECK(std::fabs(highlight.blue - washed(Ubiquitty::defaultBackgroundRGB[2], color.z)) < 1.0e-5f);
    }
}

TEST(aRunsRulesArePlacedByTheGridsRuleOnWholePixels)
{
    // A twelve-point run at a pixel a point: the face's 100/2048 em underline
    // weight is under a pixel and takes one row; its drop, from the descent
    // as AppKit places it, puts the rule a pixel below the baseline. The
    // strike rides half the x-height above it. Every edge is a whole pixel,
    // as the cell builder's are.
    constexpr float em = 12.0f;
    const DecorationFace face {
        .em = em,
        .underlineThickness = 100.0f / 2048.0f * em,
        .ascent = 1900.0f / 2048.0f * em,
        .descent = 500.0f / 2048.0f * em,
        .leading = 0.0f,
        .xHeight = 1000.0f / 2048.0f * em,
    };
    BlockQuad quad;
    const DecorationRows rows = decorationRows(face, 10.0f, quad);
    CHECK_EQ(rows.underlineTop, 11.0f);
    CHECK_EQ(rows.underlineBottom, 12.0f);
    CHECK_EQ(rows.strikeTop, 7.0f);
    CHECK_EQ(rows.strikeBottom, 8.0f);
    // The frame's multiplier scales the floored weight, as it does the grid
    // face's, and its offset moves the rule line.
    quad.underlineThicknessScale = 3.0f;
    quad.underlineOffset = 2.0f;
    const DecorationRows heavy = decorationRows(face, 10.0f, quad);
    CHECK_EQ(heavy.underlineBottom - heavy.underlineTop, 3.0f);
    CHECK(heavy.underlineTop >= rows.underlineTop + 2.0f);
    // A face that reports an x-height under a pixel takes half its ascent
    // instead: eighteen pixels of ascent put the strike's centre 4.5 pixels
    // above the baseline.
    DecorationFace bare = face;
    bare.ascent = 18.0f;
    bare.xHeight = 0.5f;
    CHECK_EQ(decorationRows(bare, 10.0f, BlockQuad {}).strikeTop, 5.0f);
}

TEST(aBlocksQuadCarriesTheFramesRuleSettingsAndTheyMoveItsKey)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20\\ul body}\x1b\\");
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    REQUIRE(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.richTextQuads().size() == 1);
    const std::uint64_t plain = frame.richTextQuads()[0].contentKey;
    options.style.underlineThicknessScale = 2.0f;
    options.style.underlineOffsetInPixels = 1.0f;
    options.style.shouldUnderlineSkipInk = true;
    options.style.underlineSkipInkPaddingInPixels = 3.0f;
    REQUIRE(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.richTextQuads().size() == 1);
    const BlockQuad& quad = frame.richTextQuads()[0];
    CHECK_EQ(quad.underlineThicknessScale, 2.0f);
    CHECK_EQ(quad.underlineOffset, 1.0f);
    CHECK_EQ(quad.isSkippingInk, std::uint8_t { 1 });
    CHECK_EQ(quad.skipInkPadding, 3.0f);
    CHECK_EQ(quad.deviceScale, rasterizer.cellMetrics().deviceScale);
    // The rules are drawn into the raster, so the frontend draws it again.
    CHECK(quad.contentKey != plain);
}

TEST(skipInkCutsAnUnderlineOnlyWhereADescenderCrossesIt)
{
    // A stem sitting on the baseline (ink through row 19, its antialiased
    // foot at row 20) and a descender through the rule (rows 0 to 30), over
    // an underline on rows 23 and 24 with the grid's three-pixel padding.
    constexpr int width = 60;
    constexpr int height = 32;
    std::vector<std::uint8_t> ink(width * height, 0);
    const auto paint = [&](int left, int right, int top, int bottom, std::uint8_t value) {
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
                ink[static_cast<std::size_t>(y * width + x)] = value;
    };
    paint(10, 12, 0, 19, 255);
    paint(10, 12, 20, 20, 96);
    paint(40, 42, 0, 30, 255);
    const PixelRect rule { .left = 0, .top = 23, .width = width, .height = 2 };
    std::vector<std::uint8_t> alpha(width * 2, 0);
    skipInkRuleAlpha(ink, width, height, rule, rule.top, 3.0f, alpha);
    const auto keep = [&](int x, int row) { return alpha[static_cast<std::size_t>(row * width + x)]; };
    // Half the padding upward does not reach the stem's foot: the rule runs
    // whole under the letter, as the grid's does.
    for (int x = 0; x < 30; ++x) {
        CHECK_MSG(keep(x, 0) == 255, ("cut under the stem at x=" + std::to_string(x)).c_str());
        CHECK_MSG(keep(x, 1) == 255, ("cut under the stem at x=" + std::to_string(x)).c_str());
    }
    // The descender opens a gap its own width and the padding each side.
    for (int x = 38; x <= 44; ++x)
        CHECK_MSG(keep(x, 0) < 64, ("no cut beside the descender at x=" + std::to_string(x)).c_str());
    CHECK_EQ(keep(35, 0), 255);
    CHECK_EQ(keep(47, 0), 255);
    // The reach is the grid's: the full padding across, half of it up and
    // down, one tap a pixel of padding.
    const Ubiquitty::Rendering::SkipInkReach reach = Ubiquitty::Rendering::skipInkReach(3.0f);
    CHECK_EQ(reach.horizontal, 3.0f);
    CHECK_EQ(reach.vertical, 1.5f);
    CHECK_EQ(reach.steps, 3);
    // A rule a single row under the foot of a round letter: the foot's
    // overshoot comes within the reach upward but never into the rule.
    std::vector<std::uint8_t> round(width * height, 0);
    for (int x = 10; x <= 20; ++x) {
        round[static_cast<std::size_t>(21 * width + x)] = 255;
        round[static_cast<std::size_t>(22 * width + x)] = 120;
    }
    std::vector<std::uint8_t> under(width * 2, 0);
    skipInkRuleAlpha(round, width, height, PixelRect { .left = 0, .top = 23, .width = width, .height = 2 }, 23, 3.0f, under);
    for (int x = 0; x < width; ++x)
        CHECK_MSG(under[static_cast<std::size_t>(x)] == 255, ("an overshoot cut the rule at x=" + std::to_string(x)).c_str());
    // Coverage that is not the size it claims leaves the rule whole.
    std::vector<std::uint8_t> whole(width * 2, 0);
    skipInkRuleAlpha(std::span(ink).first(width), width, height, rule, rule.top, 3.0f, whole);
    CHECK(std::ranges::all_of(whole, [](std::uint8_t value) { return value == 255; }));
}

TEST(aRuleMaskCannotOutgrowItsRasterOrNarrowAnOffscreenCoordinate)
{
    const DecorationFace face { .em = 12, .underlineThickness = 1, .ascent = 12, .descent = 3, .xHeight = 6 };
    BlockQuad quad;
    quad.underlineThicknessScale = 1e9f;
    const auto rows = decorationRows(face, 10, quad);
    // The accepted thickness asks for a billion-row rule. Only five rows
    // fit this block; the Swift mask allocation uses this returned area.
    const PixelRect thick = clippedRuleRect(2, rows.underlineTop, 18, rows.underlineBottom, 20, 16);
    CHECK_EQ(thick.left, 2);
    CHECK_EQ(thick.top, 11);
    CHECK_EQ(thick.width, 16);
    CHECK_EQ(thick.height, 5);
    CHECK_EQ(thick.width * thick.height, 80);

    // This offset remains finite and preserves the stroke's Float height,
    // but narrowing its top directly to the native Int32 coordinate traps.
    quad.underlineThicknessScale = 1024;
    quad.underlineOffset = 3e9f;
    const auto away = decorationRows(face, 10, quad);
    CHECK(away.underlineTop > static_cast<double>(std::numeric_limits<int>::max()));
    CHECK(away.underlineBottom > away.underlineTop);
    const PixelRect absent = clippedRuleRect(2, away.underlineTop, 18, away.underlineBottom, 20, 16);
    CHECK_EQ(absent.width, 0);
    CHECK_EQ(absent.height, 0);

    const double huge = std::numeric_limits<double>::max();
    const PixelRect covering = clippedRuleRect(-huge, -huge, huge, huge, 20, 16);
    CHECK_EQ(covering.left, 0);
    CHECK_EQ(covering.top, 0);
    CHECK_EQ(covering.width, 20);
    CHECK_EQ(covering.height, 16);
    const PixelRect unchanged = clippedRuleRect(2, 4, 18, 6, 20, 16);
    CHECK_EQ(unchanged.left, 2);
    CHECK_EQ(unchanged.top, 4);
    CHECK_EQ(unchanged.width, 16);
    CHECK_EQ(unchanged.height, 2);
    for (const double invalid : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        CHECK_EQ(clippedRuleRect(invalid, 0, 20, 16, 20, 16).width, 0);
        CHECK_EQ(clippedRuleRect(0, invalid, 20, 16, 20, 16).width, 0);
        CHECK_EQ(clippedRuleRect(0, 0, invalid, 16, 20, 16).width, 0);
        CHECK_EQ(clippedRuleRect(0, 0, 20, invalid, 20, 16).width, 0);
    }
    CHECK_EQ(clippedRuleRect(4, 2, 2, 4, 20, 16).width, 0);
    CHECK_EQ(clippedRuleRect(2, 4, 4, 2, 20, 16).width, 0);
    CHECK_EQ(clippedRuleRect(0, 0, 20, 16, 0, 16).width, 0);
}

TEST(clippingASkipInkMaskPreservesTheVisiblePixels)
{
    constexpr int width = 8;
    constexpr int height = 6;
    std::array<std::uint8_t, width * height> ink {};
    for (int row = 0; row < height; ++row)
        ink[static_cast<std::size_t>(row * width + 4)] = 255;
    ink[1] = 120;
    ink[2] = 200;
    const PixelRect whole { .left = -3, .top = -2, .width = 12, .height = 10 };
    std::array<std::uint8_t, 120> fullAlpha {};
    skipInkRuleAlpha(ink, width, height, whole, whole.top, 1.5f, fullAlpha);
    const PixelRect clipped = clippedRuleRect(whole.left, whole.top, whole.left + whole.width, whole.top + whole.height, width, height);
    REQUIRE(clipped.width == width && clipped.height == height);
    std::array<std::uint8_t, width * height> visibleAlpha {};
    skipInkRuleAlpha(ink, width, height, clipped, whole.top, 1.5f, visibleAlpha);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const auto fullIndex = static_cast<std::size_t>((row - whole.top) * whole.width + column - whole.left);
            const auto visibleIndex = static_cast<std::size_t>(row * width + column);
            CHECK_EQ(visibleAlpha[visibleIndex], fullAlpha[fullIndex]);
        }
    }
}

TEST(nonpositiveSkipInkPaddingIsZeroForBothGridAndRichText)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=3:C=1;{\\rtf1\\ul body}\x1b\\");
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    options.style.shouldUnderlineSkipInk = true;
    options.style.underlineSkipInkPaddingInPixels = 0;
    REQUIRE(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.richTextQuads().size() == 1);
    const auto zeroKey = frame.richTextQuads()[0].contentKey;
    for (const float padding : { -4.0f, -std::numeric_limits<float>::max(), std::numeric_limits<float>::quiet_NaN() }) {
        options.style.underlineSkipInkPaddingInPixels = padding;
        REQUIRE(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
        REQUIRE(frame.richTextQuads().size() == 1);
        CHECK_EQ(frame.metrics().skipInkPadding, 0.0f);
        CHECK_EQ(frame.richTextQuads()[0].skipInkPadding, 0.0f);
        CHECK_EQ(frame.richTextQuads()[0].contentKey, zeroKey);
    }
}

TEST(skipInkRejectsUnrepresentableTapsBeforeIntegerConversion)
{
    constexpr int width = 8;
    constexpr int height = 3;
    std::array<std::uint8_t, width * height> ink {};
    ink[width + 4] = 255;
    const PixelRect rule { .left = 0, .top = 1, .width = width, .height = 1 };
    std::array<std::uint8_t, width> zero {};
    skipInkRuleAlpha(ink, width, height, rule, rule.top, 0, zero);
    CHECK_EQ(zero[4], 0);
    CHECK_EQ(zero[0], 255);
    // Nonfinite reaches normalize to zero; enormous finite taps fall
    // outside the coverage and must not reach a float-to-int conversion.
    for (const float padding : { -4.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max() }) {
        std::array<std::uint8_t, width> alpha {};
        skipInkRuleAlpha(ink, width, height, rule, rule.top, padding, alpha);
        CHECK(alpha == zero);
    }
}

TEST(coveredCellsTakeNoCellHighlight)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "ab \x1b]7404;w=4:h=1:C=1;{\\rtf1 x}\x1b\\\x1b[1;8Hcd");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 0, 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 0, 9, 0);
    std::array<Ubiquitty::Rendering::SelectionRowSpan, 8> spans {};
    const int count = Ubiquitty::Selection::viewportSpans(selection, t, 0, spans);
    CHECK_EQ(count, 2);
    CHECK_EQ(spans[0].column0, 0);
    CHECK_EQ(spans[0].column1, 2);
    CHECK_EQ(spans[1].column0, 7);
    CHECK(spans[1].column1 >= 8);
}

TEST(aDragLeavingABlockHighlightsTheGridAfterItInReadingOrder)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "above\r\nleft \x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa bbbb\\par cccc}\x1b\\\x1b[2;27Hright\x1b[4;1Hbelow the block");
    // From inside the block's first line to the tenth column of the row
    // below it: the block's text from the press, the cells right of it on
    // its first row, then every cell outside it on the rows below, in order.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 5.0 + 5.6 * letter, 1.5, 9.5, 3.5);
    const std::string copied = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(copied.starts_with("bbb\ncccc") && copied.ends_with("below the") && copied.find("right") != std::string::npos, copied.c_str());
    std::array<Ubiquitty::Rendering::SelectionRowSpan, 16> spans {};
    const int count = Ubiquitty::Selection::viewportSpans(selection, t, 0, spans);
    // Each selected row's cells outside the block are highlighted: the
    // first row right of the block, the second row on both sides of it, the
    // row below up to the release.
    auto covers = [&](int row, int first, int last) {
        for (int index = 0; index < count; ++index) {
            const auto& span = spans[static_cast<std::size_t>(index)];
            if (span.visibleRow == row && span.column0 <= first && span.column1 >= last)
                return true;
        }
        return false;
    };
    CHECK(covers(1, 25, 30));
    CHECK(covers(2, 0, 4));
    CHECK(covers(2, 25, 30));
    CHECK(covers(3, 0, 8));
    for (int index = 0; index < count; ++index) {
        const auto& span = spans[static_cast<std::size_t>(index)];
        const bool isOnBlockRows = span.visibleRow == 1 || span.visibleRow == 2;
        CHECK(!isOnBlockRows || span.column1 < 5 || span.column0 > 24);
    }
}

TEST(aBlockInTheScrollbackIsCapturedWhereTheViewShowsIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 30);
    CHECK(adoptTypography(t, syntheticFacts()));
    // One parse pass, as a program's output arrives: the scroll the lines
    // make is batched, and the block must ride it into the scrollback.
    std::string output = "\x1b]7404;w=28:h=2;{\\rtf1\\fs20 history}\x1b\\";
    for (int line = 1; line <= 9; ++line)
        output += "line " + std::to_string(line) + (line < 9 ? "\r\n" : "");
    feed(t, output);
    // Nine lines from the third row push five rows into the scrollback.
    REQUIRE(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
    CHECK_EQ(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary)[0].row, -5);
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    const Ubiquitty::Rendering::CellMetrics metrics = rasterizer.cellMetrics();
    // Scrolled back five rows, the block is the view's first two.
    options.viewOffset = 5;
    CHECK(frame.capture(t, options, metrics, Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.richTextQuads().size() == 1);
    const float top = frame.richTextQuads()[0].top;
    options.viewOffset = 0;
    CHECK(frame.capture(t, options, metrics, Ubiquitty::Rendering::GlyphSlotGeometry {}));
    CHECK(frame.richTextQuads().empty());
    // Three rows back, both of its rows are still above the view.
    options.viewOffset = 3;
    CHECK(frame.capture(t, options, metrics, Ubiquitty::Rendering::GlyphSlotGeometry {}));
    CHECK(frame.richTextQuads().empty());
    CHECK(top < static_cast<float>(metrics.cellHeight));
}

TEST(aViewOffsetPastTheHistoryPlacesBlocksWhereItPlacesTheCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 30);
    CHECK(adoptTypography(t, syntheticFacts()));
    feed(t, "\x1b]7404;w=28:h=2;{\\rtf1\\fs20 on screen}\x1b\\line 1");
    // No row has scrolled off, so an offset of two shows the live screen,
    // the block on its own rows.
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    options.viewOffset = 2;
    CHECK(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(frame.richTextQuads().size() == 1);
    options.viewOffset = 0;
    Ubiquitty::Rendering::TerminalFrameInputs live;
    CHECK(live.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    REQUIRE(live.richTextQuads().size() == 1);
    CHECK_EQ(frame.richTextQuads()[0].top, live.richTextQuads()[0].top);
}

TEST(aStaticBlockIsLaidOutAndDrawnOnceAcrossFrames)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20\\b Title\\par\\b0 body text}\x1b\\");
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
    std::uint64_t key = 0;
    for (int pass = 0; pass < 60; ++pass) {
        CHECK(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
        CHECK_EQ(frame.richTextQuads().size(), std::size_t { 1 });
        if (pass == 0)
            key = frame.richTextQuads()[0].contentKey;
        CHECK_EQ(frame.richTextQuads()[0].contentKey, key);
    }
    CHECK_EQ(t.richTextLayouts.buildCount(), std::uint64_t { 1 });
    CHECK_EQ(t.richTextLayouts.drawingCount(), std::uint64_t { 1 });
    // The drawing places each grapheme at the layout's edge.
    const BlockDrawing& drawing = frame.richTextDrawing(0);
    CHECK_EQ(drawing.clusters().size(), std::size_t { 5 + 9 });
    CHECK_EQ(drawing.clusters()[1].x, 1024u * 20);
    CHECK_EQ(drawing.clusters()[5].first, 6u);
    CHECK_EQ(drawing.clusters()[5].x, 0u);
    CHECK_EQ(drawing.clusters()[5].baseline, 2400u * 20 + 1900u * 20);
    // A theme change moves the key, which is what makes the frontend draw
    // the raster again; the drawing stays.
    feed(t, "\x1b]10;#ff0000\x1b\\");
    CHECK(frame.capture(t, options, rasterizer.cellMetrics(), Ubiquitty::Rendering::GlyphSlotGeometry {}));
    CHECK(frame.richTextQuads()[0].contentKey != key);
    CHECK_EQ(t.richTextLayouts.drawingCount(), std::uint64_t { 1 });
}

TEST(aSelectionBetweenTwoPointsIsTheDragBetweenThem)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa bbbb}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 1.5 * letter, 0.5, 6.2 * letter, 0.5);
    const std::string copied = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(copied == "aaa bb", copied.c_str());
}

TEST(aDoubleClickInsideABlockSelectsTheWordUnderThePointer)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa bbbb cc}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 4, 2, 0, {}, 6.2 * letter, 0.5);
    Ubiquitty::Selection::endGesture(selection);
    const std::string word = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(word == "bbbb", word.c_str());
}

TEST(aWordDragInsideABlockKeepsItsCharacterBoundsThroughReversals)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa beta gamma dddd}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Input::PointerLadder ladder;
    const auto route = [&](Ubiquitty::Input::PointerFact::Kind kind, double x) {
        Ubiquitty::Input::PointerFact fact;
        fact.kind = kind;
        fact.button = Ubiquitty::Input::PointerButton::Left;
        fact.column = static_cast<int>(x);
        fact.xInCells = x;
        fact.yInRows = 0.5;
        fact.clickCount = 2;
        fact.isDragging = kind == Ubiquitty::Input::PointerFact::Kind::Motion;
        return Ubiquitty::Input::routePointer(ladder, t, &selection, fact, {}, 0);
    };
    using Kind = Ubiquitty::Input::PointerFact::Kind;
    CHECK(route(Kind::Press, 6.2 * letter).didOpenSelectionGesture);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "beta");
    for (const auto& [x, expected] : std::array {
             std::pair { 6.3 * letter, "beta" },
             std::pair { 11.2 * letter, "beta gamma" },
             std::pair { 1.2 * letter, "aaaa beta" },
             std::pair { 11.2 * letter, "beta gamma" },
         }) {
        CHECK(route(Kind::Motion, x).didChangeSelection);
        const std::string selected = Ubiquitty::Selection::selectedText(selection, t);
        CHECK_MSG(selected == expected, selected.c_str());
    }
    CHECK(route(Kind::Release, 11.2 * letter).didFinishSelectionGesture);
    CHECK(!route(Kind::Motion, 1.2 * letter).didChangeSelection);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "beta gamma");
}

TEST(aWordDragBetweenRichTextLinesUsesThePointersFractionalRow)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa beta\\par gamma dddd}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Input::PointerLadder ladder;
    const auto route = [&](Ubiquitty::Input::PointerFact::Kind kind, double x, double y) {
        Ubiquitty::Input::PointerFact fact;
        fact.kind = kind;
        fact.button = Ubiquitty::Input::PointerButton::Left;
        fact.row = static_cast<int>(y);
        fact.column = static_cast<int>(x);
        fact.xInCells = x;
        fact.yInRows = y;
        fact.clickCount = 2;
        fact.isDragging = kind == Ubiquitty::Input::PointerFact::Kind::Motion;
        return Ubiquitty::Input::routePointer(ladder, t, &selection, fact, {}, 0);
    };
    using Kind = Ubiquitty::Input::PointerFact::Kind;
    CHECK(route(Kind::Press, 6.2 * letter, 0.2).didOpenSelectionGesture);
    CHECK(route(Kind::Motion, 1.2 * letter, 0.8).didChangeSelection);
    const std::string selected = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(selected == "beta\ngamma", selected.c_str());
    CHECK(route(Kind::Motion, 6.2 * letter, 0.2).didChangeSelection);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "beta");
    CHECK(route(Kind::Release, 6.2 * letter, 0.2).didFinishSelectionGesture);
}

TEST(aWordDragLeavingABlockKeepsItsAnchorAndSelectsThePlainWord)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2:C=1;{\\rtf1\\fs20 aaaa beta gamma dddd}\x1b\\\x1b[3;1Houtside");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    CHECK(Ubiquitty::Selection::beginGesture(selection, t, 0, 3, 2, 0, {}, 6.2 * letter, 0.5));
    CHECK(Ubiquitty::Selection::dragGesture(selection, t, 2, 2, 0, 2.2, 2.5));
    const std::string selected = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(selected == "beta gamma dddd\n\noutside", selected.c_str());
    CHECK(Ubiquitty::Selection::dragGesture(selection, t, 0, 3, 0, 6.2 * letter, 0.5));
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "beta");
}

TEST(aWordPressedBelowABlocksAnchorKeepsItsBoundsWithoutViewportMovement)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[4;1H\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20 aaaa\\par beta\\par gamma dddd}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    CHECK(Ubiquitty::Selection::beginGesture(selection, t, 4, 0, 2, 0, {}, 1.2 * letter, 4.6));
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "gamma");
    feed(t, "\x1b[12;1Hunchanged viewport");
    CHECK(!Ubiquitty::Selection::updateAfterOutput(selection, t, 0).didChangeSelection);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "gamma");
}

TEST(aReverseRichTextWordDragResolvesTheWordUnderAStationaryPointerAfterOutput)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[4;1H\x1b]7404;w=20:h=3:C=1;{\\rtf1\\fs20 aaaa\\par beta\\par gamma dddd}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    CHECK(Ubiquitty::Selection::beginGesture(selection, t, 4, 0, 2, 0, {}, 1.2 * letter, 4.6));
    CHECK(Ubiquitty::Selection::dragGesture(selection, t, 3, 0, 0, 1.2 * letter, 3.2));
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "aaaa\nbeta\ngamma");
    CHECK(!Ubiquitty::Selection::updateAfterOutput(selection, t, 0).didChangeSelection);
    feed(t, "\x1b[12;1H\r\n");
    CHECK(Ubiquitty::Selection::updateAfterOutput(selection, t, 0).didChangeSelection);
    const std::string selected = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(selected == "beta\ngamma", selected.c_str());
}

TEST(sourceAnchorsFollowTextAcrossDifferentLocalWrapping)
{
    std::array<std::size_t, 2> firstLines {};
    for (std::size_t variant = 0; variant < firstLines.size(); ++variant) {
        Ubiquitty::Terminal terminal;
        reset(terminal);
        auto facts = syntheticFacts();
        facts.cellWidth = variant ? 12 * 64 : 8 * 64;
        facts.baseline = 12 * 64;
        facts.proportionalSize = 12 * 64;
        CHECK(adoptTypography(terminal, facts));
        feed(terminal, "\x1b]7404;w=6:h=2:g=1:v=7:C=1;{\\rtf1 aaaa aaaa aaaa aaaa aaaa}\x1b\\");
        const auto blocks = terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
        CHECK_EQ(blocks.size(), std::size_t { 1 });
        if (blocks.empty())
            continue;
        const auto& layout = layoutOf(terminal, blocks.front());
        firstLines[variant] = layout.firstVisibleLine();
        const auto first = layout.line(layout.firstVisibleLine());
        CHECK(first.first <= 7 && first.end > 7);
        CHECK_EQ(first.top - layout.viewportTop(), std::uint32_t { 0 });
        CHECK(layout.visibleLineCount() - layout.firstVisibleLine() <= 2);
        CHECK_EQ(layout.characterAt(0, 0), first.first);
        const auto drawing = BlockDrawing::draw(blocks.front(), layout, &facts);
        CHECK(!drawing.clusters().empty());
        CHECK_EQ(drawing.clusters().front().first, first.first);
    }
    CHECK(firstLines[0] != firstLines[1]);
}

TEST(nativeSourceAnchorsRetainContextAndSurviveGeometryReplication)
{
    struct FontModel {
        std::uint32_t charactersPerLine = 3;
        std::size_t largestSource = 0;
    };
    const auto shape = [](void* context, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
        auto& model = *static_cast<FontModel*>(context);
        model.largestSource = std::max(model.largestSource, document->text.size());
        const auto count = static_cast<std::uint32_t>(document->text.size());
        const auto height = static_cast<std::uint32_t>(facts->cellHeight) * 64;
        for (std::uint32_t first = 0, row = 0; first < count; first += model.charactersPerLine, ++row) {
            const auto end = std::min(count, first + model.charactersPerLine);
            output->appendLine({ first, end, row * height, height, height * 3 / 4, 0, (end - first) * 4096 });
            for (auto index = first; index < end; ++index)
                output->appendSpan({ index, index + 1, row, static_cast<std::int32_t>((index - first) * 4096), static_cast<std::int32_t>((index - first + 1) * 4096) });
        }
        return true;
    };
    for (const auto charactersPerLine : { 3U, 5U }) {
        for (const auto overflow : { "w", "e" }) {
            for (const auto anchor : { 3U, 7U, 16U }) {
                Ubiquitty::Terminal terminal;
                reset(terminal);
                auto facts = syntheticFacts();
                facts.shaping = 1;
                facts.rendersBlocks = 1;
                facts.baseline = 12 * 64;
                CHECK(adoptTypography(terminal, facts));
                FontModel model { .charactersPerLine = charactersPerLine };
                terminal.typography.shaper = { shape, &model };
                feed(terminal, "\x1b]7404;w=6:h=2:g=1:C=1:p=1,0,2,0:v=" + std::to_string(anchor) + ":o=" + overflow + ";{\\rtf1 abcdefghijklmnop}\x1b\\");
                const auto blocks = terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
                CHECK_EQ(blocks.size(), std::size_t { 1 });
                if (blocks.empty())
                    continue;
                const auto& block = blocks.front();
                const auto& local = layoutOf(terminal, block);
                CHECK(local.isShaped());
                CHECK_EQ(model.largestSource, std::size_t { 16 });
                CHECK(local.firstVisibleLine() < local.visibleLineCount());
                const auto first = local.line(local.firstVisibleLine());
                CHECK(first.first <= anchor && (first.end > anchor || anchor == 16 || first.end == local.copyableEnd()));
                CHECK_EQ(first.top - local.viewportTop(), std::uint32_t { 4096 });
                const auto measurement = measure(block.document,
                    { block.width, block.height, block.overflow, block.alignment, block.lineMode, block.padding, block.viewportTop }, terminal.typography);
                LayoutCache replica;
                CHECK(replica.adopt(block, terminal.typography, measurement.geometry));
                const auto& remote = replica.layoutOf(block, terminal.typography);
                CHECK(remote.isShaped());
                CHECK_EQ(remote.firstVisibleLine(), local.firstVisibleLine());
                CHECK_EQ(remote.copyableEnd(), local.copyableEnd());
                CHECK_EQ(remote.viewportTop(), local.viewportTop());
                terminal.typography.shaper = {};
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
