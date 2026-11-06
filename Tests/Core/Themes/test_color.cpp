// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host-side tests for Source/Core/CellColor.cpp — the portable palette resolver
// shared between the macOS PAL and a host PAL once one exists.
//
// Coverage focus:
//
//   - 256-color cube boundary samples. The 6×6×6 cube starts at index 16
//     and ends at index 231; the grayscale ramp follows in index 232..255.
//     We pin the math against the standard xterm formula so any silent
//     drift in apply256Cube() shows up here, not as a wrong-colored
//     screen on hardware.
//
//   - Attribute::Bold remap of indices 0..7 → 8..15. Bold on a non-low-index
//     color must not remap (we'd otherwise corrupt user-set 256-color
//     and truecolor cells).
//
//   - Attribute::Dim halves foreground brightness, and it must apply after reverse
//     so the dim still lands on the visible foreground.
//
// The resolver is pure C with no platform dependencies, so this links
// against just CellColor.cpp plus Base/Types.h.

#include "Core/CellColor.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#define CHECK_RGB(label, got, eR, eG, eB)                                                                           \
    do {                                                                                                            \
        if (got[0] != (eR) || got[1] != (eG) || got[2] != (eB)) {                                                   \
            printf("FAIL %s: got %d,%d,%d expected %d,%d,%d\n", (label), got[0], got[1], got[2], (eR), (eG), (eB)); \
            failures++;                                                                                             \
        }                                                                                                           \
    } while (0)

/// Build a palette with the standard ANSI 16 colors at distinctive,
/// easy-to-recognize values. The bright row (8..15) differs from the dim
/// row (0..7) by an offset so the bold-remap test can prove it actually
/// swapped lanes.
static void buildPalette(Ubiquitty::CellPalette* p)
{
    int i;
    *p = Ubiquitty::CellPalette {}; // value-init (CellPalette is non-trivial; memset is UB-adjacent)
    for (i = 0; i < 16; i++) {
        p->indexed[i].red = (uint8_t)(0x10 + i);
        p->indexed[i].green = (uint8_t)(0x20 + i);
        p->indexed[i].blue = (uint8_t)(0x30 + i);
    }
    p->defaultForeground.red = 0xDD;
    p->defaultForeground.green = 0xDD;
    p->defaultForeground.blue = 0xDD;
    p->defaultBackground.red = 0x11;
    p->defaultBackground.green = 0x11;
    p->defaultBackground.blue = 0x11;
    p->brightenBold = true;
    p->fillExtended();
}

static void cellWithForeground(Ubiquitty::Cell* c, uint16_t foregroundIndex)
{
    *c = {};
    c->pen.foregroundIndex = foregroundIndex;
    c->pen.backgroundIndex = Ubiquitty::colorDefault;
}

// ---------------- 6×6×6 cube boundary samples ----------------

/// xterm formula for cube channel: 0 if step==0, else 55 + step*40.
static uint8_t cubeChan(int step) { return step == 0 ? 0 : (uint8_t)(55 + step * 40); }

TEST(testCubeCorners)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    // index 16 — first cube entry, (0,0,0).
    cellWithForeground(&c, 16);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("cube[16]", foreground, cubeChan(0), cubeChan(0), cubeChan(0));

    // index 21 — (0,0,5), pure blue.
    cellWithForeground(&c, 21);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("cube[21]", foreground, cubeChan(0), cubeChan(0), cubeChan(5));

    // index 46 — (0,5,0), pure green.
    cellWithForeground(&c, 46);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("cube[46]", foreground, cubeChan(0), cubeChan(5), cubeChan(0));

    // index 196 — (5,0,0), pure red.
    cellWithForeground(&c, 196);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("cube[196]", foreground, cubeChan(5), cubeChan(0), cubeChan(0));

    // index 231 — (5,5,5), the white-ish cube corner.
    cellWithForeground(&c, 231);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("cube[231]", foreground, cubeChan(5), cubeChan(5), cubeChan(5));
}

TEST(testCubeGrayscaleRamp)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    // index 232 — first grayscale step, g = 8 + 0*10 = 8.
    cellWithForeground(&c, 232);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("gray[232]", foreground, 8, 8, 8);

    // index 255 — last grayscale step. Formula g = 8 + 23*10 = 238 (clamped).
    cellWithForeground(&c, 255);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("gray[255]", foreground, 238, 238, 238);

    // index 243 — middle gray, g = 8 + 11*10 = 118.
    cellWithForeground(&c, 243);
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("gray[243]", foreground, 118, 118, 118);
}

/// ---------------- bold remaps index 0..7 → index 8..15 ----------------

TEST(testBoldRemapLowIndices)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];
    int i;

    buildPalette(&pal);

    // For each of 0..7, bold should pull the foreground from ansi[i+8].
    for (i = 0; i < 8; i++) {
        cellWithForeground(&c, (uint16_t)i);
        c.pen.attributes = Ubiquitty::VT::Attribute::Bold;
        pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
        CHECK(foreground[0] == pal.indexed[i + 8].red);
        CHECK(foreground[1] == pal.indexed[i + 8].green);
        CHECK(foreground[2] == pal.indexed[i + 8].blue);
    }
}

/// With brightenBold off, bold keeps the original indexed color (the renderer
/// carries the heavier weight); black (0) stays black instead of fading to
/// the bright-black grey.
TEST(testBoldNoRemapWhenBrightenOff)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];
    int i;

    buildPalette(&pal);
    pal.brightenBold = false;

    for (i = 0; i < 8; i++) {
        cellWithForeground(&c, (uint16_t)i);
        c.pen.attributes = Ubiquitty::VT::Attribute::Bold;
        pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
        CHECK(foreground[0] == pal.indexed[i].red);
        CHECK(foreground[1] == pal.indexed[i].green);
        CHECK(foreground[2] == pal.indexed[i].blue);
    }
}

TEST(testBoldDoesNotRemapHighIndices)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    // index 9 — bold must not bump to index 17, which doesn't exist; it must
    // just stay on index 9.
    cellWithForeground(&c, 9);
    c.pen.attributes = Ubiquitty::VT::Attribute::Bold;
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("bold-on-9", foreground, pal.indexed[9].red, pal.indexed[9].green, pal.indexed[9].blue);

    // index 200 — a cube index. Bold must not touch the cube math.
    cellWithForeground(&c, 200);
    c.pen.attributes = Ubiquitty::VT::Attribute::Bold;
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    // index 200 = 16 + 184 → v=184, r6=v/36=5, g6=(v/6)%6=0, b6=v%6=4.
    CHECK_RGB("bold-on-cube", foreground, cubeChan(5), cubeChan(0), cubeChan(4));
}

/// ---------------- dim fades foreground toward the cell background ----------------

TEST(testDimFadesTowardBackground)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    // RGB foreground = 200,100,50 over the default background (0x11). Dim averages each
    // foreground channel with the background channel: (200+17)/2, (100+17)/2, (50+17)/2.
    c = {};
    c.pen.foregroundIndex = Ubiquitty::colorRGB;
    c.pen.backgroundIndex = Ubiquitty::colorDefault;
    c.pen.foregroundRed = 200;
    c.pen.foregroundGreen = 100;
    c.pen.foregroundBlue = 50;
    c.pen.attributes = Ubiquitty::VT::Attribute::Dim;
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("dim-truecolor", foreground, 108, 58, 33);
}

TEST(testDimAfterReverse)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    // DIM + REVERSE: reverse swaps foreground↔background, then dim averages the (now-background-
    // sourced) foreground with the (now-foreground-sourced) background. Defaults: foreground=0xDD,background=0x11.
    // After reverse foreground=0x11, background=0xDD; dim → (0x11+0xDD)/2 = 0x77.
    c = {};
    c.pen.foregroundIndex = Ubiquitty::colorDefault;
    c.pen.backgroundIndex = Ubiquitty::colorDefault;
    c.pen.attributes = { Ubiquitty::VT::Attribute::Dim, Ubiquitty::VT::Attribute::Reverse };
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("dim-after-reverse", foreground, 0x77, 0x77, 0x77);
    // And the background side of reverse should be the original defaultForeground.
    CHECK_RGB("reverse-bg", background, 0xDD, 0xDD, 0xDD);
}

/// ---------------- Attribute::Hidden collapses foreground onto background ----------------

TEST(testHiddenCollapsesForegroundOntoBackground)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    c = {};
    c.pen.foregroundIndex = Ubiquitty::colorRGB;
    c.pen.backgroundIndex = Ubiquitty::colorDefault;
    c.pen.foregroundRed = 200;
    c.pen.foregroundGreen = 100;
    c.pen.foregroundBlue = 50;
    c.pen.attributes = Ubiquitty::VT::Attribute::Hidden;
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    // Defaults set background to 0x11 across; foreground must collapse to that.
    CHECK_RGB("hidden-fg", foreground, 0x11, 0x11, 0x11);
    CHECK_RGB("hidden-bg-unchanged", background, 0x11, 0x11, 0x11);
}

/// Dim runs after hidden in the resolver: hidden collapses foreground onto background, then
/// dim averages foreground with background — and since they are already equal, the cell stays
/// invisible. Dimming hidden text must not make it reappear.
TEST(testHiddenThenDim)
{
    Ubiquitty::CellPalette pal;
    Ubiquitty::Cell c;
    uint8_t foreground[3], background[3];

    buildPalette(&pal);

    c = {};
    c.pen.foregroundIndex = Ubiquitty::colorDefault;
    c.pen.backgroundIndex = Ubiquitty::colorDefault;
    c.pen.attributes = { Ubiquitty::VT::Attribute::Hidden, Ubiquitty::VT::Attribute::Dim };
    pal.resolve(c.pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    CHECK_RGB("hidden+dim", foreground, 0x11, 0x11, 0x11);
    CHECK_RGB("hidden+dim-bg", background, 0x11, 0x11, 0x11);
}

/// CellPalette::seedDefaults (the 8-bit frontend palette) and the Core 16-bit
/// TermPalette::seedDefaults must seed the same compiled-in neutral-dark default
/// from the SINGLE source in Core/TermPalette.h, so the macOS buildDefaultPalette
/// and the headless TermPalette produce byte-identical color (after the trivial
/// 16->8-bit narrowing). This pins that single-source dedup.
TEST(testSeedDefaultsMatchTermPalette)
{
    Ubiquitty::CellPalette cell;
    cell = Ubiquitty::CellPalette {};
    cell.seedDefaults();

    Ubiquitty::TermPalette term;
    term.seedDefaults();

    // The 8-bit CellPalette matches the canonical ANSI-16 table verbatim.
    for (int i = 0; i < 16; ++i) {
        CHECK(cell.indexed[i].red == Ubiquitty::defaultAnsi16[i][0]);
        CHECK(cell.indexed[i].green == Ubiquitty::defaultAnsi16[i][1]);
        CHECK(cell.indexed[i].blue == Ubiquitty::defaultAnsi16[i][2]);
    }
    // A couple of representative ANSI slots, spelled out so a table typo is
    // obvious here, not on a hardware screen.
    CHECK_RGB("ansi-0 black", (&cell.indexed[0].red), 0, 0, 0);
    CHECK_RGB("ansi-1 red", (&cell.indexed[1].red), 205, 0, 0);
    CHECK_RGB("ansi-12 bright-blue", (&cell.indexed[12].red), 92, 92, 255);
    CHECK_RGB("ansi-15 white", (&cell.indexed[15].red), 255, 255, 255);

    // Default fore/back/cursor come from the shared triples.
    CHECK_RGB("default fg", (&cell.defaultForeground.red), 0xC0, 0xC0, 0xC0);
    CHECK_RGB("default bg", (&cell.defaultBackground.red), 0x10, 0x10, 0x14);
    CHECK_RGB("default cursor", (&cell.cursor.red), 0xC0, 0xC0, 0xC0);

    // And the whole 256-entry table + default slots agree with the Core 16-bit
    // TermPalette once narrowed back to 8-bit — the proof they share one seed.
    for (int i = 0; i < 256; ++i) {
        CHECK(cell.indexed[i].red == Ubiquitty::channel16To8(term.indexed[i].red));
        CHECK(cell.indexed[i].green == Ubiquitty::channel16To8(term.indexed[i].green));
        CHECK(cell.indexed[i].blue == Ubiquitty::channel16To8(term.indexed[i].blue));
    }
    CHECK(cell.defaultForeground.red == Ubiquitty::channel16To8(term.defaultForeground.red));
    CHECK(cell.defaultBackground.blue == Ubiquitty::channel16To8(term.defaultBackground.blue));
    CHECK(cell.cursor.green == Ubiquitty::channel16To8(term.cursor.green));
}

// ---------------- linkColor() derives a legible link color ----------------

/// WCAG contrast mirror, so the test asserts the floor independently.
static double linkLuma(Ubiquitty::ColorRGBA c)
{
    auto lin = [](uint8_t v) {
        double s = v / 255.0;
        return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red) + 0.7152 * lin(c.green) + 0.0722 * lin(c.blue);
}
static double linkContrast(Ubiquitty::ColorRGBA a, Ubiquitty::ColorRGBA b)
{
    double la = linkLuma(a), lb = linkLuma(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

/// Picks whichever palette blue reads better on the background. On a dark bg the
/// bright-blue (12) usually wins; the chosen color must be one of the two blues.
TEST(testLinkColorPicksBetterBlue)
{
    Ubiquitty::CellPalette pal {};
    pal.seedDefaults();
    // Dark default bg (seedDefaults' near-black). bright-blue (12) outcontrasts
    // the dim blue (4), so linkColor must return index 12.
    Ubiquitty::ColorRGBA link = pal.linkColor();
    CHECK_RGB("link-dark-picks-bright-blue", (&link.red), pal.indexed[12].red, pal.indexed[12].green, pal.indexed[12].blue);
    CHECK(linkContrast(link, pal.defaultBackground) >= 3.0);
}

/// When both palette blues are too low-contrast against the bg, linkColor blends
/// toward the (legible) foreground until it clears the floor — never returns an
/// illegible color.
TEST(testLinkColorBlendsWhenIllegible)
{
    Ubiquitty::CellPalette pal {};
    pal.seedDefaults();
    // Force a near-black bg and near-black blues so neither blue is legible.
    pal.defaultBackground = { 0, 0, 0 };
    pal.defaultForeground = { 230, 230, 230 };
    pal.indexed[4] = { 0, 0, 16 };
    pal.indexed[12] = { 0, 0, 24 };
    Ubiquitty::ColorRGBA link = pal.linkColor();
    CHECK(linkContrast(link, pal.defaultBackground) >= 3.0);
    // The blend keeps a blue bias: blue channel stays the largest.
    CHECK(link.blue >= link.red && link.blue >= link.green);
}

UBIQUITTY_TEST_MAIN()
