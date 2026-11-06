// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"

#include <optional>

namespace Ubiquitty::Themes {

/// Selects palette or semantic ink for ordinary terminal command marks.
enum class UBIQUITTY_CLOSED_ENUM TerminalMarkStyle {
    Palette,
    Primary,
    Secondary,
    Tertiary,
};

/// Optional theme overrides; absent values inherit the application's settings.
struct ThemeAppearance {
    std::optional<double> backgroundOpacity;
    std::optional<double> defaultBackgroundOpacity;
    std::optional<double> blurRadius;
    std::optional<bool> textVibrancy;
    std::optional<bool> textContrast;
    std::optional<bool> backgroundVibrancy;
    bool usesSystemColors = false;
    TerminalMarkStyle terminalMarks = TerminalMarkStyle::Palette;

    /// Configures a native material without an additional terminal tint.
    void useGlass()
    {
        backgroundOpacity = 0;
        defaultBackgroundOpacity = 1;
        blurRadius = 20;
        textVibrancy = true;
        backgroundVibrancy = true;
        usesSystemColors = true;
        terminalMarks = TerminalMarkStyle::Tertiary;
    }
};

} // namespace Ubiquitty::Themes
