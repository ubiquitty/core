// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC colour slice: palette, special, and dynamic colours (OSC 4/5/10–19/104/
// 105/110–119). Holds the colour-spec parser (X11 forms via XParseColor.h, plus
// the hsl: extension) and the palette-slot write/invalidate helpers; Dispatch.cpp
// keeps the dispatch table, and OSC 22 is PointerShape.cpp's.

#include "Core/Codec/Base64.h"
#include "Core/OSC/Internal.h"
#include "Core/OSC/XParseColor.h"
#include "Core/ReplyBuilder.h"
#include "Core/Tek/Colors.h"
#include "PAL/PAL.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace Ubiquitty::OSC {

namespace {

// `OSC <code> ; rgb:RRRR/GGGG/BBBB` and the request's terminator, the reply
// every colour query takes.
Ubiquitty::ReplyBuilder colorReply(const Ubiquitty::Terminal& t, int code, std::uint16_t red, std::uint16_t green, std::uint16_t blue)
{
    ReplyBuilder reply;
    reply.put("\033]");
    reply.putDecimal(code);
    reply.putColor(red, green, blue);
    reply.put(Ubiquitty::oscReplyTerminator(t));
    return reply;
}

} // namespace

// Parse a single hex digit. Returns 0..15 or -1 on a non-hex byte.
int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

// Standard HSL → 16-bit RGB. Hue degrees (0=red, 120=green, 240=blue), S/L percent. Integer chroma/X math
// from the Sixel DEC-HLS converter minus the DEC hue rotation (OSC's hsl: uses the CSS/X11 hue axis).
static void hslToRGB16(int hueDegrees, int saturationPercent, int lightnessPercent, uint16_t* outR, uint16_t* outG, uint16_t* outB)
{
    int h = hueDegrees;
    while (h < 0)
        h += 360;
    h %= 360;
    int s = saturationPercent < 0 ? 0 : (saturationPercent > 100 ? 100 : saturationPercent);
    int l = lightnessPercent < 0 ? 0 : (lightnessPercent > 100 ? 100 : lightnessPercent);

    int twoLm100 = (2 * l) - 100;
    int absTwoLm100 = twoLm100 < 0 ? -twoLm100 : twoLm100;
    int chroma = ((100 - absTwoLm100) * s) / 100; // 0..100

    int sector = h / 60; // 0..5
    int hMod120 = h % 120;
    int dist = hMod120 - 60;
    if (dist < 0)
        dist = -dist;
    int xValue = (chroma * (60 - dist)) / 60; // 0..100

    int r1 = 0, g1 = 0, b1 = 0;
    switch (sector) {
    case 0:
        r1 = chroma, g1 = xValue, b1 = 0;
        break;
    case 1:
        r1 = xValue, g1 = chroma, b1 = 0;
        break;
    case 2:
        r1 = 0, g1 = chroma, b1 = xValue;
        break;
    case 3:
        r1 = 0, g1 = xValue, b1 = chroma;
        break;
    case 4:
        r1 = xValue, g1 = 0, b1 = chroma;
        break;
    default:
        r1 = chroma, g1 = 0, b1 = xValue;
        break;
    }

    int m = l - chroma / 2;
    auto clamp100 = [](int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); };
    int rr = clamp100(r1 + m), gg = clamp100(g1 + m), bb = clamp100(b1 + m);
    // Scale 0..100 percent → full 16-bit (100 → 0xFFFF), round to nearest.
    auto pctTo16 = [](int v) { return static_cast<uint16_t>((v * 65535 + 50) / 100); };
    *outR = pctTo16(rr);
    *outG = pctTo16(gg);
    *outB = pctTo16(bb);
}

// Parse a decimal integer at the head of `s` (no sign), advancing `s` past the digits.
// Returns the value, or -1 if no digit was present.
static int parseDecimalField(std::string_view& s)
{
    int v = 0;
    std::size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
        v = Ubiquitty::decimalAppendSaturated(v, s[n] - '0', 100000);
        n++;
    }
    if (n == 0)
        return -1;
    s.remove_prefix(n);
    return v;
}

// Parse a color spec into 16-bit RGB. Every X11 color-spec form (#RGB..#RRRRGGGGBBBB, rgb:,
// rgbi:, CIEXYZ:, CIEuvY:, CIExyY:, CIELab:, CIELuv:, TekHVC:) is decoded in XParseColor.h, which
// reproduces the X11 arithmetic exactly, as are X11's colour names, from the rgb.txt table it
// carries; hsl: is a Ubiquitty extension.
// Returns 0 on success.
static int parseColorSpec(std::string_view s, uint16_t* outR, uint16_t* outG, uint16_t* outB)
{
    // Trim leading whitespace.
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);

    // hsl:H/S/L — Ubiquitty extension; the converter is the Sixel DEC-HLS one.
    static constexpr std::string_view hslPrefix = "hsl:";
    if (s.starts_with(hslPrefix)) {
        std::string_view p = s.substr(hslPrefix.size());
        const int h = parseDecimalField(p);
        if (h < 0 || !p.starts_with('/'))
            return -1;
        p.remove_prefix(1);
        const int sat = parseDecimalField(p);
        if (sat < 0 || !p.starts_with('/'))
            return -1;
        p.remove_prefix(1);
        const int light = parseDecimalField(p);
        if (light < 0)
            return -1;
        hslToRGB16(h, sat, light, outR, outG, outB);
        return 0;
    }

    const std::optional<Ubiquitty::PaletteRGB> parsed = XParseColor::parseColorSpecification(s);
    if (!parsed)
        return -1;
    *outR = parsed->red;
    *outG = parsed->green;
    *outB = parsed->blue;
    return 0;
}

// Record the mutation and nudge the frontend to resync + repaint. The epoch
// moves here because every caller already gates on a colour having moved; a
// renderer with no installed client — the runtime-session path — reads the
// epoch where the nudge would have reached nobody.
static void invalidateForPalette(Ubiquitty::Terminal& t)
{
    ++t.paletteEpoch;
    if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient))
        c->invalidate();
}

// Assign a palette slot, returning whether the color didMove. Apps re-assert the same OSC 4/10/11/12 colors
// every frame; a no-op write skips invalidateForPalette so the frontend isn't woken to resync unchanged.
static inline bool setPaletteSlot(Ubiquitty::PaletteRGB& slot, Ubiquitty::PaletteRGB value)
{
    if (slot.red == value.red && slot.green == value.green && slot.blue == value.blue)
        return false;
    slot = value;
    return true;
}

// SetColor is LowRiskInfo: allowed for every origin by default, and a host that distrusts a remote peer
// recoloring the palette (driving foreground toward background hides content) restricts the tier. Gate the SET
// paths; `?` queries reply through Ubiquitty::respond(ColorRead) (Benign) and stay allowed for theme detection.
// An unset policy answers PAL's default.
static inline bool colorSetAllowed(Ubiquitty::Terminal& t)
{
    return Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetColor);
}

// OSC 10/11 — one field setting or querying the default foreground/background. `code` is the reply
// prefix; `slot` is the palette entry that code names.
static void applyDefaultColorField(Ubiquitty::Terminal& t, std::string_view field, int code, Ubiquitty::PaletteRGB& slot)
{
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t'))
        field.remove_prefix(1);
    if (field.empty())
        return;
    if (field.front() == '?' && (field.size() == 1 || field[1] == ' ' || field[1] == '\t')) {
        Ubiquitty::respond(t, Ubiquitty::Capability::ColorRead, colorReply(t, code, slot.red, slot.green, slot.blue).span());
        return;
    }
    if (!colorSetAllowed(t))
        return; // SetColor gate: remote palette write suppressed (queries still reply)
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
    if (parseColorSpec(field, &r, &g, &b) == 0 && setPaletteSlot(slot, { r, g, b }))
        invalidateForPalette(t);
}

// The colour a special colour reports: the one a program set, and until then
// the default foreground, as xterm's colorBD..colorIT resources default to
// XtDefaultForeground (xterm charproc.c).
static Ubiquitty::PaletteRGB specialColorValue(const Ubiquitty::Terminal& t, int pc)
{
    return t.palette.specialSet[pc] ? t.palette.special[pc] : t.palette.defaultForeground;
}

// The colour number of an OSC 4 or OSC 5 pair, read as xterm reads it (misc.c
// 3001, `atoi`): blanks, a sign, then digits, so a field that is not a number
// is colour 0 and "1x" is colour 1. Consumes the field and its ';'; with no ';'
// the pair has no colour, which ends the list.
static std::optional<int> takeColorNumber(std::string_view& p)
{
    const std::size_t semicolon = p.find(';');
    if (semicolon == std::string_view::npos)
        return std::nullopt;
    std::string_view field = p.substr(0, semicolon);
    p.remove_prefix(semicolon + 1);
    while (!field.empty() && (field.front() == ' ' || (field.front() >= '\t' && field.front() <= '\r')))
        field.remove_prefix(1);
    const bool isNegative = field.starts_with('-');
    if (isNegative || field.starts_with('+'))
        field.remove_prefix(1);
    int value = 0;
    while (!field.empty() && field.front() >= '0' && field.front() <= '9') {
        value = Ubiquitty::decimalAppendSaturated(value, field.front() - '0', 65535);
        field.remove_prefix(1);
    }
    return isNegative ? -value : value;
}

// OSC 4 ; <index> ; <spec-or-?> [ ; … ]* — chained index/spec pairs, each set independently until a spec
// that does not parse ends the list; query pairs
// (`?`) emit one response per index. A number past the special colours ends the list.
void handleIndexedColor(Ubiquitty::Terminal& t, std::string_view p)
{
    while (!p.empty()) {
        // Skip any leading whitespace / stray separators between pairs.
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
            p.remove_prefix(1);
        if (p.empty())
            return;

        const std::optional<int> number = takeColorNumber(p);
        if (!number)
            return;
        const int index = *number;

        // Locate the end of this spec field (';' for another pair, or the payload ends).
        const std::size_t semicolon = p.find(';');
        const std::string_view spec = p.substr(0, semicolon);
        p = (semicolon == std::string_view::npos) ? std::string_view() : p.substr(semicolon + 1);

        // 0-255 = indexed palette; 256..256+specialCount-1 = OSC 5 special colors. `setFlag`, when non-null,
        // marks the special slot as explicitly assigned.
        Ubiquitty::PaletteRGB* slot = nullptr;
        bool* setFlag = nullptr;
        if (index >= 0 && index <= 255)
            slot = &t.palette.indexed[index];
        else if (index >= 256 && index < 256 + Ubiquitty::specialCount) {
            slot = &t.palette.special[index - 256];
            setFlag = &t.palette.specialSet[index - 256];
        } else
            return; // a number past the special colours ends the list, as in xterm

        // Query form (`?`): reply with the stored slot, framed as OSC 4.
        if (!spec.empty() && spec.front() == '?' && (spec.size() == 1 || spec[1] == ' ' || spec[1] == '\t')) {
            const Ubiquitty::PaletteRGB color = setFlag ? specialColorValue(t, index - 256) : *slot;
            Ubiquitty::ReplyBuilder reply;
            reply.put("\033]4;");
            reply.putDecimal(index);
            reply.putColor(color.red, color.green, color.blue);
            reply.put(Ubiquitty::oscReplyTerminator(t));
            Ubiquitty::respond(t, Ubiquitty::Capability::ColorRead, reply.span());
            continue;
        }

        if (!colorSetAllowed(t))
            continue; // SetColor gate: remote palette write suppressed (queries above still reply)

        if (spec.empty())
            continue;

        uint16_t r = 0;
        uint16_t g = 0;
        uint16_t b = 0;
        // A colour that does not parse ends the list, as it does in xterm:
        // the pairs after it were written against a palette the program
        // expected the bad one to have changed.
        if (parseColorSpec(spec, &r, &g, &b) != 0)
            return;
        bool didChange = setPaletteSlot(*slot, { r, g, b });
        // The program's colour carries no alpha, so the slot it writes draws
        // opaque from here on: a theme's dimming was for the theme's colour,
        // and a table that kept it would dim a colour the program chose.
        if (index <= 255 && t.palette.indexedAlpha[static_cast<size_t>(index)] != 255) {
            t.palette.indexedAlpha[static_cast<size_t>(index)] = 255;
            didChange = true;
        }
        if (didChange)
            invalidateForPalette(t);
        if (setFlag)
            *setFlag = true;
    }
}

// The baseline's alpha back beside its colour: the theme's dimming returns
// with the theme's colour, and a slot the program never wrote stays as it was.
static bool resetIndexedAlpha(Ubiquitty::Terminal& t, int index)
{
    const auto slot = static_cast<size_t>(index);
    if (t.palette.indexedAlpha[slot] == t.paletteBaseline.indexedAlpha[slot])
        return false;
    t.palette.indexedAlpha[slot] = t.paletteBaseline.indexedAlpha[slot];
    return true;
}

// OSC 104 ; Pc ; … — put palette colours back to the baseline, and with no
// number, all 256. The list is read as xterm's misc.c reads it: each entry
// must begin with a digit and be followed by `;` or the end, and the first
// that is not ends the list. A special colour,
// 256 and up, is restored only from a configured resource, and none is
// configurable, so it stays as it is (see resetSpecialColor).
//
// xterm also reads past the end of the list: it checks for the `;` with
// `strchr(";", c)`, which matches the terminating NUL too, so after the last
// number it steps over the end of the string and goes on reading what an
// earlier, longer control string left in the same buffer. The accumulator here
// is shared by OSC and DCS strings in the same way and holds zeros past what
// was ever written into it, so the read is the same and ends at its capacity.
void resetIndexedColor(Ubiquitty::Terminal& t, std::string_view p)
{
    bool changed = false;
    const auto resetOne = [&](int index) {
        if (index > 255)
            return;
        if (setPaletteSlot(t.palette.indexed[index], t.paletteBaseline.indexed[index]))
            changed = true;
        if (resetIndexedAlpha(t, index))
            changed = true;
    };

    if (p.empty()) {
        for (int i = 0; i < 256; i++)
            resetOne(i);
        if (changed)
            invalidateForPalette(t);
        return;
    }

    const char* const buffer = t.parser.osc.data;
    const bool isInAccumulator = buffer && p.data() >= buffer && p.data() + p.size() < buffer + t.parser.osc.capacity;
    const char* cursor = p.data();
    const char* const limit = isInAccumulator ? buffer + t.parser.osc.capacity : p.data() + p.size();
    while (cursor < limit && *cursor != '\0') {
        if (*cursor < '0' || *cursor > '9')
            break;
        int index = 0;
        while (cursor < limit && *cursor >= '0' && *cursor <= '9') {
            index = Ubiquitty::decimalAppendSaturated(index, *cursor - '0', 65535);
            ++cursor;
        }
        if (cursor < limit) {
            if (*cursor != ';' && *cursor != '\0')
                break;
            ++cursor;
        }
        resetOne(index);
    }
    if (changed)
        invalidateForPalette(t);
}

void resetIndexedColors(Ubiquitty::Terminal& t) { resetIndexedColor(t, {}); }

// OSC 5 ; Pc ; <spec-or-?> [ ; … ]* — set or query a special color. Pc (0=bold, 1=underline, 2=blink,
// 3=reverse, 4=italic) maps to index 256+Pc. Same chained-pair grammar as OSC 4; stored in palette.special.
void handleSpecialColor(Ubiquitty::Terminal& t, std::string_view p)
{
    while (!p.empty()) {
        // Skip any leading whitespace / stray separators between pairs.
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
            p.remove_prefix(1);
        if (p.empty())
            return;

        const std::optional<int> number = takeColorNumber(p);
        if (!number)
            return;
        const int pc = *number;

        // Locate the end of this spec field (';' for another pair, or the payload ends).
        const std::size_t semicolon = p.find(';');
        const std::string_view spec = p.substr(0, semicolon);
        p = (semicolon == std::string_view::npos) ? std::string_view() : p.substr(semicolon + 1);

        if (pc < 0 || pc >= Ubiquitty::specialCount)
            return; // a number out of range ends the list, as in xterm

        // Query form (`?`): report the stored slot, framed as OSC 5.
        if (!spec.empty() && spec.front() == '?' && (spec.size() == 1 || spec[1] == ' ' || spec[1] == '\t')) {
            const Ubiquitty::PaletteRGB slot = specialColorValue(t, pc);
            Ubiquitty::ReplyBuilder reply;
            reply.put("\033]5;");
            reply.putDecimal(pc);
            reply.putColor(slot.red, slot.green, slot.blue);
            reply.put(Ubiquitty::oscReplyTerminator(t));
            if (reply.isTruncated() || reply.length() == 0)
                continue;
            Ubiquitty::respond(t, Ubiquitty::Capability::ColorRead, reply.span());
            continue;
        }

        if (!colorSetAllowed(t))
            continue; // SetColor gate: remote palette write suppressed (queries above still reply)

        if (spec.empty())
            continue;

        uint16_t r = 0;
        uint16_t g = 0;
        uint16_t b = 0;
        if (parseColorSpec(spec, &r, &g, &b) != 0)
            continue;
        // A no-op re-assert skips the invalidate; a value change or first assignment wakes the frontend.
        const bool didMove = setPaletteSlot(t.palette.special[pc], { r, g, b });
        const bool firstSet = !t.palette.specialSet[pc];
        t.palette.specialSet[pc] = true;
        if (didMove || firstSet)
            invalidateForPalette(t);
    }
}

// OSC 105 ; Pc ; … — reset special colours. xterm restores a special colour
// from its resource, and the colorBD..colorIT resources default to
// XtDefaultForeground, a toolkit name the reset looks up with XParseColor and
// does not find (misc.c, the colour reset and xtermAllocColor), so a default xterm
// changes nothing. No special-colour resource is configurable here, so a reset
// leaves every special colour as it is.
void resetSpecialColor(Ubiquitty::Terminal&, std::string_view) { }

// True when a dynamic-color payload is the `?` query form. OSC 12, the OSC 17/19
// family and the Tek trio all decide on this one test, and a Tek handler asks it
// a second time to know whether to skip the repaint nudge.
static bool isColorQueryPayload(std::string_view p)
{
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        p.remove_prefix(1);
    return p.starts_with('?') && (p.size() == 1 || p[1] == ';');
}

// OSC 12 ; <spec-or-?> — set or query the cursor color.
void handleCursorColor(Ubiquitty::Terminal& t, std::string_view p)
{
    if (isColorQueryPayload(p)) {
        const Ubiquitty::PaletteRGB& slot = t.palette.cursor;
        Ubiquitty::ReplyBuilder reply;
        reply.put("\033]12");
        reply.putColor(slot.red, slot.green, slot.blue);
        reply.put(Ubiquitty::oscReplyTerminator(t));
        if (reply.isTruncated() || reply.length() == 0)
            return;
        Ubiquitty::respond(t, Ubiquitty::Capability::ColorRead, reply.span());
        return;
    }

    if (!colorSetAllowed(t))
        return; // SetColor gate: remote cursor-color write suppressed

    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
    if (parseColorSpec(p, &r, &g, &b) != 0)
        return;
    if (setPaletteSlot(t.palette.cursor, { r, g, b }))
        invalidateForPalette(t);
}

// OSC 110/111/112 — reset the default foreground/background/cursor color to the configured baseline.
void resetDefaultForeground(Ubiquitty::Terminal& t)
{
    if (setPaletteSlot(t.palette.defaultForeground, t.paletteBaseline.defaultForeground))
        invalidateForPalette(t);
}
void resetDefaultBackground(Ubiquitty::Terminal& t)
{
    if (setPaletteSlot(t.palette.defaultBackground, t.paletteBaseline.defaultBackground))
        invalidateForPalette(t);
}
void resetCursorColor(Ubiquitty::Terminal& t)
{
    if (setPaletteSlot(t.palette.cursor, t.paletteBaseline.cursor))
        invalidateForPalette(t);
}

// The colour an OSC 13-19 query reports while no program has set that code's
// colour, so a query is always answered. xterm defines OSC 13 and 14, and its
// pointerColor and pointerColorBackground resources default to the configured
// foreground and background (xterm charproc.c); OSC 10 and 11 do not move
// them, so the pointer answers with the baseline, not the live palette. The
// storage-tube colours are the ones the Tek display draws with
// (Tek/Colors.h). The highlight colours are xterm's highlightColor and
// highlightTextColor defaults, the foreground and the background (xterm
// charproc.c), since a selection drawn as a translucent overlay has no single
// opaque colour of its own.
static Ubiquitty::PaletteRGB unsetDynamicColor(const Ubiquitty::Terminal& t, int code)
{
    const auto widened = [](const Ubiquitty::PaletteRGB& color) { return Ubiquitty::Tek::TekColor { color.red, color.green, color.blue }; };
    const auto narrowed = [](Ubiquitty::Tek::TekColor color) { return Ubiquitty::PaletteRGB { color.red, color.green, color.blue }; };
    switch (code) {
    case 13:
        return t.paletteBaseline.defaultForeground;
    case 14:
        return t.paletteBaseline.defaultBackground;
    case 15:
        return narrowed(Ubiquitty::Tek::tekBeam(t, widened(t.palette.defaultForeground), t.tekColorPreference));
    case 16:
        return narrowed(Ubiquitty::Tek::tekBackground(t, widened(t.palette.defaultBackground), t.tekColorPreference));
    case 17:
        return t.palette.defaultForeground;
    case 18:
        return narrowed(Ubiquitty::Tek::tekCursor(t, Ubiquitty::Tek::tekBeam(t, widened(t.palette.defaultForeground), t.tekColorPreference)));
    case 19:
        return t.palette.defaultBackground;
    default:
        return t.palette.defaultForeground;
    }
}

// One of OSC 13-19: the `?` query reports the code's colour, the set colour or
// the default above; a colour spec sets it, under the SetColor gate every
// colour write takes. Returns whether the stored colour moved: a slot that was
// unset, or held another colour.
static bool handleOSCDynamicColor(Ubiquitty::Terminal& t, std::string_view p, int osc, uint16_t* storeR, uint16_t* storeG, uint16_t* storeB, bool* storeSet, Ubiquitty::PAL::SelectionColorSetHandler setter)
{
    if (isColorQueryPayload(p)) {
        const Ubiquitty::PaletteRGB color = *storeSet ? Ubiquitty::PaletteRGB { *storeR, *storeG, *storeB } : unsetDynamicColor(t, osc);
        Ubiquitty::ReplyBuilder reply = colorReply(t, osc, color.red, color.green, color.blue);
        if (reply.isTruncated() || reply.length() == 0)
            return false;
        Ubiquitty::respond(t, Ubiquitty::Capability::ColorRead, reply.span());
        return false;
    }

    if (!colorSetAllowed(t))
        return false;
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
    if (parseColorSpec(p, &r, &g, &b) != 0)
        return false;
    const bool didMove = !*storeSet || *storeR != r || *storeG != g || *storeB != b;
    *storeR = r;
    *storeG = g;
    *storeB = b;
    *storeSet = true;
    if (setter)
        setter(&t, r, g, b);
    return didMove;
}

// OSC 17 / 19 — selection background / foreground (routes the PAL hook). The
// selection colours are palette state: the colour stack saves them, a Sync
// palette block carries them, and an embedding host reads them beside the
// palette's, so a write that moves one moves the palette epoch.
void handleSelectionBackgroundColor(Ubiquitty::Terminal& t, std::string_view p)
{
    if (handleOSCDynamicColor(t, p, 17, &t.selectionBackgroundRed, &t.selectionBackgroundGreen, &t.selectionBackgroundBlue, &t.selectionBackgroundSet, Ubiquitty::PAL::selectionBackgroundSetter()))
        invalidateForPalette(t);
}
void handleSelectionForegroundColor(Ubiquitty::Terminal& t, std::string_view p)
{
    if (handleOSCDynamicColor(t, p, 19, &t.selectionForegroundRed, &t.selectionForegroundGreen, &t.selectionForegroundBlue, &t.selectionForegroundSet, Ubiquitty::PAL::selectionForegroundSetter()))
        invalidateForPalette(t);
}
// OSC 117 / 119 — reset the selection background / foreground color.
void resetSelectionBackground(Ubiquitty::Terminal& t)
{
    const bool wasSet = t.selectionBackgroundSet;
    t.selectionBackgroundSet = false;
    if (Ubiquitty::PAL::SelectionColorResetHandler fn = Ubiquitty::PAL::selectionBackgroundResetter())
        fn(&t);
    if (wasSet)
        invalidateForPalette(t);
}
void resetSelectionForeground(Ubiquitty::Terminal& t)
{
    const bool wasSet = t.selectionForegroundSet;
    t.selectionForegroundSet = false;
    if (Ubiquitty::PAL::SelectionColorResetHandler fn = Ubiquitty::PAL::selectionForegroundResetter())
        fn(&t);
    if (wasSet)
        invalidateForPalette(t);
}

// OSC 13/14 (mouse pointer fg/bg), 15/16 (Tek fg/bg), 18 (Tek cursor). OSC 15/16/18 drive the Tek
// display, so a change nudges a repaint.
void handleMousePointerForegroundColor(Ubiquitty::Terminal& t, std::string_view p) { handleOSCDynamicColor(t, p, 13, &t.mousePointerForegroundRed, &t.mousePointerForegroundGreen, &t.mousePointerForegroundBlue, &t.mousePointerForegroundSet, nullptr); }
void handleMousePointerBackgroundColor(Ubiquitty::Terminal& t, std::string_view p) { handleOSCDynamicColor(t, p, 14, &t.mousePointerBackgroundRed, &t.mousePointerBackgroundGreen, &t.mousePointerBackgroundBlue, &t.mousePointerBackgroundSet, nullptr); }
void handleTektronixForegroundColor(Ubiquitty::Terminal& t, std::string_view p)
{
    handleOSCDynamicColor(t, p, 15, &t.tektronixForegroundRed, &t.tektronixForegroundGreen, &t.tektronixForegroundBlue, &t.tektronixForegroundSet, nullptr);
    if (!isColorQueryPayload(p))
        invalidateForPalette(t);
}
void handleTektronixBackgroundColor(Ubiquitty::Terminal& t, std::string_view p)
{
    handleOSCDynamicColor(t, p, 16, &t.tektronixBackgroundRed, &t.tektronixBackgroundGreen, &t.tektronixBackgroundBlue, &t.tektronixBackgroundSet, nullptr);
    if (!isColorQueryPayload(p))
        invalidateForPalette(t);
}
void handleTektronixCursorColor(Ubiquitty::Terminal& t, std::string_view p)
{
    handleOSCDynamicColor(t, p, 18, &t.tektronixCursorRed, &t.tektronixCursorGreen, &t.tektronixCursorBlue, &t.tektronixCursorSet, nullptr);
    if (!isColorQueryPayload(p))
        invalidateForPalette(t);
}

// OSC 113/114/115/116/118 — reset the matching dynamic color (clear the set flag). The Tek resets
// (115/116/118) repaint so the display reverts to theme colors.
void resetMousePointerForeground(Ubiquitty::Terminal& t) { t.mousePointerForegroundSet = false; }
void resetMousePointerBackground(Ubiquitty::Terminal& t) { t.mousePointerBackgroundSet = false; }
void resetTektronixForeground(Ubiquitty::Terminal& t)
{
    if (t.tektronixForegroundSet) {
        t.tektronixForegroundSet = false;
        invalidateForPalette(t);
    }
}
void resetTektronixBackground(Ubiquitty::Terminal& t)
{
    if (t.tektronixBackgroundSet) {
        t.tektronixBackgroundSet = false;
        invalidateForPalette(t);
    }
}
void resetTektronixCursor(Ubiquitty::Terminal& t)
{
    if (t.tektronixCursorSet) {
        t.tektronixCursorSet = false;
        invalidateForPalette(t);
    }
}

static constexpr int firstDynamicColorCode = 10;
static constexpr int lastDynamicColorCode = 19;

// One field of a dynamic-color control, applied to the color `code` names.
static void applyDynamicColorField(Ubiquitty::Terminal& t, int code, std::string_view field)
{
    switch (code) {
    case 10:
        applyDefaultColorField(t, field, code, t.palette.defaultForeground);
        return;
    case 11:
        applyDefaultColorField(t, field, code, t.palette.defaultBackground);
        return;
    case 12:
        handleCursorColor(t, field);
        return;
    case 13:
        handleMousePointerForegroundColor(t, field);
        return;
    case 14:
        handleMousePointerBackgroundColor(t, field);
        return;
    case 15:
        handleTektronixForegroundColor(t, field);
        return;
    case 16:
        handleTektronixBackgroundColor(t, field);
        return;
    case 17:
        handleSelectionBackgroundColor(t, field);
        return;
    case 18:
        handleTektronixCursorColor(t, field);
        return;
    case 19:
        handleSelectionForegroundColor(t, field);
        return;
    default:
        return;
    }
}

// The dynamic colors are one numbered list, 10 through 19, and a control carries several ';'-separated
// fields, each applying to the next code in the list: "OSC 10;fg;bg" sets both, and "OSC 16;?;?"
// answers twice, once as OSC 16 and once as OSC 17. A reader that expects the second reply hangs
// without it. An empty or unparsable field leaves its color alone and still moves the chain on;
// fields past 19 name nothing. Every code in the list shares the ColorOperations gate, so a chain
// never reaches a color its first code could not.
void handleDynamicColors(Ubiquitty::Terminal& t, std::string_view p, int osc)
{
    for (int code = osc; code >= firstDynamicColorCode && code <= lastDynamicColorCode; code++) {
        const std::size_t semicolon = p.find(';');
        const std::string_view field = p.substr(0, semicolon);
        if (!field.empty())
            applyDynamicColorField(t, code, field);
        if (semicolon == std::string_view::npos)
            return;
        p.remove_prefix(semicolon + 1);
    }
}

// A color as kitty's color control encodes it in a reply: rgb: and a channel
// of two hex digits where the 16-bit value is an 8-bit one widened, else four.
static ReplyBuilder kittyColorValue(PaletteRGB color)
{
    ReplyBuilder encoded;
    encoded.put("rgb:");
    for (auto component : { color.red, color.green, color.blue }) {
        if (encoded.length() > 4)
            encoded.put('/');
        ReplyBuilder digits;
        digits.putHex16(component);
        const auto count = component % 257 == 0 ? 2u : 4u;
        encoded.put(std::string_view(digits.span().data(), count));
    }
    return encoded;
}

void handleKittyColors(Ubiquitty::Terminal& terminal, std::string_view payload)
{
    const int bulkStart = terminal.bulkOutput.length;
    bool hasReply = false;
    bool isReplyComplete = true;
    const auto appendReply = [&](std::string_view bytes) {
        if (isReplyComplete)
            isReplyComplete = respondBulk(terminal, Capability::ColorRead, { bytes.data(), bytes.size() });
    };
    const auto beginReplyField = [&] {
        appendReply(hasReply ? ";" : "\033]21;");
        hasReply = true;
    };
    while (!payload.empty()) {
        const auto separator = payload.find(';');
        const auto field = payload.substr(0, separator);
        payload = separator == std::string_view::npos ? std::string_view {} : payload.substr(separator + 1);
        const auto equal = field.find('=');
        const auto name = field.substr(0, equal);
        if (name.empty())
            continue;
        const auto value = equal == std::string_view::npos ? std::string_view {} : field.substr(equal + 1);
        int index = -1;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), index);
        if (parsed.ec != std::errc {} || parsed.ptr != name.data() + name.size() || index < 0 || index > 255 || name.front() == '-')
            index = -1;
        const int code = name == "foreground" ? 10 : name == "background" ? 11
            : name == "cursor"                                            ? 12
            : name == "selection_background"                              ? 17
            : name == "selection_foreground"                              ? 19
                                                                          : 0;
        // kitty's cursor_text, "The color of text under the cursor", which a
        // focused block cursor draws its glyph in. Unset, which is what the
        // empty value and the bare key ask for, the glyph takes the renderer's
        // own contrast with the cursor rather than the "Background color"
        // kitty's table gives as the key's dynamic value, which the table only
        // advises; a query answers it empty, as a color "that does not have a
        // defined value".
        if (name == "cursor_text") {
            if (value == "?") {
                beginReplyField();
                appendReply("cursor_text=");
                if (terminal.hasCursorTextColor) {
                    const ReplyBuilder encoded = kittyColorValue({ terminal.cursorTextRed, terminal.cursorTextGreen, terminal.cursorTextBlue });
                    appendReply({ encoded.span().data(), encoded.span().size() });
                }
                continue;
            }
            if (!colorSetAllowed(terminal))
                continue;
            if (equal == std::string_view::npos || value.empty()) {
                if (terminal.hasCursorTextColor) {
                    terminal.hasCursorTextColor = false;
                    invalidateForPalette(terminal);
                }
                continue;
            }
            const auto color = XParseColor::parseKittyColorSpecification(value.substr(0, value.find('@')));
            if (!color)
                continue;
            const bool didChange = !terminal.hasCursorTextColor || PaletteRGB { terminal.cursorTextRed, terminal.cursorTextGreen, terminal.cursorTextBlue } != *color;
            terminal.cursorTextRed = color->red;
            terminal.cursorTextGreen = color->green;
            terminal.cursorTextBlue = color->blue;
            terminal.hasCursorTextColor = true;
            if (didChange)
                invalidateForPalette(terminal);
            continue;
        }
        if (index < 0 && !code) {
            beginReplyField();
            appendReply("unknown=");
            std::array<char, 1024> encoded;
            auto remaining = name;
            while (!remaining.empty()) {
                const auto chunk = remaining.substr(0, 768);
                remaining.remove_prefix(chunk.size());
                const auto characters = Codec::base64Encode({ chunk.data(), chunk.size() }, encoded);
                if (!characters) {
                    isReplyComplete = false;
                    break;
                }
                auto text = std::string_view(characters->data(), characters->size());
                while (text.ends_with('='))
                    text.remove_suffix(1);
                appendReply(text);
            }
            continue;
        }
        if (value == "?") {
            beginReplyField();
            appendReply(name);
            appendReply("=");
            if ((code == 17 && !terminal.selectionBackgroundSet) || (code == 19 && !terminal.selectionForegroundSet))
                continue;
            const auto color = index >= 0 ? terminal.palette.indexed[static_cast<std::size_t>(index)]
                : code == 10              ? terminal.palette.defaultForeground
                : code == 11              ? terminal.palette.defaultBackground
                : code == 12              ? terminal.palette.cursor
                : code == 17              ? PaletteRGB { terminal.selectionBackgroundRed, terminal.selectionBackgroundGreen, terminal.selectionBackgroundBlue }
                                          : PaletteRGB { terminal.selectionForegroundRed, terminal.selectionForegroundGreen, terminal.selectionForegroundBlue };
            const ReplyBuilder encoded = kittyColorValue(color);
            appendReply({ encoded.span().data(), encoded.span().size() });
            continue;
        }
        if (!colorSetAllowed(terminal))
            continue;
        if (equal == std::string_view::npos) {
            if (index >= 0) {
                const bool didChange = setPaletteSlot(terminal.palette.indexed[static_cast<std::size_t>(index)], terminal.paletteBaseline.indexed[static_cast<std::size_t>(index)]);
                if (resetIndexedAlpha(terminal, index) || didChange)
                    invalidateForPalette(terminal);
            } else if (code == 10)
                resetDefaultForeground(terminal);
            else if (code == 11)
                resetDefaultBackground(terminal);
            else if (code == 12)
                resetCursorColor(terminal);
            else if (code == 17)
                resetSelectionBackground(terminal);
            else if (code == 19)
                resetSelectionForeground(terminal);
            continue;
        }
        if (value.empty()) {
            // Only selection colors support a dynamic presentation here.
            if (code == 17)
                resetSelectionBackground(terminal);
            else if (code == 19)
                resetSelectionForeground(terminal);
            continue;
        }
        const auto color = XParseColor::parseKittyColorSpecification(value.substr(0, value.find('@')));
        if (!color)
            continue;
        bool didChange = false;
        if (index >= 0) {
            const auto slot = static_cast<std::size_t>(index);
            didChange = setPaletteSlot(terminal.palette.indexed[slot], *color);
            didChange = terminal.palette.indexedAlpha[slot] != 255 || didChange;
            terminal.palette.indexedAlpha[slot] = 255;
        } else if (code == 10)
            didChange = setPaletteSlot(terminal.palette.defaultForeground, *color);
        else if (code == 11)
            didChange = setPaletteSlot(terminal.palette.defaultBackground, *color);
        else if (code == 12)
            didChange = setPaletteSlot(terminal.palette.cursor, *color);
        else {
            const bool isBackground = code == 17;
            auto& red = isBackground ? terminal.selectionBackgroundRed : terminal.selectionForegroundRed;
            auto& green = isBackground ? terminal.selectionBackgroundGreen : terminal.selectionForegroundGreen;
            auto& blue = isBackground ? terminal.selectionBackgroundBlue : terminal.selectionForegroundBlue;
            auto& isSet = isBackground ? terminal.selectionBackgroundSet : terminal.selectionForegroundSet;
            didChange = !isSet || PaletteRGB { red, green, blue } != *color;
            red = color->red;
            green = color->green;
            blue = color->blue;
            isSet = true;
            const auto setter = isBackground ? PAL::selectionBackgroundSetter() : PAL::selectionForegroundSetter();
            if (setter && didChange)
                setter(&terminal, red, green, blue);
        }
        if (didChange)
            invalidateForPalette(terminal);
    }
    if (hasReply)
        appendReply(oscReplyTerminator(terminal));
    if (!isReplyComplete) {
        terminal.bulkOutput.length = bulkStart;
        if (terminal.bulkOutput.data)
            terminal.bulkOutput.data[bulkStart] = 0;
    }
}

} // namespace Ubiquitty::OSC
