// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/LigatureSheet.h"

#include "Core/Rendering/ProportionalRun.h"
#include "Core/Rendering/ProportionalStripRaster.h"

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/FontFeatureSpecification.h"
#include <vector>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace Ubiquitty::Rendering {

namespace {

// How many strips the sheet holds before one is evicted. A screen shows far
// fewer distinct runs than this: a row holds at most one run per two columns,
// and successive frames repeat almost all of them, so the cache is sized for
// churn rather than for a worst case nobody produces.

// Slots per sheet row. A strip is stripSlotCells cells wide, so a row of them
// is already wide; four keeps the sheet closer to square than one column would
// and well inside any texture limit at the sizes a terminal uses.

// The 4096 every Vulkan implementation is required to support. The device's own
// limit is at least this and often larger, but the sheet is built before a
// device is in hand, so it fits to the number that needs no device to know.
constexpr int guaranteedMaxTextureDimension = 4096;

}

LigatureParticipation probeLigatureParticipation(GlyphRasterizer& rasterizer)
{
    if (!rasterizer.hasLigatureShaping())
        return LigatureParticipation::OperatorRuns;

    // Core's probe text, as code points. ASCII by construction, so the byte
    // is the code point.
    std::vector<std::uint32_t> codePoints;
    for (const char* c = ligatureHealingProbe; *c != 0; ++c)
        codePoints.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(*c)));

    // The whole probe as one run — what a strip would draw — against the same
    // characters shaped one at a time, which is the closest this seam offers
    // to the per-cell mono path's isolated mapping. A face that heals answers
    // the two differently; a face that merely ligates operators leaves this
    // letter-and-digit text alone and the two agree.
    std::vector<GlyphRasterizer::ShapedGlyph> scratch(codePoints.size() * 2);
    const int shapedCount = rasterizer.shapeTextRun(codePoints, 0u, scratch);
    if (shapedCount <= 0)
        return LigatureParticipation::OperatorRuns;
    std::vector<std::uint32_t> shaped;
    shaped.reserve(static_cast<std::size_t>(shapedCount));
    for (int i = 0; i < shapedCount; ++i)
        shaped.push_back(scratch[static_cast<std::size_t>(i)].glyph);

    std::vector<std::uint32_t> isolated;
    isolated.reserve(codePoints.size());
    for (const std::uint32_t codePoint : codePoints) {
        const std::array<std::uint32_t, 1> one = { codePoint };
        std::array<GlyphRasterizer::ShapedGlyph, 4> single;
        if (rasterizer.shapeTextRun(std::span(one), 0u, std::span(single)) != 1)
            return LigatureParticipation::OperatorRuns; // a face this seam cannot ask stays on the economy
        isolated.push_back(single[0].glyph);
    }

    return decideLigatureParticipation(shaped, isolated);
}

// The probe and the plan, decided together before any member needs them: the
// cache is constructor-only and its capacity is the plan's, and the plan is
// sized by the probe, so both ride into the delegating constructor as one
// value.
LigatureSheet::SheetDecision LigatureSheet::decideSheet(int cellWidthInPixels, int cellHeightInPixels,
    int marginXInPixels, int marginYInPixels, GlyphRasterizer* rasterizer)
{
    SheetDecision decision;
    // No shaper, no sheet: a renderer built over a face that cannot shape
    // constructs this member with a one-pixel cell as a placeholder, and a
    // plan for that cell is a strip sheet of half a million one-pixel slots --
    // eight megabytes allocated and zeroed for rows that never ligate, on
    // every such renderer. The invalid plan leaves the cache at one slot and
    // the sheet unusable, which is the truth about a face with no shaper.
    if (rasterizer == nullptr)
        return decision;
    // A face that shapes no ligature still gets a sheet when a proportional
    // face stands beside it: the strips then carry proportional runs alone,
    // and the participation is the narrow one a face without a shaper has,
    // since the row walk never asks it to ligate.
    const bool doesShape = rasterizer->hasLigatureShaping();
    if (!doesShape && !rasterizer->hasProportionalFace())
        return decision;
    decision.participation = doesShape ? probeLigatureParticipation(*rasterizer) : LigatureParticipation::OperatorRuns;
    // The face's proportional overhang rides on the vertical margin: a
    // bitmap face has no margin of its own, and its proportional glyphs
    // reach past the cell, so the slot grows to hold them and the quad's top
    // moves up by the same rows.
    const int stripMarginYInPixels = marginYInPixels + rasterizer->proportionalStripOverhangInPixels();
    decision.stripTopInPixels = -stripMarginYInPixels;
    if (cellWidthInPixels > 0 && cellHeightInPixels > 0) {
        decision.plan = planLigatureStripSheet(cellWidthInPixels, cellHeightInPixels,
            marginXInPixels, stripMarginYInPixels, decision.participation, guaranteedMaxTextureDimension);
    }
    return decision;
}

LigatureSheet::LigatureSheet(int cellWidthInPixels, int cellHeightInPixels,
    int marginXInPixels, int marginYInPixels, GlyphRasterizer* rasterizer)
    : LigatureSheet(marginXInPixels, rasterizer,
          decideSheet(cellWidthInPixels, cellHeightInPixels, marginXInPixels, marginYInPixels, rasterizer))
{
}

LigatureSheet::LigatureSheet(int marginXInPixels, GlyphRasterizer* rasterizer, SheetDecision decision)
    : m_marginXInPixels(marginXInPixels)
    , m_stripTopInPixels(decision.stripTopInPixels)
    , m_rasterizer(rasterizer)
    , m_participation(decision.participation)
    , m_cache(decision.plan.valid ? decision.plan.capacity : 1,
          recommendedHashBuckets(decision.plan.valid ? decision.plan.capacity : 1),
          decision.plan.valid ? decision.plan.columns : 1)
{
    if (!decision.plan.valid)
        return; // one strip alone exceeds the limit: leave rows unligatured
    m_slotWidthInPixels = decision.plan.slotWidthInPixels;
    m_slotHeightInPixels = decision.plan.slotHeightInPixels;
    m_columns = decision.plan.columns;
    m_capacity = decision.plan.capacity;
    m_sheetWidthInPixels = m_columns * m_slotWidthInPixels;
    m_sheetHeightInPixels = decision.plan.rows * m_slotHeightInPixels;
    m_coverage.assign(static_cast<std::size_t>(m_sheetWidthInPixels) * static_cast<std::size_t>(m_sheetHeightInPixels), 0);
    m_slotScratch.assign(static_cast<std::size_t>(m_slotWidthInPixels) * static_cast<std::size_t>(m_slotHeightInPixels), 0);
    m_slotDidSubstitute.assign(static_cast<std::size_t>(m_capacity), 0);
    m_cache.enableOwnedInkRecords(stripSlotCells, m_slotWidthInPixels);
    // A face with no shaper draws no ligature, and saying so here is what makes
    // the caller leave rows unligatured rather than draw them wrongly. A face
    // with a proportional face beside it keeps the sheet for its strips: the
    // ligature hook stays gated on the shaper by every caller, so the sheet's
    // row shaping is never asked of a face that cannot answer it.
    m_isUsable = m_rasterizer != nullptr && (m_rasterizer->hasLigatureShaping() || m_rasterizer->hasProportionalFace()) && m_capacity > 0;
}

AtlasGlyph LigatureSheet::rectForSlot(int slot) const
{
    if (slot < 0 || slot >= m_capacity || m_sheetWidthInPixels <= 0 || m_sheetHeightInPixels <= 0)
        return {};
    const int column = slot % m_columns;
    const int row = slot / m_columns;
    const float x0 = static_cast<float>(column * m_slotWidthInPixels);
    const float y0 = static_cast<float>(row * m_slotHeightInPixels);
    // The FULL slot, not the ink: LigatureRun's contract is that the rect spans
    // the slot and the builder maps a cell's atlas-pixel range onto it linearly.
    //
    // ATLAS PIXELS, not normalized fractions — the builder ADDS a slice's
    // atlas-pixel offsets to this rect's x (its own comment states the
    // addition), so a fraction here sends every slot's slices to the sheet's
    // origin and each run draws slot 0's ink: on a live session, whole prompt
    // lines wearing another line's text. The consumers normalize by texel
    // size at draw, the same as the mono rects.
    return { { x0, y0 },
        { x0 + static_cast<float>(m_slotWidthInPixels), y0 + static_cast<float>(m_slotHeightInPixels) },
        true };
}

unsigned int LigatureSheet::isolatedGlyphFor(std::uint32_t codePoint)
{
    // Memoized because it is a property of the face, asked once per code
    // point rather than once per run: a screen of paths and flags walks the
    // same characters every frame, and the shaper is the expensive half.
    const auto existing = m_isolatedGlyph.find(codePoint);
    if (existing != m_isolatedGlyph.end())
        return existing->second;
    unsigned int glyph = 0;
    const std::array<std::uint32_t, 1> one = { codePoint };
    std::array<GlyphRasterizer::ShapedGlyph, 4> single {};
    if (m_rasterizer != nullptr && m_rasterizer->shapeTextRun(std::span<const std::uint32_t>(one), 0u, std::span<GlyphRasterizer::ShapedGlyph>(single)) == 1)
        glyph = single[0].glyph;
    m_isolatedGlyph.emplace(codePoint, glyph);
    return glyph;
}

LigatureSheet::RunOutcome LigatureSheet::rasteriseRunInto(int slot, std::span<const std::uint32_t> codePoints,
    std::uint32_t weight, int leadingContext, int drawnCells, float cellAdvanceInPixels)
{
    if (!m_rasterizer || slot < 0 || slot >= m_capacity)
        return RunOutcome::Refused;

    m_shaped.assign(codePoints.size(), GlyphRasterizer::ShapedGlyph {});
    const int glyphCount = m_rasterizer->shapeTextRun(codePoints, weight, m_shaped);
    if (glyphCount <= 0)
        return RunOutcome::Refused;

    // A run the face left alone must not become a strip. The gated walk keys
    // on operator CHARACTERS, so on a face that ligates none of them — the
    // common case, since most monospace faces carry no coding ligatures — a
    // path, a flag or a prompt qualifies a whole stretch of ordinary text.
    // Drawn as a strip it is the same characters from a different raster:
    // shaped placement instead of per-cell slots, which lands ink a fraction
    // off and moves the columns beside it the moment an operator is typed.
    // Same text, two rasters, and which one a row gets decided by a character
    // somewhere else in it.
    //
    // Substitution is the whole question, and the count cannot answer it: a
    // face may ligate one-for-one, mapping each character to a piece of the
    // joined image, which leaves the glyph count and the clusters exactly as
    // an unligated run leaves them. JetBrains Mono draws `=>` that way. So
    // the comparison is against the ISOLATED mapping — the glyphs the mono
    // path would draw — which is the criterion the participation probe
    // applies to a font, applied here to one run.
    //
    // Only the healing mode is exempt: its faces substitute on purpose for
    // ordinary letters, and refusing those strips would refuse the healing
    // the mode exists for.
    if (m_participation == LigatureParticipation::OperatorRuns) {
        bool didSubstitute = static_cast<std::size_t>(glyphCount) != codePoints.size();
        for (int i = 0; !didSubstitute && i < glyphCount; ++i) {
            const GlyphRasterizer::ShapedGlyph& shaped = m_shaped[static_cast<std::size_t>(i)];
            didSubstitute = shaped.cluster != i
                || shaped.glyph != isolatedGlyphFor(codePoints[static_cast<std::size_t>(i)]);
        }
        if (!didSubstitute)
            return RunOutcome::Unligated;
    }

    // Every glyph is placed at its own CELL, computed from its cluster, rather
    // than at wherever a running pen has reached.
    //
    // The pen is what the shaper's advances describe, and they are the wrong
    // ruler here: `advanceInPixels` is whole pixels rounded from the face's own
    // advance, and a terminal's cell is a separate rounding of the same metric.
    // DejaVu Sans Mono at 16px advances 9 against a cell of 10, so a pen
    // accumulates a pixel of error per column — three cells of drift by the end
    // of a `ligatureMaxCells` chunk, on text the font never ligated.
    //
    // Placing absolutely is not a correction applied to that error, it removes
    // the accumulation the error would need: column K is at K cells, always. The
    // builder already reads the strip that way — cell K takes the slice
    // [margin + K*cell, margin + (K+1)*cell) — so a glyph anywhere else does not
    // merely look shifted, it puts its neighbour's edge in cell K.
    //
    // `cluster` is what makes this survive substitution, where an index cannot:
    // a ligature's glyphs share one cluster, so they anchor together at the
    // column the run started on and the shaper's offsets do the relative work
    // inside it, which is the one job those offsets are right for.
    //
    // The leading context falls out rather than being subtracted: its clusters
    // are below `leadingContext`, so it lands at negative x and the clip drops
    // it. It is there to have been seen by the shaper, not to be drawn.
    std::ranges::fill(m_slotScratch, std::uint8_t { 0 });
    const auto shaped = std::span<const GlyphRasterizer::ShapedGlyph>(m_shaped).first(static_cast<std::size_t>(glyphCount));

    // Drawn through one buffer whose width is the slot's, then blitted, because
    // the sheet's rows are the whole sheet wide and a run must not spill into
    // the strip beside it.
    //
    // The advances are left truthful and the offset absorbs them: the pen the
    // rasterizer keeps is part of its contract, so this cancels the pen's
    // contribution for this glyph instead of zeroing the advances to defeat it.
    std::vector<GlyphRasterizer::ShapedGlyph> placed(shaped.begin(), shaped.end());
    int penInPixels = 0;
    for (GlyphRasterizer::ShapedGlyph& glyph : placed) {
        const int column = glyph.cluster - leadingContext;
        const int cellOriginInPixels = m_marginXInPixels
            + static_cast<int>(std::lround(static_cast<double>(column) * static_cast<double>(cellAdvanceInPixels)));
        glyph.offsetXInPixels += cellOriginInPixels - penInPixels;
        penInPixels += glyph.advanceInPixels;
    }
    m_inkScratch.assign(placed.size(), GlyphRasterizer::GlyphInkExtent {});
    const bool isMeasured = m_rasterizer->rasteriseShapedRunMeasured(placed, m_slotWidthInPixels,
        m_slotHeightInPixels, m_slotScratch, m_inkScratch);
    if (!isMeasured) {
        // A refusing implementation leaves the scratch alone, but the contract
        // cannot promise that of one that failed mid-draw, so the fallback
        // starts from a clean strip either way.
        std::ranges::fill(m_slotScratch, std::uint8_t { 0 });
        if (!m_rasterizer->rasteriseShapedRun(placed, m_slotWidthInPixels, m_slotHeightInPixels, m_slotScratch))
            return RunOutcome::Refused;
    }

    if (isMeasured) {
        m_cache.beginOwnedInkRecord(slot);
        for (std::size_t i = 0; i < placed.size(); ++i) {
            // The rasterizer measured its own coverage, so the extent is the
            // ink and there is no fringe to draw past it.
            m_cache.recordShapedGlyphInk(slot, placed[i].cluster, leadingContext, drawnCells,
                m_inkScratch[i].leftInPixels, m_inkScratch[i].rightInPixels, 0);
        }
        m_cache.finalizeOwnedInkRecord(slot);
    } else {
        m_cache.clearOwnedInkRecord(slot);
    }
    // The strip's inked rows, from the coverage just drawn, so its quads can
    // stop at the ink rather than at the slot's margins.
    m_cache.recordInkRows(slot, m_slotScratch, m_slotWidthInPixels, m_slotHeightInPixels);

    const int column = slot % m_columns;
    const int row = slot / m_columns;
    const std::size_t originX = static_cast<std::size_t>(column * m_slotWidthInPixels);
    const std::size_t originY = static_cast<std::size_t>(row * m_slotHeightInPixels);
    for (int y = 0; y < m_slotHeightInPixels; ++y) {
        const std::size_t source = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_slotWidthInPixels);
        const std::size_t destination = (originY + static_cast<std::size_t>(y)) * static_cast<std::size_t>(m_sheetWidthInPixels) + originX;
        std::copy_n(m_slotScratch.begin() + static_cast<std::ptrdiff_t>(source),
            static_cast<std::size_t>(m_slotWidthInPixels), m_coverage.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    m_isDirty = true;
    return RunOutcome::Drawn;
}

bool LigatureSheet::hasProportionalFace() const
{
    return m_isUsable && m_rasterizer && m_rasterizer->hasProportionalFace();
}

int LigatureSheet::shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) const
{
    if (!hasProportionalFace() || codePoints.empty() || glyphsOut.empty())
        return 0;
    // The rasteriser fills the frontend's own struct, fraction and all; there
    // is no conversion here for a fraction to be lost in.
    return m_rasterizer->shapeProportionalRun(codePoints, face, glyphsOut);
}

int LigatureSheet::proportionalRunCells(std::span<const std::uint32_t> codePoints, float cellAdvanceInPixels) const
{
    if (!hasProportionalFace())
        return 0;
    return proportionalRunCellsInFace(*m_rasterizer, codePoints, cellAdvanceInPixels);
}

ProportionalStripPlacement LigatureSheet::proportionalStrip(std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, float penResidualInPixels, float cellAdvanceInPixels, FontFace face, std::uint32_t variant, int tileIndex,
    SizedTile size)
{
    ProportionalStripPlacement placement;
    if (!hasProportionalFace() || codePoints.empty() || widthCells < 1 || cellAdvanceInPixels <= 0.0f)
        return placement;
    ProportionalStripRequest request;
    request.widthCells = widthCells;
    request.horizontalAlign = horizontalAlign;
    request.penResidualInPixels = penResidualInPixels;
    request.cellAdvanceInPixels = cellAdvanceInPixels;
    request.marginXInPixels = static_cast<float>(m_marginXInPixels);
    request.slotWidthInPixels = m_slotWidthInPixels;
    request.slotHeightInPixels = m_slotHeightInPixels;
    request.face = face;
    request.scale = std::max<std::uint8_t>(size.scale, 1);
    request.band = size.band < request.scale ? size.band : 0;
    request.widthScale = size.widthScale == 2 ? 2 : 1;
    if (m_proportionalSource.size() != codePoints.size()
        || !std::equal(codePoints.begin(), codePoints.end(), m_proportionalSource.begin())
        || !(request == m_proportionalRequest)) {
        m_proportionalSource.assign(codePoints.begin(), codePoints.end());
        m_proportionalRequest = request;
        m_proportionalShape = shapeProportionalStrip(*m_rasterizer, codePoints, request, m_proportionalShaped);
    }
    if (m_proportionalShape.glyphCount <= 0)
        return placement;
    const ProportionalStripLayout layout = proportionalStripTile(m_proportionalShape.layout, m_slotWidthInPixels, tileIndex);
    if (layout.tileCount <= 0) {
        return placement;
    }
    ProportionalStripRequest slotRequest = request;
    slotRequest.face = isBold(face) ? FontFace::Bold : FontFace::Regular;
    slotRequest.tileIndex = tileIndex;
    const ProportionalStripSlot resolved = resolveProportionalStripSlot(m_cache,
        codePoints, slotRequest, layout, variant, m_runCodePoints);
    const int slot = resolved.slot;
    if (slot < 0)
        return placement;
    if (resolved.isFresh) {
        // The fraction is the residual this slot is keyed by, so a run at
        // another phase draws into a slot of its own rather than this one.
        if (!rasteriseProportionalStrip(*m_rasterizer, m_proportionalShaped, request, layout, m_slotScratch)) {
            m_cache.forgetKeyAtSlot(resolved.key, slot);
            return placement;
        }
        m_cache.clearOwnedInkRecord(slot);
        const int column = slot % m_columns;
        const int row = slot / m_columns;
        const std::size_t originX = static_cast<std::size_t>(column * m_slotWidthInPixels);
        const std::size_t originY = static_cast<std::size_t>(row * m_slotHeightInPixels);
        for (int y = 0; y < m_slotHeightInPixels; ++y) {
            const std::size_t source = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_slotWidthInPixels);
            const std::size_t destination = (originY + static_cast<std::size_t>(y)) * static_cast<std::size_t>(m_sheetWidthInPixels) + originX;
            std::copy_n(m_slotScratch.begin() + static_cast<std::ptrdiff_t>(source),
                static_cast<std::size_t>(m_slotWidthInPixels), m_coverage.begin() + static_cast<std::ptrdiff_t>(destination));
        }
        m_isDirty = true;
    }
    placement.strip = rectForSlot(slot);
    placement.strip.minUV.x += layout.rasterInsetInPixels;
    placement.strip.maxUV.x = placement.strip.minUV.x + layout.usedWidthInSlotPixels;
    placement.leftInPixels = layout.quadLeftInPixels;
    placement.widthInPixels = layout.usedWidthInSlotPixels;
    // At a size the slot is a slot height of the strip `scale` times as
    // tall, standing `scale` times the strip's top above the block and the
    // band's slot heights down.
    placement.topInPixels = static_cast<float>(request.scale * m_stripTopInPixels + request.band * m_slotHeightInPixels);
    placement.tileCount = layout.tileCount;
    placement.drawnWidthScale = static_cast<float>(request.scale * request.widthScale);
    placement.drawnScale = static_cast<float>(request.scale);
    return placement;
}

bool LigatureSheet::deriveShapedAnswers(int leadingContext, int drawnCells)
{
    // The ink extents were assigned one per placed glyph before the measured
    // raster, so their count is the glyph count of the run the sheet holds.
    const std::size_t glyphCount = m_inkScratch.size();
    if (glyphCount == 0 || glyphCount != m_runCodePoints.size() || drawnCells > ligatureMaxCells)
        return false;
    for (std::size_t i = 0; i < glyphCount; ++i) {
        if (m_shaped[i].cluster != static_cast<int>(i))
            return false;
        const bool isBlank = m_inkScratch[i].rightInPixels <= m_inkScratch[i].leftInPixels;
        if (isBlank && m_runCodePoints[i] != 0x20u)
            return false;
    }
    m_answerScratch.assign(static_cast<std::size_t>(drawnCells), ShapedCell {});
    for (int cell = 0; cell < drawnCells; ++cell) {
        const std::size_t glyph = static_cast<std::size_t>(leadingContext + cell);
        if (glyph >= glyphCount || m_shaped[glyph].glyph == 0u)
            return false;
        ShapedCell answer;
        answer.glyphID = m_shaped[glyph].glyph;
        answer.coveredCells = 1;
        m_answerScratch[static_cast<std::size_t>(cell)] = answer;
    }
    return true;
}

void LigatureSheet::shapeRow(LigatureRowSpan row, float cellAdvanceInPixels,
    LigatureRunStream runs)
{
    // Every entry, not only the covered ones: the caller reuses one span across
    // rows and frames, so a column this row leaves alone would otherwise still
    // hold the previous row's run and draw it.
    for (LigatureRun& run : runs)
        run.length = 0;
    if (!m_isUsable || row.empty() || runs.empty())
        return;

    const int columns = static_cast<int>(std::min(row.size(), runs.size()));
    // SplitByFace: this sheet shapes through `GlyphRasterizer::shapeTextRun`,
    // which takes one weight for the whole call, so a run carrying two faces
    // would be drawn wholly in one of them.
    identifyLigatureRuns(row.data(), columns, m_participation, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) {
            if (candidate.startColumn < 0 || candidate.length <= 0 || candidate.startColumn >= columns)
                return;

            // Context and drawn cells together, in shaping order. The context is
            // what lets the font's rules see across a chunk split; it is shaped and
            // then clipped away.
            const int first = candidate.startColumn - candidate.leftContext;
            const int total = candidate.leftContext + candidate.length + candidate.rightContext;
            // The row edge shapes as a space, per the flank predicates: an
            // OpenType backtrack cannot match past the run's first glyph, so
            // a word at the margin would otherwise heal differently from the
            // same word after a real space. A glyph neighbour still shapes as
            // itself.
            const bool shouldStandLeftSpace = ligatureRunFlanksRowStart(candidate);
            const bool shouldStandRightSpace = ligatureRunFlanksRowEnd(candidate, columns);
            m_runCodePoints.clear();
            m_runCodePoints.reserve(static_cast<std::size_t>(total) + 2);
            if (shouldStandLeftSpace)
                m_runCodePoints.push_back(0x20u);
            for (int i = 0; i < total; ++i) {
                const int column = first + i;
                if (column < 0 || column >= columns)
                    return; // the candidate's context ran off the row: leave it unligatured
                m_runCodePoints.push_back(row[static_cast<std::size_t>(column)].codePoint);
            }
            if (shouldStandRightSpace)
                m_runCodePoints.push_back(0x20u);
            const int shapedLeftContext = candidate.leftContext + (shouldStandLeftSpace ? 1 : 0);
            const std::uint32_t weight = (row[static_cast<std::size_t>(candidate.startColumn)].shape & ligatureShapeBold) != 0 ? 7u : 0u;

            // The run's answers by identity, asked for before the strip: keyed
            // by the run's words, its context, its length and its face, never
            // by paint, and held across frames, so a run shaped once is
            // answered from then on. A `.notdef` or a joined cluster is a run
            // with no answer, and that verdict is held too, so the strip alone
            // is not re-derived every frame.
            m_answerKey.assign(m_runCodePoints.begin(), m_runCodePoints.end());
            m_answerKey.push_back(static_cast<std::uint32_t>(shapedLeftContext));
            m_answerKey.push_back(static_cast<std::uint32_t>(candidate.length));
            m_answerKey.push_back(weight);
            ShapedCellSpan answers = m_answers.lookup(m_answerKey, 0u);
            const bool isKnownUnanswered = answers.empty() && m_answers.holdsNoAnswer(m_answerKey, 0u);

            // Content-verified rather than keyed on the fold alone: two chunks of
            // one long stretch carry the same code points and differ only in their
            // context, so a lossy key cannot separate them and both can be live in
            // one frame.
            const std::uint32_t key = coverageClusterKey(m_runCodePoints.data(),
                static_cast<int>(m_runCodePoints.size()), 0u, FontFace::Regular);
            bool isFresh = false;
            const int slot = m_cache.slotForKeyVerified(key, m_runCodePoints.data(),
                static_cast<int>(m_runCodePoints.size()), &isFresh);
            if (slot < 0)
                return; // the sheet is full of strips this frame already placed

            if (isFresh) {
                const RunOutcome outcome = rasteriseRunInto(slot, m_runCodePoints, weight,
                    shapedLeftContext, candidate.length, cellAdvanceInPixels);
                if (outcome == RunOutcome::Refused) {
                    // A slot taken and not filled would hand later frames the
                    // previous tenant's ink under this run's name.
                    m_cache.forgetKeyAtSlot(key, slot);
                    return;
                }
                m_slotDidSubstitute[static_cast<std::size_t>(slot)] = outcome == RunOutcome::Drawn ? 1u : 0u;
                // The answers come out of the strip's own shaping and measured
                // raster, so a fresh strip is the one moment they can be derived
                // without a second pass.
                if (outcome == RunOutcome::Drawn && answers.empty() && !isKnownUnanswered) {
                    if (m_cache.slotHasOwnedInk(slot) && deriveShapedAnswers(shapedLeftContext, candidate.length))
                        answers = m_answers.publish(m_answerKey, 0u, m_answerScratch);
                    else
                        m_answers.publish(m_answerKey, 0u, {});
                }
            }
            // The slot's verdict rides with it: a run whose face changed nothing
            // publishes no entry, so every column it covers takes the mono path
            // it would have taken had the row held no operator at all.
            if (m_slotDidSubstitute[static_cast<std::size_t>(slot)] == 0u)
                return;

            LigatureRun placedRun;
            placedRun.strip = rectForSlot(slot);
            placedRun.slotWidthInAtlasPixels = static_cast<float>(m_slotWidthInPixels);
            placedRun.usedWidthInAtlasPixels = ligatureStripUsedWidthInAtlasPixels(candidate.length,
                cellAdvanceInPixels, m_marginXInPixels, static_cast<float>(m_slotWidthInPixels));
            const LigatureStripContextInsets insets = ligatureStripContextInsets(
                candidate.leftContext, candidate.rightContext, candidate.length, cellAdvanceInPixels,
                m_marginXInPixels, placedRun.usedWidthInAtlasPixels);
            placedRun.contextInsetLeftInAtlasPixels = insets.leftInAtlasPixels;
            placedRun.contextInsetRightInAtlasPixels = insets.rightInAtlasPixels;
            placedRun.startColumn = candidate.startColumn;
            placedRun.length = candidate.length;
            // Into each column the run covers, and not the first one alone. The
            // consumer asks every column for its own entry
            // (`buildCellInstances` indexes `ligatureRuns[column]`) and
            // takes the slice from `column - startColumn`, so a run published only
            // at its start leaves the rest reading a zero-length entry: they stay on
            // the mono path, one cell of the strip is drawn, and the picture is the
            // unligatured one. A comparison of pictures over a font with no
            // ligatures cannot see that, and every font in this container but the
            // bundled one is such a font.
            const bool isSlotInkMeasured = m_cache.slotHasOwnedInk(slot);
            for (int covered = 0; covered < candidate.length; ++covered) {
                const int column = candidate.startColumn + covered;
                if (column >= columns)
                    break;
                LigatureRun columnRun = placedRun;
                if (isSlotInkMeasured) {
                    attachOwnedInk(columnRun, m_cache.ownedInkForSlot(slot, covered));
                }
                attachInkRows(columnRun, m_cache.inkRowsForSlot(slot));
                // Both answers travel: a backend that draws by identity takes the
                // bounded per-glyph path, and one that cannot keeps the strip
                // beside it, so the picture is the same either way.
                if (static_cast<std::size_t>(covered) < answers.size()) {
                    columnRun.shaped = answers[static_cast<std::size_t>(covered)];
                    columnRun.hasShapedGlyph = true;
                }
                runs[static_cast<std::size_t>(column)] = columnRun;
            }
        });
}

} // namespace Ubiquitty::Rendering
