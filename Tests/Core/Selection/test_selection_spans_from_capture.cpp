// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_selection_spans_from_capture.cpp — a selection's spans resolved over a
// captured frame (Rendering::CapturedGrid,
// Selection::viewportSpansAndEndsOverCapture). The reference is the live
// resolution over the terminal the frame froze: for the same session at the
// same moment, the two must answer the same spans and the same ends, at every
// view offset, on both screens, across wide characters, combining marks,
// wrapped rows and double-width lines, for every selection shape.

#include "Core/Cell.h"
#include "Core/Rendering/CapturedGrid.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using Rendering::CapturedGrid;
using Rendering::FrameCapture;
using UbiquittyTest::feed;

namespace {

using TakenCapture = std::unique_ptr<FrameCapture, decltype([](FrameCapture* capture) { releaseFrameCapture(capture); })>;

TakenCapture captureAt(const Terminal& terminal, int viewOffset)
{
    // A helper that returns cannot REQUIRE; a failed capture aborts here,
    // before any comparison reads it.
    TakenCapture capture { FrameCapture::make() };
    Rendering::UnifontRasterizer rasterizer(1, 0);
    Rendering::TerminalSnapshotOptions options;
    options.viewOffset = viewOffset;
    const bool didCapture = capture && capture->capture(terminal, options, rasterizer.cellMetrics(), Rendering::GlyphSlotGeometry {});
    if (!didCapture)
        std::abort();
    return capture;
}

struct Resolved {
    std::vector<Rendering::SelectionRowSpan> spans;
    Selection::ViewportEnds ends;

    friend bool operator==(const Resolved& left, const Resolved& right)
    {
        return left.spans == right.spans && left.ends.isVisible == right.ends.isVisible
            && (!left.ends.isVisible
                || (left.ends.startRow == right.ends.startRow && left.ends.startColumn == right.ends.startColumn
                    && left.ends.endRow == right.ends.endRow && left.ends.endColumn == right.ends.endColumn));
    }
};

Resolved resolveLive(Selection::Session& session, Terminal& terminal, int viewOffset)
{
    Resolved resolved;
    resolved.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
    const int count = Selection::viewportSpansAndEnds(session, terminal, viewOffset, resolved.spans, resolved.ends);
    resolved.spans.resize(static_cast<std::size_t>(count));
    return resolved;
}

Resolved resolveOverCapture(const Selection::Session& session, const CapturedGrid& grid)
{
    Resolved resolved;
    resolved.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(grid.rows(), grid.columns())));
    const int count = Selection::viewportSpansAndEndsOverCapture(session, grid, resolved.spans, resolved.ends);
    resolved.spans.resize(static_cast<std::size_t>(count));
    return resolved;
}

struct Gesture {
    int fromRow = 0;
    int fromColumn = 0;
    int toRow = 0;
    int toColumn = 0;
    int clickCount = 1;
    bool isRectangular = false;
    int viewOffset = 0;
};

// Makes the selection a pointer makes, then compares its spans live and over
// a capture at each view offset. Returns how many spans the live resolution
// drew in all, so a case cannot pass by comparing empty answers.
std::size_t checkSelection(Terminal& terminal, const Gesture& gesture, std::initializer_list<int> viewOffsets, const char* label)
{
    Selection::Session session;
    Selection::init(session);
    Selection::beginGesture(session, terminal, gesture.fromRow, gesture.fromColumn, gesture.clickCount, gesture.viewOffset,
        { .isRectangular = gesture.isRectangular });
    Selection::dragGesture(session, terminal, gesture.toRow, gesture.toColumn, gesture.viewOffset);
    Selection::endGesture(session);
    std::size_t drawn = 0;
    for (const int viewOffset : viewOffsets) {
        // The live resolution synchronizes the session with the terminal,
        // which the capture path takes as its precondition.
        const Resolved live = resolveLive(session, terminal, viewOffset);
        const TakenCapture capture = captureAt(terminal, viewOffset);
        const CapturedGrid grid(*capture);
        CHECK_MSG(grid.isReadable(), label);
        const Resolved captured = resolveOverCapture(session, grid);
        CHECK_MSG(captured == live, (std::string(label) + " at view offset " + std::to_string(viewOffset)).c_str());
        drawn += live.spans.size();
    }
    return drawn;
}

// History of plain, wide, combining and wrapped lines on a 20-column grid.
void feedMixedHistory(Terminal& terminal)
{
    for (int line = 0; line < 24; ++line) {
        switch (line % 6) {
        case 0:
            feed(terminal, "plain words on " + std::to_string(line) + "\r\n");
            break;
        case 1:
            feed(terminal, "wide \xE6\xBC\xA2\xE5\xAD\x97\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 " + std::to_string(line) + "\r\n");
            break;
        case 2:
            feed(terminal, "cafe\xCC\x81 combining " + std::to_string(line) + "\r\n");
            break;
        case 3:
            feed(terminal, "aaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbb cc " + std::to_string(line) + "\r\n");
            break;
        case 4:
            feed(terminal, "\033#6double " + std::to_string(line) + "\r\n");
            break;
        default:
            feed(terminal, "tail \xE5\xAD\x97" + std::to_string(line) + "\r\n");
            break;
        }
    }
    feed(terminal, "the screen's last line, long enough to wrap twice over");
}

} // namespace

TEST(aSelectionOverACaptureDrawsTheSpansTheTerminalDraws)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feedMixedHistory(t);
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > 2 * t.rows);
    const std::initializer_list<int> offsets { 0, 1, 5, history / 2, history - 3, history, history + 4 };

    std::size_t drawn = 0;
    drawn += checkSelection(t, { .fromRow = 1, .fromColumn = 3, .toRow = 6, .toColumn = 11 }, offsets, "character drag");
    drawn += checkSelection(t, { .fromRow = 0, .fromColumn = 7, .toRow = 7, .toColumn = 2, .viewOffset = history / 2 }, offsets,
        "character drag through history");
    drawn += checkSelection(t, { .fromRow = 2, .fromColumn = 6, .toRow = 2, .toColumn = 6, .viewOffset = 9 }, offsets,
        "character drag onto a wide character's second half");
    drawn += checkSelection(t, { .fromRow = 3, .fromColumn = 8, .toRow = 3, .toColumn = 8, .clickCount = 2, .viewOffset = 4 }, offsets,
        "word");
    drawn += checkSelection(t, { .fromRow = 5, .fromColumn = 2, .toRow = 6, .toColumn = 9, .clickCount = 2, .viewOffset = history / 2 },
        offsets, "word drag");
    drawn += checkSelection(t, { .fromRow = 4, .fromColumn = 1, .toRow = 4, .toColumn = 1, .clickCount = 3, .viewOffset = 7 }, offsets,
        "line");
    drawn += checkSelection(t, { .fromRow = 6, .fromColumn = 0, .toRow = 7, .toColumn = 15, .clickCount = 3 }, offsets,
        "line drag over a wrapped line");
    drawn += checkSelection(t, { .fromRow = 1, .fromColumn = 4, .toRow = 6, .toColumn = 12, .isRectangular = true, .viewOffset = 3 },
        offsets, "rectangle");
    CHECK(drawn > 0);
}

TEST(aSelectionOverACaptureOfTheAlternateScreenDrawsTheSpansTheTerminalDraws)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feedMixedHistory(t);
    const int history = Screen::scrollbackCount(&t);
    feed(t, "\033[?1049h\033[2;1Hwide \xE6\xBC\xA2\xE5\xAD\x97 on the alternate screen wraps\033[6;1H\033#6double");
    REQUIRE(t.screens.isUsingAlternateScreen);
    // A scroll position left from the primary screen addresses nothing here.
    const std::initializer_list<int> offsets { 0, 3, history };

    std::size_t drawn = 0;
    drawn += checkSelection(t, { .fromRow = 1, .fromColumn = 2, .toRow = 5, .toColumn = 4 }, offsets, "character drag");
    drawn += checkSelection(t, { .fromRow = 1, .fromColumn = 6, .toRow = 1, .toColumn = 6, .clickCount = 2 }, offsets, "word on wide");
    drawn += checkSelection(t, { .fromRow = 2, .fromColumn = 3, .toRow = 2, .toColumn = 3, .clickCount = 3 }, offsets, "line");
    drawn += checkSelection(t, { .fromRow = 0, .fromColumn = 1, .toRow = 6, .toColumn = 9, .isRectangular = true }, offsets, "rectangle");
    CHECK(drawn > 0);
}

TEST(aSelectionOverACaptureReadsTheRowsUnderAStatusRow)
{
    // A status row composited over the frame's bottom row covers the cells a
    // selection reads; the grid reads the ones the terminal stores, as the
    // live resolution does. A word on the covered row grows by its own
    // letters, not by the status row's.
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "first line\r\nsecond\r\nthird\r\nfourth\r\nfifth\r\nbottom words here");
    std::vector<Cell> statusCells(static_cast<std::size_t>(t.columns), defaultBlankCell);
    for (Cell& cell : statusCells)
        cell.codePoint = 'S';
    Rendering::UnifontRasterizer rasterizer(1, 0);
    Rendering::TerminalSnapshotOptions options;
    options.statusRowCells = statusCells.data();
    options.statusRowColumns = t.columns;
    const TakenCapture capture { FrameCapture::make() };
    REQUIRE(capture);
    REQUIRE(capture->capture(t, options, rasterizer.cellMetrics(), Rendering::GlyphSlotGeometry {}));
    const CapturedGrid grid(*capture);
    REQUIRE(grid.isReadable());

    for (const Gesture& gesture : { Gesture { .fromRow = 3, .fromColumn = 2, .toRow = 5, .toColumn = 9 },
             Gesture { .fromRow = 5, .fromColumn = 9, .toRow = 5, .toColumn = 9, .clickCount = 2 } }) {
        Selection::Session session;
        Selection::init(session);
        Selection::beginGesture(session, t, gesture.fromRow, gesture.fromColumn, gesture.clickCount, 0, {});
        Selection::dragGesture(session, t, gesture.toRow, gesture.toColumn, 0);
        Selection::endGesture(session);
        const Resolved live = resolveLive(session, t, 0);
        REQUIRE(!live.spans.empty());
        CHECK(resolveOverCapture(session, grid) == live);
    }
}

TEST(aCaptureHoldingWhatOnlyTheTerminalResolvesIsNotReadable)
{
    // A frame holding a sized glyph, a right-to-left row or nothing from a
    // terminal answers no spans: the capture's own resolved selection stands
    // for it.
    const auto isReadableAfter = [](std::string_view bytes) {
        auto t = std::make_unique<Terminal>();
        hardReset(*t);
        resize(*t, 6, 20);
        t->drawsSizedText = true;
        feed(*t, "plain\r\n");
        feed(*t, bytes);
        const TakenCapture capture = captureAt(*t, 0);
        return CapturedGrid(*capture).isReadable();
    };
    CHECK(isReadableAfter("more plain text"));
    CHECK(!isReadableAfter("\033]66;s=2;AB\007"));
    CHECK(!isReadableAfter("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));

    Selection::Session session;
    Selection::init(session);
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    t.drawsSizedText = true;
    feed(t, "\033]66;s=2;AB\007 and more");
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    Selection::dragGesture(session, t, 0, 10, 0);
    REQUIRE(resolveLive(session, t, 0).spans.size() > 0);
    const TakenCapture capture = captureAt(t, 0);
    CHECK(resolveOverCapture(session, CapturedGrid(*capture)).spans.empty());
}

namespace {

// A selection over a terminal at its history limit, met by the live
// resolution once so the session holds that moment's observation.
Selection::Session selectAtLimit(Terminal& terminal, const Gesture& gesture)
{
    Selection::Session session;
    Selection::init(session);
    Selection::beginGesture(session, terminal, gesture.fromRow, gesture.fromColumn, gesture.clickCount, gesture.viewOffset, {});
    Selection::dragGesture(session, terminal, gesture.toRow, gesture.toColumn, gesture.viewOffset);
    Selection::endGesture(session);
    (void)resolveLive(session, terminal, gesture.viewOffset);
    return session;
}

void feedLines(Terminal& terminal, int count, std::string_view stem)
{
    for (int line = 0; line < count; ++line)
        feed(terminal, std::string(stem) + " " + std::to_string(line) + "\r\n");
}

} // namespace

TEST(aSessionBroughtToACaptureDrawsTheSpansTheTerminalDraws)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    setScrollbackLimit(&t, 30);
    feedLines(t, 60, "history line");
    REQUIRE(Screen::scrollbackCount(&t) == 30);

    // Output at the limit evicts a row per line: the live resolution follows
    // the selected text up the numbering, and so does the rebase.
    Selection::Session live = selectAtLimit(t, { .fromRow = 1, .fromColumn = 3, .toRow = 4, .toColumn = 9, .viewOffset = 8 });
    Selection::Session held = live;
    feedLines(t, 4, "more output");
    std::size_t drawn = 0;
    for (const int viewOffset : { 0, 4, 8, 12, 30 }) {
        const Resolved expected = resolveLive(live, t, viewOffset);
        const TakenCapture capture = captureAt(t, viewOffset);
        const CapturedGrid grid(*capture);
        Selection::Session rebased = held;
        CHECK(Selection::rebaseOverCapture(rebased, grid) == Selection::CaptureRebase::Moved);
        CHECK_MSG(resolveOverCapture(rebased, grid) == expected, ("view offset " + std::to_string(viewOffset)).c_str());
        drawn += expected.spans.size();
    }
    CHECK(drawn > 0);
    // Brought once, the session is at the frame's numbering: a second rebase
    // over the same frame moves nothing.
    const TakenCapture capture = captureAt(t, 8);
    Selection::Session rebased = held;
    REQUIRE(Selection::rebaseOverCapture(rebased, CapturedGrid(*capture)) == Selection::CaptureRebase::Moved);
    CHECK(Selection::rebaseOverCapture(rebased, CapturedGrid(*capture)) == Selection::CaptureRebase::Unchanged);
}

TEST(aSessionBroughtToACaptureBelowTheLimitStaysOnItsRows)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    feedLines(t, 12, "history line");
    Selection::Session live = selectAtLimit(t, { .fromRow = 0, .fromColumn = 2, .toRow = 3, .toColumn = 7, .viewOffset = 3 });
    Selection::Session held = live;
    feedLines(t, 5, "more output");
    // Below the limit the rows keep their numbers: the view five rows further
    // back shows the same text.
    const Resolved expected = resolveLive(live, t, 8);
    const TakenCapture capture = captureAt(t, 8);
    const CapturedGrid grid(*capture);
    CHECK(Selection::rebaseOverCapture(held, grid) == Selection::CaptureRebase::Unchanged);
    CHECK(resolveOverCapture(held, grid) == expected);
    CHECK(!expected.spans.empty());
}

TEST(aSessionWhoseRowsWereEvictedIsLostOverACapture)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    setScrollbackLimit(&t, 30);
    feedLines(t, 60, "history line");
    const int history = Screen::scrollbackCount(&t);
    Selection::Session live = selectAtLimit(t, { .fromRow = 0, .fromColumn = 0, .toRow = 1, .toColumn = 5, .viewOffset = history });
    Selection::Session held = live;
    feedLines(t, 4, "more output");
    const Resolved expected = resolveLive(live, t, 0);
    CHECK(expected.spans.empty());
    const TakenCapture capture = captureAt(t, 0);
    const CapturedGrid grid(*capture);
    CHECK(Selection::rebaseOverCapture(held, grid) == Selection::CaptureRebase::Lost);
    CHECK(resolveOverCapture(held, grid) == expected);
    CHECK(!Selection::hasSelection(held));
}

TEST(aSessionMeetingAnotherScreenOrNumberingOverACapture)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    feedLines(t, 12, "history line");
    const Selection::Session held = selectAtLimit(t, { .fromRow = 1, .fromColumn = 0, .toRow = 2, .toColumn = 5 });

    // The alternate screen takes the selection, as the terminal's
    // synchronization takes it.
    feed(t, "\033[?1049h");
    {
        const TakenCapture capture = captureAt(t, 0);
        Selection::Session moved = held;
        CHECK(Selection::rebaseOverCapture(moved, CapturedGrid(*capture)) == Selection::CaptureRebase::Lost);
        CHECK(!Selection::hasSelection(moved));
    }
    feed(t, "\033[?1049l");

    // A resize renumbers the rows through the reflow, which only the owner
    // can follow; the session is left as it was.
    resize(t, 6, 24);
    const TakenCapture capture = captureAt(t, 0);
    Selection::Session renumbered = held;
    CHECK(Selection::rebaseOverCapture(renumbered, CapturedGrid(*capture)) == Selection::CaptureRebase::NeedsOwner);
    CHECK_EQ(renumbered.state.anchorRow, held.state.anchorRow);
    CHECK_EQ(renumbered.state.headRow, held.state.headRow);
}

UBIQUITTY_TEST_MAIN()
