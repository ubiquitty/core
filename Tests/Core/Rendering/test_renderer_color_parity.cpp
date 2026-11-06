// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_renderer_color_parity — pins the macOS Metal renderer's color resolver
// (CellColor.cpp CellPalette::resolve, 8-bit + alpha, Preferences-keyed)
// byte-for-byte against the portable resolver (Rendering/ColorResolve.cpp
// resolveCell, keyed on the Core 16-bit TermPalette) that the shared
// buildCellInstances drives. macOS and the portable builder must resolve
// every (pen, palette) to the same final foreground/background, or a macOS
// frame drawn through the shared builder shifts colors. The proof rests on
// invariants this test locks in
// so a later edit to either resolver, the xterm cube, or the channel width
// conversion can't silently break parity:
//   1. channel16To8(channel8To16(v)) == v exactly for all v in 0..255, so a
//      TermPalette seeded by widening the same 8-bit source round-trips back to
//      CellPalette's 8-bit values on read.
//   2. CellPalette::seedDefaults / fillExtended and TermPalette::seedDefaults /
//      fillExtended fill indices 0..255 from the same xterm 16 + 6x6x6 cube +
//      grayscale formulas.
//   3. CellPalette::resolve and resolveCell apply bold-as-bright / reverse /
//      hidden / dim in the same order with the same arithmetic.
//   4. Per-color foreground ALPHA (an independent compositing formula vs
//      CellInstanceBuilder's BuildInputs.paletteForegroundAlpha path) select
//      the same alpha source (default / indexed / opaque) and fold in the
//      same SGR 5/6 blink curve the same way — two independent
//      implementations by architecture (Rendering/ColorResolve.h: "macOS's
//      per-color opacity ... stays backend-specific"), so this is a parity
//      proof, not a shared-function guarantee.
//   5. DECSCNM (whole-screen reverse video): swapping each palette's DEFAULT
//      foreground/background slot produces the same resolved color for a
//      colorDefault pen on both sides, and leaves an explicit-SGR-color pen
//      completely unaffected on both sides — distinct from invariant 3's
//      reverseAlso (a per-cell XOR applied inside resolve to every pen
//      regardless of color source; DECSCNM only ever touches the default
//      slots, before resolve runs).
// Blink-curve computation itself and the SGR 56:1 gradient run are not
// re-tested here: both call the exact same shared function on both sides
// (Core/Cursor/Blink.h blinkAlpha, Rendering/CellDecisions.h applyGradientRun)
// — a single shared implementation can't diverge between callers.

#include "StrictAtlas.h"

#include "Core/CellColor.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/ColorResolve.h"

#include "Core/Cursor/Blink.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

// Invariant 1: the 8 -> 16 -> 8 channel round-trip is exact, so a TermPalette
// built by widening CellPalette's 8-bit values reads back identical bytes.
TEST(testChannelRoundTrip)
{
    for (int v = 0; v <= 255; ++v) {
        const uint16_t wide = channel8To16(static_cast<uint8_t>(v));
        CHECK(channel16To8(wide) == v);
    }
}

// Invariant 2: both palettes fill all 256 indices identically (8-bit direct vs
// 16-bit widened), so resolve has the same source of truth on each side.
static void testPaletteSeedParity(const Ubiquitty::CellPalette& cp, const TermPalette& tp)
{
    for (int i = 0; i < 256; ++i) {
        CHECK(channel16To8(tp.indexed[i].red) == cp.indexed[i].red);
        CHECK(channel16To8(tp.indexed[i].green) == cp.indexed[i].green);
        CHECK(channel16To8(tp.indexed[i].blue) == cp.indexed[i].blue);
    }
    CHECK(channel16To8(tp.defaultForeground.red) == cp.defaultForeground.red);
    CHECK(channel16To8(tp.defaultForeground.green) == cp.defaultForeground.green);
    CHECK(channel16To8(tp.defaultForeground.blue) == cp.defaultForeground.blue);
    CHECK(channel16To8(tp.defaultBackground.red) == cp.defaultBackground.red);
    CHECK(channel16To8(tp.defaultBackground.green) == cp.defaultBackground.green);
    CHECK(channel16To8(tp.defaultBackground.blue) == cp.defaultBackground.blue);
}

// Invariant 3: the two resolvers agree on the final foreground/background for a
// given pen + brightenBold + reverseAlso.
static void checkPen(const Ubiquitty::CellPalette& cp, const TermPalette& tp, const Ubiquitty::Pen& pen, bool shouldBrightenBold, Ubiquitty::ReverseMode reverseAlso) // bool-ok: shouldBrightenBold mirrors CellPalette::brightenBold, the field under test
{
    uint8_t mfg[3], mbg[3]; // macOS CellPalette side
    Ubiquitty::CellPalette cpb = cp;
    cpb.brightenBold = shouldBrightenBold;
    cpb.resolve(pen, reverseAlso, mfg, mbg);

    const ResolvedCell portable = resolveCell(pen, tp, reverseAlso == Ubiquitty::ReverseMode::Reversed, shouldBrightenBold);

    CHECK(portable.foreground.red == mfg[0] && portable.foreground.green == mfg[1] && portable.foreground.blue == mfg[2]);
    CHECK(portable.background.red == mbg[0] && portable.background.green == mbg[1] && portable.background.blue == mbg[2]);
    if (portable.foreground.red != mfg[0] || portable.foreground.green != mfg[1] || portable.foreground.blue != mfg[2] || portable.background.red != mbg[0] || portable.background.green != mbg[1] || portable.background.blue != mbg[2]) {
        printf("  pen fg=%u bg=%u attributes=0x%x bb=%d rev=%d -> "
               "macOS(%u,%u,%u / %u,%u,%u) portable(%u,%u,%u / %u,%u,%u)\n",
            pen.foregroundIndex, pen.backgroundIndex, pen.attributes.toRaw(), shouldBrightenBold, reverseAlso == Ubiquitty::ReverseMode::Reversed, mfg[0], mfg[1], mfg[2], mbg[0], mbg[1], mbg[2], portable.foreground.red, portable.foreground.green, portable.foreground.blue, portable.background.red, portable.background.green,
            portable.background.blue);
    }
}

static Ubiquitty::Pen makePen(uint16_t attributes, uint16_t foregroundIndex, uint16_t backgroundIndex)
{
    Ubiquitty::Pen p {};
    p.attributes = Ubiquitty::VT::AttributeSet::fromRaw(attributes);
    p.foregroundIndex = foregroundIndex;
    p.backgroundIndex = backgroundIndex;
    p.underlineIndex = colorDefault;
    return p;
}

// The alpha fold reads no glyph, so this models none. Deriving from StrictAtlas
// means a build that started asking it for one aborts rather than being handed a
// plausible answer to a question this case never intended to pose.
struct AlphaAtlas : UbiquittyTest::StrictAtlas {
    void beginFrame() { }
    Ubiquitty::Rendering::MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, Ubiquitty::Rendering::FontFace, std::uint32_t, int) { return true; }
    Ubiquitty::Rendering::AtlasGlyph monoGlyph(std::uint32_t, Ubiquitty::Rendering::FontFace, std::uint32_t, std::uint32_t) { return {}; }
    Ubiquitty::Rendering::AtlasGlyph wideGlyph(std::uint32_t, Ubiquitty::Rendering::FontFace) { return {}; }
    Ubiquitty::Rendering::AtlasGlyph doubleGlyph(std::uint32_t, Ubiquitty::Rendering::FontFace, Ubiquitty::Rendering::DoubleRegion) { return {}; }
};

// Invariant 4: per-color foreground alpha. macOS folds `_palette.indexed[fi].alpha`
// / `_palette.defaultForeground.alpha` (a colorRGB foreground stays opaque)
// times the SGR 5/6 blink curve into inst.foreground.w. The portable builder folds `BuildInputs.paletteForegroundAlpha[fi]`
// / `defaultForegroundAlpha` times the same blink curve into foreground.w
// Both read the same formula over differently-shaped storage (macOS:
// ColorRGBA.alpha inside CellPalette; portable: a bare uint8_t[256] + a
// scalar) — this proves the formula, not the storage, is what must stay
// identical.
//
// The portable side runs the BUILDER rather than a restatement of it. A
// comparison whose two sides are both written here proves the two expressions
// in this file agree and says nothing about the code that ships: the alpha fold
// could change in CellInstanceBuilder and every case below would still pass.
// The macOS side stays modelled because Core cannot link the frontend, so this
// pins production against a model rather than a model against a model.
static void checkForegroundAlpha(uint16_t foregroundIndex, uint8_t defaultAlpha, uint8_t indexedAlpha, bool blinking, bool fastBlink, uint32_t nowTicks)
{
    // macOS side: a CellPalette carrying the given alpha bytes.
    Ubiquitty::CellPalette cp {};
    cp.seedDefaults();
    cp.defaultForeground.alpha = defaultAlpha;
    cp.indexed[foregroundIndex < 256 ? foregroundIndex : 0].alpha = indexedAlpha;
    float macAlpha = 1.0f;
    if (foregroundIndex == colorDefault)
        macAlpha = cp.defaultForeground.alpha / 255.0f;
    else if (foregroundIndex < 256)
        macAlpha = cp.indexed[foregroundIndex].alpha / 255.0f;

    // macOS folds the blink curve in itself; the builder does it below, inside
    // the code under test. Applied here so the modelled side matches what the
    // frontend does rather than what this file finds convenient.
    if (blinking) {
        const uint32_t period = fastBlink ? (Cursor::Blink::caretPeriodTicks / 2) : Cursor::Blink::caretPeriodTicks;
        macAlpha *= blinkAlpha(nowTicks, period);
    }

    // portable side: one cell through buildCellInstances, reading the alpha it
    // actually emitted.
    uint8_t paletteAlpha[256];
    for (auto& a : paletteAlpha)
        a = indexedAlpha;
    Ubiquitty::TermPalette tp;
    tp.seedDefaults();
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 0x41u; // inked: a blank cell emits no foreground to read
    Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
    pen.foregroundIndex = foregroundIndex;
    pen.backgroundIndex = colorDefault;
    pen.underlineIndex = colorDefault;
    if (blinking) {
        pen.attributes.add(Ubiquitty::VT::Attribute::Blink);
        if (fastBlink)
            pen.extendedAttributes.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    }
    Ubiquitty::cellSetPen(cell, pen);

    AlphaAtlas atlas;
    Ubiquitty::Rendering::BuildInputs in;
    in.cells = &cell;
    in.rows = 1;
    in.columns = 1;
    in.palette = &tp;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.paletteForegroundAlpha = paletteAlpha;
    in.defaultForegroundAlpha = defaultAlpha;
    in.resolveBlink = blinking;
    in.blinkTicks = nowTicks;
    Ubiquitty::Rendering::CellInstance cellsOut[1] {};
    Ubiquitty::Rendering::InstanceBuffers buffers;
    buffers.cells = cellsOut;
    buffers.cellCapacity = 1;
    const Ubiquitty::Rendering::InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(counts.cells == 1, "the builder emitted the cell whose alpha is being read");
    const float portableAlpha = cellsOut[0].foreground.w;

    CHECK(macAlpha == portableAlpha);
    if (macAlpha != portableAlpha) {
        printf("  fg=%u defaultAlpha=%u indexedAlpha=%u blinking=%d fast=%d ticks=%u -> "
               "macOS=%f portable=%f\n",
            foregroundIndex, defaultAlpha, indexedAlpha, blinking, fastBlink, nowTicks, macAlpha, portableAlpha);
    }
}

TEST(testForegroundAlphaParity)
{
    const uint16_t indices[] = { colorDefault, 0, 8, 244, colorRGB };
    const uint8_t alphas[] = { 0, 1, 128, 254, 255 };
    for (uint16_t fi : indices)
        for (uint8_t defaultAlpha : alphas)
            for (uint8_t indexedAlpha : alphas)
                for (bool blinking : { false, true })
                    for (bool fastBlink : { false, true })
                        for (uint32_t ticks : { 0u, 15u, 29u, 30u, 59u })
                            checkForegroundAlpha(fi, defaultAlpha, indexedAlpha, blinking, fastBlink, ticks);
}

// Invariant 5: DECSCNM swaps only the palette's DEFAULT foreground/background
// slots — distinct from invariant 3's reverseAlso (a per-cell XOR applied
// inside resolve to every pen). A colorDefault pen must pick up the swap on
// both sides; a colorRGB (explicit) pen must be completely unaffected.
TEST(testDECSCNMParity)
{
    Ubiquitty::CellPalette cp {};
    cp.seedDefaults();
    TermPalette tp;
    tp.seedDefaults();
    testPaletteSeedParity(cp, tp); // preconditions: both sides start in lockstep

    // Apply DECSCNM exactly as each renderer does — swap the frame-local
    // copy's default slots, leave the live palette untouched.
    Ubiquitty::CellPalette cpReversed = cp;
    Ubiquitty::ColorRGBA swap = cpReversed.defaultForeground;
    cpReversed.defaultForeground = cpReversed.defaultBackground;
    cpReversed.defaultBackground = swap;

    TermPalette tpReversed = tp;
    PaletteRGB swapPortable = tpReversed.defaultForeground;
    tpReversed.defaultForeground = tpReversed.defaultBackground;
    tpReversed.defaultBackground = swapPortable;

    // A colorDefault pen: resolved foreground/background must equal the
    // ORIGINAL default background/foreground (the swap), on both sides.
    Ubiquitty::Pen defaultPen = makePen(0, colorDefault, colorDefault);
    uint8_t mfg[3], mbg[3];
    cpReversed.resolve(defaultPen, Ubiquitty::ReverseMode::Normal, mfg, mbg);
    const ResolvedCell portableDefault = resolveCell(defaultPen, tpReversed, false, false);
    CHECK(mfg[0] == cp.defaultBackground.red && mfg[1] == cp.defaultBackground.green && mfg[2] == cp.defaultBackground.blue);
    CHECK(portableDefault.foreground.red == channel16To8(tp.defaultBackground.red) && portableDefault.foreground.green == channel16To8(tp.defaultBackground.green) && portableDefault.foreground.blue == channel16To8(tp.defaultBackground.blue));
    CHECK(mfg[0] == portableDefault.foreground.red && mfg[1] == portableDefault.foreground.green && mfg[2] == portableDefault.foreground.blue);
    CHECK(mbg[0] == portableDefault.background.red && mbg[1] == portableDefault.background.green && mbg[2] == portableDefault.background.blue);

    // An explicit-color (colorRGB) pen: the swap must not touch it at all —
    // resolved output must match the UN-reversed palette on both sides.
    Ubiquitty::Pen explicitRGBPen = makePen(0, colorRGB, colorRGB);
    explicitRGBPen.foregroundRed = 10;
    explicitRGBPen.foregroundGreen = 120;
    explicitRGBPen.foregroundBlue = 230;
    explicitRGBPen.backgroundRed = 200;
    explicitRGBPen.backgroundGreen = 40;
    explicitRGBPen.backgroundBlue = 90;
    uint8_t rfg[3], rbg[3], ufg[3], ubg[3];
    cpReversed.resolve(explicitRGBPen, Ubiquitty::ReverseMode::Normal, rfg, rbg);
    cp.resolve(explicitRGBPen, Ubiquitty::ReverseMode::Normal, ufg, ubg);
    const ResolvedCell portableRGBReversed = resolveCell(explicitRGBPen, tpReversed, false, false);
    const ResolvedCell portableRGBUnreversed = resolveCell(explicitRGBPen, tp, false, false);
    CHECK(rfg[0] == ufg[0] && rfg[1] == ufg[1] && rfg[2] == ufg[2]);
    CHECK(rbg[0] == ubg[0] && rbg[1] == ubg[1] && rbg[2] == ubg[2]);
    CHECK(portableRGBReversed.foreground.red == portableRGBUnreversed.foreground.red && portableRGBReversed.foreground.green == portableRGBUnreversed.foreground.green && portableRGBReversed.foreground.blue == portableRGBUnreversed.foreground.blue);
    CHECK(rfg[0] == portableRGBReversed.foreground.red && rfg[1] == portableRGBReversed.foreground.green && rfg[2] == portableRGBReversed.foreground.blue);
    CHECK(rbg[0] == portableRGBReversed.background.red && rbg[1] == portableRGBReversed.background.green && rbg[2] == portableRGBReversed.background.blue);
}

// The seeded palettes agree before any pen is resolved through them: a
// difference here would make every comparison below a comparison of two
// different palettes rather than of two resolvers.
TEST(testSeededPalettesMatchAcrossResolvers)
{
    Ubiquitty::CellPalette cp {};
    cp.seedDefaults();
    TermPalette tp;
    tp.seedDefaults();
    testPaletteSeedParity(cp, tp);
}

// macOS CellPalette::resolve against portable resolveCell over every pen and
// attribute combination that reaches the resolve path.
TEST(testResolversAgreeOnEveryPenAndAttributeCombination)
{
    Ubiquitty::CellPalette cp {};
    cp.seedDefaults();
    TermPalette tp;
    tp.seedDefaults();

    // Representative palette indices spanning every resolve branch: default, the
    // ANSI 16, a cube color, a grayscale-ramp color, and true-color (colorRGB).
    const uint16_t indices[] = { colorDefault, 0, 1, 7, 8, 15, 16, 124, 231, 232, 244, 255, colorRGB };
    // Every attribute combination that touches the resolve path.
    const uint16_t attrSets[] = {
        0,
        static_cast<uint16_t>(Ubiquitty::VT::Attribute::Bold),
        static_cast<uint16_t>(Ubiquitty::VT::Attribute::Dim),
        static_cast<uint16_t>(Ubiquitty::VT::Attribute::Reverse),
        static_cast<uint16_t>(Ubiquitty::VT::Attribute::Hidden),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Bold) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Dim)),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Bold) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Reverse)),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Reverse) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Dim)),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Reverse) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Hidden)),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Hidden) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Dim)),
        static_cast<uint16_t>(static_cast<uint16_t>(Ubiquitty::VT::Attribute::Bold) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Reverse) | static_cast<uint16_t>(Ubiquitty::VT::Attribute::Dim)),
    };

    int compared = 0;
    for (uint16_t fg : indices) {
        for (uint16_t bg : indices) {
            for (uint16_t attributes : attrSets) {
                Ubiquitty::Pen pen = makePen(attributes, fg, bg);
                if (fg == colorRGB) {
                    pen.foregroundRed = 10;
                    pen.foregroundGreen = 120;
                    pen.foregroundBlue = 230;
                }
                if (bg == colorRGB) {
                    pen.backgroundRed = 200;
                    pen.backgroundGreen = 40;
                    pen.backgroundBlue = 90;
                }
                for (bool shouldBrightenBold : { false, true }) // bool-ok: mirrors CellPalette::brightenBold, the field this parity test varies
                    for (const Ubiquitty::ReverseMode reverseAlso : { Ubiquitty::ReverseMode::Normal, Ubiquitty::ReverseMode::Reversed }) {
                        checkPen(cp, tp, pen, shouldBrightenBold, reverseAlso);
                        ++compared;
                    }
            }
        }
    }
    // The sweep's own size. Every assertion here lives inside checkPen, so a
    // loop bound that came out empty would agree with the other resolver about
    // nothing at all and report a pass.
    const int expected = 13 * 13 * 11 * 2 * 2;
    CHECK_EQ(compared, expected);
}
UBIQUITTY_TEST_MAIN()
