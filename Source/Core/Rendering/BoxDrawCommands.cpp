// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/BoxDrawCommands.cpp — the shared box-drawing geometry
// builder. Platform sinks only paint the emitted commands.

#include "Core/Rendering/BoxDrawCommands.h"

#include "Core/Rendering/BoxDrawing.h"

#include <cmath>

namespace Ubiquitty::Rendering {

float ellipticalArcDistance(FloatPoint point, FloatPoint center, FloatSize radii, FloatSize halfWidths, FloatPoint direction)
{
    const auto distance = [&](float radiusX, float radiusY) {
        radiusX = std::fmax(radiusX, 0.001f);
        radiusY = std::fmax(radiusY, 0.001f);
        const float x = std::fmax((point.x - center.x) * direction.x, 0.0f) / radiusX;
        const float y = std::fmax((point.y - center.y) * direction.y, 0.0f) / radiusY;
        const float length = std::fmax(std::hypot(x, y), 0.0001f);
        const float gradient = std::fmax(std::hypot(x / radiusX, y / radiusY), 0.000001f);
        return (length - 1.0f) * length / gradient;
    };
    const float outer = distance(radii.width + halfWidths.width, radii.height + halfWidths.height);
    if (radii.width <= halfWidths.width || radii.height <= halfWidths.height) {
        return outer;
    }
    return std::fmax(outer, -distance(radii.width - halfWidths.width, radii.height - halfWidths.height));
}

void BoxDrawSink::rasterizeEllipticalArc(FloatRect cell, FloatPoint center, FloatSize radii, FloatSize halfWidths)
{
    const FloatPoint direction { center.x > cell.origin.x + cell.size.width * 0.5f ? -1.0f : 1.0f,
        center.y > cell.origin.y + cell.size.height * 0.5f ? -1.0f : 1.0f };
    const int right = static_cast<int>(std::ceil(cell.origin.x + cell.size.width));
    const int bottom = static_cast<int>(std::ceil(cell.origin.y + cell.size.height));
    for (int y = static_cast<int>(std::floor(cell.origin.y)); y < bottom; ++y) {
        for (int x = static_cast<int>(std::floor(cell.origin.x)); x < right; ++x) {
            const float coverage = std::fmin(1.0f, 0.5f - ellipticalArcDistance({ x + 0.5f, y + 0.5f }, center, radii, halfWidths, direction));
            if (coverage > 0.0f) {
                fillRect(std::fmax(x, cell.origin.x), std::fmax(y, cell.origin.y),
                    std::fmin(x + 1, cell.origin.x + cell.size.width), std::fmin(y + 1, cell.origin.y + cell.size.height), coverage);
            }
        }
    }
}

namespace {

/// Which of a join's four edges carry a double rule.
struct DoubleEdges {
    bool hasUp = false;
    bool hasDown = false;
    bool hasLeft = false;
    bool hasRight = false;

    /// - Returns: True when either vertical edge is a double rule.
    [[nodiscard]] bool hasVertical() const { return hasUp || hasDown; }
    /// - Returns: True when either horizontal edge is a double rule.
    [[nodiscard]] bool hasHorizontal() const { return hasLeft || hasRight; }
};

/// Emits the rails of a double-rule join, each stopping where it meets the rail
/// it turns into.
///
/// A double rule is two thin rails, so a JOIN of two of them is four rails and
/// two corners — an outer and an inner — and where each rail stops is a fact
/// about the whole join rather than about one edge. A stub emitter that draws
/// each direction from the cell edge to the centre cannot express it: it leaves
/// `╔`'s verticals hanging in space beside its horizontals, and fills `╬`'s
/// middle where the glyph has a hole.
///
/// Mixed weights — `╞`, `╥`, the U+2552..2569 block — terminate against the
/// light band instead. The perpendicular edge is a single rule there, so there
/// is no outer and inner rail to pair with, and reaching that band's far edge is
/// what closes the join for the same reason it does between two light stubs.
///
/// - Parameters:
///   - edges: Which directions carry a double rule.
///   - cell: The cell rect, device pixels.
///   - lightCenter: The single-rule bands' centre — where a rail terminating
///     against a light perpendicular finds it.
///   - railCenter: The centre the rail pairs straddle, which the font's own
///     double rules may cut off the light centre.
///   - lightVerticalThickness, lightHorizontalThickness: The single rule per
///     axis, for the bands mixed joins stop against.
///   - rail: One rail's thickness.
///   - gap: The clear space between a pair's rails.
///   - sink: Receives the rails.
void emitDoubleRails(DoubleEdges edges, FloatRect cell, FloatPoint lightCenter, FloatPoint railCenter,
    float lightVerticalThickness, float lightHorizontalThickness, float rail, float gap, BoxDrawSink& sink)
{
    // Bound to scalars once, as buildBoxGlyph does, so the rail arithmetic
    // below reads plain names.
    const float left = cell.origin.x;
    const float top = cell.origin.y;
    const float right = cell.origin.x + cell.size.width;
    const float bottom = cell.origin.y + cell.size.height;
    const float cx = railCenter.x;
    const float cy = railCenter.y;
    const float railLeftLow = cx - gap - rail, railLeftHigh = cx - gap;
    const float railRightLow = cx + gap, railRightHigh = cx + gap + rail;
    const float railTopLow = cy - gap - rail, railTopHigh = cy - gap;
    const float railBottomLow = cy + gap, railBottomHigh = cy + gap + rail;
    // Where a rail stops against a SINGLE perpendicular rule: that rule's far
    // edge, so the join closes rather than butting short of it.
    const float lightHorizontalHalf = std::fmax(std::round(lightHorizontalThickness * 0.5f), 0.5f);
    const float lightVerticalHalf = std::fmax(std::round(lightVerticalThickness * 0.5f), 0.5f);
    const float lightBandBottom = lightCenter.y + lightHorizontalHalf;
    const float lightBandTop = lightBandBottom - std::fmax(lightHorizontalThickness, 1.0f);
    const float lightBandRight = lightCenter.x + lightVerticalHalf;
    const float lightBandLeft = lightBandRight - std::fmax(lightVerticalThickness, 1.0f);

    if (edges.hasVertical()) {
        for (int rail = 0; rail < 2; ++rail) {
            const float railLow = rail == 0 ? railLeftLow : railRightLow;
            const float railHigh = rail == 0 ? railLeftHigh : railRightHigh;
            // Decided by where the PERPENDICULAR rule heads: a corner's outer side
            // is the one it turns away from, so `╔` heading right makes the left
            // rail outer and `╗` heading left makes the right rail outer. Reading
            // the vertical's own direction gets two of the four corners right,
            // which is enough to look like a rounding artefact.
            const bool isOuterRail = rail == 0 ? edges.hasRight : edges.hasLeft;
            const bool isTee = edges.hasLeft && edges.hasRight;
            if (edges.hasUp && edges.hasDown) {
                // A branch splits the rail on the side it branches toward; the far
                // rail runs through, which is what makes `╠` read as a rule with a
                // tee on it rather than as a lattice.
                const bool isSplit = rail == 0 ? edges.hasLeft : edges.hasRight;
                if (isSplit && edges.hasHorizontal()) {
                    // Cut at the FAR edge of the rail it crosses, so the square
                    // where the two rails actually overlap is filled. Cutting at
                    // the near edge leaves that square to the horizontal, which
                    // cuts at its own near edge for the same reason — and the
                    // corner of every quadrant of a `╬` comes out hollow.
                    sink.fillRect(railLow, top, railHigh, railTopHigh, 1.0f);
                    sink.fillRect(railLow, railBottomLow, railHigh, bottom, 1.0f);
                } else {
                    sink.fillRect(railLow, top, railHigh, bottom, 1.0f);
                }
            } else if (edges.hasDown) {
                // A TEE has no outer rail. Outer and inner name the two sides of a
                // turn, so they exist only where the perpendicular rule goes one
                // way: at `╦` both horizontals run through, and a rail carried up
                // to the far one crosses the rule it is supposed to hang from.
                const float stop = !edges.hasHorizontal() ? lightBandTop
                    : isTee                               ? railBottomLow
                    : isOuterRail                         ? railTopLow
                                                          : railBottomLow;
                sink.fillRect(railLow, stop, railHigh, bottom, 1.0f);
            } else {
                const float stop = !edges.hasHorizontal() ? lightBandBottom
                    : isTee                               ? railTopHigh
                    : isOuterRail                         ? railBottomHigh
                                                          : railTopHigh;
                sink.fillRect(railLow, top, railHigh, stop, 1.0f);
            }
        }
    }

    if (edges.hasHorizontal()) {
        for (int rail = 0; rail < 2; ++rail) {
            const float railLow = rail == 0 ? railTopLow : railBottomLow;
            const float railHigh = rail == 0 ? railTopHigh : railBottomHigh;
            const bool isOuterRail = rail == 0 ? edges.hasDown : edges.hasUp;
            const bool isTee = edges.hasUp && edges.hasDown;
            if (edges.hasLeft && edges.hasRight) {
                const bool isSplit = rail == 0 ? edges.hasUp : edges.hasDown;
                if (isSplit && edges.hasVertical()) {
                    sink.fillRect(left, railLow, railLeftLow, railHigh, 1.0f);
                    sink.fillRect(railRightHigh, railLow, right, railHigh, 1.0f);
                } else {
                    sink.fillRect(left, railLow, right, railHigh, 1.0f);
                }
            } else if (edges.hasRight) {
                const float stop = !edges.hasVertical() ? lightBandLeft
                    : isTee                             ? railRightLow
                    : isOuterRail                       ? railLeftLow
                                                        : railRightLow;
                sink.fillRect(stop, railLow, right, railHigh, 1.0f);
            } else {
                const float stop = !edges.hasVertical() ? lightBandRight
                    : isTee                             ? railLeftHigh
                    : isOuterRail                       ? railRightHigh
                                                        : railLeftHigh;
                sink.fillRect(left, railLow, stop, railHigh, 1.0f);
            }
        }
    }
}

} // namespace

bool buildBoxGlyph(unsigned codePoint, FloatRect cell, float strokeBasis, const Rendering::BoxDrawing::BoxStrokeProfile* profile, BoxDrawSink& sink)
{
    // Bound to scalars once, so the geometry below reads plain names; a member
    // path at every use is a place to swap one axis for the other.
    const float L = cell.origin.x;
    const float T = cell.origin.y;
    const float R = cell.origin.x + cell.size.width;
    const float B = cell.origin.y + cell.size.height;
    Rendering::BoxDrawing::Glyph g;
    const int count = Rendering::BoxDrawing::decompose(codePoint, &g);
    if (count <= 0)
        return false;

    const float cellWidth = R - L;
    const float cellHeight = B - T;

    // Stroke weights: light ~ em/12, heavy ~2x (>= light+1).
    //
    // The em and not the cell. The cell carries the line spacing, so deriving
    // the weight from it made a light rule two pixels at the default spacing of
    // 2 and one at zero — box lines thickening as a reader raises their line
    // height, and solid strokes twice the weight of the dashes the font draws
    // beside them.
    const float weightBasis = strokeBasis > 0.0f ? strokeBasis : cellHeight;
    float light = std::round(weightBasis / 12.0f);
    if (light < 1.0f)
        light = 1.0f;
    float heavy = std::round(light * 2.0f);
    if (heavy < light + 1.0f)
        heavy = light + 1.0f;

    // Pixel-snapped cell centre: opposite stubs of a join meet here, and a
    // vertical/horizontal line keeps the same column/row across stacked cells.
    const float cx = std::round((L + R) * 0.5f);
    const float cy = std::round((T + B) * 0.5f);

    // The font's measured bands override the em rule per stroke class,
    // thickness and centreline both, so a healed character shares the exact
    // geometry of the strokes the font draws itself — that shared geometry
    // is what makes the two sources stitch. Each class falls back alone.
    float lightHorizontalThickness = light, lightVerticalThickness = light;
    float heavyHorizontalThickness = heavy, heavyVerticalThickness = heavy;
    float lightCx = cx, lightCy = cy, heavyCx = cx, heavyCy = cy;
    // The measured band twice over: rounded for the stub machinery, whose
    // whole-pixel bands are what every join closes against, and EXACT for
    // the edge-line family, which draws the fractional weight the reference
    // actually carries — full rows plus alpha edges, the shape the font's
    // own rasteriser gives its rules. Rounding a 1.6px measurement to two
    // full rows is how a healed elbow ends up heavier than the rule it
    // continues.
    float lightHorizontalThicknessExact = light, lightVerticalThicknessExact = light;
    float heavyHorizontalThicknessExact = heavy, heavyVerticalThicknessExact = heavy;
    float lightCxExact = cx, lightCyExact = cy, heavyCxExact = cx, heavyCyExact = cy;
    if (profile) {
        auto adopt = [](const Rendering::BoxDrawing::BoxStrokeBand& band, float extent, float base,
                         float& thickness, float& center, float& thicknessExact, float& centerExact) {
            if (!band.isValid)
                return;
            thicknessExact = std::fmax(1.0f, band.thicknessInCell * extent);
            centerExact = base + band.centerInCell * extent;
            thickness = std::fmax(1.0f, std::round(thicknessExact));
            center = std::round(centerExact);
        };
        adopt(profile->lightHorizontal, cellHeight, T, lightHorizontalThickness, lightCy, lightHorizontalThicknessExact, lightCyExact);
        adopt(profile->lightVertical, cellWidth, L, lightVerticalThickness, lightCx, lightVerticalThicknessExact, lightCxExact);
        adopt(profile->heavyHorizontal, cellHeight, T, heavyHorizontalThickness, heavyCy, heavyHorizontalThicknessExact, heavyCyExact);
        adopt(profile->heavyVertical, cellWidth, L, heavyVerticalThickness, heavyCx, heavyVerticalThicknessExact, heavyCxExact);
    }

    auto strokeBand = [](float center, float thick, float& lo, float& hi) {
        float half = std::round(thick * 0.5f);
        if (half < 0.5f)
            half = 0.5f;
        lo = center - half;
        hi = center + (thick - half);
        if (hi - lo < 1.0f)
            hi = lo + 1.0f;
    };

    // The bands the two centre strokes occupy, which every stub needs and none of
    // them changes. Hoisted because they depend on the WEIGHT and not on the
    // primitive: computing them per stub asks `strokeBand` the same two questions
    // four times over for a cross.
    float lightVerticalLow, lightVerticalHigh, lightHorizontalLow, lightHorizontalHigh;
    strokeBand(lightCx, lightVerticalThickness, lightVerticalLow, lightVerticalHigh);
    strokeBand(lightCy, lightHorizontalThickness, lightHorizontalLow, lightHorizontalHigh);
    float heavyVerticalLow, heavyVerticalHigh, heavyHorizontalLow, heavyHorizontalHigh;
    strokeBand(heavyCx, heavyVerticalThickness, heavyVerticalLow, heavyVerticalHigh);
    strokeBand(heavyCy, heavyHorizontalThickness, heavyHorizontalLow, heavyHorizontalHigh);

    // Accumulated by the same pass that emits everything else, then drawn once
    // after it: where a double rail stops depends on the other three edges, and a
    // separate pass to discover them would walk the primitives of every block,
    // shade and quadrant to learn that none of them has any.
    DoubleEdges doubleEdges;

    for (int i = 0; i < count; ++i) {
        const Rendering::BoxDrawing::Primitive& p = g.prim[i];
        switch (p.kind) {
        case Rendering::BoxDrawing::PrimitiveKind::Rect: {
            // Interior splits round to a pixel; edges anchored at 0/1 snap exactly
            // to the cell edge so paired half/quadrant cells meet with no gap.
            float x0 = std::round(L + p.x0 * cellWidth);
            float x1 = std::round(L + p.x1 * cellWidth);
            float y0 = std::round(T + p.y0 * cellHeight);
            float y1 = std::round(T + p.y1 * cellHeight);
            if (p.x0 == 0.0f)
                x0 = L;
            if (p.x1 == 1.0f)
                x1 = R;
            if (p.y0 == 0.0f)
                y0 = T;
            if (p.y1 == 1.0f)
                y1 = B;
            sink.fillRect(x0, y0, x1, y1, p.alpha);
            break;
        }
        case Rendering::BoxDrawing::PrimitiveKind::Stipple: {
            const float x0 = std::round(L + p.x0 * cellWidth);
            const float x1 = std::round(L + p.x1 * cellWidth);
            const float y0 = std::round(T + p.y0 * cellHeight);
            const float y1 = std::round(T + p.y1 * cellHeight);
            sink.stipple(x0, y0, x1, y1, p.alpha, p.polarity);
            break;
        }
        case Rendering::BoxDrawing::PrimitiveKind::Triangle:
            sink.triangle(L + p.x0 * cellWidth, T + p.y0 * cellHeight, L + p.x1 * cellWidth, T + p.y1 * cellHeight, L + p.x2 * cellWidth, T + p.y2 * cellHeight, p.alpha);
            break;
        case Rendering::BoxDrawing::PrimitiveKind::Stub: {
            if (p.weight == Rendering::BoxDrawing::Weight::Double) {
                // Recorded, not drawn: a rail's extent is a fact about the whole
                // join, so the four are emitted together once this pass ends.
                switch (p.direction) {
                case Rendering::BoxDrawing::Direction::Up:
                    doubleEdges.hasUp = true;
                    break;
                case Rendering::BoxDrawing::Direction::Down:
                    doubleEdges.hasDown = true;
                    break;
                case Rendering::BoxDrawing::Direction::Left:
                    doubleEdges.hasLeft = true;
                    break;
                case Rendering::BoxDrawing::Direction::Right:
                    doubleEdges.hasRight = true;
                    break;
                }
            } else {
                // A stub ends at the far edge of the band its PERPENDICULAR
                // partner occupies, not at the centre point.
                //
                // `strokeBand` is asymmetric about the centre — a 3px stroke is
                // `[c-2, c+1]` — so two stubs stopping at `cx` and `cy` leave the
                // square `[cx-2, cx] x [cy-2, cy]` covered by neither, and every
                // corner and tee is drawn with a notch bitten out of it. Reaching
                // the partner's far edge fills that square exactly once; a stub
                // whose partner is absent overshoots the centre by at most the
                // stroke, which is what a stroke's own end cap is.
                const bool isHeavy = p.weight == Rendering::BoxDrawing::Weight::Heavy;
                const float verticalLow = isHeavy ? heavyVerticalLow : lightVerticalLow;
                const float verticalHigh = isHeavy ? heavyVerticalHigh : lightVerticalHigh;
                const float horizontalLow = isHeavy ? heavyHorizontalLow : lightHorizontalLow;
                const float horizontalHigh = isHeavy ? heavyHorizontalHigh : lightHorizontalHigh;
                // An inset stub stops short of the join because the shape it
                // draws has no join to reach — the `⌐` fragments and the padlock
                // shackle end in mid-cell — so it is measured from the centre and
                // never extended to a partner that is not there.
                const bool isInset = p.inset > 0.0f;
                const float insetX = p.inset * (cx - L);
                const float insetY = p.inset * (cy - T);
                if (p.direction == Rendering::BoxDrawing::Direction::Up)
                    sink.fillRect(verticalLow, T, verticalHigh, isInset ? cy - insetY : horizontalHigh, 1.0f);
                else if (p.direction == Rendering::BoxDrawing::Direction::Down)
                    sink.fillRect(verticalLow, isInset ? cy + insetY : horizontalLow, verticalHigh, B, 1.0f);
                else if (p.direction == Rendering::BoxDrawing::Direction::Left)
                    sink.fillRect(L, horizontalLow, isInset ? cx - insetX : verticalHigh, horizontalHigh, 1.0f);
                else
                    sink.fillRect(isInset ? cx + insetX : verticalLow, horizontalLow, R, horizontalHigh, 1.0f);
            }
            break;
        }
        case Rendering::BoxDrawing::PrimitiveKind::Arc: {
            const bool isHeavy = p.weight == Rendering::BoxDrawing::Weight::Heavy;
            const bool hasVerticalProfile = profile && (isHeavy ? profile->heavyVertical : profile->lightVertical).isValid;
            const bool hasHorizontalProfile = profile && (isHeavy ? profile->heavyHorizontal : profile->lightHorizontal).isValid;
            const float verticalLow = isHeavy ? heavyVerticalLow : lightVerticalLow;
            const float verticalHigh = isHeavy ? heavyVerticalHigh : lightVerticalHigh;
            const float horizontalLow = isHeavy ? heavyHorizontalLow : lightHorizontalLow;
            const float horizontalHigh = isHeavy ? heavyHorizontalHigh : lightHorizontalHigh;
            const float centerX = hasVerticalProfile ? (isHeavy ? heavyCxExact : lightCxExact) : (verticalLow + verticalHigh) * 0.5f;
            const float centerY = hasHorizontalProfile ? (isHeavy ? heavyCyExact : lightCyExact) : (horizontalLow + horizontalHigh) * 0.5f;
            const float verticalWidth = hasVerticalProfile ? (isHeavy ? heavyVerticalThicknessExact : lightVerticalThicknessExact) : verticalHigh - verticalLow;
            const float horizontalWidth = hasHorizontalProfile ? (isHeavy ? heavyHorizontalThicknessExact : lightHorizontalThicknessExact) : horizontalHigh - horizontalLow;
            // Equal device-pixel radii keep the turn circular at any cell aspect
            // ratio. The clamped arc distance extends both tangents to the edges.
            const float insetX = std::fmin(1.0f, cellWidth * 0.125f);
            const float insetY = std::fmin(1.0f, cellHeight * 0.125f);
            const float directionX = p.x0 > 0.5f ? 1.0f : -1.0f;
            const float directionY = p.y0 > 0.5f ? 1.0f : -1.0f;
            const float availableX = directionX > 0 ? R - centerX : centerX - L;
            const float availableY = directionY > 0 ? B - centerY : centerY - T;
            const float radius = std::fmax(0.001f, std::fmin(availableX - insetX, availableY - insetY));
            const FloatPoint center { centerX + directionX * radius, centerY + directionY * radius };
            sink.ellipticalArc({ { L, T }, { cellWidth, cellHeight } }, center,
                { radius, radius },
                { verticalWidth * 0.5f, horizontalWidth * 0.5f });
            break;
        }
        case Rendering::BoxDrawing::PrimitiveKind::Diagonal: {
            const float thick = (p.weight == Rendering::BoxDrawing::Weight::Heavy)
                ? (heavyVerticalThickness + heavyHorizontalThickness) * 0.5f
                : (lightVerticalThickness + lightHorizontalThickness) * 0.5f;
            // A 45° capsule of the same perpendicular width as an orthogonal
            // stroke reads heavier — its antialiased edge spans more pixels — so
            // thin it to match the font's diagonal weight.
            const float half = std::fmax(0.5f, thick * 0.5f * 0.8f);
            const float ax = L + p.x0 * cellWidth;
            const float ay = T + p.y0 * cellHeight;
            const float bx = L + p.x1 * cellWidth;
            const float by = T + p.y1 * cellHeight;
            // The caps lie outside the row span. Clipping at its horizontal
            // boundaries partitions a continuous stroke between adjoining rows:
            // no narrowed cap at the join and no double-blended feather there.
            const float runX = bx - ax;
            const float runY = by - ay;
            const float height = std::abs(runY);
            if (height == 0.0f) {
                break;
            }
            const float length = std::hypot(runX, runY);
            const float extension = (half + 1.0f) / height;
            const float sideBleed = (half + 1.0f) * length / height;
            const FloatRect clip {
                { std::fmin(ax, bx) - sideBleed, std::fmin(ay, by) },
                { std::abs(runX) + 2.0f * sideBleed, height }
            };
            sink.capsule(ax - runX * extension, ay - runY * extension,
                bx + runX * extension, by + runY * extension, half, clip);
            break;
        }
        case Rendering::BoxDrawing::PrimitiveKind::EdgeLine: {
            const bool isVertical = p.direction == Rendering::BoxDrawing::Direction::Up;
            const bool isEdgeHeavy = p.weight == Rendering::BoxDrawing::Weight::Heavy;
            const float thickExact = isVertical
                ? (isEdgeHeavy ? heavyVerticalThicknessExact : lightVerticalThicknessExact)
                : (isEdgeHeavy ? heavyHorizontalThicknessExact : lightHorizontalThicknessExact);
            const float thickRounded = isVertical
                ? (isEdgeHeavy ? heavyVerticalThickness : lightVerticalThickness)
                : (isEdgeHeavy ? heavyHorizontalThickness : lightHorizontalThickness);
            // A centred line shares the stub band's centre exactly, so an
            // elbow stacked over a `│` continues it with no jog; a line at
            // any other position takes its own snap.
            float centerExact;
            float centerRounded;
            if (isVertical) {
                centerExact = p.x0 == 0.5f ? (isEdgeHeavy ? heavyCxExact : lightCxExact) : L + p.x0 * cellWidth;
                centerRounded = p.x0 == 0.5f ? (isEdgeHeavy ? heavyCx : lightCx) : std::round(L + p.x0 * cellWidth);
            } else {
                centerExact = p.y0 == 0.5f ? (isEdgeHeavy ? heavyCyExact : lightCyExact) : T + p.y0 * cellHeight;
                centerRounded = p.y0 == 0.5f ? (isEdgeHeavy ? heavyCy : lightCy) : std::round(T + p.y0 * cellHeight);
            }
            // A whole-pixel measurement degenerates to the stub arithmetic
            // bit for bit — a hard-edged reference (Unifont, the em rule)
            // must keep drawing exactly what it drew, and a procedural `│`
            // beside a procedural elbow must share one band. Only a
            // genuinely fractional measurement takes the alpha-edged form.
            // Within a thirty-secondth of a pixel counts as whole: the
            // values arrive through unit-cell floats, and a scan that
            // measured a hard edge must not land in the fractional form on
            // a rounding hair.
            constexpr float wholePixelSlack = 1.0f / 32.0f;
            const bool isWholePixel = std::fabs(thickExact - thickRounded) < wholePixelSlack
                && std::fabs(centerExact - centerRounded) < wholePixelSlack;
            float bandLow = 0.0f, bandHigh = 0.0f;
            if (isWholePixel)
                strokeBand(centerRounded, thickRounded, bandLow, bandHigh);
            else {
                bandLow = centerExact - thickExact * 0.5f;
                bandHigh = centerExact + thickExact * 0.5f;
            }
            const float axisLow = isVertical ? L : T;
            const float axisHigh = isVertical ? R : B;
            // Flush inside the cell: a band centred on an edge would
            // half-clip to a hairline, and half of it would paint the
            // neighbouring cell.
            if (bandLow < axisLow) {
                bandHigh += axisLow - bandLow;
                bandLow = axisLow;
            }
            if (bandHigh > axisHigh) {
                bandLow -= bandHigh - axisHigh;
                bandHigh = axisHigh;
            }
            float spanLow, spanHigh;
            if (isVertical) {
                spanLow = p.y0 == 0.0f ? T : std::round(T + p.y0 * cellHeight);
                spanHigh = p.y1 == 1.0f ? B : std::round(T + p.y1 * cellHeight);
            } else {
                spanLow = p.x0 == 0.0f ? L : std::round(L + p.x0 * cellWidth);
                spanHigh = p.x1 == 1.0f ? R : std::round(L + p.x1 * cellWidth);
            }
            auto emitBand = [&](float low, float high, float alpha) {
                if (high <= low || alpha <= 0.0f)
                    return;
                if (isVertical)
                    sink.fillRect(low, spanLow, high, spanHigh, alpha);
                else
                    sink.fillRect(spanLow, low, spanHigh, high, alpha);
            };
            if (isWholePixel) {
                emitBand(bandLow, bandHigh, 1.0f);
            } else {
                // The fractional band as the font's rasteriser would fill
                // it: whole rows at full coverage, the partial rows at their
                // overlap. A band inside one pixel collapses to that pixel
                // at the band's whole mass.
                const float coreLow = std::ceil(bandLow);
                const float coreHigh = std::floor(bandHigh);
                if (coreHigh < coreLow) {
                    emitBand(std::floor(bandLow), std::floor(bandLow) + 1.0f, bandHigh - bandLow);
                } else {
                    emitBand(coreLow, coreHigh, 1.0f);
                    emitBand(coreLow - 1.0f, coreLow, coreLow - bandLow);
                    emitBand(coreHigh, coreHigh + 1.0f, bandHigh - coreHigh);
                }
            }
            break;
        }
        }
    }

    if (doubleEdges.hasVertical() || doubleEdges.hasHorizontal()) {
        // Rails and gap from the font's own double rules where measured; a
        // face shipping doubles on one axis only still fixes both, because
        // rails cut differently per axis read as a mistake at every cross.
        float railThickness = light;
        float railGap = light;
        FloatPoint railCenter = { lightCx, lightCy };
        if (profile) {
            const Rendering::BoxDrawing::BoxDoubleBand& h = profile->doubleHorizontal;
            const Rendering::BoxDrawing::BoxDoubleBand& v = profile->doubleVertical;
            if (h.isValid || v.isValid) {
                const float railUnit = h.isValid && v.isValid
                    ? (h.railThicknessInCell * cellHeight + v.railThicknessInCell * cellWidth) * 0.5f
                    : (h.isValid ? h.railThicknessInCell * cellHeight : v.railThicknessInCell * cellWidth);
                const float gapUnit = h.isValid && v.isValid
                    ? (h.gapInCell * cellHeight + v.gapInCell * cellWidth) * 0.5f
                    : (h.isValid ? h.gapInCell * cellHeight : v.gapInCell * cellWidth);
                railThickness = std::fmax(1.0f, std::round(railUnit));
                railGap = std::fmax(1.0f, std::round(gapUnit));
            }
            if (v.isValid)
                railCenter.x = std::round(L + v.centerInCell * cellWidth);
            if (h.isValid)
                railCenter.y = std::round(T + h.centerInCell * cellHeight);
        }
        emitDoubleRails(doubleEdges, cell, { lightCx, lightCy }, railCenter,
            lightVerticalThickness, lightHorizontalThickness, railThickness, railGap, sink);
    }
    return true;
}

} // namespace Ubiquitty::Rendering
