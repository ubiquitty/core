// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/BoxFillEmit.cpp — see BoxFillEmit.h. Plain Vec2/Vec4
// rather than simd types, so every backend emits the same quads and a
// divergence between them is a bug rather than a dialect.

#include "Core/Rendering/BoxFillEmit.h"

#include "Core/Rendering/BoxDrawCommands.h"

#include <cmath>

namespace Ubiquitty::Rendering {

namespace {

// Which counter a quad lands in. The geometry is identical either way — a
// triangle's scan-converted span is an ordinary rect — so the kind travels
// beside the call rather than being inferred from the shape.
enum class StatKind {
    Rect = 0,
    Shade = 1,
    Shape = 2,
    Triangle = 3,
    Arc = 4,
};

struct OverlayBoxDrawSink final : BoxDrawSink {
    OverlayInstance* out;
    int maxOut;
    // Counts every quad the glyph needs; writes stop at maxOut. Splitting the
    // two is what lets the caller's regrow check fail: a count clamped at the
    // capacity can never exceed it, and a retry keyed on exceeding never runs.
    int needed = 0;
    Vec4 foreground;
    // The cell rect, kept only to classify a fill as covering the whole cell or
    // part of it for the counters. The geometry is BoxDrawCommands'.
    float left, top, right, bottom;
    BoxFillStats* stats;
    std::optional<FloatRect> clip;
    ShadeField shade;

    OverlayBoxDrawSink(OverlayInstance* buffer, int capacity, Vec4 fg, float l, float t, float r, float b, BoxFillStats* s, std::optional<FloatRect> bounds, ShadeField shadeField)
        : out(buffer)
        , maxOut(capacity)
        , foreground(fg)
        , left(l)
        , top(t)
        , right(r)
        , bottom(b)
        , stats(s)
        , clip(bounds)
        , shade(shadeField)
    {
    }

    bool clipRect(float& x0, float& y0, float& x1, float& y1) const
    {
        if (clip) {
            x0 = std::fmax(x0, clip->origin.x);
            y0 = std::fmax(y0, clip->origin.y);
            x1 = std::fmin(x1, clip->origin.x + clip->size.width);
            y1 = std::fmin(y1, clip->origin.y + clip->size.height);
        }
        return x1 > x0 && y1 > y0;
    }

    void rect(float x0, float y0, float x1, float y1, float alpha, StatKind kind = StatKind::Rect)
    {
        if (alpha <= 0.0f)
            return;
        if (!clipRect(x0, y0, x1, y1))
            return;
        if (needed < maxOut) {
            OverlayInstance& quad = out[needed];
            quad = {};
            quad.topLeftInPixels = { x0, y0 };
            quad.bottomRightInPixels = { x1, y1 };
            quad.color = { foreground.x, foreground.y, foreground.z, foreground.w * alpha };
            quad.cornerRadius = 0.0f;
            quad.strokeWidth = 0.0f;
            quad.shadeDensity = 0.0f;
            quad.shapeKind = 0.0f; // ordinary axis-aligned fill
            quad.plA = { 0.0f, 0.0f, 0.0f, 0.0f };
            quad.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
        }
        if (stats) {
            if (kind == StatKind::Triangle)
                stats->triangleRects++;
            else if (kind == StatKind::Shade)
                stats->shadeQuads++;
            else if (kind == StatKind::Arc)
                stats->arcRects++;
            else if (alpha >= 0.99f && x0 <= left && y0 <= top && x1 >= right && y1 >= bottom)
                stats->fullRects++;
            else
                stats->partialRects++;
        }
        ++needed;
    }

    // The fragment stage dithers on the surface pixel offset by the grid's
    // phase, so a shaded field stitches across cell edges instead of
    // restarting its pattern at each one. A fill is a translucent rect.
    void shadeQuad(float x0, float y0, float x1, float y1, float density, StatKind kind = StatKind::Shade, int phaseOffset = 0)
    {
        if (density <= 0.0f)
            return;
        if (shade.style == ShadeStyle::Fill) {
            rect(x0, y0, x1, y1, density, kind);
            return;
        }
        if (!clipRect(x0, y0, x1, y1))
            return;
        if (needed < maxOut) {
            OverlayInstance& quad = out[needed];
            quad = {};
            quad.topLeftInPixels = { x0, y0 };
            quad.bottomRightInPixels = { x1, y1 };
            quad.color = foreground;
            quad.cornerRadius = 0.0f;
            quad.strokeWidth = 0.0f;
            quad.shadeDensity = density;
            quad.shapeKind = 0.0f; // dither, not an SDF shape
            quad.plA = { static_cast<float>((shade.phase + phaseOffset) & 3), 0.0f, 0.0f, 0.0f };
            quad.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
        }
        if (stats) {
            if (kind == StatKind::Triangle)
                stats->triangleRects++;
            else
                stats->shadeQuads++;
        }
        ++needed;
    }

    // One analytic quad whose edges the fragment stage antialiases from the SDF
    // in plA/plB, rather than approximating a diagonal or a curve with a stack of
    // axis-aligned strips. The parameters are in unit-quad space, so the shape is
    // independent of the quad's extent.
    void shapeQuad(float x0, float y0, float x1, float y1, const Rendering::BoxDrawing::PowerlineShape& shape)
    {
        const float originalX = x0, originalY = y0;
        const float originalWidth = x1 - x0, originalHeight = y1 - y0;
        if (!clipRect(x0, y0, x1, y1))
            return;
        auto adjusted = shape;
        if (clip) {
            // The shader interprets parameters relative to this quad. Moving
            // its edges without moving the parameters would squeeze a whole
            // corner or separator into each band instead of cutting it.
            const float scaleX = originalWidth / (x1 - x0);
            const float scaleY = originalHeight / (y1 - y0);
            const float offsetX = (originalX - x0) / (x1 - x0);
            const float offsetY = (originalY - y0) / (y1 - y0);
            const auto point = [&](int index) {
                adjusted.p[index] = shape.p[index] * scaleX + offsetX;
                adjusted.p[index + 1] = shape.p[index + 1] * scaleY + offsetY;
            };
            using Kind = BoxDrawing::PowerlineShapeKind;
            point(0);
            if (shape.kind == Kind::SolidHalfDisc || shape.kind == Kind::OutlineHalfDisc || shape.kind == Kind::EllipticalArc) {
                adjusted.p[2] *= scaleX;
                adjusted.p[3] *= scaleY;
                if (shape.kind == Kind::EllipticalArc) {
                    adjusted.p[4] *= scaleX;
                    adjusted.p[5] *= scaleY;
                } else if (shape.kind == Kind::OutlineHalfDisc)
                    adjusted.p[5] *= scaleY;
            } else {
                point(2);
                point(4);
                adjusted.p[6] *= scaleY;
            }
        }
        if (needed < maxOut) {
            OverlayInstance& quad = out[needed];
            quad = {};
            quad.topLeftInPixels = { x0, y0 };
            quad.bottomRightInPixels = { x1, y1 };
            quad.color = foreground;
            quad.cornerRadius = 0.0f;
            quad.strokeWidth = 0.0f;
            quad.shadeDensity = 0.0f;
            quad.shapeKind = static_cast<float>(shape.kind);
            quad.plA = { adjusted.p[0], adjusted.p[1], adjusted.p[2], adjusted.p[3] };
            quad.plB = { adjusted.p[4], adjusted.p[5], adjusted.p[6], adjusted.p[7] };
        }
        if (stats)
            stats->shapeQuads++;
        ++needed;
    }

    // One mosaic-mask quad (2x3 sextant). shapeKind -1 selects the fragment's
    // mask sampler; plA carries the mask and its grid.
    void mosaicQuad(float x0, float y0, float x1, float y1, std::uint32_t mask, int columns, int rows)
    {
        if (x1 <= x0 || y1 <= y0)
            return;
        if (clip) {
            // A mask quad's sample coordinates span its whole rect. Emit its
            // occupied tiles before clipping so a cut never restarts the mask.
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < columns; ++column) {
                    if (!(mask & (1u << (row * columns + column))))
                        continue;
                    rect(x0 + (x1 - x0) * column / columns, y0 + (y1 - y0) * row / rows,
                        x0 + (x1 - x0) * (column + 1) / columns, y0 + (y1 - y0) * (row + 1) / rows, 1.0f);
                }
            }
            return;
        }
        if (needed < maxOut) {
            OverlayInstance& quad = out[needed];
            quad = {};
            quad.topLeftInPixels = { x0, y0 };
            quad.bottomRightInPixels = { x1, y1 };
            quad.color = foreground;
            quad.cornerRadius = 0.0f;
            quad.strokeWidth = 0.0f;
            quad.shadeDensity = 0.0f;
            quad.shapeKind = -1.0f;
            quad.plA = { static_cast<float>(mask), static_cast<float>(columns), static_cast<float>(rows), 0.0f };
            quad.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
        }
        if (stats)
            stats->mosaicQuads++;
        ++needed;
    }

    void fillRect(float x0, float y0, float x1, float y1, float alpha) override { rect(x0, y0, x1, y1, alpha); }

    void stipple(float x0, float y0, float x1, float y1, float density, ShadePolarity polarity) override
    {
        shadeQuad(x0, y0, x1, y1, density, StatKind::Shade, polarity == ShadePolarity::Inverse ? inverseShadePhaseOffset : 0);
    }

    void triangle(float ax, float ay, float bx, float by, float cx, float cy, float alpha) override
    {
        // Scan-converted per device row into one span, so a triangle costs the
        // same pass as a rect. Sampling at the row CENTRE is what keeps two
        // triangles meeting along an edge from double-covering or gapping it.
        const float vx[3] = { ax, bx, cx };
        const float vy[3] = { ay, by, cy };
        float lowestY = vy[0], highestY = vy[0];
        for (int k = 1; k < 3; ++k) {
            if (vy[k] < lowestY)
                lowestY = vy[k];
            if (vy[k] > highestY)
                highestY = vy[k];
        }
        const int firstRow = static_cast<int>(std::floor(lowestY));
        const int lastRow = static_cast<int>(std::ceil(highestY));
        const bool shouldDither = alpha < 0.99f;
        // Runs to the last row even with the buffer full: the rows past the
        // capacity are what the returned count is for, and a loop that stopped
        // at the capacity would hand the caller a count that cannot exceed it.
        for (int y = firstRow; y < lastRow; ++y) {
            const float rowCentre = y + 0.5f;
            float crossings[2];
            int crossingCount = 0;
            for (int edge = 0; edge < 3 && crossingCount < 2; ++edge) {
                const float ya = vy[edge], yb = vy[(edge + 1) % 3];
                const float xa = vx[edge], xb = vx[(edge + 1) % 3];
                // Half-open in y so a vertex shared by two edges is counted once.
                if ((rowCentre >= ya && rowCentre < yb) || (rowCentre >= yb && rowCentre < ya)) {
                    const float t = (rowCentre - ya) / (yb - ya);
                    crossings[crossingCount++] = xa + t * (xb - xa);
                }
            }
            if (crossingCount < 2)
                continue;
            const float lowX = crossings[0] < crossings[1] ? crossings[0] : crossings[1];
            const float highX = crossings[0] < crossings[1] ? crossings[1] : crossings[0];
            const float spanLeft = std::round(lowX), spanRight = std::round(highX);
            if (spanRight <= spanLeft)
                continue;
            if (!shouldDither)
                rect(spanLeft, static_cast<float>(y), spanRight, static_cast<float>(y + 1), 1.0f, StatKind::Triangle);
            else
                shadeQuad(spanLeft, static_cast<float>(y), spanRight, static_cast<float>(y + 1), alpha, StatKind::Triangle);
        }
    }

    void ellipticalArc(FloatRect cell, FloatPoint center, FloatSize radii, FloatSize halfWidths) override
    {
        if (cell.size.width <= 0 || cell.size.height <= 0) {
            return;
        }
        BoxDrawing::PowerlineShape shape {};
        shape.kind = BoxDrawing::PowerlineShapeKind::EllipticalArc;
        shape.p[0] = (center.x - cell.origin.x) / cell.size.width;
        shape.p[1] = (center.y - cell.origin.y) / cell.size.height;
        shape.p[2] = radii.width / cell.size.width;
        shape.p[3] = radii.height / cell.size.height;
        shape.p[4] = halfWidths.width / cell.size.width;
        shape.p[5] = halfWidths.height / cell.size.height;
        shape.p[6] = center.x > cell.origin.x + cell.size.width * 0.5f ? -1.0f : 1.0f;
        shape.p[7] = center.y > cell.origin.y + cell.size.height * 0.5f ? -1.0f : 1.0f;
        shapeQuad(cell.origin.x, cell.origin.y, cell.origin.x + cell.size.width, cell.origin.y + cell.size.height, shape);
    }

    void capsule(float ax, float ay, float bx, float by, float half, std::optional<FloatRect> clip) override
    {
        // The SDF keeps the full segment while its quad bounds apply the clip.
        // Diagonals extend their caps beyond that quad to preserve join width.
        const float bleed = half + 1.0f;
        float qx0 = (ax < bx ? ax : bx) - bleed;
        float qy0 = (ay < by ? ay : by) - bleed;
        float qx1 = (ax > bx ? ax : bx) + bleed;
        float qy1 = (ay > by ? ay : by) + bleed;
        if (clip) {
            qx0 = std::fmax(qx0, clip->origin.x);
            qy0 = std::fmax(qy0, clip->origin.y);
            qx1 = std::fmin(qx1, clip->origin.x + clip->size.width);
            qy1 = std::fmin(qy1, clip->origin.y + clip->size.height);
        }
        const float quadWidth = qx1 - qx0;
        const float quadHeight = qy1 - qy0;
        if (quadWidth <= 0.0f || quadHeight <= 0.0f)
            return;
        Rendering::BoxDrawing::PowerlineShape shape {};
        shape.kind = Rendering::BoxDrawing::PowerlineShapeKind::OutlineTriangle;
        shape.p[0] = (ax - qx0) / quadWidth;
        shape.p[1] = (ay - qy0) / quadHeight;
        shape.p[2] = (bx - qx0) / quadWidth;
        shape.p[3] = (by - qy0) / quadHeight;
        shape.p[4] = shape.p[2];
        shape.p[5] = shape.p[3]; // c == b makes it a capsule rather than a chevron
        shape.p[6] = half / quadHeight; // half-weight, unit-Y
        shape.p[7] = 0.0f;
        shapeQuad(qx0, qy0, qx1, qy1, shape);
    }
};

} // namespace

int emitBoxFill(std::uint32_t codePoint, float left, float top, float right, float bottom,
    Vec4 foreground, OverlayInstance* out, int maxOut,
    const Rendering::BoxDrawing::PowerlineShape* powerline, BoxFillStats* stats,
    float referenceHeight, const Rendering::BoxDrawing::BoxStrokeProfile* profile, std::optional<FloatRect> clip, ShadeField shade)
{
    // A missing or full buffer still counts — the count is what sizes the
    // caller's regrow, so it must not depend on the room this call was given.
    OverlayBoxDrawSink sink(out, out ? maxOut : 0, foreground, left, top, right, bottom, stats, clip, shade);
    // Line spacing changes the cell height, not the face's stroke weight. Solid
    // Powerline shapes still span that whole height, while an outline keeps its
    // half-width tied to the natural line height just like a box rule does.
    const float cellHeight = bottom - top;
    const float strokeBasis = referenceHeight > 0.0f ? referenceHeight : cellHeight;

    // Both specials resolve the whole cell to one quad, so they are checked
    // before the decomposition rather than after it.
    if (powerline && powerline->kind != Rendering::BoxDrawing::PowerlineShapeKind::None) {
        Rendering::BoxDrawing::PowerlineShape adjusted = *powerline;
        const bool isOutlineStroke = adjusted.kind == Rendering::BoxDrawing::PowerlineShapeKind::OutlineWedge
            || adjusted.kind == Rendering::BoxDrawing::PowerlineShapeKind::OutlineDiagonal
            || adjusted.kind == Rendering::BoxDrawing::PowerlineShapeKind::OutlineHalfDisc;
        const int strokeSlot = adjusted.kind == Rendering::BoxDrawing::PowerlineShapeKind::OutlineHalfDisc ? 5 : 6;
        // The thin separators take the FACE's own light rule weight where the
        // face ships one, so a chevron sits beside the font's box lines at the
        // font designer's thickness rather than the em-derived fallback's. The
        // measured band is a fraction of the cell width, which line spacing
        // never changes — the width-derived weight is stable under spacing by
        // construction, where the em rule needs the reference-height rescale.
        const float cellWidth = right - left;
        if (isOutlineStroke && profile && profile->lightVertical.isValid
            && cellHeight > 0.0f && cellWidth > 0.0f) {
            adjusted.p[strokeSlot] = profile->lightVertical.thicknessInCell * cellWidth * 0.5f / cellHeight;
        } else if (isOutlineStroke && cellHeight > 0.0f && strokeBasis != cellHeight) {
            adjusted.p[strokeSlot] *= strokeBasis / cellHeight;
        }
        sink.shapeQuad(left, top, right, bottom, adjusted);
        if (stats && sink.needed > 0)
            stats->sourceCells++;
        return sink.needed;
    }

    Rendering::BoxDrawing::MosaicMask mosaic;
    if (Rendering::BoxDrawing::mosaicMaskForCodePoint(codePoint, &mosaic)) {
        sink.mosaicQuad(left, top, right, bottom, mosaic.mask, mosaic.columns, mosaic.rows);
        if (stats && sink.needed > 0)
            stats->sourceCells++;
        return sink.needed;
    }

    // The stroke basis: the reference line height when the caller threaded it,
    // else the cell height as this path's own fallback. The instance path is
    // handed a rectangle and knows nothing about the type inside it, so keying
    // the stroke to the cell made a box rule thicken as a reader raised the line
    // spacing, while the raster path — taking the em from the face it wraps — did
    // not. A caller that passes the reference (the natural line height, before
    // the user's spacing widens the cell) keeps the two paths the same weight.
    buildBoxGlyph(codePoint, { { left, top }, { right - left, bottom - top } }, strokeBasis, profile, sink);
    if (stats && sink.needed > 0)
        stats->sourceCells++;
    return sink.needed;
}

} // namespace Ubiquitty::Rendering
