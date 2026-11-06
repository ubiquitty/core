// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/PaletteRGB.h"

#include <array>

// The terminal's live color palette, owned by Core. 16-bit channels match
// the OSC wire precision; frontends scale to 8-bit when they sync their
// paint cache.

namespace Ubiquitty {

// The standard xterm 16-color block (8-bit channels), the single source of
// truth for indices 0-15 of the compiled-in default palette. Both the Core
// 16-bit TermPalette and the frontends' 8-bit CellPalette seed indices 0-15
// from this table so headless output and every frontend's neutral-dark default
// match byte for byte. (Indices 16-255 are the xterm cube + grayscale ramp,
// filled procedurally by fillExtended.)
inline constexpr uint8_t defaultAnsi16[16][3] = {
    { 0, 0, 0 },
    { 205, 0, 0 },
    { 0, 205, 0 },
    { 205, 205, 0 },
    { 0, 0, 238 },
    { 205, 0, 205 },
    { 0, 205, 205 },
    { 229, 229, 229 },
    { 127, 127, 127 },
    { 255, 0, 0 },
    { 0, 255, 0 },
    { 255, 255, 0 },
    { 92, 92, 255 },
    { 255, 0, 255 },
    { 0, 255, 255 },
    { 255, 255, 255 },
};

// The neutral-dark default foreground / background / cursor (8-bit channels),
// shared by the same two palettes. A light-gray foreground + cursor on a
// near-black background.
inline constexpr uint8_t defaultForegroundRGB[3] = { 0xC0, 0xC0, 0xC0 };
inline constexpr uint8_t defaultBackgroundRGB[3] = { 0x10, 0x10, 0x14 };
inline constexpr uint8_t defaultCursorRGB[3] = { 0xC0, 0xC0, 0xC0 };

// xterm OSC 5 special-color slots, indexed by Pc: 0=bold, 1=underline,
// 2=blink, 3=reverse, 4=italic. Each maps to ANSI index 256+Pc in OSC 4.
enum SpecialColor : int {
    specialBold = 0,
    specialUnderline = 1,
    specialBlink = 2,
    specialReverse = 3,
    specialItalic = 4,
    specialCount = 5,
};

/// Every indexed slot opaque: the alpha a palette starts with, and the one a
/// slot returns to when a program writes its own colour into it.
[[nodiscard]] constexpr std::array<uint8_t, 256> opaqueIndexedAlpha()
{
    std::array<uint8_t, 256> alpha {};
    alpha.fill(255);
    return alpha;
}

struct TermPalette {
    bool operator==(const TermPalette&) const = default;

    // 0-15 ANSI base; 16-255 xterm cube + grayscale; all OSC-4 overridable.
    PaletteRGB indexed[256];
    PaletteRGB defaultForeground;
    PaletteRGB defaultBackground;
    PaletteRGB cursor;

    // Per-index foreground opacity, 0-255, kept beside the colour it belongs
    // to so the one palette a frame is captured from answers both. A theme
    // carries it (an ANSI slot dimmed by alpha, `#rrggbbaa`); an OSC 4 write
    // makes the slot it writes opaque, since the program's colour has no
    // alpha and a table that kept the theme's would dim a colour the program
    // chose; OSC 104 restores the baseline's along with its colour. Not on
    // the wire: the sync protocol carries the three channels, so a thin
    // client draws every slot opaque.
    std::array<uint8_t, 256> indexedAlpha = opaqueIndexedAlpha();

    // OSC 5 special colors (per-attribute overrides for bold/underline/etc.).
    // Default-unset: a slot reads back as the default foreground until an app
    // assigns it, and OSC 105 clears the assignment. Stored here so headless
    // Core round-trips the value; render-consumption is a separate frontend step.
    PaletteRGB special[specialCount];
    bool specialSet[specialCount];

    // Seed the standard xterm 16-color block, the 6x6x6 cube + grayscale
    // ramp (indices 16-255), and a neutral dark default foreground/background/cursor. The
    // frontend overwrites these on theme apply; the values match the
    // frontends' compiled-in defaults so headless output is identical.
    void seedDefaults();

    // Fill indices 16-255 with the xterm cube + grayscale ramp, leaving
    // 0-15 and the default/cursor slots untouched.
    void fillExtended();
};

// 8-bit channel promoted to 16-bit by replication (0xFF -> 0xFFFF).
[[nodiscard]] inline uint16_t channel8To16(uint8_t v) { return static_cast<uint16_t>((static_cast<unsigned>(v) << 8) | v); }

// 16-bit channel rounded down to 8-bit (0xFFFF -> 0xFF).
[[nodiscard]] inline uint8_t channel16To8(uint16_t v) { return static_cast<uint8_t>((static_cast<unsigned>(v) * 255u + 32768u) / 65535u); }

inline void TermPalette::fillExtended()
{
    for (int index = 16; index < 256; ++index) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (index < 232) {
            int v = index - 16;
            int r6 = v / 36, g6 = (v / 6) % 6, b6 = v % 6;
            r = static_cast<uint8_t>(r6 ? 55 + r6 * 40 : 0);
            g = static_cast<uint8_t>(g6 ? 55 + g6 * 40 : 0);
            b = static_cast<uint8_t>(b6 ? 55 + b6 * 40 : 0);
        } else {
            int gray = 8 + (index - 232) * 10;
            if (gray > 238)
                gray = 238;
            r = g = b = static_cast<uint8_t>(gray);
        }
        indexed[index].red = channel8To16(r);
        indexed[index].green = channel8To16(g);
        indexed[index].blue = channel8To16(b);
    }
}

inline void TermPalette::seedDefaults()
{
    for (int i = 0; i < 16; ++i) {
        indexed[i].red = channel8To16(defaultAnsi16[i][0]);
        indexed[i].green = channel8To16(defaultAnsi16[i][1]);
        indexed[i].blue = channel8To16(defaultAnsi16[i][2]);
    }
    fillExtended();
    indexedAlpha = opaqueIndexedAlpha();
    defaultForeground = { channel8To16(defaultForegroundRGB[0]), channel8To16(defaultForegroundRGB[1]), channel8To16(defaultForegroundRGB[2]) };
    defaultBackground = { channel8To16(defaultBackgroundRGB[0]), channel8To16(defaultBackgroundRGB[1]), channel8To16(defaultBackgroundRGB[2]) };
    cursor = { channel8To16(defaultCursorRGB[0]), channel8To16(defaultCursorRGB[1]), channel8To16(defaultCursorRGB[2]) };
    for (int i = 0; i < specialCount; ++i) {
        special[i] = defaultForeground;
        specialSet[i] = false;
    }
}

} // namespace Ubiquitty
