// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/DRCSRaster.h — the portable coverage source for DECDLD
// soft-font glyphs, the bitmap counterpart of the procedural box-drawing sink
// (Rendering/BoxGlyphRaster.h). Box drawing has to DECOMPOSE a code point into
// primitives first; a DRCS bitmap is given (Core/VT/SoftFont.h holds it as uint16 row
// masks), so the only work here is fitting that matrix to the cell box.

#include "Core/VT/SoftFont.h"

#include "Base/Geometry.h"
#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering {

// True when `index` names a glyph this font actually carries — i.e. one worth an
// atlas slot. A font that was never downloaded, was erased by RIS or a DECDLD Pe
// wipe, or resolved to a degenerate matrix carries none. Callers test this before
// reserving a slot so an undefined DRCS position can't evict a live glyph to
// rasterize nothing; inline because that test runs per DRCS cell per frame, while
// the raster below runs only on an atlas miss.
[[nodiscard]] inline bool softFontHasGlyph(const VT::SoftFont& font, int index)
{
    return font.valid && index >= 0 && index < VT::drcsMaxCells && font.glyphSet[index] && font.matrixWidth > 0 && font.matrixHeight > 0;
}

/// Rasterises one soft-font glyph into a caller-owned coverage plane.
///
/// `coverageOut` is a `slotWidth * slotHeight` R8 buffer, row-major with a
/// top-left origin. The glyph fills the cell box `[cellX, cellX+cellWidth) x
/// [cellY, cellY+cellHeight)` inside the slot — the atlas's cell-box UV maps
/// exactly that region onto the cell quad — and the surrounding margins are
/// cleared, so a glyph never spills into a neighbour.
///
/// Scaling is area coverage on the upscale: a destination pixel takes the
/// fraction of its footprint the inked design covers. A source pixel that
/// maps whole onto destination pixels is still all or nothing, so an integer
/// upscale is the nearest-neighbour picture a bitmap face wants; at a ratio
/// like 4.25 rows a source row, a one-pixel stroke is four whole rows and a
/// quarter-covered fifth on every row of the design, where nearest sampling
/// made it four rows here and five there and the glyph looked hand-cut. On
/// the downscale a destination pixel ORs its whole source span instead, so a
/// stroke thinner than a destination pixel still inks rather than being
/// sampled away — the two rules coincide wherever a source pixel maps to one
/// destination pixel.
///
/// `coverageOut` is left UNTOUCHED on failure — when the glyph is absent or the
/// geometry is degenerate. The caller must then paint the cell background-only
/// rather than blit the slot, whose pixels still belong to whatever the atlas
/// last put there; blitting it draws another glyph's ink under this cell's
/// colours, with the frame's every count correct.
///
/// - Parameters:
///   - index: Position within the soft font, 0 to `drcsMaxCells - 1`.
///   - slotWidth: Atlas slot width in pixels.
///   - slotHeight: Atlas slot height in pixels.
///   - cellX: Left edge of the cell box inside the slot.
///   - cellY: Top edge of the cell box inside the slot.
///   - cellWidth: Cell box width in pixels.
///   - cellHeight: Cell box height in pixels.
///   - coverageOut: Destination coverage plane, written only on success.
/// - Returns: `true` when the glyph was rasterised.
bool rasterizeDRCSGlyph(const VT::SoftFont&, int index, IntSize slot, IntRect cell, std::span<std::uint8_t> coverage);

} // namespace Ubiquitty::Rendering
