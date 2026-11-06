// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/ScrollTuning.h"

namespace Ubiquitty::Screen::ScrollTuning {

namespace {
// Rate floor so a zero/negative config can't wedge the wheel.
constexpr double minLinesPerUnitDelta = 0.05;
} // namespace

double linesPerUnitDelta(const Policy& p)
{
    double base = p.baseLinesPerTick;
    if (base < 0.0)
        base = 0.0;
    double rate = base * p.speed;
    if (rate < minLinesPerUnitDelta)
        rate = minLinesPerUnitDelta;
    return rate;
}

int linesForDelta(double rawDelta, const Policy& p, FractionalAccumulator& accumulator, InputDeviceType device)
{
    double lines;
    if (device == InputDeviceType::Trackpad) {
        // Trackpad/Magic Mouse: rawDelta in device points, normalized to rows by row height, then the
        // trackpad multiplier only. No rate floor: small speeds bank across the precise stream in the accumulator.
        double ppl = p.pointsPerLine > 0.0 ? p.pointsPerLine : 1.0;
        double t = p.trackpadSpeed < 0.0 ? 0.0 : p.trackpadSpeed;
        lines = (rawDelta / ppl) * t;
    } else {
        // Wheel: rawDelta is already ~line units; scale by the wheel knobs.
        lines = rawDelta * linesPerUnitDelta(p);
    }
    // Bank the fraction, release whole lines (truncate toward zero so a reversal doesn't carry magnitude).
    return accumulator.take(lines);
}

} // namespace Ubiquitty::Screen::ScrollTuning
