// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Project.cpp — see Project.h for the contract. Pure integer / float
// math. No platform headers, no heap.
//

#include "Core/Tek/Project.h"

#include "Core/Rendering/ColorResolve.h"
#include "Core/Tek/BeamRaster.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Ubiquitty::Tek {

// Mirror of the Tek::LineStyle / Tek::CharSize enum values from
// Source/Core/Tek/Tek4014.h. Duplicated as plain constants so this TU stays
// free of the Base/Types.h + Ubiquitty::Terminal coupling — the values are a wire
// contract (xterm line types / 4014 char sizes), not an implementation
// detail. A divergence is caught by test_tekproject's named-size cases.
namespace {

[[maybe_unused]] constexpr int lineSolid = 0;
[[maybe_unused]] constexpr int lineDotted = 1;
[[maybe_unused]] constexpr int lineDotdash = 2;
[[maybe_unused]] constexpr int lineShortdash = 3;
[[maybe_unused]] constexpr int lineLongdash = 4;

[[maybe_unused]] constexpr int charLarge = 0; // 35 lines x 74 columns
[[maybe_unused]] constexpr int char2 = 1; // 38 lines x 81 columns
[[maybe_unused]] constexpr int char3 = 2; // 58 lines x 121 columns
[[maybe_unused]] constexpr int charSmall = 3; // 64 lines x 133 columns

// Columns-across / lines-down for each 4014 char size (from the 4014
// user manual; see Tek4014.h). Index by CharSize.
const int charColumns[4] = { 74, 81, 121, 133 };
const int charLines[4] = { 35, 38, 58, 64 };

// Base dash run lengths in TEK units (before projection scale). Chosen
// so the patterns read as dotted / dot-dash / short-dash / long-dash at
// the 4014's native resolution; the projection scale shrinks/grows them
// uniformly so density tracks the viewport.
struct BaseDash {
    int count;
    float on[4];
    float off[4];
};

const BaseDash baseDash[5] = {
    { 0, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }, // SOLID
    { 1, { 16, 0, 0, 0 }, { 24, 0, 0, 0 } }, // DOTTED
    { 2, { 64, 16, 0, 0 }, { 24, 24, 0, 0 } }, // DOTDASH
    { 1, { 48, 0, 0, 0 }, { 32, 0, 0, 0 } }, // SHORTDASH
    { 1, { 96, 0, 0, 0 }, { 48, 0, 0, 0 } }, // LONGDASH
};

} // namespace

Projection makeProjection(int tekWidth, int tekHeight, float devX, float devY, float devWidth, float devHeight)
{
    Projection p;
    p.tekWidth = tekWidth;
    p.tekHeight = tekHeight;
    p.devX = devX;
    p.devY = devY;
    p.devWidth = devWidth;
    p.devHeight = devHeight;
    p.scale = 0.0f;
    p.offsetX = 0.0f;
    p.offsetY = 0.0f;

    if (tekWidth <= 0 || tekHeight <= 0 || devWidth <= 0.0f || devHeight <= 0.0f)
        return p; // degenerate: scale 0, everything maps to (devX,devY)

    // Per-axis scale; the uniform scale is the smaller so the whole Tek
    // rect fits inside the device rect (the larger axis would overflow).
    const float sx = devWidth / static_cast<float>(tekWidth);
    const float sy = devHeight / static_cast<float>(tekHeight);
    const float s = (sx < sy) ? sx : sy;
    p.scale = s;

    // Centre the fitted rect: leftover space on each slack axis is split
    // evenly into the two letterbox / pillarbox margins.
    const float fittedWidth = s * static_cast<float>(tekWidth);
    const float fittedHeight = s * static_cast<float>(tekHeight);
    p.offsetX = (devWidth - fittedWidth) * 0.5f;
    p.offsetY = (devHeight - fittedHeight) * 0.5f;
    return p;
}

// The beam in points. The stroke is one point wide; the halo's spread and
// reach are what read as a soft bloom a few pixels either side at 1x and 2x;
// the inset holds the halo's reach plus the stroke's half-width and its
// one-pixel antialiasing feather, so nothing the beam draws at an edge address
// is cut by the rect.
constexpr float beamWidthInPoints = 1.0f;
constexpr float haloSigmaInPoints = 2.0f;
constexpr float haloReachInPoints = 3.0f * haloSigmaInPoints;
constexpr float haloPeakIntensity = 0.6f;
constexpr float edgeInsetInPoints = haloReachInPoints + beamWidthInPoints;

// The Metal glow pass binds this struct's bytes as its `TekBeamGeometry`.
static_assert(sizeof(BeamGeometry) == 5 * sizeof(float));

BeamGeometry beamGeometry(float deviceScale)
{
    const float scale = deviceScale > 0.0f ? deviceScale : 1.0f;
    const float thickness = std::max(beamWidthInPoints * scale, 1.0f);
    return BeamGeometry {
        .halfThickness = thickness * 0.5f,
        .haloSigma = haloSigmaInPoints * scale,
        .haloReach = haloReachInPoints * scale,
        .haloPeak = haloPeakIntensity,
        .edgeInset = edgeInsetInPoints * scale,
    };
}

Projection makeBeamProjection(int tekWidth, int tekHeight, float devX, float devY, float devWidth, float devHeight, float deviceScale)
{
    const float inset = beamGeometry(deviceScale).edgeInset;
    return makeProjection(tekWidth, tekHeight, devX + inset, devY + inset, devWidth - 2.0f * inset, devHeight - 2.0f * inset);
}

float distanceToRun(const StrokeRun& run, DevicePoint point)
{
    const float deltaX = run.end.x - run.start.x;
    const float deltaY = run.end.y - run.start.y;
    const float lengthSquared = deltaX * deltaX + deltaY * deltaY;
    float along = 0.0f;
    if (lengthSquared > 0.0f)
        along = std::clamp(((point.x - run.start.x) * deltaX + (point.y - run.start.y) * deltaY) / lengthSquared, 0.0f, 1.0f);
    const float offsetX = point.x - (run.start.x + deltaX * along);
    const float offsetY = point.y - (run.start.y + deltaY * along);
    return std::sqrt(offsetX * offsetX + offsetY * offsetY);
}

float haloIntensity(const BeamGeometry& beam, float distance)
{
    if (beam.haloSigma <= 0.0f || distance >= beam.haloReach)
        return 0.0f;
    const float twoSigmaSquared = 2.0f * beam.haloSigma * beam.haloSigma;
    const float floor = std::exp(-(beam.haloReach * beam.haloReach) / twoSigmaSquared);
    const float gaussian = std::exp(-(distance * distance) / twoSigmaSquared);
    return beam.haloPeak * std::max(gaussian - floor, 0.0f) / (1.0f - floor);
}

int haloLevelCount(const BeamGeometry& beam)
{
    if (beam.haloReach <= 0.0f)
        return 0;
    return static_cast<int>(std::ceil(beam.haloReach * static_cast<float>(haloLevelsPerPixel()))) + 1;
}

int fillHaloLevels(const BeamGeometry& beam, std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::span<HaloLevel> out)
{
    const int count = std::min(haloLevelCount(beam), static_cast<int>(out.size()));
    const std::array<double, 3> linear {
        Rendering::SRGBGamma::linearize(red / 255.0),
        Rendering::SRGBGamma::linearize(green / 255.0),
        Rendering::SRGBGamma::linearize(blue / 255.0),
    };
    const auto level = [](double channel, float intensity) {
        const double encoded = Rendering::SRGBGamma::encode(channel * static_cast<double>(intensity));
        return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
    };
    for (int index = 0; index < count; ++index) {
        const float intensity = haloIntensity(beam, static_cast<float>(index) / static_cast<float>(haloLevelsPerPixel()));
        out[static_cast<std::size_t>(index)] = { level(linear[0], intensity), level(linear[1], intensity), level(linear[2], intensity), 0 };
    }
    return count;
}

DevicePoint project(const Projection& p, int tekX, int tekY)
{
    DevicePoint d;
    // X: straight scale + centre.
    d.x = p.devX + p.offsetX + static_cast<float>(tekX) * p.scale;
    // Y: FLIP. Tek y measured from the BOTTOM, device y from the TOP.
    // device_y = top_of_fitted_rect + (tekHeight - tekY) * scale.
    const float flipped = static_cast<float>(p.tekHeight - tekY);
    d.y = p.devY + p.offsetY + flipped * p.scale;
    return d;
}

void unproject(const Projection& p, float devX, float devY, int* outTekX, int* outTekY)
{
    int tx = 0;
    int ty = 0;
    if (p.scale > 0.0f) {
        const float fx = (devX - p.devX - p.offsetX) / p.scale;
        // Invert the Y-flip: device_y -> flipped -> tekY.
        const float flipped = (devY - p.devY - p.offsetY) / p.scale;
        const float fy = static_cast<float>(p.tekHeight) - flipped;
        tx = static_cast<int>(fx + 0.5f);
        ty = static_cast<int>(fy + 0.5f);
    }
    // Clamp into the valid Tek address range.
    if (tx < 0)
        tx = 0;
    if (tx > p.tekWidth - 1)
        tx = p.tekWidth - 1;
    if (ty < 0)
        ty = 0;
    if (ty > p.tekHeight - 1)
        ty = p.tekHeight - 1;
    if (outTekX)
        *outTekX = tx;
    if (outTekY)
        *outTekY = ty;
}

DashPattern dashForLineStyle(const Projection& p, unsigned lineStyle)
{
    DashPattern out;
    out.count = 0;
    for (int i = 0; i < 4; ++i) {
        out.on[i] = 0.0f;
        out.off[i] = 0.0f;
    }

    unsigned index = lineStyle;
    if (index > lineLongdash)
        index = lineSolid; // reserved patterns fold onto solid

    const BaseDash& b = baseDash[index];
    out.count = b.count;
    // A zero scale (degenerate viewport) leaves the runs at base units;
    // harmless since nothing is drawn. Otherwise scale to device px.
    const float s = (p.scale > 0.0f) ? p.scale : 1.0f;
    for (int i = 0; i < b.count && i < 4; ++i) {
        out.on[i] = b.on[i] * s;
        out.off[i] = b.off[i] * s;
    }
    return out;
}

DeviceSize charCellSize(const Projection& p, unsigned charSize)
{
    DeviceSize out;
    unsigned index = charSize;
    if (index > charSmall)
        index = charLarge;

    // One alpha cell spans (tekWidth / columns) Tek units wide and
    // (tekHeight / lines) Tek units tall; project that span through the
    // uniform scale to device pixels.
    const float cellTekWidth = static_cast<float>(p.tekWidth) / static_cast<float>(charColumns[index]);
    const float cellTekHeight = static_cast<float>(p.tekHeight) / static_cast<float>(charLines[index]);
    out.w = cellTekWidth * p.scale;
    out.h = cellTekHeight * p.scale;
    return out;
}

int dashRunsForSegment(DevicePoint start, DevicePoint end, const DashPattern& dash, std::span<StrokeRun> out)
{
    if (out.empty())
        return 0;
    const auto whole = [&] {
        out[0] = { start, end };
        return 1;
    };
    if (dash.count <= 0)
        return whole();
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if (length < 0.5f)
        return whole();
    const float unitX = deltaX / length;
    const float unitY = deltaY / length;
    float position = 0.0f;
    int run = 0;
    int written = 0;
    while (position < length && written < static_cast<int>(out.size())) {
        const int index = run % dash.count;
        float onLength = dash.on[index];
        float offLength = dash.off[index];
        if (onLength <= 0.0f)
            onLength = 1.0f;
        if (offLength <= 0.0f)
            offLength = 1.0f;
        float runEnd = position + onLength;
        if (runEnd > length)
            runEnd = length;
        out[written] = { DevicePoint { start.x + unitX * position, start.y + unitY * position },
            DevicePoint { start.x + unitX * runEnd, start.y + unitY * runEnd } };
        ++written;
        position = runEnd + offLength;
        ++run;
    }
    return written;
}

// The stroke alphabet, printable ASCII in order from SPACE. Each glyph is a
// list of polylines on a grid four units wide and six tall, y up from the
// baseline: a polyline is a run of digit pairs "xy" and a space starts the
// next one, so "0646 2620" is a T, its bar then its stem. The letterforms are
// the single-stroke capitals a plotter draws, which is what the tube's
// character generator drew too; lower case takes the capital's pattern.
constexpr const char* const strokeAlphabet[95] = {
    "", // SPACE
    "2622 2021", // !
    "1514 3534", // "
    "1016 3036 0444 0242", // #
    "453616050413334241301001 2620", // $
    "0640 0506161505 3031414030", // %
    "43140516251302113042", // &
    "2624", // '
    "36252130", // (
    "16252110", // )
    "0640 0046 0343", // *
    "0343 2521", // +
    "222110", // , a tick with a tail, so it is not the full stop at a small size
    "1333", // -
    "2021", // .
    "0046", // /
    "103041453616050110 1135", // 0
    "152620 1030", // 1
    "05163645440040", // 2
    "0646234341301001", // 3
    "30360242", // 4
    "460604344341301001", // 5
    "4626040110304142331303", // 6
    "064610", // 7
    "13020110304142331304051636454433", // 8
    "43130405163645413010", // 9
    "2425 2122", // :
    "2425 2211", // ;
    "460340", // <
    "0444 0242", // =
    "064300", // >
    "05163645442322 2021", // ?
    "31301001051636454232222333", // @
    "0004264440 0242", // A
    "00063645443303 3342413000", // B
    "4536160501103041", // C
    "00063645413000", // D
    "40000646 0333", // E
    "000646 0333", // F
    "45361605011030414323", // G
    "0006 4046 0343", // H
    "1636 2620 1030", // I
    "3631201001", // J
    "0006 4602 1340", // K
    "060040", // L
    "0006234640", // M
    "00064046", // N
    "103041453616050110", // O
    "00063645443303", // P
    "103041453616050110 2240", // Q
    "00063645443303 2340", // R
    "453616050413334241301001", // S
    "0646 2620", // T
    "060110304146", // U
    "062046", // V
    "0610233046", // W
    "0046 0640", // X
    "062346 2320", // Y
    "06460040", // Z
    "36161030", // [
    "0640", // backslash
    "16363010", // ]
    "042644", // ^
    "0040", // _
    "1624", // `
    "0004264440 0242", // a
    "00063645443303 3342413000", // b
    "4536160501103041", // c
    "00063645413000", // d
    "40000646 0333", // e
    "000646 0333", // f
    "45361605011030414323", // g
    "0006 4046 0343", // h
    "1636 2620 1030", // i
    "3631201001", // j
    "0006 4602 1340", // k
    "060040", // l
    "0006234640", // m
    "00064046", // n
    "103041453616050110", // o
    "00063645443303", // p
    "103041453616050110 2240", // q
    "00063645443303 2340", // r
    "453616050413334241301001", // s
    "0646 2620", // t
    "060110304146", // u
    "062046", // v
    "0610233046", // w
    "0046 0640", // x
    "062346 2320", // y
    "06460040", // z
    "36262413222030", // {
    "2620", // |
    "16262433222010", // }
    "04153344", // ~
};

// The pattern for a byte the alphabet has none for: the tube's hollow box.
constexpr const char* const strokeBox = "0006464000";

int glyphStrokeRuns(const Projection& p, const Primitive& glyph, std::span<StrokeRun> out)
{
    if (glyph.kind != PrimitiveKind::Glyph || out.empty())
        return 0;
    const char* pattern = strokeBox;
    if (glyph.codePoint >= 0x20 && glyph.codePoint <= 0x7E)
        pattern = strokeAlphabet[glyph.codePoint - 0x20];
    const DevicePoint origin = project(p, glyph.x0, glyph.y0);
    const DeviceSize cell = charCellSize(p, static_cast<unsigned>(glyph.charSize));
    // The letterform takes seven tenths of the cell's width and six tenths of
    // its height, and stands on a baseline a tenth of the cell up from the
    // bottom the alpha cursor addresses; the rest is the space between
    // characters and lines that keeps a page of text readable.
    const float unitX = cell.w * 0.7f / 4.0f;
    const float unitY = cell.h * 0.6f / 6.0f;
    const float left = origin.x + cell.w * 0.1f;
    const float baseline = origin.y - cell.h * 0.12f;
    int written = 0;
    bool havePrevious = false;
    DevicePoint previous {};
    for (const char* c = pattern; *c != '\0'; ++c) {
        if (*c == ' ') {
            havePrevious = false;
            continue;
        }
        if (c[1] == '\0')
            break;
        const DevicePoint point { left + static_cast<float>(c[0] - '0') * unitX, baseline - static_cast<float>(c[1] - '0') * unitY };
        ++c;
        if (havePrevious) {
            if (written >= static_cast<int>(out.size()))
                break;
            out[static_cast<std::size_t>(written)] = { previous, point };
            ++written;
        }
        previous = point;
        havePrevious = true;
    }
    return written;
}

int buildStrokeRuns(std::span<const Primitive> primitives, const Projection& projection, std::span<StrokeRun> out)
{
    int written = 0;
    for (const Primitive& primitive : primitives) {
        if (written >= static_cast<int>(out.size()))
            break;
        switch (primitive.kind) {
        case PrimitiveKind::Line: {
            const DevicePoint start = project(projection, primitive.x0, primitive.y0);
            const DevicePoint end = project(projection, primitive.x1, primitive.y1);
            const DashPattern dash = dashForLineStyle(projection, static_cast<unsigned>(primitive.lineStyle));
            const int room = static_cast<int>(out.size()) - written;
            const int allowed = room < maxStrokeRunsPerLine ? room : maxStrokeRunsPerLine;
            written += dashRunsForSegment(start, end, dash, out.subspan(static_cast<std::size_t>(written), static_cast<std::size_t>(allowed)));
            break;
        }
        case PrimitiveKind::Point: {
            const DevicePoint at = project(projection, primitive.x0, primitive.y0);
            out[static_cast<std::size_t>(written)] = { at, at };
            ++written;
            break;
        }
        case PrimitiveKind::Glyph: {
            const int room = static_cast<int>(out.size()) - written;
            const int allowed = room < maxStrokeRunsPerGlyph ? room : maxStrokeRunsPerGlyph;
            written += glyphStrokeRuns(projection, primitive, out.subspan(static_cast<std::size_t>(written), static_cast<std::size_t>(allowed)));
            break;
        }
        }
    }
    return written;
}

} // namespace Ubiquitty::Tek
