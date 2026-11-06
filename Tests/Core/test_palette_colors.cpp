// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

// test_palette_colors.cpp — a palette's colours as chrome and an embedding host
// read them (Core/Rendering/PaletteColors.h): each slot in its own place, at the
// one narrowing the chrome colours use, and following a program's OSC writes,
// the selection colours among them.
//
// Every colour is distinct, because the failure worth catching copies the
// right number of colours to the wrong places: an off-by-one over the ANSI
// slots, a foreground read into the cursor, one colour broadcast to all.

#include "Core/Rendering/PaletteColors.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdint>
#include <optional>

using namespace Ubiquitty;
using Rendering::ansiColorCount;
using Rendering::PaletteColors;
using Rendering::paletteColors;

namespace {

PaletteRGB wide(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    return { channel8To16(red), channel8To16(green), channel8To16(blue) };
}

bool is(const ColorRGBA& color, std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 0xFF)
{
    return color.red == red && color.green == green && color.blue == blue && color.alpha == alpha;
}

bool isSameColor(const ColorRGBA& left, const ColorRGBA& right)
{
    return left.red == right.red && left.green == right.green && left.blue == right.blue && left.alpha == right.alpha;
}

bool isSameOptionalColor(const std::optional<ColorRGBA>& left, const std::optional<ColorRGBA>& right)
{
    return left.has_value() == right.has_value() && (!left || isSameColor(*left, *right));
}

bool isSameColors(const PaletteColors& left, const PaletteColors& right)
{
    if (!isSameColor(left.foreground, right.foreground) || !isSameColor(left.background, right.background) || !isSameColor(left.cursor, right.cursor))
        return false;
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        if (!isSameColor(left.ansi[index], right.ansi[index]))
            return false;
    }
    return isSameOptionalColor(left.selectionBackground, right.selectionBackground) && isSameOptionalColor(left.selectionForeground, right.selectionForeground);
}

} // namespace

TEST(eachSlotLandsInItsOwnPlace)
{
    TermPalette palette;
    palette.seedDefaults();
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        const auto offset = static_cast<std::uint8_t>(index);
        palette.indexed[index] = wide(offset, static_cast<std::uint8_t>(0x40 + offset), static_cast<std::uint8_t>(0x80 + offset));
        palette.indexedAlpha[index] = static_cast<std::uint8_t>(0xE0 + offset);
    }
    palette.defaultForeground = wide(0xF1, 0xF2, 0xF3);
    palette.defaultBackground = wide(0xB1, 0xB2, 0xB3);
    palette.cursor = wide(0xC1, 0xC2, 0xC3);

    const PaletteColors colors = paletteColors(palette);
    CHECK(is(colors.foreground, 0xF1, 0xF2, 0xF3));
    CHECK(is(colors.background, 0xB1, 0xB2, 0xB3));
    CHECK(is(colors.cursor, 0xC1, 0xC2, 0xC3));
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        const auto offset = static_cast<std::uint8_t>(index);
        CHECK_MSG(is(colors.ansi[index], offset, static_cast<std::uint8_t>(0x40 + offset), static_cast<std::uint8_t>(0x80 + offset), static_cast<std::uint8_t>(0xE0 + offset)),
            "ANSI slot i holds slot i's colour at slot i's opacity");
    }
}

TEST(theNarrowingIsTheChromeColoursOwn)
{
    // Channels no theme can express, set the way OSC 10, 11 and 12 can: the
    // colours a host is handed and the ones chrome tints with are one reading.
    Terminal t;
    hardReset(t);
    t.palette.defaultForeground = { 0x1234, 0x5678, 0x9ABC };
    t.palette.defaultBackground = { 0x80FF, 0x01FE, 0xFFFE };
    t.palette.cursor = { 0x7F80, 0x0101, 0xFEFF };
    t.palette.indexed[4] = { 0x4321, 0x8765, 0xCBA9 };

    const PaletteColors colors = paletteColors(t.palette);
    CHECK(isSameColor(colors.foreground, chromeForegroundColor(&t)));
    CHECK(isSameColor(colors.background, chromeBackgroundColor(&t)));
    CHECK(isSameColor(colors.cursor, chromeCursorColor(&t)));
    CHECK_MSG(isSameColor(colors.ansi[4], chromeAccentColor(&t)), "the accent is ANSI blue, read the same way");
}

TEST(theDefaultsAreTheCompiledInPalettes)
{
    TermPalette seeded;
    seeded.seedDefaults();
    const PaletteColors colors = paletteColors(seeded);
    CHECK(is(colors.foreground, defaultForegroundRGB[0], defaultForegroundRGB[1], defaultForegroundRGB[2]));
    CHECK(is(colors.background, defaultBackgroundRGB[0], defaultBackgroundRGB[1], defaultBackgroundRGB[2]));
    CHECK(is(colors.cursor, defaultCursorRGB[0], defaultCursorRGB[1], defaultCursorRGB[2]));
    // The seed widens each byte into both halves of its channel, so the high
    // byte is the byte the compiled-in table holds.
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        const PaletteRGB& seed = seeded.indexed[index];
        CHECK(is(colors.ansi[index], static_cast<std::uint8_t>(seed.red >> 8), static_cast<std::uint8_t>(seed.green >> 8), static_cast<std::uint8_t>(seed.blue >> 8)));
    }

    Terminal t;
    hardReset(t);
    CHECK_MSG(isSameColors(paletteColors(t.palette), colors), "a terminal nothing has themed shows the compiled-in colours");
}

TEST(aProgramsColourWritesMoveTheColoursAndItsResetsRestoreThem)
{
    Terminal t;
    hardReset(t);
    const PaletteColors before = paletteColors(t.palette);

    UbiquittyTest::feed(t, "\033]10;rgb:ab/cd/ef\a\033]11;rgb:12/34/56\a\033]12;rgb:65/43/21\a\033]4;1;rgb:0a/0b/0c\a");
    const PaletteColors written = paletteColors(t.palette);
    CHECK(is(written.foreground, 0xAB, 0xCD, 0xEF));
    CHECK(is(written.background, 0x12, 0x34, 0x56));
    CHECK(is(written.cursor, 0x65, 0x43, 0x21));
    CHECK(is(written.ansi[1], 0x0A, 0x0B, 0x0C));
    CHECK_MSG(isSameColor(written.ansi[2], before.ansi[2]), "a write to one ANSI slot leaves its neighbour");

    UbiquittyTest::feed(t, "\033]110\a\033]111\a\033]112\a\033]104;1\a");
    CHECK_MSG(isSameColors(paletteColors(t.palette), before), "the resets land on the colours the writes replaced");
}

TEST(aTerminalsColoursCarryTheSelectionColoursAProgramSets)
{
    Terminal t;
    hardReset(t);
    const PaletteColors before = terminalColors(t);
    CHECK_MSG(isSameColors(before, paletteColors(t.palette)), "a terminal no program has written shows its palette's colours");
    CHECK_MSG(!before.selectionBackground && !before.selectionForeground, "no program named a selection colour");

    UbiquittyTest::feed(t, "\033]17;rgb:21/43/65\a\033]19;rgb:fe/dc/ba\a");
    const PaletteColors written = terminalColors(t);
    CHECK(written.selectionBackground && is(*written.selectionBackground, 0x21, 0x43, 0x65));
    CHECK(written.selectionForeground && is(*written.selectionForeground, 0xFE, 0xDC, 0xBA));
    CHECK_MSG(isSameColor(written.background, before.background), "a selection colour is its own slot, not the background");

    UbiquittyTest::feed(t, "\033]117\a\033]119\a");
    CHECK_MSG(isSameColors(terminalColors(t), before), "the resets take the selection colours back off");
}

TEST(eachSelectionColourMovesThePaletteEpochWhenItMovesAColour)
{
    Terminal t;
    hardReset(t);
    // One sequence per feed, so no neighbour's move can stand in for its own.
    // Programs re-assert their colours every frame, and a reset with nothing
    // set is the same no-op, so neither moves the epoch.
    struct Step {
        const char* bytes;
        bool shouldMoveEpoch;
        const char* what;
    };
    constexpr Step steps[] = {
        { "\033]17;rgb:21/43/65\a", true, "OSC 17 setting a selection background" },
        { "\033]17;rgb:21/43/65\a", false, "OSC 17 re-asserting it" },
        { "\033]17;rgb:21/43/66\a", true, "OSC 17 moving it" },
        { "\033]19;rgb:fe/dc/ba\a", true, "OSC 19 setting a selected text colour" },
        { "\033]19;rgb:fe/dc/ba\a", false, "OSC 19 re-asserting it" },
        { "\033]117\a", true, "OSC 117 clearing the background" },
        { "\033]117\a", false, "OSC 117 with nothing set" },
        { "\033]119\a", true, "OSC 119 clearing the text colour" },
        { "\033]119\a", false, "OSC 119 with nothing set" },
    };
    for (const Step& step : steps) {
        const std::uint64_t epoch = t.paletteEpoch;
        UbiquittyTest::feed(t, step.bytes);
        CHECK_MSG((t.paletteEpoch != epoch) == step.shouldMoveEpoch, step.what);
    }
}

UBIQUITTY_TEST_MAIN()
