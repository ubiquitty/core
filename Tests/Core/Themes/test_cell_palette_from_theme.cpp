// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Building a CellPalette from a Theme.
//
// The function is eleven lines and every one of them can be wrong in a way that
// looks like a theme problem rather than a code problem: a shifted memcpy tints
// the whole terminal, a named colour landing in the wrong field swaps the cursor
// with the text, and an extended fill running from 0 instead of 16 erases the
// theme it was just handed.
//
// Every case uses a theme whose colours are all DISTINCT, because the failures
// worth catching are the ones that copy the right NUMBER of colours to the wrong
// places. A theme of uniform grey satisfies all of them and asserts nothing.

#include "Harness.h"

#include "Core/CellColor.h"
#include "Core/Themes/Themes.h"

#include <cstring>

namespace {

// Slot i gets red = i, so a colour in the wrong slot names the slot it came
// from. Green and blue differ from red so a channel swap is visible too.
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

} // namespace

TEST(everyANSISlotTakesItsOwnColour)
{
    const Ubiquitty::CellPalette palette = Ubiquitty::CellPalette::fromTheme(distinctTheme());

    for (int i = 0; i < 16; ++i) {
        CHECK_MSG(palette.indexed[i].red == static_cast<uint8_t>(i),
            "slot i holds slot i's red, so a copy shifted by one is visible");
        CHECK(palette.indexed[i].green == static_cast<uint8_t>(0x40 + i));
        CHECK(palette.indexed[i].blue == static_cast<uint8_t>(0x80 + i));
    }
}

TEST(theNamedColoursDoNotTradePlaces)
{
    const Ubiquitty::CellPalette palette = Ubiquitty::CellPalette::fromTheme(distinctTheme());

    // Three fields of the same type in a row: the compiler cannot tell them
    // apart and neither can a reader, so the assignment order is worth pinning.
    CHECK_MSG(palette.defaultForeground.red == 0xF1, "foreground is the theme's foreground");
    CHECK_MSG(palette.defaultBackground.red == 0xB1, "background is the theme's background");
    CHECK_MSG(palette.cursor.red == 0xC1, "cursor is the theme's cursor");
}

TEST(theExtendedCubeIsFilledAndLeavesTheThemesSlotsAlone)
{
    const Ubiquitty::CellPalette palette = Ubiquitty::CellPalette::fromTheme(distinctTheme());

    // 16-255 are the xterm cube and greyscale ramp. They come from the fill, not
    // the theme, so a theme carrying sixteen colours still resolves a 256-colour
    // program.
    bool isAnyExtendedSlotSet = false;
    for (int i = 16; i < 256; ++i)
        isAnyExtendedSlotSet = isAnyExtendedSlotSet || palette.indexed[i].red != 0
            || palette.indexed[i].green != 0 || palette.indexed[i].blue != 0;
    CHECK_MSG(isAnyExtendedSlotSet, "the cube is filled rather than left at the value-initialised zero");

    // The fill's RANGE is what protects the theme, not its position in the
    // function: 16..255 and 0..15 do not overlap, so the two writes commute and
    // reordering them changes nothing. A fill starting from 0 does erase the
    // theme, and this catches it — asserted on slot 9, where the cube's entry
    // and the theme's differ, rather than on slot 0 where both are black.
    CHECK_MSG(palette.indexed[9].red == 9,
        "the extended fill starts at 16 and does not reach back over the ANSI block");
}

TEST(aThemeTurnsOnTheBoldIsBrightPalette)
{
    // CellPalette default-constructs with brightenBold false; a theme sets it.
    // Left off, bold text renders in the standard colour and the terminal looks
    // washed out against every other terminal on the machine.
    Ubiquitty::CellPalette bare {};
    CHECK_MSG(!bare.brightenBold, "a bare palette does not promote bold to bright");

    const Ubiquitty::CellPalette palette = Ubiquitty::CellPalette::fromTheme(distinctTheme());
    CHECK_MSG(palette.brightenBold, "one built from a theme does");
}

UBIQUITTY_TEST_MAIN()
