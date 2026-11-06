// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/BoxGlyphRaster.cpp — the Linux/CPU sink for the shared
// box-drawing command builder (Rendering/BoxDrawCommands.h). All geometry +
// positioning + snapping lives in buildBoxGlyph; this just paints each emitted
// command (fillRect / stipple / triangle / capsule) into an R8 coverage buffer.
// Pure C++ (no platform types).

#include "Core/Rendering/BoxGlyphRaster.h"

#include "Core/Rendering/BoxDrawCommands.h"
#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/ShadeStyle.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace Ubiquitty::Rendering {

namespace {

std::uint8_t alphaToCoverage(float alpha)
{
    if (alpha <= 0.0f)
        return 0;
    if (alpha >= 1.0f)
        return 255;
    return static_cast<std::uint8_t>(alpha * 255.0f + 0.5f);
}

// Paints BoxDrawSink commands into a caller-owned slotW*slotH R8 coverage buffer.
class CoverageSink final : public BoxDrawSink {
public:
    CoverageSink(std::uint8_t* buf, int slotW, int slotH, ShadeStyle shadeStyle, int shadePhase)
        : m_buf(buf)
        , m_slotW(slotW)
        , m_slotH(slotH)
        , m_shadeStyle(shadeStyle)
        , m_shadePhase(shadePhase)
    {
    }

    void fillRect(float x0, float y0, float x1, float y1, float alpha) override { rect(x0, y0, x1, y1, alphaToCoverage(alpha)); }

    // Core's shade field (ShadeStyle.h). A slot is one glyph reused at every
    // column, so its phase is anchored to the cell box it holds: the pattern
    // continues across a cell edge only where the cell's pixel extent is a
    // multiple of the field's period (4 across, 2 down). The fill has no
    // phase and tiles at every size.
    void stipple(float fx0, float fy0, float fx1, float fy1, float density, ShadePolarity polarity) override
    {
        if (m_shadeStyle == ShadeStyle::Fill) {
            rect(fx0, fy0, fx1, fy1, alphaToCoverage(density));
            return;
        }
        const std::optional<PixelRect> clipped = clipRect(fx0, fy0, fx1, fy1);
        if (!clipped)
            return;
        const int lit = shadeLitCount(density);
        const int phase = (m_shadePhase + (polarity == ShadePolarity::Inverse ? inverseShadePhaseOffset : 0)) & 3;
        for (int y = clipped->y0; y < clipped->y1; ++y) {
            std::uint8_t* row = m_buf + static_cast<std::size_t>(y) * m_slotW;
            for (int x = clipped->x0; x < clipped->x1; ++x)
                row[x] = isShadePixelLit(x, y, lit, phase) ? 255 : 0;
        }
    }

    void triangle(float ax, float ay, float bx, float by, float cx, float cy, float alpha) override
    {
        // A translucent triangle is a triangular shade: the pattern lights
        // solid pixels of Core's field, the fill takes the alpha as coverage.
        const bool isPatternShade = alpha < 0.99f && m_shadeStyle == ShadeStyle::Pattern;
        const int lit = shadeLitCount(alpha);
        const std::uint8_t cov = isPatternShade ? 255 : alphaToCoverage(alpha);
        int minX = clampX(static_cast<int>(std::floor(std::fmin(ax, std::fmin(bx, cx)))));
        int maxX = clampX(static_cast<int>(std::ceil(std::fmax(ax, std::fmax(bx, cx)))));
        int minY = clampY(static_cast<int>(std::floor(std::fmin(ay, std::fmin(by, cy)))));
        int maxY = clampY(static_cast<int>(std::ceil(std::fmax(ay, std::fmax(by, cy)))));
        for (int y = minY; y < maxY; ++y) {
            std::uint8_t* row = m_buf + static_cast<std::size_t>(y) * m_slotW;
            for (int x = minX; x < maxX; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                const float w0 = edge(bx, by, cx, cy, px, py);
                const float w1 = edge(cx, cy, ax, ay, px, py);
                const float w2 = edge(ax, ay, bx, by, px, py);
                if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                    if (isPatternShade && !isShadePixelLit(x, y, lit, m_shadePhase))
                        continue;
                    row[x] = cov;
                }
            }
        }
    }

    void capsule(float ax, float ay, float bx, float by, float half, std::optional<FloatRect> clip) override
    {
        // ~1px feather past the half-width for antialiasing (smooth diagonals +
        // arc polylines instead of hard, jagged edges).
        const float pad = half + 1.0f;
        int x0 = clampX(static_cast<int>(std::floor(std::fmin(ax, bx) - pad)));
        int x1 = clampX(static_cast<int>(std::ceil(std::fmax(ax, bx) + pad)));
        int y0 = clampY(static_cast<int>(std::floor(std::fmin(ay, by) - pad)));
        int y1 = clampY(static_cast<int>(std::ceil(std::fmax(ay, by) + pad)));
        if (clip) {
            x0 = std::max(x0, clampX(static_cast<int>(std::ceil(clip->origin.x - 0.5f))));
            y0 = std::max(y0, clampY(static_cast<int>(std::ceil(clip->origin.y - 0.5f))));
            x1 = std::min(x1, clampX(static_cast<int>(std::ceil(clip->origin.x + clip->size.width - 0.5f))));
            y1 = std::min(y1, clampY(static_cast<int>(std::ceil(clip->origin.y + clip->size.height - 0.5f))));
        }
        const float dx = bx - ax;
        const float dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* row = m_buf + static_cast<std::size_t>(y) * m_slotW;
            for (int x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                float t = (len2 > 0.0f) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float qx = ax + t * dx;
                const float qy = ay + t * dy;
                const float dist = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
                // Coverage = 1 inside (half - 0.5), ramping to 0 over a 1px edge.
                const float cov = std::clamp(half + 0.5f - dist, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    const std::uint8_t c = static_cast<std::uint8_t>(cov * 255.0f + 0.5f);
                    if (c > row[x]) // max-blend: don't darken where strokes overlap
                        row[x] = c;
                }
            }
        }
    }

private:
    [[nodiscard]] int clampX(int x) const { return std::clamp(x, 0, m_slotW); }
    [[nodiscard]] int clampY(int y) const { return std::clamp(y, 0, m_slotH); }

    static float edge(float ax, float ay, float bx, float by, float px, float py) { return (px - ax) * (by - ay) - (py - ay) * (bx - ax); }

    // The clipped rect, or nothing when it is empty before or after clamping.
    // Returned whole because the four values are one answer: written through
    // four references, a caller could read x1 having only had x0 assigned.
    struct PixelRect {
        int x0;
        int y0;
        int x1;
        int y1;
    };

    [[nodiscard]] std::optional<PixelRect> clipRect(float fx0, float fy0, float fx1, float fy1) const
    {
        if (fx1 <= fx0 || fy1 <= fy0)
            return std::nullopt;
        const PixelRect clipped {
            clampX(static_cast<int>(std::lround(fx0))),
            clampY(static_cast<int>(std::lround(fy0))),
            clampX(static_cast<int>(std::lround(fx1))),
            clampY(static_cast<int>(std::lround(fy1))),
        };
        if (clipped.x1 <= clipped.x0 || clipped.y1 <= clipped.y0)
            return std::nullopt;
        return clipped;
    }

    void rect(float fx0, float fy0, float fx1, float fy1, std::uint8_t cov)
    {
        const std::optional<PixelRect> clipped = clipRect(fx0, fy0, fx1, fy1);
        if (!clipped)
            return;
        for (int y = clipped->y0; y < clipped->y1; ++y) {
            std::uint8_t* row = m_buf + static_cast<std::size_t>(y) * m_slotW;
            for (int x = clipped->x0; x < clipped->x1; ++x)
                row[x] = cov;
        }
    }

    std::uint8_t* m_buf;
    int m_slotW;
    int m_slotH;
    ShadeStyle m_shadeStyle;
    int m_shadePhase;
};

} // namespace

bool rasterizeBoxGlyph(unsigned codePoint, IntSize slot, IntSize cell, float emHeight, std::span<std::uint8_t> coverage, const BoxDrawing::BoxStrokeProfile* profile, ShadeStyle shadeStyle)
{
    // Probe first so a non-box codePoint leaves the buffer untouched (the caller
    // delegates to the font).
    if (!Rendering::BoxDrawing::covers(codePoint))
        return false;

    // The span's own extent, against the geometry it was allocated for. The
    // pointer form took the product as a separate argument and could not tell a
    // short buffer from a wide slot, so a mismatch wrote past the end.
    const std::size_t required = static_cast<std::size_t>(slot.width) * static_cast<std::size_t>(slot.height);
    if (coverage.size() < required)
        return false;

    std::memset(coverage.data(), 0, required);

    // The cell box is centred in the (possibly larger) overflow slot.
    const float left = static_cast<float>((slot.width - cell.width) / 2);
    const float top = static_cast<float>((slot.height - cell.height) / 2);

    const int cellLeft = (slot.width - cell.width) / 2;
    const int cellTop = (slot.height - cell.height) / 2;
    CoverageSink sink(coverage.data(), slot.width, slot.height, shadeStyle, shadePhase(cellLeft, cellTop));
    return buildBoxGlyph(codePoint,
        { { left, top }, { static_cast<float>(cell.width), static_cast<float>(cell.height) } }, emHeight, profile, sink);
}

} // namespace Ubiquitty::Rendering
