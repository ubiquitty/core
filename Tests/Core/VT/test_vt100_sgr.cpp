// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Select Graphic Rendition, and the per-cell state it writes.
//
// The attribute bits, the three colour models (indexed, 256, truecolor) in both
// their semicolon and colon forms, the extended underline styles, hyperlinks,
// and the line-rendition controls DECDWL and DECSWL.
//
// SGR's failure mode is parametric rather than behavioural: the effect of a
// selector is easy, and the arithmetic that decides how many parameters it
// consumes is where the defects are. A run mis-counted by one leaks its
// remainder into the loop as standalone codes, which is a silent pen rewrite
// rather than a visible error.

#include "Harness.h"
#include "VT100Fixture.h"

#include <cstdio>
#include <cstring>

TEST(testDECDWLSetsCurrentRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#6");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeNormal);
}

TEST(testDECDHLTopAndBottomSetDistinctAttributes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#3");
    feed(t, "\033[2;1H\033#4");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK(t.screens.active->lineAttribute[1] == Ubiquitty::lineAttributeDoubleHeightBottom);
}

TEST(testDECSWLResetsLineattr)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033#6");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
    feed(t, "\033#5");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeNormal);
}

TEST(testDECDWLPersistsAcrossTyping)
{
    /// Typing into a DWL row must not clear the attribute; it persists
    /// until DECSWL, scroll, erase, or DECALN.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033#6");
    feed(t, "hello world");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
}

TEST(testDECSWLUnknownHashByteNoOp)
{
    /// ESC # <bytes other than 3..6, 8> are no-operations; lineAttribute unchanged.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033#6");
    feed(t, "\033#0"); // unhandled
    feed(t, "\033#9"); // unhandled
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
}

TEST(testDECRARATogglesSelectedAttributes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB");
    // Ps=1 selects BOLD — not reverse.
    CHECK((cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold)) == 0);
    feed(t, "\033[1;1;1;2;1$t");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK((cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse)) == 0); // reverse untouched
    // Apply again: the selected attribute toggles back off.
    feed(t, "\033[1;1;1;2;1$t");
    CHECK((cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold)) == 0);
    // Ps=7 selects reverse.
    feed(t, "\033[1;1;1;2;7$t");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK((cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold)) == 0);
    // No selector list = toggle all four: bold/underline/blink flip on, the
    // reverse set just above flips back off.
    feed(t, "\033[1;1;1;2$t");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK((cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse)) == 0);
}

TEST(testDECRARAReversesSelectorsCumulativelyInSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1mab\033[0mcd");
    // Bold, underline, bold: bold is reversed twice and so is as it was, underline once.
    feed(t, "\033[1;1;1;4;1;4;1$t");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(!cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    // 0 reverses all four, and a following 1 reverses bold back: bold ends as it began.
    feed(t, "\033[1;1;1;1;0;1$t");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
}

TEST(testGRCMResetMakesEachSGRReplaceTheRendition)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[21l\033[1m\033[4mX");
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    // Colours are renditions too; the SGR names only bold, so the colour is gone.
    feed(t, "\033[31m\033[1mY");
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorDefault);
    // One SGR may name several aspects, and a bare SGR is SGR 0.
    feed(t, "\033[1;4mZ\033[mW");
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(!cell(t, 0, 3)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Underline }));
}

TEST(testGRCMSetAndSoftResetReturnToCumulativeRendition)
{
    for (const char* restore : { "\033[21h", "\033[!p" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[21l");
        feed(t, restore);
        feed(t, "\033[1m\033[4mX");
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    }
}

TEST(testGRCMReplacingKeepsProtectionOfThePen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[21l\033[1\"q\033[1m\033[4mX\033[?2K");
    CHECK_EQ(cell(t, 0, 0)->codePoint, static_cast<uint32_t>('X'));
}

TEST(testDECCARAChangesRectAttributes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "ABC");
    CHECK(!cell(t, 0, 0)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Underline }));
    // DECCARA over the first two columns: set bold (1) + underline (4).
    feed(t, "\033[1;1;1;2;1;4$r");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    // 'C' is outside the rect and untouched; codepoints are never altered.
    CHECK(!cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    // Clear bold only (22) — underline survives.
    feed(t, "\033[1;1;1;2;22$r");
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    // Ps 0 turns all four off.
    feed(t, "\033[1;1;1;2;0$r");
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(cell(t, 0, 0)->codePoint == 'A');
}

TEST(testSGRIndexedAndTruecolor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t,
        "\033[38;5;254mA\033[38;5;255mB"
        "\033[38;2;1;2;3mC\033[38;5;255mD");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 254);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == 255);
    CHECK(cell(t, 0, 2)->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 2)->pen.foregroundRed == 1);
    CHECK(cell(t, 0, 2)->pen.foregroundGreen == 2);
    CHECK(cell(t, 0, 2)->pen.foregroundBlue == 3);
    CHECK(cell(t, 0, 3)->pen.foregroundIndex == 255);
}

TEST(testSGRTruncatedExtcolorNoLeak)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t,
        "\033[38;5mA" // truncated fg palette -> no color, no blink leak
        "\033[38;2;1;2mB" // truncated fg truecolor -> no bold+dim leak
        "\033[48;5mC" // truncated bg palette -> no color, no leak
        "\033[38;5;200mD"); // well-formed still applies after a truncated one
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(!cell(t, 0, 1)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Dim }));
    CHECK(cell(t, 0, 2)->pen.backgroundIndex == Ubiquitty::colorDefault);
    CHECK(!cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 3)->pen.foregroundIndex == 200);
}

TEST(testSGRBasicForegroundColors3037)
{
    Ubiquitty::Terminal t;
    int n;
    char buffer[16];
    Ubiquitty::hardReset(t);
    for (n = 30; n <= 37; n++) {
        snprintf(buffer, sizeof(buffer), "\033[%dmX", n);
        feed(t, buffer);
    }
    // Eight cells emitted, each tagged with the appropriate index.
    for (n = 30; n <= 37; n++) {
        const Ubiquitty::Cell* c = cell(t, 0, n - 30);
        CHECK(c->codePoint == 'X');
        CHECK(c->pen.foregroundIndex == (uint16_t)(n - 30));
    }
}

TEST(testSGRBasicBackgroundColors4047)
{
    Ubiquitty::Terminal t;
    int n;
    char buffer[16];
    Ubiquitty::hardReset(t);
    for (n = 40; n <= 47; n++) {
        snprintf(buffer, sizeof(buffer), "\033[%dmX", n);
        feed(t, buffer);
    }
    for (n = 40; n <= 47; n++) {
        const Ubiquitty::Cell* c = cell(t, 0, n - 40);
        CHECK(c->codePoint == 'X');
        CHECK(c->pen.backgroundIndex == (uint16_t)(n - 40));
    }
}

TEST(testSGRBrightForegroundColors9097)
{
    Ubiquitty::Terminal t;
    int n;
    char buffer[16];
    Ubiquitty::hardReset(t);
    for (n = 90; n <= 97; n++) {
        snprintf(buffer, sizeof(buffer), "\033[%dmX", n);
        feed(t, buffer);
    }
    for (n = 90; n <= 97; n++) {
        const Ubiquitty::Cell* c = cell(t, 0, n - 90);
        CHECK(c->codePoint == 'X');
        CHECK(c->pen.foregroundIndex == (uint16_t)(n - 90 + 8));
    }
}

TEST(testSGRBrightBackgroundColors100107)
{
    Ubiquitty::Terminal t;
    int n;
    char buffer[16];
    Ubiquitty::hardReset(t);
    for (n = 100; n <= 107; n++) {
        snprintf(buffer, sizeof(buffer), "\033[%dmX", n);
        feed(t, buffer);
    }
    for (n = 100; n <= 107; n++) {
        const Ubiquitty::Cell* c = cell(t, 0, n - 100);
        CHECK(c->codePoint == 'X');
        CHECK(c->pen.backgroundIndex == (uint16_t)(n - 100 + 8));
    }
}

TEST(testSGRDefaultForegroundBackground3949)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033[31mR\033[39mD");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 1);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorDefault);
    // background untouched by the 39 reset.
    CHECK(cell(t, 0, 1)->pen.backgroundIndex == Ubiquitty::colorDefault);

    Ubiquitty::hardReset(t);
    feed(t, "\033[41mR\033[49mD");
    CHECK(cell(t, 0, 0)->pen.backgroundIndex == 1);
    CHECK(cell(t, 0, 1)->pen.backgroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorDefault);
}

TEST(testSGRCombinedCodes)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Cell* c;

    Ubiquitty::hardReset(t);
    feed(t, "\033[1;31;42mX");
    c = cell(t, 0, 0);
    CHECK(c->codePoint == 'X');
    CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(c->pen.foregroundIndex == 1);
    CHECK(c->pen.backgroundIndex == 2);

    /// Mixing basic + bright on the same CSI. The last selector for each
    /// channel wins.
    Ubiquitty::hardReset(t);
    feed(t, "\033[33;94;105mY");
    c = cell(t, 0, 0);
    CHECK(c->codePoint == 'Y');
    CHECK(c->pen.foregroundIndex == 94 - 90 + 8);
    CHECK(c->pen.backgroundIndex == 105 - 100 + 8);

    /// SGR 0 in a combined run resets everything before applying later
    /// selectors.
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;31m\033[0;42mZ");
    c = cell(t, 0, 0);
    CHECK(c->codePoint == 'Z');
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(c->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(c->pen.backgroundIndex == 2);
}

TEST(testRethemePreservesExplicitSGRColors)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\033[31mR\033[38;5;200mX\033[38;2;1;2;3mT");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == 1);
    CHECK(cell(t, 0, 2)->pen.foregroundIndex == 200);
    CHECK(cell(t, 0, 3)->pen.foregroundIndex == Ubiquitty::colorRGB);
    Ubiquitty::Themes::rethemeDefaults(&t);
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == 1);
    CHECK(cell(t, 0, 2)->pen.foregroundIndex == 200);
    CHECK(cell(t, 0, 3)->pen.foregroundIndex == Ubiquitty::colorRGB);
}

TEST(testColonTruecolor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[38:2:1:2:3mA\033[48:2::4:5:6mB");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 0)->pen.foregroundRed == 1);
    CHECK(cell(t, 0, 0)->pen.foregroundGreen == 2);
    CHECK(cell(t, 0, 0)->pen.foregroundBlue == 3);
    CHECK(cell(t, 0, 1)->pen.backgroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 1)->pen.backgroundRed == 4);
    CHECK(cell(t, 0, 1)->pen.backgroundGreen == 5);
    CHECK(cell(t, 0, 1)->pen.backgroundBlue == 6);

    /// Both shapes must fold through the same normalizer. After a colon
    /// truecolor SGR a subsequent SGR in the same sequence still applies
    /// cleanly. A parser that miscounts the colon group swallows the selector
    /// after it, and the loss is silent because the swallowed code is valid.
    Ubiquitty::hardReset(t);
    feed(t, "\033[38:2:10:20:30;1mX");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 0)->pen.foregroundRed == 10);
    CHECK(cell(t, 0, 0)->pen.foregroundGreen == 20);
    CHECK(cell(t, 0, 0)->pen.foregroundBlue == 30);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));

    /// Semicolon form whose R happens to be zero must not be misread as
    /// the colon `2::r:g:b` shape — a `parameters[i+2]==0` test
    /// would alias green/blue.
    Ubiquitty::hardReset(t);
    feed(t, "\033[38;2;0;7;9mY\033[1mZ");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 0)->pen.foregroundRed == 0);
    CHECK(cell(t, 0, 0)->pen.foregroundGreen == 7);
    CHECK(cell(t, 0, 0)->pen.foregroundBlue == 9);
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));

    /// 256-color colon form: \033[38:5:200m
    Ubiquitty::hardReset(t);
    feed(t, "\033[38:5:200mC");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 200);
}

TEST(testSGRAttributeSetAndReset)
{
    struct Pair {
        int setCode;
        int resetCode;
        Ubiquitty::VT::Attribute bit;
    };
    /// 21 and 22 both clear BOLD|DIM together (the parser folds the
    /// "double-underline" 21 onto the same reset arm). The two cases
    /// are exercised independently below.
    const Pair pairs[] = {
        { 1, 22, Ubiquitty::VT::Attribute::Bold },
        { 2, 22, Ubiquitty::VT::Attribute::Dim },
        { 3, 23, Ubiquitty::VT::Attribute::Italic },
        { 4, 24, Ubiquitty::VT::Attribute::Underline },
        { 5, 25, Ubiquitty::VT::Attribute::Blink },
        { 6, 25, Ubiquitty::VT::Attribute::Blink },
        { 7, 27, Ubiquitty::VT::Attribute::Reverse },
        { 8, 28, Ubiquitty::VT::Attribute::Hidden },
        { 9, 29, Ubiquitty::VT::Attribute::Strike },
    };
    Ubiquitty::Terminal t;
    char buffer[32];
    size_t i;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        Ubiquitty::hardReset(t);
        snprintf(buffer, sizeof(buffer), "\033[%dmA\033[%dmB", pairs[i].setCode, pairs[i].resetCode);
        feed(t, buffer);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(pairs[i].bit));
        CHECK(!cell(t, 0, 1)->pen.attributes.contains(pairs[i].bit));
    }
    /// SGR 21 is doubly-underlined (ECMA-48 / xterm), not an intensity
    /// reset: it leaves BOLD|DIM intact. SGR 22 clears BOLD|DIM.
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;2;21mA\033[22mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Dim));
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineDouble);
    CHECK(!cell(t, 0, 1)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Dim }));

    /// SGR 5 is slow blink (Attribute::Blink only). SGR 6 is fast blink — same
    /// Attribute::Blink bit plus the ExtendedAttribute::BlinkFast extension flag so the
    /// renderer can drive a half-period curve. SGR 25 clears both, and
    /// 5→6→5 toggling lands each variant cleanly.
    Ubiquitty::hardReset(t);
    feed(t, "\033[5mA\033[6mB\033[5mC\033[25mD");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(!cell(t, 0, 0)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 1)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(!cell(t, 0, 2)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    CHECK(!cell(t, 0, 3)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(!cell(t, 0, 3)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
}

TEST(testSGRUnderlineStyles)
{
    struct Case {
        const char* seq;
        uint8_t style;
    };
    const Case cases[] = {
        { "\033[4mA", Ubiquitty::VT::underlineSingle },
        { "\033[4:0mA", Ubiquitty::VT::underlineNone },
        { "\033[4:1mA", Ubiquitty::VT::underlineSingle },
        { "\033[4:2mA", Ubiquitty::VT::underlineDouble },
        { "\033[4:3mA", Ubiquitty::VT::underlineCurly },
        { "\033[4:4mA", Ubiquitty::VT::underlineDotted },
        { "\033[4:5mA", Ubiquitty::VT::underlineDashed },
        // Unknown style clamps to a drawn (single) underline.
        { "\033[4:9mA", Ubiquitty::VT::underlineSingle },
    };
    Ubiquitty::Terminal t;
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Ubiquitty::hardReset(t);
        feed(t, cases[i].seq);
        const Ubiquitty::Cell* c = cell(t, 0, 0);
        CHECK(c->codePoint == 'A');
        CHECK(c->pen.underlineStyle == cases[i].style);
        if (cases[i].style == Ubiquitty::VT::underlineNone)
            CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
        else
            CHECK(c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    }

    /// SGR 24 turns a curly underline back off (style and bit both).
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3mA\033[24mB");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineCurly);
    CHECK(cell(t, 0, 1)->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));

    /// A colon underline group does not swallow a following selector.
    Ubiquitty::hardReset(t);
    feed(t, "\033[4:2;1mX");
    CHECK(cell(t, 0, 0)->pen.underlineStyle == Ubiquitty::VT::underlineDouble);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
}

TEST(testSGRUnderlineColor)
{
    Ubiquitty::Terminal t;

    /// Indexed, both colon and semicolon forms.
    Ubiquitty::hardReset(t);
    feed(t, "\033[58:5:200mA\033[58;5;42mB");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == 200);
    CHECK(cell(t, 0, 1)->pen.underlineIndex == 42);

    /// Truecolor, colon (with empty colorspace slot) and semicolon.
    Ubiquitty::hardReset(t);
    feed(t, "\033[58:2::1:2:3mA\033[58;2;4;5;6mB");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 0)->pen.underlineRed == 1);
    CHECK(cell(t, 0, 0)->pen.underlineGreen == 2);
    CHECK(cell(t, 0, 0)->pen.underlineBlue == 3);
    CHECK(cell(t, 0, 1)->pen.underlineIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 1)->pen.underlineRed == 4);
    CHECK(cell(t, 0, 1)->pen.underlineGreen == 5);
    CHECK(cell(t, 0, 1)->pen.underlineBlue == 6);

    /// Default on init follows text color; 59 resets after a set.
    Ubiquitty::hardReset(t);
    feed(t, "\033[58:5:9mA\033[59mB");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == 9);
    CHECK(cell(t, 0, 1)->pen.underlineIndex == Ubiquitty::colorDefault);
}

TEST(testPenResetUnderlineDefault)
{
    Ubiquitty::Terminal t;

    /// Power-on default.
    Ubiquitty::hardReset(t);
    feed(t, "A");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == Ubiquitty::colorDefault);

    /// DECSTR (CSI ! p) resets the pen the same way.
    Ubiquitty::hardReset(t);
    feed(t, "\033[58:5:9m\033[!pA");
    CHECK(cell(t, 0, 0)->pen.underlineIndex == Ubiquitty::colorDefault);
}

TEST(testSGROverlineFramedEncircled)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033[53mA\033[55mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Overline));
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Overline));

    Ubiquitty::hardReset(t);
    feed(t, "\033[51;52mA\033[54mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Framed));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Encircled));
    CHECK(!cell(t, 0, 1)->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled }));

    /// SGR 0 clears the extended attribute bits + underline color.
    Ubiquitty::hardReset(t);
    feed(t, "\033[53;51;52;58:5:7;4:3m\033[0mB");
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(!c->pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Overline, Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled }));
    CHECK(!c->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(c->pen.underlineStyle == Ubiquitty::VT::underlineNone);
    CHECK(c->pen.underlineIndex == Ubiquitty::colorDefault);
}

TEST(testSGREmptyParametersResets)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;31;42mA\033[mB");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 1);
    CHECK(cell(t, 0, 0)->pen.backgroundIndex == 2);
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 1)->pen.backgroundIndex == Ubiquitty::colorDefault);
}

// A component past 255 leaves the colour as it was, and the direct colour's
// parameters are still consumed, so the code after them applies.
TEST(testSGRTruecolorComponentOutOfRangeIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[32m\033[38;2;256;0;0;3mA\033[48;2;0;999;0mB\033[38:2::1:2:300;1mC\033[38;2;1;2;300mD");
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 2);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(cell(t, 0, 1)->pen.backgroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 2)->pen.foregroundIndex == 2);
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 3)->pen.foregroundIndex == 2);
    feed(t, "\033[38;2;255;0;7mE");
    CHECK(cell(t, 0, 4)->pen.foregroundIndex == Ubiquitty::colorRGB);
    CHECK(cell(t, 0, 4)->pen.foregroundRed == 255);
    CHECK(cell(t, 0, 4)->pen.foregroundBlue == 7);
}

TEST(testSGRColonTruecolorTruncated)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "X\033[38:2:10:20mY");
    // Both cells keep the default foreground — the malformed SGR is a no-op.
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == Ubiquitty::colorDefault);
}

TEST(testSGRColonUnknownModeConsumesRun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1m\033[38:3:0:50:50mX");
    // Bold survives (no leaked SGR 0 reset); colour is unchanged.
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);

    Ubiquitty::hardReset(t);
    feed(t, "\033[38:5mY");
    // The 5 must not leak through as SGR 5 (blink).
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == Ubiquitty::colorDefault);
}

TEST(testRepRepeatsColoredPrecedingCharacter)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    feed(t, "\033[31;44mA\033[4b");
    for (i = 0; i < 5; i++) {
        CHECK(cell(t, 0, i)->codePoint == 'A');
        CHECK(cell(t, 0, i)->pen.foregroundIndex == 1);
        CHECK(cell(t, 0, i)->pen.backgroundIndex == 4);
    }
    CHECK(t.screens.active->cursorX == 5);
}

/// DECSC / DECRC — SGR pen (bold + italic + foreground=red).
TEST(testDECSCDECRCSavesPenAttributes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;3;31m"); // bold + italic + foreground=red(1)
    CHECK(t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(t.screens.active->pen.foregroundIndex == 1);
    feed(t, "\0337"); // DECSC
    feed(t, "\033[0m"); // reset SGR
    CHECK(!t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(!t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(t.screens.active->pen.foregroundIndex == Ubiquitty::colorDefault);
    feed(t, "\0338"); // DECRC
    CHECK(t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(t.screens.active->pen.foregroundIndex == 1);
}

// ---------------------------------------------------------------------------
// The colon-run rule, swept over the whole selector space.
//
// The two cases above name codes whose colon run leaks when nothing reads it. A
// named case only covers the selector someone thought of, and a rule written
// down correctly still misses a code whose `case` arm has no colon reader. The
// sweep asks the invariant of all of them, so the next selector added with no
// colon reader is covered before anyone notices it needs to be.

// The pen at (0,0) after one sequence, from a freshly reset terminal.
static Ubiquitty::Pen penAfterSequence(const char* sequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, sequence);
    return cell(t, 0, 0)->pen;
}

// Selectors that read their own colon sub-parameters, each paired with a
// spelling whose effect differs from the bare code.
//
// The pairing is what makes a stale entry fail. An exemption is a hole in the
// sweep, and deleting a selector leaves its entry behind — a hole held open
// for a code the switch does not have. The second test below spends the
// pairing: an exemption whose colon form matches its bare code covers nothing
// and fails.
struct ColonFormSelector {
    int selector;
    const char* colonForm;
    const char* bare;
};

static const ColonFormSelector colonFormSelectors[] = {
    { 1, "\033[1:3mX", "\033[1mX" }, // variable weight, orthogonal to bold
    { 4, "\033[4:3mX", "\033[4mX" }, // underline style
    { 38, "\033[38:5:196mX", "\033[38mX" }, // indexed foreground
    { 48, "\033[48:5:196mX", "\033[48mX" }, // indexed background
    { 58, "\033[58:5:196mX", "\033[58mX" }, // indexed underline colour
    // 76:0 and bare 76 are the same state (auto), so the probe has to be 76:1 —
    // a pairing chosen for symmetry rather than read from the handler would sit
    // here asserting that two spellings of one value differ, and fail.
    { 76, "\033[76:1mX", "\033[76mX" }, // forced emoji presentation
};

TEST(testNoSelectorLeaksItsColonRunIntoThePen)
{
    // Sub-parameters whose TOP-LEVEL meaning is unmistakable: if a run leaks,
    // the pen carries bold/dim/italic/underline/blink/reverse and the whole-pen
    // comparison sees it. A sub-parameter with no top-level effect would leak
    // just as much and prove nothing.
    const int subParameters[] = { 1, 2, 3, 4, 5, 7 };

    int swept = 0;
    for (int selector = 0; selector <= 127; selector++) {
        bool isExempt = false;
        for (const ColonFormSelector& exempt : colonFormSelectors)
            isExempt = isExempt || exempt.selector == selector;
        if (isExempt)
            continue;

        char bare[16];
        std::snprintf(bare, sizeof(bare), "\033[%dmX", selector);
        const Ubiquitty::Pen expected = penAfterSequence(bare);

        for (int sub : subParameters) {
            char withRun[24];
            std::snprintf(withRun, sizeof(withRun), "\033[%d:%dmX", selector, sub);
            const Ubiquitty::Pen actual = penAfterSequence(withRun);
            // Whole pen, not the attribute word: a leaked sub-parameter can land
            // in a colour index, the packed underline byte or the weight
            // subfield, and an assertion on attributes alone would pass while
            // the cell renders differently. Pen is trivially copyable with no
            // padding, which is what makes the comparison total.
            CHECK_MSG(std::memcmp(&expected, &actual, sizeof(Ubiquitty::Pen)) == 0, withRun + 1);
            swept++;
        }
    }

    // The sweep's own size. Every assertion is inside the loop, so a bound that
    // came out empty — an exemption list that grew to cover everything, a
    // selector range read as zero — would agree with nothing and report a pass.
    const int exemptCount = (int)(sizeof(colonFormSelectors) / sizeof(colonFormSelectors[0]));
    CHECK_EQ(swept, (128 - exemptCount) * 6);
}

TEST(testEveryColonFormExemptionStillDiffersFromItsBareCode)
{
    int checked = 0;
    for (const ColonFormSelector& exempt : colonFormSelectors) {
        const Ubiquitty::Pen bare = penAfterSequence(exempt.bare);
        const Ubiquitty::Pen colon = penAfterSequence(exempt.colonForm);
        // An exemption whose colon form matches its bare code is a selector
        // the sweep above skips for a reason that does not hold.
        CHECK_MSG(std::memcmp(&bare, &colon, sizeof(Ubiquitty::Pen)) != 0, exempt.colonForm + 1);
        checked++;
    }
    CHECK_EQ(checked, (int)(sizeof(colonFormSelectors) / sizeof(colonFormSelectors[0])));
}

// ---------------------------------------------------------------------------
// A selector takes its colon run with it, whether or not it is recognised.
//
// This is why ECMA-48 offers the colon form at all: a terminal that does not
// implement a selector can skip its sub-parameters, where the semicolon form
// would leave them to be read as further selectors. A parser that ignores the
// selector but not its run turns ESC[99:1m into bold — a pen write no program
// asked for, from a sequence it had every right to send.
//
// The rule is not about UNKNOWN codes. A selector with its own `case` arm and
// no colon reader is in exactly the same position — without the rule, ESC[52:2m
// sets Dim and ESC[53:1m sets Bold. Stated only in the `default:` arm, the rule
// misses every code with a `case`, which skips that arm by construction.

TEST(testASelectorTakesItsColonRunWithIt)
{
    struct Case {
        const char* sequence;
        std::uint16_t attributes;
        const char* what;
    };
    const Case cases[] = {
        { "\033[99:1mX", 0, "a one-parameter run" },
        { "\033[99:2mX", 0, "a different parameter, same shape" },
        { "\033[99:1:2:3mX", 0, "a run of any length" },
        // A SEMICOLON ends the group, so what follows is a parameter in its own
        // right and must still apply. This is the case that separates skipping
        // a colon run from skipping everything after an unknown selector.
        { "\033[99:1;1mX", (std::uint16_t)Ubiquitty::VT::Attribute::Bold, "a semicolon ends the run" },
        { "\033[99;1mX", (std::uint16_t)Ubiquitty::VT::Attribute::Bold, "no run at all" },
        // Codes the switch does know, with no colon reader of their own. Both
        // leaked into the pen, and neither could reach the arm stating the rule.
        { "\033[52:2mX", (std::uint16_t)Ubiquitty::VT::Attribute::Encircled, "a known code with no colon reader" },
        { "\033[53:1mX", (std::uint16_t)Ubiquitty::VT::Attribute::Overline, "the same, one code along" },
        { "\033[52:2;1mX", (std::uint16_t)((std::uint16_t)Ubiquitty::VT::Attribute::Encircled | (std::uint16_t)Ubiquitty::VT::Attribute::Bold), "a semicolon still ends a known code's run" },
    };
    for (const Case& one : cases) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, one.sequence);
        CHECK_MSG(cell(t, 0, 0)->pen.attributes.toRaw() == one.attributes, one.what);
        CHECK(cell(t, 0, 0)->codePoint == 'X');
    }
}

TEST(testKnownColonFormsStillConsumeTheirOwnRuns)
{
    // The arm above must not swallow a run belonging to a selector that does
    // implement one: every known colon form goes through its own helper, and a
    // fix aimed at the unknown arm is only correct if it leaves those alone.
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033[4:3mX");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));

    Ubiquitty::hardReset(t);
    feed(t, "\033[38:2::1:2:3mX");
    CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.foregroundIndex), static_cast<int>(Ubiquitty::colorRGB));

    Ubiquitty::hardReset(t);
    feed(t, "\033[1:7mX");
    CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.extendedAttributes.weight()), 7);

    // A colon run belongs to its parameter (ECMA-48 / ITU T.416), so a code
    // that does not read its own sub-parameters must still consume them. The
    // values chosen are the ones that would be written if the run leaked: 2 is
    // Dim, 1 is Bold, 3 is Italic. Asserting the attribute is absent is what
    // distinguishes "consumed" from "read as a further selector" — the two are
    // identical from outside until a sub-parameter happens to name an
    // attribute.
    Ubiquitty::hardReset(t);
    feed(t, "\033[52:2mX"); // encircled, sub-parameter 2 == Dim
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Dim));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Encircled));

    Ubiquitty::hardReset(t);
    feed(t, "\033[53:1mX"); // overline, sub-parameter 1 == Bold
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Overline));

    Ubiquitty::hardReset(t);
    feed(t, "\033[7:3mX"); // reverse, sub-parameter 3 == Italic
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
}

// ---------------------------------------------------------------------------
// The selectors the decorative-effect family left behind.
//
// A sequence does not stop existing when its effect does: a program still sends
// SGR 56 and 57, and 51 carries a colon run that nothing consumes. What
// the parser does with those is a contract nobody wrote, and the deletions that
// retired the effects took with them every case that would have noticed.
//
// The three survivors below share a byte or a code with something retired,
// which is why they are worth asserting: 51 keeps Framed while its :2
// sub-selector goes, 6 keeps BlinkFast as the one flag left in a byte whose
// other four bits vacated, and 1:n rides that same byte's high three.

TEST(testSGR51StillFramesWithNoSelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[51mX");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Framed));
    CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.extendedAttributes.flags.toRaw()), 0);
}

TEST(testRetiredFillSelectorsSelectNothing)
{
    // 56 and 57 have no meaning. Asserted without a colon run: what happens to
    // the run after an unrecognised selector is a separate contract, stated by
    // testASelectorTakesItsColonRunWithIt.
    for (const char* sequence : { "\033[56mX", "\033[57mX" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, sequence);
        CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.attributes.toRaw()), 0);
        CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.extendedAttributes.flags.toRaw()), 0);
        CHECK(cell(t, 0, 0)->codePoint == 'X');
    }
}

TEST(testSGR6BlinkFastSurvivesTheVacatedBits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6mX");
    CHECK(cell(t, 0, 0)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    feed(t, "\033[25mY"); // SGR 25 clears both blink rates
    CHECK(!cell(t, 0, 1)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
}

TEST(testVariableWeightRidesTheHighBitsUndisturbed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1:7mX");
    CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.extendedAttributes.weight()), 7);
    // Weight and the one surviving flag share the byte, so setting each must
    // leave the other alone — the disjointness the layout asserts at compile
    // time, observed through the parser that writes both.
    feed(t, "\033[6mY");
    CHECK_EQ(static_cast<int>(cell(t, 0, 1)->pen.extendedAttributes.weight()), 7);
    CHECK(cell(t, 0, 1)->pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
}

TEST(testReprintSameGlyphSameColorsRefreshesTheStyleBytes)
{
    // The write path's idempotency probe decides whether a same-char reprint
    // touches the cell at all. The pen bytes outside its comparison — the
    // weight subfield, the underline shape, the underline colour, the font —
    // must still reach the cell, or the probe's skip leaves the FIRST pen's
    // styling under the second print with nothing reporting it.
    struct Scenario {
        const char* first;
        const char* second;
    };
    const Scenario scenarios[] = {
        // SGR 1:3 writes only the weight subfield, so the reprint matches on
        // every compared field and differs in the one it must refresh.
        { "\033[1:3mA", "\033[0mA" },
        // Both carry Attribute::Underline; only the shape byte differs.
        { "\033[4:3mA", "\033[4:1mA" },
        // Both underline; the underline COLOUR index differs.
        { "\033[4m\033[58:5:196mA", "\033[4m\033[59mA" },
        // Alternate font 1 back to primary; the font byte differs.
        { "\033[11mA", "\033[10mA" },
    };
    for (const Scenario& scenario : scenarios) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, scenario.first);
        feed(t, "\033[H");
        feed(t, scenario.second);

        Ubiquitty::Terminal expected;
        Ubiquitty::hardReset(expected);
        feed(expected, scenario.second);
        const Ubiquitty::Pen& reprinted = cell(t, 0, 0)->pen;
        const Ubiquitty::Pen& fresh = cell(expected, 0, 0)->pen;
        CHECK_EQ(static_cast<int>(reprinted.extendedAttributes.weight()),
            static_cast<int>(fresh.extendedAttributes.weight()));
        CHECK_EQ(static_cast<int>(reprinted.underlineStyle), static_cast<int>(fresh.underlineStyle));
        CHECK_EQ(static_cast<int>(reprinted.underlineIndex), static_cast<int>(fresh.underlineIndex));
        CHECK_EQ(static_cast<int>(reprinted.font), static_cast<int>(fresh.font));
    }
}

TEST(testNoSGRSequenceSetsAVacatedExtendedBit)
{
    // Bits 1-4 hold the text-size spec id and the font byte's low bits hold
    // the band, and no SGR arm may write either: the OSC 66 producer is their
    // only writer. A parser arm that set one would stamp a sizing fact from a
    // styling selector, and nothing else looks — the bytes are compared whole,
    // so a stray bit reads as a pen difference with no name on it.
    int swept = 0;
    for (int code = 0; code <= 75; ++code) {
        for (const char* suffix : { "", ":1", ":2", ":4", ":7" }) {
            char sequence[32];
            std::snprintf(sequence, sizeof(sequence), "\033[%d%smX", code, suffix);
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            feed(t, sequence);
            CHECK_EQ(static_cast<int>(cell(t, 0, 0)->pen.extendedAttributes.textSizeSpecID()), 0);
            CHECK_EQ(static_cast<int>(Ubiquitty::penTextBand(&cell(t, 0, 0)->pen)), 0);
            ++swept;
        }
    }
    // The sweep's own size: every assertion is inside the loop, so a bound that
    // came out empty would agree with nothing and report a pass.
    CHECK_EQ(swept, 76 * 5);
}

// XTPUSHSGR saves the pen; XTPOPSGR restores only the attributes the push
// named, so an attribute set between them and not named survives the pop.
// Frames pop in reverse order, and a pop with nothing pushed changes nothing.
TEST(testXTPUSHSGRAndXTPOPSGRRestoreTheNamedAttributes)
{
    using Ubiquitty::VT::Attribute;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;4;7m\033[#{\033[0mA\033[#}B");
    CHECK(cell(t, 0, 0)->pen.attributes.toRaw() == 0);
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Attribute::Bold));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Attribute::Underline));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Attribute::Reverse));

    feed(t, "\033[0;1;4;31m\033[1;30#{\033[0;9m\033[#}C");
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Attribute::Bold));
    CHECK(!cell(t, 0, 2)->pen.attributes.contains(Attribute::Underline));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Attribute::Strike));
    CHECK(cell(t, 0, 2)->pen.foregroundIndex == 1);

    feed(t, "\033[0;1m\033[#p\033[0;4m\033[#{\033[0m\033[#}D\033[#qE\033[#}F");
    CHECK(cell(t, 0, 3)->pen.attributes.contains(Attribute::Underline));
    CHECK(!cell(t, 0, 3)->pen.attributes.contains(Attribute::Bold));
    CHECK(cell(t, 0, 4)->pen.attributes.contains(Attribute::Bold));
    CHECK(!cell(t, 0, 4)->pen.attributes.contains(Attribute::Underline));
    CHECK(cell(t, 0, 5)->pen.attributes.contains(Attribute::Bold));

    // An explicit 0 names nothing, so its pop restores nothing.
    feed(t, "\033[0;1m\033[0#{\033[0m\033[#}G");
    CHECK(cell(t, 0, 6)->pen.attributes.toRaw() == 0);
}

// XTREPORTSGR answers the rendition every cell of the rectangle shares, as an
// SGR: an attribute or colour some cell lacks is left out.
TEST(testXTREPORTSGRReportsTheCommonRendition)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[128] = {};
    feed(t, "\033[1;4mA\033[24mB\033[1;1;1;2#|");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[0;1m") == 0);
    feed(t, "\033[1;1;1;1#|");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[0;1;4m") == 0);
    feed(t, "\033[0;48;2;1;2;3mC\033[1;3;1;3#|");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[0;48:2::1:2:3m") == 0);
    feed(t, "\033[2;1;2;5#|");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[0m") == 0);
}

namespace {

bool hasColors(const Ubiquitty::Cell* cell, uint16_t foreground, uint16_t background)
{
    return cell && Ubiquitty::cellPen(*cell).foregroundIndex == foreground && Ubiquitty::cellPen(*cell).backgroundIndex == background;
}

} // namespace

// VT520 DECAC: "The normal text colors are used in ANSI SGR color mode as the
// default foreground and background colors." Text written after it takes them,
// and SGR 0, 39 and 49 return to them.
TEST(testDECACAssignsTheNormalTextColors)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\033[1;1;2,|B\033[33;44mC\033[39mD\033[49mE\033[0mF");
    CHECK(hasColors(cell(t, 0, 1), 1, 2));
    CHECK(hasColors(cell(t, 0, 2), 3, 4));
    CHECK(hasColors(cell(t, 0, 3), 1, 4));
    CHECK(hasColors(cell(t, 0, 4), 1, 2));
    CHECK(hasColors(cell(t, 0, 5), 1, 2));

    // A later assignment replaces the earlier one in the pen and in the text.
    feed(t, "\033[1;5;6,|G");
    CHECK(hasColors(cell(t, 0, 6), 5, 6));
    CHECK(hasColors(cell(t, 0, 1), 5, 6));
    CHECK(hasColors(cell(t, 0, 2), 3, 4));
    CHECK(hasColors(cell(t, 0, 3), 5, 4));

    // DECSTR resets the rendition to the assigned colours; RIS forgets them.
    feed(t, "\033[31m\033[!pH");
    CHECK(hasColors(cell(t, 0, 7), 5, 6));
    feed(t, "\033cI");
    CHECK(hasColors(cell(t, 0, 0), Ubiquitty::colorDefault, Ubiquitty::colorDefault));
}

// The window frame item is stored and reported; it does not touch the text.
// An undefined item, a colour past 15 or a parameter count other than three
// leaves everything as it was. None of them disturbs a pending wrap.
TEST(testDECACIgnoresWhatItDoesNotDefine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[2;3;4,|\033[0;1;2,|\033[99;1;2,|\033[1;16;2,|\033[1;2,|\033[1;2;3;4,|A");
    CHECK(hasColors(cell(t, 0, 0), Ubiquitty::colorDefault, Ubiquitty::colorDefault));
    feed(t, "BCD\033[1;1;2,|E");
    CHECK(cell(t, 1, 0)->codePoint == 'E');
    CHECK(hasColors(cell(t, 1, 0), 1, 2));
}

// VT520 DECAC: the normal text colours are the default colours, so text
// already written in the default colours takes them too, on either screen, and
// so do the blank positions. A colour selected explicitly stays.
TEST(testDECACRecolorsTextInTheDefaultColors)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 4);
    feed(t, "\033[?1049hX\033[?1049l");
    feed(t, "\r\nA\033[33mB\033[44mC\033[0m\r\n\r\nD\033[1;1;2,|E");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(hasColors(cell(t, 0, 0), 1, 2));
    CHECK(hasColors(cell(t, 0, 1), 3, 2));
    CHECK(hasColors(cell(t, 0, 2), 3, 4));
    CHECK(hasColors(cell(t, 0, 3), 1, 2));
    CHECK(hasColors(cell(t, 1, 0), 1, 2));
    CHECK(cell(t, 2, 0)->codePoint == 'D');
    CHECK(hasColors(cell(t, 2, 0), 1, 2));
    CHECK(hasColors(cell(t, 2, 1), 1, 2));
    CHECK(hasColors(cell(t, 2, 3), 1, 2));
    feed(t, "\033[?47h");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(hasColors(cell(t, 0, 0), 1, 2));
    CHECK(hasColors(cell(t, 2, 3), 1, 2));
}

// VT520 DECAC: the normal text colours stand in for the default colours only,
// so a later assignment leaves a colour selected explicitly, in the text and in
// the pen, even one with the index the earlier assignment gave.
TEST(testDECACKeepsAnExplicitColorEqualToTheAssignment)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1;2,|\033[31mR\033[0;42mB\033[0;31m\033[1;5;6,|X");
    CHECK(hasColors(cell(t, 0, 0), 1, 6));
    CHECK(hasColors(cell(t, 0, 1), 5, 2));
    CHECK(hasColors(cell(t, 0, 2), 1, 6));

    // SGR selecting the assigned index makes it explicit; SGR 0 returns to the
    // assignment, and the rendition stack keeps which colours were the default.
    feed(t, "\033[0;35mY\033[0m\033[#{\033[1;3;4,|\033[#}Z");
    CHECK(hasColors(cell(t, 0, 3), 5, 4));
    CHECK(hasColors(cell(t, 0, 4), 3, 4));
}

// VT520 DECAC: "DECAC can be reported using DECRQSS and DECRPSS. Use Ps1 in
// DECRQSS to select the item to be reported." An item never assigned has no
// index to report.
TEST(testDECRQSSReportsTheDECACItems)
{
    Ubiquitty::Terminal t;
    char buffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033P$q1,|\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\") == 0);
    feed(t, "\033[1;1;2,|\033[2;3;4,|\033P$q1,|\033\\\033P$q2,|\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P1$r1;1;2,|\033\\\033P1$r2;3;4,|\033\\") == 0);
    feed(t, "\033P$q,|\033\\\033P$q3,|\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\\033P0$r\033\\") == 0);
}

// VT520 DECATC: "DECATC can be reported using DECRQSS and DECRPSS. Use Ps1 in
// DECRQSS to select the attribute combination to be reported." Each of the 16
// combinations is held once assigned; one never assigned has no index to
// report. The text keeps its SGR colours: the VT520 draws from the table only
// under its alternate colour look-up table, which Core does not offer.
TEST(testDECATCHoldsAndReportsTheCombinations)
{
    Ubiquitty::Terminal t;
    char buffer[96];
    Ubiquitty::hardReset(t);
    feed(t, "\033P$q1,}\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\") == 0);
    feed(t, "\033[0;7;0,}\033[1;15;4,}\033[15;9;1,}A");
    feed(t, "\033P$q0,}\033\\\033P$q1,}\033\\\033P$q15,}\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P1$r0;7;0,}\033\\\033P1$r1;15;4,}\033\\\033P1$r15;9;1,}\033\\") == 0);
    CHECK(hasColors(cell(t, 0, 0), Ubiquitty::colorDefault, Ubiquitty::colorDefault));

    // A combination past 15, a colour past 15, a parameter count other than
    // three, or no combination in the request, is ignored or refused.
    feed(t, "\033[16;1;2,}\033[2;16;2,}\033[2;1,}\033[2;1;2;3,}");
    feed(t, "\033P$q2,}\033\\\033P$q,}\033\\\033P$q16,}\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\\033P0$r\033\\\033P0$r\033\\") == 0);

    // RIS forgets the table.
    feed(t, "\033c\033P$q1,}\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\") == 0);
}

// The DECATC table is held out of line: a terminal holds none until a DECATC
// assigns a combination, and an ignored DECATC does not make one.
TEST(testDECATCHoldsNoTableUntilACombinationIsAssigned)
{
    Ubiquitty::Terminal t;
    char buffer[96];
    Ubiquitty::hardReset(t);
    CHECK(!t.alternateTextColors);
    feed(t, "\033[16;1;2,}\033[2;16;2,}\033[2;1,}\033P$q2,}\033\\");
    drain(t, buffer);
    CHECK(strcmp(buffer, "\033P0$r\033\\") == 0);
    CHECK(!t.alternateTextColors);
    feed(t, "\033[2;1;3,}");
    REQUIRE(t.alternateTextColors);
    CHECK(Ubiquitty::alternateTextColorTable(t)[2] == (Ubiquitty::ColorAssignment { 1, 3 }));
    CHECK(!Ubiquitty::isColorAssigned(Ubiquitty::alternateTextColorTable(t)[0]));
}

UBIQUITTY_TEST_MAIN()
