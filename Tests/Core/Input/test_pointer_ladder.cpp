// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The pointer ladder: one fact in, one verdict out, ranked by the terminal's
// modes. The cases here are the questions on which hand-kept ladders disagree
// — which modifier keeps a drag local, and whether a reported press drops the
// selection — asked of the one ladder every frontend runs.

#include "Harness.h"

#include "Core/Input/PointerLadder.h"
#include "Core/Mouse/Locator.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/RichText/Blocks.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <array>
#include <string>
#include <string_view>

using namespace Ubiquitty;
using namespace Ubiquitty::Input;
using UbiquittyTest::feed;

namespace {

struct Bench {
    Terminal terminal;
    Selection::Session session;
    PointerLadder ladder;
    PointerPolicy policy;

    Bench()
    {
        hardReset(terminal);
        resize(terminal, 6, 40);
        feed(terminal, "hello ladder world");
        Selection::init(session);
    }

    PointerRoute route(const PointerFact& fact)
    {
        return routePointer(ladder, terminal, &session, fact, policy, /*nowTicks=*/0);
    }
};

PointerFact fact(PointerFact::Kind kind, int row, int column)
{
    PointerFact f;
    f.kind = kind;
    f.button = PointerButton::Left;
    f.row = row;
    f.column = column;
    f.clickCount = 1;
    f.isDragging = kind == PointerFact::Kind::Motion;
    return f;
}

std::string_view report(const PointerRoute& route)
{
    return { route.report.data(), static_cast<std::size_t>(route.reportLength) };
}

std::string_view locatorReport(const PointerRoute& route)
{
    return { route.locatorReport.data(), static_cast<std::size_t>(route.locatorReportLength) };
}

// A shaper laying four-pixel letters in eight-pixel cells: two letters' ink a
// cell, which is a packed cell a caret has to reach inside.
struct HalfCellLetterMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = 4.0f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

}

// A fact carries where along the row the pointer is, and the ladder resolves
// the caret from it: inside a packed proportional cell that names a letter,
// and the gesture is by letters — the pressed one is selected as soon as the
// drag moves, and the next joins when the pointer reaches it.
TEST(aCaretFactInsideAPackedCellSelectsOneLetterThroughTheLadder)
{
    Bench bench;
    bench.terminal.drawsSizedText = true;
    bench.terminal.drawsProportionalText = true;
    HalfCellLetterMeasurer measurer;
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(bench.terminal, "\033[2;1H\033]66;p=1:c=4;abcdefgh\007");

    // The fact a frontend builds: the cell under the pointer, and the point
    // itself, unrounded.
    const auto caretFact = [](PointerFact::Kind kind, double xInCells) {
        PointerFact built = fact(kind, 1, static_cast<int>(xInCells));
        built.xInCells = xInCells;
        return built;
    };

    // Down on the a, a motion still on it — a quarter of a cell of travel,
    // which a gesture addressing cells could not tell from none — then onto
    // the b, half a cell in.
    CHECK(bench.route(caretFact(PointerFact::Kind::Press, 0.4 / 8.0)).didOpenSelectionGesture);
    CHECK_MSG(bench.route(caretFact(PointerFact::Kind::Motion, 2.1 / 8.0)).didChangeSelection,
        "the first motion on the pressed letter opens the selection");
    const std::string one = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(one == "a", ("the pressed letter, alone: " + one).c_str());
    CHECK(bench.route(caretFact(PointerFact::Kind::Motion, 4.1 / 8.0)).didChangeSelection);
    const std::string two = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(two == "ab", ("the letter the pointer reached joins: " + two).c_str());
    // Back past the press: the selection runs from the far edge of the
    // pressed letter to the letter under the pointer, so it never empties.
    CHECK(bench.route(caretFact(PointerFact::Kind::Motion, 0.2 / 8.0)).didChangeSelection);
    const std::string back = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(back == "a", ("back on the pressed letter it is still selected: " + back).c_str());
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aWordDragInsideAProportionalRunKeepsBothWordsWhole)
{
    Bench bench;
    bench.terminal.drawsSizedText = true;
    bench.terminal.drawsProportionalText = true;
    HalfCellLetterMeasurer measurer;
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(bench.terminal, "\033[2;1H\033]66;p=1:c=10;aaaa beta gamma dddd\007");
    const auto wordFact = [](PointerFact::Kind kind, double xInCells) {
        PointerFact built = fact(kind, 1, static_cast<int>(xInCells));
        built.xInCells = xInCells;
        built.clickCount = 2;
        return built;
    };
    CHECK(bench.route(wordFact(PointerFact::Kind::Press, 3.2)).didOpenSelectionGesture);
    CHECK(Selection::selectedText(bench.session, bench.terminal) == "beta");
    CHECK(bench.route(wordFact(PointerFact::Kind::Motion, 6.2)).didChangeSelection);
    const std::string selected = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(selected == "beta gamma", selected.c_str());
    CHECK(bench.route(wordFact(PointerFact::Kind::Motion, 3.2)).didChangeSelection);
    CHECK(Selection::selectedText(bench.session, bench.terminal) == "beta");
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aReverseProportionalWordDragResolvesTheWordUnderAStationaryPointerAfterOutput)
{
    Bench bench;
    bench.terminal.drawsSizedText = true;
    bench.terminal.drawsProportionalText = true;
    HalfCellLetterMeasurer measurer;
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(bench.terminal, "\033[2;1H\033]66;p=1:c=10;aaaa beta\007"
                         "\033[3;1H\033]66;p=1:c=10;short wideword\007"
                         "\033[4;1H\033]66;p=1:c=10;gamma dddd\007");
    const auto wordFact = [](PointerFact::Kind kind, int row, double xInCells) {
        PointerFact built = fact(kind, row, static_cast<int>(xInCells));
        built.xInCells = xInCells;
        built.yInRows = row + 0.5;
        built.clickCount = 2;
        return built;
    };
    CHECK(bench.route(wordFact(PointerFact::Kind::Press, 3, 1.2)).didOpenSelectionGesture);
    CHECK(bench.route(wordFact(PointerFact::Kind::Motion, 1, 3.2)).didChangeSelection);
    CHECK(Selection::selectedText(bench.session, bench.terminal) == "beta\nshort wideword\ngamma");
    CHECK(!Selection::updateAfterOutput(bench.session, bench.terminal, 0).didChangeSelection);
    feed(bench.terminal, "\033[6;1H\r\n");
    CHECK(Selection::updateAfterOutput(bench.session, bench.terminal, 0).didChangeSelection);
    const std::string selected = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(selected == "wideword\ngamma", selected.c_str());
    bench.terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A mouse protocol is in cells and a reader's caret is not the program's
// cursor: whatever letter a caret stands before, the report an application
// receives names the cell the pointer is over.
TEST(aReportedPressNamesItsCellWhateverCaretStandsInsideIt)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[?1006h");
    PointerFact press = fact(PointerFact::Kind::Press, 2, 3);
    press.xInCells = 3.7;
    CHECK_MSG(report(bench.route(press)) == "\033[<0;4;3M", "the report names the cell, not the point inside it");
}

// A gesture opened on carets moves on carets to its end. A frontend that
// could not measure one motion's point hands the ladder the cell alone, and
// the head goes to that cell's boundary rather than the gesture falling to
// cells: routed on cells, the model would re-anchor on the lower end it keeps,
// which on a drag towards the left is the pointer's side, and the selection
// would collapse under the pointer.
TEST(aCaretDragTowardsTheLeftSurvivesAMotionWithoutAMeasuredPoint)
{
    Bench bench;
    feed(bench.terminal, "\033[2;1Habcdefghij");
    const auto caretFact = [](PointerFact::Kind kind, double xInCells) {
        PointerFact built = fact(kind, 1, static_cast<int>(xInCells));
        built.xInCells = xInCells;
        return built;
    };
    CHECK(bench.route(caretFact(PointerFact::Kind::Press, 7.4)).didOpenSelectionGesture);
    CHECK(bench.route(caretFact(PointerFact::Kind::Motion, 3.1)).didChangeSelection);
    std::string text = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(text == "defg", ("leftwards from the boundary before h to the one before d: " + text).c_str());
    PointerFact unmeasured = fact(PointerFact::Kind::Motion, 1, 2);
    CHECK(bench.route(unmeasured).didChangeSelection);
    text = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(text == "cdefg", ("the head moves to the cell's boundary and the anchor holds: " + text).c_str());
    CHECK(bench.route(caretFact(PointerFact::Kind::Motion, 1.2)).didChangeSelection);
    text = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(text == "bcdefg", ("and the next measured point carries on from there: " + text).c_str());
}

TEST(withNoReportingModeTheLadderRunsTheLocalGesture)
{
    Bench bench;
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 0, 0));
    CHECK_MSG(press.wasConsumed && press.didOpenSelectionGesture, "an unreported press opens the local gesture");
    CHECK_MSG(press.reportLength == 0, "and sends the host nothing");
    CHECK(bench.ladder.isSelectionGestureOpen);

    const PointerRoute drag = bench.route(fact(PointerFact::Kind::Motion, 0, 4));
    CHECK_MSG(drag.didChangeSelection, "a drag extends it");
    CHECK(Selection::selectedText(bench.session, bench.terminal) == "hello");

    const PointerRoute release = bench.route(fact(PointerFact::Kind::Release, 0, 4));
    CHECK_MSG(release.didFinishSelectionGesture, "the release finishes a gesture that selected");
    CHECK(!bench.ladder.isSelectionGestureOpen);
    CHECK_MSG(!bench.route(fact(PointerFact::Kind::Motion, 0, 9)).didChangeSelection, "a straggling drag extends nothing");
}

TEST(aReportedPressGoesToTheHostAndDropsTheSelection)
{
    Bench bench;
    bench.route(fact(PointerFact::Kind::Press, 0, 0));
    bench.route(fact(PointerFact::Kind::Motion, 0, 4));
    bench.route(fact(PointerFact::Kind::Release, 0, 4));
    CHECK(Selection::hasSelection(bench.session));

    feed(bench.terminal, "\033[?1000h\033[?1006h");
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 2, 3));
    CHECK_MSG(press.wasConsumed, "the press is the host's");
    CHECK_MSG(report(press) == "\033[<0;4;3M", "as an SGR press report");
    CHECK_MSG(!press.didOpenSelectionGesture, "no local gesture opens");
    CHECK_MSG(!Selection::hasSelection(bench.session), "and the standing selection is input to the host, so it drops");
    CHECK_MSG(press.didChangeSelection, "which the frontend is told to redraw");

    const PointerRoute release = bench.route(fact(PointerFact::Kind::Release, 2, 3));
    CHECK(report(release) == "\033[<0;4;3m");
}

TEST(theWindowActivatingPressKeepsTheSelectionAndOpensNothing)
{
    Bench bench;
    bench.route(fact(PointerFact::Kind::Press, 0, 0));
    bench.route(fact(PointerFact::Kind::Motion, 0, 4));
    bench.route(fact(PointerFact::Kind::Release, 0, 4));

    PointerFact activating = fact(PointerFact::Kind::Press, 1, 1);
    activating.isWindowActivating = true;
    const PointerRoute press = bench.route(activating);
    CHECK_MSG(!press.didOpenSelectionGesture, "the click that activates the window opens no gesture");
    CHECK_MSG(Selection::hasSelection(bench.session), "and keeps the selection");
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Press, 1, 1)).didOpenSelectionGesture, "the next press is an ordinary one");
}

TEST(shiftKeepsAPressLocalUnderReporting)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h");
    PointerFact shifted = fact(PointerFact::Kind::Press, 0, 0);
    shifted.hasShift = true;
    const PointerRoute press = bench.route(shifted);
    CHECK_MSG(press.reportLength == 0, "shift is the user's override: nothing reaches the host");
    CHECK_MSG(press.didOpenSelectionGesture, "and the local gesture opens");
}

namespace {

enum class TopEdgePosition {
    Inside,
    Above,
};

void checkShiftDragToTopRow(TopEdgePosition topEdgePosition)
{
    enum class Viewport {
        Live,
        History,
        Alternate,
    };
    for (const Viewport viewport : std::array { Viewport::Live, Viewport::History, Viewport::Alternate }) {
        Bench bench;
        hardReset(bench.terminal);
        resize(bench.terminal, 6, 40);
        int viewOffset = 0;
        if (viewport == Viewport::Live) {
            feed(bench.terminal, "TOP\r\nmiddle\r\nbottom");
        } else {
            feed(bench.terminal, "hidden0\r\nhidden1\r\nTOP\r\nmiddle\r\nbottom\r\nlater0\r\nlater1\r\nlater2\r\nlater3");
            if (viewport == Viewport::History) {
                viewOffset = 1;
            } else {
                feed(bench.terminal, "\033[?1049h\033[HTOP\r\nmiddle\r\nbottom");
            }
        }
        feed(bench.terminal, "\033[?1002h\033[?1006h");
        PointerFact press = fact(PointerFact::Kind::Press, 2, 4);
        press.hasShift = true;
        press.viewOffset = viewOffset;
        press.xInCells = 4.25;
        press.yInRows = 2.25;
        const PointerRoute opened = bench.route(press);
        CHECK(opened.didOpenSelectionGesture);
        CHECK_EQ(opened.reportLength, 0);

        PointerFact drag = fact(PointerFact::Kind::Motion, 0, topEdgePosition == TopEdgePosition::Above ? 30 : 0);
        drag.hasShift = true;
        drag.viewOffset = viewOffset;
        drag.xInCells = topEdgePosition == TopEdgePosition::Above ? 30.25 : 0.25;
        drag.yInRows = topEdgePosition == TopEdgePosition::Above ? -0.25 : 0.25;
        const PointerRoute moved = bench.route(drag);
        CHECK(moved.didChangeSelection);
        CHECK_EQ(moved.reportLength, 0);

        const std::string selected = Selection::selectedText(bench.session, bench.terminal);
        const char* scenario = viewport == Viewport::Live ? "live" : viewport == Viewport::History ? "history"
                                                                                                   : "alternate";
        CHECK_MSG(selected == "TOP\nmiddle\nbotto", (std::string(scenario) + ": " + selected).c_str());
        std::array<Rendering::SelectionRowSpan, 6> spans {};
        const int count = Selection::viewportSpans(bench.session, bench.terminal, viewOffset, spans);
        CHECK_EQ(count, 3);
        if (count > 0) {
            CHECK_EQ(spans[0].visibleRow, 0);
            CHECK_EQ(spans[0].column0, 0);
        }
    }
}

}

TEST(aShiftOverrideDragIncludesTheFirstVisibleRow)
{
    checkShiftDragToTopRow(TopEdgePosition::Inside);
}

TEST(aShiftOverrideDragAboveTheGridIncludesTheWholeFirstRow)
{
    checkShiftDragToTopRow(TopEdgePosition::Above);
}

TEST(aCaretDragAboveTheGridIncludesTheWholeFirstRow)
{
    Bench bench;
    feed(bench.terminal, "\033[2J\033[HTOP\r\nmiddle\r\nbottom");
    PointerFact press = fact(PointerFact::Kind::Press, 2, 4);
    press.xInCells = 4.25;
    press.yInRows = 2.25;
    CHECK(bench.route(press).didOpenSelectionGesture);
    PointerFact drag = fact(PointerFact::Kind::Motion, 0, 30);
    drag.xInCells = 30.25;
    drag.yInRows = -0.25;
    CHECK(bench.route(drag).didChangeSelection);
    CHECK(Selection::selectedText(bench.session, bench.terminal) == "TOP\nmiddle\nbott");
}

TEST(aLinearDragBelowTheGridIncludesTheWholeLastRow)
{
    enum class Gesture {
        Caret,
        ShiftOverride,
    };
    for (const Gesture gesture : std::array { Gesture::Caret, Gesture::ShiftOverride }) {
        Bench bench;
        feed(bench.terminal, "\033[2J\033[4;1Hmiddle\r\nnext\r\nBOTTOM");
        if (gesture == Gesture::ShiftOverride) {
            feed(bench.terminal, "\033[?1002h\033[?1006h");
        }
        PointerFact press = fact(PointerFact::Kind::Press, 3, 0);
        press.hasShift = gesture == Gesture::ShiftOverride;
        press.xInCells = 0.25;
        press.yInRows = 3.25;
        CHECK(bench.route(press).didOpenSelectionGesture);
        PointerFact drag = fact(PointerFact::Kind::Motion, 5, 0);
        drag.hasShift = gesture == Gesture::ShiftOverride;
        drag.xInCells = 0.25;
        drag.yInRows = 6.25;
        const PointerRoute moved = bench.route(drag);
        CHECK(moved.didChangeSelection);
        CHECK_EQ(moved.reportLength, 0);
        CHECK(Selection::selectedText(bench.session, bench.terminal) == "middle\nnext\nBOTTOM");
        std::array<Rendering::SelectionRowSpan, 6> spans {};
        const int count = Selection::viewportSpans(bench.session, bench.terminal, 0, spans);
        CHECK_EQ(count, 3);
        if (count == 3) {
            CHECK_EQ(spans[2].visibleRow, 5);
            CHECK_EQ(spans[2].column1, 39);
        }
    }
}

TEST(aRectangularDragBeyondVerticalEdgesKeepsItsColumns)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    PointerFact press = fact(PointerFact::Kind::Press, 2, 4);
    press.hasAlternate = true;
    press.xInCells = 4.25;
    press.yInRows = 2.25;
    CHECK(bench.route(press).didOpenSelectionGesture);
    PointerFact drag = fact(PointerFact::Kind::Motion, 0, 30);
    drag.hasAlternate = true;
    drag.xInCells = 30.25;
    drag.yInRows = -0.25;
    CHECK(bench.route(drag).didChangeSelection);
    std::array<Rendering::SelectionRowSpan, 6> spans {};
    int count = Selection::viewportSpans(bench.session, bench.terminal, 0, spans);
    CHECK_EQ(count, 3);
    for (int index = 0; index < count; ++index) {
        CHECK_EQ(spans[static_cast<std::size_t>(index)].column0, 4);
        CHECK_EQ(spans[static_cast<std::size_t>(index)].column1, 30);
    }
    drag.row = 5;
    drag.column = 1;
    drag.xInCells = 1.25;
    drag.yInRows = 6.25;
    CHECK(bench.route(drag).didChangeSelection);
    count = Selection::viewportSpans(bench.session, bench.terminal, 0, spans);
    CHECK_EQ(count, 4);
    for (int index = 0; index < count; ++index) {
        CHECK_EQ(spans[static_cast<std::size_t>(index)].column0, 1);
        CHECK_EQ(spans[static_cast<std::size_t>(index)].column1, 4);
    }
}

TEST(reportedDragsBeyondVerticalEdgesKeepTheirCellCoordinates)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    CHECK(report(bench.route(fact(PointerFact::Kind::Press, 2, 4))) == "\033[<0;5;3M");
    PointerFact drag = fact(PointerFact::Kind::Motion, 0, 30);
    drag.xInCells = 30.25;
    drag.yInRows = -0.25;
    CHECK(report(bench.route(drag)) == "\033[<32;31;1M");
    drag.row = 5;
    drag.column = 0;
    drag.xInCells = 0.25;
    drag.yInRows = 6.25;
    CHECK(report(bench.route(drag)) == "\033[<32;1;6M");
    CHECK(!Selection::hasSelection(bench.session));
}

// A touch surface has no modifier to hold, so the platform's own selection
// gesture is how a reader selects under an application that tracks the mouse:
// it stays local from press to release, as a Shift drag does, and the
// application hears none of it. The same facts without the gesture are an
// ordinary double-click drag, which the application receives.
TEST(thePlatformSelectionGestureSelectsLocallyUnderReporting)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[?1002h\033[?1006h");
    const auto gesture = [](PointerFact::Kind kind, int column) {
        PointerFact built = fact(kind, 0, column);
        built.clickCount = 2;
        built.isSelectionGesture = true;
        return built;
    };
    const PointerRoute press = bench.route(gesture(PointerFact::Kind::Press, 0));
    CHECK_MSG(press.reportLength == 0, "the press reaches no application");
    CHECK_MSG(press.didOpenSelectionGesture, "and opens the local gesture");
    const PointerRoute drag = bench.route(gesture(PointerFact::Kind::Motion, 10));
    CHECK_MSG(drag.reportLength == 0, "the drag reaches no application");
    CHECK_MSG(drag.didChangeSelection, "and grows the selection");
    const PointerRoute release = bench.route(gesture(PointerFact::Kind::Release, 10));
    CHECK_MSG(release.reportLength == 0, "the release reaches no application");
    CHECK_MSG(release.didFinishSelectionGesture, "and ends the gesture");
    const std::string selected = Selection::selectedText(bench.session, bench.terminal);
    CHECK_MSG(selected == "hello ladder", ("the words dragged across are selected: " + selected).c_str());
    // Having selected through the application once, the reader is not taught
    // to reach through: a later drag the application takes shows no lesson.
    bench.route(fact(PointerFact::Kind::Press, 2, 2));
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Motion, 2, 6)).hint == Hint::None, "the gesture is the lesson landing");
    bench.route(fact(PointerFact::Kind::Release, 2, 6));

    Bench desktop;
    feed(desktop.terminal, "\033[?1000h\033[?1002h\033[?1006h");
    PointerFact doubleClick = fact(PointerFact::Kind::Press, 0, 0);
    doubleClick.clickCount = 2;
    const PointerRoute reported = desktop.route(doubleClick);
    CHECK_MSG(report(reported) == "\033[<0;1;1M", "a double click that is not the platform's gesture is the application's");
    CHECK(!reported.didOpenSelectionGesture);
    PointerFact doubleClickRelease = fact(PointerFact::Kind::Release, 0, 0);
    doubleClickRelease.clickCount = 2;
    desktop.route(doubleClickRelease);
    desktop.route(fact(PointerFact::Kind::Press, 2, 2));
    CHECK_MSG(desktop.route(fact(PointerFact::Kind::Motion, 2, 6)).hint != Hint::None,
        "a double click the application took has not reached through, so the lesson still stands");
}

TEST(commandReachesALinkAndNotASelection)
{
    // Command reaching a selection as well makes two gestures out of one
    // chord: over a tracking application a Command-drag both opens the link
    // under the press and selects the text it sits in. The link gesture is
    // resolved by the frontend that owns the link; what the ladder owes is to
    // report the press like any other.
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    PointerFact press = fact(PointerFact::Kind::Press, 0, 0);
    press.hasCommand = true;
    CHECK_MSG(bench.route(press).reportLength > 0, "a Command press is reported like any other");
    CHECK_MSG(!bench.route(press).didOpenSelectionGesture, "and opens no selection");
}

TEST(theBlockChordReachesARectangularSelectionAndFollowsThePreference)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    PointerFact press = fact(PointerFact::Kind::Press, 0, 0);
    press.hasAlternate = true;
    CHECK_MSG(bench.route(press).didOpenSelectionGesture, "the default chord is Option, and it reaches through");
    PointerFact drag = fact(PointerFact::Kind::Motion, 1, 5);
    drag.hasAlternate = true;
    bench.route(drag);
    CHECK_MSG(Selection::isRectangular(&bench.session.state), "and what it drags out is a block");

    // Moved to another chord, Option alone is the application's again — matched
    // exactly, so the chord it was cannot still fire.
    Bench moved;
    feed(moved.terminal, "\033[?1002h\033[?1006h");
    moved.policy.blockSelectChord = chordFromSpec("shift+opt");
    CHECK_MSG(moved.route(press).reportLength > 0, "a bare Option press reports once the chord has moved");
    PointerFact configured = press;
    configured.hasShift = true;
    CHECK_MSG(moved.route(configured).didOpenSelectionGesture, "and the configured chord reaches through instead");
    PointerFact configuredDrag = drag;
    configuredDrag.hasShift = true;
    moved.route(configuredDrag);
    CHECK_MSG(Selection::isRectangular(&moved.session.state), "still as a block");

    // A chord naming nothing is the gesture switched off, and must not arm on
    // the bare pointer, which names nothing either.
    Bench off;
    feed(off.terminal, "\033[?1002h\033[?1006h");
    off.policy.blockSelectChord = chordFromSpec("");
    CHECK_MSG(off.route(fact(PointerFact::Kind::Press, 0, 0)).reportLength > 0, "an empty chord arms no block");
}

TEST(theMotionDedupSendsOneReportPerCell)
{
    Bench bench;
    feed(bench.terminal, "\033[?1003h\033[?1006h");
    PointerFact move = fact(PointerFact::Kind::Motion, 1, 1);
    move.isDragging = false;
    move.button = PointerButton::None;
    const PointerRoute first = bench.route(move);
    CHECK_MSG(report(first) == "\033[<35;2;2M", "any-event tracking reports a button-less move as button 3 with the motion bit");
    CHECK_MSG(bench.route(move).reportLength == 0, "the same cell again is not repeated");
    move.column = 2;
    CHECK_MSG(bench.route(move).reportLength > 0, "the next cell is");
}

TEST(theHiliteGestureArmsOnThePressAndEndsOnTheRelease)
{
    Bench bench;
    feed(bench.terminal, "\033[?1001h");
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 1, 1));
    CHECK_MSG(press.reportLength > 0, "the press is reported");
    CHECK_MSG(bench.ladder.isHiliteGestureActive, "and the gesture is in flight");
    CHECK_MSG(bench.terminal.modes.hiliteAwaiting, "with the bounded await armed");
    CHECK_MSG(press.needsRepublish, "which the frontend must publish, since no host output follows");

    PointerFact outside = fact(PointerFact::Kind::Release, -1, -1);
    const PointerRoute release = bench.route(outside);
    CHECK_MSG(release.wasConsumed && release.reportLength > 0, "the release carries the end report");
    CHECK_MSG(report(release).rfind("\033[t", 0) == 0, "in XTHIMOUSE's same-cell framing: outside the grid, the end falls back to the press cell");
    CHECK_MSG(!bench.ladder.isHiliteGestureActive, "and the gesture is over");
    CHECK_MSG(!bench.terminal.modes.hiliteAwaiting, "with the await torn down");
}

TEST(tekGINTakesThePressAsAVerdictForTheFrontend)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[?38h\033\032"); // Tek mode, then ESC SUB arms GIN
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 2, 5));
    CHECK_MSG(press.wantsTekGIN && press.wasConsumed, "the press is the GIN trigger");
    CHECK_MSG(press.reportLength == 0, "not an xterm report, whatever mode the host left on");
    CHECK_MSG(!press.didOpenSelectionGesture, "and not a selection: the frontend converts the point to Tek space itself");
}

TEST(theLocatorTakesThePressBeforeAnyOtherRung)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[1;1'z\033[1'{"); // DECELR on, DECSLE button-down
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 2, 5));
    CHECK_MSG(press.wasConsumed, "the locator's press is consumed");
    CHECK_MSG(locatorReport(press).rfind("\033[", 0) == 0 && locatorReport(press).back() == 'w', "as a DECLRP");
    CHECK_MSG(press.reportLength == 0, "and the xterm rung below it never runs");
    CHECK_MSG(!press.didOpenSelectionGesture, "nor the local one");
}

// DECSLE picks the transitions and DECELR the lifetime; the ladder answers in
// the locator's own coordinates, one-based, with the held buttons in DEC's
// bit order. The fact is what a frontend builds from its event -- the cell
// and the held set -- so this is every frontend's answer.
TEST(theLocatorReportsTheTransitionsDECSLESelectsAndAOneShotDisarmsOnItsFirst)
{
    Bench bench;
    feed(bench.terminal, "\033[1;2'z\033[3'{"); // continuous, in cells; releases only
    PointerFact press = fact(PointerFact::Kind::Press, 2, 5);
    press.heldButtonBits = Mouse::Locator::buttonBit(0);
    CHECK_MSG(bench.route(press).locatorReportLength == 0, "DECSLE did not select the press");
    const PointerRoute release = bench.route(fact(PointerFact::Kind::Release, 2, 5));
    CHECK_MSG(locatorReport(release) == "\033[3;0;3;6;1&w", "the release is reported: Pe 3, nothing held, row 3, column 6");
    CHECK_MSG(release.wasConsumed, "and is the locator's, not the selection's");
    CHECK_MSG(bench.terminal.locatorEnabled, "a continuous locator stays armed");

    feed(bench.terminal, "\033[2;2'z\033[1;3'{"); // one-shot, both transitions
    PointerFact second = fact(PointerFact::Kind::Press, 4, 9);
    second.heldButtonBits = Mouse::Locator::buttonBit(0);
    CHECK_MSG(locatorReport(bench.route(second)) == "\033[2;4;5;10;1&w", "a one-shot locator reports its first transition, left held");
    CHECK_MSG(!bench.terminal.locatorEnabled, "and disarms on it");
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Release, 4, 9)).locatorReportLength == 0,
        "so the release that follows is no longer the locator's");
}

// The arm edge: a frontend tells the locator where a pointer that has not
// moved stands, so a DECRQLP before the next move answers a position rather
// than an undefined one. Only the locator hears it -- a stationary pointer is not a move
// to any-event tracking or to the selection.
TEST(aSeededLocatorAnswersDECRQLPAndNothingElseHearsTheSeed)
{
    Bench bench;
    feed(bench.terminal, "\033[?1003h\033[?1006h\033[1;2'z"); // any-event tracking, DECELR in cells
    char reply[64] = {};
    feed(bench.terminal, "\033['|");
    UbiquittyTest::drain(bench.terminal, reply);
    CHECK_MSG(std::string_view(reply) == "\033[1;0&w", "armed but never told where the pointer is: Pe 1, the position undefined");

    PointerFact stationary = fact(PointerFact::Kind::Press, 2, 5); // the kind is not read
    stationary.heldButtonBits = Mouse::Locator::buttonBit(0);
    const PointerRoute seeded = seedLocator(bench.terminal, stationary);
    CHECK_MSG(seeded.locatorReportLength == 0, "with no rectangle armed the seed is silent");
    CHECK_MSG(seeded.reportLength == 0 && !seeded.wasConsumed && !seeded.didChangeSelection,
        "and no rung below the locator runs");
    feed(bench.terminal, "\033['|");
    UbiquittyTest::drain(bench.terminal, reply);
    CHECK_MSG(std::string_view(reply) == "\033[1;4;3;6;1&w", "DECRQLP answers the pointer: Pe 1, left held, row 3, column 6");
}

// A DECEFR in the same drain as the arm can leave the pointer already outside
// its rectangle; the seed then owes the leave, and returns it to be sent.
TEST(aSeedOutsideARectangleArmedBesideItReturnsTheLeave)
{
    Bench bench;
    feed(bench.terminal, "\033[1;2'z\033[1;1;1;1'w"); // DECELR, then a one-cell rectangle at the origin
    const PointerRoute seeded = seedLocator(bench.terminal, fact(PointerFact::Kind::Motion, 2, 5));
    CHECK_MSG(locatorReport(seeded) == "\033[10;0;3;6;1&w", "Pe 10 at the pointer, row 3, column 6");
    CHECK_MSG(!bench.terminal.locatorFilterActive, "and the rectangle disarms");
}

TEST(aWindowDragPressMatchesTheSpecExactly)
{
    PointerFact press = fact(PointerFact::Kind::Press, 0, 0);
    press.hasCommand = true;
    press.hasShift = true;
    CHECK(isWindowDragPress(press, "cmd+shift", WindowChrome::Titled));
    CHECK(isWindowDragPress(press, "super+shift", WindowChrome::Titled));
    CHECK_MSG(!isWindowDragPress(press, "cmd", WindowChrome::Titled), "an extra modifier held is a different gesture");
    press.hasControl = true;
    CHECK_MSG(!isWindowDragPress(press, "cmd+shift", WindowChrome::Titled), "so is one the spec does not name");
    CHECK_MSG(!isWindowDragPress(press, "", WindowChrome::Titled), "an empty spec drags nothing");
    press.kind = PointerFact::Kind::Motion;
    CHECK_MSG(!isWindowDragPress(press, "cmd+shift+ctrl", WindowChrome::Titled), "only a press starts the drag");
}

TEST(aChromelessWindowIsDraggedByCommandWhateverTheSpecSays)
{
    // No title bar means no other way to move the window, so Command over the
    // body stands in for one -- and only there: a window with a title bar keeps
    // Command for opening links and reaching through mouse reporting.
    const HeldModifiers command { .hasCommand = true };
    CHECK(windowDragArmed(command, "", WindowChrome::Chromeless));
    CHECK_MSG(!windowDragArmed(command, "", WindowChrome::Titled), "a titled window is not dragged by Command");
    CHECK_MSG(windowDragArmed(command, "ctrl+cmd", WindowChrome::Chromeless), "and the spec does not narrow the chromeless way in");

    // Asked without a press, for the pointer that advertises the gesture
    // before it is made.
    const HeldModifiers controlCommand { .hasCommand = true, .hasControl = true };
    CHECK(windowDragArmed(controlCommand, "ctrl+cmd", WindowChrome::Titled));
    CHECK_MSG(!windowDragArmed(HeldModifiers {}, "ctrl+cmd", WindowChrome::Titled), "and nothing held arms nothing");
    CHECK_MSG(!windowDragArmed(HeldModifiers {}, "", WindowChrome::Chromeless), "including in a chromeless window");

    // The spec is parsed to a chord and the chord is matched exactly, which is
    // what keeps two gestures on neighbouring chords from both firing.
    CHECK(chordFromSpec("ctrl+opt") == (HeldModifiers { .hasControl = true, .hasAlternate = true }));
    CHECK_MSG(chordFromSpec("OPT") == chordFromSpec("alt"), "a name is a name whatever its case or its platform's word for it");
    CHECK_MSG(!chordArmed(HeldModifiers { .hasAlternate = true }, chordFromSpec("shift+opt")),
        "Option alone does not arm a chord that also names Shift");
    CHECK_MSG(!chordArmed(HeldModifiers { .hasAlternate = true, .hasShift = true }, chordFromSpec("opt")),
        "and Shift+Option does not arm the chord that names Option alone");
    CHECK_MSG(!chordArmed(HeldModifiers {}, chordFromSpec("")), "a chord naming nothing is a gesture switched off");
    CHECK_MSG(!chordArmed(HeldModifiers {}, chordFromSpec("nonsense")), "and so is one naming what no keyboard has");
}

TEST(aLockedPointerReportsItsDeltasAndNothingElse)
{
    Bench bench;
    bench.route(fact(PointerFact::Kind::Press, 0, 0));
    bench.route(fact(PointerFact::Kind::Motion, 0, 4));
    bench.route(fact(PointerFact::Kind::Release, 0, 4));
    CHECK(Selection::hasSelection(bench.session));

    feed(bench.terminal, "\033[?7403h");
    PointerFact move = fact(PointerFact::Kind::Motion, 2, 2);
    move.deltaX = 3;
    move.deltaY = -2;
    const PointerRoute noFrame = bench.route(move);
    CHECK_MSG(noFrame.wasConsumed && noFrame.reportLength == 0, "without SGR-pixels the lock has no frame to ride, and the event is swallowed");

    feed(bench.terminal, "\033[?1016h");
    const PointerRoute delta = bench.route(move);
    CHECK_MSG(report(delta) == "\033[<32;32771;32766M", "the delta rides the locked frame, biased by 0x8000, as a button-less motion");
    PointerFact still = move;
    still.deltaX = 0;
    still.deltaY = 0;
    CHECK_MSG(bench.route(still).reportLength == 0, "a sample with no motion sends no zero delta");
    const PointerRoute press = bench.route(fact(PointerFact::Kind::Press, 2, 2));
    CHECK_MSG(report(press) == "\033[<0;32768;32768M", "a button event carries a zero delta");
    CHECK_MSG(!press.didOpenSelectionGesture && Selection::hasSelection(bench.session), "and is the host's: no gesture, and the selection is not the host's to drop");
}

TEST(aPixelReportIsClampedToTheExtentTheProgramWasTold)
{
    // The clamp is Core's, to the extent CSI 14 t advertises: a frontend
    // clamping to the grid it fitted reports, under DECSET 7402, a space about
    // a percent smaller than the drawable the picture fills, and the pointer
    // overshoots the picture's edge.
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[?1016h");
    bench.terminal.cellPixelWidth = 9;
    bench.terminal.cellPixelHeight = 16;
    bench.terminal.textAreaPixelWidth = 360; // 40 columns of 9
    bench.terminal.textAreaPixelHeight = 96; // 6 rows of 16
    bench.terminal.drawablePixelWidth = 372; // the padding around the grid
    bench.terminal.drawablePixelHeight = 104;

    PointerFact past = fact(PointerFact::Kind::Press, 5, 39);
    past.pixelX = 500;
    past.pixelY = 200;
    PointerFact release = fact(PointerFact::Kind::Release, 5, 39);
    release.pixelX = 500;
    release.pixelY = 200;

    CHECK_MSG(report(bench.route(past)) == "\033[<0;360;96M", "past the grid's edge the pixel is the last one the program was told about");
    CHECK_MSG(report(bench.route(release)) == "\033[<0;360;96m", "and the release is clamped the same way");

    feed(bench.terminal, "\033[?7402h");
    CHECK_MSG(report(bench.route(past)) == "\033[<0;372;104M", "under DECSET 7402 the space is the drawable the picture fills");
    (void)bench.route(release);
    feed(bench.terminal, "\033]7402;fill=pass\033\\");
    CHECK_MSG(report(bench.route(past)) == "\033[<0;372;104M", "under either letterbox: the picture fills the drawable under both");
    (void)bench.route(release);

    feed(bench.terminal, "\033[?7402l");
    CHECK_MSG(report(bench.route(past)) == "\033[<0;360;96M", "the reset restores the grid's extent");
    (void)bench.route(release);

    // Nothing published clamps nothing: a frontend that never measured leaves
    // the pixel where it put it.
    bench.terminal.textAreaPixelWidth = 0;
    bench.terminal.textAreaPixelHeight = 0;
    CHECK_MSG(report(bench.route(past)) == "\033[<0;500;200M", "without a published extent the pixel passes through");
    (void)bench.route(release);
}

namespace {

Screen::ScrollTuning::Policy notchedTuning()
{
    Screen::ScrollTuning::Policy tuning;
    tuning.baseLinesPerTick = 3.0;
    tuning.pointsPerLine = 16.0;
    return tuning;
}

ScrollFact at(double deltaY)
{
    ScrollFact fact;
    fact.deltaY = deltaY;
    fact.row = 3;
    fact.column = 5;
    return fact;
}

// A notched wheel: whole lines per event.
ScrollFact wheelNotch(double deltaY) { return at(deltaY); }

// A continuous surface panning sideways: the horizontal residue banks in its
// own accumulator, so a diagonal pan cannot spend one axis on the other.
ScrollFact trackpadPan(double deltaX)
{
    ScrollFact fact;
    fact.deltaX = deltaX;
    fact.row = 3;
    fact.column = 5;
    fact.isPrecise = true;
    return fact;
}

// A continuous surface: points, folded and banked.
ScrollFact trackpadSwipe(double deltaY)
{
    ScrollFact fact = at(deltaY);
    fact.isPrecise = true;
    return fact;
}

std::string_view bytes(const ScrollRoute& route)
{
    return { route.bytes.data(), static_cast<std::size_t>(route.byteLength) };
}

}

TEST(theWheelScrollsTheViewWhenNothingTracks)
{
    Bench bench;
    const ScrollRoute notch = routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning());
    CHECK_MSG(notch.wasConsumed && notch.linesToOffsetView == 3 && notch.byteLength == 0, "a notch is three lines of history, no bytes");
    // Sub-row point deltas bank until they cover one complete row.
    const ScrollRoute swipe = routeScroll(bench.ladder, bench.terminal, trackpadSwipe(4.0), notchedTuning());
    CHECK_MSG(swipe.linesToOffsetView == 0 && !swipe.wasConsumed, "under a line of trackpad banks its residue");
    const ScrollRoute more = routeScroll(bench.ladder, bench.terminal, trackpadSwipe(12.0), notchedTuning());
    CHECK_MSG(more.linesToOffsetView == 1, "and the next sample completes the line");
    CHECK_MSG(!routeScroll(bench.ladder, bench.terminal, wheelNotch(0.0), notchedTuning()).wasConsumed, "no delta, no route");
}

TEST(theWheelReportsToATrackingHostOncePerNotchAndOncePerLineOfSwipe)
{
    Bench bench;
    feed(bench.terminal, "\033[?1000h\033[?1006h");
    const ScrollRoute notch = routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning());
    CHECK_MSG(bytes(notch) == "\033[<64;6;4M", "one wheel-up press at the cell");
    CHECK_MSG(notch.linesToOffsetView == 0, "and the view holds");
    const ScrollRoute flick = routeScroll(bench.ladder, bench.terminal, trackpadSwipe(-16.0), notchedTuning());
    CHECK_MSG(bytes(flick) == "\033[<65;6;4M", "one row of point displacement produces one wheel-down press");
}

// Each axis banks its own residue.
//
// A diagonal pan samples both axes on every event, and each sample is a
// fraction of a line. One accumulator would spend the vertical residue on a
// horizontal sample and the other way about, so a slow diagonal drift would
// step whichever axis happened to be asked last — and a pan that never quite
// reaches a line on either axis would still move, in the wrong direction.
TEST(theTwoAxesBankTheirResiduesApart)
{
    Bench bench;
    // One quarter of a line of vertical residue, banked and not spent.
    CHECK_MSG(routeScroll(bench.ladder, bench.terminal, trackpadSwipe(4.0), notchedTuning()).linesToOffsetView == 0,
        "under a line of vertical banks");

    // A horizontal sample of the same size must not complete it: the view is
    // scrolled by the vertical axis alone, and this axis has its own bank.
    CHECK_MSG(routeScroll(bench.ladder, bench.terminal, trackpadPan(4.0), notchedTuning()).linesToOffsetView == 0,
        "a horizontal sample does not spend the vertical bank");

    // The vertical bank is still where it was, so its own next sample is what
    // completes the line.
    CHECK_MSG(routeScroll(bench.ladder, bench.terminal, trackpadSwipe(12.0), notchedTuning()).linesToOffsetView == 1,
        "and the vertical axis completes its own line");
}

TEST(viewportOwnershipIncludesSubLineAndZeroDeltaScrollEvents)
{
    Bench bench;
    const ScrollRoute beginning = routeScroll(bench.ladder, bench.terminal, trackpadSwipe(0.0), notchedTuning());
    CHECK(beginning.isViewportScroll);
    CHECK(beginning.linesToOffsetView == 0);
    const ScrollRoute partial = routeScroll(bench.ladder, bench.terminal, trackpadSwipe(0.1), notchedTuning());
    CHECK(partial.isViewportScroll);
    CHECK(partial.linesToOffsetView == 0);
    feed(bench.terminal, "\033[?1000h");
    CHECK(!routeScroll(bench.ladder, bench.terminal, trackpadSwipe(0.0), notchedTuning()).isViewportScroll);
    CHECK(!routeScroll(bench.ladder, bench.terminal, trackpadSwipe(0.1), notchedTuning()).isViewportScroll);
}

TEST(theWheelBecomesCursorKeysOnTheAlternateScreenUnder1007)
{
    Bench bench;
    feed(bench.terminal, "\033[?1049h\033[?1007h");
    const ScrollRoute up = routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning());
    CHECK_MSG(bytes(up) == "\033[A\033[A\033[A", "a notch up is three cursor-up presses in one burst");
    feed(bench.terminal, "\033[?1h");
    const ScrollRoute down = routeScroll(bench.ladder, bench.terminal, wheelNotch(-1.0), notchedTuning());
    CHECK_MSG(bytes(down) == "\033OB\033OB\033OB", "and follows DECCKM");
    feed(bench.terminal, "\033[?1000h");
    CHECK_MSG(bytes(routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning())).rfind("\033[M", 0) == 0, "tracking outranks alternate scroll");
}

TEST(touchNavigatesAlternateScreenApplicationsWithoutMouseTracking)
{
    Bench bench;
    ScrollFact swipe = trackpadSwipe(16.0);
    swipe.isDirectTouch = true;
    const auto primary = routeScroll(bench.ladder, bench.terminal, swipe, notchedTuning());
    CHECK(primary.isViewportScroll && primary.linesToOffsetView == 1);
    CHECK(primary.byteLength == 0);

    feed(bench.terminal, "\033[?1049h");
    CHECK(!bench.terminal.modes.alternateScrollMode.asRequested());
    const auto alternate = routeScroll(bench.ladder, bench.terminal, swipe, notchedTuning());
    CHECK(bytes(alternate) == "\033[A");
    CHECK(!alternate.isViewportScroll && alternate.linesToOffsetView == 0);
    CHECK(!bench.terminal.modes.alternateScrollMode.asRequested());

    feed(bench.terminal, "\033[?1h");
    swipe.deltaY = -16.0;
    CHECK(bytes(routeScroll(bench.ladder, bench.terminal, swipe, notchedTuning())) == "\033OB");

    feed(bench.terminal, "\033[?1000h\033[?1006h");
    CHECK(bytes(routeScroll(bench.ladder, bench.terminal, swipe, notchedTuning())) == "\033[<65;6;4M");

    feed(bench.terminal, "\033[?1000l");
    swipe.isDirectTouch = false;
    CHECK(routeScroll(bench.ladder, bench.terminal, swipe, notchedTuning()).byteLength == 0);
}

TEST(aLockedWheelRidesTheLockedFrame)
{
    Bench bench;
    feed(bench.terminal, "\033[?7403h");
    const ScrollRoute noFrame = routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning());
    CHECK_MSG(noFrame.wasConsumed && noFrame.byteLength == 0 && noFrame.linesToOffsetView == 0, "no frame, nothing sent, and the view does not scroll under a lock");
    feed(bench.terminal, "\033[?1016h");
    const ScrollRoute notch = routeScroll(bench.ladder, bench.terminal, wheelNotch(1.0), notchedTuning());
    CHECK_MSG(bytes(notch) == "\033[<64;32768;32768M", "the wheel press carries a zero delta");
}

// --- Teaching the gestures nobody can see ------------------------------------
//
// A drag over an application that took the mouse is swallowed, and the reach-
// through modifiers that would have kept it local are documented in a config
// file rather than where the user is looking. The ladder raises the hint at the
// only moment it is relevant, names only modifiers the preferences leave
// enabled, and stops once the lesson has landed.

TEST(aDragAnApplicationSwallowsRaisesTheHintThatWouldHaveHelped)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    bench.route(fact(PointerFact::Kind::Press, 5, 5));
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Motion, 5, 6)).hint == Hint::None,
        "one cell is a click whose pointer wobbled, not a drag");
    const PointerRoute dragged = bench.route(fact(PointerFact::Kind::Motion, 5, 9));
    CHECK_MSG(dragged.hint == Hint::SelectWithShift, "Shift is the reach no configuration can take away, so it is the lesson");
    CHECK_MSG(std::string_view(hintText(dragged.hint, ModifierVocabulary::Mac)).find("Shift") != std::string_view::npos, "and the text names it");
    CHECK_MSG(std::string_view(hintText(dragged.hint, ModifierVocabulary::Words)) == hintText(dragged.hint, ModifierVocabulary::Mac),
        "every keyboard prints Shift, so every keyboard is told the same thing");
    CHECK_MSG(!isHintPersistent(dragged.hint), "a swallowed drag is a moment, not a state");

    // The lesson does not follow the reader's block chord: moved onto one, a
    // reader who has just lost a plain drag is still told about Shift, which is
    // the one that gets them a selection.
    Bench moved;
    moved.policy.blockSelectChord = chordFromSpec("ctrl+opt");
    feed(moved.terminal, "\033[?1002h\033[?1006h");
    moved.route(fact(PointerFact::Kind::Press, 5, 5));
    CHECK(moved.route(fact(PointerFact::Kind::Motion, 5, 9)).hint == Hint::SelectWithShift);
}

TEST(aReaderWithNoModifierKeysIsTaughtNothingAndChargedNothing)
{
    // Every lesson names a modifier. A touch device with no keyboard attached
    // has none of them, so naming one describes a gesture the reader cannot
    // perform, which reads as a broken feature rather than an absent one. The
    // frontend reports the device fact; which lesson follows stays Core's.
    const auto swallowOnce = [](bool hasKeys) {
        Bench bench;
        feed(bench.terminal, "\033[?1002h\033[?1006h");
        bench.route(fact(PointerFact::Kind::Press, 5, 5));
        PointerFact drag = fact(PointerFact::Kind::Motion, 5, 9);
        drag.hasModifierKeys = hasKeys;
        const PointerRoute dragged = bench.route(drag);
        return std::pair { dragged.hint, bench.ladder.selectionHintShowCount };
    };
    const auto [keyless, keylessSpend] = swallowOnce(false);
    CHECK_MSG(keyless == Hint::None, "with no modifier to name, there is no lesson to give");
    CHECK_MSG(keylessSpend == 0,
        "and no budget is spent: three silent drags must not use up the teaching a keyboard would earn");

    // The control: the same drag on the same bench with keys present. Without
    // it this test would pass against a ladder that had stopped teaching
    // anyone at all.
    const auto [keyed, keyedSpend] = swallowOnce(true);
    CHECK_MSG(keyed != Hint::None, "the same drag with keys attached still teaches");
    CHECK(keyedSpend == 1);
}

TEST(aTransientLessonStandsForItsIntervalAndAReplacementTakesItsOwnClock)
{
    // The lifetime is Core's so every arm fades a lesson at the same tick
    // and none of them keeps a timer of its own. The ladder is asked, not
    // told: a frontend passes its tick and draws what comes back.
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    bench.route(fact(PointerFact::Kind::Press, 5, 5));
    const PointerRoute dragged = bench.route(fact(PointerFact::Kind::Motion, 5, 9));
    CHECK(dragged.hint != Hint::None);
    CHECK_MSG(dragged.needsDisplay, "a lesson is chrome, and the frame that shows it must be drawn");
    CHECK_MSG(hintShowing(bench.ladder, 0) == dragged.hint, "the lesson stands at the tick it was raised");
    CHECK_MSG(hintShowing(bench.ladder, transientHintTicks - 1) == dragged.hint, "and through its interval");
    CHECK_MSG(hintShowing(bench.ladder, transientHintTicks) == Hint::None, "and not a tick past it");
    CHECK_MSG(hintExpiryTicks(bench.ladder) == transientHintTicks, "the one wake-up a frontend owes is the expiry");

    // A replacement takes its own clock: an inherited one would cut it short.
    HintClock clock;
    raiseHint(clock, Hint::PointerLockRelease, 0);
    raiseHint(clock, Hint::SelectWithShift, 100);
    CHECK_MSG(hintShowing(clock, 100 + transientHintTicks - 1) == Hint::SelectWithShift, "the replacement stands its full interval");
    CHECK(hintShowing(clock, 100 + transientHintTicks) == Hint::None);
    CHECK(hintExpiryTicks(clock) == 100 + transientHintTicks);
    raiseHint(clock, Hint::None, 200);
    CHECK_MSG(hintShowing(clock, 200) == Hint::None && hintExpiryTicks(clock) == 0, "None withdraws, and owes no wake-up");

    // The tick counter wraps; the interval must not care.
    raiseHint(clock, Hint::SelectWithShift, 0xFFFFFFF0u);
    CHECK_MSG(hintShowing(clock, 10) == Hint::SelectWithShift, "26 ticks across the wrap is still inside the interval");
    CHECK(hintShowing(clock, 0xFFFFFFF0u + transientHintTicks) == Hint::None);
}

TEST(theLockLessonStandsWithTheGrabWithinItsBudgetAndOutranksADrag)
{
    // The grab is the frontend's fact -- Core holds the mode bit, but an arm
    // that never captures the pointer, or declined this time, has no trap to
    // name the way out of. Everything after the report is the ladder's.
    Bench bench;
    notePointerLockGrab(bench.ladder, PointerLockGrab::Held);
    CHECK_MSG(hintShowing(bench.ladder, 0) == Hint::PointerLockRelease, "a held grab is answered with the way out");
    CHECK_MSG(hintExpiryTicks(bench.ladder) == 0, "and it has no clock: it ends with the grab");
    CHECK_MSG(hintShowing(bench.ladder, 100000) == Hint::PointerLockRelease, "however long the trap lasts");
    notePointerLockGrab(bench.ladder, PointerLockGrab::Held);
    CHECK_MSG(bench.ladder.pointerLockEngagements == 1, "a grab reported twice is one capture");
    notePointerLockGrab(bench.ladder, PointerLockGrab::Released);
    CHECK(hintShowing(bench.ladder, 0) == Hint::None);

    // The budget counts captures. An application that locks on every click
    // must not nag.
    for (int capture = 1; capture < hintMaxShows; ++capture) {
        notePointerLockGrab(bench.ladder, PointerLockGrab::Held);
        CHECK_MSG(hintShowing(bench.ladder, 0) == Hint::PointerLockRelease, "the first few captures teach");
        notePointerLockGrab(bench.ladder, PointerLockGrab::Released);
    }
    notePointerLockGrab(bench.ladder, PointerLockGrab::Held);
    CHECK_MSG(hintShowing(bench.ladder, 0) == Hint::None, "past the budget the trap is silent");
    notePointerLockGrab(bench.ladder, PointerLockGrab::Released);

    // The way out of a trap outranks a lesson about a drag, and the drag's
    // lesson returns once the trap ends, on its own clock.
    Bench captured;
    feed(captured.terminal, "\033[?1002h\033[?1006h");
    captured.route(fact(PointerFact::Kind::Press, 5, 5));
    const Hint lesson = captured.route(fact(PointerFact::Kind::Motion, 5, 9)).hint;
    CHECK(lesson != Hint::None);
    notePointerLockGrab(captured.ladder, PointerLockGrab::Held);
    CHECK(hintShowing(captured.ladder, 0) == Hint::PointerLockRelease);
    notePointerLockGrab(captured.ladder, PointerLockGrab::Released);
    CHECK_MSG(hintShowing(captured.ladder, 0) == lesson, "the drag's lesson was not lost under the trap");
    CHECK(hintShowing(captured.ladder, transientHintTicks) == Hint::None);
}

TEST(theBreakOutChordArmsUnderAGrabAndFiresAfterItsHoldOnTheLiveKeys)
{
    // The chord's clock is Core's: a frontend reports the modifiers on each
    // change, books the one wake-up the ladder names, and asks then whether
    // to release. Modifiers do not auto-repeat, so the hold is measured from
    // the tick the chord became the sole modifiers to the tick of that ask.
    PointerLockKeyState chord;
    chord.isControlHeld = true;
    chord.isCommandHeld = true;
    PointerLockKeyState nothing;

    // Without a grab there is nothing to release, and a ⌃⌘ held over an
    // unlocked surface is a real shortcut the chord must not shadow.
    Bench free;
    notePointerLockChord(free.ladder, chord, 0);
    CHECK_MSG(!free.ladder.pointerLockChord.isArmed, "no grab, no chord");
    CHECK(pointerLockChordTicksUntilDeadline(free.ladder, 0) == 0);

    Bench bench;
    notePointerLockGrab(bench.ladder, PointerLockGrab::Held);
    notePointerLockChord(bench.ladder, chord, 100);
    CHECK_MSG(bench.ladder.pointerLockChord.isArmed, "the sole modifiers under a grab arm the chord");
    CHECK_MSG(pointerLockChordTicksUntilDeadline(bench.ladder, 100) == pointerLockBreakoutHoldTicks, "the wake-up is the whole hold");
    CHECK(pointerLockChordTicksUntilDeadline(bench.ladder, 110) == pointerLockBreakoutHoldTicks - 10);
    // A second report of the same chord does not restart the clock.
    notePointerLockChord(bench.ladder, chord, 120);
    CHECK_MSG(bench.ladder.pointerLockChord.armedAtTicks == 100, "the hold is measured from the first tick, not the latest report");
    CHECK_MSG(tickPointerLockChord(bench.ladder, chord, 100 + pointerLockBreakoutHoldTicks - 1) == PointerLockChordVerdict::Keep,
        "a tick short of the hold keeps the grab");
    CHECK_MSG(bench.ladder.pointerLockChord.isArmed, "and leaves the chord armed for the real deadline");
    CHECK_MSG(tickPointerLockChord(bench.ladder, chord, 100 + pointerLockBreakoutHoldTicks) == PointerLockChordVerdict::BreakOut,
        "the hold elapsed with the chord still down: break out");
    CHECK_MSG(!bench.ladder.pointerLockChord.isArmed, "a break-out disarms; the caller clears the mode and releases");

    // A release in the last instant aborts: the live keys at the deadline
    // decide, not the keys at arming.
    notePointerLockChord(bench.ladder, chord, 200);
    CHECK(tickPointerLockChord(bench.ladder, nothing, 200 + pointerLockBreakoutHoldTicks) == PointerLockChordVerdict::Keep);
    CHECK_MSG(!bench.ladder.pointerLockChord.isArmed, "and disarms, so the next hold starts its own clock");

    // A key joining the chord makes it a shortcut, which disarms.
    notePointerLockChord(bench.ladder, chord, 300);
    PointerLockKeyState shortcut = chord;
    shortcut.isAnyNonModifierKeyHeld = true;
    notePointerLockChord(bench.ladder, shortcut, 305);
    CHECK_MSG(!bench.ladder.pointerLockChord.isArmed, "⌃⌘ plus a key is a shortcut, not the break-out");
    CHECK(pointerLockChordTicksUntilDeadline(bench.ladder, 305) == 0);

    // The grab ending disarms whatever was held: nothing left to release.
    notePointerLockChord(bench.ladder, chord, 400);
    notePointerLockGrab(bench.ladder, PointerLockGrab::Released);
    CHECK(tickPointerLockChord(bench.ladder, chord, 400 + pointerLockBreakoutHoldTicks) == PointerLockChordVerdict::Keep);
}

TEST(theHintStopsAfterThreeShowsAndOnceTheUserHasReachedThrough)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    const auto swallowOne = [&bench](int row) {
        bench.route(fact(PointerFact::Kind::Press, row, 5));
        const PointerRoute dragged = bench.route(fact(PointerFact::Kind::Motion, row, 9));
        bench.route(fact(PointerFact::Kind::Release, row, 9));
        return dragged.hint;
    };
    for (int show = 0; show < hintMaxShows; ++show)
        CHECK_MSG(swallowOne(show) != Hint::None, "the first few encounters teach");
    CHECK_MSG(swallowOne(4) == Hint::None, "a fourth does not: someone who has not learned it will not now");

    // A user who reaches through has learned it, and is never told again.
    Bench learner;
    feed(learner.terminal, "\033[?1002h\033[?1006h");
    PointerFact reachThrough = fact(PointerFact::Kind::Press, 2, 2);
    reachThrough.hasShift = true;
    learner.route(reachThrough);
    CHECK_MSG(learner.ladder.hasUsedSelectionOverride, "the reach-through is the lesson landing");
    learner.route(fact(PointerFact::Kind::Release, 2, 2));
    learner.route(fact(PointerFact::Kind::Press, 5, 5));
    CHECK_MSG(learner.route(fact(PointerFact::Kind::Motion, 5, 9)).hint == Hint::None, "so nothing is taught after it");
}

TEST(nothingIsTaughtWhenTheDragWasNeverSwallowed)
{
    Bench bench;
    bench.route(fact(PointerFact::Kind::Press, 5, 5));
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Motion, 5, 9)).hint == Hint::None,
        "no application holds the mouse, so the drag selected and taught nothing");

    Bench held;
    feed(held.terminal, "\033[?1002h\033[?1006h");
    PointerFact shifted = fact(PointerFact::Kind::Press, 5, 5);
    shifted.hasShift = true;
    held.route(shifted);
    PointerFact shiftedDrag = fact(PointerFact::Kind::Motion, 5, 9);
    shiftedDrag.hasShift = true;
    CHECK_MSG(held.route(shiftedDrag).hint == Hint::None,
        "a user already holding a modifier is not waiting to be told one exists");
}

TEST(thePointerLockHintIsPersistentBecauseItsTrapIsOngoing)
{
    CHECK_MSG(isHintPersistent(Hint::PointerLockRelease),
        "a hint that faded would leave someone still trapped with the instructions gone");
    CHECK_MSG(std::string_view(hintText(Hint::PointerLockRelease, ModifierVocabulary::Words)).find("Ctrl+Super") != std::string_view::npos,
        "a keyboard that prints words is told the keys it can find");
    CHECK_MSG(std::string_view(hintText(Hint::PointerLockRelease, ModifierVocabulary::Mac)).find("Control-Command") != std::string_view::npos,
        "and it names the chord that releases it, spelled as the guidelines spell a chord in text");
    CHECK(std::string_view(hintText(Hint::None, ModifierVocabulary::Mac)).empty());
    CHECK(std::string_view(hintText(Hint::None, ModifierVocabulary::Words)).empty());
    // Every lesson names a modifier, and every named modifier has a spelling
    // in every vocabulary: no lesson may ship a glyph or a middle dot to any
    // keyboard — the guidelines spell keys as words in text and join two
    // gestures with a clause.
    for (const Hint hint : { Hint::SelectWithShift, Hint::PointerLockRelease }) {
        for (const ModifierVocabulary vocabulary : { ModifierVocabulary::Mac, ModifierVocabulary::Words, ModifierVocabulary::Windows }) {
            const std::string_view words = hintText(hint, vocabulary);
            CHECK_MSG(words.find("⌘") == std::string_view::npos && words.find("⌥") == std::string_view::npos && words.find("⇧") == std::string_view::npos
                    && words.find("⌃") == std::string_view::npos && words.find("·") == std::string_view::npos,
                "a lesson is words, not glyphs or a dot");
            CHECK(!words.empty());
        }
        // The Mac names its logo key Command; the other keyboards do not have
        // one. Shift is on every keyboard, so that lesson reads alike.
        CHECK_MSG(hint == Hint::SelectWithShift
                || std::string_view(hintText(hint, ModifierVocabulary::Mac)) != hintText(hint, ModifierVocabulary::Words),
            "a Mac lesson names the Mac key, a words lesson the Super key");
    }
    // A Windows keyboard has no key called Super: its logo key is the Windows key.
    CHECK_MSG(std::string_view(hintText(Hint::PointerLockRelease, ModifierVocabulary::Windows)).find("Ctrl+Win") != std::string_view::npos,
        "Windows is told the key its keyboard shows");
    CHECK(std::string_view(hintText(Hint::PointerLockRelease, ModifierVocabulary::Windows)).find("Super") == std::string_view::npos);
    CHECK_MSG(!shouldShowHint(hintMaxShows), "the show budget is bounded");
    CHECK(shouldShowHint(0));
}

TEST(onlyADragTeaches_notAPressOrAReleaseOrAnotherButton)
{
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Press, 5, 5)).hint == Hint::None,
        "a press is a click the application may well have wanted");
    CHECK_MSG(bench.route(fact(PointerFact::Kind::Release, 5, 5)).hint == Hint::None,
        "and so is its release");

    Bench right;
    feed(right.terminal, "\033[?1002h\033[?1006h");
    PointerFact rightPress = fact(PointerFact::Kind::Press, 5, 5);
    rightPress.button = PointerButton::Right;
    right.route(rightPress);
    PointerFact rightDrag = fact(PointerFact::Kind::Motion, 5, 9);
    rightDrag.button = PointerButton::Right;
    CHECK_MSG(right.route(rightDrag).hint == Hint::None,
        "selection is the left button's; no other button's drag is a selection someone meant");
}

TEST(aVerticalDragTeachesAsReadilyAsAHorizontalOne)
{
    // The distance test takes whichever axis moved: a selection down a column
    // of output is as much a selection as one across a line.
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    bench.route(fact(PointerFact::Kind::Press, 1, 5));
    CHECK(bench.route(fact(PointerFact::Kind::Motion, 4, 5)).hint != Hint::None);
}

TEST(aLocalGestureClearsThePressTheDistanceIsMeasuredFrom)
{
    // A press kept local is not a press the application swallowed, so a later
    // drag must not measure its distance from it — that would raise the hint
    // for a gesture that was never swallowed at all.
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    bench.route(fact(PointerFact::Kind::Press, 5, 5)); // reported: origin recorded
    PointerFact shifted = fact(PointerFact::Kind::Press, 5, 5);
    shifted.hasShift = true;
    bench.route(shifted); // kept local: the origin is stale and must be dropped
    CHECK_MSG(bench.ladder.reportedPressRow < 0, "the local press cleared the swallowed-press origin");
}

TEST(theHintIsRaisedOnceForOneSwallowedDragNotOnEveryMotionOfIt)
{
    // A drag is a stream of motions. Teaching on each would spend the whole
    // three-show budget on a single gesture.
    Bench bench;
    feed(bench.terminal, "\033[?1002h\033[?1006h");
    bench.route(fact(PointerFact::Kind::Press, 5, 5));
    int raised = 0;
    for (int column = 8; column < 14; ++column) {
        if (bench.route(fact(PointerFact::Kind::Motion, 5, column)).hint != Hint::None)
            ++raised;
    }
    CHECK_MSG(raised == 1, "one drag is one lesson, however many motions it is made of");
    CHECK_MSG(bench.ladder.selectionHintShowCount == 1,
        "so a single gesture spends one of the three shows, not all of them");
}

// Which presses the caret model drives, and which are handed to another
// gesture. Both shells ask this rather than spelling the three exclusions
// themselves, which is how they came to disagree about it before.
TEST(testCaretDrivenGestureAdmitsThePlainSingleClick)
{
    PointerFact fact;
    fact.clickCount = 1;
    CHECK_MSG(!isCaretDrivenGesture(fact), "a frontend that cannot measure the pointer's point keeps whole cells");
    fact.xInCells = 3.5;
    CHECK_MSG(isCaretDrivenGesture(fact), "a plain single click with a measured point drags by carets");

    fact.clickCount = 2;
    CHECK_MSG(!isCaretDrivenGesture(fact), "a second click is a word, whose ends are cells");
    fact.clickCount = 3;
    CHECK_MSG(!isCaretDrivenGesture(fact), "and a third is a line");

    fact = {};
    fact.clickCount = 1;
    fact.xInCells = 3.5;
    fact.hasShift = true;
    CHECK_MSG(!isCaretDrivenGesture(fact), "Shift extends the standing selection from its anchor");

    fact = {};
    fact.clickCount = 1;
    fact.xInCells = 3.5;
    fact.hasAlternate = true;
    CHECK_MSG(!isCaretDrivenGesture(fact), "the block modifier makes a column span");

    // The two modifiers with no claim on the gesture leave it alone: Command
    // is the link and window-drag reach-through, Control is the context menu.
    fact = {};
    fact.clickCount = 1;
    fact.xInCells = 3.5;
    fact.hasCommand = true;
    fact.hasControl = true;
    CHECK_MSG(isCaretDrivenGesture(fact), "Command and Control do not take the drag");
}

// A caret's meaning on a terminal: the absolute row it names, and the open
// threshold that keeps a sweep along a blank line from selecting it.
TEST(testResolveSelectionCaretCollapsesABlankLine)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "ABCDEFGH");

    // A content line keeps its caret, whatever the boundary: the trailing void
    // of a row that printed something stays selectable.
    const Selection::CaretResolution onText = Selection::resolveSelectionCaret(t, 0, 0, 3);
    CHECK(onText.caret == 3);
    CHECK_MSG(onText.slot == 3, "a content line's caret is its own threshold");
    const Selection::CaretResolution pastText = Selection::resolveSelectionCaret(t, 0, 0, 15);
    CHECK_MSG(pastText.slot == 15, "including out in its trailing void");

    // A blank line has one selectable slot, so two carets far apart on it are
    // the same slot and the drag between them opens nothing.
    const Selection::CaretResolution left = Selection::resolveSelectionCaret(t, 0, 3, 1);
    const Selection::CaretResolution right = Selection::resolveSelectionCaret(t, 0, 3, 18);
    CHECK_MSG(left.slot == right.slot, "a blank line collapses to one slot");
    CHECK_MSG(left.caret != right.caret, "while the carets themselves still differ");

    // The row is absolute, so a drag that walks into history orders linearly.
    CHECK(onText.absoluteRow == Selection::resolveSelectionCaret(t, 0, 0, 9).absoluteRow);
    CHECK(left.absoluteRow == onText.absoluteRow + 3);
}

TEST(richTextReportsCharactersWithinOneCellAndKeepsShiftSelectionLocal)
{
    Bench bench;
    feed(bench.terminal, "\x1b[?1049h\x1b[?1003h\x1b[?1006h\x1b[1;1H");
    RichText::TypographyFactsBuilder builder;
    builder.setCell(8 * 64, 16 * 64);
    builder.setTerminalMetrics(12 * 64, 12 * 64, 12 * 64);
    CHECK(builder.addFace(RichText::FamilyClass::SansSerif, 0, 1900, 500, 2400, 400));
    builder.setAdvance('i', 400);
    CHECK(RichText::adoptTypography(bench.terminal, builder.facts()));
    feed(bench.terminal, "\x1b]7404;w=20:h=1:i=9:D=1:C=1:g=1:M=1:P=1;{\\rtf1 iiiii}\x1b\\");
    auto pointer = fact(PointerFact::Kind::Press, 0, 0);
    pointer.xInCells = 0.1;
    pointer.yInRows = 0.5;
    CHECK(report(bench.route(pointer)).find("7405;a=p:i=9:D=1:e=p:b=0:m=0:x=1:y=1:c=0:k=0") != std::string_view::npos);
    pointer.kind = PointerFact::Kind::Motion;
    pointer.isDragging = true;
    pointer.xInCells = 0.7;
    CHECK(report(bench.route(pointer)).find("c=2:k=2") != std::string_view::npos);
    pointer.kind = PointerFact::Kind::Release;
    CHECK(report(bench.route(pointer)).find("e=r") != std::string_view::npos);
    pointer.kind = PointerFact::Kind::Press;
    pointer.hasShift = true;
    CHECK_EQ(bench.route(pointer).reportLength, 0);
    CHECK(bench.ladder.isSelectionGestureOpen);
}

TEST(richTextPointerReportsRetainTheSourceRevisionAcrossReplacement)
{
    Bench bench;
    feed(bench.terminal, "\x1b[?1049h\x1b[H\x1b[?1000h\x1b[?1006h");
    feed(bench.terminal, "\x1b]7404;w=12:h=1:i=9:D=4294967294:C=1:M=1:P=1;{\\rtf1 before}\x1b\\");
    const auto pointer = fact(PointerFact::Kind::Press, 0, 0);
    const auto oldRoute = bench.route(pointer);
    const auto oldReport = std::string(report(oldRoute));
    feed(bench.terminal, "\x1b]7404;w=12:h=1:i=9:D=4294967295:C=1:M=1:P=1;{\\rtf1 after}\x1b\\");
    const auto newRoute = bench.route(pointer);
    CHECK(oldReport.find("i=9:D=4294967294:") != std::string::npos);
    CHECK(report(newRoute).find("i=9:D=4294967295:") != std::string_view::npos);
    CHECK(oldReport != report(newRoute));
}

UBIQUITTY_TEST_MAIN()

TEST(configuredSelectionModifiersBypassMouseReporting)
{
    for (const std::string_view token : { "shift", "cmd", "ctrl" }) {
        Bench bench;
        bench.policy.selectChord = chordFromSpec(token);
        feed(bench.terminal, "\033[?1049h\033[?1000h\033[?1006h");
        PointerFact press = fact(PointerFact::Kind::Press, 0, 0);
        press.hasShift = token == "shift";
        press.hasCommand = token == "cmd";
        press.hasControl = token == "ctrl";
        const auto route = bench.route(press);
        CHECK(route.didOpenSelectionGesture);
        CHECK(route.reportLength == 0);
        CHECK(!bench.session.gestureStyle.isRectangular);
        if (token != "shift") {
            Bench other;
            other.policy.selectChord = chordFromSpec(token);
            feed(other.terminal, "\033[?1000h\033[?1006h");
            press.hasCommand = false;
            press.hasControl = false;
            press.hasShift = true;
            CHECK(other.route(press).reportLength > 0);
        }
    }
}

TEST(readlineButtonOneMovesPointOnlyAfterACleanClick)
{
    Bench bench;
    feed(bench.terminal, "\033[?2001h\033[1;8H");
    auto press = fact(PointerFact::Kind::Press, 0, 3);
    press.xInCells = 3.75;
    CHECK(bench.route(press).readlineInput.empty());
    auto release = press;
    release.kind = PointerFact::Kind::Release;
    auto route = bench.route(release);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\033[D\033[D\033[D");
    CHECK_EQ(bench.terminal.screens.active->cursorX, 7);
    CHECK(bench.route(release).readlineInput.empty());

    (void)bench.route(press);
    (void)bench.route(fact(PointerFact::Kind::Motion, 0, 4));
    CHECK(bench.route(release).readlineInput.empty());
    press.hasShift = true;
    (void)bench.route(press);
    CHECK(bench.route(release).readlineInput.empty());
    press.hasShift = false;
    press.clickCount = 2;
    (void)bench.route(press);
    CHECK(bench.route(release).readlineInput.empty());
    press.clickCount = 1;
    (void)routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, 10);
    CHECK(routePointer(bench.ladder, bench.terminal, &bench.session, release, bench.policy, 41).readlineInput.empty());
}

TEST(readlineMouseMovementStaysOnTheCurrentWrappedLine)
{
    Bench bench;
    resize(bench.terminal, 3, 5);
    feed(bench.terminal, "\033[2J\033[Habcdefg\033[?2002h");
    auto press = fact(PointerFact::Kind::Press, 0, 4);
    press.button = PointerButton::Middle;
    auto route = bench.route(press);
    CHECK(!route.wasConsumed);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\033[D\033[D\033[D");
    press.row = 2;
    CHECK(bench.route(press).readlineInput.empty());
    press.row = 0;
    feed(bench.terminal, "\033[1;1H");
    press.row = 1;
    press.column = 1;
    route = bench.route(press);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\033[C\033[C\033[C\033[C\033[C\033[C");
}

TEST(readlineMouseMovementHonorsKeyboardEncodingAndHasNoShortReportLimit)
{
    Bench bench;
    resize(bench.terminal, 3, 200);
    feed(bench.terminal, "\033[?2002;1h\033[1;100H");
    auto press = fact(PointerFact::Kind::Press, 0, 0);
    press.button = PointerButton::Middle;
    auto route = bench.route(press);
    REQUIRE(route.readlineInput.size() == 297u);
    for (std::size_t offset = 0; offset < route.readlineInput.size(); offset += 3)
        CHECK(std::string_view(route.readlineInput.data() + offset, 3) == "\033OD");
    feed(bench.terminal, "\033 G");
    route = bench.route(press);
    REQUIRE(route.readlineInput.size() == 198u);
    for (std::size_t offset = 0; offset < route.readlineInput.size(); offset += 2)
        CHECK(std::string_view(route.readlineInput.data() + offset, 2) == "\x9b"
                                                                          "D");
}

TEST(readlineMouseMovementYieldsToReportingModifiersHistoryAndKeyboardLock)
{
    Bench bench;
    auto press = fact(PointerFact::Kind::Press, 0, 0);
    press.button = PointerButton::Middle;
    CHECK(bench.route(press).readlineInput.empty());
    feed(bench.terminal, "\033[?2002h");
    REQUIRE(!bench.route(press).readlineInput.empty());
    press.viewOffset = 1;
    CHECK(bench.route(press).readlineInput.empty());
    press.viewOffset = 0;
    press.hasShift = true;
    CHECK(bench.route(press).readlineInput.empty());
    press.hasShift = false;
    press.column = -1;
    CHECK(bench.route(press).readlineInput.empty());
    press.column = 0;
    feed(bench.terminal, "\033[2h");
    CHECK(bench.route(press).readlineInput.empty());
    feed(bench.terminal, "\033[2l\033[?1000;1006h");
    auto route = bench.route(press);
    CHECK(route.readlineInput.empty());
    CHECK(route.reportLength > 0);
    CHECK(route.wasConsumed);
}

namespace {

PointerRoute rightClick(Bench& bench, int row, int column, unsigned clickCount, std::uint32_t ticks)
{
    auto press = fact(PointerFact::Kind::Press, row, column);
    press.button = PointerButton::Right;
    press.clickCount = clickCount;
    (void)routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, ticks);
    press.kind = PointerFact::Kind::Release;
    return routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, ticks + 1);
}

void selectCells(Bench& bench, int startRow, int startColumn, int endRow, int endColumn)
{
    bench.session.state = { .active = true, .anchorRow = startRow, .anchorColumn = startColumn, .headRow = endRow, .headColumn = endColumn, .mode = SelectionMode::Character };
}

} // namespace

TEST(readlineRightDoubleClickMovesToTheEndAndDeletesTheWholeSelection)
{
    Bench bench;
    feed(bench.terminal, "\033[?2003h\033[1;10H");
    selectCells(bench, 0, 2, 0, 5);
    CHECK(rightClick(bench, 0, 5, 1, 10).readlineInput.empty());
    const auto route = rightClick(bench, 0, 5, 2, 15);
    CHECK(route.wasConsumed);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\033[D\033[D\033[D\177\177\177\177");
    CHECK_EQ(bench.terminal.screens.active->cursorX, 9);
    CHECK(rightClick(bench, 0, 5, 3, 20).readlineInput.empty());
}

TEST(readlineRightClickExtendsTheNearestSelectionEnd)
{
    Bench bench;
    feed(bench.terminal, "\033[?2003h\033[1;7H");
    selectCells(bench, 0, 2, 0, 5);
    (void)rightClick(bench, 0, 1, 1, 10);
    const auto bounds = Selection::selectedBounds(bench.session, bench.terminal);
    REQUIRE(bounds);
    CHECK_EQ(bounds->column0, 1);
    CHECK_EQ(bounds->column1, 5);
    const auto route = rightClick(bench, 0, 1, 2, 15);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\177\177\177\177\177");
}

TEST(readlineDeletionHonorsThePTYEraseCharacterAndDisabledErase)
{
    for (const std::optional<std::uint8_t> erase : { std::optional<std::uint8_t> { 8 }, std::optional<std::uint8_t> {}, std::optional<std::uint8_t> { 0 } }) {
        Bench bench;
        bench.policy.readlineEraseCharacter = erase;
        feed(bench.terminal, "\033[?2003h\033[1;6H");
        selectCells(bench, 0, 2, 0, 4);
        (void)rightClick(bench, 0, 4, 1, 10);
        const auto route = rightClick(bench, 0, 4, 2, 15);
        CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == (erase ? std::string(3, static_cast<char>(*erase)) : std::string {}));
    }
}

TEST(readlineDeletionRequiresOneCurrentWrappedLine)
{
    Bench bench;
    resize(bench.terminal, 4, 5);
    feed(bench.terminal, "\033[2J\033[HabcdefghijK\033[?2003h");
    selectCells(bench, 0, 3, 1, 2);
    (void)rightClick(bench, 1, 2, 1, 10);
    const auto route = rightClick(bench, 1, 2, 2, 15);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\033[D\033[D\033[D\177\177\177\177\177");
    feed(bench.terminal, "\033[4;1H");
    selectCells(bench, 0, 3, 1, 2);
    (void)rightClick(bench, 1, 2, 1, 40);
    CHECK(rightClick(bench, 1, 2, 2, 45).readlineInput.empty());
}

TEST(readlineDeletionRejectsLongClicksAndChangedCells)
{
    Bench bench;
    feed(bench.terminal, "\033[?2003h\033[1;6H");
    selectCells(bench, 0, 2, 0, 4);
    (void)rightClick(bench, 0, 4, 1, 10);
    CHECK(rightClick(bench, 0, 4, 2, 50).readlineInput.empty());
    (void)rightClick(bench, 0, 4, 1, 100);
    CHECK(rightClick(bench, 0, 3, 2, 105).readlineInput.empty());
    (void)rightClick(bench, 0, 3, 1, 150);
    auto press = fact(PointerFact::Kind::Press, 0, 3);
    press.button = PointerButton::Right;
    press.clickCount = 2;
    (void)routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, 155);
    press.kind = PointerFact::Kind::Release;
    CHECK(routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, 190).readlineInput.empty());
}

TEST(readlineDeletionYieldsToModifiersHistoryTrackingAndKeyboardLock)
{
    for (int condition = 0; condition < 8; ++condition) {
        Bench bench;
        feed(bench.terminal, "\033[?2003h\033[1;6H");
        selectCells(bench, 0, 2, 0, 4);
        (void)rightClick(bench, 0, 4, 1, 10);
        auto press = fact(PointerFact::Kind::Press, 0, 4);
        press.button = PointerButton::Right;
        press.clickCount = 2;
        press.hasShift = condition == 0;
        press.hasControl = condition == 1;
        press.hasAlternate = condition == 2;
        press.hasCommand = condition == 3;
        press.viewOffset = condition == 4 ? 1 : 0;
        if (condition == 5)
            feed(bench.terminal, "\033[?1000;1006h");
        if (condition == 6)
            feed(bench.terminal, "\033[2h");
        if (condition == 7)
            feed(bench.terminal, "\033[?2003l");
        const auto down = routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, 15);
        CHECK(down.readlineInput.empty());
        if (condition != 5)
            CHECK(!down.wasConsumed);
        press.kind = PointerFact::Kind::Release;
        CHECK(routePointer(bench.ladder, bench.terminal, &bench.session, press, bench.policy, 16).readlineInput.empty());
    }
}

TEST(readlineDoubleClickSurvivesPointerHoverBetweenClicks)
{
    Bench bench;
    feed(bench.terminal, "\033[?2003h\033[1;6H");
    selectCells(bench, 0, 2, 0, 4);
    (void)rightClick(bench, 0, 4, 1, 10);
    auto hover = fact(PointerFact::Kind::Motion, 0, 4);
    hover.button = PointerButton::None;
    hover.isDragging = false;
    (void)routePointer(bench.ladder, bench.terminal, &bench.session, hover, bench.policy, 12);
    const auto route = rightClick(bench, 0, 4, 2, 15);
    CHECK(std::string(route.readlineInput.begin(), route.readlineInput.end()) == "\177\177\177");
}
