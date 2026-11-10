// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_selection_gestures_over_capture.cpp — a pointer gesture expanded over a
// captured frame (Selection::beginGesture / dragGesture taking a
// Rendering::CapturedGrid). The reference is the same gesture on the terminal
// the frame froze: the session must end where the terminal's gesture leaves
// it, draw the same spans, and, once output has moved on, resolve on the
// terminal to what the terminal's own gesture resolves to.

#include "Core/Rendering/CapturedGrid.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Triggers.h"
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

Selection::Session freshSession()
{
    Selection::Session session;
    Selection::init(session);
    return session;
}

std::vector<Rendering::SelectionRowSpan> liveSpans(Selection::Session& session, Terminal& terminal, int viewOffset)
{
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
    Selection::ViewportEnds ends;
    spans.resize(static_cast<std::size_t>(Selection::viewportSpansAndEnds(session, terminal, viewOffset, spans, ends)));
    return spans;
}

std::vector<Rendering::SelectionRowSpan> capturedSpans(const Selection::Session& session, const CapturedGrid& grid)
{
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(grid.rows(), grid.columns())));
    Selection::ViewportEnds ends;
    spans.resize(static_cast<std::size_t>(Selection::viewportSpansAndEndsOverCapture(session, grid, spans, ends)));
    return spans;
}

std::string describe(const SelectionState& state)
{
    return "active " + std::to_string(state.active) + " mode " + std::to_string(static_cast<int>(state.mode)) + " anchor "
        + std::to_string(state.anchorRow) + ":" + std::to_string(state.anchorColumn) + " head " + std::to_string(state.headRow) + ":"
        + std::to_string(state.headColumn);
}

bool haveSameEnds(const SelectionState& left, const SelectionState& right)
{
    return left.active == right.active && left.mode == right.mode && left.anchorRow == right.anchorRow
        && left.anchorColumn == right.anchorColumn && left.headRow == right.headRow && left.headColumn == right.headColumn;
}

struct Press {
    int fromRow = 0;
    int fromColumn = 0;
    int toRow = 0;
    int toColumn = 0;
    int clickCount = 1;
    Selection::GestureStyle style {};
    int viewOffset = 0;
};

void pressLive(Selection::Session& session, Terminal& terminal, const Press& press)
{
    Selection::beginGesture(session, terminal, press.fromRow, press.fromColumn, press.clickCount, press.viewOffset, press.style);
    Selection::dragGesture(session, terminal, press.toRow, press.toColumn, press.viewOffset);
    Selection::endGesture(session);
}

// Whether the gesture changed the session. A single click opens nothing
// until the pointer moves, so the press alone may answer false.
bool pressOverCapture(Selection::Session& session, const CapturedGrid& grid, const Press& press)
{
    const bool didBegin = Selection::beginGesture(session, grid, press.fromRow, press.fromColumn, press.clickCount, press.style);
    const bool didDrag = Selection::dragGesture(session, grid, press.toRow, press.toColumn);
    Selection::endGesture(session);
    return didBegin || didDrag;
}

// The gesture on the terminal and over a capture of it, at one moment: same
// ends, same spans, and the same resolution once the terminal meets the
// captured session. Returns how many spans the terminal drew.
std::size_t checkPress(Terminal& terminal, const Press& press, const char* label)
{
    Selection::Session live = freshSession();
    pressLive(live, terminal, press);
    const TakenCapture capture = captureAt(terminal, press.viewOffset);
    const CapturedGrid grid(*capture);
    Selection::Session captured = freshSession();
    CHECK_MSG(pressOverCapture(captured, grid, press), label);
    CHECK_MSG(haveSameEnds(captured.state, live.state), (std::string(label) + ": captured " + describe(captured.state) + ", live " + describe(live.state)).c_str());
    const std::vector<Rendering::SelectionRowSpan> expected = liveSpans(live, terminal, press.viewOffset);
    CHECK_MSG(capturedSpans(captured, grid) == expected, label);
    // The owner's resolution of the hand-off: the terminal names the ends the
    // capture's gesture left unnamed, where they stand.
    Selection::Session handedOff = captured;
    CHECK_MSG(liveSpans(handedOff, terminal, press.viewOffset) == expected, label);
    CHECK_MSG(haveSameEnds(handedOff.state, live.state), label);
    return expected.size();
}

void feedLines(Terminal& terminal, int count, std::string_view stem)
{
    for (int line = 0; line < count; ++line)
        feed(terminal, std::string(stem) + " " + std::to_string(line) + "\r\n");
}

} // namespace

TEST(aGestureOverACaptureEndsWhereTheTerminalsGestureEnds)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 24);
    feedLines(t, 20, "history");
    feed(t, "plain words here\r\n");
    feed(t, "wide \xE6\xBC\xA2\xE5\xAD\x97\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 end\r\n");
    feed(t, "see https://example.com/a/path?q=1 now\r\n");
    feed(t, "aaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cc\r\n");
    feed(t, "\033#6double row\r\n");
    feed(t, "cafe\xCC\x81 combining\r\n");
    feed(t, "last line on screen");
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > 0);

    std::size_t drawn = 0;
    drawn += checkPress(t, { .fromRow = 2, .fromColumn = 3, .toRow = 6, .toColumn = 10 }, "character drag");
    drawn += checkPress(t, { .fromRow = 6, .fromColumn = 12, .toRow = 2, .toColumn = 1 }, "character drag upward");
    drawn += checkPress(t, { .fromRow = 3, .fromColumn = 6, .toRow = 3, .toColumn = 8 }, "character drag over wide characters");
    drawn += checkPress(t, { .fromRow = 2, .fromColumn = 7, .toRow = 2, .toColumn = 7, .clickCount = 2 }, "word");
    drawn += checkPress(t, { .fromRow = 5, .fromColumn = 10, .toRow = 5, .toColumn = 10, .clickCount = 2 }, "wrapped word");
    drawn += checkPress(t, { .fromRow = 4, .fromColumn = 14, .toRow = 4, .toColumn = 14, .clickCount = 2, .style = { .smartToken = Selection::SmartTokenMode::Enabled } },
        "smart token");
    drawn += checkPress(t, { .fromRow = 3, .fromColumn = 1, .toRow = 7, .toColumn = 4, .clickCount = 2 }, "word drag");
    drawn += checkPress(t, { .fromRow = 5, .fromColumn = 2, .toRow = 5, .toColumn = 2, .clickCount = 3 }, "wrapped line");
    drawn += checkPress(t, { .fromRow = 2, .fromColumn = 0, .toRow = 8, .toColumn = 3, .clickCount = 3 }, "line drag");
    drawn += checkPress(t, { .fromRow = 7, .fromColumn = 9, .toRow = 7, .toColumn = 9, .clickCount = 2 }, "word on a double-width row");
    drawn += checkPress(t, { .fromRow = 2, .fromColumn = 2, .toRow = 8, .toColumn = 11, .style = { .isRectangular = true } }, "rectangle");
    drawn += checkPress(t, { .fromRow = 1, .fromColumn = 4, .toRow = 8, .toColumn = 6, .viewOffset = history / 2 }, "scrolled back");
    drawn += checkPress(t, { .fromRow = 0, .fromColumn = 0, .toRow = 9, .toColumn = 20, .viewOffset = history }, "at the top");
    CHECK(drawn > 0);
}

TEST(aGestureOverACaptureOfTheAlternateScreenEndsWhereTheTerminalsGestureEnds)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 24);
    feedLines(t, 20, "history");
    feed(t, "\033[?1049h\033[2;1Hwide \xE6\xBC\xA2\xE5\xAD\x97 alternate words wrap around the edge\033[6;1Hlast");
    REQUIRE(t.screens.isUsingAlternateScreen);
    std::size_t drawn = 0;
    drawn += checkPress(t, { .fromRow = 1, .fromColumn = 2, .toRow = 5, .toColumn = 3 }, "character drag");
    drawn += checkPress(t, { .fromRow = 1, .fromColumn = 14, .toRow = 1, .toColumn = 14, .clickCount = 2 }, "word");
    drawn += checkPress(t, { .fromRow = 2, .fromColumn = 4, .toRow = 2, .toColumn = 4, .clickCount = 3 }, "wrapped line");
    CHECK(drawn > 0);
}

TEST(aGestureOverAHeldCaptureResolvesWhereTheTerminalsGestureResolves)
{
    // The view gestures over the frame it holds while the parser moves on:
    // when the owner resolves the hand-off, the ends name the characters the
    // pointer was over, not the rows those characters have since left.
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    setScrollbackLimit(&t, 30);
    feedLines(t, 60, "history line");
    REQUIRE(Screen::scrollbackCount(&t) == 30);
    const Press press { .fromRow = 1, .fromColumn = 3, .toRow = 4, .toColumn = 9, .viewOffset = 8 };

    Selection::Session live = freshSession();
    pressLive(live, t, press);
    const TakenCapture held = captureAt(t, press.viewOffset);
    Selection::Session captured = freshSession();
    REQUIRE(pressOverCapture(captured, CapturedGrid(*held), press));

    feedLines(t, 3, "more output");
    for (const int viewOffset : { 0, 8, 11 }) {
        Selection::Session handedOff = captured;
        const std::vector<Rendering::SelectionRowSpan> expected = liveSpans(live, t, viewOffset);
        CHECK_MSG(liveSpans(handedOff, t, viewOffset) == expected, ("view offset " + std::to_string(viewOffset)).c_str());
        CHECK(haveSameEnds(handedOff.state, live.state));
    }
}

TEST(aGestureOverACaptureTheTerminalMustExpandIsRefused)
{
    // A configured character-class table grows a double click by rules only
    // the terminal holds: the press goes to the terminal, and the session is
    // left as it was.
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    REQUIRE(Triggers::addCharacterClass(t, "path", "[A-Za-z0-9_./-]+", CaseSensitivity::Sensitive));
    feed(t, "open /usr/local/bin now");
    const TakenCapture capture = captureAt(t, 0);
    const CapturedGrid grid(*capture);
    CHECK(grid.isReadable());
    CHECK(!grid.canExpandGestures());
    Selection::Session session = freshSession();
    CHECK(!Selection::beginGesture(session, grid, 0, 8, 2, {}));
    CHECK(!Selection::hasSelection(session));

    // A resize renumbers the rows under a standing selection: only the owner
    // can say where it went, and the press waits for it.
    Terminal resized;
    hardReset(resized);
    resize(resized, 6, 30);
    feedLines(resized, 12, "history line");
    Selection::Session standing = freshSession();
    pressLive(standing, resized, { .fromRow = 1, .fromColumn = 0, .toRow = 2, .toColumn = 5 });
    resize(resized, 6, 22);
    const TakenCapture after = captureAt(resized, 0);
    const SelectionState before = standing.state;
    CHECK(!Selection::beginGesture(standing, CapturedGrid(*after), 3, 2, 1, {}));
    CHECK(haveSameEnds(standing.state, before));
}

UBIQUITTY_TEST_MAIN()
