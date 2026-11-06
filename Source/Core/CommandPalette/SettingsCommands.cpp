// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// SettingsCommands.cpp — ports the macOS SettingsCommandProvider's numeric
// helpers (beamFormatNumber / beamFractionDigits / sliderStep + the unitless-
// slider percentage rule), so a non-macOS frontend formats the same drills.
//

#include "Core/CommandPalette/SettingsCommands.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace Ubiquitty::CommandPalette::Settings {

int fractionDigits(double step)
{
    if (step >= 1.0)
        return 0;
    double scaled = step;
    for (int digits = 1; digits <= 4; ++digits) {
        scaled *= 10.0;
        if (std::fabs(std::round(scaled) - scaled) < 0.000001)
            return digits;
    }
    return 4;
}

std::string formatNumber(double value, double step)
{
    const int digits = fractionDigits(step);
    char buffer[32];
    if (digits == 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(std::llround(value)));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    std::string text(buffer);
    // The step says how fine the control moves, not how wide the readout is: a
    // quarter-point step on a size of 12 reads "12", and only a size that has a
    // fraction spends digits on it. Trailing zeros are dropped, then the point
    // they were the only reason for.
    const std::size_t point = text.find('.');
    if (point != std::string::npos) {
        std::size_t last = text.size();
        while (last > point + 1 && text[last - 1] == '0')
            --last;
        if (last == point + 1)
            last = point;
        text.resize(last);
    }
    // A value that rounds to zero from below spells the zero without a sign.
    if (text == "-0")
        text = "0";
    return text;
}

double sliderStep(double low, double high)
{
    const double span = std::fabs(high - low);
    if (span <= 5.0)
        return 0.1;
    if (span <= 50.0)
        return 1.0;
    return 5.0;
}

std::string valuePreview(double value, double step, std::string_view unit)
{
    const std::string number = formatNumber(value, step);
    if (unit.empty())
        return number;
    return number + " " + std::string(unit);
}

std::string rangeHint(double low, double high, double step, std::string_view unit)
{
    const std::string lowText = formatNumber(low, step);
    const std::string highText = formatNumber(high, step);
    // U+2013 EN DASH between the bounds, matching the macOS editor hint.
    std::string hint = lowText + "–" + highText;
    if (!unit.empty())
        hint += " " + std::string(unit);
    return hint;
}

} // namespace Ubiquitty::CommandPalette::Settings
