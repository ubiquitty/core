// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Applying a Theme to a Terminal's live 16-bit palette.
//
// Every case here uses a theme whose colours are all DISTINCT, because the
// failures worth catching are the ones that copy the right number of colours to
// the wrong places: an off-by-one over the sixteen ANSI slots, a foreground
// written into the cursor, one colour broadcast to all of them. A theme of
// uniform grey satisfies every one of those and asserts nothing.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Accessibility/Palette.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Themes/Themes.h"

using Ubiquitty::Themes::applyThemeToTerminal;
using Ubiquitty::Themes::applyToTermPalette;

namespace {

// Slot i gets red = i, so a colour landing in the wrong slot names the slot it
// came from. Green and blue differ from red so a channel swap is visible too.
Ubiquitty::Themes::Theme distinctTheme()
{
    Ubiquitty::Themes::Theme theme {};
    theme.name = "distinct";
    for (int i = 0; i < 16; ++i)
        theme.ansi[i] = Ubiquitty::ColorRGBA { static_cast<uint8_t>(i), static_cast<uint8_t>(0x40 + i),
            static_cast<uint8_t>(0x80 + i), 255 };
    theme.defaultForeground = Ubiquitty::ColorRGBA { 0xF1, 0xF2, 0xF3, 255 };
    theme.defaultBackground = Ubiquitty::ColorRGBA { 0xB1, 0xB2, 0xB3, 255 };
    theme.cursor = Ubiquitty::ColorRGBA { 0xC1, 0xC2, 0xC3, 255 };
    return theme;
}

// The conversion the palette stores: an 8-bit channel doubled into 16 bits, so
// 0xC0 becomes 0xC0C0 rather than 0xC000.
uint16_t widened(uint8_t v)
{
    return static_cast<uint16_t>((static_cast<unsigned>(v) << 8) | v);
}

} // namespace

TEST(testEachANSISlotTakesItsOwnColour)
{
    const Ubiquitty::Themes::Theme theme = distinctTheme();
    Ubiquitty::TermPalette palette {};
    applyToTermPalette(theme, palette);

    for (int i = 0; i < 16; ++i) {
        CHECK_MSG(palette.indexed[i].red == widened(static_cast<uint8_t>(i)),
            "slot i holds slot i's red, so a shifted copy is visible");
        CHECK(palette.indexed[i].green == widened(static_cast<uint8_t>(0x40 + i)));
        CHECK(palette.indexed[i].blue == widened(static_cast<uint8_t>(0x80 + i)));
    }
}

TEST(testTheNamedColoursDoNotTradePlaces)
{
    const Ubiquitty::Themes::Theme theme = distinctTheme();
    Ubiquitty::TermPalette palette {};
    applyToTermPalette(theme, palette);

    CHECK_MSG(palette.defaultForeground.red == widened(0xF1), "foreground is the theme's foreground");
    CHECK_MSG(palette.defaultBackground.red == widened(0xB1), "background is the theme's background");
    CHECK_MSG(palette.cursor.red == widened(0xC1), "cursor is the theme's cursor");
}

TEST(testAnEightBitChannelIsDoubledNotShifted)
{
    Ubiquitty::Themes::Theme theme = distinctTheme();
    theme.ansi[0] = Ubiquitty::ColorRGBA { 0xC0, 0x00, 0xFF, 255 };
    Ubiquitty::TermPalette palette {};
    applyToTermPalette(theme, palette);

    // 0xC0C0, not 0xC000: the low byte repeats the high one, so full-scale 8-bit
    // reaches full-scale 16-bit and 0xFF does not land at 0xFF00.
    CHECK(palette.indexed[0].red == 0xC0C0);
    CHECK(palette.indexed[0].green == 0x0000);
    CHECK_MSG(palette.indexed[0].blue == 0xFFFF, "0xFF widens to the maximum, not 0xFF00");
}

TEST(testAnIndexNamingNoThemeChangesNothing)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    const Ubiquitty::TermPalette before = terminal.palette;

    CHECK_MSG(!applyThemeToTerminal(terminal, -1), "a negative index names no theme");
    CHECK_MSG(!applyThemeToTerminal(terminal, Ubiquitty::Themes::count()),
        "one past the last theme names no theme");

    CHECK_MSG(std::memcmp(&terminal.palette, &before, sizeof(before)) == 0,
        "a refused apply leaves the palette exactly as it was");
}

TEST(testApplyingTakesTheBaselineWithIt)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    if (Ubiquitty::Themes::count() <= 0) {
        CHECK_MSG(false, "no themes are registered, so this asserts nothing");
        return;
    }

    CHECK(applyThemeToTerminal(terminal, 0));
    // The baseline is what OSC 104/110-119 restore to. Left stale, a reset would
    // return the terminal to whatever theme was applied before this one.
    CHECK_MSG(std::memcmp(&terminal.paletteBaseline, &terminal.palette, sizeof(terminal.palette)) == 0,
        "the baseline is the palette that was just applied");
}

TEST(testApplyingAThemeLeavesTheCubeAlone)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    if (Ubiquitty::Themes::count() <= 0)
        return;

    // 16-255 are the xterm cube and greyscale ramp, seeded at construction and
    // independent of the theme — a theme sets 0-15 and the three named colours
    // and nothing else. Asserted as UNCHANGED rather than as non-zero: the cube
    // is already filled before any theme is applied, so "something up there is
    // non-zero" holds whether or not this function runs, and cannot fail.
    Ubiquitty::PaletteRGB before[240];
    for (int i = 16; i < 256; ++i)
        before[i - 16] = terminal.palette.indexed[i];

    CHECK(applyThemeToTerminal(terminal, 0));

    for (int i = 16; i < 256; ++i) {
        const Ubiquitty::PaletteRGB& now = terminal.palette.indexed[i];
        CHECK_MSG(now.red == before[i - 16].red && now.green == before[i - 16].green
                && now.blue == before[i - 16].blue,
            "a theme apply does not disturb the extended cube");
    }
}

// The OSC 110/111/112 revert target, which is the half a theme apply is easy to
// leave behind.
//
// `resetDefaultForeground` restores `palette` from `paletteBaseline`, so a test
// that applies a theme and asserts the two are equal after a reset passes
// whatever the baseline holds — it is the function's own postcondition read
// back. What decides whether a user sees their theme again is the VALUE, so
// these assert against the theme rather than against the baseline.
//
// The two entry points differ here and only here: `applyThemeToTerminal` mirrors
// the baseline, `applyToTermPalette` writes a bare palette and knows of no
// Terminal to mirror into. Both are correct for what they are, which is why a
// frontend reaching for the second one against a live Terminal's palette gets a
// terminal whose theme survives until the first OSC 110 and then reverts to a
// colour the user never chose.

TEST(testOSC110RevertsToTheAppliedTheme)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    const Ubiquitty::Themes::Theme theme = distinctTheme();
    applyToTermPalette(theme, t.palette);
    t.paletteBaseline = t.palette; // what applyThemeToTerminal does for a caller

    const Ubiquitty::PaletteRGB themed = t.palette.defaultForeground;
    UbiquittyTest::feed(t, "\033]10;#aaaabbbbcccc\033\\");
    CHECK(t.palette.defaultForeground.red == 0xaaaa);

    UbiquittyTest::feed(t, "\033]110\033\\");
    CHECK_MSG(t.palette.defaultForeground.red == themed.red
            && t.palette.defaultForeground.green == themed.green
            && t.palette.defaultForeground.blue == themed.blue,
        "OSC 110 reverts to the theme's foreground, not to the seeded default");
}

TEST(testAThemeApplyThatSkipsTheBaselineLosesItAtTheFirstReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const Ubiquitty::PaletteRGB seeded = t.palette.defaultForeground;

    // The bare-palette entry point, used exactly as a frontend would misuse it:
    // against a live Terminal's palette, with no baseline mirror.
    const Ubiquitty::Themes::Theme theme = distinctTheme();
    applyToTermPalette(theme, t.palette);

    const Ubiquitty::PaletteRGB themed = t.palette.defaultForeground;
    CHECK_MSG(themed.red != seeded.red || themed.green != seeded.green || themed.blue != seeded.blue,
        "the theme has to differ from the seed or the case below cannot fail");

    UbiquittyTest::feed(t, "\033]110\033\\");
    // This is the defect stated as a fact, not a wish: the bare-palette apply
    // never moves the reset target, so the revert lands on the seed. A
    // frontend that takes this path ships it.
    CHECK_MSG(t.palette.defaultForeground.red == seeded.red,
        "a bare-palette theme apply leaves the OSC 110 target on the seeded default");
}

UBIQUITTY_TEST_MAIN()

// A theme applied to a terminal goes through the one palette adaptation, the
// same transform the renderer runs on its 8-bit palette, so the colour-vision
// remap and the system's contrast request reach a window whichever arm opened
// it. Asserted against the 8-bit transform itself rather than a pinned colour:
// a second implementation inside the theme routine could agree with a pinned
// number and disagree with the renderer.
TEST(testAThemeAppliedToATerminalIsAdapted)
{
    using Ubiquitty::Accessibility::ColorblindMode;
    using Ubiquitty::Accessibility::PaletteAdaptation;
    using Ubiquitty::Accessibility::paletteAdaptation;
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    if (Ubiquitty::Themes::count() <= 0) {
        CHECK_MSG(false, "no themes are registered, so this asserts nothing");
        return;
    }
    const int index = 0;
    const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(index);
    CHECK(theme != nullptr);

    // The transform on its own, on the 8-bit colours the theme is authored in.
    Ubiquitty::CellPalette expected {};
    for (int i = 0; i < 16; ++i)
        expected.indexed[i] = theme->ansi[i];
    expected.defaultForeground = theme->defaultForeground;
    expected.defaultBackground = theme->defaultBackground;
    expected.cursor = theme->cursor;
    const PaletteAdaptation adaptation { ColorblindMode::Deuteranopia, 1.0 };
    Ubiquitty::Accessibility::adaptPalette(expected, adaptation);

    paletteAdaptation() = adaptation;
    CHECK(applyThemeToTerminal(term, index));
    // Every slot the adaptation touches, and the remap moved at least one:
    // the red and green slots trade for distinguishable hues.
    bool didMove = false;
    for (int i = 0; i < 16; ++i) {
        CHECK_MSG(term.palette.indexed[i].red == widened(expected.indexed[i].red), "slot red through the one transform");
        CHECK(term.palette.indexed[i].green == widened(expected.indexed[i].green));
        CHECK(term.palette.indexed[i].blue == widened(expected.indexed[i].blue));
        if (expected.indexed[i].red != theme->ansi[i].red || expected.indexed[i].green != theme->ansi[i].green)
            didMove = true;
    }
    CHECK_MSG(didMove, "the adaptation changed the base, or the test saw a no-op");
    CHECK(term.palette.defaultForeground.red == widened(expected.defaultForeground.red));
    CHECK(term.palette.defaultBackground.red == widened(expected.defaultBackground.red));
    // The baseline follows the adapted palette, so a reset lands on it.
    CHECK(term.paletteBaseline.indexed[1].red == term.palette.indexed[1].red);

    // Back to no adaptation: the same theme applies unadapted, and the
    // process-wide state leaves the way it came for the tests after this one.
    paletteAdaptation() = PaletteAdaptation {};
    CHECK(applyThemeToTerminal(term, index));
    CHECK(term.palette.indexed[1].red == widened(theme->ansi[1].red));
    CHECK(term.palette.indexed[1].green == widened(theme->ansi[1].green));
}

// A theme's per-index opacity lands in the palette beside its colour: the
// sixteen it carries keep theirs, and the cube it does not carry is opaque —
// a whole apply, so a slot the previous theme dimmed does not stay dimmed.
TEST(testThemeAlphaLandsBesideItsColour)
{
    Ubiquitty::Themes::Theme theme = distinctTheme();
    theme.ansi[3].alpha = 0x40;
    theme.ansi[9].alpha = 0xC0;
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    for (int i = 0; i < 256; ++i)
        CHECK(palette.indexedAlpha[static_cast<size_t>(i)] == 255);
    palette.indexedAlpha[200] = 0x10;
    palette.indexedAlpha[4] = 0x10;
    applyToTermPalette(theme, palette);
    CHECK(palette.indexedAlpha[3] == 0x40);
    CHECK(palette.indexedAlpha[9] == 0xC0);
    CHECK(palette.indexedAlpha[4] == 255);
    CHECK(palette.indexedAlpha[200] == 255);
    // And the colour beside it is the theme's, so the alpha did not land in
    // a slot other than its colour's.
    CHECK(palette.indexed[3].red == Ubiquitty::channel8To16(theme.ansi[3].red));
}
