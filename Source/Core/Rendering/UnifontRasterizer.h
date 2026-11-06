// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Rendering/UnifontRasterizer.h — the GlyphRasterizer that wraps the
// embedded 1-bit GNU Unifont (Rendering/Unifont.h). It carries the EXACT
// slot-fill logic of SoftwareRenderer::rasterizeInto and
// VulkanGlyphAtlas::rasterizeInto (native-size centered when the glyph fits,
// downscale-averaged when it is wider than the slot) so all backends produce
// identical coverage. It is also the no-real-font fallback the FreeTypeRasterizer
// delegates to for code points a font lacks. Portable: no platform /
// font-library types.

#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/UnifontFaceMetrics.h"

#include "Core/Rendering/FontSelection.h"
#include <cstdint>
#include <span>
#include <string>

namespace Ubiquitty::Rendering {

// Integer upscale for a point-sized frontend. The requested size and backing
// scale are explicit inputs, so glyph geometry cannot vary with a host font's
// metrics. Pixel-native backends use scale 1 directly.
[[nodiscard]] int unifontScaleForRequestedSize(double requestedPointSize, double backingScale);

/// The integer upscale for a configured family, or zero when the family is
/// not one Core rasterises itself. One call for a frontend that owns a
/// family name and a backing scale and wants Core's routing verdict and the
/// scale together — the map is Core's, and a frontend repeating it drifts.
[[nodiscard]] inline int unifontScaleForConfiguredFamily(const std::string& family, double requestedPointSize, double backingScale)
{
    if (selectFontFamily(family).rasterizerKind != FontRasterizerKind::EmbeddedBitmap)
        return 0;
    return unifontScaleForRequestedSize(requestedPointSize, backingScale);
}

/// The family a frame on the bitmap tier draws its text in, whichever family
/// it was configured with: the embedded family this rasterizer draws.
///
/// - Returns: GNU Unifont's family name, as the family table spells it.
[[nodiscard]] inline std::string unifontFamilyName()
{
    for (const auto& descriptor : embeddedFontFamilies) {
        if (descriptor.rasterizerKind == FontRasterizerKind::EmbeddedBitmap)
            return std::string(descriptor.family);
    }
    return {};
}

// One R8 coverage slot, row-major, as the seam's own name for the span.
//
// Named rather than spelled inline because `std::span`'s extent is a non-type
// template parameter with no Swift spelling: an alias is the one name both
// languages can write, so the Swift renderer calls the fill below without a
// shim in between.
using CoverageSlot = std::span<std::uint8_t>;

class UnifontRasterizer final : public GlyphRasterizer {
public:
    /// The embedded face at an upscale, with the reader's spacing folded into
    /// its cell.
    ///
    /// Both spacings are in whole pixels and both land in the CELL rather than
    /// in a pitch laid around it: the bitmap draws on whole pixels, so a cell
    /// that is not a whole number of them is a cell the face cannot fill, and a
    /// glyph spread over one is resampled instead of spaced. The raster sits
    /// centred in the gap the spacing opens, which is what puts
    /// `character_spacing` between two clusters rather than after one.
    ///
    /// - Parameters:
    ///   - scale: The integer upscale; at most 1 draws the native 8×16 cell.
    ///   - lineSpacingInPixels: Rows added to the cell, split around the glyph.
    ///   - characterSpacingInPixels: Columns added to the cell, split around
    ///     the glyph. Negative tightens it; the cell keeps one column.
    explicit UnifontRasterizer(int scale = 1, int lineSpacingInPixels = 0, int characterSpacingInPixels = 0);
    // The fixed 8×16 Unifont cell + the decoration metrics SoftwareRenderer
    // and the Vulkan grid path use — returned through this seam so the Unifont
    // path stays byte-identical across backends.
    [[nodiscard]] CellMetrics cellMetrics() const override;
    [[nodiscard]] FaceMetrics faceMetrics() const override;

    // Fill coverageOut (slotWidth*slotHeight R8, row-major) with the Unifont
    // glyph for codePoint. Style flags / weight are ignored (Unifont is a
    // single-weight bitmap face) but kept for interface parity. Returns false
    // (slot left blank) when Unifont lacks the code point.
    bool rasterizeGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override;
    /// Composites a cluster into a bounded coverage slot.
    bool rasteriseCluster(std::span<const std::uint32_t> codePoints, int slotWidth, int slotHeight, CoverageSlot coverageOut)
    {
        if (slotWidth <= 0 || slotHeight <= 0 || coverageOut.size() < static_cast<std::size_t>(slotWidth) * slotHeight) {
            return false;
        }
        return rasterizeCluster(codePoints.data(), static_cast<int>(codePoints.size()), FontFace::Regular, 0, slotWidth, slotHeight, coverageOut.data());
    }
    /// Rasterizes into a measured cell inside an atlas slot's margins.
    bool rasterizeGlyphInCell(std::uint32_t codePoint, int cellWidth, int cellHeight,
        int slotWidth, int slotHeight, CoverageSlot coverageOut);
    /// Whether a proportional face stands beside the bitmap grid face.
    ///
    /// Always: the bitmap tier carries Noto Sans pre-rendered into Core
    /// (`ProportionalStrike`), so a proportional run in this tier is one set
    /// of bytes on every arm rather than each arm's platform face.
    ///
    /// - Returns: True.
    [[nodiscard]] bool hasProportionalFace() const override { return true; }
    /// The rows the strike's ink reaches past the cell, above or below,
    /// times the upscale: a strip's slot grows by it on both sides so a
    /// descender at the cell's edge is drawn rather than cut.
    ///
    /// - Returns: The rows, zero when the strike fits the cell.
    [[nodiscard]] int proportionalStripOverhangInPixels() const override;
    /// Shapes a proportional run in the strike: the advances HarfBuzz
    /// measured for the face in 1/64 px, the pair kerning it recorded, no
    /// offsets, the glyph named by its code point.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - face: Regular or bold; italic resolves to its upright.
    ///   - glyphsOut: Caller-owned destination; its size is the bound.
    /// - Returns: How many entries were written; 0 refuses.
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override;
    /// Draws the glyphs ``shapeProportionalRun`` placed into a strip on the
    /// grid's baseline: at the strikes' two sizes each glyph's raster at the
    /// phase its fractional pen lands on, and on a grid upscaled past two the
    /// face's outlines at the grid's em.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets carrying the caller's pen.
    ///   - face: The face the glyphs were shaped in.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when the strip was drawn.
    bool rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override;
    /// Draws placed glyphs at a sized master, the face rasterised at that size:
    /// the strike that size is, the 24 px strike at twice the 12 px size as a
    /// scale-two grid draws it, and past the strikes' two sizes, or doubled
    /// across on a DECDWL row, the face's outlines drawn at the master's em
    /// (``ProportionalOutline``) — never a strike's pixels magnified. The
    /// baseline is the strip's at its own size taken `scale` times. A
    /// fraction declines.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets in master pixels.
    ///   - face: The face the glyphs were shaped in.
    ///   - master: The scale and the doubling across.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height, `scale` times the strip's.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when the master was drawn.
    bool rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, const SizedMaster& master, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override;

private:
    /// Draws placed glyphs from the proportional face's outlines at a size,
    /// and a code point the face lacks from the grid face by whole texels.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets in the destination's pixels.
    ///   - face: The face the glyphs were shaped in.
    ///   - pixelsPerEm: The face's em down, in pixels.
    ///   - across: The factor across past the em: 2 on a DECDWL row.
    ///   - advanceScale: What a glyph's advance is taken times to reach the next pen.
    ///   - baselineY: The baseline's row.
    ///   - fallbackScale: The grid face's whole-texel magnification past its upscale.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when every glyph was drawn.
    bool drawOutlines(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, double pixelsPerEm, int across,
        double advanceScale, int baselineY, int fallbackScale, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut);

    int m_scale;
    int m_lineSpacingInPixels;
    int m_characterSpacingInPixels;
};

/// The bitmap cell at a scale and its two spacings, without constructing the
/// rasterizer at the call site — the importer-friendly spelling of
/// ``UnifontRasterizer::cellMetrics``.
///
/// - Parameters:
///   - scale: The integer upscale.
///   - lineSpacingInPixels: Rows added to the cell.
///   - characterSpacingInPixels: Columns added to the cell.
/// - Returns: The cell the reader's settings ask the bitmap face for.
[[nodiscard]] inline CellMetrics unifontCellMetricsForScale(int scale, int lineSpacingInPixels, int characterSpacingInPixels)
{
    return UnifontRasterizer(scale, lineSpacingInPixels, characterSpacingInPixels).cellMetrics();
}

/// Fills a slot with one Unifont glyph, integer-upscaled so the bitmap tiles
/// the slot exactly rather than sitting native-size in its middle.
///
/// `rasterizeGlyph` centres a glyph that fits and averages one that does not,
/// which is the right answer for a pixel-native backend drawing into a slot
/// that is exactly its cell. A point-sized frontend asks for a cell an integer
/// multiple larger, and centring there leaves a small glyph in a large box. The
/// upscale is that frontend's policy and it lives here because every frontend
/// that wants it wants the same one — each source pixel `scale` times on both
/// axes, centred in a slot with room to spare and condensed into one without —
/// and a copy per frontend is a copy that drifts.
///
/// `coverageOut` is cleared before lookup, so a missing code point leaves a
/// blank slot. It arrives as a span because the slot's
/// extent and the buffer's are two facts: a caller that got the dimensions
/// right and the allocation wrong writes past the end, and the span is what
/// makes that checkable here rather than a crash at the caller.
///
/// - Parameters:
///   - codePoint: The character to draw.
///   - scale: The integer upscale factor; at most 1 draws native size.
///   - nativeWidth: The source cell width, 8 for one cell and 16 for two.
///   - nativeHeight: The source cell height.
///   - coverageOut: Slot coverage, `slotWidthInPixels * slotHeightInPixels` R8.
///   - slotWidthInPixels: The destination slot width. Wider than the upscaled
///     bitmap carries the reader's character spacing, and the glyph sits
///     centred in it; narrower condenses the glyph into it.
///   - slotHeightInPixels: The destination slot height. Taller than the
///     upscaled bitmap carries the line spacing, split around the glyph.
/// - Returns: Whether Unifont carried the code point.
[[nodiscard]] bool rasteriseUnifontIntoSlot(std::uint32_t codePoint, int scale,
    int nativeWidth, int nativeHeight, CoverageSlot coverageOut,
    int slotWidthInPixels, int slotHeightInPixels);

} // namespace Ubiquitty::Rendering
