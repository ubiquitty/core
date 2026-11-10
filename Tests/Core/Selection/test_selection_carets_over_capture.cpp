// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_selection_carets_over_capture.cpp — a plain single-click drag, which
// the pointer ladder runs on carets between characters, over a captured frame
// (resolveSelectionCaret, configureCharacterCaretGesture and
// dragCharacterCaretGesture taking a Rendering::CapturedGrid). The reference
// is the same drag on the terminal the frame froze, driven as the ladder
// drives it: the caret resolution, the session's ends, the spans, and the
// terminal's later resolution of the captured session must all match.

#include "Core/Rendering/CapturedGrid.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <cmath>
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

bool haveSameEnds(const SelectionState& left, const SelectionState& right)
{
    return left.active == right.active && left.mode == right.mode && left.anchorRow == right.anchorRow
        && left.anchorColumn == right.anchorColumn && left.headRow == right.headRow && left.headColumn == right.headColumn
        && left.startSubColumn == right.startSubColumn && left.endSubColumn == right.endSubColumn;
}

bool haveSameCaret(const Selection::CaretResolution& left, const Selection::CaretResolution& right)
{
    return left.absoluteRow == right.absoluteRow && left.caret == right.caret && left.slot == right.slot
        && left.subCaret == right.subCaret && left.caretAfter == right.caretAfter && left.subCaretAfter == right.subCaretAfter;
}

// Where the pointer is: a visible row and a position along it in cells.
struct PointerAt {
    int row = 0;
    double xInCells = 0.0;
};

// The boundary nearest the pointer, as the ladder rounds it.
int boundaryOf(const PointerAt& point, int columns)
{
    return std::clamp(static_cast<int>(std::floor(point.xInCells + 0.5)), 0, columns);
}

Selection::CaretResolution liveCaret(Terminal& terminal, int viewOffset, const PointerAt& point)
{
    return Selection::resolveSelectionCaret(terminal, viewOffset, point.row, boundaryOf(point, terminal.columns), point.xInCells, point.row + 0.5);
}

Selection::CaretResolution capturedCaret(const CapturedGrid& grid, const PointerAt& point)
{
    return Selection::resolveSelectionCaret(grid, point.row, boundaryOf(point, grid.columns()), point.xInCells);
}

void dragLive(Selection::Session& session, Terminal& terminal, int viewOffset, const PointerAt& from, const PointerAt& to)
{
    Selection::beginGesture(session, terminal, from.row, static_cast<int>(std::floor(from.xInCells)), 1, viewOffset, {}, from.xInCells, from.row + 0.5);
    Selection::configureCharacterCaretGesture(session, terminal, liveCaret(terminal, viewOffset, from));
    Selection::dragCharacterCaretGesture(session, terminal, liveCaret(terminal, viewOffset, to), viewOffset);
    Selection::endGesture(session);
}

void dragOverCapture(Selection::Session& session, const CapturedGrid& grid, const PointerAt& from, const PointerAt& to)
{
    Selection::beginGesture(session, grid, from.row, static_cast<int>(std::floor(from.xInCells)), 1, {});
    Selection::configureCharacterCaretGesture(session, grid, capturedCaret(grid, from));
    Selection::dragCharacterCaretGesture(session, grid, capturedCaret(grid, to));
    Selection::endGesture(session);
}

// The drag on the terminal and over a capture of it, at one moment. Returns
// how many spans the terminal drew.
std::size_t checkDrag(Terminal& terminal, int viewOffset, const PointerAt& from, const PointerAt& to, const char* label)
{
    const TakenCapture capture = captureAt(terminal, viewOffset);
    const CapturedGrid grid(*capture);
    CHECK_MSG(grid.canExpandGestures(), label);
    if (!grid.canExpandGestures())
        return 0;
    CHECK_MSG(haveSameCaret(capturedCaret(grid, from), liveCaret(terminal, viewOffset, from)), label);
    CHECK_MSG(haveSameCaret(capturedCaret(grid, to), liveCaret(terminal, viewOffset, to)), label);

    Selection::Session live = freshSession();
    dragLive(live, terminal, viewOffset, from, to);
    Selection::Session captured = freshSession();
    dragOverCapture(captured, grid, from, to);
    CHECK_MSG(haveSameEnds(captured.state, live.state), label);
    const std::vector<Rendering::SelectionRowSpan> expected = liveSpans(live, terminal, viewOffset);
    CHECK_MSG(capturedSpans(captured, grid) == expected, label);
    // The owner's resolution of the hand-off names the ends where they stand.
    Selection::Session handedOff = captured;
    CHECK_MSG(liveSpans(handedOff, terminal, viewOffset) == expected, label);
    return expected.size();
}

void feedLines(Terminal& terminal, int count, std::string_view stem)
{
    for (int line = 0; line < count; ++line)
        feed(terminal, std::string(stem) + " " + std::to_string(line) + "\r\n");
}

} // namespace

TEST(aCaretDragOverACaptureEndsWhereTheTerminalsDragEnds)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 24);
    feedLines(t, 20, "history");
    feed(t, "plain words here\r\n");
    feed(t, "wide \xE6\xBC\xA2\xE5\xAD\x97\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 end\r\n");
    feed(t, "\r\n");
    feed(t, "aaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cc\r\n");
    feed(t, "\033#6double row\r\n");
    feed(t, "short\r\n");
    feed(t, "last line on screen");
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > 0);
    REQUIRE(t.screens.active->lineAttribute[7] == lineAttributeDoubleWidth);
    REQUIRE(Screen::absoluteRowWrapped(&t, history + 5));

    std::size_t drawn = 0;
    // The screen, top to bottom: two history rows, the plain row (2), the
    // wide row (3), a blank row (4), the wrapped line (5, 6), the
    // double-width row (7), the short row (8) and the last line (9).
    drawn += checkDrag(t, 0, { 2, 2.3 }, { 2, 9.6 }, "along a row");
    drawn += checkDrag(t, 0, { 8, 4.2 }, { 2, 1.4 }, "upward across rows");
    drawn += checkDrag(t, 0, { 3, 5.4 }, { 3, 8.7 }, "over wide characters");
    drawn += checkDrag(t, 0, { 3, 6.0 }, { 6, 3.0 }, "from a wide character's middle onto a wrapped row");
    drawn += checkDrag(t, 0, { 4, 7.0 }, { 4, 12.0 }, "along a blank row");
    drawn += checkDrag(t, 0, { 8, 3.2 }, { 8, 20.5 }, "past a short row's end");
    drawn += checkDrag(t, 0, { 7, 3.1 }, { 7, 13.8 }, "along a double-width row");
    drawn += checkDrag(t, 0, { 7, 9.6 }, { 9, 2.2 }, "from a double-width row down");
    drawn += checkDrag(t, 0, { 2, 4.4 }, { 2, 4.6 }, "a press that never leaves its boundary");
    drawn += checkDrag(t, history / 2, { 0, 1.5 }, { 9, 6.5 }, "scrolled back");
    drawn += checkDrag(t, history, { 9, 7.5 }, { 0, 0.2 }, "at the top, upward");
    CHECK(drawn > 0);
}

TEST(aCaretDragOverACaptureOfTheAlternateScreenEndsWhereTheTerminalsDragEnds)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 24);
    feedLines(t, 20, "history");
    feed(t, "\033[?1049h\033[2;1Hwide \xE6\xBC\xA2\xE5\xAD\x97 alternate words wrap around the edge\033[6;1Hlast");
    REQUIRE(t.screens.isUsingAlternateScreen);
    std::size_t drawn = 0;
    drawn += checkDrag(t, 0, { 1, 2.2 }, { 5, 3.6 }, "across rows");
    drawn += checkDrag(t, 0, { 1, 9.4 }, { 1, 5.6 }, "leftward over wide characters");
    CHECK(drawn > 0);
}

TEST(aCaretDragOverAHeldCaptureResolvesWhereTheTerminalsDragResolves)
{
    // The view drags over the frame it holds while the parser moves on: when
    // the owner resolves the hand-off, the ends name the characters the
    // pointer was between.
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    setScrollbackLimit(&t, 30);
    feedLines(t, 60, "history line");
    REQUIRE(Screen::scrollbackCount(&t) == 30);
    const PointerAt from { 1, 3.4 };
    const PointerAt to { 4, 9.6 };

    Selection::Session live = freshSession();
    dragLive(live, t, 8, from, to);
    const TakenCapture held = captureAt(t, 8);
    Selection::Session captured = freshSession();
    dragOverCapture(captured, CapturedGrid(*held), from, to);
    REQUIRE(Selection::hasSelection(captured));

    feedLines(t, 3, "more output");
    for (const int viewOffset : { 0, 8, 11 }) {
        Selection::Session handedOff = captured;
        const std::vector<Rendering::SelectionRowSpan> expected = liveSpans(live, t, viewOffset);
        CHECK_MSG(liveSpans(handedOff, t, viewOffset) == expected, ("view offset " + std::to_string(viewOffset)).c_str());
        CHECK(haveSameEnds(handedOff.state, live.state));
    }
}

UBIQUITTY_TEST_MAIN()
