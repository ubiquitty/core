// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_chrome_colors.cpp — the four theme colours chrome tints with, as Core
// derives them off the model's palette (Core/TerminalOperations.cpp). One
// derivation for every frontend: a shell narrowing the 16-bit palette itself,
// or seeding the accent from the cursor, is a second reading of the theme.

#include "Core/Runtime/RuntimeLink.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

using namespace Ubiquitty;

namespace {

PaletteRGB wide(uint8_t red, uint8_t green, uint8_t blue)
{
    return { channel8To16(red), channel8To16(green), channel8To16(blue) };
}

bool is(const ColorRGBA& color, uint8_t red, uint8_t green, uint8_t blue)
{
    return color.red == red && color.green == green && color.blue == blue && color.alpha == 0xFF;
}

} // namespace

TEST(chromeColorsFollowTheModelsPalette)
{
    Terminal t;
    hardReset(t);
    t.palette.defaultForeground = wide(0x11, 0x22, 0x33);
    t.palette.defaultBackground = wide(0x44, 0x55, 0x66);
    t.palette.cursor = wide(0x77, 0x88, 0x99);
    t.palette.indexed[4] = wide(0x12, 0x34, 0x56);

    CHECK(is(chromeForegroundColor(&t), 0x11, 0x22, 0x33));
    CHECK(is(chromeBackgroundColor(&t), 0x44, 0x55, 0x66));
    CHECK(is(chromeCursorColor(&t), 0x77, 0x88, 0x99));
    CHECK_MSG(is(chromeAccentColor(&t), 0x12, 0x34, 0x56), "the accent is ANSI blue, the theme's own, not the cursor");

    // Live: an OSC 4 override of slot 4 moves the accent with it.
    t.palette.indexed[4] = wide(0x65, 0x43, 0x21);
    CHECK_MSG(is(chromeAccentColor(&t), 0x65, 0x43, 0x21), "the accent follows the palette rather than being sampled once");
}

TEST(chromeColorsWithoutATerminalAreTheCompiledInDefaults)
{
    TermPalette defaults;
    defaults.seedDefaults();
    const PaletteRGB blue = defaults.indexed[4];
    CHECK(is(chromeAccentColor(nullptr), channel16To8(blue.red), channel16To8(blue.green), channel16To8(blue.blue)));
    CHECK(is(chromeCursorColor(nullptr), defaultCursorRGB[0], defaultCursorRGB[1], defaultCursorRGB[2]));
}

// The chrome's own tint, folded off the published snapshot rather than by each
// shell: the window's backing, its title row, the tab band and every pane bar
// are painted from this one answer, and a frontend deriving it beside the
// others is how a title row came to disagree with the grid under it.
TEST(chromeTintFollowsTheLetterboxThenTheGround)
{
    Runtime::RuntimeSnapshot snapshot;
    snapshot.defaultBackgroundRed = 0.125F;
    snapshot.defaultBackgroundGreen = 0.25F;
    snapshot.defaultBackgroundBlue = 0.5F;
    snapshot.letterboxColor = 0x1A2B3C;

    const Runtime::ChromeTint ground = Runtime::chromeTint(snapshot);
    CHECK_MSG(!ground.isLetterbox, "no letterbox stands, so the tint is the ground");
    CHECK(ground.red == 0.125F && ground.green == 0.25F && ground.blue == 0.5F);

    // The effective background is what the snapshot publishes: DECSCNM's
    // whole-frame swap is applied where the snapshot is filled, so a reversed
    // screen paints its chrome the colour the frame clears to.
    snapshot.defaultBackgroundRed = 0.9F;
    CHECK(Runtime::chromeTint(snapshot).red == 0.9F);
    snapshot.defaultBackgroundRed = 0.125F;

    snapshot.isEdgeToEdge = true;
    const Runtime::ChromeTint letterbox = Runtime::chromeTint(snapshot);
    CHECK_MSG(letterbox.isLetterbox, "under a Color letterbox the fill is the tint");
    CHECK(is(ColorRGBA { static_cast<uint8_t>(letterbox.red * 255 + 0.5F), static_cast<uint8_t>(letterbox.green * 255 + 0.5F),
                 static_cast<uint8_t>(letterbox.blue * 255 + 0.5F), 0xFF },
        0x1A, 0x2B, 0x3C));
}

TEST(chromeTintFlipsPolarityAtTheMidpoint)
{
    // The polarity the system-drawn title and the traffic lights follow. The
    // channels are truncated to bytes rather than rounded: rounding here would
    // flip a theme sitting exactly on the boundary.
    Runtime::RuntimeSnapshot dark;
    dark.defaultBackgroundRed = 127.0F / 255.0F;
    dark.defaultBackgroundGreen = 127.0F / 255.0F;
    dark.defaultBackgroundBlue = 127.0F / 255.0F;
    CHECK_MSG(Runtime::chromeTint(dark).isDark, "lightness 127 reads dark");

    Runtime::RuntimeSnapshot light = dark;
    light.defaultBackgroundRed = 128.0F / 255.0F;
    light.defaultBackgroundGreen = 128.0F / 255.0F;
    light.defaultBackgroundBlue = 128.0F / 255.0F;
    CHECK_MSG(!Runtime::chromeTint(light).isDark, "and 128 reads light");

    Runtime::RuntimeSnapshot letterbox;
    letterbox.isEdgeToEdge = true;
    letterbox.letterboxColor = 0xFFFFFF;
    CHECK_MSG(!Runtime::chromeTint(letterbox).isDark, "a letterbox fill decides the polarity while it stands");
}

UBIQUITTY_TEST_MAIN()
