// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <climits>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// Owns the portable policy and host mirror for proportional glyphs.
///
/// The sheet is separate from both the two-cell glyph atlas and
/// the run-keyed ligature sheet. Core assigns its slots by glyph identity and
/// keeps its eviction budget independent; a frontend supplies only the font
/// rasterizer and uploads this mirror to its own GPU resource.
class ProportionalGlyphSheet {
public:
    /// Builds the bounded sheet for one font configuration.
    ///
    /// The object lifetime is part of the cache identity: a frontend replaces
    /// this object whenever font, variation, scale, or rendering configuration
    /// changes, so a slot can never outlive the pixels its key describes.
    ///
    /// - Parameters:
    ///   - cellWidthInPixels: The grid face's cell width.
    ///   - cellHeightInPixels: The grid face's cell height.
    ///   - marginXInPixels: Horizontal overhang room on each side.
    ///   - marginYInPixels: Vertical overhang room on each side.
    ///   - rasterizer: The proportional face provider. Borrowed.
    ///   - maxTextureDimension: The frontend's maximum texture edge.
    ProportionalGlyphSheet(int cellWidthInPixels, int cellHeightInPixels,
        int marginXInPixels, int marginYInPixels, GlyphRasterizer* rasterizer,
        int maxTextureDimension);

    /// Reports whether the device limit permits a usable sheet.
    [[nodiscard]] bool isUsable() const { return m_isUsable; }
    /// Begins the cache's frame lifetime for touched slots.
    void beginFrame() { m_cache.beginFrame(); }

    /// Returns the cache and geometry consumed by the shared builder.
    [[nodiscard]] ProportionalAtlasBinding binding();

    /// Shapes text through the proportional face without applying layout.
    ///
    /// - Parameters:
    ///   - codePoints: The source text.
    ///   - face: The requested face style.
    ///   - glyphsOut: Receives the face-local glyphs and metrics.
    /// - Returns: The number of glyphs written, or zero on refusal.
    int shapeRun(ClusterSpan codePoints, FontFace face,
        std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut);

    /// Rasterises one face-local glyph into a slot assigned by Core.
    ///
    /// - Parameters:
    ///   - glyphID: The face-local glyph identity.
    ///   - face: The requested face style.
    ///   - weight: The variable-font weight.
    ///   - variant: The subpixel phase and coverage rendering tier.
    ///   - slot: The cache-assigned destination.
    /// - Returns: Whether the raster and its authoritative ink were recorded.
    bool rasteriseGlyph(std::uint32_t glyphID, FontFace face,
        std::uint32_t weight, std::uint32_t variant, int slot);

    /// Rasterises one slot-wide window of a glyph drawn larger than its slot —
    /// for a DECDWL row, a DECDHL pair or an OSC 66 scale — into a slot
    /// assigned by Core.
    ///
    /// Backs ``GlyphBackend::rasteriseSizedProportionalGlyph``. The master is
    /// Core's (``selectSizedMaster``): the glyph at `tile.scale` times its size,
    /// `tile.widthScale` times again across, on a canvas `tile.scale` slots
    /// tall, of which the window is the slot height `tile.band` slots down. The
    /// pen stands `tile.scale * tile.widthScale` margins in at the variant's
    /// phase, and the window is the cut `tile.tile` slots along.
    ///
    /// - Parameters:
    ///   - glyphID: The face-local glyph identity.
    ///   - face: The requested face style.
    ///   - weight: The variable-font weight.
    ///   - variant: The sized origin's subpixel phase and coverage tier.
    ///   - tile: The scale, the band, the window and the doubling across.
    ///   - slot: The cache-assigned destination.
    /// - Returns: Whether the rasterizer drew the master.
    bool rasteriseSizedGlyph(std::uint32_t glyphID, FontFace face,
        std::uint32_t weight, std::uint32_t variant, SizedTile tile, int slot);

    /// Draws one slot-wide window of a glyph drawn larger than its slot into
    /// a slot-sized plane, the way ``rasteriseSizedGlyph`` fills this sheet's
    /// own slot. Shared with the frontends that hold a sheet of their own, so
    /// their bytes are this sheet's by construction.
    ///
    /// - Parameters:
    ///   - rasterizer: The proportional face.
    ///   - glyphID: The face-local glyph identity.
    ///   - face: The requested face style.
    ///   - variant: The sized origin's subpixel phase and coverage tier.
    ///   - tile: The scale, the band, the window and the doubling across.
    ///   - marginXInPixels: The slot's horizontal margin before the pen.
    ///   - slotWidthInPixels: The slot's width.
    ///   - slotHeightInPixels: The slot's height.
    ///   - masterScratch: Storage for the master, `tile.scale` slots tall.
    ///   - coverageOut: The plane, `slotWidthInPixels * slotHeightInPixels` bytes.
    /// - Returns: Whether the rasterizer drew the master.
    [[nodiscard]] static bool rasteriseSizedGlyphIntoSlot(GlyphRasterizer& rasterizer, std::uint32_t glyphID, FontFace face,
        std::uint32_t variant, SizedTile tile, int marginXInPixels, int slotWidthInPixels, int slotHeightInPixels,
        std::vector<std::uint8_t>& masterScratch, std::span<std::uint8_t> coverageOut);

    /// Returns the row-major R8 host mirror.
    [[nodiscard]] std::span<const std::uint8_t> coverage() const { return m_coverage; }
    /// Returns the host mirror's row width.
    [[nodiscard]] int widthInPixels() const { return m_sheetWidthInPixels; }
    /// Returns the host mirror's row count.
    [[nodiscard]] int heightInPixels() const { return m_sheetHeightInPixels; }
    /// Returns one raster slot's width including margins.
    [[nodiscard]] int slotWidthInPixels() const { return m_slotWidthInPixels; }
    /// Returns one raster slot's height including margins.
    [[nodiscard]] int slotHeightInPixels() const { return m_slotHeightInPixels; }
    /// Returns the bounded number of raster slots.
    [[nodiscard]] int slotCapacity() const { return m_capacity; }
    /// Returns the mirror's raster-content generation.
    [[nodiscard]] std::uint32_t generation() const { return m_generation; }

    /// Returns the pixel rows whose mirror is newer than an uploaded image.
    [[nodiscard]] AtlasRowBand dirtyRows() const;

    /// Marks the current mirror as uploaded.
    void didUpload();

    /// Carries validated raster geometry bounded by the device limit.
    struct Plan {
        AtlasGridFit grid { 1, 1, false };
        int slotWidthInPixels = 1;
        int slotHeightInPixels = 1;
        IntSize marginInPixels {};
    };

    /// The slot and grid a sheet cuts for a cell and a face, bounded by the
    /// device limit. Public so a frontend holding its own texture over this
    /// contract cuts the slot the reference cuts, to the pixel.
    static Plan makePlan(int cellWidthInPixels, int cellHeightInPixels,
        int marginXInPixels, int marginYInPixels, GlyphRasterizer*,
        int maxTextureDimension);

    /// Draws one face-local glyph at a phase into a slot-sized plane, the way
    /// ``rasteriseGlyph`` fills this sheet's own slot: the glyph's origin is
    /// the slot's margin plus the phase's fraction, and the face places its
    /// rows from the slot's overhang. Shared with the frontends that hold a
    /// sheet of their own so their bytes are this sheet's by construction.
    ///
    /// - Parameters:
    ///   - rasterizer: The proportional face.
    ///   - glyphID: The face-local glyph identity.
    ///   - face: The requested face style.
    ///   - variant: The subpixel phase and coverage rendering tier.
    ///   - marginXInPixels: The slot's horizontal margin before the pen.
    ///   - slotWidthInPixels: The slot's width.
    ///   - slotHeightInPixels: The slot's height.
    ///   - coverageOut: The plane, `slotWidthInPixels * slotHeightInPixels` bytes.
    /// - Returns: Whether the glyph was drawn.
    [[nodiscard]] static bool rasteriseGlyphIntoSlot(GlyphRasterizer& rasterizer,
        std::uint32_t glyphID, FontFace face, std::uint32_t variant,
        int marginXInPixels, int slotWidthInPixels, int slotHeightInPixels,
        std::span<std::uint8_t> coverageOut);

private:
    /// Builds the sheet from validated geometry and a borrowed rasterizer.
    ProportionalGlyphSheet(GlyphRasterizer*, Plan);
    /// Extends the pending upload range to include a rasterised slot.
    ///
    /// - Parameters:
    ///   - slot: The occupied atlas slot whose coverage changed.
    void markSlotDirty(int slot);

    GlyphRasterizer* m_rasterizer = nullptr;
    GlyphSlotCache m_cache;
    int m_columns = 1;
    int m_rows = 1;
    int m_capacity = 1;
    int m_slotWidthInPixels = 1;
    int m_slotHeightInPixels = 1;
    int m_sheetWidthInPixels = 1;
    int m_sheetHeightInPixels = 1;
    IntSize m_marginInPixels {};
    bool m_isUsable = false;
    std::uint32_t m_generation = 0;
    int m_firstDirtySlot = 0;
    int m_lastDirtySlot = INT_MAX;
    std::vector<std::uint8_t> m_coverage;
    std::vector<std::uint8_t> m_slotScratch;
    std::vector<std::uint8_t> m_sizedScratch; // a sized glyph's master, `scale` slots tall
};

/// The slot a bitmap-tier proportional sheet cuts, for a frontend that holds
/// its own texture over the same cache and placement contract
/// (Docs/proportional-text.md): the plan the reference sheet cuts for the same
/// cell, so a GPU arm's glyph lands where the reference's does, to the pixel.
struct UBIQUITTY_ESCAPABLE ProportionalGlyphSlotPlan {
    /// One slot's width including margins.
    int slotWidthInPixels = 0;
    /// One slot's height including margins and the strike's overhang.
    int slotHeightInPixels = 0;
    /// The room before the pen and above the cell, in slot pixels.
    IntSize marginInPixels {};
    /// Slots per sheet row, after the device-limit clamp.
    int columns = 0;
    /// Rows of slots, after the same clamp.
    int rows = 0;
    /// False when the face has no proportional strike or a slot exceeds the limit.
    bool valid = false;
};

/// ``ProportionalGlyphSheet``'s plan over the bitmap tier's face, for a sheet a
/// frontend holds.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - cellWidthInPixels: The grid face's cell width.
///   - cellHeightInPixels: The grid face's cell height, spacing included.
///   - maxTextureDimension: The frontend's maximum texture edge.
/// - Returns: The plan; invalid when no sheet fits.
[[nodiscard]] ProportionalGlyphSlotPlan unifontProportionalGlyphSlotPlan(int unifontScale,
    int cellWidthInPixels, int cellHeightInPixels, int maxTextureDimension);

/// ``ProportionalGlyphSheet::shapeRun`` over the bitmap tier's face, for a
/// sheet a frontend holds: the strike is Core's, so the shape is the reference's.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - codePoints: The run's text.
///   - face: The requested face style.
///   - glyphsOut: Receives one glyph per code point.
///   - capacity: How many `glyphsOut` holds.
/// - Returns: The number of glyphs written, or zero on refusal.
[[nodiscard]] int shapeUnifontProportionalRun(int unifontScale, ClusterSpan codePoints, FontFace face,
    Ubiquitty::ProportionalShapedGlyph* glyphsOut, int capacity);

/// ``ProportionalGlyphSheet::rasteriseGlyph`` over the bitmap tier's face, into
/// a slot a frontend holds: the same origin, phase and overhang as the
/// reference sheet's slot, so the bytes are the reference's.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - glyphID: The glyph, as ``shapeUnifontProportionalRun`` named it.
///   - face: The requested face style.
///   - variant: The subpixel phase and coverage rendering tier.
///   - plan: The slot, from ``unifontProportionalGlyphSlotPlan``.
///   - coverageOut: The slot, `slotWidthInPixels * slotHeightInPixels` bytes.
/// - Returns: Whether the glyph was drawn.
[[nodiscard]] bool rasteriseUnifontProportionalGlyph(int unifontScale, std::uint32_t glyphID, FontFace face,
    std::uint32_t variant, const ProportionalGlyphSlotPlan& plan, CoverageSlot coverageOut);

/// ``ProportionalGlyphSheet::rasteriseSizedGlyph`` over the bitmap tier's
/// face, into a slot a frontend holds: the reference sheet's window, so the
/// bytes are the reference's.
///
/// - Parameters:
///   - unifontScale: The bitmap family's upscale.
///   - glyphID: The glyph, as ``shapeUnifontProportionalRun`` named it.
///   - face: The requested face style.
///   - variant: The sized origin's subpixel phase and coverage tier.
///   - tile: The scale, the band, the window and the doubling across.
///   - plan: The slot, from ``unifontProportionalGlyphSlotPlan``.
///   - coverageOut: The slot, `slotWidthInPixels * slotHeightInPixels` bytes.
/// - Returns: Whether the window was drawn.
[[nodiscard]] bool rasteriseUnifontSizedProportionalGlyph(int unifontScale, std::uint32_t glyphID, FontFace face,
    std::uint32_t variant, SizedTile tile, const ProportionalGlyphSlotPlan& plan, CoverageSlot coverageOut);

} // namespace Ubiquitty::Rendering
