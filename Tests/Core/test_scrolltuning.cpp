// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_scrolltuning.cpp — the portable wheel-speed policy: the raw
// delta → line-count mapping (base lines/notch × speed multiplier),
// the precise-delta (trackpad) throttle, the sub-line accumulator
// residual, sign handling, and the rate floor that keeps a zero/garbage
// config from freezing the wheel.

#include "Core/Screen/ScrollTuning.h"

#include "Base/FractionalAccumulator.h"

#include <cstdio>

using Ubiquitty::FractionalAccumulator;
using Ubiquitty::Screen::ScrollTuning::InputDeviceType;
using Ubiquitty::Screen::ScrollTuning::linesForDelta;
using Ubiquitty::Screen::ScrollTuning::linesPerUnitDelta;
using Ubiquitty::Screen::ScrollTuning::Policy;

/// Default policy (1 line/notch, 1.0x) is a fixed step: one unit of delta ==
/// one line, no residual left over.
TEST(testDefaultIsOneLinePerUnit)
{
    Policy p; // base 1.0, speed 1.0
    CHECK(linesPerUnitDelta(p) == 1.0);
    FractionalAccumulator accumulator;
    CHECK(linesForDelta(3.0, p, accumulator, InputDeviceType::Wheel) == 3);
    CHECK(accumulator.residual() == 0.0);
    CHECK(linesForDelta(-2.0, p, accumulator, InputDeviceType::Wheel) == -2);
    CHECK(accumulator.residual() == 0.0);
}

/// The speed multiplier scales the line count.
TEST(testSpeedMultiplier)
{
    Policy p;
    p.speed = 2.0;
    CHECK(linesPerUnitDelta(p) == 2.0);
    FractionalAccumulator accumulator;
    CHECK(linesForDelta(3.0, p, accumulator, InputDeviceType::Wheel) == 6);
    CHECK(accumulator.residual() == 0.0);
}

/// Base lines-per-notch scales independently of speed; they compound.
TEST(testBaseLinesAndSpeedCompound)
{
    Policy p;
    p.baseLinesPerTick = 3.0;
    p.speed = 2.0;
    CHECK(linesPerUnitDelta(p) == 6.0);
    FractionalAccumulator accumulator;
    CHECK(linesForDelta(1.0, p, accumulator, InputDeviceType::Wheel) == 6);
}

/// Sub-line deltas accumulate: several small pans add up to one line, and
/// the fractional residual carries forward.
TEST(testSublineAccumulates)
{
    Policy p; // 1.0x
    FractionalAccumulator accumulator;
    CHECK(linesForDelta(0.4, p, accumulator, InputDeviceType::Wheel) == 0); // 0.4 banked
    CHECK(linesForDelta(0.4, p, accumulator, InputDeviceType::Wheel) == 0); // 0.8 banked
    CHECK(linesForDelta(0.4, p, accumulator, InputDeviceType::Wheel) == 1); // 1.2 → 1 line, 0.2 left
    // residual ~0.2 within float slop
    CHECK(accumulator.residual() > 0.15 && accumulator.residual() < 0.25);
}

/// A direction reversal truncates toward zero, so stale magnitude in the
/// accumulator doesn't bleed across into the new direction.
TEST(testReversalTruncatesTowardZero)
{
    Policy p;
    FractionalAccumulator accumulator;
    (void)linesForDelta(0.6, p, accumulator, InputDeviceType::Wheel); // residual ~0.6
    int lines = linesForDelta(-1.0, p, accumulator, InputDeviceType::Wheel); // residual ~-0.4 → 0
    CHECK(lines == 0);
    CHECK(accumulator.residual() < 0.0);
}

/// A zero/negative config can't freeze the wheel: the rate clamps to a
/// small positive floor.
TEST(testRateFloor)
{
    Policy zero;
    zero.speed = 0.0;
    CHECK(linesPerUnitDelta(zero) > 0.0);
    Policy neg;
    neg.baseLinesPerTick = -5.0;
    CHECK(linesPerUnitDelta(neg) > 0.0);
}

/// The trackpad is an independent path: precise deltas are normalized to rows by
/// pointsPerLine and scaled by trackpadSpeed only — the wheel knobs don't apply —
/// while non-precise wheel ticks ignore trackpadSpeed/pointsPerLine entirely.
TEST(testTrackpadIndependentOfWheel)
{
    Policy p;
    p.baseLinesPerTick = 3.0; // wheel knobs — must not affect the trackpad
    p.speed = 2.0;
    p.trackpadSpeed = 0.5;
    p.pointsPerLine = 10.0; // 10 device points per text row
    FractionalAccumulator accumulator;
    // Wheel (non-precise) uses the wheel knobs, ignores trackpad/pointsPerLine:
    // 1 unit × (3 × 2) = 6 lines.
    CHECK(linesForDelta(1.0, p, accumulator, InputDeviceType::Wheel) == 6);
    CHECK(accumulator.residual() == 0.0);
    CHECK(linesForDelta(40.0, p, accumulator, InputDeviceType::Trackpad) == 2);
    CHECK(accumulator.residual() == 0.0);
}

/// Precise point deltas normalize by the row height: bigger rows → fewer lines per
/// point of finger travel, and a sub-row pan banks toward a step.
TEST(testTrackpadPointsToRows)
{
    Policy p;
    p.pointsPerLine = 16.0;
    FractionalAccumulator accumulator;
    CHECK(linesForDelta(16.0, p, accumulator, InputDeviceType::Trackpad) == 1);
    CHECK(accumulator.residual() == 0.0);
    CHECK(linesForDelta(8.0, p, accumulator, InputDeviceType::Trackpad) == 0);
    CHECK(linesForDelta(8.0, p, accumulator, InputDeviceType::Trackpad) == 1);
    CHECK(accumulator.residual() == 0.0);
    // A zero/garbage pointsPerLine can't divide-by-zero: treated as 1.0.
    Policy z;
    z.pointsPerLine = 0.0;
    FractionalAccumulator a2;
    CHECK(linesForDelta(2.0, z, a2, InputDeviceType::Trackpad) == 2);
}

UBIQUITTY_TEST_MAIN()
