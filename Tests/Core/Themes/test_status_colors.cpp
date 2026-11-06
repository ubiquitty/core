// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_status_colors.cpp — a status is drawn in the theme's colour for it.
//
// The slots' defaults come from the theme's own ANSI colours, every theme has
// them, and the colour Core hands a frontend for a program's status follows the
// pane's theme rather than a literal: the same record is a different blue under
// a different theme, and a program repainting an ANSI slot does not move it.

#include "Core/App/ProgramActivity.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Themes/ThemePreferences.h"
#include "Core/Themes/ThemeTOMLWrite.h"
#include "Core/Themes/Themes.h"
#include "Core/VT/Parser.h"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using namespace Ubiquitty;
using Themes::StatusColorSlot;

namespace {

bool isColor(ColorRGBA color, std::uint8_t red, std::uint8_t green, std::uint8_t blue) { return color.red == red && color.green == green && color.blue == blue && color.alpha == 255; }

Themes::Theme sampleTheme()
{
    Themes::Theme theme {};
    for (int index = 0; index < 16; ++index)
        theme.ansi[index] = { static_cast<std::uint8_t>(10 * index), static_cast<std::uint8_t>(10 * index + 1), static_cast<std::uint8_t>(10 * index + 2), 128 };
    theme.defaultForeground = { 200, 100, 0 };
    theme.defaultBackground = { 0, 50, 101 };
    return theme;
}

Preferences::Values parseString(const std::string& text)
{
    Preferences::Values values;
    std::istringstream input(text);
    Preferences::parse(input, values);
    return values;
}

void setBaseline(Terminal& terminal, const Themes::Theme& theme)
{
    const auto wide = [](ColorRGBA color) { return PaletteRGB { static_cast<std::uint16_t>(color.red * 257), static_cast<std::uint16_t>(color.green * 257), static_cast<std::uint16_t>(color.blue * 257) }; };
    for (int index = 0; index < 16; ++index)
        terminal.paletteBaseline.indexed[index] = wide(theme.ansi[index]);
    terminal.paletteBaseline.defaultForeground = wide(theme.defaultForeground);
    terminal.paletteBaseline.defaultBackground = wide(theme.defaultBackground);
    terminal.palette = terminal.paletteBaseline;
}

} // namespace

TEST(aThemesStatusColoursDefaultToItsOwnANSIColours)
{
    const Themes::StatusColors colors = sampleTheme().statusColors();
    CHECK_MSG(isColor(colors.color(StatusColorSlot::Failed), 10, 11, 12), "failed is the theme's red, opaque whatever the slot's alpha");
    CHECK(isColor(colors.color(StatusColorSlot::Blocked), 30, 31, 32));
    CHECK(isColor(colors.color(StatusColorSlot::Attention), 50, 51, 52));
    CHECK(isColor(colors.color(StatusColorSlot::Done), 20, 21, 22));
    CHECK(isColor(colors.color(StatusColorSlot::Working), 40, 41, 42));
    CHECK_MSG(isColor(colors.color(StatusColorSlot::Neutral), 100, 75, 51), "neutral is halfway between foreground and background");

    // Every registered theme has all six, distinct from black on black.
    CHECK(Themes::count() > 0);
    for (int index = 0; index < Themes::count(); ++index) {
        const Themes::Theme* theme = Themes::get(index);
        CHECK(theme != nullptr);
        if (!theme)
            continue;
        const Themes::StatusColors themed = theme->statusColors();
        CHECK(isColor(themed.color(StatusColorSlot::Failed), theme->ansi[1].red, theme->ansi[1].green, theme->ansi[1].blue));
        CHECK(isColor(themed.color(StatusColorSlot::Working), theme->ansi[4].red, theme->ansi[4].green, theme->ansi[4].blue));
    }
}

TEST(everyStatusKindHasASlot)
{
    using App::StatusKind;
    CHECK(App::statusColorSlot(StatusKind::Failed) == StatusColorSlot::Failed);
    CHECK(App::statusColorSlot(StatusKind::Blocked) == StatusColorSlot::Blocked);
    CHECK(App::statusColorSlot(StatusKind::Attention) == StatusColorSlot::Attention);
    CHECK(App::statusColorSlot(StatusKind::Done) == StatusColorSlot::Done);
    CHECK(App::statusColorSlot(StatusKind::Working) == StatusColorSlot::Working);
    for (const StatusKind quiet : { StatusKind::Running, StatusKind::Idle, StatusKind::None })
        CHECK(App::statusColorSlot(quiet) == StatusColorSlot::Neutral);
}

TEST(aProgramsStatusIsDrawnInThePanesTheme)
{
    Terminal terminal;
    hardReset(terminal);
    const Themes::Theme theme = sampleTheme();
    setBaseline(terminal, theme);
    CHECK(!App::tabActivityColor(&terminal));

    const auto feed = [&](std::string_view bytes) { processBytes(&terminal, bytes); };
    const auto shows = [&](std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
        const std::optional<ColorRGBA> color = App::tabActivityColor(&terminal);
        return color && isColor(*color, red, green, blue);
    };
    feed("\033]7501;id=job:state=working\033\\");
    CHECK_MSG(shows(40, 41, 42), "working is the theme's working colour, not a literal");
    feed("\033]7501;id=job:state=blocked:kind=question\033\\");
    CHECK(shows(30, 31, 32));
    feed("\033]7501;id=job:state=error\033\\");
    CHECK(shows(10, 11, 12));
    feed("\033]7501;id=job:state=done\033\\");
    CHECK(shows(20, 21, 22));
    feed("\033]7501;id=job:state=idle\033\\");
    CHECK(shows(100, 75, 51));

    // A program repainting the ANSI slot changes its text, not the chrome's status colour.
    feed("\033]7501;id=job:state=error\033\\\033]4;1;rgb:ff/ff/ff\033\\");
    CHECK_MSG(shows(10, 11, 12), "the status colour is the theme's, not the program's OSC 4 write");

    // Another theme, another colour for the same record.
    Themes::Theme other = theme;
    other.ansi[1] = { 250, 1, 2 };
    setBaseline(terminal, other);
    CHECK(shows(250, 1, 2));

    // With no record the program's own indicator colour still shows.
    feed("\033]7501;id=job:state=clear\033\\\033]21337;indicator=#0d0e0f\007");
    CHECK(shows(13, 14, 15));
}

TEST(aThemeFileNamesItsOwnStatusColours)
{
    const Preferences::Values parsed = parseString(
        "[[themes]]\n"
        "name = \"Named\"\n"
        "ansi1 = \"#101112\"\n"
        "ansi2 = \"#202122\"\n"
        "status_failed = \"#aa0000\"\n"
        "status_neutral = \"#0a0b0c\"\n"
        "status_unknown = \"#ffffff\"\n"
        "status_done = \"not a colour\"\n");
    CHECK(parsed.themes.size() == 1);
    if (parsed.themes.size() != 1)
        return;
    const Preferences::ThemeEntry& entry = parsed.themes[0];
    CHECK(entry.status[static_cast<std::size_t>(StatusColorSlot::Failed)].set);
    CHECK(!entry.status[static_cast<std::size_t>(StatusColorSlot::Working)].set);
    CHECK_MSG(!entry.status[static_cast<std::size_t>(StatusColorSlot::Done)].set, "a value that is not a colour names nothing");

    const Themes::Theme theme = Themes::themeFromValues(entry, nullptr);
    const Themes::StatusColors colors = theme.statusColors();
    CHECK_MSG(isColor(colors.color(StatusColorSlot::Failed), 0xaa, 0, 0), "a named slot is the named colour");
    CHECK(isColor(colors.color(StatusColorSlot::Neutral), 0x0a, 0x0b, 0x0c));
    CHECK_MSG(isColor(colors.color(StatusColorSlot::Done), 0x20, 0x21, 0x22), "an unnamed slot is still derived from the theme's ANSI colours");

    // Written back, the named slots are keys and the derived ones are not, and
    // the block reads back to the same colours.
    const std::string block = Themes::renderThemeBlock("Named", true, theme.ansi, theme.defaultForeground, theme.defaultBackground, theme.cursor, {}, theme.statusOverrides);
    CHECK(block.find("status_failed = \"#aa0000\"") != std::string::npos);
    CHECK(block.find("status_neutral = \"#0a0b0c\"") != std::string::npos);
    CHECK_MSG(block.find("status_done") == std::string::npos && block.find("status_working") == std::string::npos, "a derived colour is not frozen into the file");
    const Preferences::Values reread = parseString(block);
    CHECK(reread.themes.size() == 1);
    if (reread.themes.size() == 1) {
        const Themes::StatusColors again = Themes::themeFromValues(reread.themes[0], nullptr).statusColors();
        for (std::size_t slot = 0; slot < Themes::statusColorSlotCount; ++slot) {
            const ColorRGBA first = colors.slots[slot];
            CHECK(isColor(again.slots[slot], first.red, first.green, first.blue));
        }
    }
    Themes::Theme exported = sampleTheme();
    exported.name = "Exported";
    CHECK_MSG(exported.serializeTOML().find("status_") == std::string::npos, "a theme that names none exports none");
    exported.statusOverrides[static_cast<std::size_t>(StatusColorSlot::Blocked)] = ColorRGBA { 1, 2, 3 };
    CHECK(exported.serializeTOML().find("status_blocked = \"#010203\"") != std::string::npos);
}

TEST(aSnapshotsStatusColourFollowsTheThemeItIsReadUnder)
{
    Themes::Theme theme = sampleTheme();
    theme.statusOverrides[static_cast<std::size_t>(StatusColorSlot::Working)] = ColorRGBA { 9, 8, 7 };
    const Themes::StatusColors themed = theme.statusColors();

    Runtime::RuntimeSnapshot snapshot;
    CHECK_MSG(!snapshot.tabStatusColor(themed).isSet, "no colour set, none to draw");

    // A program's status record: the slot decides, under whichever theme reads it.
    snapshot.isTabStatusColorSet = true;
    snapshot.tabStatusRed = 40;
    snapshot.tabStatusGreen = 41;
    snapshot.tabStatusBlue = 42;
    snapshot.tabStatusSlot = std::to_underlying(StatusColorSlot::Working);
    const Runtime::TabStatusColor working = snapshot.tabStatusColor(themed);
    CHECK_MSG(working.isSet && working.red == 9 && working.green == 8 && working.blue == 7, "the theme's named colour, not the palette-derived one the snapshot carries");

    // An OSC 21337 indicator names no slot and is drawn as the program sent it.
    snapshot.tabStatusSlot = Themes::noStatusColorSlot;
    const Runtime::TabStatusColor chosen = snapshot.tabStatusColor(themed);
    CHECK(chosen.isSet && chosen.red == 40 && chosen.green == 41 && chosen.blue == 42);

    // The terminal says which it is.
    Terminal terminal;
    hardReset(terminal);
    CHECK(!App::tabActivitySlot(&terminal));
    processBytes(&terminal, "\033]21337;indicator=#0d0e0f\007");
    CHECK_MSG(!App::tabActivitySlot(&terminal), "a program's own indicator colour names no theme slot");
    processBytes(&terminal, "\033]7501;id=job:state=blocked:kind=question\033\\");
    CHECK(App::tabActivitySlot(&terminal) == StatusColorSlot::Blocked);
}

TEST(aMarkIsDrawnInThePlatformsColourUntilTheThemeNamesOne)
{
    using App::StatusMark;
    CHECK(App::statusMarkSlot(StatusMark::Progress) == StatusColorSlot::Progress);
    CHECK(App::statusMarkSlot(StatusMark::ProgressInactive) == StatusColorSlot::Neutral);
    CHECK(App::statusMarkSlot(StatusMark::ProgressError) == StatusColorSlot::Failed);
    CHECK(App::statusMarkSlot(StatusMark::Notification) == StatusColorSlot::Attention);
    CHECK(App::progressStatusMark(OSC::ProgressTint::Accent) == StatusMark::Progress);
    CHECK(App::progressStatusMark(OSC::ProgressTint::Inactive) == StatusMark::ProgressInactive);
    CHECK(App::progressStatusMark(OSC::ProgressTint::Error) == StatusMark::ProgressError);

    Themes::Theme theme = sampleTheme();
    const Themes::StatusColors derived = theme.statusColors();
    CHECK_MSG(isColor(derived.color(StatusColorSlot::Progress), 40, 41, 42), "progress derives from the theme's blue");
    CHECK(!derived.isNamed(StatusColorSlot::Progress) && !derived.isNamed(StatusColorSlot::Failed));

    const App::PlatformStatusColors everything { .hasProgress = true, .hasProgressInactive = true, .hasProgressError = true, .hasNotification = true };
    const App::PlatformStatusColors nothing {};
    for (const StatusMark mark : { StatusMark::Progress, StatusMark::ProgressInactive, StatusMark::ProgressError, StatusMark::Notification }) {
        CHECK_MSG(App::statusMarkInk(mark, derived, everything).isPlatformColor, "an unnamed slot leaves the mark to a platform that has a colour for it");
        const App::StatusMarkInk themed = App::statusMarkInk(mark, derived, nothing);
        const ColorRGBA slotColor = derived.color(App::statusMarkSlot(mark));
        CHECK_MSG(!themed.isPlatformColor && isColor(themed.color, slotColor.red, slotColor.green, slotColor.blue), "a platform with no colour of its own draws the theme's");
    }

    // Each platform fact speaks for its own mark only.
    const App::PlatformStatusColors accentOnly { .hasProgress = true };
    CHECK(App::statusMarkInk(StatusMark::Progress, derived, accentOnly).isPlatformColor);
    CHECK(!App::statusMarkInk(StatusMark::ProgressInactive, derived, accentOnly).isPlatformColor);
    CHECK(!App::statusMarkInk(StatusMark::ProgressError, derived, accentOnly).isPlatformColor);
    CHECK(!App::statusMarkInk(StatusMark::Notification, derived, accentOnly).isPlatformColor);

    // A colour the theme names wins over the platform's, for that slot alone.
    theme.statusOverrides[static_cast<std::size_t>(StatusColorSlot::Failed)] = ColorRGBA { 200, 1, 1 };
    theme.statusOverrides[static_cast<std::size_t>(StatusColorSlot::Progress)] = ColorRGBA { 1, 200, 1 };
    const Themes::StatusColors named = theme.statusColors();
    CHECK(named.isNamed(StatusColorSlot::Failed) && named.isNamed(StatusColorSlot::Progress) && !named.isNamed(StatusColorSlot::Attention));
    const App::StatusMarkInk error = App::statusMarkInk(StatusMark::ProgressError, named, everything);
    CHECK_MSG(!error.isPlatformColor && isColor(error.color, 200, 1, 1), "the theme's failed colour is drawn for a failed progress bar");
    const App::StatusMarkInk progress = App::statusMarkInk(StatusMark::Progress, named, everything);
    CHECK(!progress.isPlatformColor && isColor(progress.color, 1, 200, 1));
    CHECK_MSG(App::statusMarkInk(StatusMark::Notification, named, everything).isPlatformColor, "a slot the theme did not name stays the platform's");
}

UBIQUITTY_TEST_MAIN()
