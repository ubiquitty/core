// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_visible_link.cpp — the link under a visible cell, over a real grid.
//
// test_linkrun covers the span computation against synthetic probes. This
// covers what a frontend actually asks: given a terminal and a hovered cell,
// what link is there. The cases that separate the two are the ones a
// single-row scan gets wrong — a URL that soft-wraps, an OSC 8 link whose
// continuation row is not marked wrapped, and a program that re-emits each
// visual segment of one link under a fresh id.

#include "Harness.h"

#include "Core/CellPen.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Screen/Addressing.h"
#include <cstdint>
#include <memory>

#include "Core/Screen/VisibleLink.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace Ubiquitty;

constexpr int scanCells = 2048;

// Scratch big enough for every grid these cases build. A frontend sizes this
// for its own limits; the tests use one size so a failure is never about it.
struct Scratch {
    uint32_t codePoints[scanCells];
    uint16_t linkIDs[scanCells];

    Screen::LinkScanBuffer buffer() { return { codePoints, linkIDs, scanCells }; }
};

std::unique_ptr<Terminal> makeTerminal(int rows = 6, int columns = 20)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

void feed(Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

// The URI under (row, column), or "" when there is none.
std::string uriAt(Terminal& t, int row, int column, Screen::VisibleLink* out = nullptr,
    Screen::LinkScanScope scope = Screen::LinkScanScope::Any)
{
    Scratch scratch;
    char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
    Screen::VisibleLink link {};
    if (!Screen::linkAtVisibleCell(&t, 0, row, column, scratch.buffer(), uri, &link, scope))
        return {};
    if (out)
        *out = link;
    return { uri };
}

// ---- nothing there ---------------------------------------------------------

TEST(testPlainTextIsNotALink)
{
    auto t = makeTerminal();
    feed(*t, "just some words");
    CHECK(uriAt(*t, 0, 5).empty());
    t.reset();
}

TEST(testAnEmptyCellIsNotALink)
{
    auto t = makeTerminal();
    CHECK_MSG(uriAt(*t, 3, 10).empty(), "a blank grid has nothing under the pointer");
    t.reset();
}

TEST(testOutOfRangeIsNotALink)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "https://example.com");
    CHECK_MSG(uriAt(*t, -1, 0).empty(), "a row above the grid");
    CHECK_MSG(uriAt(*t, 6, 0).empty(), "a row below the grid");
    CHECK_MSG(uriAt(*t, 0, -1).empty(), "a column left of the grid");
    CHECK_MSG(uriAt(*t, 0, 20).empty(), "a column right of the grid");
    t.reset();
}

TEST(aSizedBlocksLowerRowJoinsNoLinkItSitsBeside)
{
    // The link scan fills a column-indexed array of code points so a hover
    // maps to a position, and a sized block's lower rows carry the glyph for
    // the renderer to crop. Read as text they extend whatever sits beside
    // them: a URL ending where the block begins picks the block's glyph up as
    // one more character of itself.
    auto t = makeTerminal(6, 40);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    // A two-row block whose lower band lands on row 1 immediately after the
    // URL, which ends at column 18.
    feed(*t, "\033[1;20H\033]66;s=2;A\007");
    feed(*t, "\033[2;1Hhttp://example.com/");

    // The fixture only proves anything if the block is really there.
    const Ubiquitty::Cell* band = Ubiquitty::Screen::absoluteCell(t.get(), t->scrollbackCount + 1, 19);
    CHECK_MSG(band && Ubiquitty::cellPen(*band).extendedAttributes.textSizeSpecID() != 0,
        "the fixture placed no block, so the case proves nothing");

    const std::string uri = uriAt(*t, 1, 3);
    CHECK_MSG(uri == "http://example.com/", uri.c_str());
}

TEST(testANullTerminalIsNotALink)
{
    Scratch scratch;
    char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
    CHECK(!Screen::linkAtVisibleCell(nullptr, 0, 0, 0, scratch.buffer(), uri, nullptr));
}

// ---- the scratch is the caller's, so its limits are the caller's -----------

TEST(testScratchTooSmallForOneRowFindsNothing)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "https://example.com");
    uint32_t codePoints[8];
    uint16_t linkIDs[8];
    Screen::LinkScanBuffer tiny { codePoints, linkIDs, 8 };
    char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
    CHECK_MSG(!Screen::linkAtVisibleCell(t.get(), 0, 0, 3, tiny, uri, nullptr),
        "a scratch that cannot hold one row reports nothing rather than scanning past its end");
    t.reset();
}

TEST(testAMissingScratchFindsNothing)
{
    auto t = makeTerminal();
    feed(*t, "https://example.com");
    char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
    Screen::LinkScanBuffer none { nullptr, nullptr, 0 };
    CHECK(!Screen::linkAtVisibleCell(t.get(), 0, 0, 3, none, uri, nullptr));
    t.reset();
}

// ---- bare URLs -------------------------------------------------------------

TEST(testABareURLIsFoundAndSpanned)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "see https://example.com/x now");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 10, &link), "https://example.com/x");
    CHECK_MSG(!link.isHyperlink, "detected text is not a declared hyperlink");
    CHECK_MSG(!link.isFilePath, "an http URL is not a path candidate");
    CHECK_EQ(link.column0, 4);
    CHECK_EQ(link.column1, 24);
    t.reset();
}

TEST(testHoveringOutsideTheURLFindsNothing)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "see https://example.com/x now");
    CHECK_MSG(uriAt(*t, 0, 0).empty(), "the word before it");
    CHECK_MSG(uriAt(*t, 0, 26).empty(), "the word after it");
    t.reset();
}

// A URL longer than the row is the case a per-row scan gets wrong: it sees the
// truncated head, matches it, and hands back half a link.
TEST(testAWrappedURLIsStitchedAcrossRows)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "https://example.com/averylongpath/that/wraps");
    Screen::VisibleLink link {};
    const std::string uri = uriAt(*t, 0, 5, &link);
    CHECK_STR_EQ(uri, "https://example.com/averylongpath/that/wraps");
    CHECK_MSG(link.absoluteRow1 > link.absoluteRow0, "the span covers more than one row");
    t.reset();
}

TEST(testTheWrappedURLIsFoundFromItsTailToo)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "https://example.com/averylongpath/that/wraps");
    CHECK_STR_EQ(uriAt(*t, 2, 1), "https://example.com/averylongpath/that/wraps");
    t.reset();
}

// A URL a program hard-wrapped is still one URL.
//
// tmux lays a pane out at its own width and writes each visual line as its own
// write, so the terminal is never told the rows are continuous and nothing
// marks them wrapped. A walk that trusts the wrap flag detects a 450-character
// authorize URL inside tmux as its first line only, underlined to the right edge
// and no further. Written here as the bytes tmux would send -- each row's text, then
// a newline, then the next row's -- rather than through the wrap flag, so the
// case fails on a walk that trusts the flag alone.
TEST(testAHardWrappedURLJoinsAcrossUnmarkedRows)
{
    auto t = makeTerminal(6, 20);
    // The 44-character URL cut at 20 columns and sent as three separate
    // lines, exactly as tmux would. Each cut row is FULL to its last column
    // -- that is what a hard wrap at the pane width produces, and it is the
    // condition the join asks for. A middle row cut one short is not full, and
    // the join declines it: that is the prose case below.
    feed(*t, "https://example.com/\r\naverylongpath/that/w\r\nraps");
    CHECK_STR_EQ(uriAt(*t, 1, 5), "https://example.com/averylongpath/that/wraps");
    CHECK_STR_EQ(uriAt(*t, 0, 2), "https://example.com/averylongpath/that/wraps");
    t.reset();
}

// And a line of prose under a URL is not pulled into it.
//
// The join asks for URL bytes at both edges of the seam. A URL that ends before
// the right margin leaves the last column blank, so a following line that
// merely starts with a letter stays its own line -- which is what keeps a URL
// at the end of one sentence from swallowing the start of the next.
TEST(testARowOfProseBelowAURLStaysOut)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "see https://a.io/x\r\nand then more text");
    CHECK_STR_EQ(uriAt(*t, 0, 8), "https://a.io/x");
    t.reset();
}

// A URL wrapped inside a tmux pane joins at the pane's border.
//
// tmux writes each pane row separately at the pane's width, so the wrap falls
// at the pane's right edge, not the screen's, and no row is ever marked. The
// border glyph beside the clicked cell is the witness here: the pane has not
// scrolled, so no margin pair has been seen yet. The right pane holds prose,
// which the join must not enter.
TEST(testAURLWrappedInsideAPaneJoinsAtItsBorder)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\033[1;1Hhttps://example.com/\033[1;21H\xE2\x94\x82 notes\033[2;1Haverylongpath/that/w\033[2;21H\xE2\x94\x82 notes\033[3;1Hraps\033[3;21H\xE2\x94\x82");
    Screen::VisibleLink paneLink {};
    CHECK_STR_EQ(uriAt(*t, 1, 3, &paneLink), "https://example.com/averylongpath/that/wraps");
    // The span rides the result so a painter can clip the run's rows to the
    // pane rather than stroking the middle rows across the neighbour.
    CHECK(paneLink.spanColumnFirst == 0);
    CHECK(paneLink.spanColumnLast == 19);
    CHECK_STR_EQ(uriAt(*t, 0, 24), "");
    t.reset();
}

// The margins a pane once scrolled with join its rows, border or no border.
//
// tmux scrolls a pane by setting the pane's columns as DECSLRM margins and
// clearing them again, so the pair is gone by the time the text is written --
// but the screen remembered it, and the join uses it where no border glyph
// stands (pane-border-lines simple, say). The count is checked first so a
// memory that never recorded cannot pass this test by accident.
TEST(testTheMarginsAPaneOnceScrolledWithJoinTheRows)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\033[?69h\033[1;20s\033[s\033[?69l");
    CHECK(t->screens.active->rememberedMarginCount == 1);
    CHECK(t->screens.active->rememberedMargins[0].left == 0);
    CHECK(t->screens.active->rememberedMargins[0].right == 19);
    feed(*t, "\033[1;1Hhttps://example.com/\033[2;1Haverylongpath/that/w\033[3;1Hraps");
    CHECK_STR_EQ(uriAt(*t, 1, 3), "https://example.com/averylongpath/that/wraps");
    t.reset();
}

// A remembered pair from a layout that no longer exists is refuted by text
// crossing its edge, and the join falls back to the screen's own width.
TEST(testAStaleMarginPairIsRefutedByTextAcrossIt)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\033[?69h\033[1;20s\033[s\033[?69l");
    CHECK(t->screens.active->rememberedMarginCount == 1);
    // Forty URL characters fill row 1 to the screen edge, straight across the
    // remembered pair's right margin at column 19.
    feed(*t, "\033[1;1Hhttps://example.com/averylongpath/that/w\033[2;1Hraps");
    Screen::VisibleLink wideLink {};
    CHECK_STR_EQ(uriAt(*t, 0, 5, &wideLink), "https://example.com/averylongpath/that/wraps");
    CHECK(wideLink.spanColumnLast == 39); // the refuted pair fell back to the screen's width
    t.reset();
}

// The memory dies with the margins: after a reset that clears them, the same
// borderless layout is two lines again.
TEST(testAResetForgetsTheRememberedMargins)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\033[?69h\033[1;20s\033[s\033[?69l");
    CHECK(t->screens.active->rememberedMarginCount == 1);
    feed(*t, "\033[!p"); // DECSTR
    CHECK(t->screens.active->rememberedMarginCount == 0);
    feed(*t, "\033[1;1Hhttps://example.com/\033[2;1Haverylongpath/that/w");
    CHECK_STR_EQ(uriAt(*t, 0, 3), "https://example.com/");
    t.reset();
}

// ---- OSC 8 -----------------------------------------------------------------

TEST(testAnOSC8LinkResolvesToItsTarget)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;;https://example.com/target\x1b\\click here\x1b]8;;\x1b\\ after");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 2, &link), "https://example.com/target");
    CHECK_MSG(link.isHyperlink, "a declared link says so");
    CHECK_EQ(link.column0, 0);
    CHECK_EQ(link.column1, 9);
    t.reset();
}

// The link text reads nothing like a URL, so only the OSC 8 id can find it —
// which is what separates a declared hyperlink from text that looks like one.
TEST(testAnOSC8LinkIsFoundUnderTextThatIsNotAURL)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;;https://example.com/t\x1b\\ordinary words\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 4, &link), "https://example.com/t");
    CHECK(link.isHyperlink);
    t.reset();
}

TEST(testTheCellAfterAnOSC8LinkIsNotInIt)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;;https://example.com/t\x1b\\link\x1b]8;;\x1b\\ tail");
    CHECK(uriAt(*t, 0, 6).empty());
    t.reset();
}

// Two segments of one link emitted under different ids. Grouping by id alone
// splits them; grouping by resolved target keeps the underline whole.
TEST(testOneLinkReemittedUnderTwoIDsSpansBoth)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;id=a;https://example.com/same\x1b\\first\x1b]8;;\x1b\\"
             "\x1b]8;id=b;https://example.com/same\x1b\\second\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/same");
    CHECK_MSG(link.column0 == 0, "the span starts at the first segment");
    CHECK_MSG(link.column1 == 10, "and runs through the second");
    t.reset();
}

// The same link hovered from its SECOND segment. Hovering the first finds the
// span's start by id alone — the hovered cell's own id begins there — so only
// this direction proves the backward reach into a foreign id. Both are needed:
// removing the same-target walk from one loop leaves the other case passing.
TEST(testTheReemittedLinkIsWholeFromItsSecondSegmentToo)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;id=a;https://example.com/same\x1b\\first\x1b]8;;\x1b\\"
             "\x1b]8;id=b;https://example.com/same\x1b\\second\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 7, &link), "https://example.com/same");
    CHECK_MSG(link.column0 == 0, "the span still reaches back to the first segment");
    CHECK_MSG(link.column1 == 10, "and still ends at the second");
    t.reset();
}

// One target reaching a row twice is two links, not one long one. The span is
// the run through the hovered cell; taking the outermost pair of cells carrying
// the target instead puts every cell between them under the link, unlinked text
// included.
TEST(testTheSameTargetTwiceOnARowStaysTwoLinks)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;id=a;https://example.com/same\x1b\\AB\x1b]8;;\x1b\\"
             "        plain        "
             "\x1b]8;id=b;https://example.com/same\x1b\\CD\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 0, &link), "https://example.com/same");
    CHECK_MSG(link.column0 == 0, "the span starts at the hovered occurrence");
    CHECK_MSG(link.column1 == 1, "and ends there, short of the plain text after it");

    Screen::VisibleLink second {};
    CHECK_STR_EQ(uriAt(*t, 0, 23, &second), "https://example.com/same");
    CHECK_MSG(second.column0 == 23, "the far occurrence is its own span");
    CHECK_MSG(second.column1 == 24, "with its own end");
    t.reset();
}

// An explicit `id=` is the spec's own statement that two runs are one link, and
// the showcase entry draws that shape: an id=foo run, three blanks, a second
// id=foo run at the same target, all on one row. Hovering either underlines
// both, so the span crosses the blanks between them -- the one place where
// padding inside a row falls inside a link rather than between two of them.
TEST(testTwoRunsSharingAnIDAreOneSpanAcrossTheBlanksBetweenThem)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;id=foo;https://example.com/manual\x1b\\first run\x1b]8;;\x1b\\"
             "   "
             "\x1b]8;id=foo;https://example.com/manual\x1b\\second run\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/manual");
    CHECK_MSG(link.column0 == 0, "the span starts at the first run");
    CHECK_MSG(link.column1 == 21, "and ends at the last cell of the second");

    Screen::VisibleLink fromSecond {};
    CHECK_STR_EQ(uriAt(*t, 0, 15, &fromSecond), "https://example.com/manual");
    CHECK_MSG(fromSecond.column0 == 0, "hovering the second run reaches back across the gap");
    CHECK_MSG(fromSecond.column1 == 21, "and keeps its own end");

    // One key interns one entry, so both runs carry one id. That id is what the
    // frame's per-cell membership test reads, and it is what leaves the three
    // unlinked cells inside the span's column band out of the highlight.
    const std::uint16_t hoveredID = Screen::visibleCell(t.get(), 0, 0, 1)->linkID();
    CHECK(hoveredID != 0);
    CHECK_MSG(Screen::visibleCell(t.get(), 0, 0, 15)->linkID() == hoveredID,
        "the second run carries the first run's id");
    for (int column = 9; column <= 11; ++column) {
        const std::uint16_t gapID = Screen::visibleCell(t.get(), 0, 0, column)->linkID();
        CHECK_MSG(gapID == 0, "the blanks between the runs carry no link");
        CHECK_MSG(!Rendering::linkHoverContainsCell(true, hoveredID, column, link.column0, link.column1, gapID),
            "and draw unmarked although the run's column band covers them");
    }
    CHECK_MSG(Rendering::linkHoverContainsCell(true, hoveredID, 15, link.column0, link.column1, hoveredID),
        "while the far run draws as hovered");
    t.reset();
}

// The same row without the key is two links. A keyless repeat of one URI
// interns into the entry the first use minted, so both runs carry one id here
// too and the id alone cannot tell this row from the one above -- the ring
// entry's key is what can.
TEST(testTwoRunsOfOneURIWithoutAnIDStayTwoLinks)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;;https://example.com/manual\x1b\\first run\x1b]8;;\x1b\\"
             "   "
             "\x1b]8;;https://example.com/manual\x1b\\second run\x1b]8;;\x1b\\");
    const std::uint16_t firstID = Screen::visibleCell(t.get(), 0, 0, 1)->linkID();
    CHECK(firstID != 0);
    CHECK_MSG(Screen::visibleCell(t.get(), 0, 0, 15)->linkID() == firstID,
        "the fixture proves nothing unless the two runs share an id");

    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/manual");
    CHECK_MSG(link.column0 == 0, "the span is the hovered run");
    CHECK_MSG(link.column1 == 8, "and stops short of the blanks after it");

    Screen::VisibleLink second {};
    CHECK_STR_EQ(uriAt(*t, 0, 15, &second), "https://example.com/manual");
    CHECK_MSG(second.column0 == 12, "the far occurrence is its own span");
    CHECK_MSG(second.column1 == 21, "with its own end");
    t.reset();
}

// Only blanks are padding, key or no key. Words between two runs of one keyed
// link end the span at the hovered run, so what the key buys is a reach across
// a gap and never a reach across prose.
TEST(testWordsBetweenTwoRunsOfOneIDStillEndTheSpan)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;id=foo;https://example.com/manual\x1b\\first run\x1b]8;;\x1b\\"
             " and then "
             "\x1b]8;id=foo;https://example.com/manual\x1b\\second run\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/manual");
    CHECK_MSG(link.column0 == 0, "the span starts at the hovered run");
    CHECK_MSG(link.column1 == 8, "and ends where its own text does");
    t.reset();
}

// The shape a horizontal tmux split draws: two panes side by side with a border
// column between, each showing the same target. tmux enables DECSLRM against a
// terminal reporting VT420 or better and writes each pane row by cursor
// address, so one row genuinely holds both panes' cells.
TEST(testOneTargetInTwoSideBySidePanesDoesNotBridgeThem)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b[1;1H\x1b]8;id=a;https://example.com/same\x1b\\left\x1b]8;;\x1b\\");
    feed(*t, "\x1b[1;20H|");
    feed(*t, "\x1b[1;22H\x1b]8;id=b;https://example.com/same\x1b\\right\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/same");
    CHECK_MSG(link.column1 < 19, "the span stops inside the pane it was hovered in");
    t.reset();
}

// The same split with nothing drawn between the panes: a pane border can be a
// styled blank rather than a line character, so blankness alone cannot be what
// separates them. Crossing blank cells is what carries a link into its next row,
// and this is the same shape inside one row, where it must not.
TEST(testTwoSideBySidePanesDoNotBridgeAcrossBlanksEither)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b[1;1H\x1b]8;id=a;https://example.com/same\x1b\\left\x1b]8;;\x1b\\");
    feed(*t, "\x1b[1;22H\x1b]8;id=b;https://example.com/same\x1b\\right\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/same");
    CHECK_MSG(link.column1 < 21, "the span stops at the pane it was hovered in");
    t.reset();
}

// Two genuinely different links must not merge, however adjacent.
TEST(testTwoDifferentLinksDoNotMerge)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "\x1b]8;;https://example.com/one\x1b\\aaa\x1b]8;;\x1b\\"
             "\x1b]8;;https://example.com/two\x1b\\bbb\x1b]8;;\x1b\\");
    Screen::VisibleLink first {};
    Screen::VisibleLink second {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &first), "https://example.com/one");
    CHECK_STR_EQ(uriAt(*t, 0, 4, &second), "https://example.com/two");
    CHECK_MSG(first.column1 == 2, "the first ends where its own text does");
    CHECK_MSG(second.column0 == 3, "and the second starts at its own");
    t.reset();
}

// A row joins the hovered link's group by matching id or by resolving to the
// same target. Both discriminations only decide anything ACROSS rows — on one
// row the span is bounded by the link's own cells either way. Every multi-row
// case above carries a single link, and every two-link case sits on one row, so
// nothing yet asks the question the group test exists to answer.
TEST(testADifferentLinkOnTheNextRowDoesNotJoinTheGroup)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "\x1b]8;id=a;https://example.com/one\x1b\\aaa\x1b]8;;\x1b\\\r\n"
             "\x1b]8;id=b;https://example.com/two\x1b\\bbb\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/one");
    CHECK_MSG(link.absoluteRow1 == link.absoluteRow0,
        "a link on another row with another target is not part of this span");
}

// The id is the identity, not the target: two cells declaring the same id are
// one link even when their URIs differ. A program that reuses an id for an
// unrelated target has said they are the same anchor, and the group follows the
// declaration rather than second-guessing it. Written down because the opposite
// is the intuitive guess — the target looks like the stronger identity — and a
// reader who assumes it would report this as a defect.
TEST(testTheSameIDGroupsRowsEvenWhenTheTargetsDiffer)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "\x1b]8;id=a;https://example.com/one\x1b\\aaa\x1b]8;;\x1b\\\r\n"
             "\x1b]8;id=a;https://example.com/two\x1b\\bbb\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/one");
    CHECK_MSG(link.absoluteRow1 > link.absoluteRow0,
        "a shared id makes both rows one link, whatever each row's target says");
}

// The other half of the group test: a row joins by resolving to the same target
// under a DIFFERENT id. A program that re-emits one link per visual segment
// gives each segment its own id=, so this is the shape a wrapped hyperlink
// actually takes, and only the URI comparison can see it.
//
// The id case above cannot reach this branch: ids intern on the id= key, so two
// cells sharing an id resolve to one stored URI by construction. That makes the
// id comparison a fast path the URI comparison would also satisfy -- which is
// why mutating it survives, and why this case rather than that one is what
// holds the group walk up.
TEST(testAnotherIDForTheSameTargetJoinsTheGroupAcrossRows)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "\x1b]8;id=a;https://example.com/same\x1b\\aaa\x1b]8;;\x1b\\\r\n"
             "\x1b]8;id=b;https://example.com/same\x1b\\bbb\x1b]8;;\x1b\\");
    Screen::VisibleLink link {};
    CHECK_STR_EQ(uriAt(*t, 0, 1, &link), "https://example.com/same");
    CHECK_MSG(link.absoluteRow1 > link.absoluteRow0,
        "a second id pointing at the same target is the same link, on the next row too");
}

// ---- paths -----------------------------------------------------------------

TEST(testAPathIsACandidateRatherThanALink)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "edit src/main.cpp:42");
    Screen::VisibleLink link {};
    const std::string uri = uriAt(*t, 0, 8, &link);
    CHECK_MSG(!uri.empty(), "a path with a slash is found");
    CHECK_MSG(link.isFilePath, "and reported as a candidate the caller must confirm");
    CHECK_MSG(!link.isHyperlink, "not as a declared hyperlink");
    t.reset();
}

TEST(testASlashlessWordIsNotAPath)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "make clean");
    CHECK(uriAt(*t, 0, 6).empty());
    t.reset();
}

// ---- the span is absolute, so it survives scrolling ------------------------

TEST(testTheSpanIsInAbsoluteRows)
{
    auto t = makeTerminal(4, 40);
    feed(*t, "\r\n\r\n\r\nhttps://example.com/x");
    Screen::VisibleLink link {};
    CHECK_MSG(!uriAt(*t, 3, 2, &link).empty(), "the link is on the last visible row");
    const int expected = t->scrollbackCount + 3;
    CHECK_MSG(link.absoluteRow0 == expected, "absolute row is scrollback plus the visible row");
    t.reset();
}

// A declared-only scan reads the hovered cell's link id and stops there.
//
// Everything past that read exists to find a link the program never declared —
// the pane-edge witnesses, the soft-wrap flatten, the URL and path sweeps — and
// a modifier-less hover runs on every pointer move over any text at all. Asking
// for the whole scan there spends a row walk per move to answer no.
TEST(testADeclaredOnlyScanSkipsTheGuessSweep)
{
    auto t = makeTerminal(6, 40);
    feed(*t, "see https://example.com/x now");
    CHECK_MSG(uriAt(*t, 0, 6) == "https://example.com/x", "the whole scan finds the guess");
    CHECK_MSG(uriAt(*t, 0, 6, nullptr, Screen::LinkScanScope::Declared).empty(),
        "a declared-only scan does not, because the program declared nothing");

    // A declared link is found by both, which is what makes the case above a
    // statement about the scope rather than about the scan being broken.
    auto declared = makeTerminal(6, 40);
    feed(*declared, "\033]8;;https://example.com/y\033\\LINK\033]8;;\033\\");
    CHECK(uriAt(*declared, 0, 1) == "https://example.com/y");
    CHECK_MSG(uriAt(*declared, 0, 1, nullptr, Screen::LinkScanScope::Declared) == "https://example.com/y",
        "the cheap scan finds a declared link, which is the point of it");
}

} // namespace

UBIQUITTY_TEST_MAIN()
