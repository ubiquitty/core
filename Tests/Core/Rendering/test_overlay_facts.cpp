// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SearchFixture.h"

// test_overlay_facts.cpp — the search, trigger and hilite tiers a frame draws
// over a capture, from the facts the model's writer publishes
// (Core/Rendering/OverlayFacts.h, OverlaySpanGather.h). The reference is the
// live gathers over the terminal the capture froze: wherever the facts and the
// capture describe the same moment, the two must answer the same spans, row
// for row; across moments, the rows must land where the live gathers put them
// once the model has moved on.

#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/OverlayFacts.h"
#include "Core/Rendering/OverlaySpanGather.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryFacts.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Triggers.h"
#include "TerminalFixture.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;
using UbiquittyTest::feed;

namespace {

using TakenCapture = std::unique_ptr<FrameCapture, decltype([](FrameCapture* capture) { releaseFrameCapture(capture); })>;

struct Tiers {
    std::vector<SelectionRowSpan> focused;
    std::vector<SelectionRowSpan> other;
    std::vector<SelectionRowSpan> triggers;
    std::vector<SelectionRowSpan> hilite;

    friend bool operator==(const Tiers&, const Tiers&) = default;
};

CellMetrics unifontMetrics()
{
    UnifontRasterizer rasterizer(1, 0);
    return rasterizer.cellMetrics();
}

TakenCapture captureAt(const Terminal& terminal, int viewOffset)
{
    // A helper that returns cannot REQUIRE; a failed capture aborts here,
    // before any comparison reads it.
    TakenCapture capture { FrameCapture::make() };
    TerminalSnapshotOptions options;
    options.viewOffset = viewOffset;
    const bool didCapture = capture && capture->capture(terminal, options, unifontMetrics(), GlyphSlotGeometry {});
    if (!didCapture)
        std::abort();
    return capture;
}

// What a live read draws at `viewOffset`: the frontends hand the gathers the
// offset their frame resolved, which is the one a capture records.
Tiers liveTiers(const Terminal& terminal, int viewOffset)
{
    const int resolved = Screen::resolvedViewOffset(&terminal, viewOffset);
    Tiers tiers;
    collectSearchMatchRowSpans(terminal, resolved, tiers.focused);
    collectOtherSearchMatchRowSpans(terminal, resolved, tiers.other);
    collectTriggerHighlightRowSpans(terminal, resolved, tiers.triggers);
    collectHiliteRegionRowSpans(terminal, resolved, tiers.hilite);
    return tiers;
}

Tiers capturedTiers(const SearchOverlayFacts& search, const MarkOverlayFacts& marks, const FrameCapture& capture)
{
    Tiers tiers;
    tiers.focused.resize(maxSearchOverlaySpans);
    tiers.other.resize(maxSearchOverlaySpans);
    tiers.triggers.resize(maxTriggerOverlaySpans);
    tiers.hilite.resize(static_cast<std::size_t>(capture.rows()));
    const OverlaySpanCounts counts = overlaySpansOverCapture(search, marks, capture, tiers.focused, tiers.other, tiers.triggers, tiers.hilite);
    tiers.focused.resize(static_cast<std::size_t>(counts.focusedSearch));
    tiers.other.resize(static_cast<std::size_t>(counts.otherSearch));
    tiers.triggers.resize(static_cast<std::size_t>(counts.triggers));
    tiers.hilite.resize(static_cast<std::size_t>(counts.hilite));
    return tiers;
}

struct Gathered {
    SearchOverlayFacts search;
    MarkOverlayFacts marks;
};

std::unique_ptr<Gathered> gather(const Terminal& terminal)
{
    auto gathered = std::make_unique<Gathered>();
    gatherSearchOverlayFacts(terminal, gathered->search);
    gatherMarkOverlayFacts(terminal, gathered->marks);
    return gathered;
}

// Counts every tier the comparisons saw drawn, so a case cannot pass by
// comparing empty answers.
struct Coverage {
    std::size_t focused = 0;
    std::size_t other = 0;
    std::size_t triggers = 0;
    std::size_t hilite = 0;

    void add(const Tiers& tiers)
    {
        focused += tiers.focused.size();
        other += tiers.other.size();
        triggers += tiers.triggers.size();
        hilite += tiers.hilite.size();
    }
};

void checkSameMoment(const Terminal& terminal, int viewOffset, Coverage& coverage)
{
    const auto gathered = gather(terminal);
    const TakenCapture capture = captureAt(terminal, viewOffset);
    const Tiers live = liveTiers(terminal, viewOffset);
    const Tiers captured = capturedTiers(gathered->search, gathered->marks, *capture);
    CHECK_MSG(captured == live, ("view offset " + std::to_string(viewOffset)).c_str());
    coverage.add(live);
}

// History with matches spread through it, one wrapping across a row edge, a
// trigger highlight, and a hilite band.
void feedOverlayHistory(Terminal& terminal)
{
    REQUIRE(Triggers::addRuleSpec(&terminal, "ERROR", "highlight:red", CaseSensitivity::Sensitive));
    for (int line = 0; line < 30; ++line) {
        if (line % 7 == 2)
            feed(terminal, "alpha at line " + std::to_string(line) + "\r\n");
        else if (line % 7 == 5)
            feed(terminal, "an ERROR on " + std::to_string(line) + "\r\n");
        else if (line == 20)
            feed(terminal, "xxxxxxxxxxxxxxxxxalpha\r\n"); // 17 + 5 on 20 columns: wraps
        else
            feed(terminal, "filler " + std::to_string(line) + "\r\n");
    }
    feed(terminal, "alpha on the screen");
    terminal.modes.hiliteRegionValid = true;
    terminal.modes.hiliteStartX = 4;
    terminal.modes.hiliteStartY = 2;
    terminal.modes.hiliteFirstRow = 2;
    terminal.modes.hiliteLastRow = 4;
}

} // namespace

TEST(aCaptureDrawsThePublishedOverlaysTheLiveGathersDraw)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feedOverlayHistory(t);
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("alpha") >= 5);
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > 2 * t.rows);

    Coverage coverage;
    for (const int viewOffset : { 0, 1, 3, history / 2, history - 2, history, history + 7 })
        checkSameMoment(t, viewOffset, coverage);
    // Step the focus through every match, so the focused tier is compared at
    // the wrapped match and at the screen's.
    for (int step = 0; step < 6; ++step) {
        REQUIRE(search.step(Search::StepDirection::Older));
        for (const int viewOffset : { 0, history / 2, history })
            checkSameMoment(t, viewOffset, coverage);
    }
    CHECK(coverage.focused > 0);
    CHECK(coverage.other > 0);
    CHECK(coverage.triggers > 0);
    CHECK(coverage.hilite > 0);
}

TEST(theAlternateScreenDrawsThePublishedOverlaysTheLiveGathersDraw)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    feedOverlayHistory(t);
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > t.rows);
    feed(t, "\033[?1049h\033[4;27Hneedle\033[1;1Hneedle"); // the first wraps onto row 4
    t.modes.hiliteRegionValid = true;
    t.modes.hiliteStartX = 2;
    t.modes.hiliteStartY = 1;
    t.modes.hiliteFirstRow = 1;
    t.modes.hiliteLastRow = 3;
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("needle") == 2);

    // A stale scroll position from the primary screen addresses nothing here:
    // the capture and the live gathers both resolve it to the screen.
    Coverage coverage;
    for (const int viewOffset : { 0, 2, history })
        checkSameMoment(t, viewOffset, coverage);
    REQUIRE(search.step(Search::StepDirection::Older));
    checkSameMoment(t, history, coverage);
    CHECK(coverage.focused > 0);
    CHECK(coverage.other > 0);
    CHECK(coverage.hilite > 0);
}

TEST(rowsEvictedBetweenGatheringAndCaptureCarryTheMatches)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    setScrollbackLimit(&t, 12);
    // Matches only among the newest lines, so the rows evicted below are
    // filler and every match stays in the history on both sides.
    for (int line = 0; line < 40; ++line)
        feed(t, (line >= 30 && line % 3 == 0 ? "alpha " : "filler ") + std::to_string(line) + "\r\n");
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("alpha") == 4);
    const Screen::RowNumbering before = Screen::rowNumbering(&t);
    REQUIRE(before.historyRows > 8);
    const std::vector<int> viewOffsets { 0, 3, 6, before.historyRows };

    std::vector<TakenCapture> earlier;
    std::vector<Tiers> earlierLive;
    for (const int viewOffset : viewOffsets) {
        earlier.push_back(captureAt(t, viewOffset));
        earlierLive.push_back(liveTiers(t, viewOffset));
    }
    const auto gatheredEarlier = gather(t);

    // At the limit every appended row evicts one: the text keeps its place in
    // the history while every row index moves up by three.
    feed(t, "plain 1\r\nplain 2\r\nplain 3\r\n");
    const Screen::RowNumbering after = Screen::rowNumbering(&t);
    REQUIRE(after.historyRows == before.historyRows);
    REQUIRE(after.appendedRows == before.appendedRows + 3);
    const auto gatheredLater = gather(t);

    Coverage coverage;
    for (std::size_t index = 0; index < viewOffsets.size(); ++index) {
        const std::string label = "view offset " + std::to_string(viewOffsets[index]);
        // Newer facts over a held capture: the writer has published past the
        // frame the view still shows.
        CHECK_MSG(capturedTiers(gatheredLater->search, gatheredLater->marks, *earlier[index]) == earlierLive[index], label.c_str());
        // Older facts over a newer capture.
        const TakenCapture later = captureAt(t, viewOffsets[index]);
        CHECK_MSG(capturedTiers(gatheredEarlier->search, gatheredEarlier->marks, *later) == liveTiers(t, viewOffsets[index]), label.c_str());
        coverage.add(earlierLive[index]);
    }
    CHECK(coverage.focused > 0);
    CHECK(coverage.other > 0);

    // The control: read as if nothing were evicted, the newer facts land three
    // rows high on the held capture, so the comparisons above turned on the
    // translation.
    SearchOverlayFacts unmoved = gatheredLater->search;
    unmoved.numbering = earlier[0]->rowNumbering();
    bool didAnyDiffer = false;
    for (std::size_t index = 0; index < viewOffsets.size(); ++index)
        didAnyDiffer = didAnyDiffer || capturedTiers(unmoved, gatheredLater->marks, *earlier[index]) != earlierLive[index];
    CHECK(didAnyDiffer);
}

TEST(aRenumberedGridBetweenGatheringAndCaptureDrawsNoSearch)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feedOverlayHistory(t);
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("alpha") > 0);
    const auto gathered = gather(t);
    REQUIRE(gathered->search.focusedCount > 0);

    // A resize renumbers every row: nothing gathered before it names a row of
    // the grid after it, so the capture draws no search until the writer
    // gathers again.
    resize(t, 6, 13);
    const TakenCapture capture = captureAt(t, 0);
    const Tiers captured = capturedTiers(gathered->search, gathered->marks, *capture);
    CHECK(captured.focused.empty());
    CHECK(captured.other.empty());

    const auto regathered = gather(t);
    CHECK(capturedTiers(regathered->search, regathered->marks, *capture) == liveTiers(t, 0));
}

TEST(aGatheringWithoutAFocusedMatchHoldsNoNumbering)
{
    // Two gatherings of a terminal without a search compare equal whatever the
    // history did, so the writer publishes nothing new for output alone.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    const auto first = gather(t);
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n");
    const auto second = gather(t);
    CHECK(hasSameContent(first->search, second->search));
    CHECK(hasSameContent(first->marks, second->marks));
    CHECK_EQ(second->search.focusedCount + second->search.otherCount, 0);
}

TEST(aRowTranslatesAcrossEvictionAndNotAcrossEpochsOrScreens)
{
    const Screen::RowNumbering growing { .historyRows = 10, .appendedRows = 10, .absoluteRowEpoch = 1 };
    const Screen::RowNumbering grown { .historyRows = 13, .appendedRows = 13, .absoluteRowEpoch = 1 };
    const Screen::RowNumbering evicting { .historyRows = 13, .appendedRows = 20, .absoluteRowEpoch = 1 };
    int row = 0;
    // Growth below the limit numbers nothing anew.
    CHECK(Screen::translateAbsoluteRow(5, growing, grown, row));
    CHECK_EQ(row, 5);
    // Seven appended at the limit evict seven.
    CHECK(Screen::translateAbsoluteRow(9, grown, evicting, row));
    CHECK_EQ(row, 2);
    // Either direction.
    CHECK(Screen::translateAbsoluteRow(2, evicting, grown, row));
    CHECK_EQ(row, 9);
    // A dropped history evicts all of it.
    const Screen::RowNumbering dropped { .historyRows = 0, .appendedRows = 13, .absoluteRowEpoch = 1 };
    CHECK(Screen::translateAbsoluteRow(14, grown, dropped, row));
    CHECK_EQ(row, 1);
    // The alternate screen's rows are screen rows below the history.
    const Screen::RowNumbering alternate { .historyRows = 10, .appendedRows = 10, .absoluteRowEpoch = 1, .isAlternateScreen = true };
    const Screen::RowNumbering alternateTrimmed { .historyRows = 6, .appendedRows = 10, .absoluteRowEpoch = 1, .isAlternateScreen = true };
    CHECK(Screen::translateAbsoluteRow(12, alternate, alternateTrimmed, row));
    CHECK_EQ(row, 8);
    // No shared rows across an epoch or a screen.
    CHECK(!Screen::translateAbsoluteRow(5, growing, { .historyRows = 10, .appendedRows = 10, .absoluteRowEpoch = 2 }, row));
    CHECK(!Screen::translateAbsoluteRow(5, growing, alternate, row));
}

UBIQUITTY_TEST_MAIN()
