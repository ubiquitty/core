// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/SoftwareRenderer.cpp — see SoftwareRenderer.h.

#include "Core/Rendering/SoftwareRenderer.h"

#include "Core/Rendering/AtlasCoverage.h"
#include "Core/Rendering/BackgroundImage.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/Rendering/BoxDrawCommands.h"
#include "Core/Rendering/GlyphFault.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Rendering/PaddingGeometry.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Rendering/SizedMaster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace Ubiquitty::Rendering {

// The colour slot in whole atlas pixels. The square is a float because the cell
// is; a slot is an integer count of texels, and rounding down keeps the slot
// inside the square it draws into rather than a texel past it.
namespace {
int colourGlyphSlotInPixels(int cellWidthInPixels, int cellHeightInPixels)
{
    const float square = colourGlyphSquareInPixels(static_cast<float>(cellWidthInPixels), static_cast<float>(cellHeightInPixels));
    const int whole = static_cast<int>(square);
    return whole > 0 ? whole : 1;
}

// The colour sheet's grid, from Core's own derivation so the Vulkan sheet
// beside this one lands on the same one. A CPU sheet has no texture limit of
// its own and passes the floor every target guarantees, for the same reason.
AtlasGridFit colourAtlasGrid(int cellWidthInPixels, int cellHeightInPixels)
{
    return colourGlyphAtlasGrid(colourGlyphSlotInPixels(cellWidthInPixels, cellHeightInPixels),
        sharedColourAtlasTextureLimit);
}

// The mono and wide sheets' grids, by the rule every coverage sheet follows,
// held to the texture limit a GPU uploading them is held to. A slot past that
// limit is no font this renderer is given; one slot is then the grid, so every
// member is still constructed.
constexpr int monoSheetColumns = 32;
constexpr int wideSheetColumns = 16;

GridSize coverageSheetGrid(int cellsOnScreen, int cellsPerSlot, int columns, IntSize slot)
{
    const AtlasGridFit fit = glyphSheetGridForScreen(cellsOnScreen, cellsPerSlot, columns, slot, sharedColourAtlasTextureLimit);
    if (!fit.valid || fit.columns <= 0 || fit.rows <= 0)
        return { 1, 1 };
    return { fit.columns, fit.rows };
}
}

namespace {

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A quad's overlap with the surface, resolved once for a whole composite rather
// than tested per pixel: the bounds do not change inside the loops they guard,
// and these run per pixel of the frame where the rest of the backend runs per
// cell of it. Empty when the quad falls outside entirely, which is the caller's
// signal to draw nothing at all.
struct ClippedQuad {
    int firstColumn = 0; // offsets into the quad, not the surface
    int lastColumn = 0;
    int firstRow = 0;
    int lastRow = 0;

    [[nodiscard]] bool isEmpty() const { return firstRow >= lastRow || firstColumn >= lastColumn; }
};

// `origin` is where the quad's own (0, 0) sits on the surface; the returned
// bounds are offsets within the quad, so a caller adds the origin back to reach
// a pixel.
[[nodiscard]] inline ClippedQuad clipQuadToSurface(int originX, int originY, int quadWidth, int quadHeight, IntSize surface)
{
    return {
        .firstColumn = originX < 0 ? -originX : 0,
        .lastColumn = originX + quadWidth > surface.width ? surface.width - originX : quadWidth,
        .firstRow = originY < 0 ? -originY : 0,
        .lastRow = originY + quadHeight > surface.height ? surface.height - originY : quadHeight,
    };
}

// Choose opacity once per quad. Ordinary opaque ink retains its original
// coverage loop; blink and conceal scale only the quads that carry them.
template<typename Paint>
void withForegroundCoverage(float opacity, const Paint& paint)
{
    const int alpha = channelToByte(std::abs(opacity));
    if (alpha == 255)
        paint([](int coverage) { return coverage; });
    else
        paint([alpha](int coverage) { return (coverage * alpha + 127) / 255; });
}

// mix(background, foreground, coverage) = round((background*(255-coverage) + foreground*coverage) / 255).
//
// The CPU<->GPU oracle, and the reason it is one function: the float mix into a
// UNORM8 store diverges from this by a least significant bit once coverage is
// anti-aliased, so the GPU's cell shader runs this same integer formula and
// emits byte/255. The two paths land on the identical byte only while the two
// spellings stay identical, and a formula written out in three composites is
// three spellings that have to be kept so.
//
// At coverage 0 it is the background byte and at 255 the foreground, since the
// remainder can never reach 255.
[[nodiscard]] inline std::uint8_t mixCoverage(int background, int foreground, int coverage)
{
    return static_cast<std::uint8_t>((background * (255 - coverage) + foreground * coverage + 127) / 255);
}

// The four channels of one pixel, with alpha mixed toward opaque: text over the
// background's own alpha, which is the window's opacity.
inline void writeMixedPixel(std::uint8_t* out, int backgroundRed, int backgroundGreen, int backgroundBlue,
    int backgroundAlpha, int foregroundRed, int foregroundGreen, int foregroundBlue, int coverage)
{
    out[0] = mixCoverage(backgroundRed, foregroundRed, coverage);
    out[1] = mixCoverage(backgroundGreen, foregroundGreen, coverage);
    out[2] = mixCoverage(backgroundBlue, foregroundBlue, coverage);
    out[3] = mixCoverage(backgroundAlpha, 255, coverage);
}

// One pixel of ink over `ground`, four bytes a glyph pass picks: its instance's
// cell background, or the surface pixel `out` itself. Every ground is read
// before `out` is written, so the two may be the same pixel.
inline void writeInkPixel(std::uint8_t* out, const std::uint8_t* ground, int foregroundRed, int foregroundGreen, int foregroundBlue, int coverage)
{
    writeMixedPixel(out, ground[0], ground[1], ground[2], ground[3], foregroundRed, foregroundGreen, foregroundBlue, coverage);
}

// The inline span-column table. A cell spans at most four times its width (a
// wide cell on a doubled row), so this covers every cell geometry a font
// produces; past it the table is taken from the heap instead.
constexpr std::size_t maxSpanTexelColumns = 512;

} // namespace

SoftwareRenderer::Atlas::Atlas(int capacityIn, int columns, int slotWidthInPixels, int slotHeightInPixels, int glyphWidthInPixels, IntSize marginInPixels, int bytesPerPixelIn)
    : cache(capacityIn, recommendedHashBuckets(capacityIn), columns)
    , geometry { columns, slotWidthInPixels, slotHeightInPixels, glyphWidthInPixels > 0 ? glyphWidthInPixels : slotWidthInPixels, { marginInPixels.width, marginInPixels.height } }
    , capacity(capacityIn)
    , atlasWidth(columns * slotWidthInPixels)
    , atlasHeight((capacityIn / columns) * slotHeightInPixels)
    , bytesPerPixel(bytesPerPixelIn > 0 ? bytesPerPixelIn : 1)
{
    pixels.assign(static_cast<std::size_t>(atlasWidth) * atlasHeight * bytesPerPixel, 0);
}

namespace {
// The horizontal overflow margin (px per side) the shared sizing policy derives
// from the rasterizer's font overhang. Zero for Unifont / no real face (the mono
// slot stays cell-tight); a few px for a face whose Nerd-Font fallback spills
// past the cell. The slot is symmetric (margin each side, no +1) — phase count 1,
// so the cell box stays centered and the cell-baked raster is unshifted.
int monoOverflowMarginX(GlyphRasterizer& rasterizer) { return computeGlyphSlot(rasterizer.glyphOverhang()).marginInPixels.width; }
// The vertical overflow margin (px per side) — a fallback face whose ink reaches
// past the cell ascent/descent. The taller slot keeps the glyph on the cell
// baseline (FreeTypeRasterizer::primaryBaselineY shifts it down by this margin).
int monoOverflowMarginY(GlyphRasterizer& rasterizer) { return computeGlyphSlot(rasterizer.glyphOverhang()).marginInPixels.height; }
// The largest slot dims the scratch the rasteriser fills must hold: the wider of
// the mono (cell + 2·marginInPixels.width) and wide (2·cell) slots, and the taller of the mono
// (cell + 2·marginInPixels.height) and wide (cell) slots.
int maxSlotWidthInPixels(GlyphRasterizer& rasterizer, int cellWidth)
{
    const int mono = cellWidth + 2 * monoOverflowMarginX(rasterizer);
    const int wide = cellWidth * 2;
    return mono > wide ? mono : wide;
}
int maxSlotHeightInPixels(GlyphRasterizer& rasterizer, int cellHeight)
{
    return cellHeight + 2 * monoOverflowMarginY(rasterizer); // mono is the taller slot
}
} // namespace

// Mono slots are one cell wide (Unifont narrow, 8px); wide slots two (16px);
// both 16px tall. Capacities cover a large screen's distinct glyph set with
// cross-frame LRU reuse. The scratch slot is wide-sized (the largest slot) so
// one buffer serves both atlases.
// The bitmap tier's partial coverage, held to 51 levels before it mixes. A
// rule edge, a curl and a rounded corner are float coverages, and the GPU arms
// reach the same value through their own sin and sqrt: equal to a millionth,
// and a millionth is enough to round a byte the other way where the exact mix
// lands on a half. Fifty-one levels keep the ramp smooth to the eye and put
// the two arithmetics on the same code except within a millionth of a level
// boundary, which no case has reached. Only the bitmap tier calls it: the
// native tiers are each their own reference and keep the full ramp. Fifty-one because every level is an
// exact byte (5k of 255), so a blender that narrows the alpha to eight bits
// before it mixes uses the same value, and because with an odd count no mix
// of two bytes lands on an exact half, so a blender that rounds halves to
// even agrees with the round-half-up here.
static float quantizedCoverage(float alpha)
{
    return std::floor(alpha * 51.0f + 0.5f) / 51.0f;
}

SoftwareRenderer::SoftwareRenderer(int cellWidthInPixels, int cellHeightInPixels)
    : SoftwareRenderer(cellWidthInPixels, cellHeightInPixels, m_unifont)
{
}

SoftwareRenderer::SoftwareRenderer(int cellWidthInPixels, int cellHeightInPixels, GlyphRasterizer& rasterizer)
    : m_cellWidth(cellWidthInPixels)
    , m_cellHeight(cellHeightInPixels)
    , m_mono(coverageSheetGrid(0, 1, monoSheetColumns, { cellWidthInPixels + 2 * monoOverflowMarginX(rasterizer), cellHeightInPixels + 2 * monoOverflowMarginY(rasterizer) }),
          cellWidthInPixels + 2 * monoOverflowMarginX(rasterizer), cellHeightInPixels + 2 * monoOverflowMarginY(rasterizer),
          /*glyphWidthInPixels=*/cellWidthInPixels,
          /*marginInPixels=*/ { monoOverflowMarginX(rasterizer), monoOverflowMarginY(rasterizer) })
    , m_wide(coverageSheetGrid(0, 2, wideSheetColumns, { cellWidthInPixels * 2, cellHeightInPixels }), cellWidthInPixels * 2, cellHeightInPixels)
    , m_hasColorSource(rasterizer.hasColorGlyphs())
    // Square at the cell height. A colourless rasterizer gets a one-pixel sheet
    // rather than none, so every member is constructed unconditionally and the
    // absence lives in m_hasColorSource instead of in an extent each accessor
    // would have to interpret.
    , m_color(m_hasColorSource ? colourAtlasGrid(cellWidthInPixels, cellHeightInPixels).columns * colourAtlasGrid(cellWidthInPixels, cellHeightInPixels).rows : 1,
          m_hasColorSource ? colourAtlasGrid(cellWidthInPixels, cellHeightInPixels).columns : 1,
          m_hasColorSource ? colourGlyphSlotInPixels(cellWidthInPixels, cellHeightInPixels) : 1,
          m_hasColorSource ? colourGlyphSlotInPixels(cellWidthInPixels, cellHeightInPixels) : 1,
          /*glyphWidthInPixels=*/-1, /*marginInPixels=*/ {}, /*bytesPerPixel=*/4)
    , m_hasLigatureShaping(rasterizer.hasLigatureShaping())
    // The strip sheet stands for a face that shapes ligatures or one with a
    // proportional face beside it: the bitmap tier ligates nothing and still
    // draws proportional runs, from the strike Core carries.
    , m_hasStripSheet(rasterizer.hasProportionalFace())
    , m_proportional(cellWidthInPixels, cellHeightInPixels,
          monoOverflowMarginX(rasterizer), monoOverflowMarginY(rasterizer),
          &rasterizer, 2048)
    , m_proportionalStrip(m_hasStripSheet ? cellWidthInPixels : 1, m_hasStripSheet ? cellHeightInPixels : 1,
          m_hasStripSheet ? monoOverflowMarginX(rasterizer) : 0,
          m_hasStripSheet ? monoOverflowMarginY(rasterizer) : 0,
          m_hasStripSheet ? &rasterizer : nullptr)
    // The margins the mono slots use, so a strip's slot and a glyph's slot are
    // the same box and the builder's sub-slice map lands on the cell it means.
    , m_ligature(m_hasLigatureShaping ? cellWidthInPixels : 1, m_hasLigatureShaping ? cellHeightInPixels : 1,
          m_hasLigatureShaping ? monoOverflowMarginX(rasterizer) : 0,
          m_hasLigatureShaping ? monoOverflowMarginY(rasterizer) : 0,
          m_hasLigatureShaping ? &rasterizer : nullptr)
    , m_rasterizer(rasterizer)
    , m_slotScratch(static_cast<std::size_t>(maxSlotWidthInPixels(rasterizer, cellWidthInPixels)) * maxSlotHeightInPixels(rasterizer, cellHeightInPixels), 0)
{
}

// Font-derived: size the atlas slots to the rasterizer's cellMetrics() cell
// (rounded to whole px) before allocating, so a glyph at the face's pixel
// size fills its slot. Delegates to the dim-taking ctor.
namespace {
} // namespace

SoftwareRenderer::SoftwareRenderer(GlyphRasterizer& rasterizer)
    : SoftwareRenderer(cellDimension(rasterizer.cellMetrics().cellWidth), cellDimension(rasterizer.cellMetrics().cellHeight), rasterizer)
{
}

SoftwareRenderer::AtlasView SoftwareRenderer::monoAtlas() const
{
    if (m_mono.pixels.empty())
        return {};
    return { m_mono.pixels.data(), m_mono.atlasWidth, m_mono.atlasHeight };
}

SoftwareRenderer::AtlasView SoftwareRenderer::wideAtlas() const
{
    if (m_wide.pixels.empty())
        return {};
    return { m_wide.pixels.data(), m_wide.atlasWidth, m_wide.atlasHeight };
}

SoftwareRenderer::AtlasView SoftwareRenderer::proportionalAtlas() const
{
    const std::span<const std::uint8_t> coverage = m_proportional.coverage();
    if (!m_proportional.isUsable() || coverage.empty())
        return {};
    return { coverage.data(), m_proportional.widthInPixels(),
        m_proportional.heightInPixels() };
}

SoftwareRenderer::AtlasView SoftwareRenderer::proportionalStripAtlas() const
{
    const std::span<const std::uint8_t> coverage = m_proportionalStrip.coverage();
    if (!m_proportionalStrip.isUsable() || coverage.empty())
        return {};
    return { coverage.data(), m_proportionalStrip.widthInPixels(),
        m_proportionalStrip.heightInPixels() };
}

SoftwareRenderer::AtlasView SoftwareRenderer::ligatureAtlas() const
{
    const std::span<const std::uint8_t> coverage = m_ligature.coverage();
    if (!m_ligature.isUsable() || coverage.empty()) {
        return {};
    }
    return { coverage.data(), m_ligature.widthInPixels(), m_ligature.heightInPixels() };
}

SoftwareRenderer::AtlasView SoftwareRenderer::colorAtlas() const
{
    if (!m_hasColorSource || m_color.pixels.empty())
        return {};
    return { m_color.pixels.data(), m_color.atlasWidth, m_color.atlasHeight };
}

bool SoftwareRenderer::rasteriseColorGlyph(const std::uint32_t* codePoints, int count, int slot)
{
    if (!codePoints || count <= 0 || slot < 0 || slot >= m_color.capacity)
        return false;
    ++m_colorAtlasGeneration;
    const int edge = m_color.geometry.slotWidthInPixels;
    const std::size_t rowBytes = static_cast<std::size_t>(edge) * 4;
    if (m_colorScratch.size() < rowBytes * edge)
        m_colorScratch.assign(rowBytes * edge, 0);
    const bool present = m_rasterizer.rasterizeColorCluster(codePoints, count, edge, m_colorScratch.data());
    const int x0 = (slot % m_color.geometry.columns) * edge;
    const int y0 = (slot / m_color.geometry.columns) * edge;
    // Cleared on a decline rather than left alone: the slot is whatever the last
    // tenant wrote, and the cache has already handed the key over, so leaving it
    // draws the evicted emoji under this one's name for as long as it stays
    // resident.
    for (int y = 0; y < edge; ++y) {
        std::uint8_t* destination = &m_color.pixels[(static_cast<std::size_t>(y0 + y) * m_color.atlasWidth + x0) * 4];
        if (present)
            std::memcpy(destination, m_colorScratch.data() + static_cast<std::size_t>(y) * rowBytes, rowBytes);
        else
            std::memset(destination, 0, rowBytes);
    }
    return true;
}

// The keys are the builder's, not this file's. The builder probes the cache
// directly whenever `bindColorAtlas` hands one over, so a key derived any other
// way here would fault a second slot for a glyph already resident and the two
// paths would disagree about what is cached.
//
// `clusterID` is unused for that reason: the key comes from the code points, so
// the ID distinguishes nothing the points do not, and folding it in is exactly
// the disagreement above.
AtlasGlyph SoftwareRenderer::sizedColorGlyph(std::span<const std::uint32_t> codePoints, SizedTile tile)
{
    if (!m_hasColorSource || codePoints.empty() || !(tile.scale >= 2 || tile.isFractional()))
        return {};
    const SizedColorTileContents contents = sizedColorTileContents(codePoints, tile);
    if (contents.count == 0)
        return {};
    bool isFresh = false;
    const int slot = m_color.cache.slotForKeyVerified(contents.key, contents.words.data(), contents.count, &isFresh);
    if (slot < 0)
        return {};
    if (isFresh) {
        ++m_colorAtlasGeneration;
        const int edge = m_color.geometry.slotWidthInPixels;
        const std::size_t rowBytes = static_cast<std::size_t>(edge) * 4;
        if (m_colorScratch.size() < rowBytes * static_cast<std::size_t>(edge))
            m_colorScratch.assign(rowBytes * static_cast<std::size_t>(edge), 0);
        const std::span<std::uint8_t> window = std::span(m_colorScratch).first(rowBytes * static_cast<std::size_t>(edge));
        if (!rasteriseSizedColorWindow(m_rasterizer, codePoints, tile, edge, m_sizedScratch, window)) {
            m_color.cache.forgetKey(contents.key);
            return {};
        }
        const int x0 = (slot % m_color.geometry.columns) * edge;
        const int y0 = (slot / m_color.geometry.columns) * edge;
        for (int y = 0; y < edge; ++y) {
            const std::span<const std::uint8_t> row = window.subspan(static_cast<std::size_t>(y) * rowBytes, rowBytes);
            std::ranges::copy(row, m_color.pixels.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(m_color.atlasWidth) + static_cast<std::size_t>(x0)) * 4));
        }
    }
    const GlyphUV uv = bindColorAtlas().atlasRectForSlot(slot);
    return { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
}

AtlasGlyph SoftwareRenderer::colorGlyph(const std::uint32_t* codePoints, int count, std::uint16_t)
{
    if (!m_hasColorSource || !codePoints || count <= 0)
        return {};
    bool isFresh = false;
    const int slot = count > 1
        ? m_color.cache.slotForKeyVerified(colorClusterKey(codePoints, count), codePoints, count, &isFresh)
        : m_color.cache.slotForKey(colorGlyphKey(codePoints[0]), &isFresh);
    if (slot < 0)
        return {};
    if (isFresh)
        rasteriseColorGlyph(codePoints, count, slot);
    const ColorAtlasBinding binding = bindColorAtlas();
    const GlyphUV uv = binding.atlasRectForSlot(slot);
    return { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
}

ColorAtlasBinding SoftwareRenderer::bindColorAtlas()
{
    ColorAtlasBinding binding;
    binding.cache = &m_color.cache;
    binding.columns = m_color.geometry.columns;
    binding.rows = m_color.geometry.columns > 0 ? m_color.capacity / m_color.geometry.columns : 0;
    binding.slotInPixels = m_color.geometry.slotWidthInPixels;
    binding.slotCapacity = m_color.capacity;
    return binding;
}

void SoftwareRenderer::recutAtlas(Atlas& atlas, GridSize grid)
{
    const GlyphSlotGeometry geometry = atlas.geometry;
    const int bytesPerPixel = atlas.bytesPerPixel;
    std::destroy_at(&atlas);
    std::construct_at(&atlas, grid, geometry.slotWidthInPixels, geometry.slotHeightInPixels, geometry.glyphWidthInPixels,
        geometry.glyphMarginInPixels, bytesPerPixel);
}

void SoftwareRenderer::reserveSheetsForScreen(int cellsOnScreen)
{
    const GridSize mono = coverageSheetGrid(cellsOnScreen, 1, monoSheetColumns, { m_mono.geometry.slotWidthInPixels, m_mono.geometry.slotHeightInPixels });
    if (mono.columns * mono.rows > m_mono.capacity) {
        recutAtlas(m_mono, mono);
        ++m_monoAtlasGeneration;
    }
    const GridSize wide = coverageSheetGrid(cellsOnScreen, 2, wideSheetColumns, { m_wide.geometry.slotWidthInPixels, m_wide.geometry.slotHeightInPixels });
    if (wide.columns * wide.rows > m_wide.capacity) {
        recutAtlas(m_wide, wide);
        ++m_wideAtlasGeneration;
    }
}

void SoftwareRenderer::beginFrame()
{
    m_mono.cache.beginFrame();
    m_wide.cache.beginFrame();
    m_color.cache.beginFrame();
    m_proportional.beginFrame();
    m_proportionalStrip.beginFrame();
    // The strip sheet ages with the rest: a run a row stopped using is reclaimed
    // rather than held until the sheet fills. Unconditional — the placeholder
    // sheet's cache is empty and ageing it costs nothing.
    m_ligature.beginFrame();
}

ProportionalAtlasBinding SoftwareRenderer::bindProportionalAtlas()
{
    return m_proportional.binding();
}

int SoftwareRenderer::shapeProportionalRun(ClusterSpan codePoints, FontFace face,
    std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut)
{
    return m_proportional.shapeRun(codePoints, face, glyphsOut);
}

bool SoftwareRenderer::rasteriseProportionalGlyph(std::uint32_t glyphID,
    FontFace face, std::uint32_t weight, std::uint32_t variant, int slot)
{
    return m_proportional.rasteriseGlyph(glyphID, face, weight, variant, slot);
}

// Fault a glyph's coverage into a slot via the injected rasterizer (default
// Unifont). The rasterizer fills a slot-local R8 scratch buffer (the raster
// seam); we blit that into the atlas's slot sub-rect. On a false return (the
// rasterizer has no glyph) the slot is left blank.
//
// The face reaches the rasterizer, because the slot keys spend a slot on it:
// `monoGlyphKey` and `wideGlyphKey` both take a face, so a bold cell holds a
// slot of its own. Naming a fixed face here would fill that slot with another
// one's glyph, leaving the key and the bytes disagreeing about what the slot
// holds — which is the one thing a cache cannot survive.
//
// A rasterizer with one weight is free to ignore the argument, and
// `UnifontRasterizer` does, so the portable tier draws the same either way.
// The rasterizers behind the other arms have bold members and answer it.
//
// Weight is a different question and stays pinned at zero: this rasterizer
// applies no variable axis, and the bindings report zero so key and raster
// agree about that too.
void SoftwareRenderer::rasterizeInto(Atlas& atlas, int slot, std::uint32_t codePoint, FontFace face)
{
    // Whichever sheet this call was handed. The reference is the only thing
    // that says which, so the count is taken from it rather than from the
    // caller's intent.
    ++(&atlas == &m_wide ? m_wideAtlasGeneration : m_monoAtlasGeneration);
    const int columns = atlas.geometry.columns;
    const int slotWidthInPixels = atlas.geometry.slotWidthInPixels;
    const int slotHeightInPixels = atlas.geometry.slotHeightInPixels;
    const int x0 = (slot % columns) * slotWidthInPixels;
    const int y0 = (slot / columns) * slotHeightInPixels;

    const bool present = m_rasterizer.rasterizeGlyph(codePoint, face,
        /*weight=*/0, slotWidthInPixels, slotHeightInPixels, m_slotScratch.data());
    writeAtlasCoverage(atlas.pixels, atlas.atlasWidth, { x0, y0, slotWidthInPixels, slotHeightInPixels },
        present ? std::span<const std::uint8_t>(m_slotScratch).first(static_cast<std::size_t>(slotWidthInPixels) * slotHeightInPixels) : std::span<const std::uint8_t> {});
}

bool SoftwareRenderer::stageMonoCoverage(int slot, std::span<const std::uint8_t> coverage)
{
    const GlyphSlotGeometry& geometry = m_mono.geometry;
    const int slotWidth = geometry.slotWidthInPixels;
    const int slotHeight = geometry.slotHeightInPixels;
    // The count is checked rather than trusted: it is the caller's idea of this
    // slot's size, and a plane the wrong shape would be blitted row by row into
    // the neighbours rather than refused.
    if (slot < 0 || slot >= m_mono.capacity || geometry.columns <= 0
        || coverage.size() != static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight))
        return false;

    ++m_monoAtlasGeneration;
    const int x0 = (slot % geometry.columns) * slotWidth;
    const int y0 = (slot / geometry.columns) * slotHeight;
    writeAtlasCoverage(m_mono.pixels, m_mono.atlasWidth, { x0, y0, slotWidth, slotHeight }, coverage);
    return true;
}

void SoftwareRenderer::rasterizeClusterInto(Atlas& atlas, int slot, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight)
{
    // Whichever sheet this call was handed. The reference is the only thing
    // that says which, so the count is taken from it rather than from the
    // caller's intent.
    ++(&atlas == &m_wide ? m_wideAtlasGeneration : m_monoAtlasGeneration);
    const GlyphSlotGeometry& geometry = atlas.geometry;
    const int width = geometry.slotWidthInPixels;
    const int height = geometry.slotHeightInPixels;
    const AtlasSlotRegion region { (slot % geometry.columns) * width, (slot / geometry.columns) * height, width, height };
    // A cluster the rasterizer declines still lands as a blank: the slot is
    // this key's now, not the previous tenant's ink.
    const bool inked = m_rasterizer.rasterizeCluster(codePoints.data(), static_cast<int>(codePoints.size()), face, weight, width, height, m_slotScratch.data());
    const std::span<const std::uint8_t> plane = inked ? std::span<const std::uint8_t>(m_slotScratch).first(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) : std::span<const std::uint8_t> {};
    writeAtlasCoverage(atlas.pixels, atlas.atlasWidth, region, plane);
}

// Reached by no unit test. The builder probes the cache itself and asks for a
// fill through rasteriseMonoGlyph, so the mono path through here belongs to
// callers that fault a glyph directly, and shell_snapshots is the only lane that
// executes it — a 36-second run is the cheapest thing that can fail on a change
// to this function.
//
// Stated because the shape misleads: disabling the rasterise below leaves every
// test under Tests/Core/Rendering green, which reads as dead code and is not.
AtlasGlyph SoftwareRenderer::faultGlyph(Atlas& atlas, std::uint32_t key, std::uint32_t codePoint, FontFace face)
{
    bool fresh = false;
    const int slot = atlas.cache.slotForKey(key, &fresh);
    if (slot < 0)
        return {}; // atlas full this frame -> draw background only
    if (fresh)
        rasterizeInto(atlas, slot, codePoint, face);
    const GlyphUV uv = atlas.geometry.atlasRectForSlot(slot, atlas.capacity);
    AtlasGlyph out;
    out.minUV = { uv.minU, uv.minV };
    out.maxUV = { uv.maxU, uv.maxV };
    out.present = true;
    return out;
}

AtlasGlyph SoftwareRenderer::faultCluster(Atlas& atlas, std::uint32_t key, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight)
{
    const ClusterSlot found = faultClusterSlot(atlas.cache, key, codePoints.data(), static_cast<int>(codePoints.size()));
    if (found.slot < 0)
        return {}; // the sheet is full this frame: background only
    if (found.isFresh)
        rasterizeClusterInto(atlas, found.slot, codePoints, face, weight);
    const GlyphUV uv = atlas.geometry.atlasRectForSlot(found.slot, atlas.capacity);
    return { .minUV = { uv.minU, uv.minV }, .maxUV = { uv.maxU, uv.maxV }, .present = true };
}

void SoftwareRenderer::rasteriseSizedWindowInto(Atlas& atlas, int slot, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile)
{
    const bool isWide = &atlas == &m_wide;
    ++(isWide ? m_wideAtlasGeneration : m_monoAtlasGeneration);
    const GlyphSlotGeometry& geometry = atlas.geometry;
    // A wide window is two cells across, so it steps two advances.
    const SizedWindowCanvas canvas = sizedWindowCanvas(geometry.slotWidthInPixels, geometry.slotHeightInPixels,
        geometry.glyphMarginInPixels.width, geometry.glyphMarginInPixels.height, m_sizedBandLineHeightInPixels,
        m_sizedWindowAdvanceInPixels, isWide ? 2 : 1, tile.step, static_cast<float>(geometry.glyphWidthInPixels));
    const std::size_t windowBytes = static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height);
    if (m_slotScratch.size() < windowBytes)
        m_slotScratch.resize(windowBytes);
    const std::span<std::uint8_t> window = std::span(m_slotScratch).first(windowBytes);
    if (!rasteriseSizedWindow(m_rasterizer, codePoint, face, weight, tile, phase, canvas, m_sizedScratch, window))
        std::ranges::fill(window, std::uint8_t { 0 });

    const int x0 = (slot % geometry.columns) * canvas.width;
    const int y0 = (slot / geometry.columns) * canvas.height;
    for (int y = 0; y < canvas.height; ++y) {
        const std::span<const std::uint8_t> sourceRow = window.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width), static_cast<std::size_t>(canvas.width));
        std::ranges::copy(sourceRow, atlas.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(atlas.atlasWidth) + static_cast<std::size_t>(x0)));
    }
}

AtlasGlyph SoftwareRenderer::faultSizedWindow(Atlas& atlas, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile)
{
    // Declined for a face that would only magnify: the builder's crop draws the
    // same picture and spends no slot doing it.
    if (!m_rasterizer.rasterisesScaledGlyphsSharply())
        return {};
    // A doubled fraction keeps the builder's crop; every other tile past the
    // cell's own glyph is cut from its master.
    if (codePoint == 0 || codePoint == ' ' || !isSizedMasterTile(tile))
        return {};

    const std::uint32_t scaleLane = tile.scaleLane();
    const std::uint32_t key = sizedTileKey(codePoint, weight, face, phase, scaleLane, tile.band, tile.tile);
    // The key is a hash over these two words, so the probe compares them: a
    // collision misses instead of drawing another glyph's window.
    const std::array<std::uint32_t, 2> contents = { codePoint, sizedTileWord(weight, face, phase, scaleLane, tile.band, tile.tile) };
    bool isFresh = false;
    const int slot = atlas.cache.slotForKeyVerified(key, contents.data(), static_cast<int>(contents.size()), &isFresh);
    if (slot < 0)
        return {}; // atlas full this frame -> the crop draws the cell
    if (isFresh)
        rasteriseSizedWindowInto(atlas, slot, codePoint, face, weight, phase, tile);
    const GlyphUV uv = atlas.geometry.atlasRectForSlot(slot, atlas.capacity);
    AtlasGlyph out;
    out.minUV = { uv.minU, uv.minV };
    out.maxUV = { uv.maxU, uv.maxV };
    out.present = true;
    return out;
}

AtlasGlyph SoftwareRenderer::sizedGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile)
{
    return faultSizedWindow(m_mono, codePoint, face, weight, phase, tile);
}

// Weight and phase 0, as ``wideGlyph`` keys them: this rasterizer applies no
// variable weight and a wide cell has no phase.
AtlasGlyph SoftwareRenderer::sizedWideGlyph(std::uint32_t codePoint, FontFace face, SizedTile tile)
{
    return faultSizedWindow(m_wide, codePoint, face, 0u, 0u, tile);
}

AtlasGlyph SoftwareRenderer::monoGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase) { return faultGlyph(m_mono, monoGlyphKey(codePoint, weight, face, phase), codePoint, face); }

// The builder probes this cache itself, so what crosses is the cache and its
// layout rather than a call per cell. This rasterizer applies no variable
// weight, so the binding reports 0 and key and raster stay in agreement.
MonoAtlasBinding SoftwareRenderer::bindMonoAtlas()
{
    MonoAtlasBinding binding;
    binding.cache = &m_mono.cache;
    binding.geometry = m_mono.geometry;
    binding.slotCapacity = m_mono.capacity;
    binding.weight = 0u;
    return binding;
}

bool SoftwareRenderer::rasteriseMonoGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t, int slot)
{
    rasterizeInto(m_mono, slot, codePoint, face);
    return true;
}

AtlasGlyph SoftwareRenderer::clusterGlyph(const std::uint32_t* codePoints, int count, FontFace face, std::uint32_t weight, std::uint32_t phase)
{
    const std::span<const std::uint32_t> cluster = count > 0 ? std::span(codePoints, static_cast<std::size_t>(count)) : std::span<const std::uint32_t> {};
    return faultCluster(m_mono, coverageClusterKey(codePoints, count, phase, face), cluster, face, weight);
}

// Weight 0: this CPU rasterizer applies no variable weight, so keying a single
// weight keeps key and raster in agreement (mirrors VulkanGlyphAtlas).
AtlasGlyph SoftwareRenderer::wideGlyph(std::uint32_t codePoint, FontFace face) { return faultGlyph(m_wide, wideGlyphKey(codePoint, 0u, face), codePoint, face); }

// Phase 0, because a wide cell is never a subpixel one — there is no phase to
// key. The wide page has its own keyspace, so the mono cluster derivation keys
// it without collision across the two.
AtlasGlyph SoftwareRenderer::wideClusterGlyph(std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight)
{
    return faultCluster(m_wide, coverageClusterKey(codePoints.data(), static_cast<int>(codePoints.size()), 0, face), codePoints, face, weight);
}

GlyphBackend SoftwareRenderer::glyphBackend()
{
    // Captureless lambdas, so each entry is a function pointer rather than a
    // std::function holding one — nothing here allocates and nothing is
    // type-erased twice.
    GlyphBackend backend;
    backend.context = this;
    // The primary face's pitch is the rasterizer's verdict, and the letter
    // layout's shaper is the rasterizer's too: a proportional face whose
    // rasterizer declines keeps the grid.
    backend.primaryFacePitch = m_rasterizer.primaryFacePitch();
    backend.shapePrimaryRun = [](void* context, ClusterSpan codePoints, FontFace face, std::uint32_t weight,
                                  ProportionalShapedGlyph* out, int capacity) {
        if (!out || capacity <= 0)
            return 0;
        return static_cast<SoftwareRenderer*>(context)->m_rasterizer.shapePrimaryRun(codePoints, face, weight,
            std::span<ProportionalShapedGlyph>(out, static_cast<std::size_t>(capacity)));
    };
    backend.beginFrame = [](void* context) { static_cast<SoftwareRenderer*>(context)->beginFrame(); };
    backend.bindMonoAtlas = [](void* context) { return static_cast<SoftwareRenderer*>(context)->bindMonoAtlas(); };
    backend.rasteriseMonoGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t variant, int slot) {
        return static_cast<SoftwareRenderer*>(context)->rasteriseMonoGlyph(codePoint, face, variant, slot);
    };
    backend.monoGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase) {
        return static_cast<SoftwareRenderer*>(context)->monoGlyph(codePoint, face, weight, phase);
    };
    backend.wideGlyph = [](void* context, std::uint32_t codePoint, FontFace face) {
        return static_cast<SoftwareRenderer*>(context)->wideGlyph(codePoint, face);
    };
    backend.wideClusterGlyph = [](void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::uint32_t) {
        return static_cast<SoftwareRenderer*>(context)->wideClusterGlyph(codePoints, face, weight);
    };
    backend.sizedGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, GlyphPass, SizedTile tile) {
        return static_cast<SoftwareRenderer*>(context)->sizedGlyph(codePoint, face, weight, phase, tile);
    };
    backend.sizedWideGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t, SizedTile tile) {
        return static_cast<SoftwareRenderer*>(context)->sizedWideGlyph(codePoint, face, tile);
    };
    backend.doubleGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t, DoubleRegion region) {
        return static_cast<SoftwareRenderer*>(context)->doubleGlyph(codePoint, face, region);
    };
    backend.clusterGlyph = [](void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::uint32_t phase) {
        return static_cast<SoftwareRenderer*>(context)->clusterGlyph(codePoints.data(), static_cast<int>(codePoints.size()), face, weight, phase);
    };
    backend.stageMonoCoverage = [](void* context, int slot, std::span<const std::uint8_t> coverage) {
        return static_cast<SoftwareRenderer*>(context)->stageMonoCoverage(slot, coverage);
    };
    // Box LINES keep Core's font-first policy: the face's own strokes carry
    // its weight, dashes and junction artwork, and the box sheet draws only
    // what the face lacks plus the classes prefersProcedural claims. Answered
    // by the ordinary mono fetch — the same route these cells took before the
    // box sheet existed here — so the face's box repertoire renders as the
    // face drew it, and a refusal falls to the sheet's geometry.
    backend.boxGlyph = [](void* context, std::uint32_t codePoint, FontFace face) {
        return static_cast<SoftwareRenderer*>(context)->monoGlyph(codePoint, face, 0, 0);
    };
    // A run the shaper drew is taken off the mono path, so a hook with no strip
    // to answer with draws nothing at all rather than drawing it unligatured.
    // Both conditions for that reason: a rasterizer that shapes, and a sheet whose
    // slots fit. Absent either, the named refusal stands and rows render
    // unligatured, which is the picture a font without the feature gives.
    if (m_hasLigatureShaping && m_ligature.isUsable()) {
        backend.shapeLigatureRuns = [](void* context, LigatureRowSpan row, float /*rowOriginXInPixels*/,
                                        float cellWidthInPixels, GlyphPass, LigatureRunStream runs) {
            // The row origin is not passed on: the sheet reports strips in its
            // own atlas pixels, so where the row lands on screen is the builder's
            // arithmetic and never the shaper's.
            static_cast<SoftwareRenderer*>(context)->m_ligature.shapeRow(row, cellWidthInPixels, runs);
        };
    } else {
        backend.shapeLigatureRuns = declineLigatureShaping;
    }
    // The proportional strip over the same sheet, when its rasterizer has a
    // proportional face; the builder draws the grid-face fallback otherwise.
    // Published under the same two conditions as the shaper, for the same
    // reason: the hook moves the run off the fallback, and one answering
    // absent draws nothing.
    if (m_hasStripSheet && m_proportionalStrip.isUsable() && m_proportionalStrip.hasProportionalFace()) {
        backend.proportionalStrip = [](void* context, ClusterSpan codePoints, int widthCells, std::uint8_t horizontalAlign,
                                        float penResidualInPixels, FontFace face, std::uint32_t /*weight*/, std::uint32_t variant, int tileIndex, SizedTile size) {
            SoftwareRenderer* renderer = static_cast<SoftwareRenderer*>(context);
            return renderer->m_proportionalStrip.proportionalStrip(codePoints, widthCells, horizontalAlign, penResidualInPixels,
                static_cast<float>(renderer->m_cellWidth), face, variant, tileIndex, size);
        };
    }
    if (m_proportional.isUsable()) {
        backend.bindProportionalAtlas = [](void* context) {
            return static_cast<SoftwareRenderer*>(context)->bindProportionalAtlas();
        };
        backend.shapeProportionalRun = [](void* context, ClusterSpan codePoints,
                                           FontFace face, std::uint32_t,
                                           ProportionalShapedGlyph* out, int capacity) {
            if (!out || capacity <= 0)
                return 0;
            return static_cast<SoftwareRenderer*>(context)->shapeProportionalRun(
                codePoints, face, std::span<ProportionalShapedGlyph>(out, static_cast<std::size_t>(capacity)));
        };
        backend.rasteriseProportionalGlyph = [](void* context,
                                                 std::uint32_t glyphID,
                                                 FontFace face,
                                                 std::uint32_t weight,
                                                 std::uint32_t variant,
                                                 int slot) {
            return static_cast<SoftwareRenderer*>(context)
                ->rasteriseProportionalGlyph(glyphID, face, weight, variant, slot);
        };
        backend.rasteriseSizedProportionalGlyph = [](void* context, std::uint32_t glyphID, FontFace face, std::uint32_t weight,
                                                      std::uint32_t variant, SizedTile tile, int slot) {
            return static_cast<SoftwareRenderer*>(context)->m_proportional.rasteriseSizedGlyph(glyphID, face, weight, variant, tile, slot);
        };
    }
    // The colour hooks only when the rasterizer has a colour source. Filling them
    // regardless would make `optionalDefaultsAreLossy` report false while every
    // emoji still drew as coverage in the foreground colour — a claim of a path
    // that answers absent every time, where the null at least routes the
    // cluster somewhere that draws it.
    if (m_hasColorSource) {
        backend.bindColorAtlas = [](void* context) { return static_cast<SoftwareRenderer*>(context)->bindColorAtlas(); };
        backend.rasteriseColorGlyph = [](void* context, std::span<const std::uint32_t> codePoints, int slot) {
            return static_cast<SoftwareRenderer*>(context)->rasteriseColorGlyph(codePoints.data(), static_cast<int>(codePoints.size()), slot);
        };
        backend.colorGlyph = [](void* context, std::span<const std::uint32_t> codePoints, std::uint16_t clusterID) {
            return static_cast<SoftwareRenderer*>(context)->colorGlyph(codePoints.data(), static_cast<int>(codePoints.size()), clusterID);
        };
        backend.sizedColorGlyph = [](void* context, std::span<const std::uint32_t> codePoints, SizedTile tile) {
            return static_cast<SoftwareRenderer*>(context)->sizedColorGlyph(codePoints, tile);
        };
    }
    return backend;
}

void SoftwareRenderer::compositeProportionalGlyph(const GlyphInstance& g,
    std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const
{
    if (g.atlasBottomRight.x <= g.atlasTopLeft.x)
        return;
    const std::span<const std::uint8_t> sheet = m_proportional.coverage();
    const int sheetWidth = m_proportional.widthInPixels();
    const int sheetHeight = m_proportional.heightInPixels();
    if (sheet.empty() || sheetWidth <= 0 || sheetHeight <= 0)
        return;

    const int originX = static_cast<int>(g.originInPixels.x);
    const int originY = static_cast<int>(g.originInPixels.y);
    const int width = static_cast<int>(g.sizeInPixels.x);
    const int height = static_cast<int>(g.sizeInPixels.y);
    const ClippedQuad clip = clipQuadToSurface(originX, originY, width, height, surface);
    if (clip.isEmpty())
        return;

    const std::array<std::uint8_t, 4> cellGround { channelToByte(g.background.x), channelToByte(g.background.y), channelToByte(g.background.z), channelToByte(g.background.w) };
    const bool isOverSurface = ground == InkGround::Surface;
    const int foregroundRed = channelToByte(g.foreground.x);
    const int foregroundGreen = channelToByte(g.foreground.y);
    const int foregroundBlue = channelToByte(g.foreground.z);
    const int slotMinX = static_cast<int>(g.atlasTopLeft.x + 0.5f);
    const int slotMinY = static_cast<int>(g.atlasTopLeft.y + 0.5f);
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;
    // Texels across per pixel: one for every glyph drawn at its own width, a
    // half for a run doubled with its DECDWL row and drawn from the ordinary
    // raster. Each pixel takes the texel its centre falls in, so a one-to-one
    // quad reads exactly the texel at its own offset.
    const float texelsPerPixel = g.sizeInPixels.x > 0.0f ? (g.atlasBottomRight.x - g.atlasTopLeft.x) / g.sizeInPixels.x : 1.0f;
    // Down, likewise: one texel a pixel, or a half for a run on a DECDHL pair
    // drawn from the ordinary raster, each half cut at the pair's midline.
    const float texelsPerPixelY = g.sizeInPixels.y > 0.0f ? (g.atlasBottomRight.y - g.atlasTopLeft.y) / g.sizeInPixels.y : 1.0f;

    withForegroundCoverage(g.foreground.w, [&](const auto& coverageFor) {
        for (int localY = clip.firstRow; localY < clip.lastRow; ++localY) {
            const int atlasY = clampInt(texelsPerPixelY == 1.0f
                    ? slotMinY + localY
                    : static_cast<int>(std::floor(g.atlasTopLeft.y + (static_cast<float>(localY) + 0.5f) * texelsPerPixelY)),
                0, sheetHeight - 1);
            const std::uint8_t* atlasRow = sheet.data()
                + static_cast<std::size_t>(atlasY) * sheetWidth;
            std::uint8_t* destination = rgba.data()
                + static_cast<std::size_t>(originY + localY) * rowStride
                + static_cast<std::size_t>(originX + clip.firstColumn) * 4;
            for (int localX = clip.firstColumn; localX < clip.lastColumn;
                ++localX, destination += 4) {
                const int atlasX = clampInt(slotMinX + static_cast<int>((static_cast<float>(localX) + 0.5f) * texelsPerPixel), 0, sheetWidth - 1);
                const int coverage = coverageFor(atlasRow[atlasX]);
                if (coverage == 0)
                    continue;
                writeInkPixel(destination, isOverSurface ? destination : cellGround.data(),
                    foregroundRed, foregroundGreen, foregroundBlue, coverage);
            }
        }
    });
}

void SoftwareRenderer::compositeLigatureGlyph(const GlyphInstance& g, const LigatureSheet& stripSheet, std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const
{
    // compositeGlyph's arithmetic over a different sheet. The tap is the same
    // integer texelFetch the shader does, because the two arms are compared byte
    // for byte and a filtered read here would differ from an unfiltered read
    // there by a fraction of a channel on every edge pixel.
    //
    // The builder already reduced the run's strip to this cell's slice — origin,
    // size and both atlas corners are the slice, not the run — so nothing here
    // needs to know which cell of the run it is drawing.
    if (g.atlasBottomRight.x <= g.atlasTopLeft.x)
        return;
    const std::span<const std::uint8_t> sheet = stripSheet.coverage();
    const int sheetWidth = stripSheet.widthInPixels();
    const int sheetHeight = stripSheet.heightInPixels();
    if (sheet.empty() || sheetWidth <= 0 || sheetHeight <= 0)
        return;

    const int originX = static_cast<int>(g.originInPixels.x);
    const int originY = static_cast<int>(g.originInPixels.y);
    const int sliceWidthInPixels = static_cast<int>(g.sizeInPixels.x);
    const int sliceHeightInPixels = static_cast<int>(g.sizeInPixels.y);

    const std::array<std::uint8_t, 4> cellGround { channelToByte(g.background.x), channelToByte(g.background.y), channelToByte(g.background.z), channelToByte(g.background.w) };
    const bool isOverSurface = ground == InkGround::Surface;
    const int foregroundRed = channelToByte(g.foreground.x), foregroundGreen = channelToByte(g.foreground.y), foregroundBlue = channelToByte(g.foreground.z);

    const int sliceMinX = static_cast<int>(g.atlasTopLeft.x + 0.5f);
    const int sliceMinY = static_cast<int>(g.atlasTopLeft.y + 0.5f);
    // One texel a pixel for a slice at its own width; a proportional strip
    // doubled with its DECDWL row spans half a texel a pixel. Each pixel takes
    // the texel its centre falls in, which at one to one is its own offset.
    const float texelsPerPixel = g.sizeInPixels.x > 0.0f ? (g.atlasBottomRight.x - g.atlasTopLeft.x) / g.sizeInPixels.x : 1.0f;
    // Down, likewise: one texel a pixel, or a half for a proportional strip
    // on a DECDHL pair, each half cut at the pair's midline.
    const float texelsPerPixelY = g.sizeInPixels.y > 0.0f ? (g.atlasBottomRight.y - g.atlasTopLeft.y) / g.sizeInPixels.y : 1.0f;

    // Clipped once against the surface, as compositeGlyph's slice loop is.
    const ClippedQuad clip = clipQuadToSurface(originX, originY, sliceWidthInPixels, sliceHeightInPixels, surface);
    if (clip.isEmpty())
        return;
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;

    withForegroundCoverage(g.foreground.w, [&](const auto& coverageFor) {
        for (int ly = clip.firstRow; ly < clip.lastRow; ++ly) {
            const int ty = clampInt(texelsPerPixelY == 1.0f
                    ? sliceMinY + ly
                    : static_cast<int>(std::floor(g.atlasTopLeft.y + (static_cast<float>(ly) + 0.5f) * texelsPerPixelY)),
                0, sheetHeight - 1);
            const std::uint8_t* const sheetRow = sheet.data() + static_cast<std::size_t>(ty) * sheetWidth;
            std::uint8_t* out = rgba.data() + static_cast<std::size_t>(originY + ly) * rowStride
                + static_cast<std::size_t>(originX + clip.firstColumn) * 4;
            for (int lx = clip.firstColumn; lx < clip.lastColumn; ++lx, out += 4) {
                const int tx = clampInt(sliceMinX + static_cast<int>((static_cast<float>(lx) + 0.5f) * texelsPerPixel), 0, sheetWidth - 1);
                const int coverage = coverageFor(sheetRow[tx]);
                if (coverage == 0)
                    continue;
                writeInkPixel(out, isOverSurface ? out : cellGround.data(), foregroundRed, foregroundGreen, foregroundBlue, coverage);
            }
        }
    });
}

void SoftwareRenderer::compositeColorGlyph(const GlyphInstance& g, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    if (g.atlasBottomRight.x <= g.atlasTopLeft.x || !m_hasColorSource)
        return;
    const int originX = static_cast<int>(g.originInPixels.x);
    const int originY = static_cast<int>(g.originInPixels.y);
    const int width = static_cast<int>(g.sizeInPixels.x);
    const int height = static_cast<int>(g.sizeInPixels.y);
    if (width <= 0 || height <= 0)
        return;

    // Pixel-boundary UVs preserve texel centres at native size and scale across
    // the entire source for a backend without sized colour rasters.
    const float uSpan = g.atlasBottomRight.x - g.atlasTopLeft.x;
    const float vSpan = g.atlasBottomRight.y - g.atlasTopLeft.y;
    // Clipped once against the surface rather than per pixel.
    const ClippedQuad clip = clipQuadToSurface(originX, originY, width, height, surface);
    if (clip.isEmpty())
        return;
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;

    // The sheet column depends on x alone, so the rows of the slot share it:
    // resolved once for the span, in the same expression and the same order.
    const auto sheetColumnFor = [&](int lx) {
        const float u = g.atlasTopLeft.x + uSpan * ((static_cast<float>(originX + lx) + 0.5f) - g.originInPixels.x) / g.sizeInPixels.x;
        return clampInt(static_cast<int>(u), 0, m_color.atlasWidth - 1);
    };
    // Tabled once for the span rather than resolved per pixel, which is where
    // the divide goes. A span past the table keeps the per-pixel form: the
    // arithmetic is the same lambda either way, so the two cannot disagree.
    std::array<int, maxSpanTexelColumns> sheetColumns;
    const bool areColumnsTabled = clip.lastColumn <= static_cast<int>(sheetColumns.size());
    if (areColumnsTabled) {
        for (int lx = clip.firstColumn; lx < clip.lastColumn; ++lx)
            sheetColumns[static_cast<std::size_t>(lx)] = sheetColumnFor(lx);
    }

    withForegroundCoverage(g.foreground.w, [&](const auto& coverageFor) {
        for (int ly = clip.firstRow; ly < clip.lastRow; ++ly) {
            const int y = originY + ly;
            // The pixel centre against the quad's true origin, not against its
            // truncated one. A rasteriser evaluates an interpolated coordinate at
            // the centre of each pixel it covers, so a quad whose origin carries a
            // fraction — an emoji's square slot centred in a two-cell span lands on
            // a half — samples half a texel away from `(index + 0.5) / extent`.
            const float v = g.atlasTopLeft.y + vSpan * ((static_cast<float>(y) + 0.5f) - g.originInPixels.y) / g.sizeInPixels.y;
            const int ty = clampInt(static_cast<int>(v), 0, m_color.atlasHeight - 1);
            const std::uint8_t* const sheetRow = m_color.pixels.data() + static_cast<std::size_t>(ty) * m_color.atlasWidth * 4;
            std::uint8_t* out = rgba.data() + static_cast<std::size_t>(y) * rowStride
                + static_cast<std::size_t>(originX + clip.firstColumn) * 4;
            for (int lx = clip.firstColumn; lx < clip.lastColumn; ++lx, out += 4) {
                const int tx = areColumnsTabled ? sheetColumns[static_cast<std::size_t>(lx)] : sheetColumnFor(lx);
                const std::uint8_t* source = sheetRow + static_cast<std::size_t>(tx) * 4;
                const int alpha = coverageFor(source[3]);
                if (alpha == 0)
                    continue;
                const int inverse = 255 - alpha;
                for (int channel = 0; channel < 3; ++channel)
                    out[channel] = static_cast<std::uint8_t>((out[channel] * inverse + source[channel] * alpha + 127) / 255);
                out[3] = static_cast<std::uint8_t>(std::min(255, out[3] + alpha));
            }
        }
    });
}

float SoftwareRenderer::sampleMonoCoverage(float x, float y) const
{
    // The coordinate is the texel index.
    if (m_mono.atlasWidth <= 0 || m_mono.atlasHeight <= 0)
        return 0.0f;
    int ax = static_cast<int>(x);
    int ay = static_cast<int>(y);
    if (ax < 0)
        ax = 0;
    if (ay < 0)
        ay = 0;
    if (ax >= m_mono.atlasWidth)
        ax = m_mono.atlasWidth - 1;
    if (ay >= m_mono.atlasHeight)
        ay = m_mono.atlasHeight - 1;
    return m_mono.pixels[static_cast<std::size_t>(ay) * m_mono.atlasWidth + ax] / 255.0f;
}

float SoftwareRenderer::sampleStripCoverage(float x, float y) const
{
    const std::span<const std::uint8_t> sheet = m_ligature.coverage();
    const int width = m_ligature.widthInPixels();
    const int height = m_ligature.heightInPixels();
    if (width <= 0 || height <= 0 || sheet.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return 0.0f;
    int ax = static_cast<int>(x);
    int ay = static_cast<int>(y);
    if (ax < 0)
        ax = 0;
    if (ay < 0)
        ay = 0;
    if (ax >= width)
        ax = width - 1;
    if (ay >= height)
        ay = height - 1;
    return sheet[static_cast<std::size_t>(ay) * static_cast<std::size_t>(width) + static_cast<std::size_t>(ax)] / 255.0f;
}

float SoftwareRenderer::sampleProportionalStripCoverage(float x, float y) const
{
    const std::span<const std::uint8_t> sheet = m_proportionalStrip.coverage();
    const int width = m_proportionalStrip.widthInPixels();
    const int height = m_proportionalStrip.heightInPixels();
    if (width <= 0 || height <= 0 || sheet.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return 0.0f;
    int atlasX = std::clamp(static_cast<int>(x), 0, width - 1);
    int atlasY = std::clamp(static_cast<int>(y), 0, height - 1);
    return sheet[static_cast<std::size_t>(atlasY) * static_cast<std::size_t>(width) + static_cast<std::size_t>(atlasX)] / 255.0f;
}

float SoftwareRenderer::sampleProportionalCoverage(float x, float y) const
{
    const std::span<const std::uint8_t> sheet = m_proportional.coverage();
    const int width = m_proportional.widthInPixels();
    const int height = m_proportional.heightInPixels();
    if (width <= 0 || height <= 0 || sheet.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return 0.0f;
    const int atlasX = std::clamp(static_cast<int>(x), 0, width - 1);
    const int atlasY = std::clamp(static_cast<int>(y), 0, height - 1);
    return sheet[static_cast<std::size_t>(atlasY) * static_cast<std::size_t>(width) + static_cast<std::size_t>(atlasX)] / 255.0f;
}

void SoftwareRenderer::composite(const CellInstance& instance, IntPoint gridOriginInPixels, std::vector<std::uint8_t>& rgba, IntSize surface, int rowScale, CellPaint paint) const
{
    // `widthCells` is the builder's own measure of the span — (wide ? 2 : 1)
    // times the row's xStep — so it is 2 for a wide cell, 2 for a narrow cell
    // on a doubled row, 4 for a wide one there, a fraction where a doubled
    // cell straddles the right edge, and a letter's advance on a row laid out
    // as letters (LetterLayout.h), where a cell stands at its letter's pen.
    // Painted linearly: reading it as a narrow-or-wide flag paints two cells
    // for every one of those and leaves the rest of a doubled cell holding
    // whatever the surface had. The span runs from the rounded left edge to
    // the rounded right edge rather than a rounded width from a rounded
    // left: two letters meeting on a fractional boundary then share the pixel
    // column between them, where rounding each on its own leaves a column
    // nothing paints or paints one twice.
    const int cellLeft = static_cast<int>(std::lround(instance.gridPosition.x * static_cast<float>(m_cellWidth)));
    const int cellRight = static_cast<int>(std::lround((instance.gridPosition.x + instance.widthCells) * static_cast<float>(m_cellWidth)));
    const int spanWidth = cellRight - cellLeft;
    // Which SHEET the cell's glyph came from, which the span cannot answer on
    // its own: a wide cell and a narrow cell on a doubled row both span two.
    // The row's scale divides out, leaving the cell's own width. A doubled
    // row's narrow cell is two one-cell windows of its doubled master, or the
    // 1x glyph cropped where those were declined; both are mono-sheet rects.
    const int rowWidthScale = rowScale > 0 ? rowScale : 1;
    const bool wide = instance.widthCells > 1.5f * static_cast<float>(rowWidthScale);
    const Atlas& atlas = wide ? m_wide : m_mono;
    // The origin arrives already rounded and is added in the integer domain, so
    // an inset of zero cannot move a pixel: this whole pass stays the arithmetic
    // it was. See render() for why the CPU path takes whole pixels at all.
    // The origin is whole, as every grid origin is, and added in the integer
    // domain.
    const int x0 = gridOriginInPixels.x + cellLeft;
    const int y0 = gridOriginInPixels.y + static_cast<int>(instance.gridPosition.y) * m_cellHeight;

    // Colors as exact 0..255 bytes — the palette is 8-bit, so the instance's
    // 0..1 floats are N/255 and round-trip cleanly. The mix runs in this
    // integer domain (below) so the CPU and GPU agree bit-for-bit.
    const int backgroundRed = channelToByte(instance.background.x), backgroundGreen = channelToByte(instance.background.y), backgroundBlue = channelToByte(instance.background.z), backgroundAlpha = channelToByte(instance.background.w);
    const int foregroundRed = channelToByte(instance.foreground.x), foregroundGreen = channelToByte(instance.foreground.y), foregroundBlue = channelToByte(instance.foreground.z);
    const bool hasGlyph = instance.atlasBottomRight.x > instance.atlasTopLeft.x;
    if (paint == CellPaint::Ink && !hasGlyph)
        return;
    const float spanU = instance.atlasBottomRight.x - instance.atlasTopLeft.x;
    const float spanV = instance.atlasBottomRight.y - instance.atlasTopLeft.y;

    // Clipped once against the surface rather than per pixel: the bounds do not
    // change inside the loops they guard, and this is the one loop in the
    // backend that runs per pixel of the frame rather than per cell of it.
    const ClippedQuad clip = clipQuadToSurface(x0, y0, spanWidth, m_cellHeight, surface);
    if (clip.isEmpty())
        return;

    // INTEGER mix(background, foreground, cov): out =
    // round((background*(255-cov)+foreground*cov)/255). The float mix ->
    // UNORM8 store diverges by a LSB between the CPU and lavapipe once
    // coverage is anti-aliased. The GPU cell shader runs this same integer
    // formula and emits byte/255 (which lavapipe stores exactly), so both
    // paths land on the identical byte — the CPU<->GPU oracle. For 0/255
    // coverage it reduces to background / foreground.
    std::uint8_t* const surfaceBase = rgba.data();
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;

    if (!hasGlyph || paint == CellPaint::Ground) {
        // Coverage is zero over the whole cell, and the mix at zero coverage is
        // (background*255 + 127)/255 — the background byte itself, since the
        // remainder cannot reach 255. So the cell is one four-byte pattern
        // repeated, and the four channel mixes per pixel compute that pattern
        // again for every pixel of every blank cell on the screen. The ground
        // of an inked cell is the same pattern, its ink left for a later pass.
        std::array<std::uint8_t, 4> pattern {};
        writeMixedPixel(pattern.data(), backgroundRed, backgroundGreen, backgroundBlue, backgroundAlpha,
            foregroundRed, foregroundGreen, foregroundBlue, 0);
        for (int py = clip.firstRow; py < clip.lastRow; ++py) {
            std::uint8_t* out = surfaceBase + static_cast<std::size_t>(y0 + py) * rowStride
                + static_cast<std::size_t>(x0 + clip.firstColumn) * 4;
            for (int px = clip.firstColumn; px < clip.lastColumn; ++px, out += 4)
                std::memcpy(out, pattern.data(), pattern.size());
        }
        return;
    }

    // The atlas column depends on px alone, so every row of the cell taps the
    // same columns: resolved once for the span instead of once per pixel, which
    // is where the divide in it goes. A span wider than the buffer keeps the
    // per-pixel form — the arithmetic is the lambda either way, so the two
    // schedules cannot answer differently.
    const auto texelColumnFor = [&](int px) {
        const float u = instance.atlasTopLeft.x + (px + 0.5f) / spanWidth * spanU;
        // Atlas pixels: the interpolated coordinate is the texel.
        return clampInt(static_cast<int>(u), 0, atlas.atlasWidth - 1);
    };
    // Tabled once for the span, as the colour sheet's tap above is.
    std::array<int, maxSpanTexelColumns> texelColumns;
    const bool areColumnsTabled = clip.lastColumn <= static_cast<int>(texelColumns.size());
    if (areColumnsTabled) {
        for (int px = clip.firstColumn; px < clip.lastColumn; ++px)
            texelColumns[static_cast<std::size_t>(px)] = texelColumnFor(px);
    }

    // One walk of the cell's pixels and their coverage, written two ways: the
    // whole cell mixes every pixel from its own ground, and the ink alone mixes
    // only the pixels the glyph covers, over whatever the surface holds there.
    const auto walkCoverage = [&](const auto& writePixel) {
        for (int py = clip.firstRow; py < clip.lastRow; ++py) {
            const float v = instance.atlasTopLeft.y + (py + 0.5f) / m_cellHeight * spanV;
            const int ay = clampInt(static_cast<int>(v), 0, atlas.atlasHeight - 1);
            const std::uint8_t* const atlasRow = atlas.pixels.data() + static_cast<std::size_t>(ay) * atlas.atlasWidth;
            std::uint8_t* out = surfaceBase + static_cast<std::size_t>(y0 + py) * rowStride
                + static_cast<std::size_t>(x0 + clip.firstColumn) * 4;
            for (int px = clip.firstColumn; px < clip.lastColumn; ++px, out += 4)
                writePixel(out, atlasRow[areColumnsTabled ? texelColumns[static_cast<std::size_t>(px)] : texelColumnFor(px)]);
        }
    };
    if (paint == CellPaint::Ink) {
        walkCoverage([&](std::uint8_t* out, int coverage) {
            if (coverage != 0)
                writeInkPixel(out, out, foregroundRed, foregroundGreen, foregroundBlue, coverage);
        });
        return;
    }
    walkCoverage([&](std::uint8_t* out, int coverage) {
        writeMixedPixel(out, backgroundRed, backgroundGreen, backgroundBlue, backgroundAlpha,
            foregroundRed, foregroundGreen, foregroundBlue, coverage);
    });
}

// Keep the profiled opaque compositor intact. Blink and conceal take the
// opacity walk only when their resolved foreground needs it.
void SoftwareRenderer::compositeWithCoverage(const CellInstance& instance, IntPoint gridOriginInPixels, std::vector<std::uint8_t>& rgba, IntSize surface, int rowScale, CellPaint paint) const
{
    if (std::abs(instance.foreground.w) >= 1.0f)
        composite(instance, gridOriginInPixels, rgba, surface, rowScale, paint);
    else
        compositeWithOpacity(instance, gridOriginInPixels, rgba, surface, rowScale, paint);
}

void SoftwareRenderer::compositeWithOpacity(const CellInstance& instance, IntPoint gridOriginInPixels, std::vector<std::uint8_t>& rgba, IntSize surface, int rowScale, CellPaint paint) const
{
    const int cellLeft = static_cast<int>(std::lround(instance.gridPosition.x * static_cast<float>(m_cellWidth)));
    const int cellRight = static_cast<int>(std::lround((instance.gridPosition.x + instance.widthCells) * static_cast<float>(m_cellWidth)));
    const int spanWidth = cellRight - cellLeft;
    // Which SHEET the cell's glyph came from, which the span cannot answer on
    // its own: a wide cell and a narrow cell on a doubled row both span two.
    // The row's scale divides out, leaving the cell's own width. A doubled
    // row's narrow cell is two one-cell windows of its doubled master, or the
    // 1x glyph cropped where those were declined; both are mono-sheet rects.
    const int rowWidthScale = rowScale > 0 ? rowScale : 1;
    const bool isWide = instance.widthCells > 1.5f * static_cast<float>(rowWidthScale);
    const Atlas& atlas = isWide ? m_wide : m_mono;
    // The origin arrives already rounded and is added in the integer domain, so
    // an inset of zero cannot move a pixel: this whole pass stays the arithmetic
    // it was. See render() for why the CPU path takes whole pixels at all.
    // The origin is whole, as every grid origin is, and added in the integer
    // domain.
    const int x0 = gridOriginInPixels.x + cellLeft;
    const int y0 = gridOriginInPixels.y + static_cast<int>(instance.gridPosition.y) * m_cellHeight;

    // Colors as exact 0..255 bytes — the palette is 8-bit, so the instance's
    // 0..1 floats are N/255 and round-trip cleanly. The mix runs in this
    // integer domain (below) so the CPU and GPU agree bit-for-bit.
    const int backgroundRed = channelToByte(instance.background.x), backgroundGreen = channelToByte(instance.background.y), backgroundBlue = channelToByte(instance.background.z), backgroundAlpha = channelToByte(instance.background.w);
    const int foregroundRed = channelToByte(instance.foreground.x), foregroundGreen = channelToByte(instance.foreground.y), foregroundBlue = channelToByte(instance.foreground.z);
    const bool hasGlyph = instance.atlasBottomRight.x > instance.atlasTopLeft.x;
    if (paint == CellPaint::Ink && !hasGlyph)
        return;
    const float spanU = instance.atlasBottomRight.x - instance.atlasTopLeft.x;
    const float spanV = instance.atlasBottomRight.y - instance.atlasTopLeft.y;

    // Clipped once against the surface rather than per pixel: the bounds do not
    // change inside the loops they guard, and this is the one loop in the
    // backend that runs per pixel of the frame rather than per cell of it.
    const ClippedQuad clip = clipQuadToSurface(x0, y0, spanWidth, m_cellHeight, surface);
    if (clip.isEmpty())
        return;

    // INTEGER mix(background, foreground, cov): out =
    // round((background*(255-cov)+foreground*cov)/255). The float mix ->
    // UNORM8 store diverges by a LSB between the CPU and lavapipe once
    // coverage is anti-aliased. The GPU cell shader runs this same integer
    // formula and emits byte/255 (which lavapipe stores exactly), so both
    // paths land on the identical byte — the CPU<->GPU oracle. For 0/255
    // coverage it reduces to background / foreground.
    std::uint8_t* const surfaceBase = rgba.data();
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;

    if (!hasGlyph || paint == CellPaint::Ground) {
        // Coverage is zero over the whole cell, and the mix at zero coverage is
        // (background*255 + 127)/255 — the background byte itself, since the
        // remainder cannot reach 255. So the cell is one four-byte pattern
        // repeated, and the four channel mixes per pixel compute that pattern
        // again for every pixel of every blank cell on the screen. The ground
        // of an inked cell is the same pattern, its ink left for a later pass.
        std::array<std::uint8_t, 4> pattern {};
        writeMixedPixel(pattern.data(), backgroundRed, backgroundGreen, backgroundBlue, backgroundAlpha,
            foregroundRed, foregroundGreen, foregroundBlue, 0);
        for (int py = clip.firstRow; py < clip.lastRow; ++py) {
            std::uint8_t* out = surfaceBase + static_cast<std::size_t>(y0 + py) * rowStride
                + static_cast<std::size_t>(x0 + clip.firstColumn) * 4;
            for (int px = clip.firstColumn; px < clip.lastColumn; ++px, out += 4)
                std::memcpy(out, pattern.data(), pattern.size());
        }
        return;
    }

    // The atlas column depends on px alone, so every row of the cell taps the
    // same columns: resolved once for the span instead of once per pixel, which
    // is where the divide in it goes. A span wider than the buffer keeps the
    // per-pixel form — the arithmetic is the lambda either way, so the two
    // schedules cannot answer differently.
    const auto texelColumnFor = [&](int px) {
        const float u = instance.atlasTopLeft.x + (px + 0.5f) / spanWidth * spanU;
        // Atlas pixels: the interpolated coordinate is the texel.
        return clampInt(static_cast<int>(u), 0, atlas.atlasWidth - 1);
    };
    // Tabled once for the span, as the colour sheet's tap above is.
    std::array<int, maxSpanTexelColumns> texelColumns;
    const bool areColumnsTabled = clip.lastColumn <= static_cast<int>(texelColumns.size());
    if (areColumnsTabled) {
        for (int px = clip.firstColumn; px < clip.lastColumn; ++px)
            texelColumns[static_cast<std::size_t>(px)] = texelColumnFor(px);
    }

    // One walk of the cell's pixels and their coverage, written two ways: the
    // whole cell mixes every pixel from its own ground, and the ink alone mixes
    // only the pixels the glyph covers, over whatever the surface holds there.
    const auto walkCoverage = [&](const auto& writePixel) {
        for (int py = clip.firstRow; py < clip.lastRow; ++py) {
            const float v = instance.atlasTopLeft.y + (py + 0.5f) / m_cellHeight * spanV;
            const int ay = clampInt(static_cast<int>(v), 0, atlas.atlasHeight - 1);
            const std::uint8_t* const atlasRow = atlas.pixels.data() + static_cast<std::size_t>(ay) * atlas.atlasWidth;
            std::uint8_t* out = surfaceBase + static_cast<std::size_t>(y0 + py) * rowStride
                + static_cast<std::size_t>(x0 + clip.firstColumn) * 4;
            for (int px = clip.firstColumn; px < clip.lastColumn; ++px, out += 4)
                writePixel(out, atlasRow[areColumnsTabled ? texelColumns[static_cast<std::size_t>(px)] : texelColumnFor(px)]);
        }
    };
    withForegroundCoverage(instance.foreground.w, [&](const auto& coverageFor) {
        if (paint == CellPaint::Ink) {
            walkCoverage([&](std::uint8_t* out, int coverage) {
                coverage = coverageFor(coverage);
                if (coverage != 0)
                    writeInkPixel(out, out, foregroundRed, foregroundGreen, foregroundBlue, coverage);
            });
            return;
        }
        walkCoverage([&](std::uint8_t* out, int coverage) {
            coverage = coverageFor(coverage);
            writeMixedPixel(out, backgroundRed, backgroundGreen, backgroundBlue, backgroundAlpha,
                foregroundRed, foregroundGreen, foregroundBlue, coverage);
        });
    });
}

// Subpixel overflow glyph: the quad is the FULL slot (sizeInPixels), placed at
// originInPixels (penX - margin, rowTop) so a glyph wider than the cell box
// overflows symmetrically into the neighbour columns. Coverage is sampled by
// the same integer texel tap the cell shader uses, then the foreground is mixed
// over the glyph's CELL background (not the framebuffer) in the integer domain;
// coverage 0 is SKIPPED so the slot margins + empty area leave the painted cell
// backgrounds untouched. The GPU glyph shader runs the identical mix + discard.
// A frame with a picture under its text mixes over the surface instead
// (`InkGround::Surface`).
void SoftwareRenderer::compositeGlyph(const GlyphInstance& g, const Atlas& atlas, std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const
{
    if (g.atlasBottomRight.x <= g.atlasTopLeft.x || atlas.pixels.empty())
        return;
    const int originX = static_cast<int>(g.originInPixels.x);
    const int originY = static_cast<int>(g.originInPixels.y);
    const int slotWidthInPixels = static_cast<int>(g.sizeInPixels.x);
    const int slotHeightInPixels = static_cast<int>(g.sizeInPixels.y);

    const std::array<std::uint8_t, 4> cellGround { channelToByte(g.background.x), channelToByte(g.background.y), channelToByte(g.background.z), channelToByte(g.background.w) };
    const bool isOverSurface = ground == InkGround::Surface;
    const int foregroundRed = channelToByte(g.foreground.x), foregroundGreen = channelToByte(g.foreground.y), foregroundBlue = channelToByte(g.foreground.z);

    // The slot's atlas texel origin (atlasTopLeft/atlasBottomRight are the
    // full-slot rect the builder expanded to). slotMin + the in-slot pixel
    // index is the exact integer tap, matching the cell shader's texelFetch.
    const int slotMinX = static_cast<int>(g.atlasTopLeft.x + 0.5f);
    const int slotMinY = static_cast<int>(g.atlasTopLeft.y + 0.5f);

    // Clipped once, the way compositeDecoration already clips: the surface does
    // not move inside the slot's loops.
    const ClippedQuad clip = clipQuadToSurface(originX, originY, slotWidthInPixels, slotHeightInPixels, surface);
    if (clip.isEmpty())
        return;
    const std::size_t rowStride = static_cast<std::size_t>(surface.width) * 4;

    // Does the quad cover the rect it samples one for one?
    //
    // Nearly always. A quad that does not is a scripted glyph, which the
    // builder shrinks about the cell's centre — and there the walk below reads
    // the wrong texel for every pixel but the first: stepping one texel per
    // pixel across a quad 0.7 as wide draws the top-left crop of the letter at
    // full size rather than the letter at 0.7. That is what the reference drew
    // for every super- and subscript, against GPU arms that sample the same
    // slot through a linear sampler and get the letter.
    const float sourceWidth = g.atlasBottomRight.x - g.atlasTopLeft.x;
    const float sourceHeight = g.atlasBottomRight.y - g.atlasTopLeft.y;
    const bool isOneToOne = std::fabs(sourceWidth - g.sizeInPixels.x) < 0.001f
        && std::fabs(sourceHeight - g.sizeInPixels.y) < 0.001f;

    withForegroundCoverage(g.foreground.w, [&](const auto& coverageFor) {
        if (isOneToOne) {
            for (int ly = clip.firstRow; ly < clip.lastRow; ++ly) {
                const int ty = clampInt(slotMinY + ly, 0, atlas.atlasHeight - 1);
                const std::uint8_t* const atlasRow = atlas.pixels.data() + static_cast<std::size_t>(ty) * atlas.atlasWidth;
                std::uint8_t* out = rgba.data() + static_cast<std::size_t>(originY + ly) * rowStride
                    + static_cast<std::size_t>(originX + clip.firstColumn) * 4;
                for (int lx = clip.firstColumn; lx < clip.lastColumn; ++lx, out += 4) {
                    const int tx = clampInt(slotMinX + lx, 0, atlas.atlasWidth - 1);
                    const int coverage = coverageFor(atlasRow[tx]);
                    if (coverage == 0)
                        continue;
                    writeInkPixel(out, isOverSurface ? out : cellGround.data(), foregroundRed, foregroundGreen, foregroundBlue, coverage);
                }
            }
            return;
        }

        // The scaled case, which the Vulkan glyph pass (linear mode)
        // computes the same way: the pixel centre maps into the source rect, the
        // four texels around it are weighted by the fractional part, and a tap
        // outside the instance's box, or outside the sheet, reads zero.
        //
        // Written apart from the walk above rather than folded into it: at a ratio
        // of one this arithmetic returns the same texel — a linear tap at a texel
        // centre is that texel — but it costs four loads and three lerps to say so,
        // on the pass that draws every glyph on screen.
        // The quad's own edges, not the truncated ones the walk above uses. A
        // scripted quad is placed at a fraction of a pixel and is a fraction of a
        // pixel wide, and the GPU rasterises exactly that: a pixel is covered when
        // its CENTRE falls inside the quad. Truncating the origin first shifts the
        // whole letter and shears the mapping under it, which is a picture no arm
        // draws.
        const float quadLeft = g.originInPixels.x;
        const float quadTop = g.originInPixels.y;
        const float quadWidth = g.sizeInPixels.x;
        const float quadHeight = g.sizeInPixels.y;
        if (quadWidth <= 0.0f || quadHeight <= 0.0f)
            return;
        int firstColumn = static_cast<int>(std::floor(quadLeft));
        int lastColumn = static_cast<int>(std::ceil(quadLeft + quadWidth));
        int firstRow = static_cast<int>(std::floor(quadTop));
        int lastRow = static_cast<int>(std::ceil(quadTop + quadHeight));
        firstColumn = clampInt(firstColumn, 0, surface.width);
        lastColumn = clampInt(lastColumn, 0, surface.width);
        firstRow = clampInt(firstRow, 0, surface.height);
        lastRow = clampInt(lastRow, 0, surface.height);

        // The instance's own box in the sheet. A linear tap half a texel past the
        // rect it maps would read whatever the neighbouring slot holds there —
        // nothing guarantees a margin between slots — so a tap outside the box
        // takes the box's nearest texel, as a sampler clamped to the box does, and
        // a tap outside the sheet reads zero. The picture is then the slot's own
        // pixels whatever was allocated around it. Clamped rather than zero
        // because a rect may be cut from inside a raster: a soft sized band's edge
        // falls between two rows of the 1x glyph, and a zero tap there blends the
        // band's last row with nothing, a lighter line across the glyph at every
        // band join.
        const int boxLeft = static_cast<int>(std::floor(g.atlasTopLeft.x));
        const int boxTop = static_cast<int>(std::floor(g.atlasTopLeft.y));
        const int boxRight = static_cast<int>(std::ceil(g.atlasBottomRight.x)) - 1;
        const int boxBottom = static_cast<int>(std::ceil(g.atlasBottomRight.y)) - 1;
        const auto tap = [&](int x, int y) -> float {
            x = clampInt(x, boxLeft, boxRight);
            y = clampInt(y, boxTop, boxBottom);
            if (x < 0 || y < 0 || x >= atlas.atlasWidth || y >= atlas.atlasHeight)
                return 0.0f;
            return static_cast<float>(atlas.pixels[static_cast<std::size_t>(y) * atlas.atlasWidth + static_cast<std::size_t>(x)]);
        };
        const auto tapCoverage = [&](int x, int y) { return static_cast<int>(tap(x, y)); };
        const auto toFixed64 = [](float value) { return static_cast<int>(std::floor(value * 64.0f + 0.5f)); };
        struct FixedQuad {
            int originX64, originY64, sizeX64, sizeY64, atlasX64, atlasY64, sourceWidth64, sourceHeight64;
        };
        const FixedQuad quad { toFixed64(quadLeft), toFixed64(quadTop), toFixed64(quadWidth), toFixed64(quadHeight),
            toFixed64(g.atlasTopLeft.x), toFixed64(g.atlasTopLeft.y), toFixed64(g.atlasBottomRight.x - g.atlasTopLeft.x),
            toFixed64(g.atlasBottomRight.y - g.atlasTopLeft.y) };
        if (quad.sizeX64 <= 0 || quad.sizeY64 <= 0)
            return;
        // The bitmap tier takes the GPU glyph pass's nearest tap instead: the texel
        // the pixel centre lands in, held inside the instance's box. A bitmap face
        // has no intermediate coverage for a linear tap to find, so blending its
        // texels draws a blurred letter no GPU arm draws.
        for (int y = firstRow; y < lastRow; ++y) {
            if (m_isBitmapFace) {
                const float centreY = static_cast<float>(y) + 0.5f;
                if (centreY < quadTop || centreY >= quadTop + quadHeight)
                    continue;
                const float texelY = g.atlasTopLeft.y + (centreY - quadTop) / quadHeight * sourceHeight;
                const int ty = clampInt(static_cast<int>(std::floor(texelY)), boxTop, boxBottom);
                std::uint8_t* out = rgba.data() + static_cast<std::size_t>(y) * rowStride
                    + static_cast<std::size_t>(firstColumn) * 4;
                for (int x = firstColumn; x < lastColumn; ++x, out += 4) {
                    const float centreX = static_cast<float>(x) + 0.5f;
                    if (centreX < quadLeft || centreX >= quadLeft + quadWidth)
                        continue;
                    const float texelX = g.atlasTopLeft.x + (centreX - quadLeft) / quadWidth * sourceWidth;
                    const int tx = clampInt(static_cast<int>(std::floor(texelX)), boxLeft, boxRight);
                    const int coverage = coverageFor(static_cast<int>(tap(tx, ty)));
                    if (coverage <= 0)
                        continue;
                    writeInkPixel(out, isOverSurface ? out : cellGround.data(), foregroundRed, foregroundGreen, foregroundBlue, coverage);
                }
                continue;
            }
            // The linear tap in 1/64 fixed point, with the same integer steps
            // the glyph shader takes: a float division or a fused multiply-add rounds
            // differently on a GPU, and one unit of coverage apart is a different
            // picture. Positions round to the 1/64 grid once, from the instance's
            // floats, which both arms hold bit for bit.
            const int centreY64 = y * 64 + 32;
            if (centreY64 < quad.originY64 || centreY64 >= quad.originY64 + quad.sizeY64)
                continue;
            const int sourceY64 = quad.atlasY64 + (centreY64 - quad.originY64) * quad.sourceHeight64 / quad.sizeY64 - 32;
            const int y0 = sourceY64 >> 6;
            const int weightY = sourceY64 & 63;
            std::uint8_t* out = rgba.data() + static_cast<std::size_t>(y) * rowStride
                + static_cast<std::size_t>(firstColumn) * 4;
            for (int x = firstColumn; x < lastColumn; ++x, out += 4) {
                const int centreX64 = x * 64 + 32;
                if (centreX64 < quad.originX64 || centreX64 >= quad.originX64 + quad.sizeX64)
                    continue;
                const int sourceX64 = quad.atlasX64 + (centreX64 - quad.originX64) * quad.sourceWidth64 / quad.sizeX64 - 32;
                const int x0 = sourceX64 >> 6;
                const int weightX = sourceX64 & 63;
                const int top = tapCoverage(x0, y0) * (64 - weightX) + tapCoverage(x0 + 1, y0) * weightX;
                const int bottom = tapCoverage(x0, y0 + 1) * (64 - weightX) + tapCoverage(x0 + 1, y0 + 1) * weightX;
                const int coverage = coverageFor((top * (64 - weightY) + bottom * weightY + 2048) >> 12);
                if (coverage <= 0)
                    continue;
                writeInkPixel(out, isOverSurface ? out : cellGround.data(), foregroundRed, foregroundGreen, foregroundBlue, coverage > 255 ? 255 : coverage);
            }
        }
    });
}

// Decoration rule -> pixels: a faithful CPU port of the Metal cell shader's
// decorationFragment (dotted = round balls, dashed = 65% duty, curly = a
// sine wave, solid = full), masked by skip-ink, alpha-blended over the cell
// pass. Blends in sRGB (the CPU path's space — for Unifont's 1-bit glyph
// coverage there are no partial values, so it matches; the rules' own AA
// edges feather in sRGB rather than the GPU's linear space).
void SoftwareRenderer::compositeDecoration(const DecorationInstance& d, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    // A skip-ink tap's nudge off the texel grid, shared with every GPU arm's
    // decoration shader so the two read the same texel.
    constexpr float skipInkTapBias = 1.0f / 128.0f;
    const float minX = d.topLeftInPixels.x;
    const float minY = d.topLeftInPixels.y;
    const float sizeX = d.bottomRightInPixels.x - minX;
    const float sizeY = d.bottomRightInPixels.y - minY;
    const float baseAlpha = d.color.w;
    if (sizeX <= 0.0f || sizeY <= 0.0f || baseAlpha <= 0.0f)
        return;

    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(d.bottomRightInPixels.x));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(d.bottomRightInPixels.y));
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > surface.width)
        x1 = surface.width;
    if (y1 > surface.height)
        y1 = surface.height;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // Half-open coverage (the GPU's top-left fill rule): a pixel is
            // in the rule iff its center falls in [min, max), so a 1px-tall
            // rule lights exactly one row rather than smearing across two.
            const float localX = (x + 0.5f) - minX;
            const float localY = (y + 0.5f) - minY;
            if (localX < 0.0f || localX >= sizeX)
                continue;

            // A dash's band is the stroke itself, so the band's height is the
            // weight the thickness setting asks for and it has to reach the
            // surface continuously. The half-open center test answers only in
            // whole rows, which quantizes that weight to the pixel grid: a band
            // between two row centers paints the same as the band below it, and
            // where the setting's step falls relative to those centers decides
            // whether it is rendered at all. Taking the fraction of its own row
            // that the band covers makes the summed weight the band's height
            // exactly, at any sub-pixel size.
            //
            // Every other kind draws a shape within a band built with slack —
            // a dot's diameter, a wave's amplitude — and gets its edge softness
            // from that shape's own distance falloff, so the band edge is not a
            // stroke edge and the center test is what belongs there.
            const bool isDashedRule = isDecorationKind(d.kind, DecorationKind::Dashed);
            float bandCoverage = 1.0f;
            if (isDashedRule) {
                const float rowTop = localY - 0.5f > 0.0f ? localY - 0.5f : 0.0f;
                const float rowBottom = localY + 0.5f < sizeY ? localY + 0.5f : sizeY;
                bandCoverage = rowBottom - rowTop;
                if (bandCoverage <= 0.0f)
                    continue;
            } else if (localY < 0.0f || localY >= sizeY)
                continue;

            float alpha = baseAlpha;
            if (isDecorationKind(d.kind, DecorationKind::Dotted)) {
                const float r = sizeY * 0.5f > 1.0f ? sizeY * 0.5f : 1.0f;
                const float period = d.dashPeriodInPixels * 0.5f > r * 3.0f ? d.dashPeriodInPixels * 0.5f : r * 3.0f;
                const float cx = (std::floor(localX / period) + 0.5f) * period;
                const float dx = localX - cx;
                const float dy = localY - sizeY * 0.5f;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float c = r + 0.5f - dist;
                alpha *= c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
            } else if (isDashedRule) {
                const float period = d.dashPeriodInPixels > d.thicknessInPixels * 6.0f ? d.dashPeriodInPixels : d.thicknessInPixels * 6.0f;
                const float phase = localX / period - std::floor(localX / period);
                alpha = phase >= 0.65f ? 0.0f : alpha * bandCoverage;
            } else if (isDecorationKind(d.kind, DecorationKind::Curly)) {
                const float period = d.dashPeriodInPixels > 2.0f ? d.dashPeriodInPixels : 2.0f;
                const float halfThick = d.thicknessInPixels * 0.8f;
                float amp = sizeY * 0.5f - halfThick;
                if (amp < 0.5f)
                    amp = 0.5f;
                const float mid = sizeY * 0.5f;
                const float wave = mid + amp * std::sin(localX / period * 6.2831853f);
                const float dist = std::fabs(localY - wave);
                const float c = halfThick + 0.5f - dist;
                alpha *= c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
            }

            if (alpha > 0.0f && d.skipInk > 0.5f) {
                const float fractionX = sizeX > 0.0f ? localX / sizeX : 0.0f;
                const float fractionY = sizeY > 0.0f ? localY / sizeY : 0.0f;
                const float u = d.inkTopLeft.x + fractionX * (d.inkBottomRight.x - d.inkTopLeft.x);
                const float v = d.inkTopLeft.y + fractionY * (d.inkBottomRight.y - d.inkTopLeft.y);
                // Three base-8 digits select the sheet holding the ink each of
                // this cell, its left and its right neighbour draws: mono,
                // proportional strip, ligature strip, or the proportional glyph
                // sheet. The two-cell sheet's digit never arrives here: this
                // renderer binds no wide cache to tap.
                const int selectors = static_cast<int>(d.pad1.y + 0.5f);
                // The cut is the glyph's coverage dilated by a disc of radius
                // padInAtlas, taken on a grid whose taps stay about a pixel
                // apart. Nine taps spread to the pad's full radius sample the
                // halo instead of filling it: once the pad passes a pixel, ink
                // between two taps leaves the rule standing in stripes inside
                // the cut, and a thick rule shows those stripes on every row.
                const int steps = std::max(static_cast<int>(d.pad1.x + 0.5f), 1);
                const float stepX = d.padInAtlas.x / static_cast<float>(steps);
                const float stepY = d.padInAtlas.y / static_cast<float>(steps);
                const auto discCoverage = [&](int sheetSelector, float centreX, float centreY, Vec2 minSlot, Vec2 maxSlot) {
                    // A slot whose max does not pass its min is an absent glyph.
                    if (maxSlot.x <= minSlot.x)
                        return 0.0f;
                    float strongest = 0.0f;
                    for (int ddy = -steps; ddy <= steps; ++ddy)
                        for (int ddx = -steps; ddx <= steps; ++ddx) {
                            // The disc, not its square: a square over-cuts the
                            // diagonal by up to the square root of two.
                            if (ddx * ddx + ddy * ddy > steps * steps)
                                continue;
                            // Nudged off the texel grid: a tap's offsets are
                            // eighths of a texel and land on texel edges
                            // exactly, where the GPU's interpolated coordinate
                            // and this one round to different sides.
                            const float tapX = centreX + static_cast<float>(ddx) * stepX + skipInkTapBias;
                            const float tapY = centreY + static_cast<float>(ddy) * stepY + skipInkTapBias;
                            // Only the glyph's own slot answers. The sheet packs
                            // glyphs side by side, and a tap that walked past
                            // this slot's edge read the neighbour in the SHEET,
                            // an unrelated glyph, and cut the rule under a
                            // letter with no ink anywhere near it.
                            if (tapX < minSlot.x || tapX > maxSlot.x || tapY < minSlot.y || tapY > maxSlot.y)
                                continue;
                            const float tap = sheetSelector == 1 ? sampleProportionalStripCoverage(tapX, tapY)
                                : sheetSelector == 2             ? sampleStripCoverage(tapX, tapY)
                                : sheetSelector == 3             ? sampleProportionalCoverage(tapX, tapY)
                                                                 : sampleMonoCoverage(tapX, tapY);
                            strongest = std::max(strongest, tap);
                        }
                    return strongest;
                };
                float coverage = discCoverage(selectors % 8, u, v, d.glyphSlotTopLeftUV, d.glyphSlotBottomRightUV);
                // A rule is per cell, so a descender leaning in from the cell
                // beside it is cut by this cell's rule too. A point fractionX of
                // the way across this cell is (fractionX + 1) of the way across
                // the left cell and (fractionX - 1) across the right one.
                const auto neighbourCoverage = [&](int sheetSelector, Vec4 ink, Vec4 box, float cellsAcross) {
                    const float centreX = ink.x + (fractionX + cellsAcross) * (ink.y - ink.x);
                    const float centreY = ink.z + fractionY * (ink.w - ink.z);
                    return discCoverage(sheetSelector, centreX, centreY, { box.x, box.y }, { box.z, box.w });
                };
                coverage = std::max(coverage, neighbourCoverage((selectors / 8) % 8, d.leftInk, d.leftBox, 1.0f));
                coverage = std::max(coverage, neighbourCoverage((selectors / 64) % 8, d.rightInk, d.rightBox, -1.0f));
                const float keep = 1.0f - coverage;
                alpha *= keep < 0.0f ? 0.0f : (keep > 1.0f ? 1.0f : keep);
            }

            if (m_isBitmapFace)
                alpha = quantizedCoverage(alpha);
            if (alpha <= 0.0f)
                continue;
            const std::size_t o = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            rgba[o] = channelToByte(rgba[o] / 255.0f * (1.0f - alpha) + d.color.x * alpha);
            rgba[o + 1] = channelToByte(rgba[o + 1] / 255.0f * (1.0f - alpha) + d.color.y * alpha);
            rgba[o + 2] = channelToByte(rgba[o + 2] / 255.0f * (1.0f - alpha) + d.color.z * alpha);
            rgba[o + 3] = 255;
        }
    }
}

namespace {

// The overlay fragment's analytic SDF math, mirrored per pixel. All distances
// are device pixels, negative inside; coordinates are quad-local with the
// quad's own extent as the unit-to-pixel scale, exactly as the shader reads
// plA/plB.

float plSegDistSW(float px, float py, float ax, float ay, float bx, float by)
{
    const float abx = bx - ax, aby = by - ay;
    const float apx = px - ax, apy = py - ay;
    const float denom = std::max(abx * abx + aby * aby, 1e-6f);
    float t = (apx * abx + apy * aby) / denom;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float deltaX = px - (ax + t * abx), deltaY = py - (ay + t * aby);
    return std::sqrt(deltaX * deltaX + deltaY * deltaY);
}

float plTriDistSW(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    const float e0x = bx - ax, e0y = by - ay;
    const float e1x = cx - bx, e1y = cy - by;
    const float e2x = ax - cx, e2y = ay - cy;
    const float v0x = px - ax, v0y = py - ay;
    const float v1x = px - bx, v1y = py - by;
    const float v2x = px - cx, v2y = py - cy;
    const auto edgeDistanceSquared = [](float vx, float vy, float ex, float ey) {
        const float denom = std::max(ex * ex + ey * ey, 1e-6f);
        float t = (vx * ex + vy * ey) / denom;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float qx = vx - ex * t, qy = vy - ey * t;
        return qx * qx + qy * qy;
    };
    const float s = (e0x * e2y - e0y * e2x) < 0.0f ? -1.0f : 1.0f;
    const float dSquared = std::min({ edgeDistanceSquared(v0x, v0y, e0x, e0y),
        edgeDistanceSquared(v1x, v1y, e1x, e1y),
        edgeDistanceSquared(v2x, v2y, e2x, e2y) });
    const float side = std::min({ s * (v0x * e0y - v0y * e0x),
        s * (v1x * e1y - v1y * e1x),
        s * (v2x * e2y - v2y * e2x) });
    return -std::sqrt(dSquared) * (side < 0.0f ? -1.0f : 1.0f);
}

// Signed distance to the line through a→b, negative on the side the winding
// sign names as inward — the outer boundary of the thin separators.
float plLineDistInwardSW(float px, float py, float ax, float ay, float bx, float by, float s)
{
    const float ex = bx - ax, ey = by - ay;
    const float len = std::max(std::sqrt(ex * ex + ey * ey), 1e-6f);
    return -s * (ex * (py - ay) - ey * (px - ax)) / len;
}

} // namespace

// Overlay quad -> pixels: the cursor (bar / underline / unfocused hollow). A
// filled rect, or — when strokeWidth > 0 — only the border of that width
// (hollow). Half-open coverage (the GPU's top-left fill rule) so a 1px edge
// lights exactly one row/column. The alpha-blend runs in the same INTEGER domain
// as the cell pass (the overlay shader's mirror). No SDF / corner-radius / shade.
void SoftwareRenderer::compositeOverlay(const OverlayInstance& o, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    if (o.shapeKind == toolkitCaretShapeKind) {
        compositeToolkitCaret(o, rgba, surface);
        return;
    }
    if (o.shapeKind < -2.5f) {
        compositeFocusedPillRing(o, rgba, surface);
        return;
    }
    const float minX = o.topLeftInPixels.x;
    const float minY = o.topLeftInPixels.y;
    const float maxX = o.bottomRightInPixels.x;
    const float maxY = o.bottomRightInPixels.y;
    const float sizeX = maxX - minX;
    const float sizeY = maxY - minY;
    const int alpha255 = clampInt(static_cast<int>(o.color.w * 255.0f + 0.5f), 0, 255);
    if (sizeX <= 0.0f || sizeY <= 0.0f || alpha255 <= 0)
        return;
    const float stroke = o.strokeWidth;

    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(maxX));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(maxY));
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > surface.width)
        x1 = surface.width;
    if (y1 > surface.height)
        y1 = surface.height;

    const int colorRed = channelToByte(o.color.x), colorGreen = channelToByte(o.color.y), colorBlue = channelToByte(o.color.z);
    const int invAlpha = 255 - alpha255;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float localX = (x + 0.5f) - minX;
            const float localY = (y + 0.5f) - minY;
            if (localX < 0.0f || localX >= sizeX || localY < 0.0f || localY >= sizeY)
                continue;
            // Hollow: keep only the border ring of `stroke` px; drop the
            // interior (matches the overlay shader's stroke test).
            if (stroke > 0.0f && o.cornerRadius <= 0.0f) {
                const bool inBorder = localX < stroke || localX >= sizeX - stroke || localY < stroke || localY >= sizeY - stroke;
                if (!inBorder)
                    continue;
            }
            // Rounded corners, the shape the GPU arms draw for a highlight
            // pill: the rounded-rect signed distance — the box shrunk by the
            // radius, the excess measured with the L2 norm for a circular arc
            // or the fourth-power norm for a continuous corner, re-inflated by
            // the radius — feathered over one pixel, exactly as overlayFragment
            // spells it. Before this the CPU drew every overlay square, and a
            // golden minted from it held square corners no GPU arm could match.
            int pixelAlpha = alpha255;
            int pixelInv = invAlpha;
            if (o.shapeKind == -2.0f) {
                const float pixelX = x + 0.5f;
                const float pixelY = y + 0.5f;
                float coverage;
                if (o.cornerRadius <= 0.0f) {
                    const float width = std::clamp(std::min(pixelX + 0.5f, o.plA.z) - std::max(pixelX - 0.5f, o.plA.x), 0.0f, 1.0f);
                    const float height = std::clamp(std::min(pixelY + 0.5f, o.plA.w) - std::max(pixelY - 0.5f, o.plA.y), 0.0f, 1.0f);
                    coverage = width * height;
                } else {
                    const float distanceX = pixelX - (o.plA.x + o.plA.z) * 0.5f;
                    const float distanceY = pixelY - (o.plA.y + o.plA.w) * 0.5f;
                    float distance = std::sqrt(distanceX * distanceX + distanceY * distanceY) - o.cornerRadius;
                    if (stroke > 0.0f) {
                        distance = std::fabs(distance + stroke * 0.5f) - stroke * 0.5f;
                    }
                    coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
                }
                if (m_isBitmapFace) {
                    coverage = quantizedCoverage(coverage);
                }
                pixelAlpha = clampInt(static_cast<int>(o.color.w * coverage * 255.0f + 0.5f), 0, 255);
                pixelInv = 255 - pixelAlpha;
            } else if (o.cornerRadius > 0.0f) {
                const float halfX = sizeX * 0.5f, halfY = sizeY * 0.5f;
                const float cx = std::fabs(localX - halfX), cy = std::fabs(localY - halfY);
                const float excessX = std::max(cx - (halfX - o.cornerRadius), 0.0f);
                const float excessY = std::max(cy - (halfY - o.cornerRadius), 0.0f);
                const float reach = o.cornerStyle > 0.5f
                    ? std::pow(excessX * excessX * excessX * excessX + excessY * excessY * excessY * excessY, 0.25f)
                    : std::sqrt(excessX * excessX + excessY * excessY);
                const float dist = reach - o.cornerRadius;
                const float strokeDistance = std::fabs(dist + stroke * 0.5f) - stroke * 0.5f;
                float coverage = stroke > 0.0f
                    ? (m_isBitmapFace ? (strokeDistance < 0.0f ? 1.0f : 0.0f) : 0.5f - strokeDistance)
                    : 0.5f - dist;
                coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
                if (m_isBitmapFace)
                    coverage = quantizedCoverage(coverage);
                if (coverage <= 0.0f)
                    continue;
                pixelAlpha = clampInt(static_cast<int>(o.color.w * coverage * 255.0f + 0.5f), 0, 255);
                if (pixelAlpha <= 0)
                    continue;
                pixelInv = 255 - pixelAlpha;
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            rgba[offset] = static_cast<std::uint8_t>((rgba[offset] * pixelInv + colorRed * pixelAlpha + 127) / 255);
            rgba[offset + 1] = static_cast<std::uint8_t>((rgba[offset + 1] * pixelInv + colorGreen * pixelAlpha + 127) / 255);
            rgba[offset + 2] = static_cast<std::uint8_t>((rgba[offset + 2] * pixelInv + colorBlue * pixelAlpha + 127) / 255);
            rgba[offset + 3] = 255;
        }
    }
}

// The toolkit caret: a square stem of whole pixels, the colour under the
// toolkit's 8-bit mask in `color.w`, mixed as pixman's source-over mixes it — each
// product rounded on its own, `MUL_UN8`, which is what GSK's cairo renderer
// does with GtkText's faded caret. The frame stays opaque, as under every
// other overlay. The overlay shader spells the same arithmetic.
void SoftwareRenderer::compositeToolkitCaret(const OverlayInstance& o, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    const int mask = clampInt(static_cast<int>(std::lround(o.color.w * 255.0f)), 0, 255);
    if (mask == 0)
        return;
    const auto multiply = [](int value, int weight) {
        const int product = value * weight + 0x80;
        return ((product >> 8) + product) >> 8;
    };
    const int x0 = std::max(static_cast<int>(std::ceil(o.topLeftInPixels.x - 0.5f)), 0);
    const int x1 = std::min(static_cast<int>(std::ceil(o.bottomRightInPixels.x - 0.5f)), surface.width);
    const int y0 = std::max(static_cast<int>(std::ceil(o.topLeftInPixels.y - 0.5f)), 0);
    const int y1 = std::min(static_cast<int>(std::ceil(o.bottomRightInPixels.y - 0.5f)), surface.height);
    const std::array<int, 3> color { channelToByte(o.color.x), channelToByte(o.color.y), channelToByte(o.color.z) };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel)
                rgba[offset + channel] = static_cast<std::uint8_t>(multiply(color[channel], mask) + multiply(rgba[offset + channel], 255 - mask));
            rgba[offset + 3] = 255;
        }
    }
}

// The focused pill's ring, the rule every GPU arm's overlay stage spells the
// same way: the pill's rect in plA with the corner radius, the shadow's reach
// and opacity in plB.xy, and the matched cells' box in plB.zw. Inside that box
// on the pill's row the destination is kept — the cell pass painted the pill
// there under the glyphs. Everywhere else the shadow lands first, its opacity
// falling off with the square of the distance past the pill's edge, and the
// pill's coverage over it, so the rounded ends sit above the neighbours' ink
// as Safari's find highlight does. Both opacities are held to the bitmap
// tier's levels and mixed on straight bytes with the same rounding as every
// other overlay.
void SoftwareRenderer::compositeFocusedPillRing(const OverlayInstance& o, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    const float left = o.plA.x, top = o.plA.y, right = o.plA.z, bottom = o.plA.w;
    const float shadowReach = o.plB.x;
    const float shadowAlpha = o.plB.y;
    const float boxLeft = o.plB.z, boxRight = o.plB.w;
    const float halfX = (right - left) * 0.5f, halfY = (bottom - top) * 0.5f;
    const float radius = o.cornerRadius;
    // The pill stands `strokeWidth` below the cells' box's top and
    // `shadeDensity` above its bottom, and the cell pass painted the box
    // whole; the ground for the strips the insets vacate rides in the colour's
    // alpha as three packed bytes (the overlay assembly's spelling).
    const float inset = o.strokeWidth;
    const float bottomInset = o.shadeDensity;
    const int packedGround = static_cast<int>(o.color.w);
    const int groundRed = (packedGround / 65536) & 0xFF, groundGreen = (packedGround / 256) & 0xFF, groundBlue = packedGround & 0xFF;
    int x0 = std::max(static_cast<int>(std::floor(o.topLeftInPixels.x)), 0);
    int y0 = std::max(static_cast<int>(std::floor(o.topLeftInPixels.y)), 0);
    int x1 = std::min(static_cast<int>(std::ceil(o.bottomRightInPixels.x)), surface.width);
    int y1 = std::min(static_cast<int>(std::ceil(o.bottomRightInPixels.y)), surface.height);
    const int pillRed = channelToByte(o.color.x), pillGreen = channelToByte(o.color.y), pillBlue = channelToByte(o.color.z);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float centreX = static_cast<float>(x) + 0.5f, centreY = static_cast<float>(y) + 0.5f;
            // Inside the cells' box the pill's own rows keep the cell pass's
            // fill; the strips the insets vacated are rebuilt from the ground,
            // with the shadow and the pill's edge composited over them below.
            const bool isInBox = centreX >= boxLeft && centreX < boxRight;
            const bool isInStrip = isInBox
                && ((inset > 0.0f && centreY >= top - inset && centreY < top)
                    || (bottomInset > 0.0f && centreY >= bottom && centreY < bottom + bottomInset));
            if (isInBox && !isInStrip && centreY >= top && centreY < bottom)
                continue;
            const float excessX = std::max(std::fabs(centreX - (left + halfX)) - (halfX - radius), 0.0f);
            const float excessY = std::max(std::fabs(centreY - (top + halfY)) - (halfY - radius), 0.0f);
            const float distance = std::sqrt(excessX * excessX + excessY * excessY) - radius;
            float pillCoverage = 0.5f - distance;
            pillCoverage = pillCoverage < 0.0f ? 0.0f : (pillCoverage > 1.0f ? 1.0f : pillCoverage);
            const float falloff = shadowReach > 0.0f ? 1.0f - std::min(std::max(distance / shadowReach, 0.0f), 1.0f) : 0.0f;
            float shadow = shadowAlpha * falloff * falloff;
            if (m_isBitmapFace) {
                pillCoverage = quantizedCoverage(pillCoverage);
                shadow = quantizedCoverage(shadow);
            }
            const int shadowByte = clampInt(static_cast<int>(shadow * 255.0f + 0.5f), 0, 255);
            const int pillByte = clampInt(static_cast<int>(pillCoverage * 255.0f + 0.5f), 0, 255);
            if (!isInStrip && shadowByte <= 0 && pillByte <= 0)
                continue;
            const std::size_t offset = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            int red = isInStrip ? groundRed : rgba[offset];
            int green = isInStrip ? groundGreen : rgba[offset + 1];
            int blue = isInStrip ? groundBlue : rgba[offset + 2];
            if (shadowByte > 0) {
                const int inverse = 255 - shadowByte;
                red = (red * inverse + 127) / 255;
                green = (green * inverse + 127) / 255;
                blue = (blue * inverse + 127) / 255;
            }
            if (pillByte > 0) {
                const int inverse = 255 - pillByte;
                red = (red * inverse + pillRed * pillByte + 127) / 255;
                green = (green * inverse + pillGreen * pillByte + 127) / 255;
                blue = (blue * inverse + pillBlue * pillByte + 127) / 255;
            }
            rgba[offset] = static_cast<std::uint8_t>(red);
            rgba[offset + 1] = static_cast<std::uint8_t>(green);
            rgba[offset + 2] = static_cast<std::uint8_t>(blue);
            rgba[offset + 3] = 255;
        }
    }
}

void SoftwareRenderer::compositeBoxFill(const OverlayInstance& o, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    const float kind = o.shapeKind;
    // A plain strip fill is the rect compositeOverlay already draws; the
    // analytic forms and the flat shade need per-pixel coverage of their own.
    if ((kind == 0.0f || kind == -2.0f) && o.shadeDensity <= 0.0f) {
        compositeOverlay(o, rgba, surface);
        return;
    }
    const float minX = o.topLeftInPixels.x;
    const float minY = o.topLeftInPixels.y;
    const float maxX = o.bottomRightInPixels.x;
    const float maxY = o.bottomRightInPixels.y;
    const float sizeX = maxX - minX;
    const float sizeY = maxY - minY;
    const float alphaBase = o.color.w;
    if (sizeX <= 0.0f || sizeY <= 0.0f || alphaBase <= 0.0f)
        return;
    // A pattern shade is a TONE built from solid foreground, lit where Core's
    // field says, anchored by the grid phase the quad carries in plA.x.
    const int shadeLit = o.shadeDensity > 0.0f ? shadeLitCount(o.shadeDensity) : 0;
    const int shadeFieldPhase = static_cast<int>(o.plA.x + 0.5f);

    int x0 = static_cast<int>(std::floor(minX));
    int x1 = static_cast<int>(std::ceil(maxX));
    int y0 = static_cast<int>(std::floor(minY));
    int y1 = static_cast<int>(std::ceil(maxY));
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > surface.width ? surface.width : x1;
    y1 = y1 > surface.height ? surface.height : y1;
    const int colorRed = channelToByte(o.color.x), colorGreen = channelToByte(o.color.y), colorBlue = channelToByte(o.color.z);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float localX = (x + 0.5f) - minX;
            const float localY = (y + 0.5f) - minY;
            if (localX < 0.0f || localX >= sizeX || localY < 0.0f || localY >= sizeY)
                continue;
            float coverage = 1.0f;
            if (shadeLit > 0) {
                if (!isShadePixelLit(x, y, shadeLit, shadeFieldPhase))
                    continue;
            } else if (kind < -0.5f) {
                // Mosaic mask: resolve the subcell and test its bit, the
                // fragment's rule verbatim (rounded splits, no feathering).
                const unsigned columns = std::max(static_cast<unsigned>(o.plA.y + 0.5f), 1u);
                const unsigned rows = std::max(static_cast<unsigned>(o.plA.z + 0.5f), 1u);
                unsigned column = localX < std::round(sizeX * 0.5f) ? 0u : 1u;
                unsigned row = 0u;
                if (rows == 2u) {
                    row = localY < std::round(sizeY * 0.5f) ? 0u : 1u;
                } else {
                    const float firstSplit = std::round(sizeY / 3.0f);
                    const float secondSplit = std::round(sizeY * 2.0f / 3.0f);
                    row = localY < firstSplit ? 0u : (localY < secondSplit ? 1u : 2u);
                }
                column = std::min(column, columns - 1u);
                row = std::min(row, rows - 1u);
                const unsigned mask = static_cast<unsigned>(o.plA.x + 0.5f);
                if (((mask >> (row * columns + column)) & 1u) == 0u)
                    continue;
            } else if (kind > 0.5f) {
                const float ax = o.plA.x * sizeX, ay = o.plA.y * sizeY;
                const float bx = o.plA.z * sizeX, by = o.plA.w * sizeY;
                const float cx = o.plB.x * sizeX, cy = o.plB.y * sizeY;
                float dist;
                if (kind > 6.5f) {
                    dist = ellipticalArcDistance({ localX, localY }, { ax, ay }, { bx, by }, { cx, cy }, { o.plB.z, o.plB.w });
                } else if (kind < 1.5f) {
                    dist = plTriDistSW(localX, localY, ax, ay, bx, by, cx, cy);
                } else if (kind < 2.5f) {
                    const float strokeHalf = o.plB.z * sizeY;
                    dist = std::min(plSegDistSW(localX, localY, ax, ay, bx, by),
                               plSegDistSW(localX, localY, bx, by, cx, cy))
                        - strokeHalf;
                } else if (kind < 4.5f) {
                    const float centreX = o.plA.x * sizeX, centreY = o.plA.y * sizeY;
                    const float radiusX = std::max(o.plA.z * sizeX, 1e-3f);
                    const float radiusY = std::max(o.plA.w * sizeY, 1e-3f);
                    const float qx = (localX - centreX) / radiusX, qy = (localY - centreY) / radiusY;
                    // First-order ellipse distance — the fragment's rule: the
                    // implicit value over its own gradient, so the arc stroke
                    // keeps one weight on an eccentric ellipse.
                    const float lengthQ = std::max(std::sqrt(qx * qx + qy * qy), 1e-4f);
                    const float gradient = std::max(std::sqrt((qx / radiusX) * (qx / radiusX) + (qy / radiusY) * (qy / radiusY)), 1e-6f);
                    const float ellipse = (lengthQ - 1.0f) * lengthQ / gradient;
                    const float side = o.plB.x * (localX - centreX);
                    if (kind < 3.5f) {
                        dist = std::max(ellipse, -side);
                    } else {
                        const float strokeHalf = o.plB.y * sizeY;
                        dist = std::max(std::abs(ellipse + strokeHalf) - strokeHalf, -side);
                    }
                } else {
                    const float winding = ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) < 0.0f ? -1.0f : 1.0f;
                    const float strokeHalf = o.plB.z * sizeY;
                    float outer = plLineDistInwardSW(localX, localY, ax, ay, bx, by, winding);
                    if (kind < 5.5f)
                        outer = std::max(outer, plLineDistInwardSW(localX, localY, bx, by, cx, cy, winding));
                    dist = std::abs(outer + strokeHalf) - strokeHalf;
                }
                coverage = 0.5f - dist;
                coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
                if (coverage <= 0.0f)
                    continue;
            }
            if (m_isBitmapFace)
                coverage = quantizedCoverage(coverage);
            const int alpha255 = clampInt(static_cast<int>(alphaBase * coverage * 255.0f + 0.5f), 0, 255);
            if (alpha255 <= 0)
                continue;
            const int invAlpha = 255 - alpha255;
            const std::size_t offset = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            rgba[offset] = static_cast<std::uint8_t>((rgba[offset] * invAlpha + colorRed * alpha255 + 127) / 255);
            rgba[offset + 1] = static_cast<std::uint8_t>((rgba[offset + 1] * invAlpha + colorGreen * alpha255 + 127) / 255);
            rgba[offset + 2] = static_cast<std::uint8_t>((rgba[offset + 2] * invAlpha + colorBlue * alpha255 + 127) / 255);
            rgba[offset + 3] = 255;
        }
    }
}

// Inline images -> pixels: the CPU half of the image pass. Resolves placements
// and placeholder cells to device-pixel quads once a frame (resolveImageQuads),
// z-sorts ascending (lower z first; a stable sort keeps the resolve order on
// ties, so a later same-z placement draws on top — the kitty rule the GPU path
// mirrors), then samples each quad of the requested z-`band` and straight-alpha
// over-blends it into the frame. The under band (z<0) is composited over the
// cell backgrounds and before any text ink, the over band (z>=0) after the text
// and its rules, matching the Metal frame encoder's pass order for the
// text. Clipped to the quad's device rect (resolveImagePlacements already
// clamped it to the grid viewport).
const SoftwareRenderer::ImageMipChain& SoftwareRenderer::imageMipChain(const Image::TerminalImage& image) const
{
    const std::uint64_t stamp = Image::imageContentStamp(image);
    for (ImageMipChain& chain : m_imageMipChains) {
        if (chain.imageID == image.imageID) {
            chain.isSeenThisFrame = true;
            if (chain.stamp == stamp)
                return chain;
            chain.sizes.clear();
            chain.levels.clear();
            chain.stamp = stamp;
            return buildImageMipChain(chain, image);
        }
    }
    ImageMipChain& chain = m_imageMipChains.emplace_back();
    chain.imageID = image.imageID;
    chain.stamp = stamp;
    chain.isSeenThisFrame = true;
    return buildImageMipChain(chain, image);
}

SoftwareRenderer::ImageMipChain& SoftwareRenderer::buildImageMipChain(ImageMipChain& chain, const Image::TerminalImage& image)
{
    int width = image.width;
    int height = image.height;
    std::vector<std::uint8_t> base(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const unsigned char* pixels = image.currentFrame < image.frameCount ? image.frames[image.currentFrame].pixels : image.pixels;
    Image::widenToRGBA8(pixels, width, height, image.format, base.data());
    Image::bleedIntoTransparentTexels(base.data(), width, height);
    const int levelCount = Image::imageMipLevelCount(width, height);
    chain.sizes.reserve(static_cast<std::size_t>(levelCount));
    chain.levels.reserve(static_cast<std::size_t>(levelCount));
    chain.sizes.emplace_back(width, height);
    chain.levels.push_back(std::move(base));
    for (int level = 1; level < levelCount; ++level) {
        const int nextWidth = std::max(1, width / 2);
        const int nextHeight = std::max(1, height / 2);
        std::vector<std::uint8_t> next(static_cast<std::size_t>(nextWidth) * static_cast<std::size_t>(nextHeight) * 4);
        Image::downsampleImageMipLevel(chain.levels.back().data(), width, height, next.data());
        Image::bleedIntoTransparentTexels(next.data(), nextWidth, nextHeight);
        chain.sizes.emplace_back(nextWidth, nextHeight);
        chain.levels.push_back(std::move(next));
        width = nextWidth;
        height = nextHeight;
    }
    return chain;
}

namespace {

// One texel of a level, clamped to its edge, as straight RGBA in 0-255.
std::array<float, 4> levelTexel(const std::vector<std::uint8_t>& level, IntSize size, int x, int y)
{
    x = std::clamp(x, 0, size.width - 1);
    y = std::clamp(y, 0, size.height - 1);
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(x)) * 4;
    return { static_cast<float>(level[index]), static_cast<float>(level[index + 1]), static_cast<float>(level[index + 2]), static_cast<float>(level[index + 3]) };
}

// Bilinear at normalized (u, v) within one level, texel centres at half
// integers and the edge repeated past the last texel, as a clamp-to-edge GPU
// sampler reads it.
std::array<float, 4> bilinear(const std::vector<std::uint8_t>& level, IntSize size, float u, float v)
{
    const float tx = u * static_cast<float>(size.width) - 0.5f;
    const float ty = v * static_cast<float>(size.height) - 0.5f;
    const float floorX = std::floor(tx);
    const float floorY = std::floor(ty);
    const float wx = tx - floorX;
    const float wy = ty - floorY;
    const int x0 = static_cast<int>(floorX);
    const int y0 = static_cast<int>(floorY);
    const std::array<float, 4> a = levelTexel(level, size, x0, y0);
    const std::array<float, 4> b = levelTexel(level, size, x0 + 1, y0);
    const std::array<float, 4> c = levelTexel(level, size, x0, y0 + 1);
    const std::array<float, 4> d = levelTexel(level, size, x0 + 1, y0 + 1);
    std::array<float, 4> out {};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const float top = a[channel] + (b[channel] - a[channel]) * wx;
        const float bottom = c[channel] + (d[channel] - c[channel]) * wx;
        out[channel] = top + (bottom - top) * wy;
    }
    return out;
}

} // namespace

void SoftwareRenderer::compositeFilteredImage(const Image::ImageQuad& quad, const Image::TerminalImage& image, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    const float spanX = quad.maxXInPixels - quad.minXInPixels;
    const float spanY = quad.maxYInPixels - quad.minYInPixels;
    if (spanX <= 0.0f || spanY <= 0.0f)
        return;
    const ImageMipChain& chain = imageMipChain(image);
    const float levelOfDetail = Image::imageLevelOfDetail((quad.u1 - quad.u0) * static_cast<float>(image.width),
        (quad.v1 - quad.v0) * static_cast<float>(image.height), spanX, spanY);
    const int lastLevel = static_cast<int>(chain.levels.size()) - 1;
    const float clamped = std::min(levelOfDetail, static_cast<float>(lastLevel));
    const int lower = static_cast<int>(std::floor(clamped));
    const int upper = std::min(lower + 1, lastLevel);
    const float between = clamped - static_cast<float>(lower);
    const int x0 = clampInt(static_cast<int>(std::floor(quad.minXInPixels)), 0, surface.width);
    const int y0 = clampInt(static_cast<int>(std::floor(quad.minYInPixels)), 0, surface.height);
    const int x1 = clampInt(static_cast<int>(std::ceil(quad.maxXInPixels)), 0, surface.width);
    const int y1 = clampInt(static_cast<int>(std::ceil(quad.maxYInPixels)), 0, surface.height);
    const auto& lowerLevel = chain.levels[static_cast<std::size_t>(lower)];
    const auto& upperLevel = chain.levels[static_cast<std::size_t>(upper)];
    const IntSize lowerSize = chain.sizes[static_cast<std::size_t>(lower)];
    const IntSize upperSize = chain.sizes[static_cast<std::size_t>(upper)];
    for (int y = y0; y < y1; ++y) {
        const float v = quad.v0 + (((y + 0.5f) - quad.minYInPixels) / spanY) * (quad.v1 - quad.v0);
        for (int x = x0; x < x1; ++x) {
            const float u = quad.u0 + (((x + 0.5f) - quad.minXInPixels) / spanX) * (quad.u1 - quad.u0);
            std::array<float, 4> texel = bilinear(lowerLevel, lowerSize, u, v);
            if (between > 0.0f) {
                const std::array<float, 4> coarser = bilinear(upperLevel, upperSize, u, v);
                for (std::size_t channel = 0; channel < 4; ++channel)
                    texel[channel] += (coarser[channel] - texel[channel]) * between;
            }
            const int alpha = static_cast<int>(std::lround(texel[3]));
            if (alpha <= 0)
                continue;
            const int sr = static_cast<int>(std::lround(texel[0]));
            const int sg = static_cast<int>(std::lround(texel[1]));
            const int sb = static_cast<int>(std::lround(texel[2]));
            const std::size_t o = (static_cast<std::size_t>(y) * surface.width + x) * 4;
            if (alpha >= 255) {
                rgba[o] = static_cast<std::uint8_t>(sr);
                rgba[o + 1] = static_cast<std::uint8_t>(sg);
                rgba[o + 2] = static_cast<std::uint8_t>(sb);
                rgba[o + 3] = 255;
                continue;
            }
            const int invAlpha = 255 - alpha;
            rgba[o] = static_cast<std::uint8_t>((rgba[o] * invAlpha + sr * alpha + 127) / 255);
            rgba[o + 1] = static_cast<std::uint8_t>((rgba[o + 1] * invAlpha + sg * alpha + 127) / 255);
            rgba[o + 2] = static_cast<std::uint8_t>((rgba[o + 2] * invAlpha + sb * alpha + 127) / 255);
            rgba[o + 3] = 255;
        }
    }
}

void SoftwareRenderer::resolveImageQuads(const BuildInputs& in)
{
    // Once a frame, before either band: what the previous frame's chains held
    // for an image no longer in the frame is dropped after both bands have had
    // their chance to use it.
    std::erase_if(m_imageMipChains, [](const ImageMipChain& chain) { return !chain.isSeenThisFrame; });
    for (ImageMipChain& chain : m_imageMipChains)
        chain.isSeenThisFrame = false;
    m_imageQuads.clear();
    m_imageQuadSplit = 0;
    if (!in.images || in.imageCount <= 0)
        return;

    const std::span<const Image::TerminalImage> images { in.images, static_cast<std::size_t>(in.imageCount) };
    // Room for every placement and every placeholder cell the grid shows.
    // Kept across frames: a frame with images reuses the storage the last one
    // grew.
    m_imageQuads.resize(imageQuadCapacity({ in.columns, in.rows }));
    const std::span<Image::ImageQuad> quads { m_imageQuads };
    const FloatSize cell { static_cast<float>(m_cellWidth), static_cast<float>(m_cellHeight) };
    // The grid's top-left in the drawable, which is where the cell pass put it:
    // a quad resolved against a different origin than the cells it sits among
    // lands beside the text rather than on it.
    const FloatPoint origin { in.originInPixels.x, in.originInPixels.y };
    int count = 0;
    if (in.placements && in.placementCount > 0) {
        count = Ubiquitty::Image::resolveImagePlacements(images,
            { in.placements, static_cast<std::size_t>(in.placementCount) },
            { in.columns, in.rows }, cell, origin, in.viewOffset, quads,
            /*fitOptions=*/ {}, in.alternateActive);
    }
    // A placeholder cell names its image through the cells the frame captured,
    // so it resolves from those rather than from any placement table.
    if (in.cells && count < static_cast<int>(quads.size())) {
        const Kitty::PlaceholderGrid grid {
            .cells = { in.cells, static_cast<std::size_t>(in.rows) * static_cast<std::size_t>(in.columns) },
            .size = { in.columns, in.rows },
            .readCluster = in.clusterResolver,
            .readClusterContext = in.clusterResolverContext,
            .publishedCellPixels = in.publishedCellPixels,
        };
        count += Kitty::resolvePlaceholders(grid, images, cell, origin, quads.subspan(static_cast<std::size_t>(count)));
    }
    m_imageQuads.resize(static_cast<std::size_t>(std::max(count, 0)));
    m_imageQuadSplit = Ubiquitty::Image::orderImageQuadsForDrawing(m_imageQuads);
}

void SoftwareRenderer::compositeImages(const BuildInputs& in, ImageBand band, std::vector<std::uint8_t>& rgba, IntSize surface) const
{
    const std::size_t split = static_cast<std::size_t>(std::clamp(m_imageQuadSplit, 0, static_cast<int>(m_imageQuads.size())));
    const std::span<const Image::ImageQuad> all { m_imageQuads };
    const std::span<const Image::ImageQuad> bandQuads = band == ImageBand::Under ? all.first(split) : all.subspan(split);
    for (const Image::ImageQuad& q : bandQuads) {
        // Resolve the image once; both sampling paths use its selected frame.
        const Ubiquitty::Image::TerminalImage* img = nullptr;
        for (int ii = 0; ii < in.imageCount; ++ii) {
            const Ubiquitty::Image::TerminalImage* cand = &in.images[ii];
            if (cand->used && cand->pixels && cand->imageID == q.imageID) {
                img = cand;
                break;
            }
        }
        if (!img || img->width <= 0 || img->height <= 0)
            continue;
        const int bytesPerPixel = (img->format == 32) ? 4 : 3;
        const unsigned char* src = img->currentFrame < img->frameCount ? img->frames[img->currentFrame].pixels : img->pixels;
        if (q.filter == Image::ImageFilter::Smooth) {
            compositeFilteredImage(q, *img, rgba, surface);
            continue;
        }

        int x0 = static_cast<int>(std::floor(q.minXInPixels));
        int y0 = static_cast<int>(std::floor(q.minYInPixels));
        int x1 = static_cast<int>(std::ceil(q.maxXInPixels));
        int y1 = static_cast<int>(std::ceil(q.maxYInPixels));
        x0 = clampInt(x0, 0, surface.width);
        y0 = clampInt(y0, 0, surface.height);
        x1 = clampInt(x1, 0, surface.width);
        y1 = clampInt(y1, 0, surface.height);
        const float spanX = q.maxXInPixels - q.minXInPixels;
        const float spanY = q.maxYInPixels - q.minYInPixels;
        if (spanX <= 0.0f || spanY <= 0.0f)
            continue;

        for (int y = y0; y < y1; ++y) {
            // Pixel-center UV across the quad -> source sub-rect -> nearest texel.
            const float fy = ((y + 0.5f) - q.minYInPixels) / spanY;
            const float v = q.v0 + fy * (q.v1 - q.v0);
            int sy = static_cast<int>(v * img->height);
            sy = clampInt(sy, 0, img->height - 1);
            for (int x = x0; x < x1; ++x) {
                const float fx = ((x + 0.5f) - q.minXInPixels) / spanX;
                const float u = q.u0 + fx * (q.u1 - q.u0);
                int sx = static_cast<int>(u * img->width);
                sx = clampInt(sx, 0, img->width - 1);

                const std::size_t si = (static_cast<std::size_t>(sy) * img->width + sx) * bytesPerPixel;
                const int sr = src[si];
                const int sg = src[si + 1];
                const int sb = src[si + 2];
                // RGB (format 24) is opaque; RGBA (format 32) carries straight
                // alpha. Over-blend in the integer domain — the same formula
                // the cell/overlay passes use.
                const int alpha = (bytesPerPixel == 4) ? src[si + 3] : 255;
                const std::size_t o = (static_cast<std::size_t>(y) * surface.width + x) * 4;
                if (alpha == 0)
                    continue;
                if (alpha == 255) {
                    rgba[o] = static_cast<std::uint8_t>(sr);
                    rgba[o + 1] = static_cast<std::uint8_t>(sg);
                    rgba[o + 2] = static_cast<std::uint8_t>(sb);
                    rgba[o + 3] = 255;
                    continue;
                }
                const int invAlpha = 255 - alpha;
                rgba[o] = static_cast<std::uint8_t>((rgba[o] * invAlpha + sr * alpha + 127) / 255);
                rgba[o + 1] = static_cast<std::uint8_t>((rgba[o + 1] * invAlpha + sg * alpha + 127) / 255);
                rgba[o + 2] = static_cast<std::uint8_t>((rgba[o + 2] * invAlpha + sb * alpha + 127) / 255);
                rgba[o + 3] = 255;
            }
        }
    }
}

std::span<const LetterExtent> SoftwareRenderer::rowLetters(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_letterRows.size()) || m_letterRows[static_cast<std::size_t>(row)] == 0u || m_letterColumns <= 0)
        return {};
    const std::size_t first = static_cast<std::size_t>(row) * static_cast<std::size_t>(m_letterColumns);
    if (first + static_cast<std::size_t>(m_letterColumns) > m_letters.size())
        return {};
    return std::span<const LetterExtent>(m_letters).subspan(first, static_cast<std::size_t>(m_letterColumns));
}

void SoftwareRenderer::lendLetters(const BuildInputs& in, InstanceBuffers& buffers)
{
    // The letters of the rows the build lays out as runs, kept across frames
    // so a pointer resolves against what the last frame drew.
    const std::size_t letterCount = static_cast<std::size_t>(std::max(in.rows, 0)) * static_cast<std::size_t>(std::max(in.columns, 0));
    if (m_letters.size() != letterCount || m_letterRows.size() != static_cast<std::size_t>(std::max(in.rows, 0))) {
        m_letters.assign(letterCount, LetterExtent {});
        m_letterRows.assign(static_cast<std::size_t>(std::max(in.rows, 0)), 0u);
    }
    m_letterColumns = in.columns;
    buffers.letters = m_letters.data();
    buffers.letterCapacity = m_letters.size();
    buffers.letterRows = m_letterRows.data();
    buffers.letterRowCapacity = m_letterRows.size();
}

int SoftwareRenderer::columnAt(int row, double xInPixels) const
{
    const std::span<const LetterExtent> letters = rowLetters(row);
    if (!letters.empty())
        return letterColumnAt(letters, xInPixels);
    if (m_cellWidth <= 0 || m_letterColumns <= 0)
        return 0;
    return std::clamp(static_cast<int>(std::floor(xInPixels / static_cast<double>(m_cellWidth))), 0, m_letterColumns - 1);
}

double SoftwareRenderer::xInCellsAt(int row, double xInPixels) const
{
    const std::span<const LetterExtent> letters = rowLetters(row);
    if (!letters.empty())
        return letterXInCells(letters, xInPixels);
    if (m_cellWidth <= 0)
        return 0.0;
    return std::clamp(xInPixels / static_cast<double>(m_cellWidth), 0.0, static_cast<double>(std::max(m_letterColumns, 0)));
}

bool SoftwareRenderer::render(const BuildInputs& in, std::vector<std::uint8_t>& rgbaOut, int& widthOut, int& heightOut,
    std::array<std::uint8_t, 4> paddingColor, BlockLayer blocks)
{
    m_isBitmapFace = in.isBitmapFace;
    if (!in.cells || !in.palette || in.rows <= 0 || in.columns <= 0)
        return false;

    // The surface is the caller's drawable when it has one, and the grid
    // otherwise. A frontend that insets its grid — which is what the OSC 133
    // prompt-mark gutter is — passes the whole drawable here and the inset in
    // `originInPixels`. Sizing this to the grid instead gives the frame-level
    // passes coordinates the buffer does not have, and they clip away silently.
    //
    // Zero is the grid exactly, so a caller that keeps no drawable is unaffected
    // by this existing. A drawable narrower than its grid clips, which is what a
    // drawable smaller than its content means everywhere else in the renderer.
    const int width = in.viewportSizeInPixels.x > 0.0f ? static_cast<int>(in.viewportSizeInPixels.x) : in.columns * m_cellWidth;
    const int height = in.viewportSizeInPixels.y > 0.0f ? static_cast<int>(in.viewportSizeInPixels.y) : in.rows * m_cellHeight;

    // The origin, rounded to whole pixels, and the rounding is a real decision.
    //
    // The Metal cell shader places a quad at `originInPixels + gridPos * cell`
    // in floats and lets the rasteriser's fill rule decide coverage, which
    // resolves a fractional edge to the nearest pixel boundary. This pass cannot
    // do that: it blits atlas coverage one texel to one pixel and has no
    // resampler, so a fractional origin has to become a whole one somewhere.
    // Rounding is the closest available match — truncating would disagree with
    // the shader by a whole pixel where rounding disagrees by at most half.
    //
    // The remaining divergence is bounded and real: on a surface whose inset is
    // not integral, the CPU grid can sit half a pixel from the GPU's. A frontend
    // that wants them identical passes an integral inset, which every frontend
    // driving this path controls. macOS does not use this path.
    const int gridOriginX = static_cast<int>(std::lround(in.originInPixels.x));
    const int gridOriginY = static_cast<int>(std::lround(in.originInPixels.y));

    // The cell + decoration metrics come from the injected rasterizer (the
    // Unifont rasterizer returns the fixed 8×16 cell verbatim, so the Unifont
    // path is byte-identical; the FreeType rasterizer returns a font-derived
    // cell). The decoration ALGORITHM is the macOS one; only these inputs
    // differ across backends.
    reserveSheetsForScreen(in.rows * in.columns);

    BuildInputs local = in;
    // A zero cell means the caller has no metrics of its own — the Unifont
    // rasterizer returns the fixed 8x16 cell, a FreeType one returns the
    // font-derived box. A caller that did supply metrics keeps them.
    if (local.metrics.cellWidth <= 0 || local.metrics.cellHeight <= 0)
        local.metrics = m_rasterizer.cellMetrics();
    if (local.glyphReferenceAdvanceInPixels <= 0.0f)
        local.glyphReferenceAdvanceInPixels = local.metrics.cellWidth;
    if (local.glyphReferenceLineHeightInPixels <= 0.0f)
        local.glyphReferenceLineHeightInPixels = local.metrics.cellHeight;
    m_sizedBandLineHeightInPixels = local.glyphReferenceLineHeightInPixels;
    m_sizedWindowAdvanceInPixels = local.glyphReferenceAdvanceInPixels;
    if (local.glyphBoxWidthInPixels <= 0.0f)
        local.glyphBoxWidthInPixels = static_cast<float>(m_cellWidth);
    // This renderer's wide page is two bare cells a slot, with no margins, so
    // a wide-slot geometry handed in describes some other renderer's page.
    // Zero keeps every two-cell glyph on its cell quad; the wide stream below
    // then carries only the glyphs a fractional block draws smaller.
    local.glyphWideSlotWidthInPixels = 0.0f;
    local.glyphWideSlotHeightInPixels = 0.0f;

    // One instance a cell, and one more a row: a row laid out as letters
    // (LetterLayout.h) paints the pane past its last cell with a quad of its
    // own.
    const std::size_t cellCapacity = static_cast<std::size_t>(in.rows) * in.columns + static_cast<std::size_t>(in.rows);
    m_cellInstances.resize(cellCapacity);
    // One rule per cell to start, grown to whatever a frame actually asks for.
    // A cell can carry ten — framed box four, double underline two, ideogram
    // double underline two, strike, overline, all reachable together through
    // SGR — so a guess that stops short drops rules from the last rows it
    // reaches, and a guess that covers the worst case holds megabytes for a
    // frame that uses a fraction of it. The counts the builder returns are the
    // answer, and it documents them as one: resize to the count, build again.
    // Never shrunk, so the second build happens on the frame the working set
    // grows and not on the ones after it.
    if (m_decorations.size() < cellCapacity)
        m_decorations.resize(cellCapacity);
    m_overlays.resize(maxOverlaysForRows(in.rows, static_cast<std::size_t>(in.otherSearchMatchSpanCount > 0 ? in.otherSearchMatchSpanCount : 0), static_cast<std::size_t>(in.triggerHighlightSpanCount > 0 ? in.triggerHighlightSpanCount : 0), static_cast<std::size_t>(in.hiliteRegionSpanCount > 0 ? in.hiliteRegionSpanCount : 0)));
    // One subpixel overflow glyph per plain-mono cell (the cap; the builder skips
    // wide / box / combining / space). Empty unless subpixelGlyphs is enabled.
    m_glyphs.resize(cellCapacity);
    // Never empty: the builder reads a null stream as "no wide stream", and a
    // frame needing more than a row's worth regrows from the count below.
    if (m_wideGlyphs.size() < static_cast<std::size_t>(std::max(in.rows, 1)))
        m_wideGlyphs.resize(static_cast<std::size_t>(std::max(in.rows, 1)));
    // Offering this buffer is what routes box-drawing cells onto the analytic
    // quad stream the GPU arms draw — the builder reads its presence as the
    // question. One quad per cell to start; a decomposition can emit several,
    // and the count-driven regrow below covers the frame that does.
    if (m_boxFills.size() < cellCapacity)
        m_boxFills.resize(cellCapacity);

    InstanceBuffers buffers;
    buffers.cells = m_cellInstances.data();
    buffers.cellCapacity = m_cellInstances.size();
    buffers.decorations = m_decorations.data();
    buffers.decorationCapacity = m_decorations.size();
    buffers.overlays = m_overlays.data();
    buffers.overlayCapacity = m_overlays.size();
    buffers.glyphs = m_glyphs.data();
    buffers.glyphCapacity = m_glyphs.size();
    buffers.wideGlyphs = m_wideGlyphs.data();
    buffers.wideGlyphCapacity = m_wideGlyphs.size();
    buffers.boxFills = m_boxFills.data();
    buffers.boxFillCapacity = m_boxFills.size();
    buffers.scratch = &m_builderScratch;
    lendLetters(in, buffers);
    // Offered only with a colour source. The builder reads `out.colorGlyphs` as
    // the question "does this caller keep emoji on a sheet of their own", so
    // handing it a buffer while the hooks are null would route every emoji into a
    // stream nothing can fill.
    if (m_hasColorSource) {
        if (m_colorGlyphs.size() < m_glyphs.size())
            m_colorGlyphs.resize(m_glyphs.size());
        buffers.colorGlyphs = m_colorGlyphs.data();
        buffers.colorGlyphCapacity = m_colorGlyphs.size();
    }

    if (m_hasLigatureShaping && m_ligature.isUsable()) {
        if (m_ligatureGlyphs.size() < m_glyphs.size())
            m_ligatureGlyphs.resize(m_glyphs.size());
        buffers.ligatureGlyphs = m_ligatureGlyphs.data();
        buffers.ligatureGlyphCapacity = m_ligatureGlyphs.size();
    }
    if (m_hasStripSheet && m_proportionalStrip.isUsable()) {
        if (m_proportionalStrips.size() < m_glyphs.size())
            m_proportionalStrips.resize(m_glyphs.size());
        buffers.proportionalStrips = m_proportionalStrips.data();
        buffers.proportionalStripCapacity = m_proportionalStrips.size();
    }
    if (m_proportional.isUsable()) {
        if (m_proportionalGlyphs.size() < cellCapacity)
            m_proportionalGlyphs.resize(cellCapacity);
        buffers.proportionalGlyphs = m_proportionalGlyphs.data();
        buffers.proportionalGlyphCapacity = m_proportionalGlyphs.size();
    }

    const GlyphBackend backend = glyphBackend();
    InstanceCounts counts = buildCellInstances(local, backend, buffers);
    // The builder reports a count past capacity as the signal to grow and ask
    // again, which is the only way a frame that overflows renders whole. The
    // rebuild reuses the same inputs and backend: a different one would answer
    // with different counts, and the buffers would be sized for a frame nobody
    // asked for.
    if (counts.cells > m_cellInstances.size() || counts.decorations > m_decorations.size()
        || counts.overlays > m_overlays.size() || counts.glyphs > m_glyphs.size()
        || counts.wideGlyphs > m_wideGlyphs.size()
        || counts.proportionalGlyphs > m_proportionalGlyphs.size()
        || counts.proportionalStrips > m_proportionalStrips.size()
        || counts.ligatureGlyphs > m_ligatureGlyphs.size()
        || counts.boxFills > m_boxFills.size()) {
        if (counts.cells > m_cellInstances.size())
            m_cellInstances.resize(counts.cells);
        if (counts.decorations > m_decorations.size())
            m_decorations.resize(counts.decorations);
        if (counts.overlays > m_overlays.size())
            m_overlays.resize(counts.overlays);
        if (counts.glyphs > m_glyphs.size())
            m_glyphs.resize(counts.glyphs);
        if (counts.wideGlyphs > m_wideGlyphs.size())
            m_wideGlyphs.resize(counts.wideGlyphs);
        if (counts.proportionalGlyphs > m_proportionalGlyphs.size())
            m_proportionalGlyphs.resize(counts.proportionalGlyphs);
        if (counts.proportionalStrips > m_proportionalStrips.size())
            m_proportionalStrips.resize(counts.proportionalStrips);
        if (counts.ligatureGlyphs > m_ligatureGlyphs.size())
            m_ligatureGlyphs.resize(counts.ligatureGlyphs);
        if (counts.boxFills > m_boxFills.size())
            m_boxFills.resize(counts.boxFills);
        buffers.cells = m_cellInstances.data();
        buffers.cellCapacity = m_cellInstances.size();
        buffers.decorations = m_decorations.data();
        buffers.decorationCapacity = m_decorations.size();
        buffers.overlays = m_overlays.data();
        buffers.overlayCapacity = m_overlays.size();
        buffers.glyphs = m_glyphs.data();
        buffers.glyphCapacity = m_glyphs.size();
        buffers.wideGlyphs = m_wideGlyphs.data();
        buffers.wideGlyphCapacity = m_wideGlyphs.size();
        buffers.boxFills = m_boxFills.data();
        buffers.boxFillCapacity = m_boxFills.size();
        if (m_hasColorSource) {
            if (m_colorGlyphs.size() < m_glyphs.size())
                m_colorGlyphs.resize(m_glyphs.size());
            buffers.colorGlyphs = m_colorGlyphs.data();
            buffers.colorGlyphCapacity = m_colorGlyphs.size();
        }
        if (m_hasLigatureShaping && m_ligature.isUsable()) {
            if (m_ligatureGlyphs.size() < m_glyphs.size())
                m_ligatureGlyphs.resize(m_glyphs.size());
            buffers.ligatureGlyphs = m_ligatureGlyphs.data();
            buffers.ligatureGlyphCapacity = m_ligatureGlyphs.size();
        }
        if (m_hasStripSheet && m_proportionalStrip.isUsable()) {
            buffers.proportionalStrips = m_proportionalStrips.data();
            buffers.proportionalStripCapacity = m_proportionalStrips.size();
        }
        if (m_proportional.isUsable()) {
            buffers.proportionalGlyphs = m_proportionalGlyphs.data();
            buffers.proportionalGlyphCapacity = m_proportionalGlyphs.size();
        }
        counts = buildCellInstances(local, backend, buffers);
    }
    // A count past capacity cannot survive the regrow above; the clamps are what
    // keeps a loop bound to a buffer regardless, since the alternative to an
    // over-count here is a read past the end of one.
    const std::size_t cells = counts.cells < m_cellInstances.size() ? counts.cells : m_cellInstances.size();
    const std::size_t decorations = counts.decorations < m_decorations.size() ? counts.decorations : m_decorations.size();
    const std::size_t overlays = counts.overlays < m_overlays.size() ? counts.overlays : m_overlays.size();
    const std::size_t glyphs = counts.glyphs < m_glyphs.size() ? counts.glyphs : m_glyphs.size();
    const std::size_t wideGlyphs = counts.wideGlyphs < m_wideGlyphs.size() ? counts.wideGlyphs : m_wideGlyphs.size();
    const std::size_t proportionalGlyphs = counts.proportionalGlyphs < m_proportionalGlyphs.size()
        ? counts.proportionalGlyphs
        : m_proportionalGlyphs.size();
    const std::size_t proportionalStrips = counts.proportionalStrips < m_proportionalStrips.size()
        ? counts.proportionalStrips
        : m_proportionalStrips.size();
    const std::size_t boxFills = counts.boxFills < m_boxFills.size() ? counts.boxFills : m_boxFills.size();

    // Every non-continuation cell paints its full background, so the cell pass
    // covers the grid box exactly and clearing it first writes the frame twice.
    // The gutter a viewport larger than the grid opens is painted once, after
    // the grid, image and cursor layers and before the overlays: that one fill
    // both clears the gutter and clips every grid-owned layer to the grid box,
    // so glyph, decoration and image ink never reach the padding while a
    // search pill or a caret overlay still may.
    rgbaOut.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t ligatureGlyphs = counts.ligatureGlyphs < m_ligatureGlyphs.size() ? counts.ligatureGlyphs : m_ligatureGlyphs.size();
    const std::size_t colorGlyphs = counts.colorGlyphs < m_colorGlyphs.size() ? counts.colorGlyphs : m_colorGlyphs.size();
    // The grid streams are drawn twice over: the heads, which are the grid,
    // and then the tails, which are the cursor tier (InstanceCounts::cursorTier)
    // — the cursor's cell re-emitted with the cursor's colours — as one layer
    // of its own after the images and before the overlays, so the cell's own
    // ink is not composited a second time under the tier's. Each stream is cut
    // at `count - tail`; a tail longer than what fits leaves an empty head.
    enum class StreamLayer : std::uint8_t {
        Grid,
        CursorTier,
    };
    const auto segment = [](std::size_t drawn, std::size_t tail, StreamLayer layer) {
        const std::size_t head = drawn >= tail ? drawn - tail : 0;
        return layer == StreamLayer::CursorTier ? std::pair { head, drawn } : std::pair { std::size_t { 0 }, head };
    };
    // The frame's pictures, resolved once for both bands. A picture under the
    // text changes how the grid layer inks it: the cell-baked glyphs wait for
    // the picture instead of being painted with their cells' ground, and every
    // glyph mixes over what is on the surface, the picture where one lies
    // beneath, rather than over the cell background the picture has replaced.
    resolveImageQuads(local);
    const bool hasImagesUnderText = m_imageQuadSplit > 0;
    const auto compositeStreams = [&](StreamLayer layer) {
        const InstanceCounts::CursorTier& tier = counts.cursorTier;
        const bool isTier = layer == StreamLayer::CursorTier;
        const bool isInkOverImages = !isTier && hasImagesUnderText;
        const InkGround inkGround = isInkOverImages ? InkGround::Surface : InkGround::CellBackground;
        const auto compositeCells = [&](CellPaint paint) {
            for (auto [i, end] = segment(cells, tier.cells, layer); i < end; ++i) {
                const int row = static_cast<int>(m_cellInstances[i].gridPosition.y);
                const std::uint8_t lineAttribute = (in.lineAttributes && row >= 0 && row < in.rows)
                    ? in.lineAttributes[row]
                    : lineAttributeNormal;
                compositeWithCoverage(m_cellInstances[i], { gridOriginX, gridOriginY }, rgbaOut, { width, height },
                    static_cast<int>(textScaleForLineAttribute(lineAttribute).widthScale), paint);
            }
        };
        // Cells first, then the box quads directly over the cell backgrounds —
        // the GPU arms' fill order (cell pass, then the box stream, then
        // everything else).
        compositeCells(isInkOverImages ? CellPaint::Ground : CellPaint::Whole);
        for (auto [i, end] = segment(boxFills, tier.boxFills, layer); i < end; ++i)
            compositeBoxFill(m_boxFills[i], rgbaOut, { width, height });
        // Inline images, under-text band (kitty z<0): over the cell backgrounds
        // but before every pass that inks text, the cell-baked ink included, so
        // the text reads on top. Never part of the tier, which the over band
        // and the tier's own ground cover.
        if (!isTier)
            compositeImages(local, ImageBand::Under, rgbaOut, { width, height });
        if (isInkOverImages)
            compositeCells(CellPaint::Ink);
        // Subpixel overflow glyph pass: after the cell backgrounds (it blends
        // over them), before decorations. Empty in cell-baked mode.
        for (auto [i, end] = segment(glyphs, tier.glyphs, layer); i < end; ++i)
            compositeGlyph(m_glyphs[i], m_mono, rgbaOut, { width, height }, inkGround);
        for (auto [i, end] = segment(wideGlyphs, tier.wideGlyphs, layer); i < end; ++i)
            compositeGlyph(m_wideGlyphs[i], m_wide, rgbaOut, { width, height }, inkGround);
        for (auto [i, end] = segment(proportionalGlyphs, tier.proportionalGlyphs, layer); i < end; ++i)
            compositeProportionalGlyph(m_proportionalGlyphs[i], rgbaOut, { width, height }, inkGround);
        for (auto [i, end] = segment(proportionalStrips, tier.proportionalStrips, layer); i < end; ++i)
            compositeLigatureGlyph(m_proportionalStrips[i], m_proportionalStrip, rgbaOut, { width, height }, inkGround);
        // Shaped runs and emoji after the mono overflow pass and before the
        // decorations, so an underline still crosses them: a strip's or the
        // sheet's coverage would otherwise cover the rule the pen asked for.
        // The cells a strip covers were routed off the mono pass above, so
        // the two never draw the same cell twice.
        for (auto [i, end] = segment(ligatureGlyphs, tier.ligatureGlyphs, layer); i < end; ++i)
            compositeLigatureGlyph(m_ligatureGlyphs[i], m_ligature, rgbaOut, { width, height }, inkGround);
        for (auto [i, end] = segment(colorGlyphs, tier.colorGlyphs, layer); i < end; ++i)
            compositeColorGlyph(m_colorGlyphs[i], rgbaOut, { width, height });
        for (auto [i, end] = segment(decorations, tier.decorations, layer); i < end; ++i)
            compositeDecoration(m_decorations[i], rgbaOut, { width, height });
    };
    compositeStreams(StreamLayer::Grid);
    // Inline images, over-text band (kitty z>=0, placeholder cells, sixel and
    // iTerm): after the grid so they cover the text, under the cursor and the
    // overlays. No-op when there are none.
    compositeImages(local, ImageBand::Over, rgbaOut, { width, height });
    // Rich text blocks over the grid and its images, under the cursor tier
    // and the overlays: the Metal arm's order, so a caret or a search pill
    // over a block stays visible.
    if (blocks.function)
        blocks.function(blocks.context, rgbaOut, { width, height });
    compositeStreams(StreamLayer::CursorTier);
    fillImageBorder(rgbaOut, width, height,
        { gridOriginX, gridOriginY, in.columns * m_cellWidth, in.rows * m_cellHeight }, paddingColor);
    // The window's background image, beneath everything the grid and the
    // margin drew and under the overlays, over the whole frame.
    compositeBackgroundImage(in.backgroundImage, rgbaOut, { width, height });
    // Overlays (the selection, the caret shapes, the marks) over everything
    // the grid and the cursor drew, matching the GPU draw order.
    for (std::size_t i = 0; i < overlays; ++i)
        compositeOverlay(m_overlays[i], rgbaOut, { width, height });

    widthOut = width;
    heightOut = height;
    return true;
}

} // namespace Ubiquitty::Rendering
