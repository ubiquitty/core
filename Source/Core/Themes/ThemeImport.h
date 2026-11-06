// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Color.h"

#include <optional>
#include <string>
#include <string_view>

// Parses an exported Ubiquitty TOML palette or a third-party color theme. Formats,
// auto-detected from the bytes:
//   - Ubiquitty TOML: one complete [[themes]] palette with name and darkness.
//   - iTerm2 ".itermcolors": XML plist mapping slot keys ("Ansi 0 Color" …,
//     "Foreground/Background/Cursor Color") to float Red/Green/Blue Components
//     in 0.0..1.0.
//   - Ghostty: `key = value` lines. `palette = N=#rrggbb` fills ANSI slot N;
//     `foreground`, `background`, `cursor-color` set named colors. `#rrggbb`,
//     `rrggbb`, `#rgb` accepted.

namespace Ubiquitty::Themes {

enum class ImportFormat {
    Unknown,
    ITermColors,
    Ghostty,
    Ubiquitty,
};

struct ImportedTheme {
    std::string name;
    std::optional<bool> isDark;
    bool ok = false; // true when at least the 16 ANSI slots parsed
    ImportFormat format = ImportFormat::Unknown;
    ColorRGBA ansi[16] = {};
    ColorRGBA foreground = { 0xC0, 0xC0, 0xC0, 0xFF };
    ColorRGBA background = { 0x10, 0x10, 0x14, 0xFF };
    ColorRGBA cursor = { 0xC0, 0xC0, 0xC0, 0xFF };
    bool foregroundSet = false;
    bool backgroundSet = false;
    bool cursorSet = false;
    bool ansiSet[16] = {};
};

// Parse a theme file. `format` forces a parser; Unknown (default) auto-detects.
// ok == true when all 16 ANSI slots were found.
[[nodiscard]] ImportedTheme importThemeFromBytes(std::string_view bytes, ImportFormat format = ImportFormat::Unknown);

// True when luma(background) < 128 (Rec. 709), matching Themes::isDark.
[[nodiscard]] bool importedThemeIsDark(const ImportedTheme&);

} // namespace Ubiquitty::Themes
