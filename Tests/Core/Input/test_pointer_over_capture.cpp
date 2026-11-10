// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pointer_over_capture.cpp — the pointer ladder's selection rung over the
// frame a view presents (Input::routePointerOverCapture). The reference is
// routePointer over the terminal the frame froze: for the same facts, with
// every rung above the selection quiet, the routes, the ladders' gestures,
// the sessions' ends and the spans drawn must match; and a fact an earlier
// rung could take, or one over a frame that cannot take the gesture, is
// refused with the ladder and the session as they were.

#include "Core/Input/PointerLadder.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Input;
using Rendering::FrameCapture;
using UbiquittyTest::feed;

namespace {

using TakenCapture = std::unique_ptr<FrameCapture, decltype([](FrameCapture* capture) { releaseFrameCapture(capture); })>;

TakenCapture captureOf(const Terminal& terminal)
{
    // A helper that returns cannot REQUIRE; a failed capture aborts here,
    // before any comparison reads it.
    TakenCapture capture { FrameCapture::make() };
    Rendering::UnifontRasterizer rasterizer(1, 0);
    const bool didCapture = capture && capture->capture(terminal, Rendering::TerminalSnapshotOptions {}, rasterizer.cellMetrics(), Rendering::GlyphSlotGeometry {});
    if (!didCapture)
        std::abort();
    return capture;
}

// A press, motion or release at a point along a visible row.
PointerFact at(PointerFact::Kind kind, int row, double xInCells, int clickCount = 1)
{
    PointerFact fact;
    fact.kind = kind;
    fact.button = PointerButton::Left;
    fact.row = row;
    fact.column = static_cast<std::ptrdiff_t>(std::floor(xInCells));
    fact.xInCells = xInCells;
    fact.yInRows = row + 0.5;
    fact.clickCount = clickCount;
    fact.isDragging = kind == PointerFact::Kind::Motion;
    return fact;
}

std::vector<Rendering::SelectionRowSpan> spansOf(Selection::Session& session, Terminal& terminal)
{
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
    Selection::ViewportEnds ends;
    spans.resize(static_cast<std::size_t>(Selection::viewportSpansAndEnds(session, terminal, 0, spans, ends)));
    return spans;
}

bool haveSameEnds(const SelectionState& left, const SelectionState& right)
{
    return left.active == right.active && left.mode == right.mode && left.anchorRow == right.anchorRow
        && left.anchorColumn == right.anchorColumn && left.headRow == right.headRow && left.headColumn == right.headColumn;
}

bool haveSameRoute(const PointerRoute& left, const PointerRoute& right)
{
    return left.wasConsumed == right.wasConsumed && left.didChangeSelection == right.didChangeSelection
        && left.didOpenSelectionGesture == right.didOpenSelectionGesture && left.didFinishSelectionGesture == right.didFinishSelectionGesture
        && left.needsDisplay == right.needsDisplay && left.reportLength == right.reportLength && left.reportLength == 0;
}

// Two views of one terminal: one routed over the terminal, one over a frame of
// it, each with its own ladder and session.
struct Views {
    Terminal terminal;
    PointerPolicy policy;
    PointerLadder liveLadder;
    PointerLadder capturedLadder;
    Selection::Session live;
    Selection::Session captured;

    Views()
    {
        hardReset(terminal);
        resize(terminal, 8, 30);
        feed(terminal, "first plain line here\r\n");
        feed(terminal, "wide \xE6\xBC\xA2\xE5\xAD\x97 and https://example.com/a\r\n");
        feed(terminal, "aaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cc\r\n");
        feed(terminal, "\r\n");
        feed(terminal, "short\r\n");
        feed(terminal, "last row of the screen");
        Selection::init(live);
        Selection::init(captured);
    }

    // Routes each fact both ways, comparing as it goes. Returns how many
    // spans the terminal's view drew at the end.
    std::size_t route(std::initializer_list<PointerFact> facts, const char* label)
    {
        const TakenCapture frame = captureOf(terminal);
        for (const PointerFact& fact : facts) {
            const PointerRoute expected = routePointer(liveLadder, terminal, &live, fact, policy, 0);
            const std::optional<PointerRoute> overFrame = routePointerOverCapture(capturedLadder, *frame, &captured, fact, policy, {});
            CHECK_MSG(overFrame.has_value(), label);
            if (!overFrame)
                return 0;
            CHECK_MSG(haveSameRoute(*overFrame, expected), label);
            CHECK_MSG(capturedLadder.isSelectionGestureOpen == liveLadder.isSelectionGestureOpen, label);
            CHECK_MSG(haveSameEnds(captured.state, live.state), label);
        }
        const std::vector<Rendering::SelectionRowSpan> expected = spansOf(live, terminal);
        // The owner's resolution of the view's hand-off draws what the
        // terminal's own route drew.
        Selection::Session handedOff = captured;
        CHECK_MSG(spansOf(handedOff, terminal) == expected, label);
        return expected.size();
    }
};

} // namespace

TEST(theSelectionRungOverAFrameRoutesWhatTheTerminalRoutes)
{
    Views views;
    std::size_t drawn = 0;
    drawn += views.route({ at(PointerFact::Kind::Press, 0, 2.4), at(PointerFact::Kind::Motion, 0, 6.6), at(PointerFact::Kind::Motion, 2, 9.2),
                             at(PointerFact::Kind::Release, 2, 9.2) },
        "a plain click's caret drag");
    drawn += views.route({ at(PointerFact::Kind::Press, 1, 3.6), at(PointerFact::Kind::Motion, 0, 1.2), at(PointerFact::Kind::Release, 0, 1.2) },
        "a caret drag upward from wide characters");
    drawn += views.route({ at(PointerFact::Kind::Press, 2, 9.0, 2), at(PointerFact::Kind::Release, 2, 9.0, 2) }, "a word on a wrapped row");
    drawn += views.route({ at(PointerFact::Kind::Press, 2, 2.0, 3), at(PointerFact::Kind::Motion, 4, 1.0, 3), at(PointerFact::Kind::Release, 4, 1.0, 3) },
        "a line drag");
    {
        PointerFact extend = at(PointerFact::Kind::Press, 5, 3.0);
        extend.hasShift = true;
        PointerFact extendRelease = at(PointerFact::Kind::Release, 5, 3.0);
        extendRelease.hasShift = true;
        drawn += views.route({ extend, extendRelease }, "a shift-extend");
    }
    {
        PointerFact press = at(PointerFact::Kind::Press, 0, 1.0);
        press.hasAlternate = true;
        PointerFact drag = at(PointerFact::Kind::Motion, 3, 7.0);
        drag.hasAlternate = true;
        PointerFact release = at(PointerFact::Kind::Release, 3, 7.0);
        release.hasAlternate = true;
        drawn += views.route({ press, drag, release }, "a block drag");
    }
    {
        PointerFact above = at(PointerFact::Kind::Motion, 0, 4.0);
        above.yInRows = -1.5;
        PointerFact below = at(PointerFact::Kind::Motion, 7, 4.0);
        below.yInRows = 9.5;
        drawn += views.route({ at(PointerFact::Kind::Press, 3, 2.2), above, below, at(PointerFact::Kind::Release, 7, 4.0) },
            "a drag past the frame's edges");
    }
    {
        PointerFact hover = at(PointerFact::Kind::Motion, 1, 5.0);
        hover.isDragging = false;
        PointerFact right = at(PointerFact::Kind::Press, 1, 5.0);
        right.button = PointerButton::Right;
        drawn += views.route({ hover, right }, "a hover and a right press");
    }
    CHECK(drawn > 0);
}

TEST(aFactAnEarlierRungCouldTakeIsRefusedOverAFrame)
{
    Views views;
    const TakenCapture frame = captureOf(views.terminal);
    const PointerFact press = at(PointerFact::Kind::Press, 0, 2.4);
    for (const PublishedPointerModes& modes : {
             PublishedPointerModes { .isMouseReporting = true },
             PublishedPointerModes { .isRelativePointerLock = true },
             PublishedPointerModes { .isLocatorActive = true },
             PublishedPointerModes { .isTekModeActive = true },
             PublishedPointerModes { .isReadlinePointerActive = true },
         }) {
        CHECK(!routePointerOverCapture(views.capturedLadder, *frame, &views.captured, press, views.policy, modes));
        CHECK(!views.capturedLadder.isSelectionGestureOpen);
        CHECK(!Selection::hasSelection(views.captured));
    }
    views.capturedLadder.isHiliteGestureActive = true;
    CHECK(!routePointerOverCapture(views.capturedLadder, *frame, &views.captured, press, views.policy, {}));
    views.capturedLadder.isHiliteGestureActive = false;
    CHECK(!routePointerOverCapture(views.capturedLadder, *frame, nullptr, press, views.policy, {}));

    // A frame holding a sized glyph is the terminal's to expand over.
    Terminal sized;
    hardReset(sized);
    resize(sized, 6, 30);
    sized.drawsSizedText = true;
    feed(sized, "\033]66;s=2;AB\007 and more");
    const TakenCapture sizedFrame = captureOf(sized);
    CHECK(!routePointerOverCapture(views.capturedLadder, *sizedFrame, &views.captured, press, views.policy, {}));
    CHECK(!Selection::hasSelection(views.captured));
}

UBIQUITTY_TEST_MAIN()
