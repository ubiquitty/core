// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_theme_apply.cpp — a theme's colours read without applying it
// (Core/Themes/ThemeApply.cpp) are the colours applying it draws.
//
// A host paints its chrome from these before the theme reaches a terminal,
// and from the terminal after; a reading that skipped the adaptation the
// apply makes would paint the theme file's colours around a grid drawing
// others.

#include "Core/Accessibility/Palette.h"
#include "Core/Rendering/PaletteColors.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Themes/ThemeApply.h"
#include "Core/Themes/Themes.h"

#include <cstddef>
#include <optional>
#include <vector>

using namespace Ubiquitty;
using Rendering::ansiColorCount;
using Rendering::PaletteColors;
using Rendering::paletteColors;

namespace {

/// Restores the process-wide palette adaptation when a case that sets it ends.
class AdaptationScope {
public:
    AdaptationScope()
        : m_saved(Accessibility::paletteAdaptation())
    {
    }

    ~AdaptationScope() { Accessibility::paletteAdaptation() = m_saved; }

    AdaptationScope(const AdaptationScope&) = delete;
    AdaptationScope& operator=(const AdaptationScope&) = delete;

private:
    Accessibility::PaletteAdaptation m_saved;
};

bool isSameColor(const ColorRGBA& left, const ColorRGBA& right)
{
    return left.red == right.red && left.green == right.green && left.blue == right.blue && left.alpha == right.alpha;
}

bool isSameColors(const PaletteColors& left, const PaletteColors& right)
{
    if (!isSameColor(left.foreground, right.foreground) || !isSameColor(left.background, right.background) || !isSameColor(left.cursor, right.cursor))
        return false;
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        if (!isSameColor(left.ansi[index], right.ansi[index]))
            return false;
    }
    return true;
}

/// The colours a theme file names, opaque where it names no alpha.
PaletteColors fileColors(const Themes::Theme& theme)
{
    PaletteColors colors;
    colors.foreground = { theme.defaultForeground.red, theme.defaultForeground.green, theme.defaultForeground.blue };
    colors.background = { theme.defaultBackground.red, theme.defaultBackground.green, theme.defaultBackground.blue };
    colors.cursor = { theme.cursor.red, theme.cursor.green, theme.cursor.blue };
    for (std::size_t index = 0; index < ansiColorCount; ++index)
        colors.ansi[index] = theme.ansi[index];
    return colors;
}

} // namespace

TEST(aThemesColoursAreTheColoursApplyingItDraws)
{
    // Under the system's full contrast request, which moves a theme's colours.
    AdaptationScope scope;
    Accessibility::paletteAdaptation().contrastStrength = 1;
    REQUIRE(Themes::count() > 0);
    for (int index = 0; index < Themes::count(); ++index) {
        Terminal terminal;
        hardReset(terminal);
        REQUIRE(Themes::applyThemeToTerminal(terminal, index));
        const std::optional<PaletteColors> colors = Themes::themeColors(index);
        REQUIRE(colors.has_value());
        CHECK_MSG(isSameColors(*colors, paletteColors(terminal.palette)), "the colours read for a theme are the ones its apply draws");
    }
}

TEST(theColoursFollowTheAdaptation)
{
    AdaptationScope scope;
    Accessibility::paletteAdaptation() = {};
    std::vector<PaletteColors> unadapted;
    for (int index = 0; index < Themes::count(); ++index) {
        const Themes::Theme* theme = Themes::get(index);
        const std::optional<PaletteColors> colors = Themes::themeColors(index);
        REQUIRE(theme && colors.has_value());
        CHECK_MSG(isSameColors(*colors, fileColors(*theme)), "with nothing to adapt for, a theme's colours are its file's");
        unadapted.push_back(*colors);
    }

    // The case above holds only if this one moves something: a contrast
    // request the reading ignored would leave every theme as its file.
    Accessibility::paletteAdaptation().contrastStrength = 1;
    bool hasMoved = false;
    for (int index = 0; index < Themes::count(); ++index) {
        const std::optional<PaletteColors> colors = Themes::themeColors(index);
        REQUIRE(colors.has_value());
        hasMoved = hasMoved || !isSameColors(*colors, unadapted[static_cast<std::size_t>(index)]);
    }
    CHECK_MSG(hasMoved, "the full contrast request moves some theme's colours");
}

TEST(anIndexNamingNoThemeHasNoColours)
{
    CHECK_MSG(!Themes::themeColors(-1).has_value(), "a negative index names no theme");
    CHECK_MSG(!Themes::themeColors(Themes::count()).has_value(), "one past the last theme names no theme");
    CHECK_MSG(!Themes::resolveThemePalette(Themes::count(), Accessibility::paletteAdaptation()).has_value(), "nor does it resolve a palette");
}

UBIQUITTY_TEST_MAIN()
