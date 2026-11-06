// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// SettingsCommands.h — the portable half of the palette's settings scope:
// formats a numeric setting's previews / range hints. A frontend resolves the
// schema bounds and binds the live getter/setter itself; this owns only the
// number formatting both frontends share.
//

#include <string>
#include <string_view>

namespace Ubiquitty::CommandPalette::Settings {

// The decimal places `step` implies (0 for an integer step, up to 4).
int fractionDigits(double step);

// Format `value` at the precision `step` implies (an integer for an integer
// step, else that many decimals).
std::string formatNumber(double value, double step);

// A continuous slider carries no schema step; nudge it across ~20 stops, finer
// for small ranges.
double sliderStep(double low, double high);

// The value preview a drill command shows ("12 pt", or bare when unit empty).
std::string valuePreview(double value, double step, std::string_view unit);

// The dim range hint under a number editor ("1–3 pt"); bare when unit empty.
std::string rangeHint(double low, double high, double step, std::string_view unit);

} // namespace Ubiquitty::CommandPalette::Settings
