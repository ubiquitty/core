// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Input/DragAutoScroll.h"
#include "Harness.h"

// test_dragautoscroll.cpp — pins Input::dragAutoScrollStep, the arithmetic every
// frontend supporting drag-select past an edge needs. Bare libc++; no platform
// code.

using Ubiquitty::Input::dragAutoScrollStep;

// A pointer over the grid asks for nothing, which is also how a caller knows to
// stop its timer.
TEST(testOverTheGridIsNoStep)
{
    CHECK(!dragAutoScrollStep(0, 24).isOutsideGrid);
    CHECK(dragAutoScrollStep(0, 24).lines == 0);
    CHECK(!dragAutoScrollStep(23, 24).isOutsideGrid);
    CHECK(dragAutoScrollStep(23, 24).lines == 0);
}

// The last row is inside and the one past it is outside. Off-by-one here is a
// drag that either never scrolls or scrolls while resting on the bottom row.
TEST(testTheEdgeRowsAreInside)
{
    CHECK(!dragAutoScrollStep(23, 24).isOutsideGrid);
    CHECK(dragAutoScrollStep(24, 24).isOutsideGrid);
    CHECK(dragAutoScrollStep(-1, 24).isOutsideGrid);
}

// Above the grid walks up into history; below walks back down. The signs are
// what a caller adds to a view offset, so they are the contract.
TEST(testDirectionFollowsTheEdgeLeft)
{
    CHECK(dragAutoScrollStep(-1, 24).lines == 1);
    CHECK(dragAutoScrollStep(24, 24).lines == -1);
}

// The step grows with the distance, so flicking well past the edge scrolls
// faster than easing over it — the acceleration every text view has.
TEST(testStepGrowsWithDistance)
{
    CHECK(dragAutoScrollStep(-1, 24).lines == 1);
    CHECK(dragAutoScrollStep(-10, 24).lines == 10);
    CHECK(dragAutoScrollStep(33, 24).lines == -10);
}

// A grid with no rows cannot say what is outside it, and answers so rather than
// producing a step from a row count of zero.
TEST(testNoRowsIsNoStep)
{
    CHECK(!dragAutoScrollStep(-5, 0).isOutsideGrid);
    CHECK(dragAutoScrollStep(-5, 0).lines == 0);
}

UBIQUITTY_TEST_MAIN()
