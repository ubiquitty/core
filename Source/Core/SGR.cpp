// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SGR (Select Graphic Rendition) parameter folder. Public entry: Ubiquitty::SGR::apply (SGR.h).
//
// Painter contract for the extended attributes stored here:
// - Underline shape: Pen::underlineStyle (underlineNone..DASHED). Attribute::Underline stays set for any
//   non-zero style, so a bit-only painter still draws a plain underline.
// - Underline color: Pen::underlineIndex (colorDefault = text color, colorRGB = underlineRed/Green/Blue,
//   0..255 = palette index; mirrors fg/bg layout).
// - Overline/Framed/Encircled: the matching Attribute bit.
// - SGR 76 (force emoji presentation): the emit path resolves it to the cell's Attribute::Emoji bit +
//   width, so a painter reads Attribute::Emoji with no special case.

#include "Core/SGR.h"
#include "Core/Terminal.h"
#include <cstdint>
#include <cstring>
#include <utility>

// A direct colour's components run from 0 to 255; one outside that range
// leaves the colour as it was, its parameters still consumed (xterm ctlseqs:
// "RGB values range from zero (0) to 255"). Clamping it instead would paint a
// colour the program never named.
static bool areColorComponents(int red, int green, int blue)
{
    return red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255;
}

// Parse one extended-color spec starting at parameters[i] (the 38/48 selector). *iInOut returns the last
// consumed parameter. Handles semicolon form (38;5;n, 38;2;r;g;b) and T.416 colon form (38:5:n, 38:2:r:g:b,
// 38:2::r:g:b), distinguished by parameterSeparatorWasColon[]. *outIndex left untouched ⇒ no pen effect
// (malformed).
static void parseExtendedColor(Ubiquitty::Terminal& t, int* iInOut, uint16_t* outIndex, uint8_t* outRed, uint8_t* outGreen, uint8_t* outBlue)
{
    const int i = *iInOut;

    if (i + 1 >= t.parser.parameterCount)
        return;
    const bool isColonForm = t.parser.parameterSeparatorWasColon[i + 1];
    const int mode = t.parser.parameters[i + 1];

    if (isColonForm) {
        // Walk the colon-grouped sub-parameter run; k-1 is the sub-item count (selector + data).
        int k = 2;
        while (i + k < t.parser.parameterCount && t.parser.parameterSeparatorWasColon[i + k])
            k++;
        if (mode == 5 && k >= 3) {
            const int index = t.parser.parameters[i + 2];
            if (index >= 0 && index <= 255)
                *outIndex = static_cast<uint16_t>(index);
        } else if (mode == 2 && k >= 5) {
            // k == 5: `2:r:g:b` (no colorspace-id slot).
            // k >= 6: `2:cs:r:g:b[:tol:cstype]` — the cs slot is
            // commonly empty (parses as 0) per ITU T.416.
            const int redIndex = (k >= 6) ? i + 3 : i + 2;
            const int red = t.parser.parameters[redIndex];
            const int green = t.parser.parameters[redIndex + 1];
            const int blue = t.parser.parameters[redIndex + 2];
            if (areColorComponents(red, green, blue)) {
                *outIndex = Ubiquitty::colorRGB;
                *outRed = static_cast<uint8_t>(red);
                *outGreen = static_cast<uint8_t>(green);
                *outBlue = static_cast<uint8_t>(blue);
            }
        }
        // Consume the whole self-delimiting run whatever the mode: leaving a truncated/unknown run
        // (38:5, 38:3:c:m:y) unconsumed would feed its sub-parameters back as standalone SGR codes.
        // (Store sits after the loads: iInOut may alias parameters, so storing first would force reloads.)
        *iInOut = i + (k - 1);
        return;
    }

    // Classic semicolon form. The introducer claims its whole span (2 for `;5`, 4 for `;2`), so a truncated
    // form applies no color but is still consumed — else the leftovers feed back as standalone codes.
    if (mode == 5 && i + 2 < t.parser.parameterCount) {
        const int index = t.parser.parameters[i + 2];
        if (index >= 0 && index <= 255)
            *outIndex = static_cast<uint16_t>(index);
    } else if (mode == 2 && i + 4 < t.parser.parameterCount) {
        const int red = t.parser.parameters[i + 2];
        const int green = t.parser.parameters[i + 3];
        const int blue = t.parser.parameters[i + 4];
        if (areColorComponents(red, green, blue)) {
            *outIndex = Ubiquitty::colorRGB;
            *outRed = static_cast<uint8_t>(red);
            *outGreen = static_cast<uint8_t>(green);
            *outBlue = static_cast<uint8_t>(blue);
        }
    }
    // Consume the span (clamped to the last present parameter) whether or not color applied. mode 5 spans
    // i+2, mode 2 i+4, else i+1. Store after the loads (iInOut may alias parameters).
    const int lastConsumed = (mode == 5) ? (i + 2) : (mode == 2) ? (i + 4)
                                                                 : (i + 1);
    *iInOut = (lastConsumed < t.parser.parameterCount) ? lastConsumed : (t.parser.parameterCount - 1);
}

// Set the underline style and keep Attribute::Underline in sync, so a bit-only painter still draws some
// underline while a style-aware one reads Pen::underlineStyle.
static void setUnderlineStyle(Ubiquitty::Pen* p, uint8_t style)
{
    // Write only the shape's low 3 bits; the script state shares this byte's high bits and must survive.
    Ubiquitty::penSetUnderlineShape(p, style);
    if (style == Ubiquitty::VT::underlineNone)
        p->attributes.remove(Ubiquitty::VT::Attribute::Underline);
    else
        p->attributes.add(Ubiquitty::VT::Attribute::Underline);
}

// SGR 4 underline selector. Bare `4` = single, `24` = none, `4:0`..`4:5` = style. Styles above DASHED
// collapse to SINGLE: an unrecognised style must still draw an underline, not none.
static void applyUnderlineSelector(Ubiquitty::Terminal& t, Ubiquitty::Pen* p, int* iInOut)
{
    int i = *iInOut;
    if (i + 1 < t.parser.parameterCount && t.parser.parameterSeparatorWasColon[i + 1]) {
        int style = t.parser.parameters[i + 1];
        if (style < Ubiquitty::VT::underlineNone || style > Ubiquitty::VT::underlineDashed)
            style = Ubiquitty::VT::underlineSingle;
        setUnderlineStyle(p, static_cast<uint8_t>(style));
        *iInOut = i + 1;
        return;
    }
    setUnderlineStyle(p, Ubiquitty::VT::underlineSingle);
}

// SGR 1 selector. Bare `1` = bold. `1:n` (n=1..7) selects a variable-font weight in extendedAttributes
// (orthogonal to bold; a non-variable renderer falls back to the nearest face). SGR 22/0 clear it.
static void applyWeightSelector(Ubiquitty::Terminal& t, Ubiquitty::Pen* p, int* iInOut)
{
    int i = *iInOut;
    if (i + 1 < t.parser.parameterCount && t.parser.parameterSeparatorWasColon[i + 1]) {
        int w = t.parser.parameters[i + 1];
        if (w < 0)
            w = 0;
        if (w > 7)
            w = 7;
        p->extendedAttributes.setWeight(w);
        *iInOut = i + 1;
        return;
    }
    p->attributes.add(Ubiquitty::VT::Attribute::Bold);
}

// SGR 76 — force emoji presentation on the run, overriding the codePoint default and any VS-15/16 that
// follows. ForceEmojiPresentation means "honor the Attribute::Emoji bit literally, skip auto-detection":
//   76:1 → force color (set force bit + Emoji); 76:2 → force text (set force bit, clear Emoji);
//   76:0 / bare 76 / SGR 0 → auto (clear force bit; Emoji left to emit).
static void applyEmojiPresentationSelector(Ubiquitty::Terminal& t, Ubiquitty::Pen* p, int* iInOut)
{
    int i = *iInOut;
    int mode = 0;
    if (i + 1 < t.parser.parameterCount && t.parser.parameterSeparatorWasColon[i + 1]) {
        mode = t.parser.parameters[i + 1];
        *iInOut = i + 1;
    }
    if (mode == 1) {
        p->attributes.add(Ubiquitty::VT::Attribute::ForceEmojiPresentation);
        p->attributes.add(Ubiquitty::VT::Attribute::Emoji);
    } else if (mode == 2) {
        p->attributes.add(Ubiquitty::VT::Attribute::ForceEmojiPresentation);
        p->attributes.remove(Ubiquitty::VT::Attribute::Emoji);
    } else {
        // 76:0 / bare 76 — back to auto-detected presentation.
        p->attributes.remove({ Ubiquitty::VT::Attribute::ForceEmojiPresentation, Ubiquitty::VT::Attribute::Emoji });
    }
}

// SGR 60–64 ideogram attribute → packed enum in Pen::underlineStyle; SGR 65/0 clear. ECMA-48 resets 60–64
// together with 65, so they map onto one mutually-exclusive enum.
static inline uint8_t ideogramForCode(int n)
{
    switch (n) {
    case 60:
        return Ubiquitty::VT::ideogramUnderline;
    case 61:
        return Ubiquitty::VT::ideogramDoubleUnderline;
    case 62:
        return Ubiquitty::VT::ideogramOverline;
    case 63:
        return Ubiquitty::VT::ideogramDoubleOverline;
    case 64:
        return Ubiquitty::VT::ideogramStress;
    default:
        return Ubiquitty::VT::ideogramNone; // 65
    }
}

// Combined extended-color fast path for the per-cell pair block-art and attribute-churn floods emit:
// `38;5;A;48;5;B` (either order, pc==6) and the same pair behind one leading directive (pc==7). Indexes are
// per-cell distinct, so nothing is memoized across calls. Exact shapes only, indexes 0..255, separators
// uniform within each triple; the separator JOINING the groups (and after the directive) must be a
// semicolon (a colon would make the next code a sub-parameter). Every check passes before the first pen
// store, so a near-miss falls through untouched.
[[gnu::noinline]] static bool applyCombinedIndexedPair(Ubiquitty::Terminal& t, Ubiquitty::Pen* p, int pc)
{
    const int lead = (pc == 7) ? t.parser.parameters[0] : -1;
    // Leading-directive allowlist: bit-set adds + reset only. Colon-sub-style codes (1 weight, 4 underline)
    // are safe in bare form because the semicolon follower is enforced.
    const bool isLeadDirectiveAllowed = (pc == 6) || ((lead >= 0 && lead <= 9 && lead != 6) && !t.parser.parameterSeparatorWasColon[1]);
    if (!isLeadDirectiveAllowed)
        return false;
    const int g = (pc == 7) ? 1 : 0; // first color group's introducer slot
    const int intro0 = t.parser.parameters[g];
    const int intro1 = t.parser.parameters[g + 3];
    if ((intro0 != 38 && intro0 != 48) || intro1 != (intro0 == 38 ? 48 : 38)
        || t.parser.parameters[g + 1] != 5 || t.parser.parameters[g + 4] != 5
        || t.parser.parameterSeparatorWasColon[g + 3]
        || t.parser.parameterSeparatorWasColon[g + 1] != t.parser.parameterSeparatorWasColon[g + 2]
        || t.parser.parameterSeparatorWasColon[g + 4] != t.parser.parameterSeparatorWasColon[g + 5])
        return false;
    const int index0 = t.parser.parameters[g + 2];
    const int index1 = t.parser.parameters[g + 5];
    if (index0 < 0 || index0 > 255 || index1 < 0 || index1 > 255)
        return false;
    switch (lead) {
    case -1:
        break;
    case 0:
        Ubiquitty::penResetRendition(p);
        break;
    case 1:
        p->attributes.add(Ubiquitty::VT::Attribute::Bold);
        break;
    case 2:
        p->attributes.add(Ubiquitty::VT::Attribute::Dim);
        break;
    case 3:
        p->attributes.add(Ubiquitty::VT::Attribute::Italic);
        break;
    case 4:
        setUnderlineStyle(p, Ubiquitty::VT::underlineSingle);
        break;
    case 5:
        p->attributes.add(Ubiquitty::VT::Attribute::Blink);
        p->extendedAttributes.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
        break;
    case 7:
        p->attributes.add(Ubiquitty::VT::Attribute::Reverse);
        break;
    case 8:
        p->attributes.add(Ubiquitty::VT::Attribute::Hidden);
        break;
    case 9:
        p->attributes.add(Ubiquitty::VT::Attribute::Strike);
        break;
    }
    uint16_t* first = (intro0 == 38) ? &p->foregroundIndex : &p->backgroundIndex;
    uint16_t* second = (intro0 == 38) ? &p->backgroundIndex : &p->foregroundIndex;
    *first = static_cast<uint16_t>(index0);
    *second = static_cast<uint16_t>(index1);
    return true;
}

namespace Ubiquitty::SGR {

void apply(Ubiquitty::Terminal& t)
{
    Screen::State* s = t.screens.active;
    Pen* p = &s->pen;
    const int pc = t.parser.parameterCount;
    if (pc == 0) {
        penResetRendition(p);
        return;
    }
    // Fast-path the two dominant STANDALONE extended-color SGRs — 48|38;5;N
    // (indexed) and 48|38;2;r;g;b (truecolor) — which otherwise walk the
    // general loop + the parseExtendedColor sub-parser on
    // every cell. Both separator spellings are taken, but only UNIFORMLY:
    // for these exact shapes the colon forms (38:5:N, 38:2:r:g:b) mean the
    // same thing as the semicolon forms, while a mixed run changes meaning
    // (38:2:r:g;b is a truncated colon group plus a standalone code), so
    // mixed separators — like an extra directive, an underline-color (58),
    // a colorspace-id slot (pc != 5), or an out-of-range index — fall
    // through to the unchanged general loop, keeping behaviour identical.
    // Mirrors parseExtendedColor's semantics. Scoped to extended-color forms only.
    const int n0 = t.parser.parameters[0];
    // Both arms below need three parameters or five, so the count is tested
    // before the spelling is read rather than inside them: at pc == 1 slot 1
    // sits at or above parameterCount, and what it holds is whatever the clear
    // left rather than anything this sequence said. The value is dead there
    // today, which is exactly the shape a later arm would silently consume.
    if ((n0 == 38 || n0 == 48) && pc >= 3) {
        const uint8_t colonSpelling = t.parser.parameterSeparatorWasColon[1]; // parser writes strict 0/1
        if (pc == 3 && t.parser.parameters[1] == 5 && t.parser.parameterSeparatorWasColon[2] == colonSpelling) {
            const int index = t.parser.parameters[2];
            if (index >= 0 && index <= 255) {
                if (n0 == 48)
                    p->backgroundIndex = static_cast<uint16_t>(index);
                else
                    p->foregroundIndex = static_cast<uint16_t>(index);
                return;
            }
        } else if (pc == 5 && t.parser.parameters[1] == 2 && t.parser.parameterSeparatorWasColon[2] == colonSpelling && t.parser.parameterSeparatorWasColon[3] == colonSpelling && t.parser.parameterSeparatorWasColon[4] == colonSpelling && areColorComponents(t.parser.parameters[2], t.parser.parameters[3], t.parser.parameters[4])) {
            const auto r = static_cast<uint8_t>(t.parser.parameters[2]);
            const auto g = static_cast<uint8_t>(t.parser.parameters[3]);
            const auto b = static_cast<uint8_t>(t.parser.parameters[4]);
            if (n0 == 48) {
                p->backgroundIndex = colorRGB;
                p->backgroundRed = r;
                p->backgroundGreen = g;
                p->backgroundBlue = b;
            } else {
                p->foregroundIndex = colorRGB;
                p->foregroundRed = r;
                p->foregroundGreen = g;
                p->foregroundBlue = b;
            }
            return;
        }
    }
    // Combined extended-color fast path, in a NOINLINE helper so it does not grow apply() around the
    // standalone fast path above.
    if ((pc == 6 || pc == 7) && applyCombinedIndexedPair(t, p, pc))
        return;
    // General loop: one flat switch per parameter (jump-table dispatch). AttributeSet 0..9/21..29/52..55 set
    // or clear bits; 1/4/51/56 route to selector helpers so their colon sub-parameters are inspected;
    // colors 30..49/58/59/90..107 as noted below. Extended forms advance `i` over consumed parameter cells.
    int i = 0;
    while (i < t.parser.parameterCount) {
        const int n = t.parser.parameters[i];
        // The extent of this parameter's colon run, read before dispatch so the
        // arm that handles the code does not get to decide whether its
        // sub-parameters are data or instructions.
        int runEnd = i;
        while (runEnd + 1 < t.parser.parameterCount && t.parser.parameterSeparatorWasColon[runEnd + 1])
            runEnd++;
        switch (n) {
        case 0:
            penResetRendition(p);
            break;
        case 1:
            applyWeightSelector(t, p, &i);
            break;
        case 2:
            p->attributes.add(Ubiquitty::VT::Attribute::Dim);
            break;
        case 3:
            p->attributes.add(Ubiquitty::VT::Attribute::Italic);
            break;
        case 4:
            applyUnderlineSelector(t, p, &i);
            break;
        case 5:
            p->attributes.add(Ubiquitty::VT::Attribute::Blink);
            p->extendedAttributes.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
            break;
        case 6:
            p->attributes.add(Ubiquitty::VT::Attribute::Blink);
            p->extendedAttributes.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
            break;
        case 7:
            p->attributes.add(Ubiquitty::VT::Attribute::Reverse);
            break;
        case 8:
            p->attributes.add(Ubiquitty::VT::Attribute::Hidden);
            break;
        case 9:
            p->attributes.add(Ubiquitty::VT::Attribute::Strike);
            break;
        case 10:
            // Primary (default) font; also clears Fraktur (20).
            penSetFontStyle(p, Ubiquitty::VT::fontPrimary);
            break;
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19:
            // Alternate font n-10. One font rendered, so every alt slot folds to fontAlternate; the exact
            // slot is not stored (no consumer needs it).
            penSetFontStyle(p, Ubiquitty::VT::fontAlternate);
            break;
        case 20:
            // Fraktur (blackletter); carried as a distinct font state.
            penSetFontStyle(p, Ubiquitty::VT::fontFraktur);
            break;
        case 21:
            // Doubly underlined. SGR 22 (not 21) is the bold+dim reset, so 21 is not an intensity control.
            setUnderlineStyle(p, Ubiquitty::VT::underlineDouble);
            break;
        case 22:
            p->attributes.remove({ Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Dim });
            p->extendedAttributes.setWeight(0); // also clears 1:n variable weight
            break;
        case 23:
            // Not italicized, not Fraktur. Clears italic and resets a Fraktur (20) selection to primary;
            // an alternate-font selection (11–19) is left untouched.
            p->attributes.remove(Ubiquitty::VT::Attribute::Italic);
            if (penFontStyle(p) == Ubiquitty::VT::fontFraktur)
                penSetFontStyle(p, Ubiquitty::VT::fontPrimary);
            break;
        case 24:
            setUnderlineStyle(p, Ubiquitty::VT::underlineNone);
            break;
        case 25:
            p->attributes.remove(Ubiquitty::VT::Attribute::Blink);
            p->extendedAttributes.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
            break;
        case 26:
            // Proportional spacing (ITU T.416). Reserved — parsed and carried, no render; SGR 50/0 clear it.
            penSetProportional(p, true);
            break;
        case 27:
            p->attributes.remove(Ubiquitty::VT::Attribute::Reverse);
            break;
        case 28:
            p->attributes.remove(Ubiquitty::VT::Attribute::Hidden);
            break;
        case 29:
            p->attributes.remove(Ubiquitty::VT::Attribute::Strike);
            break;
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            p->foregroundIndex = static_cast<uint16_t>(n - 30);
            break;
        case 38:
            parseExtendedColor(t, &i, &p->foregroundIndex, &p->foregroundRed, &p->foregroundGreen, &p->foregroundBlue);
            break;
        case 39:
            p->foregroundIndex = colorDefault;
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            p->backgroundIndex = static_cast<uint16_t>(n - 40);
            break;
        case 48:
            parseExtendedColor(t, &i, &p->backgroundIndex, &p->backgroundRed, &p->backgroundGreen, &p->backgroundBlue);
            break;
        case 49:
            p->backgroundIndex = colorDefault;
            break;
        case 50:
            // Disable proportional spacing — the SGR 26 reset (ITU T.416).
            penSetProportional(p, false);
            break;
        case 51:
            p->attributes.add(Ubiquitty::VT::Attribute::Framed);
            break;
        case 52:
            p->attributes.add(Ubiquitty::VT::Attribute::Encircled);
            break;
        case 53:
            p->attributes.add(Ubiquitty::VT::Attribute::Overline);
            break;
        case 54:
            p->attributes.remove({ Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled });
            break;
        case 55:
            p->attributes.remove(Ubiquitty::VT::Attribute::Overline);
            break;
        case 58: {
            // Underline color — same 5:n / 2:r:g:b grammar as 38/48.
            uint16_t index = p->underlineIndex;
            parseExtendedColor(t, &i, &index, &p->underlineRed, &p->underlineGreen, &p->underlineBlue);
            p->underlineIndex = index;
            break;
        }
        case 59:
            p->underlineIndex = colorDefault; // underline follows text color
            break;
        case 60:
        case 61:
        case 62:
        case 63:
        case 64:
        case 65:
            // Ideogram attributes (60–64; 65 resets), one mutually-exclusive enum in underlineStyle's high bits.
            penSetIdeogram(p, ideogramForCode(n));
            break;
        case 73:
            // Superscript. Mutually exclusive with subscript; penSetScript overwrites the 2-bit field.
            penSetScript(p, Ubiquitty::VT::scriptSuper);
            break;
        case 74:
            penSetScript(p, Ubiquitty::VT::scriptSub); // subscript
            break;
        case 75:
            penSetScript(p, Ubiquitty::VT::scriptNone); // neither — clear super and sub
            break;
        case 76:
            // Emoji presentation forcing (76:1 color / 76:2 mono / 76:0 auto).
            applyEmojiPresentationSelector(t, p, &i);
            break;
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            p->foregroundIndex = static_cast<uint16_t>(n - 90 + 8);
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            p->backgroundIndex = static_cast<uint16_t>(n - 100 + 8);
            break;
        default:
            // Unknown SGR — ignored, along with its colon run (consumed below).
            break;
        }
        // Sub-parameters belong to their parameter (ECMA-48 / ITU T.416), so a
        // code that did not read its own run still consumes it. Without this,
        // every `case` lacking a private consumer writes the pen from bytes it
        // does not implement: ESC[52:2m set Dim and ESC[53:1m set Bold, both
        // measured. `max` rather than assignment, so a selector that consumed
        // part of a run is not rewound.
        if (i < runEnd)
            i = runEnd;
        i++;
    }
}

// XTPUSHSGR's parameters, each naming the attributes a pop restores (xterm
// ptyx.h, psBOLD .. psBG_COLOR). Bit Ps - 1 of a frame's mask.
namespace {
enum class PushedRendition : uint32_t {
    Bold = 1u << (1 - 1),
    Faint = 1u << (2 - 1),
    Italic = 1u << (3 - 1),
    Underline = 1u << (4 - 1),
    Blink = 1u << (5 - 1),
    Inverse = 1u << (7 - 1),
    Invisible = 1u << (8 - 1),
    CrossedOut = 1u << (9 - 1),
    DoubleUnderline = 1u << (21 - 1),
    Foreground = 1u << (30 - 1),
    Background = 1u << (31 - 1),
};

constexpr bool names(uint32_t mask, PushedRendition rendition) { return (mask & std::to_underlying(rendition)) != 0; }

void restoreAttribute(Ubiquitty::Pen& pen, const Ubiquitty::Pen& saved, Ubiquitty::VT::Attribute attribute)
{
    if (saved.attributes.contains(attribute))
        pen.attributes.add(attribute);
    else
        pen.attributes.remove(attribute);
}

// Core holds one underline with a shape; xterm holds single and double as two
// flags. Ps 4 governs the underline unless it is double, Ps 21 the double one.
bool isDoubleUnderline(const Ubiquitty::Pen& pen) { return pen.attributes.contains(Ubiquitty::VT::Attribute::Underline) && Ubiquitty::penUnderlineShape(&pen) == Ubiquitty::VT::underlineDouble; }
bool isSingleUnderline(const Ubiquitty::Pen& pen) { return pen.attributes.contains(Ubiquitty::VT::Attribute::Underline) && !isDoubleUnderline(pen); }

void restoreUnderline(Ubiquitty::Pen& pen, const Ubiquitty::Pen& saved, bool (*kind)(const Ubiquitty::Pen&))
{
    if (kind(saved)) {
        pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
        Ubiquitty::penSetUnderlineShape(&pen, Ubiquitty::penUnderlineShape(&saved));
    } else if (kind(pen)) {
        pen.attributes.remove(Ubiquitty::VT::Attribute::Underline);
        Ubiquitty::penSetUnderlineShape(&pen, Ubiquitty::VT::underlineNone);
    }
}
} // namespace

void push(Ubiquitty::Terminal& t, uint32_t mask)
{
    if (t.savedRenditionCount < Ubiquitty::Terminal::savedRenditionCapacity) {
        t.savedRenditionMasks[t.savedRenditionCount] = mask;
        t.savedRenditionPens[t.savedRenditionCount] = t.screens.active->pen;
    }
    if (t.savedRenditionCount < UINT16_MAX)
        t.savedRenditionCount++;
}

void pop(Ubiquitty::Terminal& t)
{
    if (t.savedRenditionCount == 0)
        return;
    t.savedRenditionCount--;
    if (t.savedRenditionCount >= Ubiquitty::Terminal::savedRenditionCapacity)
        return;
    const uint32_t mask = t.savedRenditionMasks[t.savedRenditionCount];
    const Pen& saved = t.savedRenditionPens[t.savedRenditionCount];
    Pen& pen = t.screens.active->pen;
    using Ubiquitty::VT::Attribute;
    if (names(mask, PushedRendition::Bold))
        restoreAttribute(pen, saved, Attribute::Bold);
    if (names(mask, PushedRendition::Faint))
        restoreAttribute(pen, saved, Attribute::Dim);
    if (names(mask, PushedRendition::Italic))
        restoreAttribute(pen, saved, Attribute::Italic);
    if (names(mask, PushedRendition::Underline))
        restoreUnderline(pen, saved, isSingleUnderline);
    if (names(mask, PushedRendition::DoubleUnderline))
        restoreUnderline(pen, saved, isDoubleUnderline);
    if (names(mask, PushedRendition::Blink)) {
        restoreAttribute(pen, saved, Attribute::Blink);
        if (saved.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast))
            pen.extendedAttributes.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
        else
            pen.extendedAttributes.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    }
    if (names(mask, PushedRendition::Inverse))
        restoreAttribute(pen, saved, Attribute::Reverse);
    if (names(mask, PushedRendition::Invisible))
        restoreAttribute(pen, saved, Attribute::Hidden);
    if (names(mask, PushedRendition::CrossedOut))
        restoreAttribute(pen, saved, Attribute::Strike);
    if (names(mask, PushedRendition::Foreground)) {
        pen.foregroundIndex = saved.foregroundIndex;
        pen.isForegroundAssignedDefault = saved.isForegroundAssignedDefault;
        pen.foregroundRed = saved.foregroundRed;
        pen.foregroundGreen = saved.foregroundGreen;
        pen.foregroundBlue = saved.foregroundBlue;
    }
    if (names(mask, PushedRendition::Background)) {
        pen.backgroundIndex = saved.backgroundIndex;
        pen.isBackgroundAssignedDefault = saved.isBackgroundAssignedDefault;
        pen.backgroundRed = saved.backgroundRed;
        pen.backgroundGreen = saved.backgroundGreen;
        pen.backgroundBlue = saved.backgroundBlue;
    }
}

void writeParameters(const Pen& pen, Ubiquitty::ReplyBuilder& output)
{
    output.put("0");
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Bold))
        output.put(";1");
    // The underline's shape reports as the parameter that sets it, so the
    // report restores the pen it describes: double as 21, as xterm spells
    // it, and the shapes past it in the colon form only SGR 4 has.
    const bool isDoublyUnderlined = pen.attributes.contains(Ubiquitty::VT::Attribute::Underline) && penUnderlineShape(&pen) == Ubiquitty::VT::underlineDouble;
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Underline) && !isDoublyUnderlined) {
        switch (penUnderlineShape(&pen)) {
        case Ubiquitty::VT::underlineCurly:
        case Ubiquitty::VT::underlineDotted:
        case Ubiquitty::VT::underlineDashed:
            output.put(";4:");
            output.putDecimal(static_cast<int>(penUnderlineShape(&pen)));
            break;
        default:
            output.put(";4");
            break;
        }
    }
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
        // SGR 6 (fast blink) reports back as 6; plain 5 otherwise.
        const int code = pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast) ? 6 : 5;
        output.put(";");
        output.putDecimal(static_cast<int>(static_cast<unsigned>(code)));
    }
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse))
        output.put(";7");
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden))
        output.put(";8");
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Dim))
        output.put(";2");
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Italic))
        output.put(";3");
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Strike))
        output.put(";9");
    if (isDoublyUnderlined)
        output.put(";21");
    if (pen.foregroundIndex == Ubiquitty::colorRGB) {
        output.put(";38:2::");
        output.putDecimal(static_cast<int>(pen.foregroundRed));
        output.put(":");
        output.putDecimal(static_cast<int>(pen.foregroundGreen));
        output.put(":");
        output.putDecimal(static_cast<int>(pen.foregroundBlue));
    } else if (pen.foregroundIndex != Ubiquitty::colorDefault && pen.foregroundIndex <= 7) {
        output.put(";");
        output.putDecimal(static_cast<int>(static_cast<unsigned>(30 + pen.foregroundIndex)));
    } else if (pen.foregroundIndex >= 8 && pen.foregroundIndex <= 15) {
        output.put(";");
        output.putDecimal(static_cast<int>(static_cast<unsigned>(90 + (pen.foregroundIndex - 8))));
    } else if (pen.foregroundIndex != Ubiquitty::colorDefault) {
        output.put(";38:5:");
        output.putDecimal(static_cast<int>(pen.foregroundIndex));
    }
    if (pen.backgroundIndex == Ubiquitty::colorRGB) {
        output.put(";48:2::");
        output.putDecimal(static_cast<int>(pen.backgroundRed));
        output.put(":");
        output.putDecimal(static_cast<int>(pen.backgroundGreen));
        output.put(":");
        output.putDecimal(static_cast<int>(pen.backgroundBlue));
    } else if (pen.backgroundIndex != Ubiquitty::colorDefault && pen.backgroundIndex <= 7) {
        output.put(";");
        output.putDecimal(static_cast<int>(static_cast<unsigned>(40 + pen.backgroundIndex)));
    } else if (pen.backgroundIndex >= 8 && pen.backgroundIndex <= 15) {
        output.put(";");
        output.putDecimal(static_cast<int>(static_cast<unsigned>(100 + (pen.backgroundIndex - 8))));
    } else if (pen.backgroundIndex != Ubiquitty::colorDefault) {
        output.put(";48:5:");
        output.putDecimal(static_cast<int>(pen.backgroundIndex));
    }
}

} // namespace Ubiquitty::SGR
