// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Pure-data sixel decoder (see header for format notes): a single-pass byte loop
// with a helper that emits one sextet at the current cursor.

#include "Core/Image/Sixel/Decoder.h"
#include <cassert>
#include <cstring>

namespace Ubiquitty::Image::Sixel {

namespace {

constexpr int maxPaletteParameters = 5;
constexpr int maxRasterParameters = 4;
// Saturate parameter accumulation so a long digit run can't overflow the
// signed accumulator before callers clamp to their real domain.
constexpr int maxSixelParam = 1 << 24;
constexpr int defaultPaletteEntries = 16;

// DEC standard 16-color palette in 0..100 percent. Index 0 is black; entries
// 16..255 stay 0,0,0 until the payload defines them.
const uint8_t defaultPalettePct[defaultPaletteEntries][3] = {
    { 0, 0, 0 }, // 0  black
    { 20, 20, 80 }, // 1  blue
    { 80, 13, 13 }, // 2  red
    { 20, 80, 20 }, // 3  green
    { 80, 20, 80 }, // 4  magenta
    { 20, 80, 80 }, // 5  cyan
    { 80, 80, 20 }, // 6  yellow
    { 53, 53, 53 }, // 7  gray 50%
    { 26, 26, 26 }, // 8  gray 25%
    { 33, 33, 60 }, // 9  light blue
    { 60, 26, 26 }, // 10 light red
    { 33, 60, 33 }, // 11 light green
    { 60, 33, 60 }, // 12 light magenta
    { 33, 60, 60 }, // 13 light cyan
    { 60, 60, 33 }, // 14 light yellow
    { 80, 80, 80 }, // 15 gray 75%
};

inline uint8_t percentToByte(int pct)
{
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    // +50 for round-to-nearest in integer division.
    return static_cast<uint8_t>((pct * 255 + 50) / 100);
}

// DEC HLS → RGB. The DEC hue axis places 0° at blue, 120° at red, 240° at green;
// rotate to standard HSL, then the usual integer chroma/X formula.
void hlsPercentToRGB(int huePercent, int lightnessPercent, int saturationPercent, uint8_t rgb[3])
{
    int h = huePercent, l = lightnessPercent, s = saturationPercent;
    while (h < 0) {
        h += 360;
    }
    h %= 360;
    if (l < 0) {
        l = 0;
    }
    if (l > 100) {
        l = 100;
    }
    if (s < 0) {
        s = 0;
    }
    if (s > 100) {
        s = 100;
    }

    // Rotate DEC hue to standard HSL (so 0° = red).
    int hStd = (h + 240) % 360;

    int twoLm100 = (2 * l) - 100;
    int absTwoLm100 = (twoLm100 < 0) ? -twoLm100 : twoLm100;
    int chroma = ((100 - absTwoLm100) * s) / 100; // 0..100

    int sector = hStd / 60; // 0..5
    int hMod120 = hStd % 120;
    int dist = hMod120 - 60;
    if (dist < 0) {
        dist = -dist;
    }
    int xValue = (chroma * (60 - dist)) / 60; // 0..100

    int r1 = 0, g1 = 0, b1 = 0;
    switch (sector) {
    case 0:
        r1 = chroma;
        g1 = xValue;
        b1 = 0;
        break;
    case 1:
        r1 = xValue;
        g1 = chroma;
        b1 = 0;
        break;
    case 2:
        r1 = 0;
        g1 = chroma;
        b1 = xValue;
        break;
    case 3:
        r1 = 0;
        g1 = xValue;
        b1 = chroma;
        break;
    case 4:
        r1 = xValue;
        g1 = 0;
        b1 = chroma;
        break;
    default:
        r1 = chroma;
        g1 = 0;
        b1 = xValue;
        break;
    }

    int m = l - chroma / 2;
    int rr = r1 + m;
    int gg = g1 + m;
    int bb = b1 + m;
    if (rr < 0) {
        rr = 0;
    }
    if (rr > 100) {
        rr = 100;
    }
    if (gg < 0) {
        gg = 0;
    }
    if (gg > 100) {
        gg = 100;
    }
    if (bb < 0) {
        bb = 0;
    }
    if (bb > 100) {
        bb = 100;
    }
    rgb[0] = percentToByte(rr);
    rgb[1] = percentToByte(gg);
    rgb[2] = percentToByte(bb);
}

// Parse a semicolon-separated decimal-integer list starting at *p, stopping at
// the first non-digit, non-semicolon byte. Empty positions ("3;;5") are 0.
int parseParameters(const char*& p, const char* end, int* out, int maxOut)
{
    int count = 0;
    while (p < end && count < maxOut) {
        int val = 0;
        bool hasDigit = false;
        while (p < end && *p >= '0' && *p <= '9') {
            if (val <= maxSixelParam)
                val = val * 10 + (*p - '0');
            ++p;
            hasDigit = true;
        }
        if (val > maxSixelParam)
            val = maxSixelParam;
        out[count++] = hasDigit ? val : 0;
        if (p < end && *p == ';') {
            ++p;
            continue;
        }
        break;
    }
    return count;
}

struct DecoderState {
    uint8_t* bitmap;
    uint8_t* opaque; // optional written mask, parallel to bitmap; null = untracked
    uint16_t maxWidth;
    uint16_t maxHeight;
    Image* out;
    int currentX;
    int currentY; // top scanline of current sixel-row
    uint8_t currentColor;
    int maxXSeen; // exclusive
    int maxYSeen; // exclusive
    bool truncated;
};

void emitSextet(DecoderState& st, int bits, int repeat)
{
    if (repeat <= 0) {
        repeat = 1;
    }
    if (st.currentX < 0 || st.currentX >= st.maxWidth) {
        // Past the canvas: flag truncation and return. Advancing currentX over
        // the rest of the repeat is dead work and a DoS surface for a hostile
        // huge !repeat.
        if (bits != 0) {
            st.truncated = true;
        }
        st.currentX = st.maxWidth;
        return;
    }
    // A repeat writes one column per step, so the pixels a given row receives
    // are contiguous — the run belongs on the inside of the loop nest, where it
    // is a fill, not on the outside where it was a byte and an offset multiply
    // per pixel. `!255~` costs six fills instead of fifteen hundred stores.
    const int startX = st.currentX;
    const int room = static_cast<int>(st.maxWidth) - startX;
    const int painted = repeat < room ? repeat : room;
    if (painted < repeat && bits != 0) {
        st.truncated = true;
    }
    if (bits != 0) {
        for (int i = 0; i < bandHeight; ++i) {
            if (!(bits & (1 << i))) {
                continue;
            }
            const int y = st.currentY + i;
            if (y < 0 || y >= st.maxHeight) {
                st.truncated = true;
                continue;
            }
            const int rowStart = y * static_cast<int>(st.maxWidth) + startX;
            assert(rowStart >= 0 && rowStart + painted <= static_cast<int>(st.maxWidth) * static_cast<int>(st.maxHeight));
            std::memset(st.bitmap + rowStart, st.currentColor, static_cast<std::size_t>(painted));
            if (st.opaque) {
                std::memset(st.opaque + rowStart, 1, static_cast<std::size_t>(painted));
            }
            if (y + 1 > st.maxYSeen) {
                st.maxYSeen = y + 1;
            }
        }
        if (startX + painted > st.maxXSeen) {
            st.maxXSeen = startX + painted;
        }
    }
    st.currentX = painted < repeat ? static_cast<int>(st.maxWidth) : startX + repeat;
}

} // namespace

void defaultPalette(std::span<std::uint8_t, paletteBytes> palette)
{
    std::memset(palette.data(), 0, palette.size());
    for (int i = 0; i < defaultPaletteEntries; ++i) {
        palette[static_cast<std::size_t>(i) * 3] = percentToByte(defaultPalettePct[i][0]);
        palette[static_cast<std::size_t>(i) * 3 + 1] = percentToByte(defaultPalettePct[i][1]);
        palette[static_cast<std::size_t>(i) * 3 + 2] = percentToByte(defaultPalettePct[i][2]);
    }
}

PayloadMeasure measurePayload(std::string_view bytes, Ubiquitty::IntSize ceiling)
{
    // Mirrors decode's dispatch, counting instead of painting. Where the two
    // could disagree the count goes high: this bounds a buffer, and a bound that
    // reads low would truncate an image decode would have drawn whole.
    long long widest = 0;
    long long currentX = 0;
    long long emitted = 0; // total emissions, the term the canvas does not bound
    long long bands = 1; // a payload with no '-' is still one band tall
    int rasterWidth = 0;
    int rasterHeight = 0;
    // decode caps a repeat at twice its canvas; here the canvas is the ceiling,
    // which is the widest decode could be asked for, so the cap cannot read low.
    const long long repeatCeiling = 2LL * static_cast<long long>(ceiling.width);

    const char* p = bytes.data();
    const char* end = bytes.data() + bytes.size();
    while (p < end) {
        const unsigned char c = static_cast<unsigned char>(*p);
        // The data byte is the overwhelming majority of a payload and shares no
        // value with any introducer below — every one of those is under 0x3F —
        // so it is asked about first and the rest of the dispatch is skipped
        // rather than walked past.
        if (c >= 0x3F && c <= 0x7E) {
            if (currentX < ceiling.width) {
                ++emitted;
                ++currentX;
                if (currentX > widest)
                    widest = currentX;
            }
            ++p;
            continue;
        }
        if (c == '"') {
            ++p;
            int parameters[maxRasterParameters] = { 0, 0, 0, 0 };
            if (parseParameters(p, end, parameters, maxRasterParameters) >= 4) {
                rasterWidth = parameters[2];
                rasterHeight = parameters[3];
            }
            continue;
        }
        if (c == '#') {
            ++p;
            int parameters[maxPaletteParameters] = { 0, 0, 0, 0, 0 };
            parseParameters(p, end, parameters, maxPaletteParameters);
            continue;
        }
        if (c == '!') {
            ++p;
            int parameters[1] = { 0 };
            parseParameters(p, end, parameters, 1);
            long long repeat = parameters[0] > 0 ? parameters[0] : 1;
            if (repeat > repeatCeiling)
                repeat = repeatCeiling;
            if (p < end) {
                const unsigned char sextet = static_cast<unsigned char>(*p);
                if (sextet >= 0x3F && sextet <= 0x7E) {
                    ++p;
                    // emitSextet stops at the canvas edge rather than running the
                    // rest of the repeat, so the columns it emits are what is left
                    // of the row, not the whole count.
                    const long long room = static_cast<long long>(ceiling.width) - currentX;
                    const long long run = repeat < room ? repeat : (room > 0 ? room : 0);
                    emitted += run;
                    currentX += run;
                    if (currentX > widest)
                        widest = currentX;
                }
            }
            continue;
        }
        if (c == '-') {
            ++bands;
            currentX = 0;
            ++p;
            continue;
        }
        if (c == '$') {
            currentX = 0;
            ++p;
            continue;
        }
        ++p; // unknown byte, as decode skips it
    }

    long long width = widest > rasterWidth ? widest : rasterWidth;
    long long height = bands * bandHeight;
    if (rasterHeight > height)
        height = rasterHeight;
    if (width < 1)
        width = 1; // decode rejects a zero canvas outright
    if (height < 1)
        height = 1;
    if (width > ceiling.width)
        width = ceiling.width;
    if (height > ceiling.height)
        height = ceiling.height;
    return { { static_cast<int>(width), static_cast<int>(height) }, emitted };
}

DecodeStatus decode(std::string_view bytes, uint8_t* bitmap, uint16_t maxWidth, uint16_t maxHeight, Image* out, uint8_t* opaqueMask, std::span<const std::uint8_t> initialPalette)
{
    if (!out || !bitmap) {
        return DecodeStatus::InvalidBuffer;
    }
    if (maxWidth == 0 || maxHeight == 0) {
        return DecodeStatus::InvalidBuffer;
    }

    const size_t area = static_cast<size_t>(maxWidth) * static_cast<size_t>(maxHeight);
    static_assert(sizeof(out->palette) == paletteBytes);
    if (initialPalette.size() == paletteBytes)
        std::memcpy(out->palette, initialPalette.data(), paletteBytes);
    else
        defaultPalette(std::span<std::uint8_t, paletteBytes>(&out->palette[0][0], paletteBytes));
    std::memset(bitmap, 0, area);
    if (opaqueMask) {
        std::memset(opaqueMask, 0, area);
    }
    out->pixels = bitmap;
    out->opaque = opaqueMask;
    out->stride = maxWidth;
    out->width = 0;
    out->height = 0;
    out->truncated = false;
    out->rasterPan = 0;
    out->rasterPad = 0;

    DecoderState st;
    st.bitmap = bitmap;
    st.opaque = opaqueMask;
    st.maxWidth = maxWidth;
    st.maxHeight = maxHeight;
    st.out = out;
    st.currentX = 0;
    st.currentY = 0;
    st.currentColor = 0;
    st.maxXSeen = 0;
    st.maxYSeen = 0;
    st.truncated = false;

    int rasterWidth = 0; // 0 = unset
    int rasterHeight = 0;
    int rasterPan = 0; // pixel-aspect numerator (vertical), 0 = unset
    int rasterPad = 0; // pixel-aspect denominator (horizontal)

    const char* p = bytes.data();
    const char* end = bytes.data() + bytes.size();
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        // The data byte is the overwhelming majority of a payload and shares no
        // value with any introducer below — every one of those is under 0x3F —
        // so it is asked about first and the rest of the dispatch is skipped
        // rather than walked past.
        if (c >= 0x3F && c <= 0x7E) {
            emitSextet(st, static_cast<int>(c) - 0x3F, 1);
            ++p;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++p;
            continue;
        }
        if (c == '"') {
            ++p;
            int parameters[maxRasterParameters] = { 0, 0, 0, 0 };
            int n = parseParameters(p, end, parameters, maxRasterParameters);
            if (n >= 2) {
                rasterPan = parameters[0];
                rasterPad = parameters[1];
            }
            if (n >= 4) {
                rasterWidth = parameters[2];
                rasterHeight = parameters[3];
            }
            continue;
        }
        if (c == '#') {
            ++p;
            int parameters[maxPaletteParameters] = { 0, 0, 0, 0, 0 };
            int n = parseParameters(p, end, parameters, maxPaletteParameters);
            if (n == 0) {
                continue;
            }
            int pc = parameters[0];
            if (pc < 0 || pc > 255) {
                continue;
            }
            if (n >= 5) {
                int pu = parameters[1];
                uint8_t rgb[3] = { 0, 0, 0 };
                if (pu == 2) {
                    rgb[0] = percentToByte(parameters[2]);
                    rgb[1] = percentToByte(parameters[3]);
                    rgb[2] = percentToByte(parameters[4]);
                } else {
                    // Anything other than Pu=2 (RGB) is treated as HLS (Pu=1).
                    hlsPercentToRGB(parameters[2], parameters[3], parameters[4], rgb);
                }
                out->palette[pc][0] = rgb[0];
                out->palette[pc][1] = rgb[1];
                out->palette[pc][2] = rgb[2];
            }
            st.currentColor = static_cast<uint8_t>(pc);
            continue;
        }
        if (c == '!') {
            ++p;
            int parameters[1] = { 0 };
            parseParameters(p, end, parameters, 1);
            int repeat = parameters[0];
            if (repeat <= 0) {
                repeat = 1;
            }
            // Cap repeat at two canvas widths so "!1000000?" can't iterate for
            // millions of cycles (dead work beyond maxWidth; see emitSextet).
            int repeatCapacity = 2 * static_cast<int>(st.maxWidth);
            if (repeatCapacity < 1) {
                repeatCapacity = 1;
            }
            if (repeat > repeatCapacity) {
                repeat = repeatCapacity;
            }
            if (p >= end) {
                break;
            }
            unsigned char sx = static_cast<unsigned char>(*p);
            if (sx >= 0x3F && sx <= 0x7E) {
                ++p;
                emitSextet(st, static_cast<int>(sx) - 0x3F, repeat);
            }
            // Non-sextet byte: leave it for the outer dispatch.
            continue;
        }
        if (c == '-') {
            st.currentY += bandHeight;
            st.currentX = 0;
            ++p;
            continue;
        }
        if (c == '$') {
            st.currentX = 0;
            ++p;
            continue;
        }
        // Unknown byte (BEL, etc.) — skip silently.
        ++p;
    }

    int finalWidth = (rasterWidth > 0) ? rasterWidth : st.maxXSeen;
    int finalHeight = (rasterHeight > 0) ? rasterHeight : st.maxYSeen;
    if (finalWidth > maxWidth) {
        finalWidth = maxWidth;
        st.truncated = true;
    }
    if (finalHeight > maxHeight) {
        finalHeight = maxHeight;
        st.truncated = true;
    }
    if (finalWidth < 0) {
        finalWidth = 0;
    }
    if (finalHeight < 0) {
        finalHeight = 0;
    }

    out->width = static_cast<uint16_t>(finalWidth);
    out->height = static_cast<uint16_t>(finalHeight);
    out->truncated = st.truncated;
    out->rasterPan = static_cast<uint16_t>(rasterPan > 0 ? rasterPan : 0);
    out->rasterPad = static_cast<uint16_t>(rasterPad > 0 ? rasterPad : 0);
    return DecodeStatus::OK;
}

} // namespace Ubiquitty::Image::Sixel
