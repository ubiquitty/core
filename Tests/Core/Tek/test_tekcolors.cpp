// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tekcolors.cpp — host tests for storage-tube colour precedence
// (Source/Core/Tek/Colors.h).
//
// Pins the order between the three sources that can name the same colour —
// the host's OSC 15/16/18, the user's Custom-mode pair, and the theme — plus
// the cursor's fallback to the beam and the widening factor. Every case sets
// the LOWER tiers to a colour it would be wrong to draw, so a resolver that
// consulted them in the wrong order fails rather than coincidentally agreeing.

#include "Core/Tek/Colors.h"

using namespace Ubiquitty;
using namespace Ubiquitty::Tek;

static bool same(TekColor a, TekColor b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }

/// A preference whose custom pair is distinct from every theme colour used
/// below, so a resolver that returned it out of turn cannot pass by accident.
static ColorPreference customPreference()
{
    ColorPreference preference;
    preference.mode = ColorMode::Custom;
    preference.beam = { 0x3333, 0xFFFF, 0x6666 };
    preference.background = { 0x0A0A, 0x0A0A, 0x0A0A };
    return preference;
}

/// ---- The host's request outranks both the user's and the theme's. -------
TEST(testOSCWinsOverCustomAndTheme)
{
    Terminal terminal;
    terminal.tektronixBackgroundSet = true;
    terminal.tektronixBackgroundRed = 0x1234;
    terminal.tektronixBackgroundGreen = 0x5678;
    terminal.tektronixBackgroundBlue = 0x9ABC;
    terminal.tektronixForegroundSet = true;
    terminal.tektronixForegroundRed = 0xDEF0;
    terminal.tektronixForegroundGreen = 0x1357;
    terminal.tektronixForegroundBlue = 0x2468;

    const ColorPreference preference = customPreference();
    const ColorRGBA themeBackground { 0x11, 0x22, 0x33 };
    const ColorRGBA themeForeground { 0x44, 0x55, 0x66 };

    CHECK(same(tekBackground(terminal, themeBackground, preference), { 0x1234, 0x5678, 0x9ABC }));
    CHECK(same(tekBeam(terminal, themeForeground, preference), { 0xDEF0, 0x1357, 0x2468 }));
}

/// ---- Absent a host request, the user's Custom pair outranks the theme. --
TEST(testCustomWinsOverTheme)
{
    Terminal terminal; // no OSC set
    const ColorPreference preference = customPreference();
    const ColorRGBA themeBackground { 0x11, 0x22, 0x33 };
    const ColorRGBA themeForeground { 0x44, 0x55, 0x66 };

    CHECK(same(tekBackground(terminal, themeBackground, preference), { 0x0A0A, 0x0A0A, 0x0A0A }));
    CHECK(same(tekBeam(terminal, themeForeground, preference), { 0x3333, 0xFFFF, 0x6666 }));
}

/// ---- Theme mode ignores the custom pair even when it carries colours. ---
TEST(testThemeModeIgnoresCustomColors)
{
    Terminal terminal;
    ColorPreference preference = customPreference();
    preference.mode = ColorMode::Theme; // the pair stays populated on purpose

    CHECK(same(tekBackground(terminal, ColorRGBA { 0x11, 0x22, 0x33 }, preference), { 0x1111, 0x2222, 0x3333 }));
    CHECK(same(tekBeam(terminal, ColorRGBA { 0x44, 0x55, 0x66 }, preference), { 0x4444, 0x5555, 0x6666 }));
}

/// ---- Each colour reads its OWN OSC, so one set does not move another. ---
TEST(testEachColorReadsItsOwnRequest)
{
    Terminal terminal;
    terminal.tektronixForegroundSet = true;
    terminal.tektronixForegroundRed = 0xAAAA;
    terminal.tektronixForegroundGreen = 0xBBBB;
    terminal.tektronixForegroundBlue = 0xCCCC;

    ColorPreference preference;
    preference.mode = ColorMode::Theme;

    // The beam took the host's colour; the background must still be the theme's.
    CHECK(same(tekBeam(terminal, ColorRGBA { 0x44, 0x55, 0x66 }, preference), { 0xAAAA, 0xBBBB, 0xCCCC }));
    CHECK(same(tekBackground(terminal, ColorRGBA { 0x11, 0x22, 0x33 }, preference), { 0x1111, 0x2222, 0x3333 }));
}

/// ---- The cursor follows the beam unless OSC 18 separates them. ----------
TEST(testCursorFollowsBeamUntilRequested)
{
    Terminal terminal;
    const TekColor beam { 0x3333, 0xFFFF, 0x6666 };
    CHECK(same(tekCursor(terminal, beam), beam));

    terminal.tektronixCursorSet = true;
    terminal.tektronixCursorRed = 0x0F0F;
    terminal.tektronixCursorGreen = 0x1E1E;
    terminal.tektronixCursorBlue = 0x2D2D;
    CHECK(same(tekCursor(terminal, beam), { 0x0F0F, 0x1E1E, 0x2D2D }));
}

/// ---- Widening is exact at both ends, which is what 257 buys over 256. ---
TEST(testWideningRoundTripsTheEndpoints)
{
    CHECK(widenChannel(0) == 0);
    CHECK(widenChannel(255) == 65535);
    CHECK(widenChannel(128) == 32896);

    // A theme colour widened here and scaled back by a renderer's 1/65535 is
    // the channel it started as; a shift by eight would land 255 at 65280.
    CHECK(widenChannel(255) != static_cast<std::uint16_t>(255 << 8));
    const TekColor white = widenColor(ColorRGBA { 255, 255, 255 });
    CHECK(same(white, { 65535, 65535, 65535 }));
}

/// ---- The writing spot: the host's request outranks the setting. --------
///
/// Each case sets the setting to the opposite of what it expects, so a
/// resolver that read the setting first would return the other answer rather
/// than the same one.
TEST(testWritingSpotOverrideOutranksTheSetting)
{
    Terminal terminal;

    // 1 forces it on, against a setting that says off.
    terminal.tekBrightPenOverride = 1;
    CHECK(isWritingSpotDrawn(terminal, WritingSpotSetting::Suppressed));

    // 2 forces it off, against a setting that says on.
    terminal.tekBrightPenOverride = 2;
    CHECK(!isWritingSpotDrawn(terminal, WritingSpotSetting::Drawn));
}

/// ---- Absent a request, the setting decides, both ways. -----------------
TEST(testWritingSpotDefersToTheSetting)
{
    Terminal terminal; // override 0: the host has not asked
    CHECK(isWritingSpotDrawn(terminal, WritingSpotSetting::Drawn));
    CHECK(!isWritingSpotDrawn(terminal, WritingSpotSetting::Suppressed));
}

/// ---- A value the protocol never sets defers rather than forcing. -------
///
/// Only 0, 1 and 2 are written today (`VT/Modes.cpp` on DECSET/DECRST 7400,
/// and the resets that clear it). Pinned because a resolver written as
/// "non-zero means on" passes every case above and turns any future third
/// state into a forced spot.
TEST(testWritingSpotTreatsUnknownStatesAsNoRequest)
{
    Terminal terminal;
    terminal.tekBrightPenOverride = 7;
    CHECK(!isWritingSpotDrawn(terminal, WritingSpotSetting::Suppressed));
    CHECK(isWritingSpotDrawn(terminal, WritingSpotSetting::Drawn));
}

UBIQUITTY_TEST_MAIN()
