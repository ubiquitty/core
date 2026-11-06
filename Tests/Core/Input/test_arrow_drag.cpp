// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_arrow_drag — pins the arrow key's drag (Source/Core/Input/ArrowDrag.h):
// how far a finger travels per arrow, which axis it sends, and what it sends
// when it comes back or holds still.
//
// The metrics are round numbers rather than a frontend's: the contract is the
// arithmetic, and each frontend chooses its own distances.

#include "Core/Input/ArrowDrag.h"

#include <cmath>
#include <limits>

using Ubiquitty::Input::ArrowDrag;
using Ubiquitty::Input::ArrowDragAxis;
using Ubiquitty::Input::ArrowDragMetrics;
using Ubiquitty::Input::ArrowDragSteps;
using Ubiquitty::Input::maxArrowDragStepsPerMove;
using Ubiquitty::Keys::VirtualKey;

static constexpr ArrowDragMetrics metrics { .stepDistance = 10, .deadZoneRadius = 4 };

static bool sends(ArrowDragSteps steps, VirtualKey arrow, int count)
{
    return steps.arrow == arrow && steps.count == count;
}

static bool sendsNothing(ArrowDragSteps steps)
{
    return steps.arrow == VirtualKey::None && !steps.count;
}

// Inside the dead zone the touch is a tap: no axis, no arrows, and the
// frontend answers its release with the key under the finger.
TEST(aTouchInsideTheDeadZoneIsATap)
{
    ArrowDrag drag(metrics);
    CHECK(sendsNothing(drag.moveTo(0, 0)));
    CHECK(sendsNothing(drag.moveTo(2, -3)));
    CHECK(sendsNothing(drag.moveTo(-4, 0)));
    CHECK(drag.axis() == ArrowDragAxis::None);
    CHECK(!drag.hasLeftDeadZone());
}

// Leaving the dead zone is not yet an arrow: the first one waits for a whole
// step of travel from the touch-down point.
TEST(leavingTheDeadZoneLocksAnAxisAndSendsNothing)
{
    ArrowDrag drag(metrics);
    CHECK(sendsNothing(drag.moveTo(5, 1)));
    CHECK(drag.axis() == ArrowDragAxis::Horizontal);
    CHECK(drag.hasLeftDeadZone());
    CHECK(sendsNothing(drag.moveTo(9.9, 1)));
}

// Each step boundary crossed sends one arrow, and a movement that crosses
// several sends that many at once.
TEST(distanceSetsTheNumberOfArrows)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(10, 0), VirtualKey::RightArrow, 1));
    CHECK(sendsNothing(drag.moveTo(19, 0)));
    CHECK(sends(drag.moveTo(20, 0), VirtualKey::RightArrow, 1));
    CHECK(sends(drag.moveTo(55, 0), VirtualKey::RightArrow, 3));
    CHECK_EQ(drag.netSteps(), 5);

    ArrowDrag left(metrics);
    CHECK(sends(left.moveTo(-31, 0), VirtualKey::LeftArrow, 3));
    CHECK_EQ(left.netSteps(), -3);
}

TEST(verticalDragsSendUpAndDown)
{
    ArrowDrag down(metrics);
    CHECK(sends(down.moveTo(1, 20), VirtualKey::DownArrow, 2));
    CHECK(down.axis() == ArrowDragAxis::Vertical);

    ArrowDrag up(metrics);
    CHECK(sends(up.moveTo(-2, -10), VirtualKey::UpArrow, 1));
}

// A finger resting anywhere, a boundary included, sends nothing more.
TEST(holdingStillSendsNothing)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(20, 0), VirtualKey::RightArrow, 2));
    for (int event = 0; event < 100; ++event)
        CHECK(sendsNothing(drag.moveTo(20, 0)));
    CHECK_EQ(drag.netSteps(), 2);
}

// Coming back sends the opposite arrow once per step, and only a whole step
// past the last arrow: a finger trembling on a boundary does not alternate.
TEST(draggingBackReversesOneStepAtATime)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(30, 0), VirtualKey::RightArrow, 3));
    CHECK(sendsNothing(drag.moveTo(29, 0)));
    CHECK(sendsNothing(drag.moveTo(30.5, 0)));
    CHECK(sendsNothing(drag.moveTo(20.5, 0)));
    CHECK(sends(drag.moveTo(20, 0), VirtualKey::LeftArrow, 1));
    CHECK(sends(drag.moveTo(10, 0), VirtualKey::LeftArrow, 1));
    CHECK(sends(drag.moveTo(20, 0), VirtualKey::RightArrow, 1));
    CHECK(sends(drag.moveTo(-10, 0), VirtualKey::LeftArrow, 3));
    CHECK_EQ(drag.netSteps(), -1);
}

// Travel across the locked axis is ignored, however far it goes: a thumb
// drifting while it moves the cursor does not recall history.
TEST(theAxisHoldsForTheDrag)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(10, 2), VirtualKey::RightArrow, 1));
    CHECK(sendsNothing(drag.moveTo(14, 40)));
    CHECK(sendsNothing(drag.moveTo(12, -60)));
    CHECK(drag.axis() == ArrowDragAxis::Horizontal);
    CHECK(sends(drag.moveTo(20, -60), VirtualKey::RightArrow, 1));
}

// The dominant axis at the moment the finger leaves the dead zone wins.
TEST(theDominantAxisIsLocked)
{
    ArrowDrag mostlyDown(metrics);
    CHECK(sendsNothing(mostlyDown.moveTo(3, 4)));
    CHECK(mostlyDown.axis() == ArrowDragAxis::Vertical);
    CHECK(sendsNothing(mostlyDown.moveTo(30, 4)));
    CHECK(sends(mostlyDown.moveTo(30, 10), VirtualKey::DownArrow, 1));

    ArrowDrag mostlyLeft(metrics);
    CHECK(sendsNothing(mostlyLeft.moveTo(-4, 3)));
    CHECK(mostlyLeft.axis() == ArrowDragAxis::Horizontal);
}

// Back in the dead zone with its arrows cancelled out, a drag drops its axis,
// so the next one is picked without lifting the finger. With arrows still
// outstanding it keeps the axis, so the way home stays open.
TEST(returningToTheOriginFreesTheAxis)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(10, 0), VirtualKey::RightArrow, 1));
    CHECK(sendsNothing(drag.moveTo(3, 0)));
    CHECK(drag.axis() == ArrowDragAxis::Horizontal);
    CHECK(sends(drag.moveTo(0, 0), VirtualKey::LeftArrow, 1));
    CHECK(sendsNothing(drag.moveTo(0, 1)));
    CHECK(drag.axis() == ArrowDragAxis::None);
    CHECK(sends(drag.moveTo(1, -10), VirtualKey::UpArrow, 1));
    CHECK(drag.axis() == ArrowDragAxis::Vertical);
    CHECK(drag.hasLeftDeadZone());
}

// A jump no finger makes is reported a bounded amount at a time, and a NaN
// moves nothing.
TEST(aWildMovementIsBounded)
{
    ArrowDrag drag(metrics);
    CHECK(sends(drag.moveTo(1e9, 0), VirtualKey::RightArrow, maxArrowDragStepsPerMove));
    CHECK(sends(drag.moveTo(1e9, 0), VirtualKey::RightArrow, maxArrowDragStepsPerMove));

    ArrowDrag infinite(metrics);
    CHECK(sends(infinite.moveTo(-std::numeric_limits<double>::infinity(), 0), VirtualKey::LeftArrow,
        maxArrowDragStepsPerMove));

    ArrowDrag undefined(metrics);
    CHECK(sendsNothing(undefined.moveTo(std::nan(""), 30)));
    CHECK(!undefined.hasLeftDeadZone());
    CHECK(sends(undefined.moveTo(0, 30), VirtualKey::DownArrow, 3));
}

UBIQUITTY_TEST_MAIN()
