// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Render a theme as a TOML [[themes]] block, and locate an existing named block
// for in-place overwrite. Pure string + ColorRGBA logic. Color formatting matches
// the reader (Preferences.cpp tomlHexColor): "#rrggbb" when opaque, "#rrggbbaa"
// otherwise, lowercase. Block detection is line-based: a block starts at a line
// beginning "[[themes]]" and runs to just before the next section header (any
// line starting with '[') or EOF.

#include "Core/Color.h"
#include "Core/Preferences/TOMLString.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Themes/ThemeAppearance.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace Ubiquitty::Themes {

// Format one ColorRGBA as "#rrggbb" (alpha == 255) or "#rrggbbaa".
[[nodiscard]] inline std::string formatTOMLHexColor(ColorRGBA color)
{
    char buffer[12];
    if (color.alpha == 255)
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", color.red, color.green, color.blue);
    else
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", color.red, color.green, color.blue, color.alpha);
    return std::string(buffer);
}

// Render one theme as a TOML [[themes]] block (leading newline so it appends
// cleanly). The 16 ANSI colors print as a 4x4 grid, then fg / bg / cursor.
// A status colour the theme names prints as `status_<slot>`; one it leaves to
// derivation prints nothing, so a theme written back does not freeze today's
// derived colours into the file.
[[nodiscard]] inline std::string renderThemeBlock(const std::string& name, bool isDark, const ColorRGBA ansi[16], ColorRGBA foreground, ColorRGBA background, ColorRGBA cursor, const ThemeAppearance& appearance = {}, const StatusColorOverrides& statusOverrides = {})
{
    std::ostringstream out;
    out << "\n[[themes]]\n";
    // The name is not the writer's: import takes it from a downloaded file's
    // last path component, and the theme editor's field reaches the same call.
    out << "name = \"" << Ubiquitty::Preferences::escapeTOMLBasicString(name) << "\"\n";
    out << "is_dark = " << (isDark ? "true" : "false") << "\n";
    if (appearance.backgroundOpacity) {
        out << "background_opacity = " << *appearance.backgroundOpacity << "\n";
    }
    if (appearance.defaultBackgroundOpacity) {
        out << "default_background_opacity = " << *appearance.defaultBackgroundOpacity << "\n";
    }
    if (appearance.blurRadius) {
        out << "blur_radius = " << *appearance.blurRadius << "\n";
    }
    if (appearance.textVibrancy) {
        out << "text_vibrancy = " << (*appearance.textVibrancy ? "true" : "false") << "\n";
    }
    if (appearance.textContrast) {
        out << "text_contrast = " << (*appearance.textContrast ? "true" : "false") << "\n";
    }
    if (appearance.backgroundVibrancy) {
        out << "background_vibrancy = " << (*appearance.backgroundVibrancy ? "true" : "false") << "\n";
    }
    if (appearance.usesSystemColors) {
        out << "system_colors = true\n";
    }
    switch (appearance.terminalMarks) {
    case TerminalMarkStyle::Palette:
        break;
    case TerminalMarkStyle::Primary:
        out << "terminal_marks = \"primary\"\n";
        break;
    case TerminalMarkStyle::Secondary:
        out << "terminal_marks = \"secondary\"\n";
        break;
    case TerminalMarkStyle::Tertiary:
        out << "terminal_marks = \"tertiary\"\n";
        break;
    }
    out << "ansi = [\n";
    for (int row = 0; row < 4; ++row) {
        out << "    ";
        for (int column = 0; column < 4; ++column) {
            int index = row * 4 + column;
            out << "\"" << formatTOMLHexColor(ansi[index]) << "\"";
            if (index < 15)
                out << ", ";
        }
        out << "\n";
    }
    out << "]\n";
    out << "fg     = \"" << formatTOMLHexColor(foreground) << "\"\n";
    out << "bg     = \"" << formatTOMLHexColor(background) << "\"\n";
    out << "cursor = \"" << formatTOMLHexColor(cursor) << "\"\n";
    for (std::size_t slot = 0; slot < statusColorSlotCount; ++slot) {
        if (statusOverrides[slot])
            out << "status_" << statusColorSlotName(static_cast<StatusColorSlot>(slot)) << " = \"" << formatTOMLHexColor(*statusOverrides[slot]) << "\"\n";
    }
    return out.str();
}

} // namespace Ubiquitty::Themes
