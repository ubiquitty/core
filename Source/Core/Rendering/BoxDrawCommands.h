// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
// Source/Core/Rendering/BoxDrawCommands.h — the shared box-drawing geometry seam.
// `buildBoxGlyph` owns the positioning/sizing/snapping: it takes a codePoint +
// the device-pixel cell rect, runs the BoxDrawing decomposition, and turns the
// unit-coordinate primitives into DEVICE-SPACE draw commands — pixel-snapped
// cell centre, edge-snapped fills (so neighbours tile gaplessly), stroke
// thickness per weight (light ~ cellHeight/12, heavy ~2x), double-stroke gaps,
// rounded-join arcs, diagonals — emitted to a caller-supplied BoxDrawSink. The
// platform supplies only the sink (paint a rect / shade / triangle / capsule).
// Pure C++ (no platform types).

#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Rendering/ShadeStyle.h"

#include <cstdint>
#include <optional>

namespace Ubiquitty::Rendering {

// The paint primitives a frontend must implement. All coordinates are DEVICE
// pixels in the glyph's coordinate space; alpha/density are 0..1.
struct BoxDrawSink {
    // Axis-aligned filled rectangle [x0,x1) x [y0,y1).
    virtual void fillRect(float x0, float y0, float x1, float y1, float alpha) = 0;
    // Ordered-dither shade over [x0,x1) x [y0,y1) at `density` (foreground
    // fraction), keyed to absolute pixel position so the field stitches across
    // cells (a checkerboard, not a flat tint). An Inverse polarity asks for
    // the complement of the field (ShadeStyle.h, inverseShadePhaseOffset).
    virtual void stipple(float x0, float y0, float x1, float y1, float density, ShadePolarity polarity) = 0;
    // Filled triangle through the three device vertices.
    virtual void triangle(float ax, float ay, float bx, float by, float cx, float cy, float alpha) = 0;
    /// Paints a round-capped stroke, optionally restricted to a half-open box.
    ///
    /// The clip has hard edges; adjoining diagonal rows partition coverage at
    /// their common boundary without antialiasing that boundary twice.
    ///
    /// - Parameters:
    ///   - ax: The starting horizontal coordinate.
    ///   - ay: The starting vertical coordinate.
    ///   - bx: The ending horizontal coordinate.
    ///   - by: The ending vertical coordinate.
    ///   - half: Half the stroke width.
    ///   - clip: The optional hard clipping bounds.
    virtual void capsule(float ax, float ay, float bx, float by, float half, std::optional<FloatRect> clip = std::nullopt) = 0;

    /// Paints an elliptical stroke clipped to the cell, with per-axis weights.
    ///
    /// - Parameters:
    ///   - cell: The terminal cell’s pixel bounds.
    ///   - center: The arc center in pixels.
    ///   - radii: The outer horizontal and vertical radii.
    ///   - halfWidths: Half the stroke thickness on each axis.
    virtual void ellipticalArc(FloatRect cell, FloatPoint center, FloatSize radii, FloatSize halfWidths)
    {
        rasterizeEllipticalArc(cell, center, radii, halfWidths);
    }

protected:
    /// Emits nonoverlapping pixels for an elliptical corner stroke.
    ///
    /// - Parameters:
    ///   - cell: The terminal cell’s pixel bounds.
    ///   - center: The arc center in pixels.
    ///   - radii: The outer horizontal and vertical radii.
    ///   - halfWidths: Half the stroke thickness on each axis.
    void rasterizeEllipticalArc(FloatRect cell, FloatPoint center, FloatSize radii, FloatSize halfWidths);
    ~BoxDrawSink() = default;
};

/// Returns the signed distance approximation for a band between two ellipses.
///
/// - Parameters:
///   - point: The sampled pixel position.
///   - center: The arc center in pixels.
///   - radii: The outer horizontal and vertical radii.
///   - halfWidths: Half the stroke thickness on each axis.
///   - direction: Signs selecting the arc quadrant.
/// - Returns: Signed pixel distance, negative inside the stroke.
float ellipticalArcDistance(FloatPoint point, FloatPoint center, FloatSize radii, FloatSize halfWidths, FloatPoint direction);

// Build the device-space draw commands for `codePoint` over the cell rect
// [L,T,R,B] (device px), emitting them to `sink`. Returns true when the codePoint
// has a box-drawing form (commands were emitted), false otherwise (the caller
// then leaves the glyph to the font path). The cell rect should already be
// pixel-aligned; `buildBoxGlyph` snaps the centre + stroke bands internally.
/// Builds one box-drawing glyph as sink commands, in device pixels.
///
/// - Parameters:
///   - codePoint: The box-drawing code point.
///   - cell: The cell box to draw within; snapping happens inside.
///   - sink: Receives the fill, stipple, triangle and capsule commands.
/// - Returns: `true` when the code point has a procedural form.
/// - Parameter strokeBasis: The height the stroke weights are derived from, in
///   the same pixels as `cell`. The type's own height, not the line box's: line
///   spacing grows the cell and must not thicken a box rule, or a user raising
///   their line height gets heavier box drawing and solid strokes stop matching
///   the dashes the font still draws at one pixel. Zero falls back to the
///   cell's height.
/// - Parameter profile: The font's measured stroke bands, or null. A valid
///   band overrides the em rule for its class alone — thickness and
///   centreline both — so a healed character stitches with the strokes the
///   font draws itself; an invalid or absent band keeps the em fallback.
bool buildBoxGlyph(unsigned codePoint, FloatRect cell, float strokeBasis, const BoxDrawing::BoxStrokeProfile* profile, BoxDrawSink& sink);

} // namespace Ubiquitty::Rendering
