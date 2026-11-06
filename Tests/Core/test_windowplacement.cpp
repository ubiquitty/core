// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/WindowPlacement.h"
#include "Harness.h"

// test_windowplacement.cpp — pins Session::WindowPlacement, the arithmetic that
// decides where a terminal window opens. Both macOS shells read it. Bare
// libc++; no platform code.

using Ubiquitty::FloatRect;
using namespace Ubiquitty::Session::WindowPlacement;

namespace {
// A 1440x900 display with a menu bar, in the bottom-left origin both AppKit and
// X11 place windows in.
constexpr FloatRect display { { 0.0f, 0.0f }, { 1440.0f, 875.0f } };
}

// A second window steps down and to the right, so the one beneath it reads as a
// window rather than a shadow.
TEST(testCascadeStepsDownAndRight)
{
    const FloatRect stepped = cascaded(FloatRect { { 100.0f, 500.0f }, { 400.0f, 300.0f } }, display);
    CHECK(stepped.origin.x == 122.0f);
    CHECK(stepped.origin.y == 478.0f);
    CHECK(stepped.size.width == 400.0f);
    CHECK(stepped.size.height == 300.0f);
}

// A step that would carry the frame off the right edge wraps back to the left
// rather than putting the window where it cannot be reached.
TEST(testCascadeWrapsAtTheRightEdge)
{
    const FloatRect stepped = cascaded(FloatRect { { 1030.0f, 500.0f }, { 400.0f, 300.0f } }, display);
    CHECK(stepped.origin.x == 22.0f);
}

// And off the bottom wraps to the top, inset by the same step.
TEST(testCascadeWrapsAtTheBottomEdge)
{
    const FloatRect stepped = cascaded(FloatRect { { 100.0f, 10.0f }, { 400.0f, 300.0f } }, display);
    CHECK(stepped.origin.y == 875.0f - 300.0f - 22.0f);
}

// Re-homing keeps the gap from the TOP of the visible area, not the bottom. A
// window a little below the menu bar stays a little below it on a display of
// another height, where a bottom-anchored move would drop it by the difference.
TEST(testReHomingPreservesTheGapFromTheTop)
{
    // A second display to the right, shorter than the first.
    constexpr FloatRect other { { 1440.0f, 0.0f }, { 1280.0f, 700.0f } };
    // 75 points below the first display's top edge.
    const FloatRect frame { { 100.0f, 500.0f }, { 400.0f, 300.0f } };
    const FloatRect moved = movedToScreen(frame, display, other);
    CHECK(moved.origin.x == 1540.0f); // the same 100 from the left
    CHECK(moved.origin.y == 700.0f - 75.0f - 300.0f); // the same 75 from the top
}

// A frame wider than its destination is pulled inside it rather than left
// hanging off the edge.
TEST(testReHomingClampsIntoTheDestination)
{
    constexpr FloatRect small { { 1440.0f, 0.0f }, { 500.0f, 400.0f } };
    const FloatRect moved = movedToScreen(FloatRect { { 900.0f, 500.0f }, { 400.0f, 300.0f } }, display, small);
    CHECK(moved.origin.x + moved.size.width <= small.origin.x + small.size.width);
    CHECK(moved.origin.x >= small.origin.x);
    CHECK(moved.origin.y >= small.origin.y);
    CHECK(moved.origin.y + moved.size.height <= small.origin.y + small.size.height);
}

// Only a normal windowed frame is worth remembering. The exclusions each produce
// a window that opens wrong and reads as the terminal having forgotten.
TEST(testOnlyANormalFrameIsPersisted)
{
    constexpr FloatRect normal { { 100.0f, 100.0f }, { 800.0f, 500.0f } };
    CHECK(isWorthPersisting(normal, false, false, false));
    CHECK(!isWorthPersisting(normal, true, false, false)); // full screen
    CHECK(!isWorthPersisting(normal, false, true, false)); // minimised
    CHECK(!isWorthPersisting(FloatRect { { 100.0f, 100.0f }, { 0.0f, 500.0f } }, false, false, false));
    CHECK(!isWorthPersisting(FloatRect { { 100.0f, 100.0f }, { 800.0f, 0.0f } }, false, false, false));
}

// A window running one program belongs to that program, not to a person
// arranging a desktop. Remembering where it sat makes a run of them hysteretic:
// each inherits the frame the last finished at, which walks the window across
// the screen and reads as flakiness in whatever is driving them.
TEST(testAWindowRunningOneProgramIsNotRemembered)
{
    constexpr FloatRect normal { { 100.0f, 100.0f }, { 800.0f, 500.0f } };
    CHECK(!isWorthPersisting(normal, false, false, true));
    CHECK_MSG(isWorthPersisting(normal, false, false, false),
        "and a window beside it that outlives its child still is");
}

UBIQUITTY_TEST_MAIN()
