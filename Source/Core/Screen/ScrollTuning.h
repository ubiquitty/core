// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "Base/FractionalAccumulator.h"

// Wheel-scroll speed policy: the device-delta → line-count mapping, in portable Core (same on every
// frontend). The frontend supplies the delta and a FractionalAccumulator (so a slow trackpad pan still
// advances a line over several ticks); this owns the tuning and the fractional→integer step.

namespace Ubiquitty::Screen::ScrollTuning {

/// Which input device produced a scroll event.
///
/// Wheel and trackpad carry independent speeds, so the device selects which tuning path an event takes.
enum class InputDeviceType : uint8_t {
    Wheel,
    Trackpad
};

// User-tunable scroll policy (Values [mouse]). Wheel and trackpad are two independent speeds; each event
// uses one path:
//   Wheel (non-precise): lines = rawDelta * baseLinesPerTick * speed.
//   Trackpad/Magic Mouse (precise): lines = (rawDelta / pointsPerLine) * trackpadSpeed. Precise deltas
//     arrive in device points, normalized to rows by pointsPerLine, making trackpadSpeed font-size
//     independent. The two knob sets do not cross-affect.
struct Policy {
    double baseLinesPerTick = 1.0; // mouse wheel: lines per unit delta
    double speed = 1.0; // mouse wheel: speed multiplier
    double trackpadSpeed = 1.0; // trackpad: speed multiplier
    double pointsPerLine = 1.0; // device points per text row (precise normalization)
};

// Effective wheel lines per unit delta: baseLinesPerTick * speed, clamped to a non-negative floor so a
// garbage config can't freeze the wheel. Not the trackpad path.
double linesPerUnitDelta(const Policy& p);

// Fold one scroll event into `accumulator`, return whole lines to scroll, bank the sub-line residual.
// `device` selects the path (Trackpad normalizes by pointsPerLine × trackpadSpeed; Wheel uses the wheel
// knobs). Pure apart from the accumulator, so fully host-testable.
int linesForDelta(double rawDelta, const Policy& p, FractionalAccumulator& accumulator, InputDeviceType device);

} // namespace Ubiquitty::Screen::ScrollTuning
