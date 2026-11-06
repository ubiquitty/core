// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/BoxFillEmit.h — box-drawing geometry as overlay quads.
//
// `BoxDrawCommands.h` decides the SHAPE of a box-drawing glyph and drives a sink
// with device-space paint commands. This turns those commands into the
// `OverlayInstance` quads a GPU pass draws, which is the step every backend
// would otherwise write for itself.
//
// A box-drawing cell must not go through the glyph atlas: neighbouring cells
// have to meet with no seam, and a font glyph rasterised into a slot carries its
// own bearings and antialiasing and will not tile. That is why this exists as a
// separate lane rather than as a fallback.

#include "Base/Geometry.h"
#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/ShadeStyle.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Ubiquitty::Rendering {

/// Counters for what a frame's box-drawing cells decomposed into.
///
/// Observability only — nothing reads these to make a decision. They separate
/// the cheap axis-aligned fills from the shaded, scan-converted and SDF quads,
/// which is what tells a reader whether a scene's box cost is geometry or
/// shading.
///
/// ``emitBoxFill`` maintains every field, `sourceCells` included. A counter a
/// backend has to remember to bump itself is one a new backend reports as zero
/// while every other field is right, which reads as a scene with no box cells
/// rather than as a backend that forgot.
struct BoxFillStats {
    std::size_t sourceCells = 0;
    std::size_t fullRects = 0;
    std::size_t partialRects = 0;
    std::size_t shadeQuads = 0;
    std::size_t shapeQuads = 0;
    std::size_t triangleRects = 0;
    std::size_t arcRects = 0;
    std::size_t mosaicQuads = 0;
};

/// The shade style and the grid phase every shade quad of a frame shares.
///
/// A pattern quad carries the phase in `plA.x`, and its fragment stage lights
/// `isShadePixelLit(x, y, shadeLitCount(shadeDensity), plA.x)` for the surface
/// pixel (x, y). A fill emits no shade quad at all: the shade becomes an
/// ordinary rect whose alpha is the density, composited like any other.
struct ShadeField {
    ShadeStyle style { defaultShadeStyle };
    int phase = 0;
};

/// Emits one box-drawing, block-element or sextant glyph as overlay quads.
///
/// The cell rect edges must already be snapped to integer device columns and
/// rows by the caller: the decomposition places strokes against those edges, and
/// snapping afterwards would move a shared edge on one side of it only, which is
/// exactly the seam this lane exists to avoid.
///
/// Two analytic forms are checked before the general decomposition because each
/// resolves the whole cell to a single quad: a Powerline separator, whose
/// diagonal is antialiased from an SDF rather than stair-stepped by strip fills,
/// and a mosaic mask, whose 2x3 sextant field the fragment stage samples
/// directly.
///
/// - Parameters:
///   - codePoint: The cell's code point.
///   - left: Cell rect left edge, device pixels, integer-snapped.
///   - top: Cell rect top edge.
///   - right: Cell rect right edge.
///   - bottom: Cell rect bottom edge.
///   - foreground: The resolved ink, whose alpha is multiplied by shape coverage.
///   - out: Quads to write, at most `maxOut` of them; null writes nothing and
///     still counts.
///   - maxOut: Write capacity. A quad past it is counted, never written, so
///     the buffer holds the first `maxOut` and the return names the rest.
///   - powerline: The analytic separator shape, or null for none.
///   - stats: Counters to accumulate into, or null.
///   - referenceHeight: The stroke basis in device pixels — the natural line
///     height the face rasterises at, before the user's line spacing widens the
///     cell. Zero falls back to the cell height (`bottom - top`), the historical
///     behaviour. Passing the reference keeps a box rule the same weight as the
///     line spacing grows, matching the font-raster path, which takes its stroke
///     from the em rather than the cell it lands in.
///   - clip: Optional device-pixel bounds. The shape retains its complete
///     geometry while only its intersection with these bounds is emitted.
///   - shade: How a shade primitive is drawn and where its dither is anchored.
/// - Returns: How many quads the glyph needs. A return above `maxOut` is the
///   caller's signal to grow to that count and emit again; a count that
///   stopped at the capacity could never exceed it, and every capacity check
///   downstream of one would hold by construction rather than by room.
int emitBoxFill(std::uint32_t codePoint, float left, float top, float right, float bottom,
    Vec4 foreground, OverlayInstance* out, int maxOut,
    const Rendering::BoxDrawing::PowerlineShape* powerline = nullptr, BoxFillStats* stats = nullptr,
    float referenceHeight = 0.0f,
    const Rendering::BoxDrawing::BoxStrokeProfile* profile = nullptr,
    std::optional<FloatRect> clip = std::nullopt, ShadeField shade = {});

} // namespace Ubiquitty::Rendering
