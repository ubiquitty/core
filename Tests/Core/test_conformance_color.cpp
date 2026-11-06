// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

// test_conformance_color — the X11 color-specification forms accepted by the OSC color controls:
// OSC 4 (indexed), OSC 5 (special), OSC 10-19 (dynamic) and the OSC 110-119 resets.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
//
// The reference values are not a modern sRGB pipeline. The device-independent spaces resolve
// through Xlib's default Xcms screen characterization, and those tables — not any color science
// chosen here — fix every answer below. Source/Core/OSC/XParseColor.h documents the chain.

#include "Core/OSC/Dispatch.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

int runColorConformanceTests();

// Drive one spec through OSC 4 onto palette index 1 and assert the stored 16-bit channels.
static void checkSpec(const char* spec, uint16_t red, uint16_t green, uint16_t blue)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    char sequence[128];
    std::snprintf(sequence, sizeof(sequence), "\033]4;1;%s\033\\", spec);
    feed(t, sequence);

    const Ubiquitty::PaletteRGB& slot = t.palette.indexed[1];
    if (slot.red != red || slot.green != green || slot.blue != blue) {
        printf("FAIL %s:%d: %s -> %04x/%04x/%04x, want %04x/%04x/%04x\n", __FILE__, __LINE__, spec, slot.red, slot.green, slot.blue, red, green, blue);
        failures++;
    }
}

// A spec Xlib rejects must leave the slot exactly as it was.
static void checkRejected(const char* spec)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;rgb:1234/5678/9abc\033\\");
    const Ubiquitty::PaletteRGB before = t.palette.indexed[1];
    char sequence[128];
    std::snprintf(sequence, sizeof(sequence), "\033]4;1;%s\033\\", spec);
    feed(t, sequence);

    const Ubiquitty::PaletteRGB& slot = t.palette.indexed[1];
    if (slot.red != before.red || slot.green != before.green || slot.blue != before.blue) {
        printf("FAIL %s:%d: %s was accepted (slot now %04x/%04x/%04x)\n", __FILE__, __LINE__, spec, slot.red, slot.green, slot.blue);
        failures++;
    }
}

// The "#" form is left-justified into the channel and then quantized by the 8-bit visual, so the
// same nominal color written at 3, 6, 9 or 12 digits lands on one value. Zero-padding (#fff ->
// ffff) or naive scaling would each give a different answer per width.
static void testHashFormsAreLeftJustified()
{
    checkSpec("#fff", 0xF0F0, 0xF0F0, 0xF0F0);
    checkSpec("#888", 0x8080, 0x8080, 0x8080);
    checkSpec("#f0f0f0", 0xF0F0, 0xF0F0, 0xF0F0);
    checkSpec("#808080", 0x8080, 0x8080, 0x8080);
    checkSpec("#f00f00f00", 0xF0F0, 0xF0F0, 0xF0F0);
    checkSpec("#800800800", 0x8080, 0x8080, 0x8080);
    checkSpec("#f000f000f000", 0xF0F0, 0xF0F0, 0xF0F0);
    checkSpec("#800080008000", 0x8080, 0x8080, 0x8080);

    checkSpec("#f0a", 0xF0F0, 0x0000, 0xA0A0);
    checkSpec("#102030", 0x1010, 0x2020, 0x3030);
    checkSpec("#aaaabbbbcccc", 0xAAAA, 0xBBBB, 0xCCCC);
    checkSpec("#ddddeeeeffff", 0xDDDD, 0xEEEE, 0xFFFF);
}

// "rgb:" scales rather than shifts: v * 0xFFFF / (16^digits - 1), so rgb:f/0/0 is full red.
// "rgb:" scales each channel to 16 bits and the 8-bit visual then keeps the top byte, as xterm's
// AllocOneColor and QueryOneColor do: a client reads back the colour the display shows, so a
// 16-bit channel loses its low byte and a scaled 3-digit one its rounding.
static void testRGBFormScales()
{
    checkSpec("rgb:f0f0/f0f0/f0f0", 0xF0F0, 0xF0F0, 0xF0F0);
    checkSpec("rgb:8080/0000/0000", 0x8080, 0x0000, 0x0000);
    checkSpec("rgb:1234/5678/9abc", 0x1212, 0x5656, 0x9A9A);
    checkSpec("rgb:f/0/8", 0xFFFF, 0x0000, 0x8888);
    checkSpec("rgb:1/2/3", 0x1111, 0x2222, 0x3333);
    checkSpec("rgb:ff/00/80", 0xFFFF, 0x0000, 0x8080);
    checkSpec("rgb:f00/000/800", 0xF0F0, 0x0000, 0x8080);
    checkSpec("rgb:f/80/abc", 0xFFFF, 0x8080, 0xABAB);
}

// "rgbi:" carries floating-point intensities driven straight through the display's measured
// transfer tables — 0.5 intensity is nowhere near half-scale on the wire.
static void testRgbiForm()
{
    checkSpec("rgbi:1/1/1", 0xFFFF, 0xFFFF, 0xFFFF);
    checkSpec("rgbi:1.0/1.0/1.0", 0xFFFF, 0xFFFF, 0xFFFF);
    checkSpec("rgbi:0.5/0.5/0.5", 0xC1C1, 0xBBBB, 0xBBBB);
    checkSpec("rgbi:0/0/0", 0x0000, 0x0000, 0x0000);
}

// The device-independent spaces, each at an in-gamut and an out-of-gamut sample. The "1/1/1" rows
// all clip: their luminance is the white point, where the display has no chroma left to give.
static void testCieAndTekhvcSpaces()
{
    checkSpec("CIEXYZ:1/1/1", 0xFFFF, 0xFFFF, 0xFFFF);
    checkSpec("CIEXYZ:0.5/0.5/0.5", 0xDDDD, 0xB5B5, 0xA0A0);

    checkSpec("CIEuvY:1/1/1", 0xFFFF, 0xFFFF, 0xFFFF);
    checkSpec("CIEuvY:0.5/0.5/0.5", 0xFFFF, 0xA3A3, 0xAEAE);

    checkSpec("CIExyY:1/1/1", 0xFFFF, 0xFFFF, 0xFFFF);
    checkSpec("CIExyY:0.5/0.5/0.5", 0xF7F7, 0xB3B3, 0x0E0E);

    checkSpec("CIELab:1/1/1", 0x6C6C, 0x6767, 0x6767);
    checkSpec("CIELab:0.5/0.5/0.5", 0x5252, 0x4F4F, 0x4F4F);

    // CIELuv is not CIELab: the same numbers name a far darker color.
    checkSpec("CIELuv:1/1/1", 0x1616, 0x1414, 0x0E0E);
    checkSpec("CIELuv:0.5/0.5/0.5", 0x0E0E, 0x1313, 0x0E0E);

    checkSpec("TekHVC:1/1/1", 0x1A1A, 0x1313, 0x0F0F);
    checkSpec("TekHVC:0.5/0.5/0.5", 0x1111, 0x1313, 0x0E0E);

    // Xlib lower-cases the spec before dispatching, so the space name is case-insensitive.
    checkSpec("cielab:1/1/1", 0x6C6C, 0x6767, 0x6767);
    checkSpec("TEKHVC:1/1/1", 0x1A1A, 0x1313, 0x0F0F);
}

static void testMalformedSpecsLeaveTheSlot()
{
    checkRejected("#XYZ");
    checkRejected("#GGGGGG");
    // A hex run whose length is not 3, 6, 9 or 12 has no channel split.
    checkRejected("#10203040");
    checkRejected("#ff00");
    checkRejected("rgb:");
    checkRejected("rgb:f");
    checkRejected("rgb:f/0");
    checkRejected("rgb:fffff/0/0"); // more than 4 digits per channel
    checkRejected("rgbi:1/1");
    checkRejected("notacolour"); // neither a colour space nor a name in the colour database
    checkRejected("CIELab:101/0/0"); // L* runs 0..100
    checkRejected("CIEXYZ:1/2/1"); // Y runs 0..1
    checkRejected("CIExyY:2/0.5/0.5"); // x runs 0..1
    checkRejected("CIEuvY:0.2/0.3/2"); // Y runs 0..1
}

// hsl: is a Ubiquitty extension outside the X11 spec; folding the X11 forms into a shared parser
// must not drop it.
static void testHslExtensionStillParses()
{
    checkSpec("hsl:0/100/50", 0xFFFF, 0x0000, 0x0000);
    checkSpec("hsl:120/100/50", 0x0000, 0xFFFF, 0x0000);
}

// The reply a client reads back carries the stored slot verbatim, so a CIE spec answers in rgb:.
static void testQueryReportsTheConvertedValue()
{
    Ubiquitty::Terminal t;
    char buffer[256];
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;0;CIELab:1/1/1\033\\");
    feed(t, "\033]4;0;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]4;0;rgb:6c6c/6767/6767\033\\") != nullptr);

    // OSC 5 special colors and their OSC 4 aliases (256+Pc) report the same slot.
    feed(t, "\033]5;0;#aaaabbbbcccc\033\\");
    feed(t, "\033]5;0;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]5;0;rgb:aaaa/bbbb/cccc\033\\") != nullptr);

    feed(t, "\033]4;256;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]4;256;rgb:aaaa/bbbb/cccc\033\\") != nullptr);
}

// One dynamic-color control can carry several specs; each applies to the next code in the
// sequence (10 foreground, 11 background, 12 cursor) and each '?' answers under its own code.
static void testDynamicColorsChain()
{
    Ubiquitty::Terminal t;
    char buffer[256];
    Ubiquitty::hardReset(t);

    feed(t, "\033]10;rgb:f0f0/f0f0/f0f0;rgb:f0f0/0000/0000\033\\");
    CHECK(t.palette.defaultForeground.red == 0xF0F0);
    CHECK(t.palette.defaultForeground.green == 0xF0F0);
    CHECK(t.palette.defaultBackground.red == 0xF0F0);
    CHECK(t.palette.defaultBackground.green == 0x0000);

    feed(t, "\033]10;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]10;rgb:f0f0/f0f0/f0f0\033\\") != nullptr);
    CHECK(std::strstr(buffer, "\033]11;rgb:f0f0/0000/0000\033\\") != nullptr);

    // A third field reaches the cursor color and a fourth the mouse pointer foreground.
    feed(t, "\033]10;#111111;#222222;#333333;#444444\033\\");
    CHECK(t.palette.defaultForeground.red == 0x1111);
    CHECK(t.palette.defaultBackground.red == 0x2222);
    CHECK(t.palette.cursor.red == 0x3333);
    CHECK(t.mousePointerForegroundSet);
    CHECK(t.mousePointerForegroundRed == 0x4444);

    // A single spec, with nothing chained after it, sets its one colour.
    feed(t, "\033]11;#445566\033\\");
    CHECK(t.palette.defaultBackground.red == 0x4444);
    CHECK(t.palette.defaultBackground.green == 0x5555);
    CHECK(t.palette.defaultBackground.blue == 0x6666);
    CHECK(t.palette.cursor.red == 0x3333); // unchanged: no second field
}

// OSC 110/111/112 revert to the startup baseline, not to whatever the color happened to be.
static void testDynamicColorResetRestoresTheBaseline()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    const Ubiquitty::PaletteRGB baseline = t.paletteBaseline.defaultForeground;
    feed(t, "\033]10;#aaaabbbbcccc\033\\");
    CHECK(t.palette.defaultForeground.red == 0xAAAA);
    feed(t, "\033]110\033\\");
    CHECK(t.palette.defaultForeground.red == baseline.red);
    CHECK(t.palette.defaultForeground.green == baseline.green);
    CHECK(t.palette.defaultForeground.blue == baseline.blue);
}

static bool sameColor(const Ubiquitty::PaletteRGB& a, const Ubiquitty::PaletteRGB& b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// The list runs to 19: a chain begun at 12 walks the cursor, pointer, Tektronix and selection colors
// in code order, and a ninth field has no code left to name — the list does not wrap back to 10.
static void testDynamicColorsChainThroughNineteen()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const Ubiquitty::PaletteRGB foreground = t.palette.defaultForeground;

    feed(t, "\033]12;#121212;#131313;#141414;#151515;#161616;#171717;#181818;#191919;#202020\033\\");
    CHECK(t.palette.cursor.red == 0x1212);
    CHECK(t.mousePointerForegroundSet && t.mousePointerForegroundRed == 0x1313);
    CHECK(t.mousePointerBackgroundSet && t.mousePointerBackgroundRed == 0x1414);
    CHECK(t.tektronixForegroundSet && t.tektronixForegroundRed == 0x1515);
    CHECK(t.tektronixBackgroundSet && t.tektronixBackgroundRed == 0x1616);
    CHECK(t.selectionBackgroundSet && t.selectionBackgroundRed == 0x1717);
    CHECK(t.tektronixCursorSet && t.tektronixCursorRed == 0x1818);
    CHECK(t.selectionForegroundSet && t.selectionForegroundRed == 0x1919);
    CHECK(sameColor(t.palette.defaultForeground, foreground));
}

// Each '?' in a chain answers under its own code in a reply of its own, as xterm sends them; a '?'
// past 19 names no color and draws nothing.
static void testDynamicColorQueriesChainPastTwelve()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);

    feed(t, "\033]16;#161616;#171717;#181818;#191919\033\\");
    feed(t, "\033]16;?;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]16;rgb:1616/1616/1616\033\\\033]17;rgb:1717/1717/1717\033\\\033]18;rgb:1818/1818/1818\033\\") == 0);

    feed(t, "\033]19;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]19;rgb:1919/1919/1919\033\\") == 0);
}

// An empty or unparsable field leaves its color alone and still hands the next field to the next code.
static void testEmptyAndMalformedFieldsAdvanceTheChain()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const Ubiquitty::PaletteRGB foreground = t.palette.defaultForeground;
    const Ubiquitty::PaletteRGB background = t.palette.defaultBackground;

    feed(t, "\033]10;;nope;#333333;;#555555\033\\");
    CHECK(sameColor(t.palette.defaultForeground, foreground));
    CHECK(sameColor(t.palette.defaultBackground, background));
    CHECK(t.palette.cursor.red == 0x3333);
    CHECK(!t.mousePointerForegroundSet);
    CHECK(t.mousePointerBackgroundSet && t.mousePointerBackgroundRed == 0x5555);
}

// OSC 4 answers every '?' pair with a reply of its own. An index past the special colors is skipped
// and the pairs after it still apply; an index that is not a number ends the control.
// The colour numbers are read as xterm reads them: one past the special colours ends the list, and
// a field that is not a number is colour 0.
static void testIndexedColorNumbersReadAsXtermReadsThem()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;#111111;2;#222222\033\\");
    feed(t, "\033]4;1;?;999;?;2;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]4;1;rgb:1111/1111/1111\033\\") == 0);

    feed(t, "\033]4;261;#abcdef;3;#333333\033\\");
    CHECK(sameColor(t.palette.indexed[3], t.paletteBaseline.indexed[3]));

    feed(t, "\033]4;0;#050505\033\\\033]4;1;?;x;?;2;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]4;1;rgb:1111/1111/1111\033\\\033]4;0;rgb:0505/0505/0505\033\\\033]4;2;rgb:2222/2222/2222\033\\") == 0);
}

// OSC 104 takes several indices; one out of range is skipped, and one that is not a number ends the list.
// Each color is set by a control of its own, no longer than the reset, so the reset finds nothing of
// theirs after its end.
static void testPaletteResetTakesSeveralIndices()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;#111111\033\\\033]4;2;#222222\033\\\033]4;3;#333333\033\\");
    feed(t, "\033]104;1;999;3\033\\");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(t.palette.indexed[2].red == 0x2222);
    CHECK(sameColor(t.palette.indexed[3], t.paletteBaseline.indexed[3]));

    feed(t, "\033]4;1;#111111\033\\\033]4;3;#333333\033\\");
    feed(t, "\033]104;1;x;3\033\\");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(t.palette.indexed[3].red == 0x3333);
}

// OSC 110-119 each reset the one color their code names. The resets do not chain, and take no data:
// one that carries any is malformed and changes nothing, as in xterm.
static void testDynamicColorResetsActOnTheirOwnCode()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]10;#101010;#111111;#121212;#131313;#141414;#151515;#161616;#171717;#181818;#191919\033\\");
    feed(t, "\033]110;111\033\\");
    CHECK(t.palette.defaultForeground.red == 0x1010);
    CHECK(t.palette.defaultBackground.red == 0x1111);
    feed(t, "\033]110\033\\");
    CHECK(sameColor(t.palette.defaultForeground, t.paletteBaseline.defaultForeground));
    CHECK(t.palette.defaultBackground.red == 0x1111);
    feed(t, "\033]111\033\\");
    CHECK(sameColor(t.palette.defaultBackground, t.paletteBaseline.defaultBackground));
    feed(t, "\033]112\033\\");
    CHECK(sameColor(t.palette.cursor, t.paletteBaseline.cursor));

    feed(t, "\033]113\033\\");
    CHECK(!t.mousePointerForegroundSet);
    CHECK(t.mousePointerBackgroundSet);
    feed(t, "\033]114\033\\");
    CHECK(!t.mousePointerBackgroundSet);
    feed(t, "\033]115\033\\");
    CHECK(!t.tektronixForegroundSet);
    CHECK(t.tektronixBackgroundSet);
    feed(t, "\033]116\033\\");
    CHECK(!t.tektronixBackgroundSet);
    feed(t, "\033]117\033\\");
    CHECK(!t.selectionBackgroundSet);
    CHECK(t.tektronixCursorSet);
    feed(t, "\033]118\033\\");
    CHECK(!t.tektronixCursorSet);
    CHECK(t.selectionForegroundSet);
    feed(t, "\033]119\033\\");
    CHECK(!t.selectionForegroundSet);
}

// A host that refuses color operations refuses every code a chain could reach, the queries and the
// resets included.
static void testColorRefusalCoversTheWholeChain()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);

    feed(t, "\033]13;#131313\033\\");
    t.allowColorOperations = false;
    const Ubiquitty::PaletteRGB cursor = t.palette.cursor;

    feed(t, "\033]12;#aaaaaa;#bbbbbb;#cccccc\033\\");
    CHECK(sameColor(t.palette.cursor, cursor));
    CHECK(t.mousePointerForegroundRed == 0x1313);
    CHECK(!t.mousePointerBackgroundSet);

    feed(t, "\033]12;?;?\033\\");
    feed(t, "\033]113\033\\");
    CHECK(drain(t, buffer) == 0);
    CHECK(t.mousePointerForegroundSet);
}

// A query for a dynamic color no program has set still answers: the pointer with the configured
// foreground and background, which OSC 10 and 11 do not move, the storage tube with the colors it draws in, and the highlight with
// the foreground and background. A reset returns a set color to that answer.
static void testUnsetDynamicColorsAnswerWithTheColorShown()
{
    Ubiquitty::Terminal t;
    char buffer[1024];
    Ubiquitty::hardReset(t);
    feed(t, "\033]10;#102030;#405060\033\\");
    drain(t, buffer);

    feed(t, "\033]13;?;?;?;?;?;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer,
              "\033]13;rgb:c0c0/c0c0/c0c0\033\\\033]14;rgb:1010/1010/1414\033\\"
              "\033]15;rgb:1010/2020/3030\033\\\033]16;rgb:4040/5050/6060\033\\"
              "\033]17;rgb:1010/2020/3030\033\\\033]18;rgb:1010/2020/3030\033\\"
              "\033]19;rgb:4040/5050/6060\033\\")
        == 0);

    // The cursor color chains on into the pointer's.
    feed(t, "\033]12;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]13;rgb:c0c0/c0c0/c0c0\033\\") != nullptr);

    feed(t, "\033]13;#0080ff\033\\\033]13;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]13;rgb:0000/8080/ffff\033\\") == 0);
    feed(t, "\033]113\033\\\033]13;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]13;rgb:c0c0/c0c0/c0c0\033\\") == 0);

    feed(t, "\033]17;#0080ff\033\\\033]117\033\\\033]17;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]17;rgb:1010/2020/3030\033\\") == 0);
}

// The storage tube's unset colors are the user's custom pair when one is chosen, and the cursor
// follows the beam.
static void testUnsetTektronixColorsFollowTheCustomPreference()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);
    t.tekColorPreference.mode = Ubiquitty::Tek::ColorMode::Custom;
    t.tekColorPreference.beam = { 0x3333, 0xFFFF, 0x3333 };
    t.tekColorPreference.background = { 0x0101, 0x0202, 0x0303 };

    feed(t, "\033]15;?;?;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]18;rgb:3333/ffff/3333\033\\") != nullptr);
    CHECK(std::strstr(buffer, "\033]15;rgb:3333/ffff/3333\033\\") != nullptr);
    CHECK(std::strstr(buffer, "\033]16;rgb:0101/0202/0303\033\\") != nullptr);
}

// A special color no program has set reports the default foreground, through OSC 5 and through
// its OSC 4 alias.
static void testUnsetSpecialColorsReportTheForeground()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);
    feed(t, "\033]10;#123456\033\\");

    feed(t, "\033]5;0;?;4;?\033\\\033]4;258;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]5;0;rgb:1212/3434/5656\033\\\033]5;4;rgb:1212/3434/5656\033\\\033]4;258;rgb:1212/3434/5656\033\\") == 0);
}

// A special color has no configurable resource to be restored from, so neither OSC 105 nor
// OSC 104's alias for it changes one, and a bare OSC 104 resets only the 256 palette colors.
static void testSpecialColorResetsLeaveTheColor()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);

    feed(t, "\033]5;0;red;1;blue\033\\");
    feed(t, "\033]105;0\033\\\033]105\033\\\033]104;256\033\\\033]104\033\\");
    feed(t, "\033]5;0;?;1;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033]5;0;rgb:ffff/0000/0000\033\\\033]5;1;rgb:0000/0000/ffff\033\\") == 0);
}

// OSC 104 reads its list as xterm does: an entry must begin with a digit and be followed by ';' or
// the end, and the first that is not ends the list without resetting itself.
static void testPaletteResetListEndsAtAMalformedEntry()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;#111111;2;#222222;3;#333333\033\\");
    feed(t, "\033]104;1x;2\033\\");
    CHECK(t.palette.indexed[1].red == 0x1111);
    CHECK(t.palette.indexed[2].red == 0x2222);

    feed(t, "\033]104;;2\033\\");
    CHECK(t.palette.indexed[2].red == 0x2222);

    feed(t, "\033]104; 2\033\\");
    CHECK(t.palette.indexed[2].red == 0x2222);

    feed(t, "\033]104;\033\\");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(sameColor(t.palette.indexed[3], t.paletteBaseline.indexed[3]));
}

// After its last number OSC 104 goes on reading what an earlier, longer control string left after
// the end of its own, as xterm's does: "104;1;3" over "4;1;red;2;blue;3;green" finds "2;blue" and
// resets color 2 too, and a list that ends in something that is not a number reads no further.
static void testPaletteResetReadsOnPastItsEnd()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;red;2;blue;3;green\007");
    feed(t, "\033]104;1;3\007");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(sameColor(t.palette.indexed[2], t.paletteBaseline.indexed[2]));
    CHECK(sameColor(t.palette.indexed[3], t.paletteBaseline.indexed[3]));

    feed(t, "\033]4;1;red;2;blue\007");
    feed(t, "\033]104;1\007");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(t.palette.indexed[2].blue == 0xFFFF && t.palette.indexed[2].red == 0);
}

// A reset carrying data is malformed in xterm and ignored, for every code from 110 to 119; an empty
// field after the separator is no data, so it resets.
static void testDynamicColorResetsIgnoreData()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]10;red;blue;green;#131313;#141414;#151515;#161616;#171717;#181818;#191919\007");
    for (int code = 110; code <= 119; code++) {
        char sequence[32];
        std::snprintf(sequence, sizeof(sequence), "\033]%d;1\007", code);
        feed(t, sequence);
    }
    CHECK(t.palette.defaultForeground.red == 0xFFFF);
    CHECK(t.palette.defaultBackground.blue == 0xFFFF);
    CHECK(t.palette.cursor.green == 0xFFFF);
    CHECK(t.mousePointerForegroundSet && t.mousePointerBackgroundSet);
    CHECK(t.tektronixForegroundSet && t.tektronixBackgroundSet && t.tektronixCursorSet);
    CHECK(t.selectionBackgroundSet && t.selectionForegroundSet);

    feed(t, "\033]110;\007");
    CHECK(sameColor(t.palette.defaultForeground, t.paletteBaseline.defaultForeground));
}

// DECSTR and RIS put the indexed palette back, as xterm's ReallyReset does for both. Neither touches
// the colours OSC 10-19 set: those return only through OSC 110-119.
static void testResetsRestoreTheIndexedPaletteOnly()
{
    Ubiquitty::Terminal t;
    char buffer[512];
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;1;red\007\033]10;red;blue;green;#131313\007\033]17;#171717\007");
    feed(t, "\033[!p");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    CHECK(t.palette.defaultForeground.red == 0xFFFF);
    CHECK(t.palette.defaultBackground.blue == 0xFFFF);

    feed(t, "\033]4;1;red\007");
    feed(t, "\033c");
    CHECK(sameColor(t.palette.indexed[1], t.paletteBaseline.indexed[1]));
    feed(t, "\033]10;?;?;?;?\007\033]17;?\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(std::strstr(buffer, "\033]10;rgb:ffff/0000/0000") != nullptr);
    CHECK(std::strstr(buffer, "\033]11;rgb:0000/0000/ffff") != nullptr);
    CHECK(std::strstr(buffer, "\033]12;rgb:0000/ffff/0000") != nullptr);
    CHECK(std::strstr(buffer, "\033]13;rgb:1313/1313/1313") != nullptr);
    CHECK(std::strstr(buffer, "\033]17;rgb:1717/1717/1717") != nullptr);
}

int runColorConformanceTests()
{
    failures = 0;
    testHashFormsAreLeftJustified();
    testRGBFormScales();
    testRgbiForm();
    testCieAndTekhvcSpaces();
    testMalformedSpecsLeaveTheSlot();
    testHslExtensionStillParses();
    testQueryReportsTheConvertedValue();
    testDynamicColorsChain();
    testDynamicColorResetRestoresTheBaseline();
    testDynamicColorsChainThroughNineteen();
    testDynamicColorQueriesChainPastTwelve();
    testEmptyAndMalformedFieldsAdvanceTheChain();
    testIndexedColorNumbersReadAsXtermReadsThem();
    testPaletteResetTakesSeveralIndices();
    testDynamicColorResetsActOnTheirOwnCode();
    testColorRefusalCoversTheWholeChain();
    testUnsetDynamicColorsAnswerWithTheColorShown();
    testUnsetTektronixColorsFollowTheCustomPreference();
    testUnsetSpecialColorsReportTheForeground();
    testSpecialColorResetsLeaveTheColor();
    testPaletteResetListEndsAtAMalformedEntry();
    testPaletteResetReadsOnPastItsEnd();
    testDynamicColorResetsIgnoreData();
    testResetsRestoreTheIndexedPaletteOnly();

    if (failures == 0)
        printf("test_conformance_color: all passed\n");
    else
        printf("test_conformance_color: %d FAILED\n", failures);
    return failures;
}

// Defined out when the harness links this TU into an aggregate runner that owns main().
#ifndef UBIQUITTY_AGGREGATED_TESTS
int main() { return runColorConformanceTests() == 0 ? 0 : 1; }
#endif
