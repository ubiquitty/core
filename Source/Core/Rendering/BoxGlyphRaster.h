// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/BoxGlyphRaster.h — the Linux/CPU coverage SINK for the shared
// box-drawing command builder (Rendering/BoxDrawCommands.h). Shared pipeline:
//   1. BoxDrawing.h       — codePoint -> unit-coord primitive DECOMPOSITION.
//   2. BoxDrawCommands.h  — buildBoxGlyph(): unit primitives -> DEVICE-space draw
//                           commands (positioning / snapping / stroke thickness).
//   3. a platform SINK    — paints each command. This file is the Linux one: it
//                           rasterizes commands into an 8-bit coverage mask the
//                           VulkanGlyphAtlas + SoftwareRenderer blit into a slot.
// Cell-exact and edge-snapped so adjacent cells tile with no seam.

#include "Base/Geometry.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Rendering/ShadeStyle.h"

#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering {

// Rasterize the procedural form of `codePoint` into `coverageOut`, a caller-owned
// slotWidth*slotHeight R8 buffer (row-major, top-left origin). The cellWidth ×
// cellHeight cell box is centred in the slot (the overflow-canvas margins the
// atlas adds for spilling glyphs); the box fill is snapped to that cell's edges so
// neighbouring cells abut gaplessly. The buffer is fully written (clear-then-draw):
// pixels outside the drawn primitives are set to 0.
//
// Returns true when `codePoint` has a procedural form of any kind — area fills,
// rules, and the two mixed — and the coverage was drawn.
//
// Two paths return false and they differ in what the caller is holding. A code
// point `BoxDrawing::covers` rejects returns before anything is written, so the
// buffer is intact. A code point it accepts whose `decompose` yields no
// primitives returns false with the buffer already cleared to zero — a caller
// that treats false as "untouched" and delegates to the font is still correct,
// because it overwrites, but a caller that treats the buffer as its own prior
// contents is not.
//
// Lines are drawn here rather than delegated, which is the point of the file: a
// font's `─` carries its own bearings, so a run of them leaves a gap at every
// cell edge. `covers` already routes the cases a font draws better — the dashed
// forms, which stagger if a procedural one is tiled under fractional advance —
// so a code point reaching this function is one that must be drawn to the cell.
/// Rasterises a box-drawing glyph into a caller-owned coverage plane.
///
/// - Parameters:
///   - codePoint: The box-drawing code point.
///   - slot: The atlas slot's pixel extent.
///   - cell: The cell box inside that slot; the glyph is centred on it, and its
///     edges are where the strokes run so a line meets its neighbour.
///   - emHeight: The type's height, which the stroke weights derive from. The
///     cell's height carries the line spacing and would thicken a box rule as
///     the reader raises their line height. Zero falls back to the cell.
///   - coverage: Destination, `slot.width * slot.height` bytes. A span rather
///     than a pointer with the product passed alongside: every caller allocates
///     exactly that product, and nothing checked that the two agreed.
///   - profile: The font's measured stroke bands, or null; a valid band
///     overrides the em rule for its class so procedural strokes stitch with
///     the font's own.
///   - shadeStyle: How a shade primitive is drawn. A pattern is anchored to
///     the cell box's top-left pixel, because a slot is reused at every column.
/// - Returns: `true` when the code point has a procedural form.
bool rasterizeBoxGlyph(unsigned codePoint, IntSize slot, IntSize cell, float emHeight, std::span<std::uint8_t> coverage, const BoxDrawing::BoxStrokeProfile* profile = nullptr, ShadeStyle shadeStyle = defaultShadeStyle);

} // namespace Ubiquitty::Rendering
