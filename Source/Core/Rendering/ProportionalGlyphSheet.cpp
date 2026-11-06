// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ProportionalGlyphSheet.h"

#include "Core/Rendering/GlyphBackend.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/SizedMaster.h"
#include "Core/Rendering/SubpixelPhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ranges>

namespace Ubiquitty::Rendering {

ProportionalGlyphSheet::Plan ProportionalGlyphSheet::makePlan(int cellWidthInPixels,
    int cellHeightInPixels, int marginXInPixels, int marginYInPixels,
    GlyphRasterizer* rasterizer, int maxTextureDimension)
{
    Plan plan;
    if (!rasterizer || !rasterizer->hasProportionalFace()
        || cellWidthInPixels <= 0 || cellHeightInPixels <= 0
        || maxTextureDimension <= 0) {
        return plan;
    }

    const std::int64_t marginX = std::max(0, marginXInPixels);
    const std::int64_t marginY = static_cast<std::int64_t>(std::max(0, marginYInPixels))
        + std::max(0, rasterizer->proportionalStripOverhangInPixels());
    // Two cells leave room for the widest ordinary proportional glyph. The
    // final pixel is horizontal phase headroom, so an origin in the last phase
    // never clips the rightmost coverage from an otherwise fitting glyph.
    const std::int64_t slotWidth = 2 * static_cast<std::int64_t>(cellWidthInPixels)
        + 2 * marginX + 1;
    const std::int64_t slotHeight = static_cast<std::int64_t>(cellHeightInPixels)
        + 2 * marginY;
    if (slotWidth > maxTextureDimension || slotHeight > maxTextureDimension) {
        return plan;
    }
    plan.marginInPixels = { static_cast<int>(marginX), static_cast<int>(marginY) };
    plan.slotWidthInPixels = static_cast<int>(slotWidth);
    plan.slotHeightInPixels = static_cast<int>(slotHeight);
    plan.grid = clampAtlasGridToTextureLimit(proportionalGlyphAtlasNominalGrid(),
        { plan.slotWidthInPixels, plan.slotHeightInPixels }, maxTextureDimension);
    return plan;
}

ProportionalGlyphSheet::ProportionalGlyphSheet(int cellWidthInPixels,
    int cellHeightInPixels, int marginXInPixels, int marginYInPixels,
    GlyphRasterizer* rasterizer, int maxTextureDimension)
    : ProportionalGlyphSheet(rasterizer,
          makePlan(cellWidthInPixels, cellHeightInPixels, marginXInPixels,
              marginYInPixels, rasterizer, maxTextureDimension))
{
}

ProportionalGlyphSheet::ProportionalGlyphSheet(GlyphRasterizer* rasterizer, Plan plan)
    : m_rasterizer(rasterizer)
    , m_cache(plan.grid.valid ? plan.grid.columns * plan.grid.rows : 1,
          recommendedHashBuckets(plan.grid.valid ? plan.grid.columns * plan.grid.rows : 1),
          plan.grid.valid ? plan.grid.columns : 1)
{
    if (!plan.grid.valid) {
        return;
    }
    m_columns = plan.grid.columns;
    m_rows = plan.grid.rows;
    m_capacity = m_columns * m_rows;
    m_slotWidthInPixels = plan.slotWidthInPixels;
    m_slotHeightInPixels = plan.slotHeightInPixels;
    m_cache.enableOwnedInkRecords(1, m_slotWidthInPixels);
    m_sheetWidthInPixels = m_columns * m_slotWidthInPixels;
    m_sheetHeightInPixels = m_rows * m_slotHeightInPixels;
    m_marginInPixels = plan.marginInPixels;
    m_coverage.assign(static_cast<std::size_t>(m_sheetWidthInPixels)
            * static_cast<std::size_t>(m_sheetHeightInPixels),
        0);
    m_slotScratch.assign(static_cast<std::size_t>(m_slotWidthInPixels)
            * static_cast<std::size_t>(m_slotHeightInPixels),
        0);
    m_isUsable = m_rasterizer != nullptr && m_capacity > 0;
}

ProportionalAtlasBinding ProportionalGlyphSheet::binding()
{
    if (!m_isUsable) {
        return {};
    }
    return {
        &m_cache,
        m_columns,
        m_rows,
        m_slotWidthInPixels,
        m_slotHeightInPixels,
        m_capacity,
        m_marginInPixels,
        subpixelPhaseCount,
    };
}

int ProportionalGlyphSheet::shapeRun(ClusterSpan codePoints, FontFace face,
    std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut)
{
    if (!m_isUsable || codePoints.empty() || glyphsOut.empty()) {
        return 0;
    }
    return m_rasterizer->shapeProportionalRun(codePoints, face, glyphsOut);
}

bool ProportionalGlyphSheet::rasteriseGlyphIntoSlot(GlyphRasterizer& rasterizer,
    std::uint32_t glyphID, FontFace face, std::uint32_t variant,
    int marginXInPixels, int slotWidthInPixels, int slotHeightInPixels,
    std::span<std::uint8_t> coverageOut)
{
    if (glyphID == 0 || slotWidthInPixels <= 0 || slotHeightInPixels <= 0
        || coverageOut.size() != static_cast<std::size_t>(slotWidthInPixels) * static_cast<std::size_t>(slotHeightInPixels)) {
        return false;
    }
    std::ranges::fill(coverageOut, std::uint8_t { 0 });
    ProportionalShapedGlyph glyph {};
    glyph.glyph = glyphID;
    glyph.offsetXInPixels = static_cast<float>(marginXInPixels)
        + static_cast<float>(variantPhase(variant))
            / static_cast<float>(subpixelPhaseCount);
    const std::array<ProportionalShapedGlyph, 1> placed = { glyph };
    return rasterizer.rasteriseProportionalRun(placed, face,
        slotWidthInPixels, slotHeightInPixels, coverageOut);
}

bool ProportionalGlyphSheet::rasteriseSizedGlyphIntoSlot(GlyphRasterizer& rasterizer, std::uint32_t glyphID, FontFace face,
    std::uint32_t variant, SizedTile tile, int marginXInPixels, int slotWidthInPixels, int slotHeightInPixels,
    std::vector<std::uint8_t>& masterScratch, std::span<std::uint8_t> coverageOut)
{
    const std::size_t windowBytes = static_cast<std::size_t>(slotWidthInPixels) * static_cast<std::size_t>(slotHeightInPixels);
    if (glyphID == 0 || tile.scale < 1 || tile.band >= tile.scale || tile.isFractional() || coverageOut.size() < windowBytes) {
        return false;
    }
    // The width-axis facts on the raster seam describe the grid face, and the
    // proportional face is another: its doubling is a stretch of the whole two.
    const SizedMaster master = selectSizedMaster(tile, {}, 0.0, 0.0);
    if (master.isIdentity()) {
        return false;
    }
    const int canvasHeight = master.scale.scaled(slotHeightInPixels);
    masterScratch.assign(static_cast<std::size_t>(slotWidthInPixels) * static_cast<std::size_t>(canvasHeight), 0);
    ProportionalShapedGlyph glyph {};
    glyph.glyph = glyphID;
    glyph.offsetXInPixels = static_cast<float>(tile.scale * master.widthScale * marginXInPixels - tile.tile * slotWidthInPixels)
        + static_cast<float>(variantPhase(variant)) / static_cast<float>(subpixelPhaseCount);
    const std::array<ProportionalShapedGlyph, 1> placed = { glyph };
    if (!rasterizer.rasteriseSizedProportionalRun(placed, face, master, slotWidthInPixels, canvasHeight, masterScratch)) {
        return false;
    }
    const std::span<const std::uint8_t> cut = std::span<const std::uint8_t>(masterScratch).subspan(static_cast<std::size_t>(tile.band) * windowBytes, windowBytes);
    std::ranges::copy(cut, coverageOut.begin());
    return true;
}

bool ProportionalGlyphSheet::rasteriseSizedGlyph(std::uint32_t glyphID, FontFace face,
    std::uint32_t weight, std::uint32_t variant, SizedTile tile, int slot)
{
    (void)weight;
    if (!m_isUsable || slot < 0 || slot >= m_capacity) {
        return false;
    }
    if (!rasteriseSizedGlyphIntoSlot(*m_rasterizer, glyphID, face, variant, tile, m_marginInPixels.width,
            m_slotWidthInPixels, m_slotHeightInPixels, m_sizedScratch, m_slotScratch)) {
        return false;
    }
    const std::span<const std::uint8_t> cut = m_slotScratch;
    recordProportionalGlyphInk(m_cache, slot, m_slotScratch,
        m_slotWidthInPixels, m_slotHeightInPixels);
    const AtlasSlotRegion destination = atlasSlotRegion(slot, m_columns,
        m_slotWidthInPixels, m_slotHeightInPixels);
    for (int y = 0; y < m_slotHeightInPixels; ++y) {
        const std::span<const std::uint8_t> row = cut.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(m_slotWidthInPixels), static_cast<std::size_t>(m_slotWidthInPixels));
        std::ranges::copy(row, m_coverage.begin() + static_cast<std::ptrdiff_t>(destination.y + y) * m_sheetWidthInPixels + destination.x);
    }
    markSlotDirty(slot);
    ++m_generation;
    return true;
}

bool ProportionalGlyphSheet::rasteriseGlyph(std::uint32_t glyphID, FontFace face,
    std::uint32_t weight, std::uint32_t variant, int slot)
{
    (void)weight;
    if (!m_isUsable || glyphID == 0 || slot < 0 || slot >= m_capacity) {
        return false;
    }

    if (!rasteriseGlyphIntoSlot(*m_rasterizer, glyphID, face, variant,
            m_marginInPixels.width, m_slotWidthInPixels, m_slotHeightInPixels, m_slotScratch)) {
        return false;
    }
    recordProportionalGlyphInk(m_cache, slot, m_slotScratch,
        m_slotWidthInPixels, m_slotHeightInPixels);

    const AtlasSlotRegion region = atlasSlotRegion(slot, m_columns,
        m_slotWidthInPixels, m_slotHeightInPixels);
    for (int y = 0; y < m_slotHeightInPixels; ++y) {
        const std::uint8_t* source = m_slotScratch.data()
            + static_cast<std::ptrdiff_t>(y) * m_slotWidthInPixels;
        std::uint8_t* destination = m_coverage.data()
            + static_cast<std::ptrdiff_t>(region.y + y) * m_sheetWidthInPixels
            + region.x;
        std::memcpy(destination, source,
            static_cast<std::size_t>(m_slotWidthInPixels));
    }
    markSlotDirty(slot);
    ++m_generation;
    return true;
}

void ProportionalGlyphSheet::markSlotDirty(int slot)
{
    if (slot < 0) {
        return;
    }
    if (m_lastDirtySlot == INT_MAX) {
        return; // the never-uploaded sheet is already dirty in full
    }
    m_firstDirtySlot = std::min(m_firstDirtySlot, slot);
    if (slot > m_lastDirtySlot) {
        m_lastDirtySlot = slot;
    }
}

AtlasRowBand ProportionalGlyphSheet::dirtyRows() const
{
    if (!m_isUsable || m_lastDirtySlot < m_firstDirtySlot) {
        return {};
    }
    if (m_lastDirtySlot == INT_MAX) {
        return { 0, m_sheetHeightInPixels };
    }
    return atlasDirtyRowBand(m_firstDirtySlot, m_lastDirtySlot, m_columns,
        m_slotHeightInPixels, m_sheetHeightInPixels);
}

void ProportionalGlyphSheet::didUpload()
{
    m_firstDirtySlot = INT_MAX;
    m_lastDirtySlot = -1;
}

namespace {

// The bitmap face for a cell: Unifont's rows plus the spacing that makes up
// the cell, which is what moves the baseline the reference sheet draws on.
UnifontRasterizer unifontFaceForCell(int unifontScale, int cellHeightInPixels)
{
    const int scale = std::max(1, unifontScale);
    const UnifontRasterizer unspaced(scale, 0);
    const int lineSpacing = std::max(0, cellHeightInPixels - static_cast<int>(unspaced.cellMetrics().cellHeight));
    return UnifontRasterizer(scale, lineSpacing);
}

} // namespace

ProportionalGlyphSlotPlan unifontProportionalGlyphSlotPlan(int unifontScale,
    int cellWidthInPixels, int cellHeightInPixels, int maxTextureDimension)
{
    ProportionalGlyphSlotPlan out;
    UnifontRasterizer strike = unifontFaceForCell(unifontScale, cellHeightInPixels);
    // The margins the reference sheet is built with: the mono slot's overflow
    // room, which for the bitmap face is none. Asked of the face rather than
    // written as zero, so the two sheets can never disagree about it.
    const GlyphSlot monoSlot = computeGlyphSlot(strike.glyphOverhang());
    const ProportionalGlyphSheet::Plan plan = ProportionalGlyphSheet::makePlan(
        cellWidthInPixels, cellHeightInPixels, monoSlot.marginInPixels.width,
        monoSlot.marginInPixels.height, &strike, maxTextureDimension);
    if (!plan.grid.valid) {
        return out;
    }
    out.slotWidthInPixels = plan.slotWidthInPixels;
    out.slotHeightInPixels = plan.slotHeightInPixels;
    out.marginInPixels = plan.marginInPixels;
    out.columns = plan.grid.columns;
    out.rows = plan.grid.rows;
    out.valid = true;
    return out;
}

int shapeUnifontProportionalRun(int unifontScale, ClusterSpan codePoints, FontFace face,
    Ubiquitty::ProportionalShapedGlyph* glyphsOut, int capacity)
{
    if (!glyphsOut || capacity <= 0 || codePoints.empty()) {
        return 0;
    }
    UnifontRasterizer strike(std::max(1, unifontScale), 0);
    return strike.shapeProportionalRun(codePoints, face,
        std::span<Ubiquitty::ProportionalShapedGlyph>(glyphsOut, static_cast<std::size_t>(capacity)));
}

bool rasteriseUnifontProportionalGlyph(int unifontScale, std::uint32_t glyphID, FontFace face,
    std::uint32_t variant, const ProportionalGlyphSlotPlan& plan, CoverageSlot coverageOut)
{
    if (!plan.valid) {
        return false;
    }
    // The cell's height is the slot's less the room the plan added around it.
    const int cellHeightInPixels = plan.slotHeightInPixels - 2 * plan.marginInPixels.height;
    UnifontRasterizer strike = unifontFaceForCell(unifontScale, cellHeightInPixels);
    return ProportionalGlyphSheet::rasteriseGlyphIntoSlot(strike, glyphID, face, variant,
        plan.marginInPixels.width, plan.slotWidthInPixels, plan.slotHeightInPixels, coverageOut);
}

bool rasteriseUnifontSizedProportionalGlyph(int unifontScale, std::uint32_t glyphID, FontFace face,
    std::uint32_t variant, SizedTile tile, const ProportionalGlyphSlotPlan& plan, CoverageSlot coverageOut)
{
    if (!plan.valid) {
        return false;
    }
    const int cellHeightInPixels = plan.slotHeightInPixels - 2 * plan.marginInPixels.height;
    UnifontRasterizer strike = unifontFaceForCell(unifontScale, cellHeightInPixels);
    std::vector<std::uint8_t> master;
    return ProportionalGlyphSheet::rasteriseSizedGlyphIntoSlot(strike, glyphID, face, variant, tile,
        plan.marginInPixels.width, plan.slotWidthInPixels, plan.slotHeightInPixels, master, coverageOut);
}

} // namespace Ubiquitty::Rendering
