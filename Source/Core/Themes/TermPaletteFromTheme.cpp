// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Accessibility/Palette.h"
#include "Core/Terminal.h"

#include "Core/TerminalOperations.h"

namespace Ubiquitty::Themes {

void applyToTermPalette(const Theme& theme, Ubiquitty::TermPalette& palette)
{
    auto toRGB = [](ColorRGBA color) -> Ubiquitty::PaletteRGB { return { Ubiquitty::channel8To16(color.red), Ubiquitty::channel8To16(color.green), Ubiquitty::channel8To16(color.blue) }; };
    for (int i = 0; i < 16; ++i) {
        palette.indexed[i] = toRGB(theme.ansi[i]);
        palette.indexedAlpha[static_cast<size_t>(i)] = theme.ansi[i].alpha;
    }
    // The cube a theme does not carry is opaque: a theme apply is whole, so a
    // slot a program dimmed through the previous theme does not stay dimmed.
    for (int i = 16; i < 256; ++i)
        palette.indexedAlpha[static_cast<size_t>(i)] = 255;
    palette.defaultForeground = toRGB(theme.defaultForeground);
    palette.defaultBackground = toRGB(theme.defaultBackground);
    palette.cursor = toRGB(theme.cursor);
}

std::optional<Ubiquitty::TermPalette> resolveThemePalette(int themeIndex, const Ubiquitty::Accessibility::PaletteAdaptation& adaptation)
{
    const Theme* theme = get(themeIndex);
    if (!theme)
        return std::nullopt;
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    applyToTermPalette(*theme, palette);
    // Before the cube is filled, so the 240 derived entries describe the
    // adapted base.
    Accessibility::adaptPalette(palette, adaptation);
    palette.fillExtended();
    return palette;
}

bool applyThemeToTerminal(Ubiquitty::Terminal& terminal, int themeIndex)
{
    const Theme* theme = get(themeIndex);
    if (!theme)
        return false;
    applyToTermPalette(*theme, terminal.palette);
    // Before the cube is filled, so the 240 derived entries describe the
    // adapted base rather than the palette that was replaced.
    Accessibility::adaptPalette(terminal.palette, Accessibility::paletteAdaptation());
    terminal.palette.fillExtended();
    terminal.paletteBaseline = terminal.palette;
    ++terminal.paletteEpoch;
    return true;
}

} // namespace Ubiquitty::Themes
