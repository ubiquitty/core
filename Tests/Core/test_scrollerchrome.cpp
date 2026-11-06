// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/ScrollerChrome.h"
#include "Core/Screen/Scrollback.h"
#include "Harness.h"

// test_scrollerchrome.cpp — pins Runtime::viewOffsetLimit and
// Runtime::scrollerChrome: how far the view may scroll back, and the three
// policies a shell drawing a scroller gets wrong by having its own opinion —
// how far the bar may scroll, which way its knob is tinted, and whether the
// application wants one at all. Both shells read them, so they are pinned
// here rather than once per arm. Bare libc++; no platform code.

using Ubiquitty::Runtime::RuntimeSnapshot;
using Ubiquitty::Runtime::scrollerChrome;
using Ubiquitty::Runtime::viewOffsetLimit;

// The limit is the snapshot's gated count, not its storage count. This is the
// wheel notch that walked a full-screen program's viewport into the primary's
// history: the ring still holds 400 rows and none of them is behind the
// alternate screen.
TEST(testAlternateScreenReachesNoOffset)
{
    RuntimeSnapshot snapshot;
    snapshot.scrollbackCount = 400;
    snapshot.history.retainedRows = 0;
    snapshot.modes.alternateScreenActive = true;
    CHECK(viewOffsetLimit(snapshot) == 0);
    CHECK(Ubiquitty::Screen::clampViewOffset(40, viewOffsetLimit(snapshot)) == 0);
}

TEST(testPrimaryScreenReachesItsHistory)
{
    RuntimeSnapshot snapshot;
    snapshot.scrollbackCount = 400;
    snapshot.history.retainedRows = 400;
    CHECK(viewOffsetLimit(snapshot) == 400);
    CHECK(Ubiquitty::Screen::clampViewOffset(4000, viewOffsetLimit(snapshot)) == 400);
}

// The primary screen shows the history it has.
TEST(testPrimaryScreenShowsItsHistory)
{
    RuntimeSnapshot snapshot;
    snapshot.modes.alternateScreenActive = false;
    snapshot.history.retainedRows = 400;
    CHECK(scrollerChrome(snapshot).scrollbackLines == 400);
}

// The alternate screen has none, whatever the snapshot's raw count says. This
// is the bar vim and less would otherwise grow over a screen that cannot
// scroll: the count belongs to the primary screen behind them.
TEST(testAlternateScreenShowsNoHistory)
{
    RuntimeSnapshot snapshot;
    snapshot.modes.alternateScreenActive = true;
    snapshot.scrollbackCount = 400;
    snapshot.history.retainedRows = 0;
    CHECK(scrollerChrome(snapshot).scrollbackLines == 0);
}

// The bar's range and the range the view may reach are one number. A shell
// reading them apart draws a knob over rows a wheel cannot reach.
TEST(testTheBarsExtentIsTheViewsLimit)
{
    RuntimeSnapshot primary;
    primary.scrollbackCount = 400;
    primary.history.retainedRows = 400;
    CHECK(scrollerChrome(primary).scrollbackLines == viewOffsetLimit(primary));

    RuntimeSnapshot alternate;
    alternate.scrollbackCount = 400;
    alternate.history.retainedRows = 0;
    alternate.modes.alternateScreenActive = true;
    CHECK(scrollerChrome(alternate).scrollbackLines == viewOffsetLimit(alternate));
}

// A dark ground needs a light knob, and the reverse. The threshold is the
// midpoint of the weighted sum, so a mid grey lands on the light side and the
// two answers are never both wrong.
TEST(testKnobIsTintedAgainstTheGround)
{
    RuntimeSnapshot dark;
    dark.defaultBackgroundRed = 0.05f;
    dark.defaultBackgroundGreen = 0.05f;
    dark.defaultBackgroundBlue = 0.07f;
    CHECK(scrollerChrome(dark).prefersLightKnob);

    RuntimeSnapshot light;
    light.defaultBackgroundRed = 0.98f;
    light.defaultBackgroundGreen = 0.97f;
    light.defaultBackgroundBlue = 0.95f;
    CHECK(!scrollerChrome(light).prefersLightKnob);
}

// Green carries most of the weight, so a saturated green ground is read as
// light and a saturated blue one as dark — which is the whole reason the tint
// is not decided by the largest channel.
TEST(testTintFollowsLumaNotTheLargestChannel)
{
    RuntimeSnapshot green;
    green.defaultBackgroundGreen = 1.0f;
    CHECK(!scrollerChrome(green).prefersLightKnob);

    RuntimeSnapshot blue;
    blue.defaultBackgroundBlue = 1.0f;
    CHECK(scrollerChrome(blue).prefersLightKnob);
}

// ?30 reset takes the bar away, and that is not an extent: the history behind
// it is untouched, so the bar returns with everything it could scroll through.
// A shell that read the mode for itself is a shell with a second opinion.
TEST(testTheApplicationCanTakeTheBarAway)
{
    RuntimeSnapshot snapshot;
    snapshot.history.retainedRows = 400;
    CHECK(scrollerChrome(snapshot).isBarVisible);

    snapshot.modes.isScrollbarVisible = false;
    CHECK(!scrollerChrome(snapshot).isBarVisible);
    CHECK(scrollerChrome(snapshot).scrollbackLines == 400);
}

UBIQUITTY_TEST_MAIN()
