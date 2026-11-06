// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/GlyphBackend.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/ProportionalRun.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// What a proportional strip needs beyond its text (Docs/proportional-text.md):
/// the cells the run occupies and how it sits in them, the pen's fraction, the
/// cell advance, the slot it draws into and the sheet's margin before the pen.
struct ProportionalStripRequest {
    /// The cells the run occupies.
    int widthCells = 0;
    /// Left, right, centred or justified inside them.
    std::uint8_t horizontalAlign = 0;
    /// The pen's fraction past its whole pixel.
    float penResidualInPixels = 0.0f;
    /// One cell's advance in the sheet's pixels.
    float cellAdvanceInPixels = 0.0f;
    /// The sheet's margin before the pen, in slot pixels.
    float marginXInPixels = 0.0f;
    /// The slot's width.
    int slotWidthInPixels = 0;
    /// The slot's height.
    int slotHeightInPixels = 0;
    /// The face the run is set in; italic resolves to its upright.
    FontFace face = FontFace::Regular;
    /// The zero-based tile of the full shaped run.
    int tileIndex = 0;
    /// How many times its ordinary width the run is drawn across: 2 on a
    /// DECDWL row, 1 otherwise. A doubled strip is another raster of the same
    /// text and keys apart from a plain run of twice the cells.
    std::uint8_t widthScale = 1;
    /// How many times its ordinary size the run is drawn both ways: 2 on a
    /// DECDHL pair, the OSC 66 scale of a scaled run, 1 otherwise. The strip
    /// is then the run on a canvas `scale` slots tall, of which the request's
    /// slot is the one `band` slots down.
    std::uint8_t scale = 1;
    /// Which slot height of a strip drawn at `scale`, from the top.
    std::uint8_t band = 0;
    bool operator==(const ProportionalStripRequest&) const = default;
};

/// A run shaped for a strip: its layout by Core's one arithmetic, and how many
/// glyphs the shaper answered with — zero when the face refused the run.
struct ProportionalStripShape {
    /// Where the pen, the clip and the quad go.
    ProportionalStripLayout layout;
    /// The glyphs written; zero refuses.
    int glyphCount = 0;
};

using ProportionalStripVerification = std::vector<std::uint32_t>;

/// Identifies a verified strip slot and whether its raster must be filled.
struct ProportionalStripSlot {
    int slot = -1;
    std::uint64_t key = 0;
    bool isFresh = false;
};

/// Resolves a proportional strip through Core's complete source identity.
///
/// The owning sheet fixes the font configuration and raster geometry. Text,
/// face, claim, alignment, aligned residual, tile, and rendering tier identify
/// each raster within it; the complete payload verifies hash collisions.
///
/// - Parameters:
///   - cache: The sheet's portable slot cache.
///   - codePoints: The complete source run, independent of raster tile width.
///   - request: The claim, face, and tile being drawn.
///   - layout: The resolved placement of that tile.
///   - variant: The coverage rendering variant.
///   - verification: Reusable storage for the verified key payload.
/// - Returns: The assigned slot and key, or a negative slot on refusal.
[[nodiscard]] ProportionalStripSlot resolveProportionalStripSlot(GlyphSlotCache& cache,
    ClusterSpan codePoints, const ProportionalStripRequest& request,
    const ProportionalStripLayout& layout, std::uint32_t variant,
    ProportionalStripVerification& verification);

/// Shapes a run in the rasteriser's proportional face, opens its justified
/// gaps and lays the strip out, so every sheet keys and places a strip by the
/// same numbers whether Core or a frontend holds the slots.
///
/// - Parameters:
///   - rasterizer: The face; asked through ``GlyphRasterizer::shapeProportionalRun``.
///   - codePoints: The run's text.
///   - request: The cells, alignment, pen and slot.
///   - shapedOut: Receives the glyphs with Core's relative placement in their
///     offsets and zero advances; resized to fit.
/// - Returns: The layout and the glyph count.
[[nodiscard]] ProportionalStripShape shapeProportionalStrip(GlyphRasterizer& rasterizer, ClusterSpan codePoints,
    const ProportionalStripRequest& request, std::vector<Ubiquitty::ProportionalShapedGlyph>& shapedOut);

/// Draws a shaped strip into a slot: clears it, moves every glyph by the
/// layout's pen, rasterises in the face and clips to the run's cells.
///
/// - Parameters:
///   - rasterizer: The face; asked through ``GlyphRasterizer::rasteriseProportionalRun``.
///   - shaped: The glyphs ``shapeProportionalStrip`` wrote.
///   - request: The slot and face.
///   - layout: The layout ``shapeProportionalStrip`` answered.
///   - coverageOut: The slot, `slotWidthInPixels * slotHeightInPixels` bytes.
/// - Returns: Whether the strip was drawn.
[[nodiscard]] bool rasteriseProportionalStrip(GlyphRasterizer& rasterizer, std::span<const Ubiquitty::ProportionalShapedGlyph> shaped,
    const ProportionalStripRequest& request, const ProportionalStripLayout& layout, CoverageSlot coverageOut);

/// The cells a run needs in the rasteriser's proportional face at natural
/// advances: what an application measures before it prints.
///
/// - Parameters:
///   - rasterizer: The face.
///   - codePoints: The run's text.
///   - cellAdvanceInPixels: One cell's advance.
/// - Returns: The cell count, at least one; zero when the face refuses.
[[nodiscard]] int proportionalRunCellsInFace(GlyphRasterizer& rasterizer, ClusterSpan codePoints, float cellAdvanceInPixels);

/// ``shapeProportionalStrip`` over the bitmap tier's face, for a sheet a
/// frontend holds: the strike is Core's, so the shape is the reference's.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - codePoints: The run's text.
///   - request: The cells, alignment, pen and slot.
/// - Returns: The layout and the glyph count.
[[nodiscard]] ProportionalStripShape shapeUnifontProportionalStrip(int unifontScale, ClusterSpan codePoints,
    const ProportionalStripRequest& request);

/// Shapes and draws a run into a bitmap-tier slot in one call, for a sheet a
/// frontend holds; the slot's height past Unifont's cell is the row's spacing,
/// so the baseline lands where the reference puts it.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - codePoints: The run's text.
///   - request: The cells, alignment, pen, slot and face.
///   - coverageOut: The slot, `slotWidthInPixels * slotHeightInPixels` bytes.
/// - Returns: Whether the strip was drawn.
[[nodiscard]] bool rasteriseUnifontProportionalStrip(int unifontScale, ClusterSpan codePoints,
    const ProportionalStripRequest& request, CoverageSlot coverageOut);

/// ``proportionalRunCellsInFace`` over the bitmap tier's face.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - codePoints: The run's text.
///   - cellAdvanceInPixels: One cell's advance.
/// - Returns: The cell count, at least one; zero when the face refuses.
[[nodiscard]] int unifontProportionalRunCells(int unifontScale, ClusterSpan codePoints, float cellAdvanceInPixels);

/// ``GlyphRasterizer::proportionalStripOverhangInPixels`` for the bitmap
/// tier's face, for a sheet a frontend cuts: its strip slot is the cell plus
/// this on both sides, and its quad's top moves up by it.
///
/// - Parameter unifontScale: The bitmap family's upscale.
/// - Returns: The rows per side.
[[nodiscard]] int unifontProportionalStripOverhangInPixels(int unifontScale);

} // namespace Ubiquitty::Rendering
