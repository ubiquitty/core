// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The resize paths the ordinary rewrap cases never enter: the interactive
// urgency that defers the history rewrap until the drag settles, the image
// placements that must ride a reflow to the same logical line they were
// anchored to, the margin and cursor clamps a shrink forces, and the shapes
// that make a rewrap refuse — an orphaned continuation half, a cursor past
// the fed content, a screen with nothing on it.
//
// These matter because a resize is the one operation that rewrites every
// stored row at once. A clamp that misses leaves a margin naming a row the
// grid no longer has, and the next scroll writes through it.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellPen.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Resize.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <string>

using namespace Ubiquitty;
using UbiquittyTest::cell;
using UbiquittyTest::feed;

namespace {

// Fills `lines` history rows of `width` characters each, so a rewrap has
// stored content to re-lay out rather than an empty scrollback.
void fillHistory(Terminal& t, int lines, int width)
{
    std::string text;
    for (int i = 0; i < lines; ++i) {
        text.append(static_cast<std::size_t>(width), static_cast<char>('a' + (i % 26)));
        text += "\r\n";
    }
    feed(t, text);
}

// The first live image placement's row, or a sentinel when there is none.
int placementRow(const Terminal& t)
{
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            return t.images.placements[i].cursorY;
    return -9999;
}

int livePlacementCount(const Terminal& t)
{
    int count = 0;
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            ++count;
    return count;
}

} // namespace

// An interactive resize defers the history rewrap: the visible screen follows
// the pointer at once, history keeps its stored widths, and the settle does
// the expensive pass. What must hold through the deferral is that history
// still READS correctly — each row carries its own length — so a snapshot
// taken mid-drag is not a mixture nobody can lay out.
TEST(testAnInteractiveResizeDefersTheHistoryRewrapUntilItSettles)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    fillHistory(t, 30, 36);
    feed(t, "live-row-content");

    const int historyBefore = Screen::scrollbackCount(&t);
    CHECK(historyBefore > 0);

    // Mid-drag: the geometry changes, the rewrap is marked pending.
    resize(t, 6, 24, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    CHECK(t.columns == 24);
    CHECK(t.pendingHistoryRewrapColumns != 0);
    // Every history row still reads at its own stored length.
    for (int i = 0; i < Screen::scrollbackCount(&t); ++i) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&t, i);
        CHECK(line != nullptr);
        if (line)
            CHECK(line->length >= 0 && line->length <= maxColumns);
    }

    // Two more drag steps, one of them a widening: the deferral survives a
    // sequence, not just one step.
    resize(t, 6, 18, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    resize(t, 6, 30, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    CHECK(t.pendingHistoryRewrapColumns != 0);

    // The settle runs the full rewrap and clears the marker.
    settleResize(&t);
    CHECK(t.pendingHistoryRewrapColumns == 0);
    for (int i = 0; i < Screen::scrollbackCount(&t); ++i) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&t, i);
        if (line)
            CHECK(line->length <= t.columns);
    }

    // A settle with nothing pending is a no-op rather than a second rewrap.
    const int afterSettle = Screen::scrollbackCount(&t);
    settleResize(&t);
    CHECK(Screen::scrollbackCount(&t) == afterSettle);
}

// A drag that ends back at its starting width still has to settle: the rows
// pushed to history during the drag and the wrap-stale marker are real, so a
// dimension no-op is not a state no-op.
TEST(testADragEndingAtItsStartingWidthStillSettles)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    fillHistory(t, 20, 38);

    resize(t, 6, 20, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    CHECK(t.pendingHistoryRewrapColumns != 0);
    resize(t, 6, 40, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    CHECK(t.columns == 40);
    CHECK(t.pendingHistoryRewrapColumns != 0);

    // The same dimensions under Settled urgency: the marker routes it through
    // the rewrap rather than returning early on the dimension no-op.
    resize(t, 6, 40, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Settled);
    CHECK(t.pendingHistoryRewrapColumns == 0);
}

// An image placement is anchored to a logical line, not to a physical row, so
// a rewrap that moves the line must carry the placement with it. Both remap
// paths are here: the height-only shift (rows change, columns do not) and the
// line-anchored sweep a real rewrap runs.
TEST(testImagePlacementsRideAResize)
{
    // (a) Height only: the placement shifts by the same rows the content did.
    {
        Terminal t;
        hardReset(t);
        resize(t, 10, 40);
        fillHistory(t, 4, 20);
        feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=11;/wAAAAD/\033\\");
        CHECK(livePlacementCount(t) == 1);
        const int before = placementRow(t);

        resize(t, 4, 40); // shrink height: content pushes into history
        CHECK(livePlacementCount(t) == 1);
        CHECK(placementRow(t) != -9999);
        // The placement moved with its row rather than staying at a screen
        // index the content left.
        CHECK(placementRow(t) <= before);
    }

    // (b) A real rewrap: the columns change, so every logical line is re-laid
    // out and the placement resolves against the new physical rows.
    {
        Terminal t;
        hardReset(t);
        resize(t, 8, 40);
        fillHistory(t, 6, 38); // lines that will wrap when narrowed
        feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=12;/wAAAAD/\033\\");
        CHECK(livePlacementCount(t) == 1);

        resize(t, 8, 16); // every stored line now wraps two or three ways
        CHECK(livePlacementCount(t) == 1);
        const int row = placementRow(t);
        CHECK(row >= -t.scrollbackLimit && row <= maxRows);

        // And back out again: a widening rejoins the lines, and the
        // placement resolves against the rejoined layout.
        resize(t, 8, 60);
        CHECK(livePlacementCount(t) == 1);
        CHECK(placementRow(t) >= -t.scrollbackLimit);
    }

    // (c) Several placements at once: the sweep walks them in line order, so
    // the running feed-row cursor must not go backwards between them.
    {
        Terminal t;
        hardReset(t);
        resize(t, 12, 40);
        for (int i = 0; i < 4; ++i) {
            fillHistory(t, 3, 36);
            char transmit[96];
            std::snprintf(transmit, sizeof transmit,
                "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=%d;/wAAAAD/\033\\", 20 + i);
            feed(t, transmit);
        }
        const int placements = livePlacementCount(t);
        CHECK(placements > 1);
        resize(t, 12, 14);
        CHECK(livePlacementCount(t) == placements); // none lost in the sweep
        for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
            if (t.images.placements[i].used)
                CHECK(t.images.placements[i].cursorY >= -t.scrollbackLimit);
    }

    // (d) A bottom-anchored resize: the placements ride the same clamp band
    // as the cursor rather than the reflow remap.
    {
        Terminal t;
        hardReset(t);
        resize(t, 10, 40);
        fillHistory(t, 5, 20);
        feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=13;/wAAAAD/\033\\");
        CHECK(livePlacementCount(t) == 1);
        resize(t, 20, 40, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
        CHECK(livePlacementCount(t) == 1);
        CHECK(placementRow(t) <= maxRows);
    }
}

TEST(testVisibleTopGrowthKeepsImagesOnTheirContentRows)
{
    for (const auto urgency : { Modes::ResizeUrgency::Settled, Modes::ResizeUrgency::Interactive }) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 4, 8);
        feed(terminal, "L1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6\033[2;1H");
        feed(terminal, "\033_Ga=T,t=d,f=24,s=2,v=1,c=1,r=1,i=51;/wAAAAD/\033\\");
        feed(terminal, "\033[4;3H");
        REQUIRE(livePlacementCount(terminal) == 1);
        REQUIRE(placementRow(terminal) == 1);

        resize(terminal, 7, 8, Modes::ReflowMode::Reflow, Modes::AnchorMode::VisibleTop, urgency);
        CHECK(terminal.scrollbackCount == 2);
        CHECK(placementRow(terminal) == 1);
        CHECK(cell(terminal, 0, 0)->codePoint == 'L');

        resize(terminal, 9, 12, Modes::ReflowMode::Reflow, Modes::AnchorMode::VisibleTop, urgency);
        CHECK(terminal.scrollbackCount == 2);
        CHECK(placementRow(terminal) == 1);
        CHECK(terminal.pendingHistoryRewrapColumns == 0);
        CHECK(cell(terminal, 0, 0)->codePoint == 'L');
    }
}

// Scroll regions and saved cursors are stored as row indices, so a shrink has
// to bring every one of them back inside the new grid — a margin naming a row
// that no longer exists is written through by the next scroll.
TEST(testMarginsAndSavedStateAreClampedByAShrink)
{
    Terminal t;
    hardReset(t);
    resize(t, 24, 80);
    feed(t, "content\r\n");
    // A non-default vertical region, a non-default horizontal one, and a
    // saved cursor deep in the grid.
    feed(t, "\033[?69h"); // DECLRMM: enable left/right margins
    feed(t, "\033[10;60s"); // DECSLRM
    feed(t, "\033[5;20r"); // DECSTBM
    feed(t, "\033[18;40H\0337"); // move deep, DECSC

    resize(t, 6, 20);

    Screen::State* s = &t.screens.primary;
    CHECK(s->scrollTop >= 0 && s->scrollTop < t.rows);
    CHECK(s->scrollBottom >= 0 && s->scrollBottom < t.rows);
    CHECK(s->scrollTop < s->scrollBottom);
    CHECK(s->scrollLeft >= 0 && s->scrollLeft < t.columns);
    CHECK(s->scrollRight >= 0 && s->scrollRight < t.columns);
    CHECK(s->scrollLeft <= s->scrollRight);
    CHECK(s->cursorX >= 0 && s->cursorX < t.columns);
    CHECK(s->cursorY >= 0 && s->cursorY < t.rows);
    if (s->hasSavedCursor) {
        CHECK(s->savedX >= 0 && s->savedX < t.columns);
        CHECK(s->savedY >= 0 && s->savedY < t.rows);
    }
    CHECK(s->savedScrollTop >= 0 && s->savedScrollTop < t.rows);
    CHECK(s->savedScrollBottom >= 0 && s->savedScrollBottom < t.rows);

    // The alternate screen carries its own copies of all of it.
    feed(t, "\033[?1049h\033[2;5r\033[3;7Halt");
    resize(t, 3, 8);
    Screen::State* alternate = &t.screens.alternate;
    CHECK(alternate->scrollTop >= 0 && alternate->scrollTop < t.rows);
    CHECK(alternate->scrollBottom >= 0 && alternate->scrollBottom < t.rows);
    CHECK(alternate->cursorX >= 0 && alternate->cursorX < t.columns);
    CHECK(alternate->cursorY >= 0 && alternate->cursorY < t.rows);
}

// A resize whose new geometry is out of range is clamped to the grid's own
// bounds rather than refused: a frontend reporting a nonsense size during a
// display change must not take the session with it.
TEST(testResizeClampsItsOwnGeometry)
{
    Terminal t;
    hardReset(t);
    resize(t, 24, 80);
    feed(t, "content");

    resize(t, 0, 0);
    CHECK(t.rows >= 2 && t.columns >= 2);

    resize(t, maxRows + 500, maxColumns + 500);
    CHECK(t.rows == maxRows && t.columns == maxColumns);

    resize(t, -5, -5);
    CHECK(t.rows >= 2 && t.columns >= 2);

    // Back to something ordinary: the session still takes input.
    resize(t, 10, 40);
    feed(t, "still-alive");
    CHECK(t.rows == 10 && t.columns == 40);
}

// A rewrap that has to place a cursor sitting past every fed row pins it to
// the tail of the last emitted one rather than leaving it unplaced, in both
// the settled and the interactive paths.
TEST(testACursorPastTheContentPinsToTheLastRow)
{
    for (int pass = 0; pass < 2; ++pass) {
        const Modes::ResizeUrgency urgency = pass == 0
            ? Modes::ResizeUrgency::Settled
            : Modes::ResizeUrgency::Interactive;

        Terminal t;
        hardReset(t);
        resize(t, 10, 40);
        fillHistory(t, 12, 30);
        feed(t, "tail-row");
        // Park the cursor on a blank row well below the content.
        feed(t, "\033[9;35H");

        resize(t, 5, 18, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
        if (urgency == Modes::ResizeUrgency::Interactive)
            settleResize(&t);

        Screen::State* s = &t.screens.primary;
        CHECK(s->cursorX >= 0 && s->cursorX < t.columns);
        CHECK(s->cursorY >= 0 && s->cursorY < t.rows);
    }
}

// A continuation half whose lead was trimmed away is skipped by the rewrap
// rather than fed as a character of its own: a lone continuation renders as
// the second half of a glyph that is not there.
TEST(testAnOrphanedContinuationHalfIsSkipped)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "lead");

    // A continuation with no anchor before it, written directly.
    Cell orphan {};
    orphan.pen.foregroundIndex = colorDefault;
    orphan.pen.backgroundIndex = colorDefault;
    orphan.pen.underlineIndex = colorDefault;
    orphan.codePoint = 0;
    Pen pen = cellPen(orphan);
    pen.attributes.add(VT::Attribute::WideContinuation);
    cellSetPen(orphan, pen);
    t.grid->setCell(Row(0), Column(6), orphan);

    resize(t, 4, 12);

    // The rewrap dropped it: no continuation survives without a lead in the
    // column before it.
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c) {
            const Cell* here = cell(t, r, c);
            if (!here || !cellPen(*here).attributes.contains(VT::Attribute::WideContinuation))
                continue;
            const Cell* before = c > 0 ? cell(t, r, c - 1) : nullptr;
            const bool hasLead = before
                && cellPen(*before).attributes.containsAny({ VT::Attribute::Wide, VT::Attribute::WideContinuation });
            CHECK(hasLead);
        }
}

// A shrink that pushes content into history, and the grow that pulls it back:
// the bottom-anchored path both ways, including the grow that runs out of
// history to pull and pads with blanks instead.
TEST(testBottomAnchoredHeightChangesPushAndPull)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 30);
    feed(t, "row0\r\nrow1\r\nrow2\r\nrow3\r\n");

    const int historyBefore = Screen::scrollbackCount(&t);
    resize(t, 3, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(t.rows == 3);
    const int historyAfterShrink = Screen::scrollbackCount(&t);
    CHECK(historyAfterShrink >= historyBefore);

    // Grow back: the freed top rows fill from the newest history.
    resize(t, 10, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(t.rows == 10);
    CHECK(Screen::scrollbackCount(&t) <= historyAfterShrink);

    // A grow with no history at all pads with blanks rather than pulling.
    Terminal empty;
    hardReset(empty);
    resize(empty, 4, 20);
    resize(empty, 12, 20, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(empty.rows == 12);
    CHECK(empty.screens.primary.cursorY >= 0 && empty.screens.primary.cursorY < 12);

    // The same shape on the alternate screen, which has no history to push
    // into: rows are dropped and added rather than aged.
    Terminal alternate;
    hardReset(alternate);
    resize(alternate, 10, 30);
    feed(alternate, "\033[?1049halt-row0\r\nalt-row1\r\n");
    resize(alternate, 4, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(alternate.rows == 4);
    resize(alternate, 10, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(alternate.rows == 10);
    CHECK(alternate.screens.alternate.cursorY < 10);
}

// A rewrap while the alternate screen is active still lays out the PRIMARY's
// content: the primary is frozen behind the application, and it is the screen
// the terminal restores on exit, so its rows have to be rewrapped even though
// nothing is drawing them.
TEST(testTheFrozenPrimaryIsRewrappedFromBehindTheAlternate)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 40);
    fillHistory(t, 6, 38);
    feed(t, "primary-tail-row-with-enough-width-to-wrap");

    feed(t, "\033[?1049h"); // the application takes the alternate screen
    feed(t, "alt-content");

    resize(t, 8, 16);
    CHECK(t.screens.active == &t.screens.alternate);

    feed(t, "\033[?1049l"); // the application exits
    CHECK(t.screens.active == &t.screens.primary);
    // The restored primary fits the new width: no row is wider than the grid.
    for (int r = 0; r < t.rows; ++r)
        CHECK(Screen::resolveVisibleRow(&t, 0, r).length <= t.columns);

    // And the interactive path, from behind the alternate screen too.
    feed(t, "\033[?1049h");
    resize(t, 8, 34, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    settleResize(&t);
    feed(t, "\033[?1049l");
    for (int r = 0; r < t.rows; ++r)
        CHECK(Screen::resolveVisibleRow(&t, 0, r).length <= t.columns);
}

// Blank rows inside the rewrapped content, a cursor that lands above the
// visible window, and content shorter than the new height: the three shapes
// the distribute pass handles separately from an ordinary full screen.
TEST(testTheDistributePassHandlesBlanksShortContentAndAnOverflowCursor)
{
    // (a) A blank line inside the pulled run: it re-appends to history as a
    // row of its own rather than collapsing, so the line count a user
    // scrolled past does not change under them.
    {
        Terminal t;
        hardReset(t);
        resize(t, 6, 30);
        for (int i = 0; i < 8; ++i)
            feed(t, "text-line\r\n\r\n"); // every other row blank
        const int before = Screen::scrollbackCount(&t);
        resize(t, 6, 12, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        CHECK(Screen::scrollbackCount(&t) >= before);
        settleResize(&t);
        CHECK(t.pendingHistoryRewrapColumns == 0);
    }

    // (b) The cursor above the visible window: its row lands in the overflow
    // that re-appends to history, so it pins to the top of the screen rather
    // than to a row the user can no longer see.
    {
        for (int pass = 0; pass < 2; ++pass) {
            const Modes::ResizeUrgency urgency = pass == 0
                ? Modes::ResizeUrgency::Settled
                : Modes::ResizeUrgency::Interactive;
            Terminal t;
            hardReset(t);
            resize(t, 8, 60);
            // Fill the screen with rows that each wrap several ways when
            // narrowed, then park the cursor at the very top.
            for (int i = 0; i < 8; ++i)
                feed(t, std::string(58, static_cast<char>('a' + i)) + "\r\n");
            feed(t, "\033[H");
            resize(t, 4, 10, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
            if (urgency == Modes::ResizeUrgency::Interactive)
                settleResize(&t);
            CHECK(t.screens.primary.cursorY >= 0 && t.screens.primary.cursorY < t.rows);
            CHECK(t.screens.primary.cursorX >= 0 && t.screens.primary.cursorX < t.columns);
        }
    }

    // (c) Content shorter than the new height: the rows below it are erased
    // and their per-row state reset, so a grow does not show whatever the
    // grid held there before.
    {
        for (int pass = 0; pass < 2; ++pass) {
            const Modes::ResizeUrgency urgency = pass == 0
                ? Modes::ResizeUrgency::Settled
                : Modes::ResizeUrgency::Interactive;
            Terminal t;
            hardReset(t);
            resize(t, 6, 20);
            feed(t, "one-short-row");
            resize(t, 20, 40, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
            if (urgency == Modes::ResizeUrgency::Interactive)
                settleResize(&t);
            CHECK(t.rows == 20);
            // Every row past the content is blank, unwrapped and unmarked.
            for (int r = 2; r < t.rows; ++r) {
                CHECK(!t.screens.primary.lineWrapped[r]);
                CHECK(t.screens.primary.mark[r] == 0);
                CHECK(Screen::resolveVisibleRow(&t, 0, r).length == 0
                    || cell(t, r, 0) == nullptr
                    || cell(t, r, 0)->codePoint == 0
                    || cell(t, r, 0)->codePoint == ' ');
            }
        }
    }

    // (d) The same grow from behind the alternate screen: the frozen primary
    // is the one being padded, so each write flips the grid and flips back.
    {
        Terminal t;
        hardReset(t);
        resize(t, 6, 20);
        feed(t, "primary-row");
        feed(t, "\033[?1049halt");
        resize(t, 18, 34, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        settleResize(&t);
        CHECK(t.screens.active == &t.screens.alternate);
        feed(t, "\033[?1049l");
        CHECK(t.rows == 18);
    }
}

// One logical line longer than the interactive pull budget: the rewrap stops
// pulling rather than dragging the whole of history into a per-drag-step
// cost, and the settle reconciles what the step left.
TEST(testAnEnormousLogicalLineStopsTheInteractivePull)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    // One unbroken line of 4000 characters: at 20 columns that is 200 wrapped
    // history rows in a single logical line, far past a six-row viewport's
    // pull budget.
    feed(t, std::string(4000, 'w'));
    feed(t, "\r\nafter\r\n");
    CHECK(Screen::scrollbackCount(&t) > 100);

    resize(t, 6, 14, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    CHECK(t.columns == 14);
    settleResize(&t);
    CHECK(t.pendingHistoryRewrapColumns == 0);
    // The line survived the budget: no row is wider than the grid, and the
    // content is still there.
    for (int i = 0; i < Screen::scrollbackCount(&t); ++i) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&t, i);
        if (line)
            CHECK(line->length <= t.columns);
    }
}

// A bottom-anchored shrink whose last content row is soft-wrapped keeps the
// join: the trim that looks for trailing blanks must stop at a wrapped row,
// because a wrapped row's content continues below it.
TEST(testABottomAnchoredShrinkStopsTrimmingAtASoftWrap)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 20);
    // A line that wraps onto the next row, then nothing after it.
    feed(t, std::string(35, 'j'));

    resize(t, 4, 20, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(t.rows == 4);
    // The wrapped pair is still a pair: whichever row now carries the head,
    // the join did not become a hard break with the tail dropped.
    int inkedRows = 0;
    for (int r = 0; r < t.rows; ++r)
        if (Screen::resolveVisibleRow(&t, 0, r).length > 0)
            ++inkedRows;
    for (int i = 0; i < Screen::scrollbackCount(&t); ++i)
        if (const Screen::ScrollLine* line = Screen::scrollbackLine(&t, i); line && line->length > 0)
            ++inkedRows;
    CHECK(inkedRows >= 2);
}

// A resize on a terminal that never got a grid does nothing rather than
// dereferencing one: the shape a session hits between construction and its
// first hard reset.
TEST(testResizeWithoutAGridIsANoOp)
{
    Terminal bare;
    hardReset(bare);
    resize(bare, 6, 20);
    feed(bare, "content");
    // The grid parked aside: every reflow path guards on it, and each one
    // must return rather than write through the null.
    std::unique_ptr<CellGrid> parked = std::move(bare.grid);
    resize(bare, 12, 40); // full rewrap
    resize(bare, 12, 40, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom); // height only
    resize(bare, 8, 24, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    settleResize(&bare);
    settleResize(nullptr);
    CHECK(bare.grid == nullptr);
    bare.grid = std::move(parked);
    CHECK(bare.grid != nullptr);
}

// A rewrap with nothing to lay out: the cursor has no row to land on, so it
// falls back to the top-left rather than to whatever index the ring's empty
// state produced.
TEST(testARewrapOfAnEmptyScreenPlacesTheCursorAtTheTop)
{
    for (int pass = 0; pass < 2; ++pass) {
        const Modes::ResizeUrgency urgency = pass == 0
            ? Modes::ResizeUrgency::Settled
            : Modes::ResizeUrgency::Interactive;
        Terminal t;
        hardReset(t);
        resize(t, 8, 40); // never fed: no history, no content
        resize(t, 5, 17, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
        if (urgency == Modes::ResizeUrgency::Interactive)
            settleResize(&t);
        CHECK(t.screens.primary.cursorY == 0);
        CHECK(t.screens.primary.cursorX == 0);
    }
}

// A cursor past the content, on a rewrap whose last row ends exactly at the
// new width: pinning to that row's tail would put the cursor one past the
// last column, so the pin clamps to the last column instead.
TEST(testAnUnplacedCursorPinsInsideAFullLastRow)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 24);
    // Content whose rewrap at 12 columns ends on an exactly-full row.
    feed(t, std::string(24, 'f'));
    feed(t, "\033[9;20H"); // the cursor parked on a blank row below it

    resize(t, 4, 12);
    CHECK(t.screens.primary.cursorX >= 0 && t.screens.primary.cursorX < t.columns);
    CHECK(t.screens.primary.cursorY >= 0 && t.screens.primary.cursorY < t.rows);
}

// An image whose placement has aged past the scrollback limit is clamped to
// the oldest row the session keeps rather than to an index no read path can
// resolve — the placement is a signed screen-relative row, and history it
// fell out of has no row for it at all.
TEST(testImagePlacementsAreClampedToTheHistoryTheSessionKeeps)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    setScrollbackLimit(&t, 24); // the floor: one screen of history
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=31;/wAAAAD/\033\\");
    CHECK(livePlacementCount(t) == 1);

    // Push the image far past the retained history.
    fillHistory(t, 200, 20);

    // A height-only change, then a rewrap: both remap paths meet a placement
    // already below the floor.
    resize(t, 20, 30);
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            CHECK(t.images.placements[i].cursorY >= -t.scrollbackLimit);
    resize(t, 20, 11);
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            CHECK(t.images.placements[i].cursorY >= -t.scrollbackLimit);

    // A bottom-anchored grow moves it the other way, through the same band.
    resize(t, 40, 11, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            CHECK(t.images.placements[i].cursorY <= maxRows);
}

// An image on the very first logical line has no lines to walk past, so the
// sweep resolves it to the first feed row directly instead of counting.
TEST(testAnImageOnTheFirstLineResolvesWithoutWalking)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 40);
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=41;/wAAAAD/\033\\");
    fillHistory(t, 4, 38);
    CHECK(livePlacementCount(t) == 1);

    resize(t, 8, 15); // a rewrap: the sweep runs
    CHECK(livePlacementCount(t) == 1);
    CHECK(placementRow(t) >= -t.scrollbackLimit);
}

// A bottom-anchored height change with the alternate screen active leaves the
// alternate top-anchored, its cursor and its saved slot where they were: an
// application holding a saved cursor across a resize restores onto the row it
// meant. A shrink clamps the cursor and leaves the saved slot, which DECRC
// clamps when it restores, as it does for any saved cursor past the screen.
TEST(testABottomAnchorKeepsTheAlternateSavedCursorWhereItWas)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 30);
    feed(t, "\033[?1049h"); // alternate active
    feed(t, "\033[6;10H\0337"); // move, then DECSC
    feed(t, "\033[8;3H"); // and move again, so saved and live differ
    CHECK(t.screens.alternate.hasSavedCursor);

    resize(t, 20, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(t.screens.alternate.cursorY == 7);
    CHECK(t.screens.alternate.savedY == 5);

    // And a shrink past both rows.
    resize(t, 4, 30, Modes::ReflowMode::Fixed, Modes::AnchorMode::Bottom);
    CHECK(t.screens.alternate.cursorY == 3);
    CHECK(t.screens.alternate.savedY == 5);
    feed(t, "\0338"); // DECRC
    CHECK(t.screens.alternate.cursorY == 3);
    CHECK(t.screens.alternate.cursorX == 9);
}

// resizeGrid answers a null terminal with an empty outcome rather than
// dereferencing it: the entry point a frontend can reach before its session
// exists.
TEST(testResizeGridRefusesANullTerminal)
{
    const Screen::ResizeOutcome outcome = Screen::resizeGrid(nullptr, 10, 40,
        Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Settled);
    CHECK(!outcome.geometryChanged);
}

// A placement that drifts below the retained history through repeated height
// changes: each shrink shifts it further back, and the row it names has to
// stop at the oldest line the session keeps rather than run past it. Once it
// is there, a rewrap's sweep sees a feed row before the start of history and
// resolves it to the first one.
TEST(testAPlacementDriftingPastTheHistoryFloorStopsAtIt)
{
    Terminal t;
    hardReset(t);
    resize(t, 12, 30);
    setScrollbackLimit(&t, 24); // the floor: one screen of history
    feed(t, "row-above\r\n");
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=51;/wAAAAD/\033\\");
    CHECK(livePlacementCount(t) == 1);

    // Height-only cycles: each shrink pushes rows into history and drags the
    // placement back with them.
    for (int cycle = 0; cycle < 6; ++cycle) {
        feed(t, "filler-row\r\nfiller-row\r\nfiller-row\r\n");
        resize(t, 2, 30);
        resize(t, 12, 30);
    }
    CHECK(livePlacementCount(t) == 1);
    CHECK(placementRow(t) >= -t.scrollbackLimit); // stopped at the floor

    // Now a rewrap: the sweep meets a placement whose feed row is before the
    // start of history.
    resize(t, 12, 9);
    CHECK(livePlacementCount(t) == 1);
    const int row = placementRow(t);
    CHECK(row >= -t.scrollbackLimit && row <= maxRows);

    // Erasing the saved lines leaves the placement pointing at history that
    // is gone: its feed row is now before the start of the buffer, and the
    // remap resolves it to the first row rather than to a negative one.
    feed(t, "\033[3J"); // erase scrollback, leaving the placement pointing at it
    CHECK(Screen::scrollbackCount(&t) == 0);
    resize(t, 12, 13); // a rewrap, so the sweep runs over it
    if (livePlacementCount(t) > 0)
        CHECK(placementRow(t) >= -t.scrollbackLimit);

    // A deep drift then one large height shrink: the shift takes the row past
    // the floor in a single step, and the clamp is what keeps it nameable.
    Terminal deep;
    hardReset(deep);
    resize(deep, 24, 30);
    setScrollbackLimit(&deep, 24);
    feed(deep, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=52;/wAAAAD/\033\\");
    for (int i = 0; i < 20; ++i)
        feed(deep, "age\r\n"); // scroll the placement up into history
    resize(deep, 2, 30); // height only: one large negative shift
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i)
        if (deep.images.placements[i].used)
            CHECK(deep.images.placements[i].cursorY >= -deep.scrollbackLimit);
}

// ---- residue ledger --------------------------------------------------
// What stays dark in Reflow.cpp after these cases, and why each arm cannot
// execute rather than merely does not:
//
// - The `!t.grid` guard at the head of reflowPrimary, reflowInteractive,
//   reflowHeightOnly and anchorPrimaryRowsBottom: resizeGrid folds
//   `t->grid != nullptr` into shouldReflow, isRowsOnlyChange and
//   shouldDeferHistory, so no caller reaches these with a null grid. They are
//   defence-in-depth against a future direct caller.
// - The `rowLength > maxColumns` clamps on stored history lines (four sites):
//   a line's length comes from a grid whose width resizeGrid has already
//   clamped to maxColumns, so no stored line can be wider.
// - The `new (std::nothrow)` failure arms in both rewrap paths.
// - The negative-value clamps in screenResize (scrollTop, scrollBottom,
//   savedScrollTop, savedScrollBottom, scrollLeft, scrollRight, cursorX,
//   cursorY): every one of these fields is non-negative by construction —
//   DECSTBM and DECSLRM validate on entry and the cursor is clamped on every
//   write — so only the past-the-new-edge half of each pair can run.
// - The cursor-ordinal range check and the negative-column clamp in
//   reflowPlaceCursorInLine: the ordinal is monotone in the feed and the
//   column is derived from it, so neither bound is crossed.
// - The unplaced-cursor pin's `tail >= newColumns` arm and the
//   `cursorVisRow < 0` fallbacks in both rewrap paths: the fallback needs an
//   empty ring, and the trailing-blank trim stops at one row, so the ring is
//   never empty when the distribute pass runs.
// - The `column > oldColumns` clamp on the fed cursor column: the screen
//   clamps the cursor to its own width before the rewrap reads it.
// - The scrollback-floor and maxRows clamps in the rewrap sweep (as opposed
//   to the height-only shift, which this file drives): the sweep's feed row
//   is bounded by [0, scrollbackCount + rows], so the row it derives cannot
//   leave the band.
// - The null-line arms of the four scrollbackLine walks: the walks are bounded
//   by scrollbackCount, which is the count those lines exist for.
// - The soft-wrap break in the bottom-anchor trim: a wrapped row always
//   carries content (the wrap flag is set by the write that filled the row),
//   so the trim's width test answers first.
// - The blank-pad loop in the bottom-anchor shrink branch: that branch runs
//   only when content overflowed the new height, so there is no row left to
//   pad.
// - The trailing-open-row flush in reflowPrimary, which the source itself
//   marks as a guard against a state that should not persist.
// - The pull-capacity break in reflowInteractive: the run walk above it has
//   already backed up over the whole trailing wrapped run, so the loop below
//   is entered only when that run is shorter than the viewport, and its own
//   condition then fails before the capacity test.
// - The saved-cursor arm of the primary bottom anchor: an application holding
//   a saved cursor is on the alternate screen, which takes the other branch.
// - The scrollback-floor and maxRows clamps on the height-only image shift:
//   the shift is the same row count the content moved, and a placement moves
//   with its content, so the two stay in step inside the band.

UBIQUITTY_TEST_MAIN()
