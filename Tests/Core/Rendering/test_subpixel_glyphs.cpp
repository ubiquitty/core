// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_subpixel_glyphs — pins the opt-in subpixel mono glyph pass the
// shared buildCellInstances emits when BuildInputs.subpixelGlyphs is set: a
// plain mono inked cell becomes a pixel-snapped GlyphInstance at the phase of
// its fractional pen X, the slot is faulted at that phase, and the CellInstance
// carries background only. A cell carrying combining marks belongs to that
// population — only the atlas call differs. Wide and blank cells keep the
// cell-baked path. The Metal frame encoder composites the two passes.
//
// One case here is about the other door into the same stream: a super/subscript
// cell rides it whatever the backend, because the shrink is a box and a cell
// quad has none to give.

#include "Core/Rendering/CellInstanceBuilder.h"

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/SubpixelPhase.h"
#include "StrictAtlas.h"

#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

namespace {
// Phase-aware fake atlas: records every glyph the builder faults, with the phase
// it was faulted at, so a case can ask which phase was requested. A space (0x20)
// has no coverage (blank); a wide glyph returns a fixed UV.
//
// The phase is read back from the RECORD, not decoded out of a UV the fake
// synthesised. A fake that encodes its answer in its own return value proves
// only that the value survived the round trip; the cases here want the key the
// builder derived, so they recompute it and probe the same cache.
struct PhaseAtlas {
    // The mono cache the builder probes. A fake could return an empty binding and
    // compile, but CellInstanceBuilder gates the whole mono path on `mono.cache &&`
    // — so an empty one switches off the behaviour these cases exist to check
    // while every assertion still passes.
    //
    // The geometry tiles a 256x512 atlas exactly:
    // 32 columns of 8px slots is 256 wide, 32 rows of 16px is 512 tall, so the
    // 1024 slots tile it exactly. glyphWidth = slotWidth - 2*marginX is the
    // invariant GlyphSlotGeometry::atlasRectForSlot asserts, and that assert runs here.
    Ubiquitty::Rendering::GlyphSlotCache monoCache {
        1024, Ubiquitty::Rendering::recommendedHashBuckets(1024), 32
    };
    // One ink box per slot, default-unmeasured. Handed on the binding, which
    // moves every case in this file onto the table-present branch: entries the
    // fake never measures must behave exactly as no table at all, and the
    // cases that pin slot-sized quads are what hold that.
    std::vector<Ubiquitty::Rendering::GlyphInkBox> inkBoxes { 1024 };
    Ubiquitty::Rendering::MonoAtlasBinding bindMonoAtlas()
    {
        Ubiquitty::Rendering::MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.inkBoxForSlot = inkBoxes.data();
        binding.weight = 0u;
        return binding;
    }
    // The builder probes the slot cache itself and enters the backend only on a
    // miss, so this is where a mono glyph request arrives — once per miss, not
    // once per cell. The cases below count these, so a repeated glyph at a
    // repeated phase is one entry, not two.
    bool rasteriseMonoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t phase, int)
    {
        monoCalls.push_back({ codePoint, phase });
        return true;
    }

    int frames = 0;
    struct Call {
        std::uint32_t codePoint;
        std::uint32_t phase;
    };
    std::vector<Call> monoCalls;
    void beginFrame() { ++frames; }
    // Recorded APART from monoCalls, and asserted empty, rather than folded in
    // with the rasterisations. The mono path is the slot cache and does not run
    // through here; were a build to route it back, a shared log would absorb
    // that as an off-by-one in a count and the message would name a number
    // instead of the path that moved.
    std::vector<Call> viaMonoGlyph;
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t phase)
    {
        viaMonoGlyph.push_back({ codePoint, phase });
        if (codePoint == 0x20u)
            return {};
        const float u = static_cast<float>(phase) * 0.1f;
        return { { u, 0.0f }, { u + 0.05f, 0.05f }, true };
    }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return { { 0.5f, 0.5f }, { 0.6f, 0.6f }, true }; }
    // Overridden rather than left to the base class, whose default answers with
    // the base code point ALONE through monoGlyph. That default is a real glyph
    // and a plausible UV, so a builder that dropped the marks would satisfy
    // every assertion about counts, streams and phases while drawing `e` for
    // `é`. Recording the whole sequence is what makes the marks assertable.
    struct ClusterCall {
        std::vector<std::uint32_t> codePoints;
        std::uint32_t phase;
    };
    std::vector<ClusterCall> clusterCalls;
    AtlasGlyph clusterGlyph(std::span<const std::uint32_t> codePoints, FontFace, std::uint32_t, std::uint32_t phase)
    {
        clusterCalls.push_back({ { codePoints.begin(), codePoints.end() }, phase });
        const float u = static_cast<float>(phase) * 0.1f;
        return { { u, 0.5f }, { u + 0.05f, 0.55f }, true };
    }
    // An absent glyph is the correct answer for a fake with no double-height
    // sheet, and it is indistinguishable from a builder that asked the wrong
    // question: buildCellInstances tries doubleGlyph first and falls through on
    // an empty one, so a cell wrongly routed here still renders and still
    // passes. Recorded so a case can require that the fall-through did not
    // happen, rather than being satisfied by the value it produced.
    std::vector<std::uint32_t> reachedDoubleGlyph;
    AtlasGlyph doubleGlyph(std::uint32_t codePoint, FontFace, DoubleRegion)
    {
        reachedDoubleGlyph.push_back(codePoint);
        return {};
    }
};

void setCell(std::vector<Ubiquitty::Cell>& grid, int columns, int row, int column, uint32_t ch, uint16_t attributes, uint16_t foregroundIndex)
{
    Ubiquitty::Cell& c = grid[static_cast<std::size_t>(row) * columns + column];
    c.codePoint = ch;
    c.pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(attributes);
    c.pen.foregroundIndex = foregroundIndex;
    c.pen.backgroundIndex = colorDefault;
    c.pen.underlineIndex = colorDefault;
}

BuildInputs baseInputs(const std::vector<Ubiquitty::Cell>& grid, const TermPalette& pal, int rows, int columns, float cellWidth)
{
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;
    in.subpixelGlyphs = true; // margin-free defaults ⇒ origin=(snappedPenX,rowTop), size=(cell)
    return in;
}
} // namespace

TEST(subpixelGlyphsCases)
{

    TermPalette pal;
    pal.seedDefaults(); // ansi[1] = 205,0,0; default bg 16,16,20

    // --- Basic integer-width case: phase 0, bg-only cell, fg/bg + UV pinned. ---
    {
        const int rows = 1, columns = 3;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
        Ubiquitty::Cell blank {};
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        for (auto& c : grid)
            c = blank;
        setCell(grid, columns, 0, 0, 0x41u, 0, /*foreground ansi*/ 1); // 'A' red
        setCell(grid, columns, 0, 1, 0x20u, 0, colorDefault); // space (blank, excluded)
        setCell(grid, columns, 0, 2, 0x42u, 0, colorDefault); // 'B' default fg

        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, /*cellWidth*/ 8.0f);

        std::vector<CellInstance> cellsOut(8);
        std::vector<DecorationInstance> decoOut(64);
        std::vector<GlyphInstance> glyphsOut(8);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

        // 3 cells emitted; 2 plain-mono inked glyphs ('A','B'); space has no glyph.
        CHECK(counts.cells == 3);
        CHECK(counts.glyphs == 2);

        // 'A' cell (index 0) is background-only.
        CHECK(approx(cellsOut[0].atlasTopLeft.x, cellsOut[0].atlasBottomRight.x) && approx(cellsOut[0].atlasTopLeft.y, cellsOut[0].atlasBottomRight.y));

        // 'A' GlyphInstance: penX = 0 → phase 0, snappedPenX 0; origin (0,0),
        // size (cellWidth, cellHeight); UV is the phase-0 slot; fg red, bg default.
        const GlyphInstance& gA = glyphsOut[0];
        CHECK(approx(gA.originInPixels.x, 0.0f) && approx(gA.originInPixels.y, 0.0f));
        CHECK(approx(gA.sizeInPixels.x, 8.0f) && approx(gA.sizeInPixels.y, 16.0f));
        // The quad samples the slot the cache holds for 'A' at phase 0, derived
        // by recomputing the builder's key and probing the same cache. The probe
        // must HIT: a miss means the builder faulted some other key, which is
        // the failure worth catching, and it also rules out a builder that hands
        // every glyph slot 0. A UV literal here asserted only that the fake's own
        // encoding round-tripped, which is a fact about the fake.
        const MonoAtlasBinding bound = atlas.bindMonoAtlas();
        bool isFreshSlot = false;
        const int slotA = atlas.monoCache.slotForKey(
            monoGlyphKey(0x41u, bound.weight, FontFace::Regular, /*phase*/ 0u), &isFreshSlot);
        CHECK_MSG(!isFreshSlot, "the builder faulted 'A' at phase 0, so probing that key again hits");
        const GlyphUV slotAUV = bound.geometry.atlasRectForBuilder(slotA, bound.slotCapacity);
        CHECK(approx(gA.atlasTopLeft.x, slotAUV.minU) && approx(gA.atlasTopLeft.y, slotAUV.minV));
        CHECK(approx(gA.atlasBottomRight.x, slotAUV.maxU) && approx(gA.atlasBottomRight.y, slotAUV.maxV));
        CHECK(approx(gA.foreground.x, 205.0f / 255.0f) && approx(gA.foreground.y, 0.0f) && approx(gA.foreground.z, 0.0f) && approx(gA.foreground.w, 1.0f));
        CHECK(approx(gA.background.x, 16.0f / 255.0f) && approx(gA.background.z, 20.0f / 255.0f));

        // 'B' GlyphInstance: penX = 16 → phase 0, snappedPenX 16.
        const GlyphInstance& gB = glyphsOut[1];
        CHECK(approx(gB.originInPixels.x, 16.0f));

        // The atlas was asked to rasterise at phase 0 for 'A' and for 'B', and
        // for nothing else. Two calls, not three: the space never reaches the
        // atlas, because the builder excludes ' ' from the mono path by code
        // point. A blank cell costs no slot and no raster — it is not a glyph
        // that was fetched and came back empty.
        //
        // Through rasteriseMonoGlyph, not monoGlyph: buildCellInstances probes
        // the slot cache itself and only enters the backend when the probe
        // misses, so the per-glyph entry point is not on this path.
        //
        // REQUIRE rather than CHECK because the two lines below index this
        // vector. A failed size CHECK let them read past the end, and the run
        // reported a FATAL signal naming neither the assertion nor the cause.
        REQUIRE(atlas.monoCalls.size() == 2);
        CHECK(atlas.monoCalls[0].codePoint == 0x41u && atlas.monoCalls[0].phase == 0u);
        CHECK(atlas.monoCalls[1].codePoint == 0x42u && atlas.monoCalls[1].phase == 0u);
        CHECK_MSG(atlas.viaMonoGlyph.empty(), "the mono path is the slot cache; monoGlyph is not on it");
        // Plain mono cells are not double-height, so the double sheet is never
        // asked. Without this the fake's empty answer would carry a wrongly
        // routed cell back onto the mono path and every assertion above would
        // still hold.
        CHECK_MSG(atlas.reachedDoubleGlyph.empty(), "a plain mono cell does not consult the double sheet");
    }

    // --- Fractional pen X selects a non-zero phase + the matching slot. ---
    {
        const int rows = 1, columns = 2;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
        Ubiquitty::Cell blank {};
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        for (auto& c : grid)
            c = blank;
        setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault); // col 0: penX 0   → phase 0
        setCell(grid, columns, 0, 1, 0x42u, 0, colorDefault); // col 1: penX 8.5 → phase 4

        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, /*cellWidth*/ 8.5f);
        std::vector<CellInstance> cellsOut(4);
        std::vector<DecorationInstance> decoOut(32);
        std::vector<GlyphInstance> glyphsOut(4);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

        // col 1: penX = 8.5 → phaseForPen(8.5,8) = { snappedPenX 8, phase 4 }.
        const SubpixelPhase expected = phaseForPen(8.5f, subpixelPhaseCount);
        CHECK(expected.snappedPenX == 8 && expected.phase == 4);
        CHECK(atlas.monoCalls.size() == 2);
        CHECK(atlas.monoCalls[1].phase == 4u);
        const GlyphInstance& g1 = glyphsOut[1];
        CHECK(approx(g1.originInPixels.x, 8.0f)); // snapped, not 8.5

        // The quad samples the slot the cache holds for 'B' at PHASE 4. Probing
        // the phase-4 key hits, because the builder faulted it.
        const MonoAtlasBinding bound = atlas.bindMonoAtlas();
        bool isFreshSlot = false;
        const int slotAtPhase4 = atlas.monoCache.slotForKey(
            monoGlyphKey(0x42u, bound.weight, FontFace::Regular, /*phase*/ 4u), &isFreshSlot);
        CHECK_MSG(!isFreshSlot, "the builder faulted 'B' at phase 4, so probing that key again hits");
        const GlyphUV slotUV = bound.geometry.atlasRectForBuilder(slotAtPhase4, bound.slotCapacity);
        CHECK(approx(g1.atlasTopLeft.x, slotUV.minU) && approx(g1.atlasTopLeft.y, slotUV.minV));

        // And the same code point at phase 0 was never faulted. This is the
        // clause that separates "faulted at the right phase" from "faulted at
        // all": without it, a builder that ignored the phase entirely and keyed
        // on the code point alone would satisfy every assertion above.
        bool isFreshAtPhaseZero = false;
        atlas.monoCache.slotForKey(monoGlyphKey(0x42u, bound.weight, FontFace::Regular, /*phase*/ 0u), &isFreshAtPhaseZero);
        CHECK_MSG(isFreshAtPhaseZero, "'B' at phase 0 was never faulted, so the phase is part of the key");
    }

    // --- Exclusion: a wide cell keeps the cell-baked path. -------------------
    {
        const int rows = 1, columns = 3;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
        Ubiquitty::Cell blank {};
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        for (auto& c : grid)
            c = blank;
        setCell(grid, columns, 0, 0, 0x4E00u, static_cast<uint16_t>(Ubiquitty::VT::Attribute::Wide), colorDefault); // wide CJK lead
        setCell(grid, columns, 0, 1, wideContinuationSentinelCodePoint, static_cast<uint16_t>(Ubiquitty::VT::Attribute::WideContinuation), colorDefault);
        setCell(grid, columns, 0, 2, 0x20u, 0, colorDefault); // space: no coverage

        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, 8.0f);
        std::vector<CellInstance> cellsOut(4);
        std::vector<DecorationInstance> decoOut(32);
        std::vector<GlyphInstance> glyphsOut(4);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

        // Wide lead is cell-baked, continuation skipped, space has no glyph.
        CHECK(counts.glyphs == 0);
        CHECK(counts.cells == 2); // wide lead + space
        // The wide cell kept its baked glyph UV (the fixed wide UV, not blank).
        CHECK(!approx(cellsOut[0].atlasTopLeft.x, cellsOut[0].atlasBottomRight.x));
    }

    // --- A combining cluster takes the glyph pass at its own phase. ----------
    //
    // The phase is a property of the whole row: a
    // cluster on a subpixel row reaches the same arm every other mono cell
    // does and asks for its slot at `phase`. Holding a cluster back to phase 0
    // would place an accented letter up to a phase-width left of the plain
    // letters beside it in the same word — which is why the case asserts the
    // cluster's phase against its NEIGHBOUR'S rather than against a constant.
    // A row of clusters alone cannot show that; a phase is only wrong relative
    // to what the cells around it did.
    {
        const int rows = 1, columns = 3;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
        Ubiquitty::Cell blank {};
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        for (auto& c : grid)
            c = blank;
        // Cell width 8.5 so consecutive columns land on different phases: at a
        // whole-pixel width every column shares phase 0 and the case would
        // agree for the one reason it is not testing.
        const float cellWidth = 8.5f;
        setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault); // 'A', bare
        setCell(grid, columns, 0, 1, 0x65u, 0, colorDefault); // 'e' + one mark
        setCell(grid, columns, 0, 2, 0x42u, 0, colorDefault); // 'B', bare
        grid[1].setCombiningLength(1);
        grid[1].combining[0] = 0x301u; // COMBINING ACUTE ACCENT

        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, cellWidth);
        // Without a resolver the builder falls back to the base code point, and
        // the marks vanish while every count stays right. Supplied here so the
        // case is about routing rather than about the fallback.
        in.clusterResolver = [](const Ubiquitty::Cell* cell, std::span<std::uint32_t> codePoints, void*) {
            std::size_t n = 0;
            if (!codePoints.empty())
                codePoints[n++] = cell->codePoint;
            for (int i = 0; i < cell->combiningLength() && n < codePoints.size(); ++i)
                codePoints[n++] = cell->combining[i];
            return static_cast<int>(n);
        };
        std::vector<CellInstance> cellsOut(4);
        std::vector<DecorationInstance> decoOut(32);
        std::vector<GlyphInstance> glyphsOut(4);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

        // All three ride the glyph pass; none keeps a baked UV.
        CHECK_MSG(counts.glyphs == 3, "the cluster joins its neighbours in the glyph stream");
        CHECK(counts.cells == 3);
        for (int column = 0; column < columns; ++column)
            CHECK_MSG(approx(cellsOut[static_cast<std::size_t>(column)].atlasTopLeft.x,
                          cellsOut[static_cast<std::size_t>(column)].atlasBottomRight.x),
                "a cell whose glyph rides the pass carries background only");

        // The cluster was fetched as a cluster, with its mark — not as a bare
        // base through the mono cache.
        CHECK_MSG(atlas.clusterCalls.size() == 1, "one cluster fetch, for the one cluster cell");
        CHECK(atlas.clusterCalls[0].codePoints.size() == 2);
        CHECK(atlas.clusterCalls[0].codePoints[0] == 0x65u);
        CHECK_MSG(atlas.clusterCalls[0].codePoints[1] == 0x301u, "the mark reached the atlas");

        // The phase is the one the cell's own pen X yields — the same rule the
        // bare neighbours follow, which is what keeps the word aligned.
        const SubpixelPhase clusterExpected = phaseForPen(cellWidth, subpixelPhaseCount);
        CHECK_MSG(atlas.clusterCalls[0].phase == static_cast<std::uint32_t>(clusterExpected.phase),
            "the cluster took the phase of its own pen X");
        // ... and it is not phase 0, so the assertion above can fail.
        CHECK_MSG(clusterExpected.phase != 0, "the case would prove nothing at a phase-0 column");
        // The bare neighbours were faulted at their own phases through the mono
        // cache, so a build that sent the cluster to phase 0 would put it out of
        // step with both.
        CHECK(atlas.monoCalls.size() == 2);
        CHECK(atlas.monoCalls[0].codePoint == 0x41u && atlas.monoCalls[0].phase == static_cast<std::uint32_t>(phaseForPen(0.0f, subpixelPhaseCount).phase));
        CHECK(atlas.monoCalls[1].codePoint == 0x42u && atlas.monoCalls[1].phase == static_cast<std::uint32_t>(phaseForPen(2.0f * cellWidth, subpixelPhaseCount).phase));

        // Control: with the subpixel pass off, the cluster goes back to the
        // cell-baked path at phase 0 like every other cell. Without this the
        // case cannot tell "routed by the subpixel gate" from "routed always".
        PhaseAtlas plainAtlas;
        BuildInputs plain = in;
        plain.subpixelGlyphs = false;
        std::vector<CellInstance> plainCells(4);
        InstanceBuffers plainBuffers;
        plainBuffers.cells = plainCells.data();
        plainBuffers.cellCapacity = plainCells.size();
        plainBuffers.decorations = decoOut.data();
        plainBuffers.decorationCapacity = decoOut.size();
        const InstanceCounts plainCounts = buildCellInstances(plain, UbiquittyTest::glyphBackendFor(plainAtlas), plainBuffers);
        CHECK_MSG(plainCounts.glyphs == 0, "no glyph pass without the opt-in");
        CHECK(plainAtlas.clusterCalls.size() == 1);
        CHECK_MSG(plainAtlas.clusterCalls[0].phase == 0u, "a non-subpixel row draws its cluster at phase 0");
        CHECK_MSG(!approx(plainCells[1].atlasTopLeft.x, plainCells[1].atlasBottomRight.x),
            "and the cluster's glyph is baked onto the cell");
    }

    // --- Opt-out: subpixelGlyphs=false ⇒ no glyph pass, cell-baked UV. ---
    {
        const int rows = 1, columns = 1;
        std::vector<Ubiquitty::Cell> grid(1);
        grid[0].pen.foregroundIndex = colorDefault;
        grid[0].pen.backgroundIndex = colorDefault;
        grid[0].pen.underlineIndex = colorDefault;
        setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault);

        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, 8.0f);
        in.subpixelGlyphs = false;
        std::vector<CellInstance> cellsOut(2);
        std::vector<DecorationInstance> decoOut(8);
        std::vector<GlyphInstance> glyphsOut(2);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

        CHECK(counts.glyphs == 0); // no subpixel pass
        CHECK(atlas.monoCalls.size() == 1 && atlas.monoCalls[0].phase == 0u); // phase 0
        // Glyph baked into the CellInstance UV (not background-only).
        CHECK(!approx(cellsOut[0].atlasTopLeft.x, cellsOut[0].atlasBottomRight.x));
    }

    // --- Glyph margin: the slot quad expands the UV by the margin converted to
    // UV units, and the origin shifts back by the margin in pixels, so a glyph
    // wider than its cell box samples the slot's margin instead of clipping.
    // Every in-tree caller leaves the margin at zero, where the expansion is a
    // multiply by zero and the UV comes out identical either way — so the
    // non-zero case is the only one that pins the arithmetic. ---
    {
        const int rows = 1, columns = 1;
        std::vector<Ubiquitty::Cell> grid(1);
        Ubiquitty::Cell blank {};
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        grid[0] = blank;
        setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault); // 'A', phase 0

        auto buildWithMargin = [&](float marginX, float marginY) {
            PhaseAtlas atlas;
            BuildInputs in = baseInputs(grid, pal, rows, columns, /*cellWidth*/ 8.0f);
            in.glyphMarginInPixels.width = marginX;
            in.glyphMarginInPixels.height = marginY;
            std::vector<CellInstance> cellsOut(4);
            std::vector<DecorationInstance> decoOut(32);
            std::vector<GlyphInstance> glyphsOut(4);
            InstanceBuffers buffers;
            buffers.cells = cellsOut.data();
            buffers.cellCapacity = cellsOut.size();
            buffers.decorations = decoOut.data();
            buffers.decorationCapacity = decoOut.size();
            buffers.glyphs = glyphsOut.data();
            buffers.glyphCapacity = glyphsOut.size();
            const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
            CHECK(counts.glyphs == 1);
            return glyphsOut[0];
        };

        // Margin-free: the quad sits on the cell, and its UV is the reference the
        // margined build is measured against.
        const GlyphInstance bare = buildWithMargin(0.0f, 0.0f);
        CHECK(approx(bare.originInPixels.x, 0.0f) && approx(bare.originInPixels.y, 0.0f));

        // 4px across, 2px down: every atlas edge moves out by the margin and
        // the origin moves back by it, so the wider quad still covers the cell
        // box. The rect is atlas pixels, so the expansion is the margin itself.
        //
        // Measured as a DELTA from the margin-free UV. The subject here is the
        // expansion arithmetic; absolute edges would additionally pin whichever
        // slot the cache handed out, which belongs to the phase cases above and
        // would fail here for a reason that has nothing to do with margins.
        const GlyphInstance margined = buildWithMargin(4.0f, 2.0f);
        CHECK(approx(margined.atlasTopLeft.x, bare.atlasTopLeft.x - 4.0f));
        CHECK(approx(margined.atlasTopLeft.y, bare.atlasTopLeft.y - 2.0f));
        CHECK(approx(margined.atlasBottomRight.x, bare.atlasBottomRight.x + 4.0f));
        CHECK(approx(margined.atlasBottomRight.y, bare.atlasBottomRight.y + 2.0f));
        CHECK(approx(margined.originInPixels.x, -4.0f) && approx(margined.originInPixels.y, -2.0f));
    }
}

/// ---- A glyph tracks its cell, not the size it was rasterised at ----
//
// Slot geometry is atlas-native. The cell it draws into can be narrower
// (DECCOLM) or a different height (resize, font size), and the glyph has to
// stretch with its background or text drifts away from the background and the
// cursor it is supposed to sit on.
//
// The reference is the font's un-rounded advance, not the slot box: the slot is
// a ceil of it, so referencing the slot shrinks a glyph already at its natural
// size.
// A backend that measured its raster hands the box through the binding, and
// the quad shrinks to it. The anchor is the UV mapping's, not the physical
// slot's: the slot quad maps its left edge to `minUV - in.margin`, so the ink
// offset is measured from there — which this fixture separates from the slot
// origin by running the binding at margin 1 and the BuildInputs at margin 0.
// A dark theme with a run of dark text on a light background: the mask is
// chosen per CELL by that cell's own foreground luma, never by the theme. The
// same glyph in two foreground tiers faults two slots, each keyed and
// rasterised under its tier, and the neighbour in the light run gets the
// thin tier-0 raster a native draw would give black text — not the white
// theme foreground's tier-4 one. With tiers off every cell keys tier 0, as the
// backends that smooth nothing do.
TEST(aCellKeysItsOwnForegroundTier)
{
    using Ubiquitty::Rendering::glyphSlotKey;
    using Ubiquitty::Rendering::makeRasterVariant;
    using Ubiquitty::Rendering::monoGlyphKey;
    using Ubiquitty::Rendering::variantTier;

    TermPalette pal;
    pal.seedDefaults();
    // Palette slot 15 is bright white, slot 0 black; the theme's default
    // foreground is light on a dark default background.
    pal.indexed[15] = { 0xFFFF, 0xFFFF, 0xFFFF };
    pal.indexed[0] = { 0, 0, 0 };
    pal.indexed[7] = { 0xFFFF, 0xFFFF, 0xFFFF }; // a light background for the dark run
    pal.defaultForeground = { 0xFFFF, 0xFFFF, 0xFFFF };
    pal.defaultBackground = { 0x1010, 0x1010, 0x1414 };

    const int rows = 1, columns = 2;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid)
        c = Ubiquitty::defaultBlankCell;
    setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault); // 'A', white on the dark default
    setCell(grid, columns, 0, 1, 0x41u, 0, /*black*/ 0); // 'A', black on white
    grid[1].pen.backgroundIndex = 7;

    auto build = [&](bool hasRasterTiers, PhaseAtlas& atlas) { // bool-ok: the BuildInputs flag, named as it is there
        BuildInputs in = baseInputs(grid, pal, rows, columns, /*cellWidth*/ 8.0f);
        in.hasRasterTiers = hasRasterTiers;
        std::vector<CellInstance> cellsOut(8);
        std::vector<DecorationInstance> decoOut(64);
        std::vector<GlyphInstance> glyphsOut(8);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    };

    {
        PhaseAtlas atlas;
        const InstanceCounts counts = build(/*hasRasterTiers=*/true, atlas);
        CHECK(counts.glyphs == 2);
        // Two rasterisations of one code point, one per tier: the white cell
        // at tier 4, the black cell at tier 0.
        CHECK(atlas.monoCalls.size() == 2);
        CHECK(atlas.monoCalls[0].codePoint == 0x41u && variantTier(atlas.monoCalls[0].phase) == 4u);
        CHECK(atlas.monoCalls[1].codePoint == 0x41u && variantTier(atlas.monoCalls[1].phase) == 0u);
        // Both slots are resident under their own 64-bit keys.
        bool isFresh = true;
        CHECK(atlas.monoCache.slotForKey(glyphSlotKey(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u), makeRasterVariant(0u, 4u)), &isFresh) >= 0 && !isFresh);
        isFresh = true;
        CHECK(atlas.monoCache.slotForKey(glyphSlotKey(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u), makeRasterVariant(0u, 0u)), &isFresh) >= 0 && !isFresh);
    }
    {
        // Tiers off: one raster serves both cells and the variant is the bare phase.
        PhaseAtlas atlas;
        build(/*hasRasterTiers=*/false, atlas);
        CHECK(atlas.monoCalls.size() == 1);
        CHECK(atlas.monoCalls[0].phase == 0u);
    }
}

TEST(aMeasuredInkBoxShrinksTheQuadToTheInk)
{
    TermPalette pal;
    pal.seedDefaults();
    const int rows = 1, columns = 1;
    std::vector<Ubiquitty::Cell> grid(1);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    grid[0] = blank;
    setCell(grid, columns, 0, 0, 0x41u, 0, 1);

    PhaseAtlas atlas;
    BuildInputs in = baseInputs(grid, pal, rows, columns, /*cellWidth*/ 8.0f);

    auto build = [&](GlyphInstance* out, std::size_t capacity) {
        std::vector<CellInstance> cellsOut(4);
        std::vector<DecorationInstance> decoOut(16);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = out;
        buffers.glyphCapacity = capacity;
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    };

    // The control: the table is bound but 'A''s entry is unmeasured, so the
    // quad is the slot-sized one every other case in this file pins. Without
    // this arm a broken table read could satisfy the tight assertions below by
    // never firing.
    std::array<GlyphInstance, 4> control {};
    CHECK(build(control.data(), control.size()).glyphs == 1);
    CHECK(approx(control[0].originInPixels.x, 0.0f) && approx(control[0].originInPixels.y, 0.0f));
    CHECK(approx(control[0].sizeInPixels.x, 8.0f) && approx(control[0].sizeInPixels.y, 16.0f));

    const MonoAtlasBinding bound = atlas.bindMonoAtlas();
    bool isFreshSlot = false;
    const int slot = atlas.monoCache.slotForKey(
        monoGlyphKey(0x41u, bound.weight, FontFace::Regular, /*phase*/ 0u), &isFreshSlot);
    CHECK_MSG(!isFreshSlot, "the control build faulted 'A' at phase 0, so probing that key hits");
    const AtlasSlotRegion slotBox = atlasSlotRegion(slot, 32, 8, 16);

    // Measure: ink at (2,3)..(6,10) within the slot. The UV anchor sits one
    // geometry-margin inside the slot origin while BuildInputs carries margin
    // 0, so the expected screen offset is the ink offset minus that margin —
    // (1,2), not (2,3). Getting the anchor wrong by exactly the margin is the
    // arithmetic mistake this fixture's split margins exist to catch.
    atlas.inkBoxes[static_cast<std::size_t>(slot)] = { { 2.0f, 3.0f }, { 6.0f, 10.0f } };
    std::array<GlyphInstance, 4> tight {};
    CHECK(build(tight.data(), tight.size()).glyphs == 1);
    CHECK(approx(tight[0].originInPixels.x, 1.0f) && approx(tight[0].originInPixels.y, 2.0f));
    CHECK(approx(tight[0].sizeInPixels.x, 4.0f) && approx(tight[0].sizeInPixels.y, 7.0f));
    CHECK(approx(tight[0].atlasTopLeft.x, static_cast<float>(slotBox.x) + 2.0f));
    CHECK(approx(tight[0].atlasTopLeft.y, static_cast<float>(slotBox.y) + 3.0f));
    CHECK(approx(tight[0].atlasBottomRight.x, static_cast<float>(slotBox.x) + 6.0f));
    CHECK(approx(tight[0].atlasBottomRight.y, static_cast<float>(slotBox.y) + 10.0f));

    // A measured EMPTY box is a raster with no ink: no quad at all, while the
    // cell instance still paints the background.
    atlas.inkBoxes[static_cast<std::size_t>(slot)] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    std::array<GlyphInstance, 4> none {};
    const InstanceCounts empty = build(none.data(), none.size());
    CHECK_MSG(empty.glyphs == 0, "a measured empty box emits no glyph quad");
    CHECK_MSG(empty.cells == 1, "the cell instance still paints the background");
}

// The scanner behind the ink table, at its edges. The guard texel is the
// contract a linear tap relies on, and the clamps are what keep it from
// naming texels outside the plane.
TEST(measureInkBoxGuardsAndClamps)
{
    const int width = 8, height = 16;
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(width) * height, 0);

    // No ink: a zero-size box at the origin, which is "emit no quad" — and
    // distinct from the unmeasured marker, whose min exceeds its max.
    GlyphInkBox box = measureInkBox(plane, width, height);
    CHECK(approx(box.minInPixels.x, 0.0f) && approx(box.maxInPixels.x, 0.0f));
    CHECK(box.minInPixels.x <= box.maxInPixels.x);

    // One texel in the interior: one guard texel out on each side.
    plane[static_cast<std::size_t>(8) * width + 4] = 255;
    box = measureInkBox(plane, width, height);
    CHECK(approx(box.minInPixels.x, 3.0f) && approx(box.minInPixels.y, 7.0f));
    CHECK(approx(box.maxInPixels.x, 6.0f) && approx(box.maxInPixels.y, 10.0f));

    // Ink in the top-left corner: the guard clamps at the plane's edge rather
    // than naming texel -1.
    std::ranges::fill(plane, 0);
    plane[0] = 1;
    box = measureInkBox(plane, width, height);
    CHECK(approx(box.minInPixels.x, 0.0f) && approx(box.minInPixels.y, 0.0f));
    CHECK(approx(box.maxInPixels.x, 2.0f) && approx(box.maxInPixels.y, 2.0f));

    // Ink in the bottom-right corner: the far guard clamps to the dimensions.
    std::ranges::fill(plane, 0);
    plane[static_cast<std::size_t>(height - 1) * width + (width - 1)] = 1;
    box = measureInkBox(plane, width, height);
    CHECK(approx(box.minInPixels.x, static_cast<float>(width - 2)) && approx(box.minInPixels.y, static_cast<float>(height - 2)));
    CHECK(approx(box.maxInPixels.x, static_cast<float>(width)) && approx(box.maxInPixels.y, static_cast<float>(height)));

    // A plane shorter than its stated dimensions is a caller error, and the
    // answer is the unmeasured marker rather than a read past the span.
    box = measureInkBox(CoveragePlaneSpan(plane.data(), 4), width, height);
    CHECK_MSG(box.minInPixels.x > box.maxInPixels.x, "a short plane measures as unmeasured");
}

// SGR 73/74 scales a glyph about its quad's centre, and the tight quad's centre
// is not the slot's — so a scripted cell keeps the slot quad even when its slot
// carries a measured box.
TEST(aScriptedGlyphKeepsTheSlotQuadDespiteMeasuredInk)
{
    TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(1);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    grid[0] = blank;
    setCell(grid, 1, 0, 0, 0x41u, 0, 1);
    Ubiquitty::penSetScript(&grid[0].pen, Ubiquitty::VT::scriptSuper);

    PhaseAtlas atlas;
    BuildInputs in = baseInputs(grid, pal, 1, 1, /*cellWidth*/ 8.0f);

    std::vector<CellInstance> cellsOut(4);
    std::vector<DecorationInstance> decoOut(16);
    std::array<GlyphInstance, 4> glyphsOut {};
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decoOut.data();
    buffers.decorationCapacity = decoOut.size();
    buffers.glyphs = glyphsOut.data();
    buffers.glyphCapacity = glyphsOut.size();

    // Warm the slot, then measure it, then rebuild: the scripted cell must
    // ignore the box the table now carries.
    CHECK(buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers).glyphs == 1);
    const MonoAtlasBinding bound = atlas.bindMonoAtlas();
    bool isFreshSlot = false;
    const int slot = atlas.monoCache.slotForKey(
        monoGlyphKey(0x41u, bound.weight, FontFace::Regular, /*phase*/ 0u), &isFreshSlot);
    CHECK_MSG(!isFreshSlot, "the warm build faulted the scripted glyph, so the probe hits");
    atlas.inkBoxes[static_cast<std::size_t>(slot)] = { { 2.0f, 3.0f }, { 6.0f, 10.0f } };

    std::array<GlyphInstance, 4> scripted {};
    buffers.glyphs = scripted.data();
    CHECK(buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers).glyphs == 1);
    const GlyphUV slotUV = bound.geometry.atlasRectForBuilder(slot, bound.slotCapacity);
    CHECK_MSG(approx(scripted[0].atlasTopLeft.x, slotUV.minU) && approx(scripted[0].atlasTopLeft.y, slotUV.minV),
        "a scripted glyph samples the slot rect, not the measured ink");
    const AtlasSlotRegion slotBox = atlasSlotRegion(slot, 32, 8, 16);
    CHECK_MSG(!approx(scripted[0].atlasTopLeft.x, static_cast<float>(slotBox.x) + 2.0f)
            || !approx(scripted[0].atlasTopLeft.y, static_cast<float>(slotBox.y) + 3.0f),
        "and the ink UV it must not take differs from the one it took");
}

TEST(aScriptedCellRidesTheGlyphStreamWithoutSubpixelPlacement)
{
    // SGR 73/74 on a backend that places glyphs at whole pixels — the software
    // reference, and every arm whose slot carries no margin. The shrink cannot
    // be said on a cell quad, which draws its glyph across the whole cell, so
    // the cell must give the glyph up to the stream that can carry a box. The
    // control below is the same cell with the script cleared: it takes the
    // baked route, and that is what makes this case about the script rather
    // than about the stream being offered.
    TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(1);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    grid[0] = blank;
    setCell(grid, 1, 0, 0, 0x41u, 0, 1);

    PhaseAtlas atlas;
    BuildInputs in = baseInputs(grid, pal, 1, 1, /*cellWidth*/ 8.0f);
    in.subpixelGlyphs = false;

    std::vector<CellInstance> cellsOut(4);
    std::vector<DecorationInstance> decoOut(16);
    std::array<GlyphInstance, 4> glyphsOut {};
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decoOut.data();
    buffers.decorationCapacity = decoOut.size();
    buffers.glyphs = glyphsOut.data();
    buffers.glyphCapacity = glyphsOut.size();

    // Control first, on the fixture this case then mutates: unscripted, the
    // glyph is baked into the cell and the stream stays empty.
    const InstanceCounts baked = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(baked.glyphs == 0, "an unscripted cell keeps the baked route when the slot has no margin");
    CHECK_MSG(cellsOut[0].atlasBottomRight.x > cellsOut[0].atlasTopLeft.x,
        "and the picture it draws is on its own quad");
    const Vec2 bakedTopLeft = cellsOut[0].atlasTopLeft;
    const Vec2 bakedBottomRight = cellsOut[0].atlasBottomRight;

    Ubiquitty::penSetScript(&grid[0].pen, Ubiquitty::VT::scriptSuper);
    std::array<GlyphInstance, 4> scripted {};
    buffers.glyphs = scripted.data();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(counts.glyphs == 1, "a scripted cell emits the glyph the cell quad cannot shrink");
    CHECK_MSG(approx(cellsOut[0].atlasTopLeft.x, cellsOut[0].atlasBottomRight.x),
        "and its cell carries background only, so the glyph is not drawn twice");

    // The box is the cell's, scaled and shifted by the shared helper, so the
    // reference and the subpixel stream describe super/subscript once.
    const ScriptGlyphBox expected = scriptGlyphBox({ 0.0f, 0.0f }, { 8.0f, 16.0f },
        Ubiquitty::VT::scriptSuper, 16.0f);
    CHECK_MSG(approx(scripted[0].sizeInPixels.x, expected.size.x)
            && approx(scripted[0].sizeInPixels.y, expected.size.y),
        "the quad is the shrunk one");
    CHECK_MSG(approx(scripted[0].originInPixels.x, expected.origin.x)
            && approx(scripted[0].originInPixels.y, expected.origin.y),
        "placed where the helper puts it, above the cell's centre");
    CHECK_MSG(scripted[0].originInPixels.y < 0.0f,
        "a superscript rises, so its top is above the row it belongs to");

    // Same texels as the baked route: only the destination moved.
    CHECK_MSG(approx(scripted[0].atlasTopLeft.x, bakedTopLeft.x)
            && approx(scripted[0].atlasTopLeft.y, bakedTopLeft.y)
            && approx(scripted[0].atlasBottomRight.x, bakedBottomRight.x)
            && approx(scripted[0].atlasBottomRight.y, bakedBottomRight.y),
        "and it samples the rect the cell would have sampled");
}

TEST(glyphGeometryScalesFromAtlasNativeToTheCell)
{
    TermPalette pal;
    pal.seedDefaults();
    const int rows = 1, columns = 1;
    std::vector<Ubiquitty::Cell> grid(1);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    grid[0] = blank;
    setCell(grid, columns, 0, 0, 0x41u, 0, colorDefault);

    auto glyphFor = [&](float cellWidth, float refAdvance, float slotWidth, float marginX, float atlasToDevice = 1.0f) {
        PhaseAtlas atlas;
        BuildInputs in = baseInputs(grid, pal, rows, columns, cellWidth);
        in.glyphSlotWidthInPixels = slotWidth;
        in.glyphSlotHeightInPixels = 16.0f;
        in.glyphMarginInPixels.width = marginX;
        in.glyphReferenceAdvanceInPixels = refAdvance;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.metrics.atlasToDevicePixels = atlasToDevice;
        std::vector<CellInstance> cellsOut(4);
        std::vector<DecorationInstance> decoOut(16);
        std::vector<GlyphInstance> glyphsOut(4);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decoOut.data();
        buffers.decorationCapacity = decoOut.size();
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        CHECK(counts.glyphs == 1);
        return glyphsOut[0];
    };

    // Natural size: the cell equals the reference, so the slot blits 1:1.
    {
        const GlyphInstance g = glyphFor(/*cell*/ 8.0f, /*ref*/ 8.0f, /*slot*/ 9.0f, /*margin*/ 0.0f);
        CHECK(approx(g.sizeInPixels.x, 9.0f));
    }

    // An atlas rasterised at twice the display scale halves on screen.
    {
        const GlyphInstance g = glyphFor(/*cell*/ 4.0f, /*ref*/ 8.0f, /*slot*/ 9.0f, /*margin*/ 0.0f, /*atlasToDevice*/ 0.5f);
        CHECK(approx(g.sizeInPixels.x, 4.5f));
    }

    // The margin the origin backs off by scales too, or the pen origin lands
    // beside the snapped position instead of on it.
    {
        const GlyphInstance g = glyphFor(/*cell*/ 4.0f, /*ref*/ 8.0f, /*slot*/ 9.0f, /*margin*/ 2.0f, /*atlasToDevice*/ 0.5f);
        CHECK(approx(g.originInPixels.x, -1.0f)); // 0 - 2 * 0.5
    }

    // Referencing the ceil'd slot instead of the true advance is the shrink
    // the reference exists to avoid: at cell 8 the glyph must not come out 8.
    {
        const GlyphInstance g = glyphFor(/*cell*/ 8.0f, /*ref*/ 8.0f, /*slot*/ 9.0f, /*margin*/ 0.0f);
        CHECK(g.sizeInPixels.x > 8.0f);
    }

    // A backend whose cell is its atlas cell leaves the reference zero and gets
    // unit scale rather than a divide.
    {
        const GlyphInstance g = glyphFor(/*cell*/ 4.0f, /*ref*/ 0.0f, /*slot*/ 9.0f, /*margin*/ 0.0f);
        CHECK(approx(g.sizeInPixels.x, 9.0f));
    }
}

TEST(trackingMovesMonoPensWithoutStretchingSlotOrMeasuredInk)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 5;
    std::vector<Cell> grid(columns, defaultBlankCell);
    grid[1].codePoint = 'l';
    grid[3].codePoint = 'l';
    grid[1].pen.attributes.add(VT::Attribute::Underline);
    grid[3].pen.attributes.add(VT::Attribute::Underline);

    for (float deviceScale : { 0.5f, 1.0f, 2.0f }) {
        for (float tracking : { -0.5f, 0.0f, 2.5f }) {
            PhaseAtlas atlas;
            BuildInputs inputs = baseInputs(grid, palette, 1, columns, (6.0f + tracking) * deviceScale);
            inputs.originInPixels.x = 2.25f;
            inputs.subpixelPhaseCount = 4;
            inputs.subpixelPhaseRounding = PhaseRounding::Floor;
            inputs.glyphSlotWidthInPixels = 8.0f;
            inputs.glyphSlotHeightInPixels = 16.0f;
            inputs.glyphReferenceAdvanceInPixels = 6.0f;
            inputs.glyphReferenceLineHeightInPixels = 16.0f;
            inputs.glyphMarginInPixels = { 1.0f, 1.0f };
            inputs.metrics.atlasToDevicePixels = deviceScale;
            inputs.metrics.skipInk = true;
            std::array<CellInstance, columns> cells;
            std::array<DecorationInstance, columns * 8> decorations;
            std::array<GlyphInstance, columns> glyphs;
            InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
            buffers.glyphs = glyphs.data();
            buffers.glyphCapacity = glyphs.size();

            for (bool measured : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
                if (measured) {
                    for (auto& box : atlas.inkBoxes)
                        box = { { 2.0f, 3.0f }, { 6.0f, 10.0f } };
                }
                const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
                REQUIRE(counts.glyphs == 2);
                REQUIRE(counts.decorations == 2);
                for (int index = 0; index < 2; ++index) {
                    const int column = 1 + 2 * index;
                    const auto pen = phaseForPen(inputs.originInPixels.x + column * inputs.metrics.cellWidth, 4, PhaseRounding::Floor);
                    const auto& glyph = glyphs[static_cast<std::size_t>(index)];
                    const float offset = measured ? 1.0f : -1.0f;
                    CHECK(approx(glyph.originInPixels.x, static_cast<float>(pen.snappedPenX) + offset * deviceScale));
                    CHECK(approx(glyph.sizeInPixels.x, (measured ? 4.0f : 8.0f) * deviceScale));
                    CHECK(approx(glyph.sizeInPixels.x / (glyph.atlasBottomRight.x - glyph.atlasTopLeft.x), deviceScale));
                    const auto& rule = decorations[static_cast<std::size_t>(index)];
                    CHECK(rule.skipInk > 0.5f);
                    const float cellLeft = inputs.originInPixels.x + column * inputs.metrics.cellWidth;
                    CHECK(approx(rule.inkTopLeft.x - rule.glyphSlotTopLeftUV.x,
                        1.0f + (cellLeft - static_cast<float>(pen.snappedPenX)) / deviceScale));
                    CHECK(approx(rule.inkBottomRight.x - rule.inkTopLeft.x, 6.0f + tracking));
                    CHECK(approx(rule.glyphSlotBottomRightUV.x - rule.glyphSlotTopLeftUV.x, 8.0f));
                }
            }
            CHECK(!atlas.monoCalls.empty());
            CHECK(atlas.viaMonoGlyph.empty());
        }
    }
}

TEST(trackingKeepsShapedIdentityInkAtItsRasterSize)
{
    struct IdentityAtlas : PhaseAtlas {
        int identityFetches = 0;
        bool measured = false;
        void shapeLigatureRuns(std::span<const LigatureCell> row, float, float, GlyphPass, std::span<LigatureRun> runs)
        {
            for (std::size_t column = 0; column < row.size(); ++column) {
                auto& run = runs[column];
                run = {};
                if (row[column].codePoint != 'l')
                    continue;
                run.strip = { { 100.0f, 20.0f }, { 108.0f, 36.0f }, true };
                run.startColumn = static_cast<int>(column);
                run.length = 1;
                run.hasShapedGlyph = true;
                run.shaped.glyphID = 42;
                run.shaped.coveredCells = 1;
            }
        }
        // Core probes the slot and asks for a raster only on a miss; the
        // glyph's ink is the slot's entry in the binding's table, two pixels
        // in from the slot's left and four wide.
        bool rasteriseShapedGlyph(std::uint32_t, FontFace, std::uint32_t, int)
        {
            ++identityFetches;
            return true;
        }
        void measureEverySlot()
        {
            for (GlyphInkBox& box : inkBoxes)
                box = measured ? GlyphInkBox { { 2.0f, 3.0f }, { 6.0f, 10.0f } } : unmeasuredGlyphInkBox();
        }
    } atlas;
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(5, defaultBlankCell);
    grid[1].codePoint = 'l';
    grid[3].codePoint = 'l';
    auto inputs = baseInputs(grid, palette, 1, 5, 8.5f);
    inputs.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
    inputs.subpixelPhaseRounding = PhaseRounding::Floor;
    inputs.glyphReferenceAdvanceInPixels = 6.0f;
    inputs.glyphReferenceLineHeightInPixels = 16.0f;
    inputs.glyphSlotWidthInPixels = 8.0f;
    inputs.glyphSlotHeightInPixels = 16.0f;
    inputs.glyphMarginInPixels = { 1.0f, 1.0f };
    std::array<CellInstance, 5> cells;
    std::array<GlyphInstance, 5> glyphs;
    std::array<GlyphInstance, 5> strips;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.ligatureGlyphs = strips.data();
    buffers.ligatureGlyphCapacity = strips.size();
    for (bool measured : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
        atlas.measured = measured;
        atlas.measureEverySlot();
        const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
        REQUIRE(counts.glyphs == 2);
        CHECK(counts.ligatureGlyphs == 0);
        for (int index = 0; index < 2; ++index) {
            CHECK(approx(glyphs[index].originInPixels.x, (index == 0 ? 8.0f : 25.0f) + (measured ? 1.0f : -1.0f)));
            CHECK(approx(glyphs[index].sizeInPixels.x, measured ? 4.0f : 8.0f));
            CHECK(approx(glyphs[index].sizeInPixels.x, glyphs[index].atlasBottomRight.x - glyphs[index].atlasTopLeft.x));
        }
    }
    // Both cells stand at the same phase, so one slot serves the glyph for
    // both builds: Core probes, and the backend draws once.
    CHECK_EQ(atlas.identityFetches, 1);
    CHECK(atlas.monoCalls.empty());
}

TEST(trackingKeepsWideGlyphAndSkipInkAtTheSameAtlasScale)
{
    struct WideAtlas : PhaseAtlas {
        GlyphSlotCache cache { 16, recommendedHashBuckets(16), 4 };
        int rasterizations = 0;
    } atlas;
    auto backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.bindWideAtlas = [](void* context) {
        return WideAtlasBinding { &static_cast<WideAtlas*>(context)->cache, 4, 4, 18, 18, 16 };
    };
    backend.rasteriseWideGlyph = [](void* context, std::uint32_t, FontFace, std::uint32_t, int) {
        ++static_cast<WideAtlas*>(context)->rasterizations;
        return true;
    };
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(4, defaultBlankCell);
    grid[1].codePoint = 0x65E5;
    grid[1].pen.attributes = { VT::Attribute::Wide, VT::Attribute::Underline };
    grid[2].codePoint = wideContinuationSentinelCodePoint;
    grid[2].pen.attributes = { VT::Attribute::WideContinuation, VT::Attribute::Underline };
    auto inputs = baseInputs(grid, palette, 1, 4, 10.5f);
    inputs.subpixelPhaseRounding = PhaseRounding::Floor;
    inputs.glyphReferenceAdvanceInPixels = 8.0f;
    inputs.glyphReferenceLineHeightInPixels = 16.0f;
    inputs.glyphBoxWidthInPixels = 8.0f;
    inputs.glyphWideSlotWidthInPixels = 18.0f;
    inputs.glyphWideSlotHeightInPixels = 18.0f;
    inputs.glyphMarginInPixels = { 1.0f, 1.0f };
    inputs.metrics.skipInk = true;
    std::array<CellInstance, 4> cells;
    std::array<DecorationInstance, 32> rules;
    std::array<GlyphInstance, 4> glyphs;
    InstanceBuffers buffers { cells.data(), cells.size(), rules.data(), rules.size() };
    buffers.wideGlyphs = glyphs.data();
    buffers.wideGlyphCapacity = glyphs.size();
    for (float scale : { 0.5f, 1.0f, 2.0f }) {
        inputs.metrics.atlasToDevicePixels = scale;
        inputs.metrics.cellWidth = 10.5f * scale;
        const auto counts = buildCellInstances(inputs, backend, buffers);
        REQUIRE(counts.wideGlyphs == 1);
        REQUIRE(counts.decorations == 2);
        CHECK(approx(glyphs[0].sizeInPixels.x, 18.0f * scale));
        CHECK(approx(glyphs[0].originInPixels.x, std::floor(inputs.metrics.cellWidth) - scale));
        for (int index = 0; index < 2; ++index) {
            CHECK(rules[index].skipInk > 0.5f);
            CHECK(approx(rules[index].inkBottomRight.x - rules[index].inkTopLeft.x, 10.5f));
            const float cellLeft = static_cast<float>(index + 1) * inputs.metrics.cellWidth;
            CHECK(approx(rules[index].inkTopLeft.x - glyphs[0].atlasTopLeft.x,
                (cellLeft - glyphs[0].originInPixels.x) / scale));
            CHECK(approx(rules[index].glyphSlotBottomRightUV.x - rules[index].glyphSlotTopLeftUV.x, 18.0f));
        }
    }
    CHECK(atlas.rasterizations > 0);
}

// A glyph quad drawn into the grid that reaches past its row's band marks the
// boundary it crosses, and only that one: a retained frame reads the marks to
// refuse a partial repaint that would cut the ink or leave it behind. Measured
// ink inside the cell crosses nothing.
TEST(aGlyphReachingPastItsRowMarksTheBoundaryItCrosses)
{
    TermPalette palette;
    palette.seedDefaults();
    const int rows = 3;
    const int columns = 3;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    setCell(grid, columns, 1, 1, 'A', 0, colorDefault);
    BuildInputs inputs = baseInputs(grid, palette, rows, columns, 8.0f);
    inputs.glyphSlotWidthInPixels = 8.0f;
    inputs.glyphSlotHeightInPixels = 16.0f;
    inputs.glyphMarginInPixels = { 1.0f, 1.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns);
    std::vector<GlyphInstance> glyphs(static_cast<std::size_t>(rows) * columns);
    std::vector<std::uint8_t> crossings(static_cast<std::size_t>(rows) + 1, 7);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.rowInkCrossings = crossings.data();
    buffers.rowInkCrossingCapacity = crossings.size();

    // Unmeasured, the quad is the slot, margins and all, so it reaches past
    // the row's band into its neighbour.
    {
        PhaseAtlas atlas;
        const InstanceCounts counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
        REQUIRE(counts.glyphs == 1);
        const float rowTop = 16.0f;
        const float top = glyphs[0].originInPixels.y;
        const float bottom = top + glyphs[0].sizeInPixels.y;
        CHECK_MSG(top < rowTop || bottom > rowTop + 16.0f, "the slot-sized quad reaches past its row");
        CHECK((crossings[1] != 0) == (top < rowTop));
        CHECK((crossings[2] != 0) == (bottom > rowTop + 16.0f));
        CHECK_MSG(crossings[0] == 0 && crossings[3] == 0, "the grid's outer edges are the margin strips', never marked");
    }
    // Measured inside the cell, the quad is the ink, and it crosses nothing.
    {
        PhaseAtlas atlas;
        for (auto& box : atlas.inkBoxes)
            box = { { 2.0f, 3.0f }, { 6.0f, 10.0f } };
        std::ranges::fill(crossings, std::uint8_t { 7 });
        const InstanceCounts counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
        REQUIRE(counts.glyphs == 1);
        CHECK_MSG(std::ranges::all_of(crossings, [](std::uint8_t entry) { return entry == 0; }),
            "ink inside its cell marks no boundary, and the build clears what it does not mark");
    }
}

// A partial frame stands only where no rebuilt row borders a crossing, in the
// target or in the build; a scroll moves the target's crossings with the rows
// it keeps and refuses over a crossing it would cut.
TEST(aPartialFrameStandsOnlyWhereNoRebuiltRowBordersACrossing)
{
    const std::vector<std::uint8_t> clear(5, 0);
    const std::vector<std::uint8_t> dirty { 0, 1, 0, 0 };
    const auto settle = [&](std::vector<std::uint8_t> target, const std::vector<std::uint8_t>& built, int top, int bottom, int delta) {
        return settleRowInkCrossings(dirty, target, built, top, bottom, delta);
    };
    CHECK_MSG(settle(clear, clear, 0, -1, 0), "no crossing anywhere: the partial frame stands");
    const std::vector<std::uint8_t> aboveTheRow { 0, 1, 0, 0, 0 };
    CHECK_MSG(!settle(aboveTheRow, clear, 0, -1, 0), "the target's ink crossing into the rebuilt row refuses");
    CHECK_MSG(!settle(clear, aboveTheRow, 0, -1, 0), "the build's ink crossing out of it refuses");
    const std::vector<std::uint8_t> farAway { 0, 0, 0, 1, 0 };
    CHECK_MSG(settle(farAway, clear, 0, -1, 0), "a crossing between two clean rows stays where it is");

    // Rows 0..3 scroll up by one: rows 1..3 are kept and move to 0..2, row 0
    // leaves. A crossing between kept rows 2 and 3 (boundary 3) moves to
    // boundary 2 — next to the rebuilt row 1, so that frame refuses — while
    // one at the region's edge or beside the leaving row is cut.
    const std::vector<std::uint8_t> exposedOnly { 0, 0, 0, 1, 0 };
    CHECK_MSG(!settle(exposedOnly, clear, 0, 3, 1), "a kept crossing moved beside the rebuilt row refuses");
    CHECK_MSG(!settle({ 0, 1, 0, 0, 0 }, clear, 0, 3, 1), "a crossing beside the row the scroll drops is cut");
    CHECK_MSG(!settle({ 1, 0, 0, 0, 0 }, clear, 0, 3, 1), "and one at the region's top edge");
    const std::vector<std::uint8_t> dirtyBottom { 0, 0, 0, 1 };
    std::vector<std::uint8_t> target { 0, 0, 1, 0, 0 };
    CHECK_MSG(settleRowInkCrossings(dirtyBottom, target, clear, 0, 3, 1), "a crossing between two kept rows moves whole");
    CHECK_MSG(target == std::vector<std::uint8_t>({ 0, 1, 0, 0, 0 }), "and the record moves with the blit");
    std::vector<std::uint8_t> shortTable(4, 0);
    CHECK_MSG(!settleRowInkCrossings(dirty, shortTable, clear, 0, -1, 0), "a short table refuses");
}

// A ligature strip spans its whole slot, margins included, so drawn whole its
// quad reaches past the row above and below whether or not any ink does. The
// rows its raster inked size the quad instead: the same texels, and a quad that
// says where the ink is, so a retained frame does not take a strip for ink
// crossing into the next row.
TEST(aStripSizedToItsInkedRowsCrossesNoRowItDoesNotInk)
{
    struct StripAtlas : PhaseAtlas {
        bool hasInkRows = false;
        void shapeLigatureRuns(std::span<const LigatureCell> row, float, float, GlyphPass, std::span<LigatureRun> runs)
        {
            for (std::size_t column = 0; column < row.size(); ++column) {
                auto& run = runs[column];
                run = {};
                if (row[column].codePoint != 'l')
                    continue;
                run.strip = { { 100.0f, 32.0f }, { 108.0f, 48.0f }, true };
                run.slotWidthInAtlasPixels = 8.0f;
                run.usedWidthInAtlasPixels = 8.0f;
                run.startColumn = static_cast<int>(column);
                run.length = 1;
                run.hasInkRows = hasInkRows;
                run.inkTopInAtlasPixels = 3.0f;
                run.inkBottomInAtlasPixels = 12.0f;
            }
        }
    } atlas;
    TermPalette palette;
    palette.seedDefaults();
    const int rows = 3;
    const int columns = 3;
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    grid[4].codePoint = 'l';
    auto inputs = baseInputs(grid, palette, rows, columns, 8.0f);
    inputs.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
    inputs.glyphSlotWidthInPixels = 8.0f;
    inputs.glyphSlotHeightInPixels = 16.0f;
    inputs.glyphMarginInPixels = { 1.0f, 1.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns);
    std::vector<GlyphInstance> glyphs(static_cast<std::size_t>(rows) * columns);
    std::vector<GlyphInstance> strips(static_cast<std::size_t>(rows) * columns);
    std::vector<std::uint8_t> crossings(static_cast<std::size_t>(rows) + 1, 0);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.ligatureGlyphs = strips.data();
    buffers.ligatureGlyphCapacity = strips.size();
    buffers.rowInkCrossings = crossings.data();
    buffers.rowInkCrossingCapacity = crossings.size();

    const InstanceCounts whole = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(whole.ligatureGlyphs == 1);
    const float wholeTop = strips[0].originInPixels.y;
    CHECK_MSG(strips[0].sizeInPixels.y == 16.0f && strips[0].atlasTopLeft.y == 32.0f, "unmeasured, the quad is the slot");
    CHECK_MSG(crossings[1] != 0, "and the slot's top margin reaches into the row above");

    atlas.hasInkRows = true;
    const InstanceCounts inked = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(inked.ligatureGlyphs == 1);
    CHECK_MSG(strips[0].originInPixels.y == wholeTop + 2.0f, "measured, the quad starts a guard row above the ink");
    CHECK_MSG(strips[0].sizeInPixels.y == 11.0f, "and spans the inked rows and a guard row each side");
    CHECK_MSG(strips[0].atlasTopLeft.y == 34.0f && strips[0].atlasBottomRight.y == 45.0f,
        "sampling the same texels the slot quad sampled there");
    CHECK_MSG(std::ranges::all_of(crossings, [](std::uint8_t entry) { return entry == 0; }), "and crosses no row");
}

UBIQUITTY_TEST_MAIN()
