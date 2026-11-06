// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_animation_demand — whether a pane still owes a frame.
//
// Five independent reasons a clock stays armed, so the cases check each one
// alone against an otherwise quiet demand: a test that set several at once
// would pass for an implementation that had dropped all but one of them.
//
// The cursor half is where this rots quietly. A frontend deciding for itself
// which styles blink writes a list of 1, 3 and 5, which is equivalent only
// while DECSCUSR ends at 6 — and the failure it produces is a cursor blinking
// on screen with no repaint ever scheduled to show it, which no golden and no
// crash reports. Every enumerator is walked here rather than a chosen few.

#include "Harness.h"

#include "Core/Cursor/Style.h"
#include "Core/Rendering/AnimationSchedule.h"

namespace {

namespace Rn = Ubiquitty::Rendering;
namespace Cur = Ubiquitty::Cursor;

// A pane with nothing moving. Each case turns on exactly one reason.
Rn::AnimationDemand quiet()
{
    Rn::AnimationDemand demand;
    demand.isCursorFocused = true;
    return demand;
}

} // namespace

TEST(testAQuietPaneOwesNothing)
{
    CHECK(!quiet().isAnythingAnimating());
}

TEST(testViewAnimationStopsOwingFramesWhenItSettles)
{
    Rn::AnimationDemand cursor = quiet();
    cursor.isCursorSliding = true;
    CHECK(cursor.isAnythingAnimating());
    cursor.isCursorSliding = false;
    CHECK(!cursor.isAnythingAnimating());

    Rn::AnimationDemand match = quiet();
    match.isFocusedMatchBouncing = true;
    CHECK(match.isAnythingAnimating());
    match.isFocusedMatchBouncing = false;
    CHECK(!match.isAnythingAnimating());
}

TEST(testEachReasonArmsTheClockOnItsOwn)
{
    Rn::AnimationDemand bell = quiet();
    bell.bellFlashAlpha = 0.01f;
    CHECK(bell.isAnythingAnimating());

    Rn::AnimationDemand images = quiet();
    images.isImageAnimating = true;
    CHECK(images.isAnythingAnimating());

    Rn::AnimationDemand hilite = quiet();
    hilite.isHiliteAwaiting = true;
    CHECK(hilite.isAnythingAnimating());

    Rn::AnimationDemand blink = quiet();
    blink.hasBlinkHint = true;
    blink.hasBlinkCellOnScreen = true;
    CHECK(blink.isAnythingAnimating());

    Rn::AnimationDemand cursor = quiet();
    cursor.isCursorVisible = true;
    cursor.isCursorBlinkEnabled = true;
    cursor.cursorStyle = Cur::Style::BlinkingBlock;
    CHECK(cursor.isAnythingAnimating());
}

// A flash that has decayed to nothing is not a flash. Checked because the
// alpha is a float and "the bell fired at some point" is the wrong question.
TEST(testASpentBellFlashOwesNothing)
{
    Rn::AnimationDemand demand = quiet();
    demand.bellFlashAlpha = 0.0f;
    CHECK(!demand.isAnythingAnimating());
}

// The sticky hint alone must not arm the clock at the tail: it is never
// cleared, so it would hold the pane awake for the life of the grid once
// anything had ever blinked. Scrolled back it must, because the rows on
// display are outside the live reading that would otherwise retract it.
TEST(testTheStickyBlinkHintArmsOnlyWhenScrolledBack)
{
    Rn::AnimationDemand atTail = quiet();
    atTail.hasBlinkHint = true;
    atTail.hasBlinkCellOnScreen = false;
    atTail.presentedViewOffset = 0;
    CHECK(!atTail.isAnythingAnimating());

    Rn::AnimationDemand scrolledBack = atTail;
    scrolledBack.presentedViewOffset = 5;
    CHECK(scrolledBack.isAnythingAnimating());
}

// Every enumerator, not a chosen few. A consumer that listed the blinking
// styles would agree with this on today's enum and diverge the moment one is
// added, which is the whole reason the style is asked rather than enumerated.
TEST(testEveryCursorStyleAnswersItsOwnBlink)
{
    const Cur::Style styles[] = {
        Cur::Style::FollowUserPreference,
        Cur::Style::BlinkingBlock,
        Cur::Style::SteadyBlock,
        Cur::Style::BlinkingUnderline,
        Cur::Style::SteadyUnderline,
        Cur::Style::BlinkingBar,
        Cur::Style::SteadyBar,
    };

    for (const Cur::Style style : styles) {
        Rn::AnimationDemand demand = quiet();
        demand.isCursorVisible = true;
        demand.isCursorBlinkEnabled = true;
        demand.cursorStyle = style;

        // FollowUserPreference carries no blink of its own, so it rides the
        // preference; every other style answers for itself.
        const bool shouldBlink = style == Cur::Style::FollowUserPreference || Cur::isBlinking(style);
        CHECK(demand.isCursorBlinkingOnScreen() == shouldBlink);
        CHECK(demand.isAnythingAnimating() == shouldBlink);
    }
}

// Three separate ways a blinking cursor stops owing a frame. Each is checked
// alone so that dropping any one of them fails rather than being masked.
TEST(testABlinkingCursorStillNeedsToBeOnScreenFocusedAndEnabled)
{
    Rn::AnimationDemand blinking = quiet();
    blinking.isCursorVisible = true;
    blinking.isCursorBlinkEnabled = true;
    blinking.cursorStyle = Cur::Style::BlinkingBar;
    CHECK(blinking.isAnythingAnimating());

    Rn::AnimationDemand hidden = blinking;
    hidden.isCursorVisible = false;
    CHECK(!hidden.isAnythingAnimating());

    Rn::AnimationDemand unfocused = blinking;
    unfocused.isCursorFocused = false;
    CHECK(!unfocused.isAnythingAnimating());

    Rn::AnimationDemand disabled = blinking;
    disabled.isCursorBlinkEnabled = false;
    CHECK(!disabled.isAnythingAnimating());
}

// The blink rule does not require the hint to be armed. The snapshot's producer
// gates the live reading on the hint, so the state cannot arrive today — but a
// rule that reads `hint && cell` is correct only while that gating holds, and
// this pins the rule that does not depend on it.
TEST(testALiveBlinkReadingStandsWithoutTheHint)
{
    Rn::AnimationDemand demand = quiet();
    demand.hasBlinkCellOnScreen = true;
    demand.hasBlinkHint = false;
    CHECK(demand.isAnythingAnimating());
}

// A hilite wait owes a deadline, not a frame. The schedule reads the two
// separately: treating a timeout as content runs the smooth per-tick cadence
// for a pane whose only business is waiting, which costs sixty frames a second
// and shows nothing. Every other reason is content and must answer both.
TEST(testAHiliteWaitArmsTheClockWithoutBeingContent)
{
    Rn::AnimationDemand hilite = quiet();
    hilite.isHiliteAwaiting = true;
    CHECK(hilite.isAnythingAnimating());
    CHECK(!hilite.isContentAnimating());

    Rn::AnimationDemand images = quiet();
    images.isImageAnimating = true;
    CHECK(images.isAnythingAnimating());
    CHECK(images.isContentAnimating());

    Rn::AnimationDemand bell = quiet();
    bell.bellFlashAlpha = 0.5f;
    CHECK(bell.isContentAnimating());

    Rn::AnimationDemand cursor = quiet();
    cursor.isCursorVisible = true;
    cursor.isCursorBlinkEnabled = true;
    cursor.cursorStyle = Cur::Style::BlinkingBar;
    CHECK(cursor.isContentAnimating());

    Rn::AnimationDemand blink = quiet();
    blink.hasBlinkCellOnScreen = true;
    blink.hasBlinkHint = true;
    CHECK(blink.isContentAnimating());
}

// A pane whose frames are withheld (Runtime::paneFrameDelivery) shows nothing
// it animates, so the clock it would keep armed spends frames on a drawable
// nobody composites. A hilite deadline is the exception, as it is above: it
// owes a timeout rather than a frame, and a covered window's application is
// still waiting for the reply.
TEST(testAWithheldPaneAnimatesNothingButItsHiliteDeadline)
{
    Rn::AnimationDemand blink = quiet();
    blink.hasBlinkCellOnScreen = true;
    blink.hasBlinkHint = true;
    Rn::AnimationDemand image = quiet();
    image.isImageAnimating = true;
    Rn::AnimationDemand bell = quiet();
    bell.bellFlashAlpha = 0.5f;
    Rn::AnimationDemand slide = quiet();
    slide.isCursorSliding = true;
    Rn::AnimationDemand caret = quiet();
    caret.isCursorVisible = true;
    caret.isCursorBlinkEnabled = true;
    caret.cursorStyle = Cur::Style::BlinkingBlock;

    for (Rn::AnimationDemand moving : { blink, image, bell, slide, caret }) {
        CHECK_MSG(moving.isAnythingAnimating(), "the reason arms the clock while the pane draws");
        moving.isFrameDeliveryWithheld = true;
        CHECK_MSG(!moving.isAnythingAnimating(), "and arms nothing once the pane's frames are withheld");
        CHECK_MSG(moving.isContentAnimating(), "while the content itself is still described as moving");
    }

    Rn::AnimationDemand hilite = quiet();
    hilite.isHiliteAwaiting = true;
    hilite.isFrameDeliveryWithheld = true;
    CHECK_MSG(hilite.isAnythingAnimating(), "a hilite deadline fires for a covered window too");
}

UBIQUITTY_TEST_MAIN()
