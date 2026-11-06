// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_animation_schedule.cpp — the animation schedule wakes a blinking caret
// once per level change and never in between, on a pinned clock.
//
// The caret's solid phase is 32 ticks, longer than the schedule's half-second
// ceiling. A schedule that capped the blink's own crossing at the ceiling would
// wake the pane before the fade-out begins, find the caret unchanged, and spend
// a capture and a build on a frame that presents nothing, once per cycle.

#include "Core/Cursor/Blink.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Rendering/AnimationSchedule.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cmath>
#include <cstdint>

namespace {

using Ubiquitty::Rendering::AnimationScheduleInputs;
using Ubiquitty::Rendering::nextAnimationDelaySeconds;

constexpr double tickSeconds = 1.0 / 60.0;

struct Wakes {
    int presents = 0;
    int empty = 0;
};

// Follows the schedule's wakes for `cycles` blink periods, as the per-frame
// one-shot does: each wake draws, and the delay after it is asked again.
Wakes followBlink(const Ubiquitty::Terminal& term, int cycles)
{
    Wakes wakes;
    std::uint32_t now = 0;
    float presented = Ubiquitty::Cursor::resolve(&term, now).blinkAlpha;
    const std::uint32_t end = static_cast<std::uint32_t>(cycles) * Ubiquitty::Cursor::Blink::caretPeriodTicks;
    while (now < end) {
        AnimationScheduleInputs inputs;
        inputs.cursorDelayTicks = Ubiquitty::Cursor::ticksUntilAlphaChange(&term, now);
        if (!inputs.cursorDelayTicks)
            break;
        now += static_cast<std::uint32_t>(std::lround(nextAnimationDelaySeconds(inputs) / tickSeconds));
        const float alpha = Ubiquitty::Cursor::resolve(&term, now).blinkAlpha;
        if (alpha != presented) {
            ++wakes.presents;
            presented = alpha;
        } else {
            ++wakes.empty;
        }
    }
    return wakes;
}

void makeBlinkingCaret(Ubiquitty::Terminal& term)
{
    Ubiquitty::hardReset(term);
    term.windowFocused = true;
    term.lastActivityTick = 0;
    term.cursorHoldFrames = 1;
    term.cursorStyle = Ubiquitty::Cursor::Style::BlinkingBlock;
    term.cursorBlinkPref = true;
}

} // namespace

TEST(aBlinkingCaretWakesOncePerLevelChangeAndNeverBetween)
{
    const Ubiquitty::Cursor::Blink::Animation original = Ubiquitty::Cursor::Blink::animation();
    Ubiquitty::Terminal term;
    makeBlinkingCaret(term);
    // Fade steps through eight level changes a cycle; on/off through two.
    for (const auto& [animation, edgesPerCycle] : { std::pair { Ubiquitty::Cursor::Blink::Animation::Fade, 8 },
             std::pair { Ubiquitty::Cursor::Blink::Animation::OnOff, 2 } }) {
        Ubiquitty::Cursor::Blink::setAnimation(animation);
        const Wakes wakes = followBlink(term, 4);
        CHECK_MSG(wakes.empty == 0, ("the schedule woke " + std::to_string(wakes.empty) + " times to an unchanged caret").c_str());
        // Four cycles from the end of the hold, which opens on one more edge.
        CHECK_MSG(wakes.presents == 4 * edgesPerCycle + 1,
            ("presents over four cycles: " + std::to_string(wakes.presents)).c_str());
    }
    Ubiquitty::Cursor::Blink::setAnimation(original);
}

TEST(halfASecondBoundsOnlyTheTermsWithNoExactCrossing)
{
    AnimationScheduleInputs inputs;
    CHECK_MSG(nextAnimationDelaySeconds(inputs) == 0.5, "nothing scheduled sleeps half a second");

    // A blink's crossing is exact, however far away.
    inputs.cursorDelayTicks = 32;
    CHECK(nextAnimationDelaySeconds(inputs) == 32 * tickSeconds);
    inputs.cursorDelayTicks = 0;
    inputs.blinkDelayTicks = 45;
    CHECK(nextAnimationDelaySeconds(inputs) == 45 * tickSeconds);
    inputs.blinkDelayTicks = 0;
    inputs.resolvedBlinkDelaySeconds = 0.9;
    CHECK_MSG(nextAnimationDelaySeconds(inputs) == 0.9, "a blink resolved by the session schedules past the ceiling too");

    // The hilite wait polls at the ceiling on purpose, beside a blink too.
    inputs.hiliteAwaiting = true;
    CHECK(nextAnimationDelaySeconds(inputs) == 0.5);
    inputs.hiliteAwaiting = false;
    // An image frame's delay is capped at the ceiling.
    inputs.imageDelayMilliseconds = 2'000;
    CHECK(nextAnimationDelaySeconds(inputs) == 0.5);
    inputs.imageDelayMilliseconds = 0;
    // A smooth animation runs at the tick whatever else is pending.
    inputs.smoothAnimationActive = true;
    CHECK(nextAnimationDelaySeconds(inputs) == tickSeconds);
}

UBIQUITTY_TEST_MAIN()
