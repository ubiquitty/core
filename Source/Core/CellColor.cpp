// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Single source of truth for (Cell + CellPalette) → (foreground RGB, background
// RGB). Color-index encoding:
// - colorDefault → palette.defaultForeground / defaultBackground
// - colorRGB     → cell.pen.foreground/background RGB (true-color SGR)
// - 0..15        → palette.ansi[index]
// - 16..231      → 6×6×6 cube (xterm 256-color)
// - 232..255     → 24-step grayscale ramp
// plus SGR attribute modifiers (bold remaps 0..7 → 8..15 when brightenBold is
// set, dim fades foreground toward background, reverse swaps, hidden collapses).
#include "Core/CellColor.h"
#include "Core/CellPen.h"
#include "Core/Rendering/LinkColor.h"
#include "Core/TermPalette.h"
#include <cmath>
#include <cstring>

namespace Ubiquitty {

static void apply256Cube(int index, uint8_t outputBuffer[3])
{
    if (index < 232) {
        int v = index - 16;
        int r6 = v / 36, g6 = (v / 6) % 6, b6 = v % 6;
        outputBuffer[0] = static_cast<uint8_t>(r6 ? 55 + r6 * 40 : 0);
        outputBuffer[1] = static_cast<uint8_t>(g6 ? 55 + g6 * 40 : 0);
        outputBuffer[2] = static_cast<uint8_t>(b6 ? 55 + b6 * 40 : 0);
    } else {
        int g = 8 + (index - 232) * 10;
        if (g > 238)
            g = 238;
        outputBuffer[0] = outputBuffer[1] = outputBuffer[2] = static_cast<uint8_t>(g);
    }
}

static void resolveSide(const CellPalette* pal, const Pen& pen, int wantBG, uint8_t outputBuffer[3])
{
    int index = wantBG ? pen.backgroundIndex : pen.foregroundIndex;
    if (index == Ubiquitty::colorDefault) {
        if (wantBG) {
            outputBuffer[0] = pal->defaultBackground.red;
            outputBuffer[1] = pal->defaultBackground.green;
            outputBuffer[2] = pal->defaultBackground.blue;
        } else {
            outputBuffer[0] = pal->defaultForeground.red;
            outputBuffer[1] = pal->defaultForeground.green;
            outputBuffer[2] = pal->defaultForeground.blue;
        }
        return;
    }
    if (index == Ubiquitty::colorRGB) {
        outputBuffer[0] = wantBG ? pen.backgroundRed : pen.foregroundRed;
        outputBuffer[1] = wantBG ? pen.backgroundGreen : pen.foregroundGreen;
        outputBuffer[2] = wantBG ? pen.backgroundBlue : pen.foregroundBlue;
        return;
    }
    if (index >= 0 && index < 256) {
        outputBuffer[0] = pal->indexed[index].red;
        outputBuffer[1] = pal->indexed[index].green;
        outputBuffer[2] = pal->indexed[index].blue;
        return;
    }
    // unknown — fall back to default
    if (wantBG) {
        outputBuffer[0] = pal->defaultBackground.red;
        outputBuffer[1] = pal->defaultBackground.green;
        outputBuffer[2] = pal->defaultBackground.blue;
    } else {
        outputBuffer[0] = pal->defaultForeground.red;
        outputBuffer[1] = pal->defaultForeground.green;
        outputBuffer[2] = pal->defaultForeground.blue;
    }
}

void CellPalette::resolve(const Pen& pen, ReverseMode reverse, uint8_t outForeground[3], uint8_t outBackground[3]) const
{
    uint8_t foreground[3], background[3];
    resolveSide(this, pen, 0, foreground);
    resolveSide(this, pen, 1, background);

    // Bold remaps standard colors 0..7 to their bright 8..15 counterparts.
    int foregroundIndex = pen.foregroundIndex;
    if (brightenBold && pen.attributes.contains(Ubiquitty::VT::Attribute::Bold) && foregroundIndex >= 0 && foregroundIndex < 8) {
        foreground[0] = indexed[foregroundIndex + 8].red;
        foreground[1] = indexed[foregroundIndex + 8].green;
        foreground[2] = indexed[foregroundIndex + 8].blue;
    }
    int reversed = pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse) ? 1 : 0;
    if (reverse == ReverseMode::Reversed)
        reversed = !reversed;
    if (reversed) {
        uint8_t t[3] = { foreground[0], foreground[1], foreground[2] };
        foreground[0] = background[0];
        foreground[1] = background[1];
        foreground[2] = background[2];
        background[0] = t[0];
        background[1] = t[1];
        background[2] = t[2];
    }
    // Hidden collapses foreground onto background.
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)) {
        foreground[0] = background[0];
        foreground[1] = background[1];
        foreground[2] = background[2];
    }
    // Dim fades foreground halfway toward background, applied last so it survives reverse.
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Dim)) {
        foreground[0] = static_cast<uint8_t>((foreground[0] + background[0]) / 2);
        foreground[1] = static_cast<uint8_t>((foreground[1] + background[1]) / 2);
        foreground[2] = static_cast<uint8_t>((foreground[2] + background[2]) / 2);
    }
    std::memcpy(outForeground, foreground, 3);
    std::memcpy(outBackground, background, 3);
}

ColorRGBA CellPalette::linkColor() const
{
    return Rendering::resolveLinkColor(indexed[4], indexed[12], defaultBackground, defaultForeground);
}

void CellPalette::fillExtended()
{
    for (int i = 16; i < 256; ++i) {
        uint8_t rgb[3];
        apply256Cube(i, rgb);
        indexed[i].red = rgb[0];
        indexed[i].green = rgb[1];
        indexed[i].blue = rgb[2];
    }
}

CellPalette CellPalette::fromTerminalPalette(const TermPalette& palette)
{
    CellPalette out;
    const auto down = [](const PaletteRGB& source) -> ColorRGBA { return { channel16To8(source.red), channel16To8(source.green), channel16To8(source.blue) }; };
    for (int i = 0; i < 256; ++i)
        out.indexed[i] = down(palette.indexed[i]);
    out.defaultForeground = down(palette.defaultForeground);
    out.defaultBackground = down(palette.defaultBackground);
    out.cursor = down(palette.cursor);
    return out;
}

void CellPalette::seedDefaults()
{
    for (int i = 0; i < 16; ++i) {
        indexed[i].red = Ubiquitty::defaultAnsi16[i][0];
        indexed[i].green = Ubiquitty::defaultAnsi16[i][1];
        indexed[i].blue = Ubiquitty::defaultAnsi16[i][2];
    }
    fillExtended();
    defaultForeground = { Ubiquitty::defaultForegroundRGB[0], Ubiquitty::defaultForegroundRGB[1], Ubiquitty::defaultForegroundRGB[2] };
    defaultBackground = { Ubiquitty::defaultBackgroundRGB[0], Ubiquitty::defaultBackgroundRGB[1], Ubiquitty::defaultBackgroundRGB[2] };
    cursor = { Ubiquitty::defaultCursorRGB[0], Ubiquitty::defaultCursorRGB[1], Ubiquitty::defaultCursorRGB[2] };
}

} // namespace Ubiquitty
