// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::cell;
using UbiquittyTest::feed;

// test_textdecoration.cpp — Core coverage for text-decoration SGR state
// and box-drawing characterSet / codePoint handling.
//
// These exercise the portable parser surface (Source/Core/VT/VT.cpp +
// SGR.cpp): feeding SGR / characterSet-designation bytes through the Ubiquitty::Terminal and
// reading the resulting Cell pen attributes, underline style/color, and
// box-drawing codePoint resolution. Complements the SGR sweep already
// in Tests/Core/VT/test_vt100_characterset.cpp by pinning the full DEC
// special-graphics characterSet map,
// direct Unicode box-drawing codePoints, and the underline/strike/undercurl
// state in a focused per-feature layout. (Box-drawing-ness is a frontend
// paint concern derived from the cell codePoint; the Core stamps no
// line-drawing attribute bit.)
//
// Run via Scripts/run-tests.sh as test_textdecoration.

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>

// ---------- Underline variants -------------------------------------

/// SGR 4 sets single underline (both the style field and the legacy
/// Attribute::Underline bit); SGR 24 clears both.
TEST(testUnderlineSingleAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4mA\033[24mB");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineSingle);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 1)->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// The ITU colon sub-parameter form 4:1..4:5 each selects a distinct
/// underline shape in Pen::underlineStyle; the legacy Attribute::Underline bit
/// tracks "style is non-zero" so bit-only paint paths keep working.
TEST(testUnderlineColonStyles)
{
    struct Case {
        const char* seq;
        uint8_t style;
    };
    const Case cases[] = {
        { "\033[4:1mA", Ubiquitty::VT::underlineSingle },
        { "\033[4:2mA", Ubiquitty::VT::underlineDouble },
        { "\033[4:3mA", Ubiquitty::VT::underlineCurly },
        { "\033[4:4mA", Ubiquitty::VT::underlineDotted },
        { "\033[4:5mA", Ubiquitty::VT::underlineDashed },
    };
    Ubiquitty::Terminal t;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, cases[i].seq);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        CHECK(c->pen.underlineStyle == cases[i].style);
        CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    }
}

/// SGR 4:0 explicitly turns the underline off — style none and the bit
/// clear — without going through 24.
TEST(testUnderlineColonZeroOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3mA\033[4:0mB");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(cell(t, 0, 1)->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// Undercurl (SGR 4:3) specifically resolves to underlineCurly and keeps
/// the underline bit set so a non-style-aware painter still draws a line.
TEST(testUndercurl)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3mA");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// An unknown colon style (>5) clamps to a drawn single underline: an
/// unrecognised style must still draw, never drop the underline.
TEST(testUnderlineUnknownStyleClamps)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:7mA");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineSingle);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// SGR 24 resets a non-single style (curly) all the way to none.
TEST(testUnderline24ResetsAnyStyle)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:5mA\033[24mB");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineDashed);
    CHECK(cell(t, 0, 1)->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// SGR 21 in this Core folds onto the BOLD|DIM reset arm (it is not a
/// double-underline selector here — double underline is reached via 4:2).
/// Pin the actual behavior so a future divergence surfaces.
TEST(testSGR21DoubleUnderline)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // SGR 21 = doubly underlined (ECMA-48 / xterm). It is not an intensity
    // control: it leaves bold/dim untouched, and SGR 22 is the bold+dim
    // reset.
    feed(t, "\033[1;2;21mA\033[22mB");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineDouble);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Dim));
    // 22 clears bold+dim but not the (separately set) underline.
    CHECK(!cell(t, 0, 1)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Dim }));
}

// ---------- Superscript / subscript (SGR 73 / 74 / 75) -------------

/// SGR 73 sets the superscript script state, 74 the subscript, 75 clears
/// both back to none. The state is packed in the high bits of underlineStyle;
/// read it through penScript.
TEST(testScriptSuperSubReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[73mA\033[74mB\033[75mC");
    CHECK(Ubiquitty::penScript(&cell(t, 0, 0)->pen) == Ubiquitty::VT::scriptSuper);
    CHECK(Ubiquitty::penScript(&cell(t, 0, 1)->pen) == Ubiquitty::VT::scriptSub);
    CHECK(Ubiquitty::penScript(&cell(t, 0, 2)->pen) == Ubiquitty::VT::scriptNone);
}

/// Super and sub are mutually exclusive: 74 after 73 leaves only sub (and
/// vice-versa), never both.
TEST(testScriptMutualExclusion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[73;74mA\033[74;73mB");
    CHECK(Ubiquitty::penScript(&cell(t, 0, 0)->pen) == Ubiquitty::VT::scriptSub); // last wins
    CHECK(Ubiquitty::penScript(&cell(t, 0, 1)->pen) == Ubiquitty::VT::scriptSuper);
}

/// The script state shares Pen::underlineStyle with the underline shape but
/// is orthogonal: setting/clearing one must not disturb the other.
TEST(testScriptOrthogonalToUnderline)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // superscript + curly underline together, then drop the underline (24)
    // and confirm the superscript survives; then drop the script (75) and
    // confirm a fresh underline is unaffected.
    feed(t, "\033[73;4:3mA\033[24mB\033[75;4mC");
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(Ubiquitty::penScript(&a->pen) == Ubiquitty::VT::scriptSuper);
    CHECK(Ubiquitty::penUnderlineShape(&a->pen) == Ubiquitty::VT::underlineCurly);
    const Ubiquitty::Cell* b = cell(t, 0, 1);
    CHECK(Ubiquitty::penScript(&b->pen) == Ubiquitty::VT::scriptSuper); // underline gone, script kept
    CHECK(Ubiquitty::penUnderlineShape(&b->pen) == Ubiquitty::VT::underlineNone);
    CHECK(!b->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    const Ubiquitty::Cell* c = cell(t, 0, 2);
    CHECK(Ubiquitty::penScript(&c->pen) == Ubiquitty::VT::scriptNone); // 75 cleared script
    CHECK(Ubiquitty::penUnderlineShape(&c->pen) == Ubiquitty::VT::underlineSingle); // underline intact
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// A scripted cell with no underline must not light the underline bit, and
/// underlineStyle's shape nibble must read as none (the script bits live in
/// the high bits and are masked off by penUnderlineShape).
TEST(testScriptDoesNotImplyUnderline)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[74mA");
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(Ubiquitty::penScript(&a->pen) == Ubiquitty::VT::scriptSub);
    CHECK(Ubiquitty::penUnderlineShape(&a->pen) == Ubiquitty::VT::underlineNone);
    CHECK(!a->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// SGR 0 (full reset) clears the script state along with everything else.
TEST(testScriptResetViaSGR0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[73mA\033[0mB");
    CHECK(Ubiquitty::penScript(&cell(t, 0, 0)->pen) == Ubiquitty::VT::scriptSuper);
    CHECK(Ubiquitty::penScript(&cell(t, 0, 1)->pen) == Ubiquitty::VT::scriptNone);
}

// ---------- Underline color (SGR 58 / 59) --------------------------

/// SGR 58 indexed (colon + semicolon) lands the palette index in underlineIndex
/// without touching the style; SGR 59 resets underlineIndex to follow text color.
TEST(testUnderlineColorIndexed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3;58:5:33mA\033[59mB");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == 33);
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(cell(t, 0, 1)->pen.underlineIndex == Ubiquitty::colorDefault);
}

/// SGR 58:2 truecolor stores the RGB triple and marks underlineIndex Ubiquitty::colorRGB.
TEST(testUnderlineColorTruecolor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[58:2::10:20:30mA");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->pen.underlineIndex == Ubiquitty::colorRGB);
    CHECK(c->pen.underlineRed == 10);
    CHECK(c->pen.underlineGreen == 20);
    CHECK(c->pen.underlineBlue == 30);
}

/// Three colon-form colours in one SGR all land. The parameter cap counts
/// colon-separated pieces, so foreground, background and underline in the ITU
/// notation come to eighteen pieces — over a sixteen-slot cap before a single
/// attribute joins them, and the underline run is the one that falls off.
TEST(testThreeColonFormColorsInOneSGR)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[38:2::255:128:0;48:2::0:128:255;58:2::10:20:30mA");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(c->pen.foregroundRed == 255);
    CHECK(c->pen.backgroundIndex == Ubiquitty::colorRGB);
    CHECK(c->pen.backgroundBlue == 255);
    CHECK(c->pen.underlineIndex == Ubiquitty::colorRGB);
    CHECK(c->pen.underlineRed == 10);
    CHECK(c->pen.underlineGreen == 20);
    CHECK(c->pen.underlineBlue == 30);
}

/// SGR 0 resets the underline channel (style + bit + color) along with the
/// rest of the pen.
TEST(testUnderlineFullResetViaSGR0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3;58:5:7mA\033[0mB");
    const Ubiquitty::Cell* c = cell(t, 0, 1);
    CHECK(c->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(c->pen.underlineIndex == Ubiquitty::colorDefault);
}

/// ---------- Strikethrough (SGR 9 / 29) -----------------------------

TEST(testStrikeSetAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[9mA\033[29mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
}

/// Strike is orthogonal to underline: 9 and 4:3 coexist, and 29 clears
/// only the strike bit, leaving the curly underline intact.
TEST(testStrikeOrthogonalToUnderline)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[9;4:3mA\033[29mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
    CHECK(cell(t, 0, 1)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

/// SGR 0 clears strike.
TEST(testStrikeResetViaSGR0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[9mA\033[0mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Strike));
}

// ---------- DEC special graphics characterSet (ESC ( 0) -----------------

/// The full 32-glyph DEC special-graphics map. ESC ( 0 designates the
/// characterSet into G0; each source byte _.._~ folds to its canonical Unicode
/// codePoint. ESC ( B returns to US-ASCII so each probe is independent.
TEST(testDecSpecialGraphicsFullMap)
{
    struct Probe {
        char in;
        uint16_t want;
    };
    const Probe probes[] = {
        { '`', 0x25C6 }, // ◆ diamond
        { 'a', 0x2592 }, // ▒ checker
        { 'b', 0x2409 }, // HT
        { 'c', 0x240C }, // FF
        { 'd', 0x240D }, // CR
        { 'e', 0x240A }, // LF
        { 'f', 0x00B0 }, // ° degree
        { 'g', 0x00B1 }, // ± plus-minus
        { 'h', 0x2424 }, // NL
        { 'i', 0x240B }, // VT
        { 'j', 0x2518 }, // ┘
        { 'k', 0x2510 }, // ┐
        { 'l', 0x250C }, // ┌
        { 'm', 0x2514 }, // └
        { 'n', 0x253C }, // ┼
        { 'o', 0x23BA }, // scan 1
        { 'p', 0x23BB }, // scan 3
        { 'q', 0x2500 }, // ─
        { 'r', 0x23BC }, // scan 7
        { 's', 0x23BD }, // scan 9
        { 't', 0x251C }, // ├
        { 'u', 0x2524 }, // ┤
        { 'v', 0x2534 }, // ┴
        { 'w', 0x252C }, // ┬
        { 'x', 0x2502 }, // │
        { 'y', 0x2264 }, // ≤
        { 'z', 0x2265 }, // ≥
        { '{', 0x03C0 }, // π
        { '|', 0x2260 }, // ≠
        { '}', 0x00A3 }, // £
        { '~', 0x00B7 }, // · middle dot
    };
    Ubiquitty::Terminal t;
    char buffer[16];
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        Ubiquitty::hardReset(t);
        snprintf(buffer, sizeof(buffer), "\033(0%c\033(B", probes[i].in);
        feed(t, buffer);
        if (cell(t, 0, 0)->codePoint != probes[i].want) {
            printf("FAIL %s: ESC ( 0 %c -> %04x (want %04x)\n", __func__, probes[i].in, cell(t, 0, 0)->codePoint, probes[i].want);
            failures++;
        }
    }
}

/// A byte with no DEC-graphics substitution passes through as itself even
/// while the characterSet is active (e.g. space, digits).
TEST(testDecSpecialGraphicsPassthrough)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0 1A\033(B");
    CHECK(cell(t, 0, 0)->codePoint == ' ');
    CHECK(cell(t, 0, 1)->codePoint == '1');
    CHECK(cell(t, 0, 2)->codePoint == 'A');
}

/// The line-drawing folds resolve to the box-drawing Unicode block; the
/// non-line-drawing folds in the same characterSet (degree, ≤, π, middle dot)
/// resolve to their own codePoints. Box-drawing-ness is derived at paint.
TEST(testDecGraphicsAttrOnlyOnLineDrawing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // l q k = corners + horizontal (line drawing); f y { = degree, ≤, π.
    feed(t, "\033(0lqkfy{\033(B");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C);
    CHECK(cell(t, 0, 1)->codePoint == 0x2500);
    CHECK(cell(t, 0, 2)->codePoint == 0x2510);
    CHECK(cell(t, 0, 3)->codePoint == 0x00B0); // °
    CHECK(cell(t, 0, 4)->codePoint == 0x2264); // ≤
    CHECK(cell(t, 0, 5)->codePoint == 0x03C0); // π
}

/// ESC ( B (US-ASCII) un-designates the graphics characterSet: a following
/// 'l' renders as the literal letter, not ┌.
TEST(testDecGraphicsCharacterSetRelease)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0l\033(Bl");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // ┌ while active
    CHECK(cell(t, 0, 1)->codePoint == 'l'); // literal after release
}

// ---------- Direct Unicode box-drawing codePoints (U+2500..257F) ---

/// Box-drawing codePoints fed directly as UTF-8 (not via the characterSet)
/// land verbatim in cells and occupy one column each (box-drawing-ness is
/// derived at paint from the codePoint, not stamped as an attribute).
TEST(testUnicodeBoxDrawingDirect)
{
    struct Probe {
        const char* utf8;
        uint16_t want;
    };
    const Probe probes[] = {
        { "\xE2\x94\x80", 0x2500 }, // ─ light horizontal
        { "\xE2\x94\x82", 0x2502 }, // │ light vertical
        { "\xE2\x94\x8C", 0x250C }, // ┌
        { "\xE2\x94\x90", 0x2510 }, // ┐
        { "\xE2\x94\x94", 0x2514 }, // └
        { "\xE2\x94\x98", 0x2518 }, // ┘
        { "\xE2\x94\xBC", 0x253C }, // ┼ cross
        { "\xE2\x95\x94", 0x2554 }, // ╔ double down+right
        { "\xE2\x95\x90", 0x2550 }, // ═ double horizontal
        { "\xE2\x95\xAD", 0x256D }, // ╭ rounded corner
    };
    Ubiquitty::Terminal t;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, probes[i].utf8);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        if (c->codePoint != probes[i].want) {
            printf("FAIL %s: U+%04x landed as %04x\n", __func__, probes[i].want, c->codePoint);
            failures++;
        }
        // Single-column: the next cell is untouched (blank).
        CHECK(cell(t, 0, 1)->codePoint == ' ' || cell(t, 0, 1)->codePoint == 0);
    }
}

/// Box-drawing cells carry the active pen attributes (e.g. a curly
/// underline color) just like text — the graphic bit is additive.
TEST(testUnicodeBoxDrawingKeepsPen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[31m\xE2\x94\x80");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->codePoint == 0x2500);
    CHECK(c->pen.foregroundIndex == 1);
}

/// The double-line, heavy, and rounded-corner box-drawing codePoints fold
/// to known graphic IDs through Ubiquitty::Unicode::toMacRoman, so they land
/// verbatim in the cell just like the single-line set.
TEST(testUnicodeBoxDrawingDoubleHeavyRounded)
{
    struct Probe {
        const char* utf8;
        uint16_t want;
    };
    const Probe probes[] = {
        { "\xE2\x95\x90", 0x2550 }, // ═ double horizontal
        { "\xE2\x95\x91", 0x2551 }, // ║ double vertical
        { "\xE2\x95\x94", 0x2554 }, // ╔
        { "\xE2\x95\x97", 0x2557 }, // ╗
        { "\xE2\x95\x9A", 0x255A }, // ╚
        { "\xE2\x95\x9D", 0x255D }, // ╝
        { "\xE2\x95\xA6", 0x2566 }, // ╦
        { "\xE2\x95\xA9", 0x2569 }, // ╩
        { "\xE2\x95\xA0", 0x2560 }, // ╠
        { "\xE2\x95\xA3", 0x2563 }, // ╣
        { "\xE2\x95\xAC", 0x256C }, // ╬
        { "\xE2\x94\x81", 0x2501 }, // ━ heavy horizontal
        { "\xE2\x94\x83", 0x2503 }, // ┃ heavy vertical
        { "\xE2\x95\xAD", 0x256D }, // ╭ rounded top-left
        { "\xE2\x95\xAE", 0x256E }, // ╮ rounded top-right
        { "\xE2\x95\xAF", 0x256F }, // ╯ rounded bottom-right
        { "\xE2\x95\xB0", 0x2570 }, // ╰ rounded bottom-left
    };
    Ubiquitty::Terminal t;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, probes[i].utf8);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        if (c->codePoint != probes[i].want) {
            printf("FAIL %s: U+%04x landed as %04x\n", __func__, probes[i].want, c->codePoint);
            failures++;
        }
    }
}

/// Block-element codePoints (U+2580..U+2593 + the eighth-blocks + shades)
/// are part of the graphic set: they fold to known IDs the renderer can
/// draw procedurally (derived from the codePoint at paint).
TEST(testUnicodeBlockElements)
{
    struct Probe {
        const char* utf8;
        uint16_t want;
    };
    const Probe probes[] = {
        { "\xE2\x96\x88", 0x2588 }, // █ full block
        { "\xE2\x96\x80", 0x2580 }, // ▀ upper half
        { "\xE2\x96\x84", 0x2584 }, // ▄ lower half
        { "\xE2\x96\x8C", 0x258C }, // ▌ left half
        { "\xE2\x96\x90", 0x2590 }, // ▐ right half
        { "\xE2\x96\x91", 0x2591 }, // ░ light shade
        { "\xE2\x96\x92", 0x2592 }, // ▒ medium shade
        { "\xE2\x96\x93", 0x2593 }, // ▓ dark shade
        { "\xE2\x96\x81", 0x2581 }, // ▁ lower one eighth
        { "\xE2\x96\x95", 0x2595 }, // ▕ right one eighth
    };
    Ubiquitty::Terminal t;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, probes[i].utf8);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        if (c->codePoint != probes[i].want) {
            printf("FAIL %s: U+%04x landed as %04x\n", __func__, probes[i].want, c->codePoint);
            failures++;
        }
    }
}

/// Graphic-ID discrimination at U+0080+: codePoints land verbatim regardless
/// of whether the frontend can draw them procedurally. The `graphic` probe
/// field records the expected paint-time classification for documentation;
/// the Core's job, asserted here, is codePoint resolution.
TEST(testAttrGraphicDiscrimination)
{
    struct Probe {
        const char* utf8;
        uint16_t want;
        bool graphic;
    };
    const Probe probes[] = {
        { "A", 0x0041, false }, // ASCII: < 0x80 fast path
        { "\xC3\xA9", 0x00E9, false }, // é Latin-1: not a graphic
        { "\xE2\x80\xA2", 0x2022, false }, // • U+2022 folds to no glyph
        { "\xE2\x97\x86", 0x25C6, true }, // ◆ black diamond
        { "\xE2\x97\x8F", 0x25CF, true }, // ● black circle
        { "\xE2\x86\x92", 0x2192, true }, // → rightwards arrow
        { "\xE2\x9C\x93", 0x2713, true }, // ✓ check mark
        { "\xE2\x96\xAA", 0x25AA, true }, // ▪ small black square
    };
    Ubiquitty::Terminal t;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, probes[i].utf8);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        // Core's job is codePoint resolution; whether a glyph renders
        // procedurally is a frontend concern, covered in test_unicode.
        CHECK(c->codePoint == probes[i].want);
    }
}

// ---------- Framed (SGR 51) ----------------------------------------

/// SGR 51 sets the Attribute::Framed bit.
TEST(testFramedAttribute)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[51mA");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Framed));
}

/// ---------- Variable weight (SGR 1 / 1:n) --------------------------

TEST(testWeightSelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // bare 1 = bold (no weight); 1:5 = weight 5; 1:0 clears weight.
    feed(t, "\033[1mA\033[0m\033[1:5mB\033[0m\033[1:5m\033[1:0mC");
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(a->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(a->pen.extendedAttributes.weight() == 0);
    const Ubiquitty::Cell* b = cell(t, 0, 1);
    CHECK(b->pen.extendedAttributes.weight() == 5);
    CHECK(!b->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold)); // 1:n is not bold
    const Ubiquitty::Cell* c = cell(t, 0, 2);
    CHECK(c->pen.extendedAttributes.weight() == 0); // 1:0 cleared it
}

TEST(testWeightResetVia22And0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1:7mA\033[22mB\033[1:7mC\033[0mD");
    CHECK(cell(t, 0, 0)->pen.extendedAttributes.weight() == 7);
    CHECK(cell(t, 0, 1)->pen.extendedAttributes.weight() == 0); // 22
    CHECK(cell(t, 0, 3)->pen.extendedAttributes.weight() == 0); // 0
}

// ---------- Ideogram attributes (SGR 60-65) -------------------------

/// Each of SGR 60-64 maps onto its own value in penIdeogram; 65 resets to
/// none. The state rides Pen::underlineStyle's high bits.
TEST(testIdeogramEachCode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[60mA\033[61mB\033[62mC\033[63mD\033[64mE\033[65mF");
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 0)->pen) == Ubiquitty::VT::ideogramUnderline);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 1)->pen) == Ubiquitty::VT::ideogramDoubleUnderline);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 2)->pen) == Ubiquitty::VT::ideogramOverline);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 3)->pen) == Ubiquitty::VT::ideogramDoubleOverline);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 4)->pen) == Ubiquitty::VT::ideogramStress);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 5)->pen) == Ubiquitty::VT::ideogramNone); // 65 reset
}

/// SGR 0 clears any ideogram attribute.
TEST(testIdeogramResetViaSGR0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[64mA\033[0mB");
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 0)->pen) == Ubiquitty::VT::ideogramStress);
    CHECK(Ubiquitty::penIdeogram(&cell(t, 0, 1)->pen) == Ubiquitty::VT::ideogramNone);
}

/// The ideogram state shares Pen::underlineStyle with the underline shape and
/// the super/sub script bits but is orthogonal: setting one leaves the others
/// intact.
TEST(testIdeogramOrthogonalToUnderlineAndScript)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3m\033[73m\033[62mA");
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(Ubiquitty::penIdeogram(&a->pen) == Ubiquitty::VT::ideogramOverline);
    CHECK(Ubiquitty::penUnderlineShape(&a->pen) == Ubiquitty::VT::underlineCurly); // underline kept
    CHECK(Ubiquitty::penScript(&a->pen) == Ubiquitty::VT::scriptSuper); // script kept
}

// ---------- Alternate fonts (10-19) + Fraktur (20) ------------------

/// SGR 11-19 all fold to fontAlternate; SGR 20 selects Fraktur; SGR 10 (and
/// SGR 0) restore the primary font.
TEST(testFontAlternateFrakturPrimary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[11mA\033[19mB\033[20mC\033[10mD");
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 0)->pen) == Ubiquitty::VT::fontAlternate);
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 1)->pen) == Ubiquitty::VT::fontAlternate); // 19 also alternate
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 2)->pen) == Ubiquitty::VT::fontFraktur);
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 3)->pen) == Ubiquitty::VT::fontPrimary); // 10 reset
}

/// SGR 23 (not italic / not Fraktur) resets a Fraktur selection to primary but
/// leaves an alternate-font selection untouched; SGR 0 resets either.
TEST(testFontResetVia23AndSGR0)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[20m\033[23mA\033[11m\033[23mB\033[20m\033[0mC");
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 0)->pen) == Ubiquitty::VT::fontPrimary); // 23 cleared Fraktur
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 1)->pen) == Ubiquitty::VT::fontAlternate); // 23 left alternate alone
    CHECK(Ubiquitty::penFontStyle(&cell(t, 0, 2)->pen) == Ubiquitty::VT::fontPrimary); // 0 cleared Fraktur
}

/// The font style and the proportional-spacing flag ride the same packed pen
/// byte but are independent: SGR 26 plus a Fraktur font keep their own fields.
TEST(testFontOrthogonalToProportional)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[26m\033[20mA");
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(Ubiquitty::penFontStyle(&a->pen) == Ubiquitty::VT::fontFraktur);
    CHECK(Ubiquitty::penProportional(&a->pen)); // proportional flag intact
}

// ---------- Proportional spacing (SGR 26 / 50) ---------------------

/// SGR 26 sets the proportional-spacing flag (reserved, no render); SGR 50
/// clears it, as does SGR 0.
TEST(testProportionalSpacingSetAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[26mA\033[50mB\033[26m\033[0mC");
    CHECK(Ubiquitty::penProportional(&cell(t, 0, 0)->pen)); // 26 set
    CHECK(!Ubiquitty::penProportional(&cell(t, 0, 1)->pen)); // 50 cleared
    CHECK(!Ubiquitty::penProportional(&cell(t, 0, 2)->pen)); // 0 cleared
}

// ---------- Emoji presentation forcing (SGR 76 : 1 / 76 : 2) -------

// U+263A WHITE SMILING FACE: Extended_Pictographic, default TEXT presentation,
// width 1. UTF-8 E2 98 BA. (A width-1 dual-presentation symbol, so the
// forced-emoji widen-to-2 is observable.)
#define SMILE_UTF8 "\xE2\x98\xBA"
// U+1F600 GRINNING FACE: default EMOJI presentation, width 2. UTF-8 F0 9F 98 80.
#define GRIN_UTF8 "\xF0\x9F\x98\x80"
// U+FE0F VS16 (emoji selector), U+FE0E VS15 (text selector).
#define VS16_UTF8 "\xEF\xB8\x8F"
#define VS15_UTF8 "\xEF\xB8\x8E"

/// SGR 76:1 forces emoji (color) presentation on a default-text pictograph:
/// Attribute::Emoji is set and the glyph widens to a 2-cell pair.
TEST(testEmojiForceColorOnTextDefault)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[76:1m" SMILE_UTF8);
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->codePoint == 0x263A);
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // emoji presentation = width 2
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

/// SGR 76:2 forces text (monochrome) presentation on a default-emoji codePoint:
/// Attribute::Emoji is cleared and the glyph occupies a single cell.
TEST(testEmojiForceTextOnEmojiDefault)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[76:2m" GRIN_UTF8 "X");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->codePoint == 0x1F600);
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // text presentation = width 1
    // The next glyph lands in column 1 (no wide-continuation consumed it).
    CHECK(cell(t, 0, 1)->codePoint == 'X');
}

/// Forcing overrides a following VS-15/VS-16 selector: 76:1 + base + VS15 stays
/// color, 76:2 + emoji + VS16 stays mono.
TEST(testEmojiForceOverridesVariationSelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // 76:1 (force color) then heart + VS15 (which alone would force text).
    feed(t, "\033[76:1m" SMILE_UTF8 VS15_UTF8);
    const Ubiquitty::Cell* a = cell(t, 0, 0);
    CHECK(a->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // force wins over VS15
    CHECK(a->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));

    // Fresh row: 76:2 (force mono) then grinning face + VS16.
    feed(t, "\r\n\033[76:2m" GRIN_UTF8 VS16_UTF8 "X");
    const Ubiquitty::Cell* b = cell(t, 1, 0);
    CHECK(!b->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // force wins over VS16
    CHECK(!b->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 1, 1)->codePoint == 'X');
}

/// 76:0, bare 76, and SGR 0 clear forcing: a default-text heart after the reset
/// is back to its monochrome, single-cell auto presentation.
TEST(testEmojiForceReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[76:1m\033[76:0m" SMILE_UTF8 "Y");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->codePoint == 0x263A);
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // auto: heart defaults to text
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->codePoint == 'Y');

    // SGR 0 also clears the force state (it routes through penReset).
    feed(t, "\r\n\033[76:1m\033[0m" SMILE_UTF8);
    const Ubiquitty::Cell* d = cell(t, 1, 0);
    CHECK(!d->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));
    CHECK(!d->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
}

/// Forcing is a no-op on a non-pictographic base (a plain letter has no color
/// glyph): the force bit rides the pen but presentation/width are unchanged.
TEST(testEmojiForceNoopOnNonPictographic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[76:1mA");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c->codePoint == 'A');
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation)); // pen carries it
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // but 'A' is not retagged
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // and stays width 1
}

UBIQUITTY_TEST_MAIN()
