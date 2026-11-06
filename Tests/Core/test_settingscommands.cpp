// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_settingscommands.cpp — pins Core/CommandPalette/SettingsCommands: the
// numeric formatting (integer vs fractional steps), the slider-step nudge, and
// the value preview / range hint. Links SettingsCommands.cpp.

#include "Core/CommandPalette/SettingsCommands.h"

#include <cstdio>
#include <string>

namespace Values = Ubiquitty::CommandPalette::Settings;

namespace {
void checkString(const std::string& got, const std::string& want, const char* what)
{
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "%s — got '%s', want '%s'", what, got.c_str(), want.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

void checkNear(double got, double want, const char* what)
{
    double diff = got - want;
    if (diff < 0)
        diff = -diff;
    if (diff > 1e-9) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "%s — got %f, want %f", what, got, want);
        CHECK_MSG(false, ubqMessage);
    }
}

} // namespace

TEST(settingscommands)
{

    // formatNumber — an integer step drops the fraction; a fractional step keeps
    // its decimal places.
    checkString(Values::formatNumber(12.0, 1.0), "12", "integer step");
    checkString(Values::formatNumber(12.4, 1.0), "12", "integer step rounds");
    checkString(Values::formatNumber(1.25, 0.25), "1.25", "quarter step → 2 decimals");
    checkString(Values::formatNumber(0.5, 0.1), "0.5", "tenth step → 1 decimal");

    // The step sets how fine the control moves, not how wide the readout is:
    // a value with no fraction reads without one, however fine the step.
    checkString(Values::formatNumber(12.0, 0.25), "12", "whole point at a quarter step");
    checkString(Values::formatNumber(12.5, 0.25), "12.5", "half point drops the spare zero");
    checkString(Values::formatNumber(12.25, 0.25), "12.25", "quarter point keeps both places");
    checkString(Values::formatNumber(0.0, 0.01), "0", "hundredth step, no fraction");
    checkString(Values::formatNumber(-1.5, 0.25), "-1.5", "negative keeps its sign");
    checkString(Values::formatNumber(-0.001, 0.01), "0", "a value under half a place is unsigned zero");
    checkString(Values::valuePreview(12.5, 0.25, "pt"), "12.5 pt", "fractional preview with unit");
    checkString(Values::rangeHint(6.0, 96.0, 0.25, "pt"),
        "6–"
        "96 pt",
        "range hint at a fractional step spells whole bounds whole");

    // sliderStep — finer for small ranges, coarser for wide ones.
    checkNear(Values::sliderStep(0.0, 1.0), 0.1, "span 1 → 0.1");
    checkNear(Values::sliderStep(0.0, 50.0), 1.0, "span 50 → 1");
    checkNear(Values::sliderStep(0.0, 100.0), 5.0, "span 100 → 5");

    // Preview + range hint.
    checkString(Values::valuePreview(12.0, 1.0, "pt"), "12 pt", "value preview with unit");
    checkString(Values::valuePreview(0.5, 0.1, ""), "0.5", "value preview no unit");
    checkString(Values::rangeHint(1.0, 3.0, 1.0, "pt"),
        "1–"
        "3 pt",
        "range hint en dash");
}

UBIQUITTY_TEST_MAIN()
