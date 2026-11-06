// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_frame_damage.cpp — the damage-first drain.
//
// The drain is the loop every snapshot backend otherwise types for itself:
// settle the reuse verdict, ack the scroll, mark the exposed band, consume the
// dirty runs. These cases pin the behaviours a re-writer loses first — the
// exposed band marked at drain time, and a baseline mismatch forcing a full
// re-read — so a backend adopting the call inherits the fixes rather than the
// bugs they answered.

#include "Harness.h"
#include "SearchFixture.h"
#include <memory>

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

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

CaptureEligibility freshEligibility(Terminal& t)
{
    CaptureEligibility eligibility;
    eligibility.trustDamage = true;
    eligibility.bufferInitialized = false;
    eligibility.isSameGrid = true;
    eligibility.generationBeforeDrain = t.grid->currentDamageGeneration();
    eligibility.currentRows = t.rows;
    eligibility.currentColumns = t.columns;
    eligibility.lastRows = t.rows;
    eligibility.lastColumns = t.columns;
    return eligibility;
}

// The grid's blink hint is monotone, so the reading that can retract it has to
// come from somewhere else. This is that reading: the screen as it stands, not
// as it has ever been.
TEST(aBlinkScanReadsTheScreenRatherThanTheGridsHistory)
{
    auto t = makeTerminal();
    feed(*t, "\x1b[5mblinking\x1b[0m");

    const int rows = t->rows;
    const int columns = t->columns;
    const Cell* cells = Ubiquitty::Screen::visibleCell(t.get(), 0, 0, 0);
    CHECK_MSG(cells != nullptr, "the screen is addressable");
    CHECK_MSG(Ubiquitty::Rendering::anyCellBlinks(cells, rows, columns), "a blinking cell is found");
    CHECK_MSG(t->grid->hasBlinkHint(), "and the grid agrees while it is on screen");

    // Erase it. The hint cannot retract — it is monotone by design — and the
    // scan reads the screen as it now stands, so the two disagree here, which
    // is the disagreement a viewer needs in order to stand its clock down.
    feed(*t, "\x1b[H\x1b[2J");
    CHECK_MSG(!Ubiquitty::Rendering::anyCellBlinks(cells, rows, columns), "the cleared screen carries no blink");
    CHECK_MSG(t->grid->hasBlinkHint(), "while the grid's hint still says it once did");

    // The control: a screen that never carried blink reads false throughout, so
    // the case above is not passing because the scan always says false after a
    // clear.
    auto plain = makeTerminal();
    feed(*plain, "plain text");
    const Cell* plainCells = Ubiquitty::Screen::visibleCell(plain.get(), 0, 0, 0);
    CHECK_MSG(!Ubiquitty::Rendering::anyCellBlinks(plainCells, plain->rows, plain->columns), "no blink written, none found");
    CHECK_MSG(!plain->grid->hasBlinkHint(), "and the hint was never armed");

    plain.reset();
    t.reset();
}

// The drain hands the plan the grid's sticky hint; the capture refines it
// against the screen it reads. This is what hands a session its partial
// frames back once the blink content is erased — and what keeps the hazard
// raised while the content still stands.
// The blink reading is a whole-screen scan that three callers a frame want of
// content none of them changed, so the grid keeps it. What it must not do is
// outlive a write: a cell that starts blinking after the reading was taken has
// to be seen, or its row never rebuilds and the phase freezes.
TEST(aBlinkReadingIsDroppedByTheNextWrite)
{
    auto t = makeTerminal();
    feed(*t, "\x1b[5mblink\x1b[0m");
    CHECK_MSG(Ubiquitty::Rendering::anyCellBlinks(*t), "the screen carries blink");

    feed(*t, "\x1b[H\x1b[2J");
    CHECK_MSG(!Ubiquitty::Rendering::anyCellBlinks(*t), "and the erased screen does not");

    // The direction that matters: a reading of false standing over a write that
    // brings blink back is a clock that never restarts.
    feed(*t, "\x1b[3;1H\x1b[5mback\x1b[0m");
    CHECK_MSG(Ubiquitty::Rendering::anyCellBlinks(*t), "a write after the reading is seen");

    // And a reading is only taken from a walk that covered every row: a span
    // shorter than the grid leaves rows unread.
    std::vector<std::uint8_t> shortSpan(static_cast<std::size_t>(t->rows) - 2, 0);
    feed(*t, "\x1b[H\x1b[2J\x1b[6;1H\x1b[5mlow\x1b[0m");
    (void)Ubiquitty::Rendering::markBlinkRowsDirty(*t, shortSpan);
    CHECK_MSG(Ubiquitty::Rendering::anyCellBlinks(*t), "a partial walk records no answer for the rows it skipped");

    t.reset();
}

TEST(aSkippedGridPlanOwesEveryRowToTheFrameAfterTheMode)
{
    // DECSET 7402 with an opaque letterbox: no pixel reads a cell, so the
    // frame drains nothing and copies nothing — and the first frame after the
    // mode clears re-reads every row, so a write made under the letterbox
    // reaches the screen.
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    feed(*t, "\x1b[1;1Hplain");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(baseline.isInitialized, "two settled plans leave a baseline to reuse against");

    CHECK_MSG(!canSkipGridCapture(*t, OcclusionSkip::Enabled), "out of the mode every frame reads its cells");
    feed(*t, "\x1b]7402;fill=pass\x1b\\\x1b[?7402h");
    CHECK_MSG(!canSkipGridCapture(*t, OcclusionSkip::Enabled), "a Passthrough letterbox leaves the cells visible");
    feed(*t, "\x1b]7402;fill=204060\x1b\\");
    CHECK_MSG(canSkipGridCapture(*t, OcclusionSkip::Enabled), "the Color letterbox hides every cell");
    CHECK_MSG(!canSkipGridCapture(*t, OcclusionSkip::Disabled), "unless the build keeps the glyph path for a parity A/B");

    // A write nobody sees while the letterbox covers the grid, and a dirty
    // set pre-filled so a plan that clears it is seen to.
    feed(*t, "\x1b[4;1Hunder the letterbox");
    std::ranges::fill(rowDirty, std::uint8_t { 1 });
    const RetainedFramePlan skipped = planSkippedGridFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(!skipped.shouldLoadRetainedTarget, "the skipped frame rebuilds its letterbox rather than loading the target");
    CHECK_MSG(skipped.refresh.canReuseRows, "and copies no row: the capture keeps the cells it holds");
    CHECK_MSG(std::ranges::all_of(rowDirty, [](std::uint8_t b) { return b == 0; }), "no row is owed to a frame nobody reads");
    CHECK_MSG(!baseline.isInitialized, "the baseline is declared gone");

    // The mode clears. The next real plan cannot reuse against a gone
    // baseline, so the capture re-reads every row rather than only the dirty
    // ones — and the write above, left undrained, is among the dirty ones.
    feed(*t, "\x1b[?7402l");
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    const RetainedFramePlan after = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(!after.shouldLoadRetainedTarget, "the first frame after a skip rebuilds whole");
    CHECK_MSG(!after.refresh.canReuseRows, "and the capture re-reads every row");
    CHECK_MSG(rowDirty[3] == 1, "the write made under the letterbox was left for this drain");
    CHECK_MSG(baseline.isInitialized, "which leaves a baseline for the frame after it");
}

TEST(aCursorOwesTheGridNoRowHoweverItMovesOrBlinks)
{
    // The cursor is drawn as a tier over the retained grid in every state,
    // so the plan takes no cursor facts at all: a blink phase, a move and a
    // focus change mark no row, because the grid's pixels are the same
    // whatever the cursor does and the tier is rebuilt every frame from the
    // capture. This is the whole of what the tier buys — a blinking idle
    // prompt re-encodes nothing — so it is pinned as an absence, with the
    // cursor moved by the program and the focus moved by the window.
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;
    t->windowFocused = true;
    t->modes.isCursorVisible = true;

    feed(*t, "\x1b[1;1Hplain\x1b[4;1Hplain\x1b[6;1Hplain\x1b[4;6H");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);

    const auto noRowIsDirty = [&] {
        return std::ranges::all_of(rowDirty, [](std::uint8_t row) { return row == 0; });
    };

    // The blink steps: the plan is asked at two ticks the blink separates,
    // with no cell changed, and no row is owed.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(noRowIsDirty(), "a blink phase change owes no row");

    // The program moves the cursor along the row and onto another: neither
    // the cell it arrives on nor the one it left is in the grid, so neither
    // row is owed.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    feed(*t, "\x1b[4;10H");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(noRowIsDirty(), "a cursor move along its row owes no row");
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    feed(*t, "\x1b[6;3H");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(noRowIsDirty(), "nor a move to another row");

    // Losing focus turns the tier into a hollow outline overlay; the grid does
    // not change.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    t->windowFocused = false;
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(noRowIsDirty(), "a focus change owes no row");

    // Measured at the seam that pays: with nothing but the cursor moving, the
    // frame keeps the retained target and the capture's refresh copies no
    // row. Under a baked cursor every phase of the fade re-read and
    // re-encoded the prompt row, eight times a second for a terminal doing
    // nothing.
    std::vector<Cell> retained(static_cast<std::size_t>(t->rows) * t->columns);
    std::vector<std::uint8_t> attributes(static_cast<std::size_t>(t->rows), 0);
    std::vector<std::uint8_t> marks(static_cast<std::size_t>(t->rows), 0);
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    const RetainedFramePlan primed = planRetainedFrame(*t, baseline, request, rowDirty);
    (void)Rendering::refreshCapturedRowsWithStatistics(*t, primed.refresh, retained, attributes, marks);
    feed(*t, "\x1b[1;4H");
    t->windowFocused = true;
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    const RetainedFramePlan moved = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(moved.shouldLoadRetainedTarget, "a moved cursor keeps the retained target");
    CHECK_MSG(moved.refresh.canReuseRows, "and the capture may keep every row");
    const FrameCopyStatistics statistics = Rendering::refreshCapturedRowsWithStatistics(
        *t, moved.refresh, retained, attributes, marks);
    CHECK_MSG(statistics.rowsCopied == 0, "and the refresh copies no row into the capture");
}

// The grid cache switch is Core's: a frame request carries it into the
// capture, and with it off no frame Loads the retained target or keeps a row,
// however little changed. On, the same frames keep the target, which is the
// control that shows the switch is what refused it.
TEST(aCacheSwitchedOffRebuildsEveryFrameWhole)
{
    CHECK_MSG(Rendering::isRetainedGridCacheEnabled(), "the test process runs with the cache on, as every shipping one does");
    CHECK(RetainedFrameRequest {}.isGridCacheEnabled);
    CHECK(Rendering::RetainedCaptureRequest {}.isGridCacheEnabled);
    for (const bool isGridCacheEnabled : { true, false }) {
        auto t = makeTerminal();
        RetainedFrameBaseline baseline;
        std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
        RetainedFrameRequest request;
        request.rows = t->rows;
        request.columns = t->columns;
        request.isTargetMatching = true;
        request.cellHeightInPixels = 16;
        request.textureHeightInPixels = 16 * t->rows;
        request.isGridCacheEnabled = isGridCacheEnabled;
        feed(*t, "\x1b[1;1Hplain");
        (void)planRetainedFrame(*t, baseline, request, rowDirty);
        (void)planRetainedFrame(*t, baseline, request, rowDirty);
        feed(*t, "\x1b[2;1Hmore");
        std::ranges::fill(rowDirty, std::uint8_t { 0 });
        const RetainedFramePlan plan = planRetainedFrame(*t, baseline, request, rowDirty);
        CHECK_MSG(plan.shouldLoadRetainedTarget == isGridCacheEnabled, isGridCacheEnabled ? "the cache on keeps the target" : "the cache off Loads no retained target");
        CHECK_MSG(plan.damage.canReuse == isGridCacheEnabled, isGridCacheEnabled ? "the cache on reuses the damage set" : "the cache off trusts no damage set");
    }
}

TEST(aFocusedMatchMarksTheRowsItReachedAndTheRowsItLeft)
{
    // The focused match's highlight is baked into the cells rather than drawn
    // as an overlay quad, so a run arriving on a row whose bytes did not change
    // finds that row clean and the cell pass never paints the pill's fill. On
    // screen that is a pill whose ends are drawn and whose middle carries the
    // text's own background: the overlay ring is rebuilt every frame and the
    // cells under it are not. The plan reads the match off the terminal's own
    // search, so no frontend has to remember to report it.
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    feed(*t, "\x1b[1;1Hother\x1b[3;5Hplain\x1b[6;1Hplain");
    UbiquittyTest::SearchFixture search(*t);
    REQUIRE(search.find("plain") == 2);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    const int focusedRow = baseline.matchRow0;
    REQUIRE_MSG(focusedRow == 2 || focusedRow == 5, "the plan reads the focused match's row off the search");
    const int otherRow = focusedRow == 2 ? 5 : 2;

    // The control: a match that has not moved is already in the retained
    // pixels, and marking it every frame repaints a row sixty times a second to
    // draw what is there.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(rowDirty[static_cast<std::size_t>(focusedRow)] == 0, "a match that did not move owes no row");

    // Stepping to the next hit. Nothing wrote a cell, so these marks are the
    // only thing that can put the highlight on one row and take it off another.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    REQUIRE(search.step(Search::StepDirection::Older));
    const RetainedFramePlan stepped = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(stepped.shouldLoadRetainedTarget, "a step keeps the target and repaints rows");
    CHECK_MSG(rowDirty[static_cast<std::size_t>(otherRow)] == 1, "the row the match moved to rebuilds");
    CHECK_MSG(rowDirty[static_cast<std::size_t>(focusedRow)] == 1, "and the row it left, or the old highlight stays painted");
    CHECK_MSG(rowDirty[0] == 0, "and no row it never touched");

    // And a search that ends takes the highlight off the last row it held.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    Screen::ScrollSearch::clear(t.get());
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(rowDirty[static_cast<std::size_t>(otherRow)] == 1, "a match that goes away un-paints the row it held");
}

// A boundary capture is built without the view tiers and shares its baseline
// with the live route that draws them. Recording the search's run there told
// the next live frame the fill was already in the retained pixels: that frame
// Loaded the match's cells unfilled and drew the pill's ring around them, two
// yellow ends with plain text between. A capture that bakes no match records
// none, so the live frame after it owes the match's rows.
TEST(aFrameBuiltWithoutTheFillLeavesTheMatchRowsOwed)
{
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    feed(*t, "\x1b[1;1Hother\x1b[3;1Hneedle");
    UbiquittyTest::SearchFixture search(*t);
    REQUIRE(search.find("needle") == 1);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    REQUIRE_MSG(baseline.matchRow0 == 2 && baseline.matchRow1 == 2, "a live frame painted the match's fill on its row");

    RetainedCaptureRequest boundary;
    boundary.rows = t->rows;
    boundary.columns = t->columns;
    boundary.isFocusedMatchBaked = false;
    feed(*t, "\x1b[1;1Hedited");
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    const RetainedCapture captured = planRetainedCapture(*t->grid, *t, baseline, boundary, rowDirty);
    CHECK_MSG(captured.damage.canReuse, "the boundary frame is a Delta over the live one");
    CHECK_MSG(rowDirty[0] == 1, "it repaints the row the program wrote");
    CHECK_MSG(rowDirty[2] == 1, "and the match's row, whose fill it does not draw");
    CHECK_MSG(baseline.matchRow0 > baseline.matchRow1, "and records that it painted no match");

    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    const RetainedFramePlan live = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(live.shouldLoadRetainedTarget, "the live frame after it keeps the target");
    CHECK_MSG(rowDirty[2] == 1, "and repaints the match's row, so the fill is under the ring it draws");
    CHECK_MSG(rowDirty[0] == 0, "and not the row the boundary frame already drew");

    // The control: the live route's own capture bakes the match, so the frame
    // after it owes the row nothing.
    std::ranges::fill(rowDirty, std::uint8_t { 0 });
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(rowDirty[2] == 0, "a match the last frame painted owes no row");
    CHECK(RetainedCaptureRequest {}.isFocusedMatchBaked);
}

// One renderer drawing two panes of one size keeps one target and one
// baseline. Two fresh panes fed alike count their damage alike, so the
// generation one pane's frame leaves is the generation the other stands at:
// a plan that read only the count would Load the first pane's rows onto the
// second. The baseline names the grid it drained, and no other grid's frame
// trusts it.
TEST(aBaselineDrainedFromOneGridVouchesForNoOther)
{
    auto left = makeTerminal();
    auto right = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(left->rows), 0);
    RetainedFrameRequest request;
    request.rows = left->rows;
    request.columns = left->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * left->rows;
    feed(*left, "\x1b[1;1Hleft pane");
    feed(*right, "\x1b[1;1Hright pane");
    (void)planRetainedFrame(*left, baseline, request, rowDirty);
    (void)planRetainedFrame(*right, baseline, request, rowDirty);
    feed(*left, "\x1b[3;1Hleft edit");
    REQUIRE_MSG(left->grid->currentDamageGeneration() == baseline.generation,
        "the panes stand at one count, the case a count alone cannot tell apart");
    const RetainedFramePlan plan = planRetainedFrame(*left, baseline, request, rowDirty);
    CHECK_MSG(!plan.shouldLoadRetainedTarget, "the left pane does not Load the target the right pane drew");
    CHECK_MSG(!plan.refresh.canReuseRows, "and keeps none of its rows");
    const RetainedFramePlan again = planRetainedFrame(*left, baseline, request, rowDirty);
    CHECK_MSG(again.shouldLoadRetainedTarget, "the frame after, from the same pane, Loads its own target");
}

TEST(aCaptureConfinesBlinkToTheRowsThatCarryIt)
{
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    // A blinking cell on row 2 and plain text everywhere else. Blink changes
    // phase without a write, so nothing else puts row 2 in the dirty set.
    feed(*t, "\x1b[1;1Hplain\x1b[3;1H\x1b[5mblinking\x1b[0m\x1b[5;1Hplain");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    const RetainedFramePlan armed = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(!armed.damage.hasBlink, "the hazard is answered in the dirty set, not by refusing the frame");
    CHECK_MSG(armed.shouldLoadRetainedTarget, "so the frame keeps the pixels it already has");
    CHECK_MSG(rowDirty[2] == 1, "and the blinking row rebuilds");
    // The discriminating half: a whole-frame answer marks every row, and a
    // containment that marked every row would save nothing.
    CHECK_MSG(rowDirty[0] == 0 && rowDirty[4] == 0, "while the rows that do not blink keep theirs");

    feed(*t, "\x1b[H\x1b[2J");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    const RetainedFramePlan cleared = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(t->grid->hasBlinkHint(), "the sticky hint stands");
    CHECK_MSG(!cleared.damage.hasBlink, "while the erased screen carries no hazard at all");
    CHECK_MSG(cleared.shouldLoadRetainedTarget, "so partial frames come back");
    for (std::size_t row = 0; row < rowDirty.size(); ++row)
        CHECK_MSG(rowDirty[row] == 0 || static_cast<int>(row) == 0, "and no row is marked for a blink that is gone");

    t.reset();
}

// A frame's pixels can be invalidated by something no cell records — rebuilt
// faces, resized sheets, a retained buffer swapped for an empty one. Two
// states decide what such a frame paints, and resetting one without the other
// leaves the combination that paints nothing at all.
TEST(anInvalidatedFrameIsRedrawnWholeThoughNoCellChanged)
{
    auto t = makeTerminal();
    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    feed(*t, "painted");
    (void)planRetainedFrame(*t, baseline, request, rowDirty);

    // The control: a settled screen names no row, which is what makes the
    // arm below a statement about the invalidation rather than about a
    // terminal that reports everything dirty anyway.
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    int settled = 0;
    for (const std::uint8_t row : rowDirty)
        settled += row != 0 ? 1 : 0;
    CHECK_MSG(settled == 0, "a screen nothing touched names no dirty row");

    invalidateRetainedFrame(*t, baseline);
    const RetainedFramePlan redrawn = planRetainedFrame(*t, baseline, request, rowDirty);
    int marked = 0;
    for (const std::uint8_t row : rowDirty)
        marked += row != 0 ? 1 : 0;
    CHECK_MSG(marked == t->rows, "an invalidated frame names every row");
    CHECK_MSG(!redrawn.shouldLoadRetainedTarget, "and refuses the target it no longer trusts");

    // Resetting the baseline alone is the half that draws nothing: the target
    // is refused, so the pass clears, and a drain that still trusts its shadow
    // names no row to paint over the clear.
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    baseline = RetainedFrameBaseline();
    (void)planRetainedFrame(*t, baseline, request, rowDirty);
    int half = 0;
    for (const std::uint8_t row : rowDirty)
        half += row != 0 ? 1 : 0;
    CHECK_MSG(half < t->rows, "which is why the two invalidations are one call");

    t.reset();
}

// Scrolled back, the rows on display are outside the live-screen reading, so
// the hazard follows the hint rather than the screen: a blink cell that has
// left the tail must not freeze mid-phase in retained pixels.
TEST(aScrolledBackCaptureKeepsTheBlinkHazard)
{
    auto t = makeTerminal();
    feed(*t, "\x1b[5mblinking\x1b[0m");
    for (int line = 0; line < 12; ++line)
        feed(*t, "\r\nplain");
    CHECK_MSG(Ubiquitty::Screen::scrollbackCount(t.get()) > 0, "the blink rows left the tail");
    const Cell* cells = Ubiquitty::Screen::visibleCell(t.get(), 0, 0, 0);
    CHECK_MSG(!Ubiquitty::Rendering::anyCellBlinks(cells, t->rows, t->columns), "and the live screen is clean");

    RetainedFrameBaseline baseline;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    RetainedFrameRequest request;
    request.rows = t->rows;
    request.columns = t->columns;
    request.isTargetMatching = true;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 16 * t->rows;

    const RetainedFramePlan tail = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(!tail.damage.hasBlink, "at the tail the clean screen lowers the hazard");

    request.viewOffset = 2;
    const RetainedFramePlan scrolled = planRetainedFrame(*t, baseline, request, rowDirty);
    CHECK_MSG(scrolled.damage.hasBlink, "scrolled back it follows the hint");

    t.reset();
}

// Two walks over one shadow: the run walk a glyph backend needs, and the row
// walk a consumer that only wants which rows changed uses. They must find the
// same rows and leave the shadow in the same place, or a frontend on one and a
// snapshot on the other disagree about what a frame owes.
//
// The corpus is where they could most easily part: rows split into many runs by
// pen changes, rows dirty at one column, rows clean, a selection that moved,
// and a full redraw.
TEST(theRowWalkFindsWhatTheRunWalkFinds)
{
    struct Scene {
        const char* name;
        const char* stream;
    };
    const Scene scenes[] = {
        { "one cell", "\x1b[2;3HX" },
        { "a plain row", "\x1b[1;1Hplain text across the row" },
        { "many pens in one row", "\x1b[3;1H\x1b[31ma\x1b[32mb\x1b[33mc\x1b[34md\x1b[35me\x1b[36mf\x1b[0mg" },
        { "several rows", "\x1b[1;1Hone\x1b[3;1Hthree\x1b[5;1Hfive" },
        { "nothing", "" },
    };
    // A selection that moved dirties cells no write touched, which is the one
    // thing a two-array scan of the cells cannot see.
    constexpr int selectionScene = 5;

    int compared = 0;
    int scenesWithDirtyRows = 0;
    for (int index = 0; index <= selectionScene; ++index) {
        const Scene& scene = index < selectionScene ? scenes[index] : scenes[4];
        auto runGrid = makeTerminal();
        auto rowGrid = makeTerminal();
        // Both grids start from a drained state, so what follows is the change.
        std::vector<std::uint8_t> priming(static_cast<std::size_t>(runGrid->rows), 0);
        (void)drainFrameDamage(*runGrid->grid, freshEligibility(*runGrid), priming);
        (void)drainFrameDamage(*rowGrid->grid, freshEligibility(*rowGrid), priming);

        feed(*runGrid, scene.stream);
        feed(*rowGrid, scene.stream);
        if (index == selectionScene) {
            runGrid->grid->markSelectionFlipped(Ubiquitty::Row(2), Ubiquitty::Column(1), Ubiquitty::Column(4));
            rowGrid->grid->markSelectionFlipped(Ubiquitty::Row(2), Ubiquitty::Column(1), Ubiquitty::Column(4));
        }

        // The run walk, driven here rather than through the drain, which takes
        // the row walk.
        std::vector<std::uint8_t> fromRuns(static_cast<std::size_t>(runGrid->rows), 0);
        {
            auto session = runGrid->grid->beginPaint();
            Ubiquitty::PaintRun run;
            while (session.nextDirtyRun(&run)) {
                const int row = run.row.value();
                if (row >= 0 && row < runGrid->rows)
                    fromRuns[static_cast<std::size_t>(row)] = 1;
                session.ackRun(run);
            }
        }

        std::vector<std::uint8_t> fromRows(static_cast<std::size_t>(rowGrid->rows), 0);
        {
            auto session = rowGrid->grid->beginPaint();
            Ubiquitty::Row dirtyRow { 0 };
            while (session.nextDirtyRow(&dirtyRow)) {
                const int row = dirtyRow.value();
                if (row >= 0 && row < rowGrid->rows)
                    fromRows[static_cast<std::size_t>(row)] = 1;
                session.ackRow(dirtyRow);
            }
        }
        CHECK_MSG(fromRuns == fromRows, index == selectionScene ? "a selection that moved" : scene.name);
        if (std::ranges::count(fromRuns, std::uint8_t { 1 }) > 0)
            ++scenesWithDirtyRows;

        // And both shadows are where the walk left them: a second drain over an
        // unchanged grid finds nothing, on either side.
        std::vector<std::uint8_t> againRuns(static_cast<std::size_t>(runGrid->rows), 0);
        std::vector<std::uint8_t> againRows(static_cast<std::size_t>(rowGrid->rows), 0);
        {
            auto session = runGrid->grid->beginPaint();
            Ubiquitty::PaintRun run;
            while (session.nextDirtyRun(&run)) {
                againRuns[static_cast<std::size_t>(run.row.value())] = 1;
                session.ackRun(run);
            }
        }
        {
            auto session = rowGrid->grid->beginPaint();
            Ubiquitty::Row dirtyRow { 0 };
            while (session.nextDirtyRow(&dirtyRow)) {
                againRows[static_cast<std::size_t>(dirtyRow.value())] = 1;
                session.ackRow(dirtyRow);
            }
        }
        CHECK_MSG(std::ranges::count(againRuns, std::uint8_t { 1 }) == 0, scene.name);
        CHECK_MSG(std::ranges::count(againRows, std::uint8_t { 1 }) == 0, scene.name);

        ++compared;
        runGrid.reset();
        rowGrid.reset();
    }
    CHECK_EQ(compared, 6);
    // Guards a corpus where nothing was dirty: two walks that find nothing agree
    // about everything.
    CHECK_MSG(scenesWithDirtyRows >= 5, "too few scenes dirtied a row for the agreement to mean anything");
}

TEST(testWrittenRowsComeBackDirty)
{
    auto t = makeTerminal();

    // The resize that built the terminal dirtied every row; drain that first,
    // so the second drain sees only what the write touched.
    std::vector<std::uint8_t> rowDirty(6, 0);
    const FrameDamage construction = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);
    CHECK_MSG(!construction.canReuse, "an uninitialized buffer never reuses");

    feed(*t, "hello");
    std::ranges::fill(rowDirty, 0xFF); // stale marks, the drain must clear them
    (void)drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    CHECK_MSG(rowDirty[0] == 1, "the written row is dirty");
    CHECK_MSG(rowDirty[5] == 0, "an untouched row is not, so the drain cleared the stale marks");

    t.reset();
}

TEST(testMatchedBaselineReuses)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    std::vector<std::uint8_t> rowDirty(6, 0);
    const FrameDamage first = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    feed(*t, "!");
    CaptureEligibility eligibility = freshEligibility(*t);
    eligibility.bufferInitialized = true;
    eligibility.bufferGeneration = first.generationAfterDrain;
    eligibility.generationBeforeDrain = t->grid->currentDamageGeneration();
    const FrameDamage second = drainFrameDamage(*t->grid, eligibility, rowDirty);

    CHECK_MSG(second.canReuse, "a buffer that drained the previous generation may update incrementally");
    CHECK(second.generationAfterDrain >= first.generationAfterDrain);

    t.reset();
}

TEST(testGenerationGapForcesFullReread)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    std::vector<std::uint8_t> rowDirty(6, 0);
    const FrameDamage first = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    // A peer buffer drains in between: this buffer's baseline is one behind.
    feed(*t, "peer");
    (void)drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    feed(*t, "!");
    CaptureEligibility eligibility = freshEligibility(*t);
    eligibility.bufferInitialized = true;
    eligibility.bufferGeneration = first.generationAfterDrain;
    eligibility.generationBeforeDrain = t->grid->currentDamageGeneration();
    const FrameDamage second = drainFrameDamage(*t->grid, eligibility, rowDirty);

    CHECK_MSG(!second.canReuse, "a generation gap means the peer owns runs this buffer never applied");

    t.reset();
}

TEST(testScrollLeavesTheExposedRowDirty)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    (void)drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    // Enough lines to scroll a 6-row screen. Whether the drain reports the
    // shift as a pending scroll or coalesces it into row damage, the invariant
    // a blit relies on is the same: the newly exposed bottom row is dirty.
    feed(*t, "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh\r\n");
    const FrameDamage damage = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);

    CHECK_MSG(rowDirty[5] == 1, "the scroll-exposed bottom row repaints");
    if (damage.scrollBottom >= 0) {
        CHECK(damage.scrollTop >= 0);
        CHECK(damage.scrollTop <= damage.scrollBottom);
        CHECK(damage.scrollBottom < 6);
        CHECK_MSG(damage.scrollDelta != 0, "a reported scroll region carries its shift");
    }

    t.reset();
}

TEST(testDegenerateInputsAreRefused)
{
    auto t = makeTerminal();
    const FrameDamage nullDirty = drainFrameDamage(*t->grid, freshEligibility(*t), std::span<std::uint8_t> {});
    CHECK(!nullDirty.canReuse);
    CHECK(nullDirty.scrollBottom == -1);

    std::vector<std::uint8_t> rowDirty(6, 0);
    const FrameDamage noRows = drainFrameDamage(*t->grid, freshEligibility(*t), std::span<std::uint8_t> {});
    CHECK(!noRows.canReuse);

    t.reset();
}

// Every cell a DEC rectangle operation writes goes through the damage-tracked
// mutators, so the shadow diff already yields exactly the changed cells. An
// operation that then invalidates the shadow throws that away: a four-cell fill
// repaints the window.
//
// The rectangle is rows 2..3 of six, so the rows outside it must come back
// clean — and the incremental read must still agree with a full one byte for
// byte, which is what says the containment is correct and not merely smaller.
TEST(testRectangleOperationsDamageOnlyTheirRows)
{
    struct RectangleCase {
        const char* name;
        const char* sequence;
    };
    // DECFRA, DECERA, DECSERA, DECCRA, DECCARA, DECRARA — every operation in
    // Rect.cpp that ended with a whole-screen invalidation.
    const RectangleCase cases[] = {
        { "DECFRA", "\x1b[88;2;2;3;4$x" },
        { "DECERA", "\x1b[2;2;3;4$z" },
        { "DECSERA", "\x1b[2;2;3;4${" },
        { "DECCRA", "\x1b[5;1;6;3;1;2;2;1$v" },
        { "DECCARA", "\x1b[2;2;3;4;1$r" },
        { "DECRARA", "\x1b[2;2;3;4;7$t" },
    };

    for (const RectangleCase& c : cases) {
        auto t = makeTerminal();
        std::vector<std::uint8_t> rowDirty(6, 0);
        std::vector<Cell> retained(6 * 20);
        std::vector<std::uint8_t> attributes(6, 0);
        std::vector<std::uint8_t> marks(6, 0);

        // Something on every row, so a row that comes back clean is a row the
        // operation genuinely left alone rather than one that was never written.
        feed(*t, "\x1b[H");
        for (int row = 0; row < 6; ++row)
            feed(*t, "abcdefghij\r\n");

        const FrameDamage first = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);
        CapturedRowRefresh prime;
        prime.canReuseRows = false;
        (void)Rendering::refreshCapturedRowsWithStatistics(*t, prime, retained, attributes, marks);

        feed(*t, c.sequence);

        CaptureEligibility eligibility = freshEligibility(*t);
        eligibility.bufferInitialized = true;
        eligibility.bufferGeneration = first.generationAfterDrain;
        eligibility.generationBeforeDrain = t->grid->currentDamageGeneration();
        const FrameDamage second = drainFrameDamage(*t->grid, eligibility, rowDirty);
        CHECK_MSG(second.canReuse, c.name);

        const auto dirtyRows = std::ranges::count(rowDirty, std::uint8_t { 1 });
        CHECK_MSG(dirtyRows > 0, c.name);
        CHECK_MSG(dirtyRows < 6, c.name);

        CapturedRowRefresh refresh;
        refresh.canReuseRows = second.canReuse;
        refresh.scrollTop = second.scrollTop;
        refresh.scrollBottom = second.scrollBottom;
        refresh.scrollDelta = second.scrollDelta;
        refresh.rowDirty = rowDirty;
        (void)Rendering::refreshCapturedRowsWithStatistics(*t, refresh, retained, attributes, marks);

        std::vector<Cell> fresh(6 * 20);
        std::vector<std::uint8_t> freshAttributes(6, 0);
        std::vector<std::uint8_t> freshMarks(6, 0);
        CapturedRowRefresh full;
        full.canReuseRows = false;
        (void)Rendering::refreshCapturedRowsWithStatistics(*t, full, fresh, freshAttributes, freshMarks);

        CHECK_MSG(std::memcmp(retained.data(), fresh.data(), retained.size() * sizeof(Cell)) == 0, c.name);
        CHECK_MSG(attributes == freshAttributes, c.name);
        CHECK_MSG(marks == freshMarks, c.name);
        t.reset();
    }
}

TEST(testIncrementalRefreshEqualsFullReread)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);

    // Prime the retained copy with a full read, as a first frame does.
    std::vector<Cell> retained(6 * 20);
    std::vector<std::uint8_t> attributes(6, 0);
    std::vector<std::uint8_t> marks(6, 0);
    const FrameDamage first = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);
    CapturedRowRefresh prime;
    prime.canReuseRows = false;
    const FrameCopyStatistics primeStatistics = Rendering::refreshCapturedRowsWithStatistics(
        *t, prime, retained, attributes, marks);
    CHECK(primeStatistics.rowsInspected == 6);
    CHECK(primeStatistics.rowsCopied == 6);
    CHECK(primeStatistics.cellBytesCopied == retained.size() * sizeof(Cell));

    // Mutate one row, drain with a matched baseline, refresh incrementally.
    feed(*t, "changed");
    CaptureEligibility eligibility = freshEligibility(*t);
    eligibility.bufferInitialized = true;
    eligibility.bufferGeneration = first.generationAfterDrain;
    eligibility.generationBeforeDrain = t->grid->currentDamageGeneration();
    const FrameDamage second = drainFrameDamage(*t->grid, eligibility, rowDirty);
    CHECK_MSG(second.canReuse, "the incremental arm is the one exercised");

    CapturedRowRefresh refresh;
    refresh.canReuseRows = second.canReuse;
    refresh.scrollTop = second.scrollTop;
    refresh.scrollBottom = second.scrollBottom;
    refresh.scrollDelta = second.scrollDelta;
    refresh.rowDirty = rowDirty;
    const FrameCopyStatistics incrementalStatistics = Rendering::refreshCapturedRowsWithStatistics(
        *t, refresh, retained, attributes, marks);
    CHECK(incrementalStatistics.rowsInspected == 6);
    CHECK(incrementalStatistics.rowsCopied
        == static_cast<std::uint32_t>(std::ranges::count(rowDirty, std::uint8_t { 1 })));
    CHECK(incrementalStatistics.cellBytesCopied
        == static_cast<std::uint64_t>(incrementalStatistics.rowsCopied) * 20 * sizeof(Cell));

    // The invariant the seam is held to: reuse changes the work, never the
    // result. A full re-read into fresh storage must agree byte for byte.
    std::vector<Cell> fresh(6 * 20);
    std::vector<std::uint8_t> freshAttributes(6, 0);
    std::vector<std::uint8_t> freshMarks(6, 0);
    CapturedRowRefresh full;
    full.canReuseRows = false;
    const FrameCopyStatistics fullStatistics = Rendering::refreshCapturedRowsWithStatistics(
        *t, full, fresh, freshAttributes, freshMarks);
    CHECK(fullStatistics.rowsCopied == 6);

    CHECK_MSG(std::memcmp(retained.data(), fresh.data(), retained.size() * sizeof(Cell)) == 0,
        "incremental and full reads produce one grid");
    CHECK(attributes == freshAttributes);
    CHECK(marks == freshMarks);

    t.reset();
}

TEST(testMismatchedShapesAreRefusedWhole)
{
    auto t = makeTerminal();
    std::vector<Cell> cells(6 * 20);
    const Cell untouched = cells[0];
    std::vector<std::uint8_t> attributes(6, 0);
    std::vector<std::uint8_t> shortMarks(5, 0); // one row short: refused, not read past
    CapturedRowRefresh refresh;
    Rendering::refreshCapturedRows(*t, refresh, cells, attributes, shortMarks);
    CHECK_MSG(std::memcmp(&cells[0], &untouched, sizeof(Cell)) == 0,
        "a buffer whose shape disagrees with itself is left alone");

    t.reset();
}

TEST(testScrollBlitPlanCopiesThreeBands)
{
    FrameDamage damage;
    damage.scrollTop = 2;
    damage.scrollBottom = 7;
    damage.scrollDelta = 2;
    // 10 rows of 16px, origin at 4px, texture 168px.
    const ScrollBlitPlan plan = scrollBlitPlan(damage, 10, 16, 4, 168);
    CHECK_MSG(plan.bandCount == 3, "above, shifted region, below");
    CHECK(plan.bands[0].sourceYInPixels == 4);
    CHECK(plan.bands[0].destinationYInPixels == 4);
    CHECK(plan.bands[0].heightInPixels == 2 * 16);
    // Region rows 4..7 move to 2..5: source row 4 = 4 + 4*16.
    CHECK(plan.bands[1].sourceYInPixels == 4 + 4 * 16);
    CHECK(plan.bands[1].destinationYInPixels == 4 + 2 * 16);
    CHECK(plan.bands[1].heightInPixels == 4 * 16);
    // Rows 8..9 carry over unshifted.
    CHECK(plan.bands[2].sourceYInPixels == 4 + 8 * 16);
    CHECK(plan.bands[2].destinationYInPixels == 4 + 8 * 16);
    CHECK(plan.bands[2].heightInPixels == 2 * 16);
}

TEST(testScrollBlitPlanKeepsPinnedBandsWhenTheShiftEatsTheRegion)
{
    // A DECSTBM region scrolled by its own height (or more) exposes every
    // region row — the drain marks them dirty — but the pinned rows above
    // and below carry over unchanged, and only this plan copies them. The
    // clamped PendingScroll delta makes shift == regionRows reachable, so
    // the boundary is the case, not the overshoot alone.
    for (int delta : { 4, 7 }) {
        FrameDamage regionEaten;
        regionEaten.scrollTop = 2;
        regionEaten.scrollBottom = 5; // regionRows = 4
        regionEaten.scrollDelta = delta;
        const ScrollBlitPlan plan = scrollBlitPlan(regionEaten, 10, 16, 0, 160);
        CHECK_MSG(plan.bandCount == 2, "above and below, no region band");
        CHECK(plan.bands[0].sourceYInPixels == 0);
        CHECK(plan.bands[0].destinationYInPixels == 0);
        CHECK(plan.bands[0].heightInPixels == 2 * 16);
        CHECK(plan.bands[1].sourceYInPixels == 6 * 16);
        CHECK(plan.bands[1].destinationYInPixels == 6 * 16);
        CHECK(plan.bands[1].heightInPixels == 4 * 16);
    }
}

TEST(testScrollBlitPlanRefusesWhatFitsNoPlan)
{
    FrameDamage none;
    CHECK(scrollBlitPlan(none, 10, 16, 0, 160).bandCount == 0);

    FrameDamage whole;
    whole.scrollTop = 0;
    whole.scrollBottom = 9;
    whole.scrollDelta = 12; // shift past the whole screen: no band keeps a row
    CHECK(scrollBlitPlan(whole, 10, 16, 0, 160).bandCount == 0);

    FrameDamage clamped;
    clamped.scrollTop = 0;
    clamped.scrollBottom = 9;
    clamped.scrollDelta = 1;
    // A texture shorter than the grid clamps the last band instead of copying
    // past the edge.
    const ScrollBlitPlan plan = scrollBlitPlan(clamped, 10, 16, 0, 150);
    for (int i = 0; i < plan.bandCount; ++i) {
        CHECK(plan.bands[i].sourceYInPixels + plan.bands[i].heightInPixels <= 150);
        CHECK(plan.bands[i].destinationYInPixels + plan.bands[i].heightInPixels <= 150);
    }
}

TEST(testRetainedTargetVerdictRanksEveryHazard)
{
    FrameDamage coherent;
    coherent.canReuse = true;
    RetainedTargetEligibility eligibility;
    eligibility.isTargetMatching = true;
    eligibility.rows = 6;
    eligibility.rowDirtyCount = 6;
    CHECK(canReuseRetainedTarget(coherent, eligibility));

    // Each hazard alone forces the full frame.
    FrameDamage stale;
    CHECK(!canReuseRetainedTarget(stale, eligibility));
    auto hazard = eligibility;
    hazard.isTargetMatching = false;
    CHECK(!canReuseRetainedTarget(coherent, hazard));
    hazard = eligibility;
    hazard.hasBlink = true;
    CHECK(!canReuseRetainedTarget(coherent, hazard));
    hazard = eligibility;
    hazard.hasStatusLine = true;
    CHECK(!canReuseRetainedTarget(coherent, hazard));
    hazard = eligibility;
    hazard.rowDirtyCount = 5; // a short span reads as rows the drain never saw
    CHECK(!canReuseRetainedTarget(coherent, hazard));
}

TEST(testPlanRefusesAMismatchedSpan)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(4, 0); // four bytes for a six-row frame
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    const RetainedFramePlan plan = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK(!plan.shouldLoadRetainedTarget);
    CHECK_EQ(plan.blitPlan.bandCount, 0);
    CHECK_MSG(!baseline.isInitialized, "a refused plan advances no baseline");
    t.reset();
}

TEST(testPlanFirstFrameIsFullAndTheSecondLoads)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 96;

    feed(*t, "hello");
    const RetainedFramePlan first = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(!first.shouldLoadRetainedTarget, "an uninitialized baseline is a full frame");
    CHECK(baseline.isInitialized);

    feed(*t, "\x1b[3;1Hedited");
    request.isTargetMatching = true;
    const RetainedFramePlan second = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(second.shouldLoadRetainedTarget, "a coherent baseline with a matching target Loads");
    CHECK_MSG(rowDirty[2] == 1, "the edited row is dirty");
    CHECK_MSG(rowDirty[4] == 0, "an untouched row stays clean");
    CHECK_EQ(second.refresh.rowDirty.size(), rowDirty.size());
    t.reset();
}

TEST(testPlanConfinesAStatusLineToItsRow)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    request.isTargetMatching = true;

    (void)planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    t->statusLineType = 1;
    const RetainedFramePlan plan = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(plan.damage.canReuse, "the cells stay coherent under a status line");
    // The indicator carries the cursor's position, which moves without that row
    // being written — so the row is dirty every frame the status line is up.
    CHECK_MSG(rowDirty[5] == 1, "the status row is the last row, and it rebuilds");
    CHECK_MSG(plan.shouldLoadRetainedTarget, "while the rest of the frame keeps its pixels");

    // The mutation arm: with no status line that row is not dirty, so the mark
    // above is the status line's and not a mark every frame carries.
    t->statusLineType = 0;
    (void)planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(rowDirty[5] == 0, "and it is clean once the status line goes");
    t.reset();
}

// A frame that does not keep the retained pixels must re-read every row.
//
// The plan answers two questions, and they are one decision wearing two names:
// whether the frontend loads the retained texture, and whether the builder may
// trust the dirty set and re-read only the rows in it. A plan that says clear
// the target and trust the dirty set draws the changed rows onto a cleared
// texture — every clean row is gone, which on screen is text vanishing as the
// reader types, and coming back on whichever later frame happens to rebuild.
//
// The frontend's own refusal is the reachable case: a renderer whose sheets
// need re-baselining, or whose target no longer matches the drawable, hands
// `isTargetMatching` false while the cells themselves stayed perfectly
// coherent — so the damage still says the rows may be trusted.
TEST(testAFrameThatClearsItsTargetRereadsEveryRow)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    request.isTargetMatching = true;

    // A baseline, then one written cell: the damage is coherent and partial,
    // which is the state a reuse is for.
    (void)planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    Ubiquitty::processBytes(t.get(), "x");
    const RetainedFramePlan reusing = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(reusing.damage.canReuse, "the cells stayed coherent");
    CHECK_MSG(reusing.shouldLoadRetainedTarget, "so the frame keeps its pixels");
    CHECK_MSG(reusing.refresh.canReuseRows, "and the builder may re-read only the dirty rows");

    // The same coherent damage, with the frontend refusing the target. The
    // pixels are about to be cleared, so nothing on screen survives to be
    // trusted — every row has to be read again.
    Ubiquitty::processBytes(t.get(), "y");
    request.isTargetMatching = false;
    const RetainedFramePlan clearing = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    CHECK_MSG(clearing.damage.canReuse, "the cells are still coherent, which is what makes this reachable");
    CHECK_MSG(!clearing.shouldLoadRetainedTarget, "the frontend refused the target");
    CHECK_MSG(!clearing.refresh.canReuseRows,
        "so the builder re-reads every row rather than drawing a few onto a cleared texture");
    t.reset();
}

TEST(testPlanDemotesAnUnplannableScroll)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 96;

    (void)planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    // More lines than the screen holds: the coalesced shift spans the whole
    // region, no band survives, and the plan demotes to a full rebuild
    // rather than Load a target no copy can carry forward.
    feed(*t, "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh\r\ni\r\nj\r\nk\r\nl\r\n");
    request.isTargetMatching = true;
    const RetainedFramePlan plan = planRetainedFrame(*t->grid, *t, baseline, request, rowDirty);
    if (plan.damage.scrollDelta != 0 && plan.blitPlan.bandCount == 0)
        CHECK_MSG(!plan.shouldLoadRetainedTarget, "a scroll with no valid plan rebuilds in full");
    t.reset();
}

// The margins around the grid are never repainted once a target is retained: a
// partial frame Loads them and rebuilds only dirty rows. So ink that overhangs
// the content rectangle — a combining stack above row zero, a descender below
// the last row, a left side bearing on column zero — would stay in the margin
// for the life of the texture. Every retaining backend confines its grid passes
// to this rectangle, and a backend that derived it itself would differ in the
// last pixel, which is a column of ink present in one arm and absent in the
// other on the one edge no frame repaints.
TEST(testRetainedContentRectConfinesTheGrid)
{
    // A 200x100 target with the grid inset by 8 and a whole-pixel cell.
    const RetainedContentRect fits = retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 200, 100 });
    CHECK(fits.x == 8 && fits.y == 8);
    CHECK(fits.width == 180 && fits.height == 80);
    CHECK(!fits.isEmpty());

    // The overhang reaches past the grid on every side by the slot margin, so
    // a row-start glyph with a left bearing — every healed `w` in Monaspace —
    // draws into the padding instead of ending flat at the cell edge; the
    // rebuilt row's margin strips keep that reach from going stale.
    const RetainedContentRect reaching = retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 200, 100 }, Ubiquitty::IntSize { 3, 2 });
    CHECK(reaching.x == 5 && reaching.y == 6);
    CHECK(reaching.width == 186 && reaching.height == 84);
    // A reach past the target's edge clamps on that side alone.
    const RetainedContentRect edge = retainedContentRect(Ubiquitty::FloatPoint { 2.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 200, 100 }, Ubiquitty::IntSize { 3, 0 });
    CHECK(edge.x == 0 && edge.width == 185);

    // A fractional advance leaves the last column's right edge inside the next
    // pixel, so the extent ceils. Flooring would clip the ink the grid owns.
    const RetainedContentRect fractional = retainedContentRect(Ubiquitty::FloatPoint { 0.0f, 0.0f }, Ubiquitty::FloatSize { 10.5f, 20.25f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 400, 200 });
    CHECK(fractional.width == 189);
    CHECK(fractional.height == 81);

    // Clamped rather than overflowing: a grid wider than its target gives a
    // rectangle that ends at the target's edge.
    const RetainedContentRect clamped = retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 100, 40 }, Ubiquitty::IntSize { 200, 100 });
    CHECK(clamped.x == 8 && clamped.width == 192);
    CHECK(clamped.y == 8 && clamped.height == 92);

    // A degenerate grid or target is empty rather than negative — a scissor
    // rejects an empty rect, so the caller has to be able to ask.
    CHECK(retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 0 }, Ubiquitty::IntSize { 200, 100 }).isEmpty());
    CHECK(retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 0, 4 }, Ubiquitty::IntSize { 200, 100 }).isEmpty());
    CHECK(retainedContentRect(Ubiquitty::FloatPoint { 8.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 0, 100 }).isEmpty());
    // An origin past the target's edge leaves no width to draw into.
    CHECK(retainedContentRect(Ubiquitty::FloatPoint { 300.0f, 8.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 18, 4 }, Ubiquitty::IntSize { 200, 100 }).isEmpty());

    // A negative origin reads as zero: the grid starts at the target's edge
    // rather than at a coordinate a scissor cannot express.
    const RetainedContentRect negative = retainedContentRect(Ubiquitty::FloatPoint { -4.0f, -4.0f }, Ubiquitty::FloatSize { 10.0f, 20.0f }, Ubiquitty::GridSize { 2, 2 }, Ubiquitty::IntSize { 200, 100 });
    CHECK(negative.x == 0 && negative.y == 0);
}

// The verifier has to be able to fail, or a quiet log from it says nothing.
// A correct buffer reports no mismatch; the same buffer with one cell moved
// out from under it reports that exact cell. Without the second arm this test
// passes against a verifier that returns "clean" unconditionally.
TEST(testVerifyCapturedRowsFindsAStaleCell)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    std::vector<Cell> retained(6 * 20);
    std::vector<std::uint8_t> attributes(6, 0);
    std::vector<std::uint8_t> marks(6, 0);
    std::vector<Cell> scratch(6 * 20);
    std::vector<std::uint8_t> scratchAttributes(6, 0);
    std::vector<std::uint8_t> scratchMarks(6, 0);

    const FrameDamage first = drainFrameDamage(*t->grid, freshEligibility(*t), rowDirty);
    CapturedRowRefresh prime;
    prime.canReuseRows = false;
    Rendering::refreshCapturedRows(*t, prime, retained, attributes, marks);

    feed(*t, "changed");
    CaptureEligibility eligibility = freshEligibility(*t);
    eligibility.bufferInitialized = true;
    eligibility.bufferGeneration = first.generationAfterDrain;
    eligibility.generationBeforeDrain = t->grid->currentDamageGeneration();
    const FrameDamage second = drainFrameDamage(*t->grid, eligibility, rowDirty);
    CapturedRowRefresh refresh;
    refresh.canReuseRows = second.canReuse;
    refresh.scrollTop = second.scrollTop;
    refresh.scrollBottom = second.scrollBottom;
    refresh.scrollDelta = second.scrollDelta;
    refresh.rowDirty = rowDirty;
    Rendering::refreshCapturedRows(*t, refresh, retained, attributes, marks);

    const Rendering::CapturedRowMismatch clean = Rendering::verifyCapturedRows(
        *t, refresh, retained, scratch, scratchAttributes, scratchMarks);
    CHECK_MSG(clean.row < 0 && clean.cellCount == 0,
        "an incrementally refreshed buffer equals the full re-read it promises");

    // The mutation: one cell the refresh is no longer told about, which is the
    // shape of every staleness a retained buffer can hold.
    retained[3 * 20 + 7].codePoint = retained[3 * 20 + 7].codePoint == 'Z' ? 'Y' : 'Z';
    const Rendering::CapturedRowMismatch stale = Rendering::verifyCapturedRows(
        *t, refresh, retained, scratch, scratchAttributes, scratchMarks);
    CHECK_MSG(stale.row == 3 && stale.column == 7 && stale.cellCount == 1,
        "a stale cell is named by row and column, or the check cannot report one");
}

// An image is painted into the retained pixels with the cells, outside the row
// damage. A frame with a picture on the viewport rebuilds whole, and so does
// the frame after the picture leaves: the rows it covered were never written,
// and a Load would keep it standing where it was while the text moved on, with
// the next picture landing on top of it. A picture that has scrolled into
// history covers no row on display and costs the frame nothing.
TEST(testPlanRebuildsWholeWhileAnImageShowsAndOnceAfterItLeaves)
{
    auto t = makeTerminal();
    std::vector<std::uint8_t> rowDirty(6, 0);
    RetainedFrameBaseline baseline;
    RetainedFrameRequest request;
    request.rows = 6;
    request.columns = 20;
    request.cellHeightInPixels = 16;
    request.textureHeightInPixels = 96;
    request.isTargetMatching = true;
    const auto plan = [&] { return planRetainedFrame(*t->grid, *t, baseline, request, rowDirty).shouldLoadRetainedTarget; };
    const auto pixels = [] { return static_cast<unsigned char*>(std::malloc(4 * 4 * 4)); };

    feed(*t, "hello");
    (void)plan();
    feed(*t, "\x1b[2;1Hmore");
    CHECK_MSG(plan(), "text alone Loads");

    t->screens.active->cursorY = 3;
    CHECK(Image::storeRGBAImage(t.get(), pixels(), 4, 4, 2, 1)); // one row, row 3
    feed(*t, "\x1b[1;1Hx");
    CHECK_MSG(!plan(), "a picture on the viewport rebuilds the frame whole");
    feed(*t, "\x1b[1;1Hy");
    CHECK_MSG(!plan(), "and every frame it stays");

    Image::clearPlacementsInRows(*t, 3, 3);
    feed(*t, "\x1b[1;1Hz");
    CHECK_MSG(!plan(), "the frame after it leaves repaints the rows it covered");
    feed(*t, "\x1b[1;1Hw");
    CHECK_MSG(plan(), "then the retained rows serve again");

    // Placed, then scrolled above the top row before any frame showed it.
    t->screens.active->cursorY = 3;
    CHECK(Image::storeRGBAImage(t.get(), pixels(), 4, 4, 2, 1));
    Image::scrollPlacementsUp(*t, 4);
    feed(*t, "\x1b[1;1Hv");
    CHECK_MSG(plan(), "a picture in history is not on the viewport");
    t.reset();
}

} // namespace

UBIQUITTY_TEST_MAIN()
