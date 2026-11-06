// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ProportionalStripRaster.h"

#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/SizedMaster.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace Ubiquitty::Rendering {

namespace {

// The bitmap face for a frontend's slot. The slot is Unifont's cell plus the
// row's spacing plus the strike's overhang on both sides, and the
// rasteriser's baseline is what the spacing moves, so the spacing is read
// back from the slot rather than asked for twice.
UnifontRasterizer unifontFaceForSlot(int unifontScale, int slotHeightInPixels)
{
    const int scale = std::max(1, unifontScale);
    const UnifontRasterizer unspaced(scale, 0);
    const float cellHeight = unspaced.cellMetrics().cellHeight;
    const int lineSpacing = std::max(0, slotHeightInPixels - 2 * unspaced.proportionalStripOverhangInPixels() - static_cast<int>(cellHeight));
    return UnifontRasterizer(scale, lineSpacing);
}

} // namespace

ProportionalStripSlot resolveProportionalStripSlot(GlyphSlotCache& cache,
    ClusterSpan codePoints, const ProportionalStripRequest& request,
    const ProportionalStripLayout& layout, std::uint32_t variant,
    ProportionalStripVerification& verification)
{
    ProportionalStripSlot result;
    constexpr std::size_t metadataCount = 9;
    if (codePoints.empty() || request.widthCells < 1 || request.tileIndex < 0
        || request.tileIndex >= layout.tileCount
        || codePoints.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) - metadataCount) {
        return result;
    }
    const std::uint32_t residualCode = penResidualCode(layout.penResidualInPixels);
    verification.reserve(codePoints.size() + metadataCount);
    verification.assign(codePoints.begin(), codePoints.end());
    verification.push_back(0x5052'4F50u);
    verification.push_back(static_cast<std::uint32_t>(request.widthCells));
    verification.push_back(static_cast<std::uint32_t>(request.horizontalAlign));
    verification.push_back(residualCode);
    verification.push_back(static_cast<std::uint32_t>(request.tileIndex));
    verification.push_back(static_cast<std::uint32_t>(request.face));
    verification.push_back(static_cast<std::uint32_t>(request.widthScale));
    verification.push_back(static_cast<std::uint32_t>(request.scale));
    verification.push_back(static_cast<std::uint32_t>(request.band));
    const int count = static_cast<int>(verification.size());
    const std::uint32_t stripKey = ligatureStripKey(verification.data(), count, request.face, 0u);
    result.key = ligatureStripSlotKey(stripKey, variant, residualCode);
    result.slot = cache.slotForKeyVerified(result.key, verification.data(), count, &result.isFresh);
    return result;
}

ProportionalStripShape shapeProportionalStrip(GlyphRasterizer& rasterizer, ClusterSpan codePoints,
    const ProportionalStripRequest& request, std::vector<Ubiquitty::ProportionalShapedGlyph>& shapedOut)
{
    ProportionalStripShape shape;
    if (codePoints.empty() || request.widthCells < 1 || request.cellAdvanceInPixels <= 0.0f)
        return shape;
    const FontFace shapedFace = isBold(request.face) ? FontFace::Bold : FontFace::Regular;
    shapedOut.assign(codePoints.size() * 2, Ubiquitty::ProportionalShapedGlyph {});
    const int glyphCount = rasterizer.shapeProportionalRun(codePoints, shapedFace, shapedOut);
    if (glyphCount <= 0 || static_cast<std::size_t>(glyphCount) > shapedOut.size())
        return shape;
    shapedOut.resize(static_cast<std::size_t>(glyphCount));
    std::vector<ProportionalGlyphPlacement> placements(shapedOut.size());
    const ProportionalRunLayout runLayout = layoutProportionalRun(shapedOut, codePoints,
        request.widthCells, request.horizontalAlign, request.cellAdvanceInPixels, placements);
    // A strip drawn at a size is the run's own layout taken that many times
    // across, the pixels of the screen it is drawn on one to one.
    const int across = std::max(1, static_cast<int>(request.scale)) * std::max(1, static_cast<int>(request.widthScale));
    for (std::size_t index = 0; index < shapedOut.size(); ++index) {
        ProportionalShapedGlyph& glyph = shapedOut[index];
        // The strip carries alignment in its quad and pen; glyph offsets carry
        // only the shared layout's relative placement. Zero advances prevent a
        // rasterizer from applying the same spacing a second time.
        glyph.offsetXInPixels = static_cast<float>((placements[index].xInPixels - runLayout.penOffsetInPixels) * across);
        glyph.offsetYInPixels = placements[index].yInPixels * static_cast<float>(std::max(1, static_cast<int>(request.scale)));
        glyph.advanceInPixels = 0.0f;
    }
    shape.layout = proportionalStripLayout(runLayout.widthInPixels * across, request.widthCells * across, request.horizontalAlign,
        static_cast<double>(request.penResidualInPixels), static_cast<double>(request.cellAdvanceInPixels),
        static_cast<double>(request.marginXInPixels), static_cast<double>(request.slotWidthInPixels), request.tileIndex);
    shape.glyphCount = glyphCount;
    return shape;
}

bool rasteriseProportionalStrip(GlyphRasterizer& rasterizer, std::span<const Ubiquitty::ProportionalShapedGlyph> shaped,
    const ProportionalStripRequest& request, const ProportionalStripLayout& layout, CoverageSlot coverageOut)
{
    const std::size_t slotBytes = static_cast<std::size_t>(request.slotWidthInPixels) * static_cast<std::size_t>(request.slotHeightInPixels);
    if (shaped.empty() || layout.tileCount <= 0 || request.slotWidthInPixels <= 0 || request.slotHeightInPixels <= 0 || coverageOut.size() < slotBytes)
        return false;
    const FontFace shapedFace = isBold(request.face) ? FontFace::Bold : FontFace::Regular;
    // Core has placed every glyph; the strip's fractional pen translates that
    // placement into the slot without changing the spacing.
    std::vector<Ubiquitty::ProportionalShapedGlyph> placed(shaped.begin(), shaped.end());
    const float penX = static_cast<float>(layout.penXInSlotPixels);
    for (Ubiquitty::ProportionalShapedGlyph& glyph : placed)
        glyph.offsetXInPixels += penX;
    std::ranges::fill(coverageOut.first(slotBytes), std::uint8_t { 0 });
    SizedTile size { std::max<std::uint8_t>(request.scale, 1), request.band, 0 };
    size.widthScale = request.widthScale == 2 ? 2 : 1;
    const SizedMaster master = selectSizedMaster(size, {}, 0.0, 0.0);
    if (master.isIdentity()) {
        if (!rasterizer.rasteriseProportionalRun(placed, shapedFace, request.slotWidthInPixels, request.slotHeightInPixels, coverageOut))
            return false;
    } else {
        // The run at its size, on a canvas as many slots tall, of which the
        // slot is the band's slot height.
        if (size.band >= size.scale)
            return false;
        const int canvasHeight = master.scale.scaled(request.slotHeightInPixels);
        std::vector<std::uint8_t> canvas(static_cast<std::size_t>(request.slotWidthInPixels) * static_cast<std::size_t>(canvasHeight), 0);
        if (!rasterizer.rasteriseSizedProportionalRun(placed, shapedFace, master, request.slotWidthInPixels, canvasHeight, canvas))
            return false;
        std::ranges::copy(std::span<const std::uint8_t>(canvas).subspan(static_cast<std::size_t>(size.band) * slotBytes, slotBytes), coverageOut.begin());
    }
    // The clip: the run's cells, a margin each side when the text fits.
    const int clipLeft = static_cast<int>(std::clamp(std::floor(layout.clipLeftInSlotPixels), 0.0, static_cast<double>(request.slotWidthInPixels)));
    const int clipRight = static_cast<int>(std::clamp(std::ceil(layout.clipRightInSlotPixels), 0.0, static_cast<double>(request.slotWidthInPixels)));
    for (int y = 0; y < request.slotHeightInPixels; ++y) {
        std::uint8_t* rowStart = coverageOut.data() + static_cast<std::ptrdiff_t>(y) * request.slotWidthInPixels;
        std::fill(rowStart, rowStart + clipLeft, std::uint8_t { 0 });
        std::fill(rowStart + std::max(clipLeft, clipRight), rowStart + request.slotWidthInPixels, std::uint8_t { 0 });
    }
    return true;
}

int proportionalRunCellsInFace(GlyphRasterizer& rasterizer, ClusterSpan codePoints, float cellAdvanceInPixels)
{
    if (cellAdvanceInPixels <= 0.0f || codePoints.empty())
        return 0;
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(codePoints.size() * 2);
    const int count = rasterizer.shapeProportionalRun(codePoints, FontFace::Regular, shaped);
    if (count <= 0)
        return 0;
    std::vector<ProportionalGlyphPlacement> placements(static_cast<std::size_t>(count));
    const ProportionalRunLayout layout = layoutProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph>(shaped.data(), static_cast<std::size_t>(count)),
        codePoints, 1, 0, static_cast<double>(cellAdvanceInPixels), placements);
    return std::max(1, static_cast<int>(std::ceil(layout.widthInPixels / static_cast<double>(cellAdvanceInPixels) - 1e-6)));
}

ProportionalStripShape shapeUnifontProportionalStrip(int unifontScale, ClusterSpan codePoints, const ProportionalStripRequest& request)
{
    UnifontRasterizer face = unifontFaceForSlot(unifontScale, request.slotHeightInPixels);
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped;
    return shapeProportionalStrip(face, codePoints, request, shaped);
}

bool rasteriseUnifontProportionalStrip(int unifontScale, ClusterSpan codePoints, const ProportionalStripRequest& request, CoverageSlot coverageOut)
{
    UnifontRasterizer face = unifontFaceForSlot(unifontScale, request.slotHeightInPixels);
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped;
    const ProportionalStripShape shape = shapeProportionalStrip(face, codePoints, request, shaped);
    if (shape.glyphCount <= 0)
        return false;
    return rasteriseProportionalStrip(face, shaped, request, shape.layout, coverageOut);
}

int unifontProportionalRunCells(int unifontScale, ClusterSpan codePoints, float cellAdvanceInPixels)
{
    UnifontRasterizer face(std::max(1, unifontScale), 0);
    return proportionalRunCellsInFace(face, codePoints, cellAdvanceInPixels);
}

int unifontProportionalStripOverhangInPixels(int unifontScale)
{
    return UnifontRasterizer(std::max(1, unifontScale), 0).proportionalStripOverhangInPixels();
}

} // namespace Ubiquitty::Rendering
