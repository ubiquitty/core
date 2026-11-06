// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "StrictAtlas.h"

// test_cell_instances — drives the portable buildCellInstances() over a canned
// Cell grid with a fake atlas (returns deterministic UVs, no coverage for
// spaces), asserting the emitted CellInstance fields: per-cell grid position,
// wide-lead width + continuation skip, ColorResolve'd foreground/background (incl. reverse),
// the glyph atlas UV vs blank, and that beginFrame fires once.

#include "Core/Rendering/CellInstanceBuilder.h"

#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/RuleGeometry.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include "Core/RowMeta.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Fake atlas: deterministic non-degenerate UVs, and "no coverage" for a space
// (0x20) so the blank-glyph path is exercised. Counts beginFrame() calls.
namespace {
struct FakeAtlas : UbiquittyTest::StrictAtlas {
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
    int monoRasterised = 0;
    Ubiquitty::Rendering::MonoAtlasBinding bindMonoAtlas()
    {
        Ubiquitty::Rendering::MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int)
    {
        ++monoRasterised;
        return true;
    }

    int frames = 0;
    int nextSlot = 0;
    int doubleCalls = 0;
    bool hasDoubleMasters = true;
    void beginFrame() { ++frames; }
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t) { return assign(codePoint); }
    AtlasGlyph wideGlyph(std::uint32_t codePoint, FontFace) { return assign(codePoint); }
    AtlasGlyph doubleGlyph(std::uint32_t codePoint, FontFace, DoubleRegion region)
    {
        ++doubleCalls;
        if (!hasDoubleMasters || codePoint == 0x20u)
            return {}; // not-present -> the builder must fall back to the 1x glyph
        // Doubles live in a distinct V band (0.5+) so a test can tell a 2x
        // master crop from a mono-fallback crop.
        //
        // The BACKEND applies the region, which is what a real one does: the
        // inset around a master inside its slot is per edge, so the half is not
        // arithmetic the builder can do. Returning the full box here and letting
        // the builder halve it would test a contract nothing implements.
        const float u = static_cast<float>(nextSlot++) * 0.01f;
        const float minV = 0.5f + u;
        const float maxV = minV + 0.04f;
        const float midV = minV + 0.02f;
        if (region == DoubleRegion::Top)
            return { { minV, minV }, { minV + 0.04f, midV }, true };
        if (region == DoubleRegion::Bottom)
            return { { minV, midV }, { minV + 0.04f, maxV }, true };
        return { { minV, minV }, { maxV, maxV }, true };
    }
    AtlasGlyph assign(std::uint32_t codePoint)
    {
        if (codePoint == 0x20u)
            return {}; // space: no coverage -> not present (blank)
        const float u = static_cast<float>(nextSlot++) * 0.01f;
        return { { u, u }, { u + 0.02f, u + 0.02f }, true };
    }
};

void setCell(std::vector<Ubiquitty::Cell>& grid, int columns, int row, int column, uint32_t ch, Ubiquitty::VT::AttributeSet attributes, uint16_t foregroundIndex, uint16_t backgroundIndex)
{
    Ubiquitty::Cell& c = grid[static_cast<std::size_t>(row) * columns + column];
    c.codePoint = ch;
    c.pen.attributes = attributes;
    c.pen.foregroundIndex = foregroundIndex;
    c.pen.backgroundIndex = backgroundIndex;
    c.pen.underlineIndex = colorDefault;
}

TEST(explicitWidthTextRendersTheWholePayloadInsideItsBands)
{
    for (const auto [width, numerator, denominator, scale, textLength] : std::array {
             std::array { 3, 0, 0, 1, 3 }, std::array { 1, 1, 2, 1, 2 },
             std::array { 7, 1, 3, 1, 21 }, std::array { 2, 1, 2, 2, 4 } }) {
        struct TextAtlas : FakeAtlas {
            std::vector<std::uint32_t> text;
            int fetched = 0;
            AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t)
            {
                ++fetched;
                return { { 0, 0 }, { 8, 16 }, true };
            }
        } atlas;
        atlas.text.assign(static_cast<std::size_t>(textLength), 'a');
        auto backend = UbiquittyTest::glyphBackendFor(atlas);
        TextSizeSpec spec;
        spec.widthCells = static_cast<std::uint8_t>(width);
        spec.fractionNumerator = static_cast<std::uint8_t>(numerator);
        spec.fractionDenominator = static_cast<std::uint8_t>(denominator);
        spec.scale = static_cast<std::uint8_t>(scale);
        spec.verticalAlign = scale == 2 ? 1 : 0;
        spec.isTextRun = 1;
        TextSizeTable table;
        const auto id = table.intern(spec);
        const int columns = width * scale + 1;
        std::vector<Cell> grid(static_cast<std::size_t>(columns * scale), defaultBlankCell);
        for (int row = 0; row < scale; ++row) {
            for (int column = 0; column < width * scale; ++column) {
                auto& cell = grid[static_cast<std::size_t>(row * columns + column)];
                cell.pen.extendedAttributes.setTextSizeSpecID(id);
                penSetTextBand(&cell.pen, static_cast<unsigned>(row));
                if (column) {
                    cell.pen.attributes.add(VT::Attribute::WideContinuation);
                } else {
                    cell.codePoint = 'a';
                    cell.setClusterID(1);
                    if (width * scale > 1)
                        cell.pen.attributes.add(VT::Attribute::Wide);
                }
            }
        }
        TermPalette palette;
        palette.seedDefaults();
        BuildInputs in;
        in.cells = grid.data();
        in.rows = scale;
        in.columns = columns;
        in.palette = &palette;
        in.textSizeTable = &table;
        in.clusterResolverContext = &atlas;
        in.clusterResolver = [](const Cell*, std::span<std::uint32_t> out, void* context) {
            const auto& text = static_cast<TextAtlas*>(context)->text;
            std::copy(text.begin(), text.end(), out.begin());
            return static_cast<int>(text.size());
        };
        in.metrics.cellWidth = 8;
        in.metrics.cellHeight = 16;
        in.glyphReferenceAdvanceInPixels = 8;
        in.glyphReferenceLineHeightInPixels = 16;
        std::vector<CellInstance> cells(grid.size() + 8);
        std::vector<DecorationInstance> decorations(grid.size() * 8);
        std::array<GlyphInstance, 32> glyphs;
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        buffers.glyphs = glyphs.data();
        buffers.glyphCapacity = glyphs.size();
        const auto counts = buildCellInstances(in, backend, buffers);
        CHECK_EQ(atlas.fetched, textLength);
        REQUIRE(counts.glyphs == static_cast<std::size_t>(textLength));
        const float fraction = denominator ? static_cast<float>(numerator) / denominator : 1.0f;
        for (int index = 0; index < textLength; ++index) {
            CHECK(approx(glyphs[index].originInPixels.x, index * scale * 8.0f * fraction));
            CHECK(approx(glyphs[index].sizeInPixels.x, scale * 8.0f * fraction));
            CHECK(approx(glyphs[index].originInPixels.y, scale == 2 ? 16.0f : 0.0f));
            CHECK(approx(glyphs[index].sizeInPixels.y, scale * 16.0f * fraction));
        }
    }
}

TEST(testFrameShapingPreparationOrder)
{
    struct PreparingAtlas : FakeAtlas {
        std::vector<int> events;
    } atlas;
    auto backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.prepareLigatureRow = [](void* context, LigatureRowSpan row, float) {
        static_cast<PreparingAtlas*>(context)->events.push_back(static_cast<int>(row[0].codePoint));
    };
    backend.finishLigaturePreparation = [](void* context) {
        static_cast<PreparingAtlas*>(context)->events.push_back(0);
    };
    backend.shapeLigatureRuns = [](void* context, LigatureRowSpan row, float, float, GlyphPass, std::span<LigatureRun> runs) {
        static_cast<PreparingAtlas*>(context)->events.push_back(-static_cast<int>(row[0].codePoint));
        std::fill(runs.begin(), runs.end(), LigatureRun {});
    };

    constexpr int columns = 4;
    constexpr int rows = 4;
    std::vector<Cell> grid(columns * rows);
    for (int row = 0; row < rows; ++row) {
        setCell(grid, columns, row, 0, static_cast<std::uint32_t>('a' + row), {}, colorDefault, colorDefault);
        setCell(grid, columns, row, 1, '=', {}, colorDefault, colorDefault);
        setCell(grid, columns, row, 2, '>', {}, colorDefault, colorDefault);
        setCell(grid, columns, row, 3, ' ', {}, colorDefault, colorDefault);
    }
    const std::uint8_t dirtyRows[rows] = { 1, 0, 1, 1 };
    const std::uint8_t lineAttributes[rows] = { lineAttributeNormal, lineAttributeNormal, lineAttributeDoubleWidth, lineAttributeNormal };
    TermPalette palette;
    palette.seedDefaults();
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = rows;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.dirtyRows = dirtyRows;
    inputs.lineAttributes = lineAttributes;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.metrics.baselineFromTop = 13;
    std::vector<CellInstance> cells(columns * rows);
    std::vector<GlyphInstance> ligatures(columns * rows);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.ligatureGlyphs = ligatures.data();
    buffers.ligatureGlyphCapacity = ligatures.size();
    const auto prepared = buildCellInstances(inputs, backend, buffers);
    const std::vector<int> expected { 'a', 'd', 0, -'a', -'d' };
    CHECK(atlas.events == expected);
    CHECK(atlas.frames == 1);

    atlas.events.clear();
    backend.prepareLigatureRow = nullptr;
    backend.finishLigaturePreparation = nullptr;
    const auto unprepared = buildCellInstances(inputs, backend, buffers);
    const std::vector<int> drawOnly { -'a', -'d' };
    CHECK(atlas.events == drawOnly);
    CHECK(prepared.cells == unprepared.cells);
    CHECK(prepared.ligatureGlyphs == unprepared.ligatureGlyphs);
}

// Every cell resolves against its own pen, whatever its neighbours carry.
TEST(testPerPenColorAcrossRuns)
{
    TermPalette pal;
    pal.seedDefaults();

    const int columns = 8;
    std::vector<Ubiquitty::Cell> grid(columns);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    for (auto& c : grid)
        c = blank;

    // A A A | B B | A | C | A — B differs by color index, C only by an
    // attribute, and the row returns to A twice.
    setCell(grid, columns, 0, 0, 0x41u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 1, 0x42u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 2, 0x43u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 3, 0x44u, {}, 1, colorDefault);
    setCell(grid, columns, 0, 4, 0x45u, {}, 1, colorDefault);
    setCell(grid, columns, 0, 5, 0x46u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 6, 0x47u, Ubiquitty::VT::Attribute::Reverse, colorDefault, colorDefault);
    setCell(grid, columns, 0, 7, 0x48u, {}, colorDefault, colorDefault);

    FakeAtlas atlas;
    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;

    std::vector<CellInstance> cellsOut(columns);
    std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(columns) * 8);
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.cells == static_cast<std::size_t>(columns));

    // Every cell carries the color its OWN pen resolves to: the memo may skip
    // work, never change the answer.
    for (int column = 0; column < columns; ++column) {
        const ResolvedCell expected = resolveCell(Ubiquitty::cellPen(grid[column]), pal, /*reverseAlso=*/false, in.brightenBold);
        CHECK(approx(cellsOut[column].foreground.x, expected.foreground.red / 255.0f));
        CHECK(approx(cellsOut[column].foreground.y, expected.foreground.green / 255.0f));
        CHECK(approx(cellsOut[column].foreground.z, expected.foreground.blue / 255.0f));
        CHECK(approx(cellsOut[column].background.x, expected.background.red / 255.0f));
        CHECK(approx(cellsOut[column].background.y, expected.background.green / 255.0f));
        CHECK(approx(cellsOut[column].background.z, expected.background.blue / 255.0f));
    }

    // The reverse cell's swap must not carry into the plain cell after it — what
    // a memo comparing too few pen bytes would get wrong.
    CHECK(!approx(cellsOut[6].foreground.x, cellsOut[7].foreground.x));
    CHECK(!approx(cellsOut[6].background.x, cellsOut[7].background.x));
}

// A caller holding a retained target passes per-row damage so the builder skips
// rows whose instances it still has. The skip has to be total — a clean row
// contributes no instance of any kind — and must leave its neighbours untouched,
// since a partial skip would show as a stale band in the retained target.
TEST(testDirtyRowsSkipCleanRows)
{
    TermPalette pal;
    pal.seedDefaults();

    const int rows = 3, columns = 2;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    for (auto& c : grid)
        c = blank;
    // Every cell inked; row 1 underlined so it owns decorations as well as cells.
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0x41u + static_cast<uint32_t>(row), row == 1 ? Ubiquitty::VT::AttributeSet { Ubiquitty::VT::Attribute::Underline } : Ubiquitty::VT::AttributeSet {}, colorDefault, colorDefault);

    auto build = [&](const std::uint8_t* dirtyRows, std::vector<CellInstance>& cellsOut) {
        FakeAtlas atlas;
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.metrics.underlineDrop = 1.0f;
        in.metrics.strikeThickness = 1.0f;
        in.metrics.xHeight = 10.0f;
        in.dirtyRows = dirtyRows;
        std::vector<DecorationInstance> decorationsOut(64);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decorationsOut.data();
        buffers.decorationCapacity = decorationsOut.size();
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    };

    // No damage passed: every row builds. This is what a caller without a
    // retained target keeps getting, so it is the case that must not move.
    std::vector<CellInstance> allCells(16);
    const InstanceCounts all = build(nullptr, allCells);
    CHECK(all.cells == 6);
    CHECK(all.decorations == 2); // row 1's two underline rules

    // Rows 0 and 2 clean: only row 1 rebuilds, and it keeps its decorations.
    const std::uint8_t middleOnly[3] = { 0, 1, 0 };
    std::vector<CellInstance> middleCells(16);
    const InstanceCounts middle = build(middleOnly, middleCells);
    CHECK(middle.cells == 2);
    CHECK(middle.decorations == 2);
    CHECK(approx(middleCells[0].gridPosition.y, 1.0f) && approx(middleCells[1].gridPosition.y, 1.0f));

    // Every row clean: nothing at all is emitted.
    const std::uint8_t noneDirty[3] = { 0, 0, 0 };
    std::vector<CellInstance> noCells(16);
    const InstanceCounts none = build(noneDirty, noCells);
    CHECK(none.cells == 0);
    CHECK(none.decorations == 0);

    // Every row dirty is indistinguishable from passing nothing.
    const std::uint8_t allDirty[3] = { 1, 1, 1 };
    std::vector<CellInstance> everyCell(16);
    const InstanceCounts every = build(allDirty, everyCell);
    CHECK(every.cells == all.cells && every.decorations == all.decorations);
}
} // namespace

// A focused block cursor is the cursor tier: the cell it stands on, re-emitted
// with the cursor's colours as the tail of each stream, built after the last
// row so every backend draws it after the grid, the images and the overlays.
// The grid's own instance of that cell is untouched whatever stands around
// the cursor — a bake into it is what put a cursor into retained pixels,
// made every blink phase owe its row, and let a program's output change how
// the cursor was drawn. A slide in flight is the exception: the block then
// rides above the grid as a moving overlay, buildCursorQuad emits one for a
// focused block only while sliding (OverlayGeometry.cpp), and no tier is
// built.
//
// Asserted against the grid's own cell rather than against a colour constant:
// the tier's fill is a blend toward the cursor colour, so the value depends on
// the palette, and what these cases are about is whether the blend went into
// the tier and not into the grid.
/// A cursor on the second column of a wide pair stands on that glyph.
///
/// The continuation cell draws nothing of its own, so a tier keyed on the
/// exact cursor column would land on an empty cell and vanish. The tier
/// answers to the anchor's claim instead, and the overlay cursor is handed
/// the anchor and the claimed width so the unfocused outline spans both cells.
TEST(testCursorInsideAClaimIsTheTierOverTheAnchor)
{
    const int rows = 1, columns = 6;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 2, 0x4E00u, Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    setCell(grid, columns, 0, 3, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);

    auto build = [&](int cursorColumn, bool focused, std::vector<CellInstance>& cellsOut, std::vector<OverlayInstance>& overlaysOut, InstanceCounts& counts) {
        FakeAtlas atlas;
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.cursor.row = 0;
        in.cursor.column = cursorColumn;
        in.cursor.shape = Ubiquitty::Cursor::Shape::Block;
        in.cursor.focused = focused;
        in.cursor.mixA = 1.0f;
        in.cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
        // Room for the tier's tail beyond the grid's own cells.
        cellsOut.assign(static_cast<std::size_t>(rows) * columns + 4, CellInstance {});
        std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(rows) * columns * 8);
        overlaysOut.assign(8, OverlayInstance {});
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decorationsOut.data();
        buffers.decorationCapacity = decorationsOut.size();
        buffers.overlays = overlaysOut.data();
        buffers.overlayCapacity = overlaysOut.size();
        counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    };

    // Focused block on the continuation column: the tier is the ANCHOR's
    // cell covering the whole claim rather than the continuation column,
    // which is the part a wide pair gets wrong, and the grid's own anchor
    // keeps its colours.
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        InstanceCounts counts;
        build(3, true, cells, overlays, counts);
        CHECK_MSG(counts.cursorTier.cells == 1, "a block standing on a glyph is one tier cell");
        REQUIRE_MSG(counts.cells >= 2 && counts.cells <= cells.size(), "and the walk emitted the row");
        const CellInstance& tier = cells[counts.cells - 1];
        CHECK_MSG(approx(tier.gridPosition.x, 2.0f), "standing at the anchor");
        CHECK_MSG(approx(tier.background.x, 1.0f), "with the fill lerped to the cursor colour");
        CHECK_MSG(tier.widthCells > 1.0f, "spanning the claim the anchor spans");
        bool didTheGridCarryTheCursor = false;
        for (std::size_t i = 0; i + counts.cursorTier.cells < counts.cells; ++i)
            if (approx(cells[i].background.x, 1.0f))
                didTheGridCarryTheCursor = true;
        CHECK_MSG(!didTheGridCarryTheCursor, "and no cell of the grid carries the cursor colour");
        CHECK_MSG(counts.overlays == 0, "and needs no overlay quad");
    }
    // Unfocused hollow block on the continuation column: one outline, two cells wide.
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        InstanceCounts counts;
        build(3, false, cells, overlays, counts);
        CHECK_MSG(counts.overlays == 1, "an unfocused block is one outline quad");
        CHECK_MSG(counts.overlays >= 1 && approx(overlays[0].topLeftInPixels.x, 2 * 8.0f)
                && approx(overlays[0].bottomRightInPixels.x, 4 * 8.0f),
            "the outline starts at the anchor and spans the pair");
    }
}

TEST(testSlidingBlockBuildsNoTierAndASettledOneLeavesTheGridAlone)
{
    const int rows = 2, columns = 4;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 0x41u, {}, colorDefault, colorDefault);

    auto build = [&](bool isSliding, CellInstance& outCell, CellInstance& outTail, InstanceCounts& counts) {
        FakeAtlas atlas;
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.cursor.row = 0;
        in.cursor.column = 3;
        in.cursor.shape = Ubiquitty::Cursor::Shape::Block;
        in.cursor.focused = true;
        in.cursor.mixA = 1.0f;
        in.cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
        in.cursor.isSliding = isSliding;
        in.cursor.columnFractional = 3.5f;
        in.cursor.vrowFractional = 0.0f;
        std::vector<CellInstance> cellsOut(static_cast<std::size_t>(rows) * columns + 4);
        std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(rows) * columns * 8);
        std::vector<OverlayInstance> overlaysOut(8);
        InstanceBuffers buffers;
        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decorationsOut.data();
        buffers.decorationCapacity = decorationsOut.size();
        buffers.overlays = overlaysOut.data();
        buffers.overlayCapacity = overlaysOut.size();
        counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        // The cursor sits at (0,3); instances are emitted in grid order and the
        // cursor's row takes no blank-run fill, so index 3 is that cell.
        outCell = cellsOut[3];
        outTail = counts.cells > 0 && counts.cells <= cellsOut.size() ? cellsOut[counts.cells - 1] : CellInstance {};
    };

    CellInstance settled {};
    CellInstance settledTail {};
    InstanceCounts settledCounts;
    CellInstance sliding {};
    CellInstance slidingTail {};
    InstanceCounts slidingCounts;
    build(/*isSliding=*/false, settled, settledTail, settledCounts);
    build(/*isSliding=*/true, sliding, slidingTail, slidingCounts);

    // Neither arm touches the grid's own cell: the settled block is a tier
    // over it, the sliding one an overlay above it.
    CHECK_MSG(approx(settled.background.x, sliding.background.x),
        "a settled block and a sliding one leave the grid's cell the same");
    CHECK_MSG(!approx(settled.background.x, 1.0f), "and neither writes the cursor colour into it");

    // The settled arm builds the tier: one cell at the tail, in the cursor
    // colour, at the cursor's position.
    CHECK_MSG(settledCounts.cursorTier.cells == 1, "a settled focused block builds one tier cell");
    CHECK_MSG(approx(settledTail.background.x, 1.0f), "whose fill is the cursor colour");
    CHECK_MSG(approx(settledTail.gridPosition.x, 3.0f) && approx(settledTail.gridPosition.y, 0.0f),
        "at the cursor's cell");
    CHECK_MSG(settledCounts.overlays == 0, "and a settled focused block needs no overlay");

    // The sliding arm emits the moving block instead and no tier. Without
    // this the case would pass for a build that dropped the cursor entirely.
    CHECK_MSG(slidingCounts.cursorTier.cells == 0, "a sliding block builds no tier");
    CHECK_MSG(slidingCounts.overlays == 1, "and rides above the grid as one overlay");
}

/// The tier is built whether or not the cursor's row is dirty.
///
/// A backend keeping its grid across frames rebuilds only the dirty rows, and
/// a blinking cursor on a quiet prompt row dirties none — that is the point of
/// the tier. So the tier pass visits the cursor's row regardless, emits the
/// cursor's cell alone, and emits nothing for that row into the grid streams.
TEST(testTheTierIsBuiltOnACleanRow)
{
    const int rows = 2, columns = 4;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 0x41u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 1, 0, 0x42u, {}, colorDefault, colorDefault);
    // Row 0, the cursor's, is clean; row 1 is the frame's only dirty row.
    const std::uint8_t dirtyRows[2] = { 0, 1 };

    FakeAtlas atlas;
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.dirtyRows = dirtyRows;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.cursor.row = 0;
    in.cursor.column = 3;
    in.cursor.shape = Ubiquitty::Cursor::Shape::Block;
    in.cursor.focused = true;
    in.cursor.mixA = 1.0f;
    in.cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
    std::vector<CellInstance> cellsOut(static_cast<std::size_t>(rows) * columns + 4);
    std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(rows) * columns * 8);
    std::vector<OverlayInstance> overlaysOut(8);
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();
    buffers.overlays = overlaysOut.data();
    buffers.overlayCapacity = overlaysOut.size();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    CHECK_MSG(counts.cursorTier.cells == 1, "the tier is built for a cursor on a clean row");
    CHECK_MSG(counts.cells >= 1 && counts.cells <= cellsOut.size(), "and the fixture has room for it");
    const CellInstance& tier = cellsOut[counts.cells - 1];
    CHECK_MSG(approx(tier.gridPosition.x, 3.0f) && approx(tier.gridPosition.y, 0.0f), "at the cursor's cell");
    CHECK_MSG(approx(tier.background.x, 1.0f), "in the cursor colour");
    bool didHeadTouchCleanRow = false;
    for (std::size_t i = 0; i + counts.cursorTier.cells < counts.cells; ++i)
        if (approx(cellsOut[i].gridPosition.y, 0.0f))
            didHeadTouchCleanRow = true;
    CHECK_MSG(!didHeadTouchCleanRow, "and the clean row's cells stay out of the grid streams");
}

/// The grid never changes for the cursor.
///
/// Whatever the cursor stands on — a letter, the blank beside one, a wide
/// pair — and whatever its shape, focus and blink weight, the head of every
/// stream is byte for byte the build with no cursor at all; only the tier's
/// tail and the overlay quads differ. A bake into the cell would let a
/// program's output around the cursor decide how the cursor is drawn, and
/// make a blink owe its row.
TEST(testTheGridIsTheSameWithAndWithoutACursor)
{
    const int rows = 2, columns = 6;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 0, 0x41u, {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 2, 0x4E00u, Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    setCell(grid, columns, 0, 3, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    setCell(grid, columns, 1, 1, 0x42u, Ubiquitty::VT::Attribute::Underline, colorDefault, colorDefault);

    struct Streams {
        std::vector<CellInstance> cells;
        std::vector<DecorationInstance> decorations;
        std::vector<OverlayInstance> overlays;
        InstanceCounts counts;
    };
    const auto build = [&](int cursorRow, int cursorColumn, Ubiquitty::Cursor::Shape shape, bool focused, float mixA) {
        Streams out;
        FakeAtlas atlas;
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.cursor.row = cursorRow;
        in.cursor.column = cursorColumn;
        in.cursor.shape = shape;
        in.cursor.focused = focused;
        in.cursor.mixA = mixA;
        in.cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
        out.cells.assign(static_cast<std::size_t>(rows) * columns + 4, CellInstance {});
        out.decorations.assign(static_cast<std::size_t>(rows) * columns * 8, DecorationInstance {});
        out.overlays.assign(8, OverlayInstance {});
        InstanceBuffers buffers;
        buffers.cells = out.cells.data();
        buffers.cellCapacity = out.cells.size();
        buffers.decorations = out.decorations.data();
        buffers.decorationCapacity = out.decorations.size();
        buffers.overlays = out.overlays.data();
        buffers.overlayCapacity = out.overlays.size();
        out.counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        return out;
    };
    const auto sameHead = [](const auto& with, std::size_t withCount, const auto& without, std::size_t withoutCount, std::size_t tail) {
        if (withCount < tail || withCount - tail != withoutCount)
            return false;
        return std::memcmp(with.data(), without.data(), withoutCount * sizeof(with[0])) == 0;
    };

    const Streams bare = build(-1, 0, Ubiquitty::Cursor::Shape::Block, true, 1.0f);
    for (const auto& [row, column] : { std::pair { 0, 0 }, std::pair { 0, 1 }, std::pair { 0, 3 }, std::pair { 1, 1 }, std::pair { 1, 5 } }) {
        for (const Ubiquitty::Cursor::Shape shape : { Ubiquitty::Cursor::Shape::Block, Ubiquitty::Cursor::Shape::Bar, Ubiquitty::Cursor::Shape::Underline }) {
            for (const bool focused : { true, false }) {
                for (const float mixA : { 0.0f, 0.5f, 1.0f }) {
                    const Streams with = build(row, column, shape, focused, mixA);
                    CHECK_MSG(sameHead(with.cells, with.counts.cells, bare.cells, bare.counts.cells, with.counts.cursorTier.cells),
                        "the cell stream's head is the build without a cursor");
                    CHECK_MSG(sameHead(with.decorations, with.counts.decorations, bare.decorations, bare.counts.decorations, with.counts.cursorTier.decorations),
                        "and so is the decoration stream's head");
                    const bool isTier = shape == Ubiquitty::Cursor::Shape::Block && focused;
                    CHECK_MSG((with.counts.cursorTier.cells == 1) == isTier, "a focused block is the tier, and nothing else is");
                    // A quad at weight zero is declined; the tier is built at every weight.
                    const std::size_t quads = isTier || mixA <= 0.001f ? 0 : 1;
                    CHECK_MSG(with.counts.overlays == quads, "the rest are one overlay quad while their weight shows");
                }
            }
        }
    }
}

// The 2x4 grid every case here builds from: 'A' in red, a space, a wide CJK
// lead and its continuation on row 0; a reversed, underlined 'B' on row 1.
//
// A struct constructed in place rather than a helper returning one: `buffers`
// holds pointers into `cellsOut` and `decorationsOut`, so a fixture returned by
// value would leave them addressing the moved-from vectors. It asserts nothing —
// CHECK increments a counter only a case body declares.
struct CellFixture {
    static constexpr int rows = 2;
    static constexpr int columns = 4;

    TermPalette pal;
    Ubiquitty::Cell blank {};
    std::vector<Ubiquitty::Cell> grid;
    FakeAtlas atlas;
    BuildInputs in;
    std::vector<CellInstance> cellsOut;
    std::vector<DecorationInstance> decorationsOut;
    InstanceBuffers buffers;
    InstanceCounts counts {};

    CellFixture()
        : grid(static_cast<std::size_t>(rows) * columns)
        , cellsOut(static_cast<std::size_t>(rows) * columns)
        , decorationsOut(static_cast<std::size_t>(rows) * columns * 8)
    {
        pal.seedDefaults(); // default foreground C0C0C0, background 101014; ansi[1] = 205,0,0
        blank.pen.foregroundIndex = colorDefault;
        blank.pen.backgroundIndex = colorDefault;
        blank.pen.underlineIndex = colorDefault;
        for (auto& c : grid)
            c = blank;

        // (0,0) 'A' red foreground; (0,1) space default; (0,2) wide CJK; (0,3) its continuation.
        setCell(grid, columns, 0, 0, 0x41u, {}, /*foreground*/ 1, colorDefault);
        setCell(grid, columns, 0, 1, 0x20u, {}, colorDefault, colorDefault);
        setCell(grid, columns, 0, 2, 0x4E00u, Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
        setCell(grid, columns, 0, 3, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
        // (1,0) 'B' reverse + underline; (1,1..3) blank default.
        setCell(grid, columns, 1, 0, 0x42u, { Ubiquitty::VT::Attribute::Reverse, Ubiquitty::VT::Attribute::Underline }, colorDefault, colorDefault);

        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.metrics.underlineDrop = 1.0f;
        in.metrics.strikeThickness = 1.0f;
        in.metrics.xHeight = 10.0f;

        buffers.cells = cellsOut.data();
        buffers.cellCapacity = cellsOut.size();
        buffers.decorations = decorationsOut.data();
        buffers.decorationCapacity = decorationsOut.size();
        counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    }
};

// DecorationInstance is the full 160-byte macOS layout; the shaders read the
// prefix through padInAtlas (@64) at these offsets.
static_assert(sizeof(DecorationInstance) == 160, "DecorationInstance grew to the macOS 160-byte layout");
static_assert(offsetof(DecorationInstance, padInAtlas) == 64, "the GPU-read prefix offsets stay put");

// The build itself: one instance per cell but for the wide continuation, one
// solid rule for the single underlined cell, and each cell's colours and glyph
// presence.
TEST(testBuildEmitsOneInstancePerVisibleCell)
{
    CellFixture fx;
    [[maybe_unused]] FakeAtlas& atlas = fx.atlas;
    [[maybe_unused]] std::vector<CellInstance>& cellsOut = fx.cellsOut;
    [[maybe_unused]] std::vector<DecorationInstance>& decorationsOut = fx.decorationsOut;
    [[maybe_unused]] const InstanceCounts counts = fx.counts;

    // 8 cells minus the one wide continuation = 7 instances; beginFrame once.
    CHECK(counts.cells == 7);
    CHECK(atlas.frames == 1);

    // The one underlined cell (1,0) emits a single solid rule spanning the
    // cell's advance box. uy = cellTop(16) + baseline(13) + drop(1) = 30, and
    // both edges snap to whole device pixels — round(uy - t/2 + scale/2) with
    // t = round(thickness) — so the rule is [30, 31] rather than straddling a
    // pixel boundary at [29.5, 30.5]. An unsnapped rule antialiases across two
    // rows and reads lighter than the rules either side of it.
    CHECK(counts.decorations == 1);
    CHECK(decorationsOut[0].kind == static_cast<float>(DecorationKind::Solid));
    CHECK(approx(decorationsOut[0].topLeftInPixels.x, 0.0f) && approx(decorationsOut[0].bottomRightInPixels.x, 8.0f));
    CHECK(approx(decorationsOut[0].topLeftInPixels.y, 30.0f) && approx(decorationsOut[0].bottomRightInPixels.y, 31.0f));
    // The snapping itself, stated independently of the arithmetic above: both
    // edges land on whole pixels whatever the metrics work out to.
    CHECK(approx(decorationsOut[0].topLeftInPixels.y, std::round(decorationsOut[0].topLeftInPixels.y)));
    CHECK(approx(decorationsOut[0].bottomRightInPixels.y, std::round(decorationsOut[0].bottomRightInPixels.y)));

    // cellsOut[0] = (0,0) 'A': grid (0,0), narrow, red foreground, default background, real glyph UV.
    CHECK(approx(cellsOut[0].gridPosition.x, 0.0f) && approx(cellsOut[0].gridPosition.y, 0.0f));
    CHECK(approx(cellsOut[0].widthCells, 1.0f));
    CHECK(approx(cellsOut[0].foreground.x, 205.0f / 255.0f) && approx(cellsOut[0].foreground.y, 0.0f) && approx(cellsOut[0].foreground.z, 0.0f));
    CHECK(approx(cellsOut[0].background.x, 0x10 / 255.0f) && approx(cellsOut[0].background.z, 0x14 / 255.0f));
    CHECK(cellsOut[0].atlasBottomRight.x > cellsOut[0].atlasTopLeft.x); // glyph present

    // cellsOut[1] = (0,1) space: blank glyph (atlasTopLeft == atlasBottomRight).
    CHECK(approx(cellsOut[1].gridPosition.x, 1.0f));
    CHECK(approx(cellsOut[1].atlasTopLeft.x, cellsOut[1].atlasBottomRight.x) && approx(cellsOut[1].atlasTopLeft.y, cellsOut[1].atlasBottomRight.y));

    // cellsOut[2] = (0,2) wide CJK: widthCells 2, real glyph UV. (0,3) continuation skipped.
    CHECK(approx(cellsOut[2].gridPosition.x, 2.0f));
    CHECK(approx(cellsOut[2].widthCells, 2.0f));
    CHECK(cellsOut[2].atlasBottomRight.x > cellsOut[2].atlasTopLeft.x);

    // cellsOut[3] = (1,0) 'B' reverse: foreground/background swapped — background becomes the default foreground.
    CHECK(approx(cellsOut[3].gridPosition.x, 0.0f) && approx(cellsOut[3].gridPosition.y, 1.0f));
    CHECK(approx(cellsOut[3].background.x, 0xC0 / 255.0f) && approx(cellsOut[3].background.y, 0xC0 / 255.0f));
    CHECK(approx(cellsOut[3].foreground.x, 0x10 / 255.0f) && approx(cellsOut[3].foreground.z, 0x14 / 255.0f));

    // cellsOut[4] = (1,1) blank default cell still emits (background-only).
    CHECK(approx(cellsOut[4].gridPosition.x, 1.0f) && approx(cellsOut[4].gridPosition.y, 1.0f));
    CHECK(approx(cellsOut[4].atlasTopLeft.x, cellsOut[4].atlasBottomRight.x));
}

// The ideogram double underline is the SGR 4:2 pair reached by a different
// escape: the upper rule sits on the single rule's line and the lower hangs a
// gap below it, so one line of text shows one height of underline. The upper
// rule is held inside the cell; the lower may hang past the floor by at most
// a gap and a stroke — the SGR 4:2 lower's own reach — and never a row down,
// which is what a clamp of the upper rule alone once produced.
TEST(theIdeogramUnderlinePairHangsFromTheRuleLine)
{
    CellFixture fx;
    // Row 1 column 1, which the fixture leaves blank and undecorated, so the
    // rules counted below are this cell's and no other's. Column 0 of the
    // same row carries the fixture's SGR 4 underline: the line the pair hangs
    // from, read off that rule rather than restated from the metrics.
    setCell(fx.grid, fx.columns, 1, 1, 0x43u, {}, colorDefault, colorDefault);
    Ubiquitty::penSetIdeogram(&fx.grid[static_cast<std::size_t>(1 * fx.columns + 1)].pen,
        Ubiquitty::VT::ideogramDoubleUnderline);
    fx.counts = buildCellInstances(fx.in, UbiquittyTest::glyphBackendFor(fx.atlas), fx.buffers);

    const float cellTop = 1.0f * fx.in.metrics.cellHeight;
    const float cellBottom = cellTop + fx.in.metrics.cellHeight;
    const float cellLeft = 1.0f * fx.in.metrics.cellWidth;
    float singleTop = -1.0f;
    for (std::size_t i = 0; i < fx.counts.decorations && i < fx.decorationsOut.size(); ++i) {
        const DecorationInstance& rule = fx.decorationsOut[i];
        if (approx(rule.topLeftInPixels.x, 0.0f) && rule.topLeftInPixels.y >= cellTop && rule.topLeftInPixels.y < cellBottom)
            singleTop = rule.topLeftInPixels.y;
    }
    CHECK_MSG(singleTop >= 0.0f, "the fixture's own underline on (1,0) is the line to compare against");

    const float t = std::round(fx.in.metrics.underlineThickness);
    const float gap = Ubiquitty::Rendering::doubledRuleGap(t, fx.in.metrics.deviceScale);
    int upperRules = 0, lowerRules = 0;
    for (std::size_t i = 0; i < fx.counts.decorations && i < fx.decorationsOut.size(); ++i) {
        const DecorationInstance& rule = fx.decorationsOut[i];
        if (!approx(rule.topLeftInPixels.x, cellLeft))
            continue;
        if (rule.topLeftInPixels.y < cellTop || rule.topLeftInPixels.y >= cellBottom + t + gap)
            continue;
        if (approx(rule.topLeftInPixels.y, singleTop)) {
            ++upperRules;
            CHECK_MSG(rule.bottomRightInPixels.y <= cellBottom, "the upper rule ends at or above its cell's floor");
        } else if (approx(rule.topLeftInPixels.y, singleTop + t + gap)) {
            ++lowerRules;
            CHECK_MSG(rule.bottomRightInPixels.y <= cellBottom + t + gap, "the lower rule hangs no further than a gap and a stroke past the floor");
        } else {
            CHECK_MSG(false, "no rule of the pair lands anywhere else, a row down least of all");
        }
    }
    CHECK_MSG(upperRules == 1, "the pair's upper rule sits on the single rule's line");
    CHECK_MSG(lowerRules == 1, "the pair's lower rule hangs a gap below the upper");
}

TEST(testDECSCNMSwapsOnlyTheDefaultColors)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;

    std::vector<Ubiquitty::Cell> screen(2, blank);
    setCell(screen, 2, 0, 0, 0x41u, {}, /*foreground*/ 1, colorDefault); // 'A' red foreground, default background
    setCell(screen, 2, 0, 1, 0x42u, {}, colorDefault, colorDefault); // 'B' all-default
    BuildInputs reverse;
    reverse.cells = screen.data();
    reverse.rows = 1;
    reverse.columns = 2;
    reverse.palette = &pal;
    reverse.screenReverse = true;
    std::vector<CellInstance> reverseCells(2);
    std::vector<DecorationInstance> reverseDecorations(16);
    InstanceBuffers reverseBuffers { reverseCells.data(), reverseCells.size(), reverseDecorations.data(), reverseDecorations.size() };
    FakeAtlas reverseAtlas;
    buildCellInstances(reverse, UbiquittyTest::glyphBackendFor(reverseAtlas), reverseBuffers);
    // Explicit red foreground is not inverted; the default background now shows the swapped
    // default foreground (0xC0).
    CHECK(approx(reverseCells[0].foreground.x, 205.0f / 255.0f) && approx(reverseCells[0].foreground.y, 0.0f));
    CHECK(approx(reverseCells[0].background.x, 0xC0 / 255.0f));
    // The all-default cell inverts (defaults swap): foreground 0x10, background 0xC0.
    CHECK(approx(reverseCells[1].foreground.x, 0x10 / 255.0f) && approx(reverseCells[1].background.x, 0xC0 / 255.0f));
}

// Foreground palette-alpha × SGR 5/6 blink curve folded into foreground.w.
// Default inputs keep every foreground opaque (w == 1.0); an indexed/default
// foreground picks up its palette opacity; an RGB-direct foreground always
// stays 1.0; Attribute::Blink fades w along the shared blink curve when
// resolveBlink is set.
TEST(testForegroundAlphaFoldsIntoTheBlinkCurve)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;
    [[maybe_unused]] InstanceBuffers& buffers = fx.buffers;

    std::vector<Ubiquitty::Cell> row(4, blank);
    // col0: default fg; col1: indexed fg (palette idx 1); col2: RGB-direct
    // fg; col3: default fg + blink.
    row[0].codePoint = 0x41u;
    row[0].pen.foregroundIndex = colorDefault;
    row[1].codePoint = 0x42u;
    row[1].pen.foregroundIndex = 1; // ansi red
    row[2].codePoint = 0x43u;
    row[2].pen.foregroundIndex = colorRGB;
    row[2].pen.foregroundRed = 10;
    row[2].pen.foregroundGreen = 20;
    row[2].pen.foregroundBlue = 30;
    row[3].codePoint = 0x44u;
    row[3].pen.foregroundIndex = colorDefault;
    row[3].pen.attributes = Ubiquitty::VT::Attribute::Blink;
    for (auto& c : row) {
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }

    // (a) Defaults: paletteForegroundAlpha null, defaultForegroundAlpha 255,
    // resolveBlink off → every foreground fully opaque (byte-identical path).
    {
        BuildInputs a;
        a.cells = row.data();
        a.rows = 1;
        a.columns = 4;
        a.palette = &pal;
        std::vector<CellInstance> cells(4);
        std::vector<DecorationInstance> decos(32);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;
        buildCellInstances(a, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(approx(cells[0].foreground.w, 1.0f));
        CHECK(approx(cells[1].foreground.w, 1.0f));
        CHECK(approx(cells[2].foreground.w, 1.0f));
        CHECK(approx(cells[3].foreground.w, 1.0f));
    }

    // (b) Palette opacity: default fg half-opaque (128), indexed[1] at 64;
    // RGB-direct stays opaque regardless.
    {
        std::uint8_t indexedAlpha[256];
        for (int i = 0; i < 256; ++i)
            indexedAlpha[i] = 255;
        indexedAlpha[1] = 64;
        BuildInputs b;
        b.cells = row.data();
        b.rows = 1;
        b.columns = 4;
        b.palette = &pal;
        b.paletteForegroundAlpha = indexedAlpha;
        b.defaultForegroundAlpha = 128;
        std::vector<CellInstance> cells(4);
        std::vector<DecorationInstance> decos(32);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;
        buildCellInstances(b, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(approx(cells[0].foreground.w, 128.0f / 255.0f)); // default fg opacity
        CHECK(approx(cells[1].foreground.w, 64.0f / 255.0f)); // indexed[1] opacity
        CHECK(approx(cells[2].foreground.w, 1.0f)); // RGB-direct: no protocol alpha
        CHECK(approx(cells[3].foreground.w, 128.0f / 255.0f)); // blink off (resolveBlink unset)
    }

    // (c) Blink curve: resolveBlink at tick 0 is the solid hold (alpha 1.0),
    // so the blink cell stays opaque; at a tick in the off segment it drops
    // to 0. Folds with the default-fg opacity multiplicatively.
    {
        BuildInputs c;
        c.cells = row.data();
        c.rows = 1;
        c.columns = 4;
        c.palette = &pal;
        c.resolveBlink = true;
        c.defaultForegroundAlpha = 200;
        std::vector<CellInstance> cells(4);
        std::vector<DecorationInstance> decos(32);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;

        c.blinkTicks = 0; // solid hold → blink factor 1.0
        buildCellInstances(c, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(approx(cells[3].foreground.w, 200.0f / 255.0f)); // 200/255 * 1.0
        // A non-blink cell ignores resolveBlink.
        CHECK(approx(cells[0].foreground.w, 200.0f / 255.0f));

        // Tick 40 lands in the "off" segment (solid 32 + steps 6 → 38..53 off):
        // blink factor 0 → the blink cell's alpha collapses, the steady cell
        // keeps its palette opacity.
        c.blinkTicks = 45;
        std::vector<CellInstance> cells2(4);
        InstanceBuffers buffers2 { cells2.data(), cells2.size(), decos.data(), decos.size() };
        FakeAtlas at2;
        buildCellInstances(c, UbiquittyTest::glyphBackendFor(at2), buffers2);
        CHECK(approx(cells2[3].foreground.w, 0.0f)); // blink off-phase
        CHECK(approx(cells2[0].foreground.w, 200.0f / 255.0f)); // steady cell unaffected
    }
}

// DECDWL / DECDHL row geometry. A DOUBLE_WIDTH row paints each logical
// column at 2x advance (gridPosition.x == column*2, widthCells doubled),
// drops cells whose 2x lead starts past the last column, and clips the one
// straddling cell to the grid content width. DOUBLE_HEIGHT additionally
// halves the glyph atlas V range (top / bottom half).
TEST(testDoubleWidthAndDoubleHeightRowGeometry)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;
    [[maybe_unused]] InstanceBuffers& buffers = fx.buffers;
    [[maybe_unused]] const InstanceCounts counts = fx.counts;

    const int columns = 6;
    std::vector<Ubiquitty::Cell> row(columns, blank);
    for (int c = 0; c < columns; ++c) {
        row[c].codePoint = 0x41u + c; // A,B,C,...
        row[c].pen.foregroundIndex = colorDefault;
        row[c].pen.backgroundIndex = colorDefault;
        row[c].pen.underlineIndex = colorDefault;
    }
    // Underline col 0 so the decoration pass exercises the 2x cellLeft/right.
    row[0].pen.attributes = Ubiquitty::VT::Attribute::Underline;

    BuildInputs dwl;
    dwl.cells = row.data();
    dwl.rows = 1;
    dwl.columns = columns;
    dwl.palette = &pal;
    dwl.metrics.cellWidth = 8.0f;
    dwl.metrics.cellHeight = 16.0f;
    dwl.metrics.baselineFromTop = 13.0f;
    dwl.metrics.underlineThickness = 1.0f;
    dwl.metrics.underlineDrop = 1.0f;
    dwl.metrics.strikeThickness = 1.0f;
    dwl.metrics.xHeight = 10.0f;

    // (a) NORMAL row (null accessor): byte-identical single-width path —
    // column N at x==N, widthCells 1, all 6 cells emit.
    {
        std::vector<CellInstance> cells(columns);
        std::vector<DecorationInstance> decos(columns * 8);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;
        const InstanceCounts counts = buildCellInstances(dwl, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(counts.cells == 6);
        CHECK(approx(cells[0].gridPosition.x, 0.0f) && approx(cells[0].widthCells, 1.0f));
        CHECK(approx(cells[3].gridPosition.x, 3.0f) && approx(cells[3].widthCells, 1.0f));
    }

    // (b) DOUBLE_WIDTH row: column 0->x0, 1->x2, 2->x4; columns 3,4,5 have
    // their 2x lead at x6,x8,x10 — all >= columns(6) — so they drop. Three
    // cells survive, each widthCells 2.
    {
        const uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };
        dwl.lineAttributes = lineAttributes;
        std::vector<CellInstance> cells(columns);
        std::vector<DecorationInstance> decos(columns * 8);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;
        const InstanceCounts counts = buildCellInstances(dwl, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(counts.cells == 3); // cols 0,1,2 -> x 0,2,4; cols 3..5 dropped
        CHECK(approx(cells[0].gridPosition.x, 0.0f) && approx(cells[0].widthCells, 2.0f));
        CHECK(approx(cells[1].gridPosition.x, 2.0f) && approx(cells[1].widthCells, 2.0f));
        CHECK(approx(cells[2].gridPosition.x, 4.0f) && approx(cells[2].widthCells, 2.0f));
        // Decoration on col 0 spans the 2x advance box: [0, 16) in px.
        CHECK(counts.decorations == 1);
        CHECK(approx(decos[0].topLeftInPixels.x, 0.0f) && approx(decos[0].bottomRightInPixels.x, 16.0f));
        dwl.lineAttributes = nullptr;
    }

    // (c) Straddle clip: 5-column DOUBLE_WIDTH grid. Column 2 lands at x4
    // with full width 2 -> x4..x6, but the grid is 5 cells wide, so it is
    // clipped to width 1 (x4..x5) and its glyph UV trimmed to half.
    {
        const int cols5 = 5;
        std::vector<Ubiquitty::Cell> row5(cols5, blank);
        for (int c = 0; c < cols5; ++c) {
            row5[c].codePoint = 0x41u + c;
            row5[c].pen.foregroundIndex = colorDefault;
            row5[c].pen.backgroundIndex = colorDefault;
            row5[c].pen.underlineIndex = colorDefault;
        }
        const uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };
        BuildInputs s;
        s.cells = row5.data();
        s.rows = 1;
        s.columns = cols5;
        s.palette = &pal;
        s.metrics.cellWidth = 8.0f;
        s.metrics.cellHeight = 16.0f;
        s.metrics.baselineFromTop = 13.0f;
        s.lineAttributes = lineAttributes;
        std::vector<CellInstance> cells(cols5);
        std::vector<DecorationInstance> decos(cols5 * 8);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        FakeAtlas at;
        const InstanceCounts counts = buildCellInstances(s, UbiquittyTest::glyphBackendFor(at), buffers);
        // cols 0,1,2 -> x 0,2,4; col2 straddles (x4..x6 > 5) -> clipped to 1.
        CHECK(counts.cells == 3);
        CHECK(approx(cells[2].gridPosition.x, 4.0f));
        CHECK(approx(cells[2].widthCells, 1.0f)); // clipped from 2 to 1
        // Glyph UV trimmed by half (cellClipFrac 0.5). A DWL row fetches
        // the 2x master (FakeAtlas double band, U span 0.04), so the
        // trimmed span is half of that box.
        const float uSpan = cells[2].atlasBottomRight.x - cells[2].atlasTopLeft.x;
        CHECK(uSpan > 0.0f && approx(uSpan, 0.02f)); // half of 0.04
    }

    // (d) DOUBLE_HEIGHT top/bottom: the glyph atlas V range halves to the
    // top (atlasBottomRight.y at the V midpoint) / bottom (atlasTopLeft.y at the
    // midpoint) half, stretched to full cell height.
    {
        std::vector<Ubiquitty::Cell> one(1, blank);
        one[0].codePoint = 0x41u;
        one[0].pen.foregroundIndex = colorDefault;
        one[0].pen.backgroundIndex = colorDefault;
        one[0].pen.underlineIndex = colorDefault;
        BuildInputs h;
        h.cells = one.data();
        h.rows = 1;
        h.columns = 1;
        h.palette = &pal;
        h.metrics.cellWidth = 8.0f;
        h.metrics.cellHeight = 16.0f;

        const uint8_t topAttribute[1] = { Ubiquitty::lineAttributeDoubleHeightTop };
        h.lineAttributes = topAttribute;
        std::vector<CellInstance> topCells(1);
        std::vector<DecorationInstance> topDecorations(8);
        InstanceBuffers topBuffers { topCells.data(), topCells.size(), topDecorations.data(), topDecorations.size() };
        FakeAtlas atlasTop;
        buildCellInstances(h, UbiquittyTest::glyphBackendFor(atlasTop), topBuffers);
        // A DHL row fetches the 2x master for its region and uses the rect as
        // given: the backend returns [0.5, 0.52] for Top, and the builder does
        // not halve it again.
        CHECK(atlasTop.doubleCalls > 0);
        CHECK(approx(topCells[0].atlasTopLeft.y, 0.5f) && approx(topCells[0].atlasBottomRight.y, 0.52f));

        const uint8_t botAttribute[1] = { Ubiquitty::lineAttributeDoubleHeightBottom };
        h.lineAttributes = botAttribute;
        std::vector<CellInstance> bottomCells(1);
        std::vector<DecorationInstance> bottomDecorations(8);
        InstanceBuffers bottomBuffers { bottomCells.data(), bottomCells.size(), bottomDecorations.data(), bottomDecorations.size() };
        FakeAtlas atlasBottom;
        buildCellInstances(h, UbiquittyTest::glyphBackendFor(atlasBottom), bottomBuffers);
        // Bottom region of the same master, returned by the backend.
        CHECK(approx(bottomCells[0].atlasTopLeft.y, 0.52f) && approx(bottomCells[0].atlasBottomRight.y, 0.54f));

        // A backend with no 2x master (doubleGlyph not-present) falls back
        // to cropping the 1x glyph — the stretched look, never a blank.
        h.lineAttributes = topAttribute;
        std::vector<CellInstance> fallbackCells(1);
        std::vector<DecorationInstance> fallbackDecos(8);
        InstanceBuffers fallbackBuffers { fallbackCells.data(), fallbackCells.size(), fallbackDecos.data(), fallbackDecos.size() };
        FakeAtlas atFallback;
        atFallback.hasDoubleMasters = false;
        buildCellInstances(h, UbiquittyTest::glyphBackendFor(atFallback), fallbackBuffers);
        // The fallback glyph comes from the mono SLOT CACHE, not from
        // monoGlyph(): buildCellInstances tries doubleGlyph, then cluster,
        // then wide, then the cache, and never asks for a per-glyph mono on
        // this path. So the expectation is derived from the slot geometry
        // the fake declares rather than written down — a literal here would
        // only say the builder agrees with whatever it last produced.
        //
        // DECDHL halves the master's V range, so the top attribute must show
        // the upper half of the slot's span.
        CHECK(atFallback.doubleCalls > 0);
        CHECK_MSG(fallbackCells[0].atlasBottomRight.y > fallbackCells[0].atlasTopLeft.y,
            "the fallback crops the 1x glyph rather than leaving a blank");
        {
            const Ubiquitty::Rendering::MonoAtlasBinding binding = atFallback.bindMonoAtlas();
            const Ubiquitty::Rendering::GlyphUV full
                = binding.geometry.atlasRectForBuilder(/*slot=*/0, binding.slotCapacity);
            CHECK_MSG(approx(fallbackCells[0].atlasTopLeft.y, full.minV),
                "the top half starts where the slot does");
            CHECK_MSG(approx(fallbackCells[0].atlasBottomRight.y, (full.minV + full.maxV) * 0.5f),
                "and ends halfway down it");
        }
    }
}

// Decoration neighbor fields + OSC 8 link styling. The builder zero-fills
// the cross-cell skip-ink fields (glyphSlotTopLeftUV/glyphSlotBottomRightUV + left/right Ink/Box)
// when there is no ink to tap (single-cell correct). A flagged link cell with no SGR underline
// draws its rule in `linkUnderlineStyle`, and takes `linkColor` for both text
// and rule.
//
// The style is asserted against the INPUT rather than against a fixed kind. A
// builder that hardcoded one style passed an assertion naming that same style
// while disagreeing with every frontend whose setting said otherwise, which is
// how the divergence survived: the test agreed with the code instead of with the
// contract.
TEST(testDecorationNeighborFieldsAndLinkStyling)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;

    const int cols = 3;
    std::vector<Ubiquitty::Cell> row(cols, blank);
    for (int c = 0; c < cols; ++c) {
        row[c].codePoint = 0x41u + c;
        row[c].pen.foregroundIndex = colorDefault;
        row[c].pen.backgroundIndex = colorDefault;
        row[c].pen.underlineIndex = colorDefault;
    }
    // Column 1 is a link cell (no SGR underline of its own).
    const std::uint8_t linkFlags[cols] = { 0, 1, 0 };

    BuildInputs link;
    link.cells = row.data();
    link.rows = 1;
    link.columns = cols;
    link.palette = &pal;
    link.metrics.cellWidth = 8.0f;
    link.metrics.cellHeight = 16.0f;
    link.metrics.baselineFromTop = 13.0f;
    link.metrics.underlineThickness = 1.0f;
    link.metrics.underlineDrop = 1.0f;
    link.metrics.strikeThickness = 1.0f;
    link.metrics.xHeight = 10.0f;
    link.linkCells = linkFlags;
    link.linkUnderlineStyle = Ubiquitty::VT::underlineDotted;
    link.linkColor = { 0.2f, 0.4f, 1.0f, 1.0f }; // link blue

    std::vector<CellInstance> cells(cols);
    std::vector<DecorationInstance> decos(cols * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    FakeAtlas at;
    const InstanceCounts counts = buildCellInstances(link, UbiquittyTest::glyphBackendFor(at), buffers);

    // Only the link cell underlines (one dotted rule); the non-link cells
    // emit no decoration.
    CHECK(counts.decorations == 1);
    CHECK(decos[0].kind == static_cast<float>(DecorationKind::Dotted));
    // The rule color is the forced link blue.
    CHECK(approx(decos[0].color.x, 0.2f) && approx(decos[0].color.y, 0.4f) && approx(decos[0].color.z, 1.0f));
    // The link cell's text is forced to the link color too.
    CHECK(approx(cells[1].foreground.x, 0.2f) && approx(cells[1].foreground.y, 0.4f) && approx(cells[1].foreground.z, 1.0f));
    // Non-link cells keep the default foreground.
    CHECK(approx(cells[0].foreground.x, 0xC0 / 255.0f));
    // The cross-cell skip-ink fields are zero (absent neighbour), so the rule
    // renders as a plain rule.
    CHECK(approx(decos[0].glyphSlotTopLeftUV.x, 0.0f) && approx(decos[0].glyphSlotBottomRightUV.x, 0.0f));
    CHECK(approx(decos[0].leftInk.x, 0.0f) && approx(decos[0].leftBox.x, 0.0f));
    CHECK(approx(decos[0].rightInk.x, 0.0f) && approx(decos[0].rightBox.x, 0.0f));

    // The same link cell under the other setting draws a solid rule. Asserted as
    // a pair, because either kind alone is satisfied by a builder that ignores
    // the input and always emits that one.
    {
        BuildInputs solidLink = link;
        solidLink.linkUnderlineStyle = Ubiquitty::VT::underlineSingle;
        std::vector<CellInstance> cellsSolid(cols);
        std::vector<DecorationInstance> decosSolid(cols * 8);
        InstanceBuffers buffersSolid { cellsSolid.data(), cellsSolid.size(), decosSolid.data(), decosSolid.size() };
        FakeAtlas atSolid;
        const InstanceCounts countsSolid = buildCellInstances(solidLink, UbiquittyTest::glyphBackendFor(atSolid), buffersSolid);
        CHECK(countsSolid.decorations == 1);
        CHECK(decosSolid[0].kind == static_cast<float>(DecorationKind::Solid));
        CHECK(decosSolid[0].kind != decos[0].kind);
    }

    // A link cell whose pen names its OWN underline colour still draws the rule
    // in linkColor.
    //
    // This is the only case that can see the link colour rule at all. With
    // `colorDefault` the underline follows the text foreground, and the text is
    // already linkColor — so the rule arrives at link blue whether or not
    // anything forces it, and an assertion there passes for a reason unrelated to
    // what it names. Without this case, deleting the link-underline override
    // leaves the whole suite green.
    {
        std::vector<Ubiquitty::Cell> penRow = row;
        penRow[1].pen.underlineIndex = 1; // a palette red, nothing like link blue
        BuildInputs penLink = link;
        penLink.cells = penRow.data();
        std::vector<CellInstance> cellsPen(cols);
        std::vector<DecorationInstance> decosPen(cols * 8);
        InstanceBuffers buffersPen { cellsPen.data(), cellsPen.size(), decosPen.data(), decosPen.size() };
        FakeAtlas atPen;
        const InstanceCounts countsPen = buildCellInstances(penLink, UbiquittyTest::glyphBackendFor(atPen), buffersPen);
        CHECK(countsPen.decorations == 1);
        CHECK(approx(decosPen[0].color.x, 0.2f) && approx(decosPen[0].color.y, 0.4f) && approx(decosPen[0].color.z, 1.0f));
    }

    // A cell carrying its own SGR underline keeps that style: the link affordance
    // does not overwrite a rule the content asked for.
    {
        std::vector<Ubiquitty::Cell> sgrRow = row;
        sgrRow[1].pen.underlineStyle = Ubiquitty::VT::underlineCurly;
        sgrRow[1].pen.attributes = Ubiquitty::VT::Attribute::Underline;
        BuildInputs sgrLink = link;
        sgrLink.cells = sgrRow.data();
        std::vector<CellInstance> cellsSGR(cols);
        std::vector<DecorationInstance> decosSGR(cols * 8);
        InstanceBuffers buffersSGR { cellsSGR.data(), cellsSGR.size(), decosSGR.data(), decosSGR.size() };
        FakeAtlas atSGR;
        const InstanceCounts countsSGR = buildCellInstances(sgrLink, UbiquittyTest::glyphBackendFor(atSGR), buffersSGR);
        CHECK(countsSGR.decorations == 1);
        CHECK(decosSGR[0].kind == static_cast<float>(DecorationKind::Curly));
    }

    // No link flags (null accessor): byte-identical — no underline at all
    // (the cells carry no SGR underline).
    {
        BuildInputs noLink = link;
        noLink.linkCells = nullptr;
        std::vector<CellInstance> cells2(cols);
        std::vector<DecorationInstance> decos2(cols * 8);
        InstanceBuffers buffers2 { cells2.data(), cells2.size(), decos2.data(), decos2.size() };
        FakeAtlas at2;
        const InstanceCounts counts2 = buildCellInstances(noLink, UbiquittyTest::glyphBackendFor(at2), buffers2);
        CHECK(counts2.decorations == 0);
        CHECK(approx(cells2[1].foreground.x, 0xC0 / 255.0f)); // default fg, no override
    }
}

// Cursor overlay pass. A FOCUSED BLOCK is the cursor tier — its cell repeated
// at the cell stream's tail with the cursor colour, no overlay quad; a bar /
// underline / unfocused-hollow cursor emits one OverlayInstance via
// OverlayGeometry::buildCursorQuad.
TEST(testCursorOverlayPass)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;
    [[maybe_unused]] InstanceBuffers& buffers = fx.buffers;
    [[maybe_unused]] const InstanceCounts counts = fx.counts;

    std::vector<Ubiquitty::Cell> screen(4, blank);
    BuildInputs current;
    current.cells = screen.data();
    current.rows = 1;
    current.columns = 4;
    current.palette = &pal;
    current.metrics.cellWidth = 10.0f;
    current.metrics.cellHeight = 22.0f;
    current.metrics.baselineFromTop = 17.0f;
    current.cursor.row = 0;
    current.cursor.column = 1;
    current.cursor.mixA = 1.0f;
    current.cursor.shouldInvertText = true;
    current.cursor.rgb = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Point domain == pixel domain (the Linux frontend's setup): 1 px/pt, no
    // line spacing, so the bar/underline thickness reduces to plain pixels.
    current.metrics.cellHeight = 22.0f;

    auto runCursor = [&](Ubiquitty::Cursor::Shape shape, bool focused, std::vector<CellInstance>& cells, std::vector<OverlayInstance>& overlays) {
        current.cursor.shape = shape;
        current.cursor.focused = focused;
        // Four grid cells and room for the tier's tail.
        cells.assign(6, CellInstance {});
        overlays.assign(2, OverlayInstance {});
        std::vector<DecorationInstance> decorations(32);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
        FakeAtlas atlasLocal;
        return buildCellInstances(current, UbiquittyTest::glyphBackendFor(atlasLocal), buffers);
    };

    // Focused block: the grid's cell (1,0) is untouched, the tier repeats it
    // at the tail with the background lerped to the cursor colour, and no
    // overlay is emitted.
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        const InstanceCounts counts = runCursor(Ubiquitty::Cursor::Shape::Block, true, cells, overlays);
        CHECK(counts.overlays == 0);
        CHECK(counts.cursorTier.cells == 1);
        CHECK(!(approx(cells[1].background.x, 1.0f) && approx(cells[1].background.y, 1.0f) && approx(cells[1].background.z, 1.0f)));
        CHECK(counts.cells >= 2 && counts.cells <= cells.size());
        const CellInstance& tier = cells[counts.cells - 1];
        CHECK(approx(tier.gridPosition.x, 1.0f) && approx(tier.gridPosition.y, 0.0f));
        CHECK(approx(tier.background.x, 1.0f) && approx(tier.background.y, 1.0f) && approx(tier.background.z, 1.0f));
    }
    // Bar: one overlay quad, narrow (≈2 px wide), filled (strokeWidth 0), at
    // the cursor cell's left edge (column 1 → x ≈ 10).
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        const InstanceCounts counts = runCursor(Ubiquitty::Cursor::Shape::Bar, true, cells, overlays);
        CHECK(counts.overlays == 1);
        CHECK(approx(overlays[0].strokeWidth, 0.0f));
        // 2 pt thick, and this case's domain is 1 px/pt, so exactly 2 px.
        // Pinned as a value rather than a range: a range loose enough to
        // accept 4 px also accepts a doubled pixelsPerPoint, which is the
        // arithmetic the thickness exists to check.
        const float thicknessInPixels = 2.0f;
        const float widthInPixels = overlays[0].bottomRightInPixels.x - overlays[0].topLeftInPixels.x;
        CHECK(approx(widthInPixels, thicknessInPixels));
        // CENTRED on the cursor cell's left edge (column 1 x cellWidth 10),
        // not butted up against it — half the bar sits over the cell before.
        CHECK(approx(overlays[0].topLeftInPixels.x, 10.0f - thicknessInPixels * 0.5f));
        CHECK(approx(overlays[0].bottomRightInPixels.x, 10.0f + thicknessInPixels * 0.5f));
        // The bar spans the font's line box, which with no line spacing is
        // the whole cell — a bar inset by a phantom vpad would shorten here.
        CHECK(approx(overlays[0].topLeftInPixels.y, 0.0f) && approx(overlays[0].bottomRightInPixels.y, 22.0f));
        // No tier for a bar: the cell background stays the default.
        CHECK(approx(cells[1].background.x, 0x10 / 255.0f));
    }
    // Underline: one overlay quad, short and wide, near the cell bottom.
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        const InstanceCounts counts = runCursor(Ubiquitty::Cursor::Shape::Underline, true, cells, overlays);
        CHECK(counts.overlays == 1);
        CHECK(approx(overlays[0].strokeWidth, 0.0f));
        // The bar's thickness rotated: 2 pt, so 2 px in this domain.
        const float thicknessInPixels = 2.0f;
        const float heightInPixels = overlays[0].bottomRightInPixels.y - overlays[0].topLeftInPixels.y;
        CHECK(approx(heightInPixels, thicknessInPixels));
        // Sits ON the font box bottom, which without line spacing is the cell
        // bottom (22) — vpad applied here would drop it below the glyph.
        CHECK(approx(overlays[0].bottomRightInPixels.y, 22.0f));
        CHECK(approx(overlays[0].topLeftInPixels.y, 22.0f - thicknessInPixels));
        // Spanning the cursor cell edge to edge (column 1 -> x 10..20).
        CHECK(approx(overlays[0].topLeftInPixels.x, 10.0f) && approx(overlays[0].bottomRightInPixels.x, 20.0f));
    }
    // Unfocused block → hollow outline: one overlay quad with strokeWidth > 0.
    {
        std::vector<CellInstance> cells;
        std::vector<OverlayInstance> overlays;
        const InstanceCounts counts = runCursor(Ubiquitty::Cursor::Shape::Block, false, cells, overlays);
        CHECK(counts.overlays == 1);
        CHECK(overlays[0].strokeWidth > 0.0f);
        // Unfocused: no tier, and the cell background stays the default.
        CHECK(approx(cells[1].background.x, 0x10 / 255.0f));
    }
}

// Selection overlay pass. Each selected on-screen row span emits one
// translucent FILLED overlay quad (cellSpanQuad) — no inversion of the
// cells, no stroke/corner. Two spans (rows 0 and 1) → two overlay quads at
// the span's pixel rect in the selection color; the cells are untouched.
TEST(testSelectionOverlayPass)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;
    [[maybe_unused]] const Ubiquitty::Cell& blank = fx.blank;

    std::vector<Ubiquitty::Cell> screen(8, blank); // 2 rows × 4 columns
    BuildInputs selection;
    selection.cells = screen.data();
    selection.rows = 2;
    selection.columns = 4;
    selection.palette = &pal;
    selection.metrics.cellWidth = 10.0f;
    selection.metrics.cellHeight = 20.0f;
    selection.metrics.baselineFromTop = 16.0f;
    const SelectionRowSpan spans[] = { { 0, 1, 3 }, { 1, 0, 2 } };
    selection.selectionSpans = spans;
    selection.selectionSpanCount = 2;
    selection.selectionColor = { 0.62f, 0.74f, 0.93f, 0.40f };

    std::vector<CellInstance> cells(8, CellInstance {});
    std::vector<DecorationInstance> decorations(32);
    std::vector<OverlayInstance> overlays(4, OverlayInstance {});
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
    FakeAtlas selectionAtlas;
    const InstanceCounts counts = buildCellInstances(selection, UbiquittyTest::glyphBackendFor(selectionAtlas), buffers);

    // No cursor this frame, so the only overlays are the two selection spans.
    CHECK(counts.overlays == 2);
    // Span 0 (row 0, columns 1..3): x in [10,40), y in [0,20), filled, in color.
    CHECK(approx(overlays[0].topLeftInPixels.x, 10.0f) && approx(overlays[0].bottomRightInPixels.x, 40.0f));
    CHECK(approx(overlays[0].topLeftInPixels.y, 0.0f) && approx(overlays[0].bottomRightInPixels.y, 20.0f));
    CHECK(approx(overlays[0].color.w, 0.40f)); // translucent, not opaque
    CHECK(approx(overlays[0].strokeWidth, 0.0f) && approx(overlays[0].cornerRadius, 0.0f));
    // Span 1 (row 1, columns 0..2): x in [0,30), y in [20,40).
    CHECK(approx(overlays[1].topLeftInPixels.x, 0.0f) && approx(overlays[1].bottomRightInPixels.x, 30.0f));
    CHECK(approx(overlays[1].topLeftInPixels.y, 20.0f) && approx(overlays[1].bottomRightInPixels.y, 40.0f));
    // The cells are not inverted by the selection — foreground/background stay the defaults.
    CHECK(approx(cells[1].background.x, 0x10 / 255.0f) && approx(cells[1].foreground.x, 0xC0 / 255.0f));
}

// OSC 19 selection-foreground: when selectionForegroundOverride is set, every
// cell inside a selection span draws its glyph in selectionForeground; cells
// outside the span keep their resolved (default / SGR) color. The translucent
// selection overlay is still emitted on top (one quad for the span). With the
// override OFF the foreground is unchanged (parity with the no-OSC-19 path).
TEST(testSelectionForegroundOverrideAppliesInsideTheSpan)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;

    // One row of 4 glyphs ('A'..'D'), all default foreground (0xC0 per channel).
    std::vector<Ubiquitty::Cell> screen(4);
    Ubiquitty::Cell b {};
    b.pen.foregroundIndex = colorDefault;
    b.pen.backgroundIndex = colorDefault;
    b.pen.underlineIndex = colorDefault;
    for (auto& c : screen)
        c = b;
    setCell(screen, 4, 0, 0, 0x41u, {}, colorDefault, colorDefault);
    setCell(screen, 4, 0, 1, 0x42u, {}, colorDefault, colorDefault);
    setCell(screen, 4, 0, 2, 0x43u, {}, colorDefault, colorDefault);
    setCell(screen, 4, 0, 3, 0x44u, {}, colorDefault, colorDefault);

    BuildInputs sel;
    sel.cells = screen.data();
    sel.rows = 1;
    sel.columns = 4;
    sel.palette = &pal;
    sel.metrics.cellWidth = 10.0f;
    sel.metrics.cellHeight = 20.0f;
    sel.metrics.baselineFromTop = 16.0f;
    // Select columns 1..2 (the 'B' and 'C' cells); 0 and 3 stay unselected.
    const SelectionRowSpan spans[] = { { 0, 1, 2 } };
    sel.selectionSpans = spans;
    sel.selectionSpanCount = 1;
    sel.selectionColor = { 0.62f, 0.74f, 0.93f, 0.40f };
    sel.selectionForegroundOverride = true;
    sel.selectionForeground = { 1.0f, 0.0f, 0.0f, 1.0f }; // OSC 19 red

    std::vector<CellInstance> cells(4, CellInstance {});
    std::vector<DecorationInstance> decorations(32);
    std::vector<OverlayInstance> overlays(4, OverlayInstance {});
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
    FakeAtlas selAtlas;
    const InstanceCounts counts = buildCellInstances(sel, UbiquittyTest::glyphBackendFor(selAtlas), buffers);

    // Cells emit in column order (no wide/continuation here): [0]=A [1]=B [2]=C [3]=D.
    CHECK(counts.cells == 4);
    // Unselected 'A' (col 0): keeps the default foreground (0xC0).
    CHECK(approx(cells[0].foreground.x, 0xC0 / 255.0f) && approx(cells[0].foreground.y, 0xC0 / 255.0f));
    // Selected 'B' (col 1) + 'C' (col 2): foreground is the OSC 19 red override.
    CHECK(approx(cells[1].foreground.x, 1.0f) && approx(cells[1].foreground.y, 0.0f) && approx(cells[1].foreground.z, 0.0f));
    CHECK(approx(cells[2].foreground.x, 1.0f) && approx(cells[2].foreground.y, 0.0f) && approx(cells[2].foreground.z, 0.0f));
    // Unselected 'D' (col 3): keeps the default foreground.
    CHECK(approx(cells[3].foreground.x, 0xC0 / 255.0f) && approx(cells[3].foreground.y, 0xC0 / 255.0f));
    // The translucent selection overlay is still emitted (one quad).
    CHECK(counts.overlays == 1);
    CHECK(approx(overlays[0].color.w, 0.40f));

    // Override OFF: the same selection leaves every cell's foreground at the
    // resolved default — byte-identical to the no-OSC-19 path.
    BuildInputs off = sel;
    off.selectionForegroundOverride = false;
    std::vector<CellInstance> cellsOff(4, CellInstance {});
    std::vector<OverlayInstance> overlaysOff(4, OverlayInstance {});
    InstanceBuffers buffersOff { cellsOff.data(), cellsOff.size(), decorations.data(), decorations.size(), overlaysOff.data(), overlaysOff.size() };
    FakeAtlas offAtlas;
    buildCellInstances(off, UbiquittyTest::glyphBackendFor(offAtlas), buffersOff);
    for (int i = 0; i < 4; ++i)
        CHECK(approx(cellsOff[i].foreground.x, 0xC0 / 255.0f));
}

// A selection behind the text is each selected cell's background, opaque, with
// the glyph drawn over it in its own colour — the way a text view draws one —
// and no overlay quad, which would cover that glyph with the opaque fill.
TEST(testASelectionBehindTheTextIsTheSelectedCellsBackground)
{
    CellFixture fx;
    [[maybe_unused]] TermPalette& pal = fx.pal;

    std::vector<Ubiquitty::Cell> screen(4);
    for (int column = 0; column < 4; ++column)
        setCell(screen, 4, 0, column, static_cast<std::uint32_t>('A' + column), {}, colorDefault, colorDefault);

    BuildInputs sel;
    sel.cells = screen.data();
    sel.rows = 1;
    sel.columns = 4;
    sel.palette = &pal;
    sel.metrics.cellWidth = 10.0f;
    sel.metrics.cellHeight = 20.0f;
    sel.metrics.baselineFromTop = 16.0f;
    const SelectionRowSpan spans[] = { { 0, 1, 2 } };
    sel.selectionSpans = spans;
    sel.selectionSpanCount = 1;
    sel.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };
    sel.isSelectionBehindText = true;

    std::vector<CellInstance> cells(4, CellInstance {});
    std::vector<DecorationInstance> decorations(32);
    std::vector<OverlayInstance> overlays(4, OverlayInstance {});
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(sel, UbiquittyTest::glyphBackendFor(atlas), buffers);

    CHECK(counts.cells == 4);
    CHECK(approx(cells[0].background.x, 0x10 / 255.0f));
    CHECK(approx(cells[1].background.x, 0.70f) && approx(cells[1].background.z, 1.0f) && approx(cells[1].background.w, 1.0f));
    CHECK(approx(cells[2].background.y, 0.84f) && approx(cells[2].background.w, 1.0f));
    CHECK(approx(cells[3].background.x, 0x10 / 255.0f));
    CHECK_MSG(approx(cells[1].foreground.x, 0xC0 / 255.0f), "the glyph keeps its own colour");

    // The fill's alpha is how much of it lies over the cell's own
    // background: at half, a selected cell is halfway from its background to
    // the selection colour, and stays opaque.
    sel.selectionColor = { 0.70f, 0.84f, 1.0f, 0.5f };
    const InstanceCounts washed = buildCellInstances(sel, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(washed.cells == 4);
    CHECK_MSG(approx(cells[1].background.x, (cells[0].background.x + 0.70f) / 2.0f) && approx(cells[1].background.z, (cells[0].background.z + 1.0f) / 2.0f),
        "a half-opaque fill washes the cell halfway from its own background to the selection colour");
    CHECK(approx(cells[1].background.w, 1.0f));
    CHECK(approx(cells[0].background.x, 0x10 / 255.0f));
    CHECK_MSG(counts.overlays == 0, "no quad covers the glyphs");

    // The same selection drawn over the text keeps the cells and adds the quad.
    BuildInputs over = sel;
    over.selectionColor = { 0.40f, 0.55f, 0.85f, 0.35f };
    over.isSelectionBehindText = false;
    std::vector<CellInstance> cellsOver(4, CellInstance {});
    std::vector<OverlayInstance> overlaysOver(4, OverlayInstance {});
    InstanceBuffers buffersOver { cellsOver.data(), cellsOver.size(), decorations.data(), decorations.size(), overlaysOver.data(), overlaysOver.size() };
    FakeAtlas overAtlas;
    const InstanceCounts countsOver = buildCellInstances(over, UbiquittyTest::glyphBackendFor(overAtlas), buffersOver);
    CHECK(approx(cellsOver[1].background.x, 0x10 / 255.0f));
    CHECK(countsOver.overlays == 1);
}
// A row a rich text block crosses is selected in two pieces, one each side
// of the block, which draws its own highlight on its glyphs. Behind the text
// both pieces fill their cells, and the covered cells between them take none.
TEST(testASelectionSplitAroundABlockFillsBothPiecesBehindTheText)
{
    CellFixture fx;
    std::vector<Ubiquitty::Cell> screen(6);
    for (int column = 0; column < 6; ++column)
        setCell(screen, 6, 0, column, static_cast<std::uint32_t>('A' + column), {}, colorDefault, colorDefault);
    for (const int column : { 2, 3 }) {
        Ubiquitty::Pen pen = Ubiquitty::cellPen(screen[static_cast<std::size_t>(column)]);
        penSetRichTextCovered(&pen, true);
        Ubiquitty::cellSetPen(screen[static_cast<std::size_t>(column)], pen);
    }

    BuildInputs in;
    in.cells = screen.data();
    in.rows = 1;
    in.columns = 6;
    in.palette = &fx.pal;
    in.metrics.cellWidth = 10.0f;
    in.metrics.cellHeight = 20.0f;
    in.metrics.baselineFromTop = 16.0f;
    const SelectionRowSpan spans[] = { { 0, 0, 1 }, { 0, 4, 5 } };
    in.selectionSpans = spans;
    in.selectionSpanCount = 2;
    in.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };
    in.isSelectionBehindText = true;

    std::vector<CellInstance> cells(6, CellInstance {});
    std::vector<DecorationInstance> decorations(48);
    std::vector<OverlayInstance> overlays(4, OverlayInstance {});
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.cells == 6);
    for (const int column : { 0, 1, 4, 5 })
        CHECK_MSG(approx(cells[static_cast<std::size_t>(column)].background.x, 0.70f), "a cell of either piece is filled");
    for (const int column : { 2, 3 })
        CHECK_MSG(approx(cells[static_cast<std::size_t>(column)].background.x, 0x10 / 255.0f), "a covered cell between the pieces is not");
}

// A row selected in two pieces fills the cells of each and none between them,
// whatever those cells hold: the hull of the pieces is not the selection. The
// pieces a sized block adds on the rows the drag does not reach stand apart
// from the drag's own span the same way.
TEST(testASelectionInTwoPiecesLeavesTheCellsBetweenThemUnfilled)
{
    CellFixture fx;
    std::vector<Ubiquitty::Cell> screen(6);
    for (int column = 0; column < 6; ++column)
        setCell(screen, 6, 0, column, static_cast<std::uint32_t>('A' + column), {}, colorDefault, colorDefault);

    BuildInputs in;
    in.cells = screen.data();
    in.rows = 1;
    in.columns = 6;
    in.palette = &fx.pal;
    in.metrics.cellWidth = 10.0f;
    in.metrics.cellHeight = 20.0f;
    in.metrics.baselineFromTop = 16.0f;
    // Out of order, as a caller that appends a piece after the row's own span.
    const SelectionRowSpan spans[] = { { 0, 4, 5 }, { 0, 0, 1 } };
    in.selectionSpans = spans;
    in.selectionSpanCount = 2;
    in.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };
    in.isSelectionBehindText = true;

    std::vector<CellInstance> cells(6, CellInstance {});
    std::vector<DecorationInstance> decorations(48);
    std::vector<OverlayInstance> overlays(4, OverlayInstance {});
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size(), overlays.data(), overlays.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.cells == 6);
    for (const int column : { 0, 1, 4, 5 })
        CHECK_MSG(approx(cells[static_cast<std::size_t>(column)].background.x, 0.70f), "a cell of either piece is filled");
    for (const int column : { 2, 3 })
        CHECK_MSG(approx(cells[static_cast<std::size_t>(column)].background.x, 0x10 / 255.0f), "a cell between the pieces is not");
}

// A DECDWL/DECDHL master is rasterised at its foreground's tier, as the mono
// and wide pages are: the dilation steps with the ink's luminance, so one
// master per glyph would draw a double-width row at the weight of whichever
// colour faulted it first.
namespace {
std::uint32_t doubleVariantSeen = 0;
}

TEST(testADoubleMasterIsAskedForAtItsForegroundsTier)
{
    CellFixture fx;
    std::vector<Ubiquitty::Cell> row(1);
    const uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };
    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &fx.pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.lineAttributes = lineAttributes;
    in.hasRasterTiers = true;
    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    FakeAtlas atlas;
    GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.doubleGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t variant, DoubleRegion) {
        doubleVariantSeen = variant;
        return AtlasGlyph { { 0.1f, 0.1f }, { 0.2f, 0.2f }, true };
    };

    setCell(row, 1, 0, 0, 0x41u, {}, 0, colorDefault);
    buildCellInstances(in, backend, buffers);
    const std::uint32_t darkTier = variantTier(doubleVariantSeen);

    setCell(row, 1, 0, 0, 0x41u, {}, 15, colorDefault);
    buildCellInstances(in, backend, buffers);
    CHECK_MSG(variantTier(doubleVariantSeen) > darkTier, "a light ink asks for a heavier master than a dark one");
}
// The DECDWL/DECDHL stream. A backend that keeps 2x masters on a sheet of their
// own takes the doubled glyph off the cell, and the quad it gets is not the
// cell: two cells wide, half a master slot high, margins backed off doubled.
//
// The midline case is the one worth pinning. A DHL pair is two independent grid
// rows sampling two halves of one master, and they meet only if the top quad's
// bottom edge lands exactly on the bottom row's top. Geometry that is merely
// close still draws a legible letter with a hairline through it, at every DHL
// boundary, with every count in the frame correct.
TEST(testDoubledGlyphsRideTheirOwnStream)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    // Two rows of two columns: a doubled cell spans two cells, so column 0 holds
    // the whole glyph and column 1 is past the grid. One quad per row, and the
    // straddle clip stays out of it.
    std::vector<Ubiquitty::Cell> grid(4, blank);
    for (int i = 0; i < 4; ++i) {
        grid[i].codePoint = 0x41u;
        grid[i].pen.foregroundIndex = colorDefault;
        grid[i].pen.backgroundIndex = colorDefault;
        grid[i].pen.underlineIndex = colorDefault;
    }
    const uint8_t pairAttributes[2] = { Ubiquitty::lineAttributeDoubleHeightTop, Ubiquitty::lineAttributeDoubleHeightBottom };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 2;
    in.columns = 2;
    in.palette = &pal;
    in.lineAttributes = pairAttributes;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.glyphMarginInPixels.width = 1.0f;
    in.glyphMarginInPixels.height = 2.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    // The slot the atlas rasterises a 2x master into: two cell boxes wide and
    // two cell boxes tall, each with its own doubled margins.
    in.glyphDoubleSlotWidthInPixels = 2.0f * (in.metrics.cellWidth + 2.0f * in.glyphMarginInPixels.width);
    in.glyphDoubleSlotHeightInPixels = 2.0f * (in.metrics.cellHeight + 2.0f * in.glyphMarginInPixels.height);

    std::vector<CellInstance> cells(4);
    std::vector<DecorationInstance> decorations(16);
    std::vector<GlyphInstance> doubles(4);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.doubleGlyphs = doubles.data();
    buffers.doubleGlyphCapacity = doubles.size();
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    CHECK_MSG(counts.doubleGlyphs == 2, "one quad per doubled row, and the past-the-grid column emits none");
    // The cell keeps its background and hands the glyph over, exactly as the
    // wide and colour sheets do.
    for (std::size_t i = 0; i < counts.cells; ++i)
        CHECK_MSG(approx(cells[i].atlasTopLeft.y, cells[i].atlasBottomRight.y),
            "a cell whose glyph went to the double stream carries no UV");

    // scaleX and scaleY are 1 here (cell == reference), so the expectations are
    // the geometry rather than the geometry times a factor.
    const GlyphInstance& top = doubles[0];
    const GlyphInstance& bottom = doubles[1];
    CHECK(approx(top.originInPixels.x, -2.0f) && approx(bottom.originInPixels.x, -2.0f));
    CHECK(approx(top.sizeInPixels.x, 20.0f) && approx(top.sizeInPixels.y, 20.0f));
    CHECK(approx(bottom.sizeInPixels.x, 20.0f) && approx(bottom.sizeInPixels.y, 20.0f));
    // Top backs off by the doubled top margin; Bottom starts at its own row top.
    CHECK(approx(top.originInPixels.y, -4.0f));
    CHECK(approx(bottom.originInPixels.y, 16.0f));
    CHECK_MSG(approx(top.originInPixels.y + top.sizeInPixels.y, bottom.originInPixels.y),
        "the two halves meet on the pair's shared row boundary");
    // Each half samples its own band of the master; the backend applied the
    // region, so the builder must not have halved it again.
    CHECK(top.atlasBottomRight.y < bottom.atlasTopLeft.y + 1e-4f);

    // Without a stream to write to, the master stays baked on the cell — the
    // behaviour every caller had before the lane existed, and the reason the
    // lane can be added without touching a backend that does not want it.
    std::vector<CellInstance> bakedCells(4);
    InstanceBuffers baked { bakedCells.data(), bakedCells.size(), decorations.data(), decorations.size() };
    FakeAtlas bakedAtlas;
    const InstanceCounts bakedCounts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(bakedAtlas), baked);
    CHECK(bakedCounts.doubleGlyphs == 0);
    CHECK_MSG(bakedCells[0].atlasBottomRight.y > bakedCells[0].atlasTopLeft.y,
        "a null stream leaves the master on the cell rather than blanking it");

    // A stream offered with no slot dimensions is the same case: the quad cannot
    // be sized, so the glyph stays where it draws. Left unstated, a backend that
    // wires the buffer and forgets the metrics gets zero-sized quads — invisible
    // text with a full count, which is the failure this whole seam is shaped to
    // refuse.
    BuildInputs unsized = in;
    unsized.glyphDoubleSlotWidthInPixels = 0.0f;
    std::vector<CellInstance> unsizedCells(4);
    std::vector<GlyphInstance> unsizedDoubles(4);
    InstanceBuffers unsizedBuffers { unsizedCells.data(), unsizedCells.size(), decorations.data(), decorations.size() };
    unsizedBuffers.doubleGlyphs = unsizedDoubles.data();
    unsizedBuffers.doubleGlyphCapacity = unsizedDoubles.size();
    FakeAtlas unsizedAtlas;
    const InstanceCounts unsizedCounts = buildCellInstances(unsized, UbiquittyTest::glyphBackendFor(unsizedAtlas), unsizedBuffers);
    CHECK(unsizedCounts.doubleGlyphs == 0);
    CHECK(unsizedCells[0].atlasBottomRight.y > unsizedCells[0].atlasTopLeft.y);
}

UBIQUITTY_TEST_MAIN()

TEST(aSecondFrameOverOneAtlasReusesItsGlyphsAndDrawsTheSame)
{
    // Every render oracle in this tree renders one frame per renderer, so it
    // only ever tests a cold start. The defect that shape hides is a glyph
    // first seen on frame 2 or later: an atlas that writes its host mirror
    // without marking the upload dirty draws that glyph blank, because only
    // frame 1 uploads unconditionally — and blank is what a space looks like,
    // so no count disagrees.
    //
    // Two assertions, and the second is what makes the first mean anything. A
    // cache that stopped being consulted would still produce identical output by
    // rasterising everything again; only the rasterisation count says which path
    // ran. Without it this degrades into a copy test that cannot fail once
    // the reuse verdict rots.
    constexpr int columns = 8;
    std::vector<Ubiquitty::Cell> grid(columns);
    TermPalette pal;
    pal.seedDefaults();
    for (int column = 0; column < columns; ++column)
        setCell(grid, columns, 0, column, 0x41u + static_cast<std::uint32_t>(column), {}, colorDefault, colorDefault);

    FakeAtlas atlas;
    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;

    std::vector<CellInstance> firstCells(columns);
    std::vector<DecorationInstance> firstDecorations(static_cast<std::size_t>(columns) * 8);
    InstanceBuffers firstBuffers;
    firstBuffers.cells = firstCells.data();
    firstBuffers.cellCapacity = firstCells.size();
    firstBuffers.decorations = firstDecorations.data();
    firstBuffers.decorationCapacity = firstDecorations.size();
    const InstanceCounts first = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), firstBuffers);
    const int rasterisedAfterFirstFrame = atlas.monoRasterised;
    CHECK_MSG(rasterisedAfterFirstFrame > 0, "the cold frame rasterised something to cache");

    // The same content over the same atlas. Nothing about the glyphs changed,
    // so nothing should need rasterising again.
    std::vector<CellInstance> secondCells(columns);
    std::vector<DecorationInstance> secondDecorations(static_cast<std::size_t>(columns) * 8);
    InstanceBuffers secondBuffers;
    secondBuffers.cells = secondCells.data();
    secondBuffers.cellCapacity = secondCells.size();
    secondBuffers.decorations = secondDecorations.data();
    secondBuffers.decorationCapacity = secondDecorations.size();
    const InstanceCounts second = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), secondBuffers);

    CHECK_MSG(second.cells == first.cells, "the second frame emits the same number of cells");
    CHECK_MSG(atlas.monoRasterised == rasterisedAfterFirstFrame,
        "a glyph already in the atlas is not rasterised again on a later frame");

    // Byte-identical, cell by cell: a reuse path that produced a different
    // quad would be a defect the count above cannot see.
    for (std::size_t i = 0; i < first.cells; ++i) {
        CHECK_MSG(std::memcmp(&firstCells[i], &secondCells[i], sizeof(CellInstance)) == 0,
            "the second frame draws each cell exactly as the first did");
    }
}

// A heavy single underline hangs from the rule line; it does not climb into the
// type. Above the rule line is the descender band, where skip-ink erases
// whatever crosses a stem, so any geometry that carries weight upward — a
// centred stroke, or a floor derived from the thickness — spends that weight
// where it will be eaten. The rule then thins as it is asked to thicken: at the
// maximum setting the upper half goes across the whole run and what survives
// reads as a hairline sitting lower than the double's pair.
//
// Asserted on the emitted rect rather than on a rendered image, because the
// symptom is only visible once skip-ink has run and the geometry is what
// decides it. The double pair hangs this way for the same reason; this pins the
// single to the same rule.
TEST(aThickSingleUnderlineHangsBelowTheRuleLineRatherThanClimbingIntoTheType)
{
    CellFixture fx;
    Ubiquitty::Cell cell = fx.blank;
    cell.codePoint = 0x67u; // 'g', a descender: the ink a climbing rule collides with
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.pen.attributes = Ubiquitty::VT::Attribute::Underline;

    BuildInputs in;
    in.cells = &cell;
    in.rows = 1;
    in.columns = 1;
    in.palette = &fx.pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineDrop = 1.0f; // the rule line: 13 + 1 = 14
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;

    // Thin first, as the control: the floor must not move a hairline, or every
    // rule in the tree shifts and this case proves only that something changed.
    in.metrics.underlineThickness = 1.0f;
    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decos(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    FakeAtlas atlas;
    InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.decorations == 1);
    const float thinTop = decos[0].topLeftInPixels.y;
    CHECK_MSG(approx(thinTop, 14.0f), "a hairline still sits on the rule line");
    CHECK_MSG(approx(decos[0].bottomRightInPixels.y - thinTop, 1.0f), "and carries its one pixel");

    // Now the maximum the setting reaches. The stroke grows DOWNWARD from the
    // rule line: same top, four times the weight.
    in.metrics.underlineThickness = 4.0f;
    counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.decorations == 1);
    const float thickTop = decos[0].topLeftInPixels.y;
    const float thickBottom = decos[0].bottomRightInPixels.y;
    CHECK_MSG(thickTop >= 14.0f, "a thick rule never starts above the rule line");
    CHECK_MSG(approx(thickTop, thinTop), "thickening a rule does not move where it begins");
    CHECK_MSG(approx(thickBottom - thickTop, 4.0f),
        "the emitted rule carries the full requested weight");
    // The weight is what the setting asks for, so it must be MONOTONE in it: a
    // rule that stops getting heavier is the failure a fixed-position assert
    // alone would still pass.
    CHECK_MSG(thickBottom - thickTop > 1.0f, "a thicker setting draws a heavier rule");
}

// The atlas slot a colour glyph is cut to is the square it draws into.
//
// Equal, no sampler is observable and every backend agrees whatever it filters
// with. Unequal, the glyph is resampled on the way to the screen, and the
// backends do not filter alike — macOS samples the atlas linear where the web
// and the CPU compositor take the nearest texel — so one emoji is smoothed and
// another has whole rows dropped. No image oracle can see that: each backend is
// compared against its own golden, so both stay correct forever.
//
// Asserted on the rule both the slot and the destination are cut from, which is
// what makes the equality structural rather than two expressions that happen to
// agree today.
TEST(theColourGlyphSquareIsNeverLargerThanTheBoxItDrawsInto)
{
    using Ubiquitty::Rendering::colourGlyphSquareInPixels;

    // Real metrics, 14pt at 2x, plus the Unifont fallback. Which term binds is
    // a property of the face: the first three are proportioned so that two
    // cells is the smaller, and the fallback sits exactly on the boundary,
    // where both terms give 16. Covering the boundary is what stops the
    // expression being simplified to the span term alone.
    const struct {
        float cellWidth, cellHeight, square;
    } faces[] = {
        { 14.4f, 36.0f, 28.0f }, // JetBrains Mono, two cells of 14.4 floored
        { 14.8f, 34.0f, 29.0f }, // SF Mono
        { 14.4f, 32.0f, 28.0f }, // Menlo
        { 8.0f, 16.0f, 16.0f }, // the Unifont fallback, where 2 * width == height
    };
    for (const auto& face : faces) {
        const float square = colourGlyphSquareInPixels(face.cellWidth, face.cellHeight);
        CHECK(approx(square, face.square));
        CHECK_MSG(square <= face.cellHeight, "the square never overflows the cell box vertically");
        CHECK_MSG(square <= 2.0f * face.cellWidth, "the square never overflows the two-cell span horizontally");
        CHECK_MSG(approx(square, std::floor(square)), "a slot is a whole number of texels, so the square is whole pixels");
    }

    // The height binds only where a cell is at least half as wide as it is tall,
    // and there the square stops at the cell rather than running past it.
    CHECK(approx(colourGlyphSquareInPixels(20.0f, 32.0f), 32.0f));
    CHECK(approx(colourGlyphSquareInPixels(16.0f, 32.0f), 32.0f));

    // A degenerate cell yields no square rather than a negative one, so an
    // atlas sized from this cannot ask for a slot it can never fill.
    CHECK(approx(colourGlyphSquareInPixels(0.0f, 32.0f), 0.0f));
    CHECK(approx(colourGlyphSquareInPixels(14.0f, -1.0f), 0.0f));
}

// ---- guards, ceilings and the rarely-taken decoration arms ------------
// The builder's refusals and clamps: a backend missing a required hook, a
// caller passing nothing, spans naming rows that do not exist, buffers too
// small for the scene, and the SGR attributes no other case here carries.

// A backend rejected by isDrawable writes nothing, and is never asked to
// begin a frame it cannot supply.
TEST(testANonDrawableBackendWritesNothing)
{
    CellFixture fx;
    FakeAtlas at;
    Ubiquitty::Rendering::GlyphBackend backend = UbiquittyTest::glyphBackendFor(at);
    backend.monoGlyph = nullptr; // a required entry, absent
    std::vector<CellInstance> cells(8);
    std::vector<DecorationInstance> decorations(64);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(fx.in, backend, buffers);
    CHECK(counts.cells == 0);
    CHECK(counts.decorations == 0);
    CHECK(at.frames == 0); // beginFrame never fired
}

// A caller passing no grid, no palette, or a degenerate geometry gets an
// empty count — after beginFrame, which the backend is entitled to.
TEST(testEmptyInputsProduceNoInstances)
{
    CellFixture fx;
    const BuildInputs base = fx.in;
    struct Case {
        const char* what;
        BuildInputs (*mutate)(BuildInputs);
    };
    const Case cases[] = {
        { "no cells", [](BuildInputs in) { in.cells = nullptr; return in; } },
        { "no palette", [](BuildInputs in) { in.palette = nullptr; return in; } },
        { "no rows", [](BuildInputs in) { in.rows = 0; return in; } },
        { "no columns", [](BuildInputs in) { in.columns = 0; return in; } },
        { "negative rows", [](BuildInputs in) { in.rows = -1; return in; } },
    };
    for (const Case& c : cases) {
        FakeAtlas at;
        std::vector<CellInstance> cells(8);
        std::vector<DecorationInstance> decorations(64);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        const InstanceCounts counts = buildCellInstances(c.mutate(base), UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK_MSG(counts.cells == 0, c.what);
        CHECK(at.frames == 1);
    }
}

// A span naming a row off the grid, or ending before it starts, is dropped
// rather than indexing the per-row lookup it would build.
TEST(testOutOfRangeSpansAreDropped)
{
    CellFixture fx;
    const SelectionRowSpan spans[] = {
        { /*visibleRow=*/-1, 0, 3 },
        { /*visibleRow=*/fx.rows, 0, 3 },
        { /*visibleRow=*/0, 3, 1 }, // column1 < column0
    };

    for (int which = 0; which < 2; ++which) {
        BuildInputs in = fx.in;
        if (which == 0) {
            in.selectionSpans = spans;
            in.selectionSpanCount = 3;
            in.selectionForeground = { 1.0f, 1.0f, 1.0f, 1.0f };
            in.selectionForegroundOverride = true;
        } else {
            in.searchMatchSpans = spans;
            in.searchMatchSpanCount = 3;
        }
        FakeAtlas at;
        std::vector<CellInstance> cells(16);
        std::vector<DecorationInstance> decorations(128);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
        // The scene still builds; no span took effect.
        CHECK(counts.cells > 0);
    }
}

// A buffer too small stops the writes but not the counting: the count is what
// the caller resizes from, so a scene that overflowed must report its true
// size rather than the size that fitted.
TEST(testCapacityCeilingsKeepCounting)
{
    CellFixture fx;
    BuildInputs in = fx.in;
    // Every cell decorated, so the decoration stream overflows first.
    std::vector<Ubiquitty::Cell> grid(fx.grid);
    for (auto& cell : grid)
        cell.pen.attributes = Ubiquitty::VT::Attribute::Underline;
    in.cells = grid.data();

    FakeAtlas at;
    std::vector<CellInstance> cells(2); // fewer than the 8 cells
    std::vector<DecorationInstance> decorations(1); // fewer than the 8 rules
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    CHECK(counts.cells > cells.size());
    CHECK(counts.decorations > decorations.size());

    // Null streams count the same as full ones.
    InstanceBuffers none {};
    FakeAtlas nullAtlas;
    const InstanceCounts nullCounts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(nullAtlas), none);
    CHECK(nullCounts.cells == counts.cells);
    CHECK(nullCounts.decorations == counts.decorations);
}

// A run of blank cells sharing an indexed pen draws one background per cell
// through the run filler, and takes its alpha from the caller's per-index
// table rather than the default-foreground one.
TEST(testBlankRunTakesItsAlphaFromThePaletteTable)
{
    TermPalette pal;
    pal.seedDefaults();
    const int columns = 12;
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = 3; // an indexed pen, so the table applies
    blank.pen.backgroundIndex = 4;
    blank.pen.underlineIndex = colorDefault;
    blank.codePoint = ' ';
    std::vector<Ubiquitty::Cell> row(columns, blank);

    std::uint8_t alphas[256];
    for (int i = 0; i < 256; ++i)
        alphas[i] = 255;
    alphas[3] = 51; // 0.2

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.paletteForegroundAlpha = alphas;
    in.cursor.row = -1; // the cursor's row opts out of run filling
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    FakeAtlas at;
    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(columns * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    CHECK(counts.cells == static_cast<std::size_t>(columns));
    CHECK(approx(cells[0].foreground.w, 51.0f / 255.0f));
    CHECK(approx(cells[columns - 1].foreground.w, 51.0f / 255.0f));

    // The same run against a stream too small: the count is the run's, the
    // writes stop at the ceiling.
    std::vector<CellInstance> tiny(3);
    InstanceBuffers small { tiny.data(), tiny.size(), decorations.data(), decorations.size() };
    FakeAtlas smallAtlas;
    const InstanceCounts smallCounts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(smallAtlas), small);
    CHECK(smallCounts.cells == static_cast<std::size_t>(columns));
}

// Overline, framed and encircled: each emits its own rule, and the box draws
// as four rules rather than an ellipse.
TEST(testOverlineFramedAndEncircledRules)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;

    struct Case {
        const char* what;
        Ubiquitty::VT::Attribute attribute;
        std::size_t rules;
    };
    const Case cases[] = {
        { "overline", Ubiquitty::VT::Attribute::Overline, 1 },
        { "framed", Ubiquitty::VT::Attribute::Framed, 4 },
        { "encircled", Ubiquitty::VT::Attribute::Encircled, 4 },
    };
    for (const Case& c : cases) {
        std::vector<Ubiquitty::Cell> row(1, blank);
        row[0].codePoint = 0x41u;
        row[0].pen.attributes = c.attribute;

        BuildInputs in;
        in.cells = row.data();
        in.rows = 1;
        in.columns = 1;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.metrics.strikeThickness = 1.0f;

        FakeAtlas at;
        std::vector<CellInstance> cells(1);
        std::vector<DecorationInstance> decorations(16);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK_MSG(counts.decorations == c.rules, c.what);
        // Each rule has a real extent: a zero-height band draws nothing.
        for (std::size_t i = 0; i < counts.decorations; ++i) {
            CHECK(decorations[i].bottomRightInPixels.y > decorations[i].topLeftInPixels.y);
            CHECK(decorations[i].bottomRightInPixels.x > decorations[i].topLeftInPixels.x);
        }
    }

    // Framed and encircled together are one box, not two.
    std::vector<Ubiquitty::Cell> row(1, blank);
    row[0].codePoint = 0x41u;
    row[0].pen.attributes = { Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled };
    BuildInputs both;
    both.cells = row.data();
    both.rows = 1;
    both.columns = 1;
    both.palette = &pal;
    both.metrics.cellWidth = 8.0f;
    both.metrics.cellHeight = 16.0f;
    both.metrics.baselineFromTop = 13.0f;
    FakeAtlas at;
    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    CHECK(buildCellInstances(both, UbiquittyTest::glyphBackendFor(at), buffers).decorations == 4);
}

// A hovered link takes the accent colour and a single rule whatever style the
// frontend configured for a resting one, so the run under the pointer reads
// as one affordance.
TEST(testAHoveredLinkOverridesTheRestingStyle)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    std::vector<Ubiquitty::Cell> row(2, blank);
    row[0].codePoint = 0x41u;
    row[1].codePoint = 0x42u;
    const std::uint8_t linkStates[2] = {
        static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::Hovered),
        static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::Resting),
    };

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 2;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.linkCells = linkStates;
    in.linkUnderlineStyle = Ubiquitty::VT::underlineDotted;
    in.linkColor = { 0.2f, 0.4f, 1.0f, 1.0f };
    in.linkHoverColor = { 1.0f, 0.5f, 0.0f, 1.0f };

    FakeAtlas at;
    std::vector<CellInstance> cells(2);
    std::vector<DecorationInstance> decorations(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    CHECK(counts.decorations == 2);
    // The hovered cell: solid, in the hover colour. The resting one keeps the
    // configured dotted style and the resting colour — asserted as a pair, so
    // a builder ignoring the state cannot satisfy both.
    CHECK(decorations[0].kind == static_cast<float>(DecorationKind::Solid));
    CHECK(approx(decorations[0].color.x, 1.0f) && approx(decorations[0].color.y, 0.5f));
    CHECK(decorations[1].kind == static_cast<float>(DecorationKind::Dotted));
    CHECK(approx(decorations[1].color.x, 0.2f) && approx(decorations[1].color.z, 1.0f));
}

// An action cell — a Welcome button — is not drawn as a link: resting, it
// keeps its pen's colours and draws no underline, so a label set on a plate
// keeps its contrast; hovered, its plate lightens a step towards the text.
TEST(testAnActionCellKeepsItsColoursAndLightsItsPlateOnHover)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    std::vector<Ubiquitty::Cell> row(3, blank);
    for (int c = 0; c < 3; ++c) {
        row[static_cast<std::size_t>(c)].codePoint = 0x41u + static_cast<std::uint32_t>(c);
        // Ink (20,20,28) on a slate plate (55,55,65), as the Welcome screen sets.
        Ubiquitty::Cell& cell = row[static_cast<std::size_t>(c)];
        cell.pen.foregroundIndex = colorRGB;
        cell.pen.foregroundRed = 20;
        cell.pen.foregroundGreen = 20;
        cell.pen.foregroundBlue = 28;
        cell.pen.backgroundIndex = colorRGB;
        cell.pen.backgroundRed = 55;
        cell.pen.backgroundGreen = 55;
        cell.pen.backgroundBlue = 65;
    }
    const std::uint8_t linkStates[3] = {
        static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::RestingAction),
        static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::HoveredAction),
        static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::Resting),
    };

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 3;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.linkCells = linkStates;
    in.linkUnderlineStyle = Ubiquitty::VT::underlineSingle;
    in.linkColor = { 0.2f, 0.4f, 1.0f, 1.0f };
    in.linkHoverColor = { 1.0f, 0.5f, 0.0f, 1.0f };

    FakeAtlas at;
    std::vector<CellInstance> cells(3);
    std::vector<DecorationInstance> decorations(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    REQUIRE(counts.cells == 3);
    CHECK_MSG(counts.decorations == 1, "only the plain resting link underlines");
    const float ink = 20.0f / 255.0f;
    const float slate = 55.0f / 255.0f;
    CHECK_MSG(approx(cells[0].foreground.x, ink) && approx(cells[0].background.x, slate), "a resting action keeps its pen's ink and plate");
    CHECK_MSG(approx(cells[1].foreground.x, ink), "a hovered action keeps its ink");
    CHECK_MSG(approx(cells[1].background.x, slate + (ink - slate) * 0.18f), "and its plate moves a step towards the ink");
    CHECK_MSG(approx(cells[2].foreground.x, 0.2f) && approx(cells[2].foreground.z, 1.0f), "the link cell takes the link tint");
}

// Sub-pixel metrics floor rather than vanish: a rule thinner than a device
// pixel would round to nothing on the way to the shader.
TEST(testRuleThicknessFloors)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    std::vector<Ubiquitty::Cell> row(3, blank);
    for (int c = 0; c < 3; ++c)
        row[static_cast<std::size_t>(c)].codePoint = 0x41u + static_cast<std::uint32_t>(c);
    row[0].pen.attributes = Ubiquitty::VT::Attribute::Underline;
    row[1].pen.attributes = Ubiquitty::VT::Attribute::Strike;
    row[2].pen.attributes = Ubiquitty::VT::Attribute::Underline;
    row[2].pen.underlineStyle = Ubiquitty::VT::underlineCurly;

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 3;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    // Every thickness under its floor.
    in.metrics.underlineThickness = 0.1f;
    in.metrics.underlineCurlyThickness = 0.1f;
    in.metrics.strikeThickness = 0.1f;

    FakeAtlas at;
    std::vector<CellInstance> cells(3);
    std::vector<DecorationInstance> decorations(24);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    CHECK(counts.decorations == 3);
    for (std::size_t i = 0; i < counts.decorations; ++i)
        CHECK(decorations[i].thicknessInPixels >= 0.75f);
}

// A double-height row on a backend with no 2x master falls back to halving
// the 1x glyph: the top half for the top row, the bottom half for the bottom,
// and never the same range twice.
TEST(testDoubleHeightWithoutAMasterHalvesTheOneXGlyph)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;

    float topMin = 0.0f, topMax = 0.0f, bottomMin = 0.0f, bottomMax = 0.0f;
    const std::uint8_t halves[2] = { Ubiquitty::lineAttributeDoubleHeightTop, Ubiquitty::lineAttributeDoubleHeightBottom };
    for (int half = 0; half < 2; ++half) {
        std::vector<Ubiquitty::Cell> row(1, blank);
        row[0].codePoint = 0x41u;
        const std::uint8_t lineAttributes[1] = { halves[half] };
        BuildInputs in;
        in.cells = row.data();
        in.rows = 1;
        in.columns = 1;
        in.palette = &pal;
        in.lineAttributes = lineAttributes;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;

        FakeAtlas at;
        at.hasDoubleMasters = false; // no 2x master: the 1x fallback
        std::vector<CellInstance> cells(1);
        std::vector<DecorationInstance> decorations(8);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
        CHECK(counts.cells == 1);
        CHECK(at.doubleCalls > 0); // the master was asked for, and refused
        if (half == 0) {
            topMin = cells[0].atlasTopLeft.y;
            topMax = cells[0].atlasBottomRight.y;
        } else {
            bottomMin = cells[0].atlasTopLeft.y;
            bottomMax = cells[0].atlasBottomRight.y;
        }
    }
    // Two disjoint halves of one box, in order.
    CHECK(topMax > topMin && bottomMax > bottomMin);
    CHECK(approx(topMax, bottomMin));
    CHECK(approx(topMax - topMin, bottomMax - bottomMin));
}

// ---- refused rasters and the cluster routes --------------------------
// The cache-backed routes and what they do when the backend declines. A slot
// is reported fresh exactly once, so a caller that takes a slot and then
// fails to fill it must unclaim the key: left resident, every later frame is
// told the glyph is cached and hands back a slot nobody wrote — the cell
// draws the evicted glyph's pixels under its own colours, with every count in
// the frame correct.
namespace {

// A backend built entry by entry rather than through the fixture adapter: the
// cache-backed entries (bindColorAtlas, rasteriseColorGlyph,
// rasteriseClusterGlyph) have no probe there, and they are the subject here.
struct CacheBackend {
    // Two slots and one column: small enough that a row of distinct glyphs
    // saturates it inside one frame, and tiled exactly so an assigned slot
    // still yields a real rect.
    static constexpr int monoSlots = 256;
    static constexpr int monoColumns = 16;
    GlyphSlotCache monoCache { monoSlots, recommendedHashBuckets(monoSlots), monoColumns };
    GlyphSlotCache colorCache { 64, recommendedHashBuckets(64), 8 };
    bool monoRasterSucceeds = true;
    bool colorRasterSucceeds = true;
    bool clusterRasterSucceeds = true;
    int monoRasters = 0;
    int colorRasters = 0;
    int clusterRasters = 0;
    int frames = 0;

    static MonoAtlasBinding monoBinding(CacheBackend& self)
    {
        MonoAtlasBinding binding;
        binding.cache = &self.monoCache;
        binding.geometry = { /*columns=*/monoColumns, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = monoSlots;
        binding.weight = 0u;
        return binding;
    }

    GlyphBackend backend()
    {
        GlyphBackend b;
        b.context = this;
        b.beginFrame = [](void* context) {
            auto* self = static_cast<CacheBackend*>(context);
            ++self->frames;
            // The frame stamp is what makes a slot placed this pass exempt from
            // eviction; without it a saturated cache recycles instead of
            // reporting full.
            self->monoCache.beginFrame();
            self->colorCache.beginFrame();
        };
        b.bindMonoAtlas = [](void* context) { return monoBinding(*static_cast<CacheBackend*>(context)); };
        b.rasteriseMonoGlyph = [](void* context, std::uint32_t, FontFace, std::uint32_t, int) {
            auto* self = static_cast<CacheBackend*>(context);
            ++self->monoRasters;
            return self->monoRasterSucceeds;
        };
        b.monoGlyph = [](void*, std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t) {
            return codePoint == 0x20u ? AtlasGlyph {} : AtlasGlyph { { 0.1f, 0.1f }, { 0.2f, 0.2f }, true };
        };
        b.wideGlyph = [](void*, std::uint32_t, FontFace) { return AtlasGlyph { { 0.3f, 0.3f }, { 0.4f, 0.4f }, true }; };
        b.doubleGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, DoubleRegion) { return AtlasGlyph {}; };
        b.bindColorAtlas = [](void* context) {
            ColorAtlasBinding binding;
            binding.cache = &static_cast<CacheBackend*>(context)->colorCache;
            binding.columns = 8;
            binding.rows = 8;
            binding.slotInPixels = 32;
            binding.slotCapacity = 64;
            return binding;
        };
        b.rasteriseColorGlyph = [](void* context, ClusterSpan, int) {
            auto* self = static_cast<CacheBackend*>(context);
            ++self->colorRasters;
            return self->colorRasterSucceeds;
        };
        b.rasteriseClusterGlyph = [](void* context, ClusterSpan, FontFace, std::uint32_t, int) {
            auto* self = static_cast<CacheBackend*>(context);
            ++self->clusterRasters;
            return self->clusterRasterSucceeds;
        };
        return b;
    }
};

// A cell carrying its marks in an overflow cluster rather than inline, with a
// resolver that hands them back — the shape a grid holding more marks than
// `combining` has room for produces.
struct ClusterScene {
    static constexpr std::uint32_t base = 0x0065u; // 'e'
    static constexpr std::uint32_t marks[3] = { 0x0301u, 0x0308u, 0x0327u };

    static int resolve(const Ubiquitty::Cell*, std::span<std::uint32_t> out, void*)
    {
        out[0] = base;
        for (std::size_t i = 0; i < 3; ++i)
            out[i + 1] = marks[i];
        return 4;
    }
};

} // namespace

// A cell whose marks live in the cluster pool reaches the resolver, and the
// resolved run is what the cluster raster is asked for. A refused raster
// unclaims its key, so the next frame asks again instead of blitting a slot
// nobody wrote.
TEST(testClusterCellsRouteThroughTheResolverAndUnclaimARefusedRaster)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = ClusterScene::base;
    cell.setClusterID(7); // marks live in the pool, not inline
    std::vector<Ubiquitty::Cell> row(1, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.clusterResolver = &ClusterScene::resolve;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };

    // (a) The raster succeeds: one call, and a second frame is served from the
    // cache without asking again.
    {
        CacheBackend backend;
        const GlyphBackend table = backend.backend();
        CHECK(buildCellInstances(in, table, buffers).cells == 1);
        CHECK(backend.clusterRasters == 1);
        CHECK(buildCellInstances(in, table, buffers).cells == 1);
        CHECK(backend.clusterRasters == 1); // resident: not re-rasterised
    }

    // (b) The raster refuses: the key is unclaimed, so every frame asks again
    // rather than being told the glyph is resident.
    {
        CacheBackend backend;
        backend.clusterRasterSucceeds = false;
        const GlyphBackend table = backend.backend();
        buildCellInstances(in, table, buffers);
        buildCellInstances(in, table, buffers);
        CHECK(backend.clusterRasters == 2);
    }
}

// The same contract on the mono route: a refused glyph raster is retried on
// the next frame rather than served from the slot it never wrote.
TEST(testARefusedMonoRasterIsAskedForAgain)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = 0x41u;
    std::vector<Ubiquitty::Cell> row(1, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.skipInk = true; // the coverage route probes the mono cache

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };

    CacheBackend backend;
    backend.monoRasterSucceeds = false;
    const GlyphBackend table = backend.backend();
    buildCellInstances(in, table, buffers);
    const int afterFirst = backend.monoRasters;
    buildCellInstances(in, table, buffers);
    CHECK(afterFirst > 0);
    CHECK(backend.monoRasters > afterFirst); // asked again, not served blank
}

// A colour cell on its own sheet takes the cache route; a refused colour
// raster unclaims its key the same way.
TEST(testARefusedColourRasterIsAskedForAgain)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = 0x1F600u;
    Pen pen = Ubiquitty::cellPen(cell);
    pen.attributes.add(Ubiquitty::VT::Attribute::Emoji);
    Ubiquitty::cellSetPen(cell, pen);
    std::vector<Ubiquitty::Cell> row(1, cell);

    std::vector<GlyphInstance> colorOut(4);
    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.colorGlyphs = colorOut.data();
    buffers.colorGlyphCapacity = colorOut.size();

    {
        CacheBackend backend;
        const GlyphBackend table = backend.backend();
        buildCellInstances(in, table, buffers);
        buildCellInstances(in, table, buffers);
        CHECK(backend.colorRasters == 1); // resident after the first
    }
    {
        CacheBackend backend;
        backend.colorRasterSucceeds = false;
        const GlyphBackend table = backend.backend();
        buildCellInstances(in, table, buffers);
        buildCellInstances(in, table, buffers);
        CHECK(backend.colorRasters == 2); // unclaimed, so asked again
    }
}

// A backend with neither a cluster entry nor a cluster raster draws the base
// code point alone — an `e` where the text said `é`. The substitution is
// Core's documented one, and this is the case that pins it: a fixture
// deriving from StrictAtlas cannot reach it, because refusing to answer is
// exactly what that base is for.
TEST(testABackendWithoutClusterEntriesDrawsTheBaseCodePoint)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = ClusterScene::base;
    cell.setClusterID(7);
    std::vector<Ubiquitty::Cell> row(1, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.clusterResolver = &ClusterScene::resolve;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    CacheBackend backend;
    GlyphBackend table = backend.backend();
    table.rasteriseClusterGlyph = nullptr; // no cluster route at all
    table.clusterGlyph = nullptr;
    table.wideClusterGlyph = nullptr;

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, table, buffers);
    CHECK(counts.cells == 1);
    // The cell is inked — with the base glyph's UV, which is what the
    // substitution costs and why it is documented rather than silent.
    CHECK(cells[0].atlasBottomRight.x > cells[0].atlasTopLeft.x);
}

namespace {
// A cluster led by a Prepend: MALAYALAM LETTER DOT REPH before the consonant it
// joins, MALAYALAM LETTER KA.
struct PrependClusterScene {
    static constexpr std::uint32_t prepend = 0x0D4Eu;
    static constexpr std::uint32_t base = 0x0D15u;
    static inline std::uint32_t drawnCodePoint = 0;

    static int resolve(const Ubiquitty::Cell*, std::span<std::uint32_t> out, void*)
    {
        out[0] = prepend;
        out[1] = base;
        return 2;
    }
};
} // namespace

// The base a backend without cluster entries draws alone is the cluster's
// base, not the Prepend standing before it.
TEST(testABackendWithoutClusterEntriesDrawsTheBaseAfterAPrepend)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = PrependClusterScene::prepend;
    cell.setClusterID(7);
    std::vector<Ubiquitty::Cell> row(1, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.clusterResolver = &PrependClusterScene::resolve;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    CacheBackend backend;
    GlyphBackend table = backend.backend();
    table.rasteriseClusterGlyph = nullptr;
    table.clusterGlyph = nullptr;
    table.wideClusterGlyph = nullptr;
    table.monoGlyph = [](void*, std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t) {
        PrependClusterScene::drawnCodePoint = codePoint;
        return AtlasGlyph { { 0.1f, 0.1f }, { 0.2f, 0.2f }, true };
    };

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    PrependClusterScene::drawnCodePoint = 0;
    const InstanceCounts counts = buildCellInstances(in, table, buffers);
    CHECK(counts.cells == 1);
    CHECK(PrependClusterScene::drawnCodePoint == PrependClusterScene::base);
}

// A dashed box-drawing character the box rasterizer cannot supply falls back
// to the text lane rather than drawing nothing: a dash the font carries is
// better than a gap in a table border.
TEST(testADashTheBoxLaneRefusesFallsBackToText)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = 0x2504u; // BOX DRAWINGS LIGHT TRIPLE DASH HORIZONTAL
    std::vector<Ubiquitty::Cell> row(1, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = 1;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;

    std::vector<CellInstance> cells(1);
    std::vector<DecorationInstance> decorations(8);
    std::vector<OverlayInstance> boxFills(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    // The box lane exists only when the caller offers its stream, so without
    // this the dash never reaches the box branch at all.
    buffers.boxFills = boxFills.data();
    buffers.boxFillCapacity = boxFills.size();

    // (a) No box entry at all: the dash takes the text lane.
    {
        CacheBackend backend;
        const GlyphBackend table = backend.backend();
        const InstanceCounts counts = buildCellInstances(in, table, buffers);
        CHECK(counts.cells == 1);
        CHECK(cells[0].atlasBottomRight.x > cells[0].atlasTopLeft.x); // inked
    }

    // (b) A box entry that declines this dash: the same fallback, through the
    // branch that asked.
    {
        CacheBackend backend;
        GlyphBackend table = backend.backend();
        table.boxGlyph = [](void*, std::uint32_t, FontFace) { return AtlasGlyph {}; };
        const InstanceCounts counts = buildCellInstances(in, table, buffers);
        CHECK(counts.cells == 1);
        CHECK(cells[0].atlasBottomRight.x > cells[0].atlasTopLeft.x);
    }
}

// Braille takes the dot lattice, and the setting takes it back to the font.
//
// The advance here is FRACTIONAL. The builder holds a snapped cell rect for
// the box family, and a braille dot placed inside that rect passes every
// per-cell assertion while putting every second boundary on a half pixel; at a
// whole-pixel advance the two derivations agree and this case would say
// nothing. The claim is about the row rather than about any one cell.
TEST(testBrailleTakesTheLatticeAndTheSettingTakesItBackToTheFont)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = 0x28FFu; // BRAILLE PATTERN DOTS-12345678: every dot raised
    constexpr int columns = 6;
    std::vector<Ubiquitty::Cell> row(columns, cell);

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    // 14.4 x 30.6: fractional on both axes, as a 13pt face at 2x measures.
    in.metrics.cellWidth = 14.4f;
    in.metrics.cellHeight = 30.6f;
    in.metrics.baselineFromTop = 24.0f;
    in.metrics.xHeight = 14.0f;

    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(8);
    std::vector<OverlayInstance> boxFills(columns * brailleDotsPerCell);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    // The lattice rides the box-fill stream, so a caller offering none keeps
    // braille on the font — the same rule the box family follows.
    buffers.boxFills = boxFills.data();
    buffers.boxFillCapacity = boxFills.size();

    {
        CacheBackend backend;
        const GlyphBackend table = backend.backend();
        in.drawsBrailleGrid = true;
        const InstanceCounts counts = buildCellInstances(in, table, buffers);
        CHECK(counts.cells == static_cast<std::size_t>(columns));
        CHECK_MSG(counts.boxFills == static_cast<std::size_t>(columns) * brailleDotsPerCell,
            "every raised dot of every cell is a quad");
        CHECK(std::fabs(boxFills[0].plA.y - 2.825f) < 0.001f);
        // Background only: the dots are the cell's whole ink, so a glyph quad
        // over them would draw the face's braille on top of the lattice.
        for (int column = 0; column < columns; ++column)
            CHECK(cells[column].atlasBottomRight.x == cells[column].atlasTopLeft.x);

        // Ink bounds retain the text phase; the quad adds coverage headroom.
        const float side = brailleDotSide(BrailleLattice { 0.0f, 0.0f,
            in.metrics.cellWidth, in.metrics.cellHeight });
        for (std::size_t i = 0; i < counts.boxFills; ++i) {
            const OverlayInstance& dot = boxFills[i];
            CHECK(std::fabs(dot.plA.z - dot.plA.x - side) < 0.001f);
            CHECK(std::fabs(dot.plA.w - dot.plA.y - side) < 0.001f);
            CHECK_EQ(dot.shapeKind, -2.0f);
            const int column = static_cast<int>(i) / brailleDotsPerCell;
            const int dotColumn = (static_cast<int>(i) % brailleDotsPerCell) / brailleDotRows;
            const float ideal = in.originInPixels.x + (column * 2 + dotColumn + 0.5f) * in.metrics.cellWidth * 0.5f;
            const auto phase = phaseForPen(ideal, in.subpixelPhaseCount, in.subpixelPhaseRounding);
            CHECK(std::fabs((dot.plA.x + dot.plA.z) * 0.5f - phase.snappedPenX - static_cast<float>(phase.phase) / in.subpixelPhaseCount) < 0.001f);
        }

        // The dot columns across the row, deduplicated. Two dot columns per
        // cell, evenly spaced through every cell boundary — which is the claim
        // the row is here to make, and the one a face cannot hold.
        std::vector<float> lefts;
        for (std::size_t i = 0; i < counts.boxFills; ++i)
            lefts.push_back(boxFills[i].topLeftInPixels.x);
        std::sort(lefts.begin(), lefts.end());
        lefts.erase(std::unique(lefts.begin(), lefts.end()), lefts.end());
        CHECK_MSG(lefts.size() == static_cast<std::size_t>(columns) * brailleDotColumns,
            "the row draws two dot columns per cell and no more");
        float smallest = 1e9f;
        float largest = -1e9f;
        for (std::size_t i = 1; i < lefts.size(); ++i) {
            const float step = lefts[i] - lefts[i - 1];
            smallest = step < smallest ? step : smallest;
            largest = step > largest ? step : largest;
        }
        CHECK_MSG(largest - smallest <= 1.0f / in.subpixelPhaseCount + 0.001f,
            "the pitch varies by at most one text phase across the run");
    }

    {
        // The setting off: no lattice, and the cell fetches its glyph like any
        // other character. This is the arm that says the flag is read rather
        // than the range being drawn unconditionally.
        CacheBackend backend;
        const GlyphBackend table = backend.backend();
        in.drawsBrailleGrid = false;
        const InstanceCounts counts = buildCellInstances(in, table, buffers);
        CHECK(counts.boxFills == 0);
        for (int column = 0; column < columns; ++column)
            CHECK(cells[column].atlasBottomRight.x > cells[column].atlasTopLeft.x);
    }
}

// A doubled cell whose second half runs past the last column loses that half
// of its quad: the visible part keeps the texel-to-pixel mapping the crop
// gave it, so the quad has to lose the same fraction or the remaining half
// stretches to fill a width the grid no longer has.
TEST(testADoubleGlyphQuadIsClippedByTheGridEdge)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;

    // Three columns under DOUBLE_WIDTH: columns 0 and 1 land at x0 and x2;
    // column 1's full width runs to x4, past the 3-column grid, so it is the
    // straddling cell.
    const int columns = 3;
    std::vector<Ubiquitty::Cell> row(columns, blank);
    for (int c = 0; c < columns; ++c)
        row[static_cast<std::size_t>(c)].codePoint = 0x41u + static_cast<std::uint32_t>(c);
    const std::uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.lineAttributes = lineAttributes;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphMarginInPixels.width = 1.0f;
    in.glyphMarginInPixels.height = 2.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphDoubleSlotWidthInPixels = 2.0f * (in.metrics.cellWidth + 2.0f * in.glyphMarginInPixels.width);
    in.glyphDoubleSlotHeightInPixels = 2.0f * (in.metrics.cellHeight + 2.0f * in.glyphMarginInPixels.height);

    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(columns * 8);
    std::vector<GlyphInstance> doubles(columns);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.doubleGlyphs = doubles.data();
    buffers.doubleGlyphCapacity = doubles.size();
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    CHECK(counts.doubleGlyphs == 2); // columns 0 and 1; column 2's lead is past the grid
    // The whole cell keeps the full quad; the straddling one keeps half of it.
    CHECK(doubles[1].sizeInPixels.x < doubles[0].sizeInPixels.x);
    CHECK(approx(doubles[1].sizeInPixels.x, doubles[0].sizeInPixels.x * 0.5f));
    // Height is untouched: only the horizontal span left the grid.
    CHECK(approx(doubles[1].sizeInPixels.y, doubles[0].sizeInPixels.y));
}

// On the phased path a glyph is blitted texel for pixel from its snapped pen,
// so the cell's own left edge lands a phase's fraction into the slot and the
// cell's pitch spans the slot at that pitch. The skip-ink tap reads the slot
// where the glyph is drawn, not the glyph box stretched over the cell: a tap
// at the box reads the ink a fraction to one side of where it is on screen
// and cuts the rule beside the descender rather than around it.
TEST(testASkipInkTapOnThePhasedPathFollowsTheSnappedPen)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    const int columns = 3;
    std::vector<Ubiquitty::Cell> row(columns, blank);
    row[1].codePoint = 0x67u; // 'g'
    row[1].pen.attributes = Ubiquitty::VT::Attribute::Underline;

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    // Cell 1's left edge at 10.5: four phases under floor rounding snap the
    // pen to 10 at phase 2, so the edge is half a texel into the slot's cell.
    in.originInPixels = { 2.5f, 0.0f };
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 4;
    in.subpixelPhaseRounding = PhaseRounding::Floor;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.skipInk = true;
    in.metrics.skipInkPadding = 1.0f;
    in.metrics.atlasToDevicePixels = 1.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 1.0f };

    CacheBackend backend;
    GlyphBackend table = backend.backend();
    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(columns * 8);
    std::vector<GlyphInstance> glyphs(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    const InstanceCounts counts = buildCellInstances(in, table, buffers);

    REQUIRE_MSG(counts.glyphs == 1, "the one glyph draws");
    REQUIRE_MSG(counts.decorations == 1, "under one rule");
    const GlyphInstance& glyph = glyphs[0];
    const DecorationInstance& rule = decorations[0];
    // The quad: the snapped pen less the margin, one slot wide.
    CHECK_MSG(approx(glyph.originInPixels.x, 9.0f), ("the quad stands at the snapped pen less the margin: " + std::to_string(glyph.originInPixels.x)).c_str());
    CHECK_MSG(approx(glyph.sizeInPixels.x, 8.0f), "one slot wide");
    CHECK_MSG(rule.skipInk > 0.5f, "the rule is cut by ink");
    // The slot the quad samples, in atlas pixels; the rule's tap must be the
    // same slot at the same pitch, offset by where the cell's edge falls in
    // it: screen x 10.5 is texel 1.5 of the quad, and the cell's eight pixels
    // reach texel 9.5.
    const float slotLeft = glyph.atlasTopLeft.x;
    CHECK_MSG(approx(rule.inkTopLeft.x, slotLeft + 1.5f),
        ("the tap begins where the cell's left edge falls in the slot: " + std::to_string(rule.inkTopLeft.x - slotLeft)).c_str());
    CHECK_MSG(approx(rule.inkBottomRight.x, slotLeft + 9.5f),
        ("and spans the cell's pitch at the blit's own: " + std::to_string(rule.inkBottomRight.x - slotLeft)).c_str());
    CHECK_MSG(approx(rule.glyphSlotTopLeftUV.x, slotLeft) && approx(rule.glyphSlotBottomRightUV.x, slotLeft + 8.0f), "clamped to the slot");
}

// The skip band is the rule's own rect, mapped onto the slot row for row, so
// a pixel of a curl's crest samples the ink at the crest's height. A band
// squeezed to the stroke at the line's middle sampled every row of the curl
// at the middle: the crest stood against a letter it touched, and the trough
// was cut by ink well above it.
TEST(aPatternedRulesSkipBandIsItsOwnRect)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    const int columns = 3;
    const auto ruleFor = [&](std::uint8_t style) {
        std::vector<Ubiquitty::Cell> row(columns, blank);
        row[1].codePoint = 0x67u; // 'g'
        row[1].pen.attributes = Ubiquitty::VT::Attribute::Underline;
        row[1].pen.underlineStyle = style;
        BuildInputs in;
        in.cells = row.data();
        in.rows = 1;
        in.columns = columns;
        in.palette = &pal;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.metrics.skipInk = true;
        in.metrics.skipInkPadding = 1.0f;
        in.metrics.atlasToDevicePixels = 1.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphSlotWidthInPixels = 8.0f;
        in.glyphSlotHeightInPixels = 16.0f;
        CacheBackend backend;
        GlyphBackend table = backend.backend();
        std::vector<CellInstance> cells(columns);
        std::vector<DecorationInstance> decorations(columns * 8);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        const InstanceCounts counts = buildCellInstances(in, table, buffers);
        CHECK_MSG(counts.decorations == 1 && decorations[0].skipInk > 0.5f, "one rule, cut by ink");
        return decorations[0];
    };
    // The single rule's rect is its stroke, so its band is its rect whichever
    // way the band is taken: it fixes the map from the cell's rows to the
    // glyph box's in the slot.
    const DecorationInstance single = ruleFor(Ubiquitty::VT::underlineSingle);
    const float singleHeight = single.bottomRightInPixels.y - single.topLeftInPixels.y;
    REQUIRE_MSG(singleHeight >= 1.0f, "a whole-pixel stroke");
    const float scale = (single.inkBottomRight.y - single.inkTopLeft.y) / singleHeight;
    const float origin = single.inkTopLeft.y - scale * single.topLeftInPixels.y;
    for (const std::uint8_t style : { Ubiquitty::VT::underlineCurly, Ubiquitty::VT::underlineDotted }) {
        const DecorationInstance rule = ruleFor(style);
        const float top = rule.topLeftInPixels.y;
        const float bottom = rule.bottomRightInPixels.y;
        REQUIRE_MSG(bottom - top > singleHeight, "a patterned rule's rect is taller than its stroke");
        CHECK_MSG(approx(rule.inkTopLeft.y, origin + scale * top),
            ("style " + std::to_string(style) + ": the band starts at the rect's top: " + std::to_string(rule.inkTopLeft.y) + " for " + std::to_string(origin + scale * top)).c_str());
        CHECK_MSG(approx(rule.inkBottomRight.y, origin + scale * bottom),
            ("style " + std::to_string(style) + ": and ends at its bottom: " + std::to_string(rule.inkBottomRight.y) + " for " + std::to_string(origin + scale * bottom)).c_str());
    }
}

// The skip-ink pass asks about a cell's neighbours before the column walk
// reaches them, so it can fault a slot the walk will later be told is
// resident. On an atlas with no room left the tap gets no slot at all, and
// the pass must leave the neighbour's mask absent rather than sample one it
// never wrote.
TEST(testASkipInkTapOnASaturatedAtlasYieldsNoMask)
{
    TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;

    // Every cell distinct and underlined, so each one takes a slot of its own
    // and the decoration pass asks about both its neighbours.
    const int columns = 16;
    std::vector<Ubiquitty::Cell> row(columns, blank);
    for (int c = 0; c < columns; ++c) {
        row[static_cast<std::size_t>(c)].codePoint = 0x41u + static_cast<std::uint32_t>(c);
        row[static_cast<std::size_t>(c)].pen.attributes = Ubiquitty::VT::Attribute::Underline;
    }

    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.skipInk = true;
    in.metrics.skipInkPadding = 1.0f;
    in.metrics.atlasToDevicePixels = 1.0f;

    struct TinyCacheBackend : CacheBackend {
        // Four slots: fewer than the row's glyphs, so the pass runs out.
        GlyphSlotCache small { 4, recommendedHashBuckets(4), 1 };
    } backend;
    GlyphBackend table = backend.backend();
    table.bindMonoAtlas = [](void* context) {
        auto* self = static_cast<TinyCacheBackend*>(context);
        MonoAtlasBinding binding;
        binding.cache = &self->small;
        binding.geometry = { /*columns=*/1, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = 4;
        binding.weight = 0u;
        return binding;
    };
    table.beginFrame = [](void* context) { static_cast<TinyCacheBackend*>(context)->small.beginFrame(); };

    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(columns * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    const InstanceCounts counts = buildCellInstances(in, table, buffers);

    // Every cell still emits, and every underline still draws: running out of
    // atlas costs a mask, never a rule.
    CHECK(counts.cells == static_cast<std::size_t>(columns));
    CHECK(counts.decorations == static_cast<std::size_t>(columns));
    // At least one rule past the atlas ceiling carries no neighbour ink box —
    // an absent mask, not a stale one.
    bool sawAbsentNeighbourMask = false;
    for (std::size_t i = 0; i < counts.decorations; ++i)
        if (approx(decorations[i].rightBox.x, 0.0f) && approx(decorations[i].rightInk.x, 0.0f))
            sawAbsentNeighbourMask = true;
    CHECK(sawAbsentNeighbourMask);
}

// ---- residue ledger --------------------------------------------------
// What stays dark in CellInstanceBuilder.cpp after these cases, and why each
// cannot execute rather than merely does not:
//
// - The straddle clamp's negative arm (`cellVisibleWidth < 0`): a cell whose
//   lead is at or past the last column is dropped by the guard immediately
//   above, so `columns - gridX` is positive by the time the clamp reads it.
// - Four closing braces after a `break` inside the underline-style switch:
//   llvm-cov gives each an unreachable region of its own. The emitRule above
//   each one is covered, so the case body ran.
// OSC 66 sized blocks: a band takes its own 1/scale slice of the master's V
// range, and the block claims scale*width columns.
//
// The oracle is the partition rather than the arithmetic: the bands of one
// block must tile their glyph's V range exactly — contiguous, equal, and
// together covering what the same glyph spans unsized. A check that restated
// the crop formula back to itself would pass while sampling the wrong third.
TEST(testSizedBlockBandCropAndClaim)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    auto build = [&](const Ubiquitty::Cell* cells, int rows, const Ubiquitty::TextSizeTable* t,
                     std::vector<CellInstance>& out) {
        BuildInputs in;
        in.cells = cells;
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = t;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
        InstanceBuffers buffers { out.data(), out.size(), decos.data(), decos.size() };
        FakeAtlas at;
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    };

    // The unsized reference: the same code point with no size id, so its UV is
    // the whole glyph the bands below have to partition.
    float wholeMinV = 0.0f;
    float wholeMaxV = 0.0f;
    {
        std::vector<Ubiquitty::Cell> row(columns, blank);
        row[0].codePoint = 0x41u; // 'A'
        std::vector<CellInstance> cells(columns);
        const InstanceCounts counts = build(row.data(), 1, &table, cells);
        CHECK(counts.cells == static_cast<std::size_t>(columns));
        CHECK_MSG(approx(cells[0].widthCells, 1.0f), "an unsized cell claims one column whatever the table holds");
        wholeMinV = cells[0].atlasTopLeft.y;
        wholeMaxV = cells[0].atlasBottomRight.y;
        CHECK(wholeMaxV > wholeMinV);
    }

    // A scale-3 block: three rows, each carrying one band anchor stamped the way
    // writeSizedBlock stamps them — Wide on the anchor, WideContinuation on the
    // rest of the run, the size id and the band on every cell of it.
    {
        const int rows = 3;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
        for (int band = 0; band < rows; ++band) {
            Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
            anchor.codePoint = 0x41u;
            Ubiquitty::Pen stamped = anchor.pen;
            stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
            stamped.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(anchor, stamped);
            for (int c = 1; c < 3; ++c) {
                Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
                continuation.codePoint = 0x41u;
                Ubiquitty::Pen cp = stamped;
                cp.attributes.remove(Ubiquitty::VT::Attribute::Wide);
                cp.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
                Ubiquitty::cellSetPen(continuation, cp);
            }
        }
        std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows);
        const InstanceCounts counts = build(grid.data(), rows, &table, cells);

        // Three tiles per band — one per glyph-width group — plus the three
        // untouched cells of each row. The run's continuations are skipped by
        // their lead, as a wide run's are, and the anchor emits for all of them.
        const int tilesPerBand = 3;
        CHECK(counts.cells == static_cast<std::size_t>(rows) * (tilesPerBand + 3));

        const float slice = (wholeMaxV - wholeMinV) / 3.0f;
        for (int band = 0; band < rows; ++band) {
            const std::size_t base = static_cast<std::size_t>(band) * (tilesPerBand + 3);
            for (int tile = 0; tile < tilesPerBand; ++tile) {
                const CellInstance& inst = cells[base + static_cast<std::size_t>(tile)];
                CHECK_MSG(approx(inst.gridPosition.y, static_cast<float>(band)), "each band draws into its own row");
                // The claim is the GLYPH's own width, never the block's. A tile
                // claiming the whole run would name the wide sheet while holding
                // a mono rect, and the compositor would sample a slot nothing
                // wrote — the block would place and draw no ink.
                CHECK_MSG(approx(inst.widthCells, 1.0f), "each tile claims its glyph's own width");
                CHECK_MSG(approx(inst.gridPosition.x, static_cast<float>(tile)), "tiles sit side by side across the run");
                CHECK_MSG(approx(inst.atlasTopLeft.y, wholeMinV + slice * static_cast<float>(band)),
                    "band b starts at b/scale of the master's V range");
                CHECK_MSG(approx(inst.atlasBottomRight.y - inst.atlasTopLeft.y, slice),
                    "every band takes an equal slice");
            }
            // The horizontal partition closes across the row, the same property
            // the bands have vertically: contiguous, equal, covering the whole.
            CHECK(approx(cells[base].atlasBottomRight.x, cells[base + 1].atlasTopLeft.x));
            CHECK(approx(cells[base + 1].atlasBottomRight.x, cells[base + 2].atlasTopLeft.x));
        }
        // The vertical partition closes: no gap, no overlap, and the last band
        // ends where the unsized glyph ends.
        CHECK(approx(cells[0].atlasBottomRight.y, cells[6].atlasTopLeft.y));
        CHECK(approx(cells[6].atlasBottomRight.y, cells[12].atlasTopLeft.y));
        CHECK(approx(cells[12].atlasBottomRight.y, wholeMaxV));
    }

    // No table reachable: the size id resolves to nothing, so the anchor's Wide
    // attribute is all that is left to read and the cell must render as exactly
    // the wide character that attribute spells — same claim, same page, no band
    // crop. `claimedRunColumns` documents this as erasing rather than cropping a
    // damaged sized chain, and the render half has to agree with it.
    //
    // The control is a genuinely wide cell rather than the mono reference above:
    // a wide glyph is fetched from a different sheet, whose V range is its own
    // coordinate space, so comparing the degrade against the mono glyph would
    // compare two pages and fail for a reason that is not the crop.
    {
        enum class SizeID {
            Absent,
            Present
        };
        auto wideRow = [&](SizeID sizeID) {
            std::vector<Ubiquitty::Cell> row(columns, blank);
            row[0].codePoint = 0x41u;
            Ubiquitty::Pen stamped = row[0].pen;
            stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
            if (sizeID == SizeID::Present) {
                stamped.extendedAttributes.setTextSizeSpecID(scale3);
                Ubiquitty::penSetTextBand(&stamped, 1u);
            }
            Ubiquitty::cellSetPen(row[0], stamped);
            return row;
        };

        std::vector<Ubiquitty::Cell> sized = wideRow(SizeID::Present);
        std::vector<Ubiquitty::Cell> plain = wideRow(SizeID::Absent);
        std::vector<CellInstance> sizedCells(columns);
        std::vector<CellInstance> plainCells(columns);
        const InstanceCounts sizedCounts = build(sized.data(), 1, nullptr, sizedCells);
        const InstanceCounts plainCounts = build(plain.data(), 1, nullptr, plainCells);

        CHECK(sizedCounts.cells == plainCounts.cells);
        CHECK_MSG(approx(sizedCells[0].widthCells, 2.0f), "a null table falls back to the wide claim the attribute spells");
        CHECK_MSG(approx(sizedCells[0].widthCells, plainCells[0].widthCells)
                && approx(sizedCells[0].atlasTopLeft.y, plainCells[0].atlasTopLeft.y)
                && approx(sizedCells[0].atlasBottomRight.y, plainCells[0].atlasBottomRight.y),
            "an unresolvable size id renders as the plain wide cell, cropping nothing");
    }
}

// A sized block is one glyph drawn across its bands, so a block anchored on a
// DECDWL row is doubled in every band, not only the anchor's. The bands below
// sit on rows with no attribute of their own; drawn at those rows' width they
// were half as wide as the anchor's band, and a doubled T drew its stem under
// the left half of its crossbar.
TEST(testASizedBlockOnADoubledRowIsDoubledInEveryBand)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    const int columns = 12;
    const int rows = 3;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, fx.blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x54u; // 'T'
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }
    // A letter the row holds under the block's doubled half, which the walk
    // reaches rather than the blank-run fill.
    grid[static_cast<std::size_t>(columns) + 4].codePoint = 0x78u; // 'x'
    const std::uint8_t lineAttributes[rows] = { Ubiquitty::lineAttributeDoubleWidth, lineAttributeNormal, lineAttributeNormal };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(columns) * rows * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    std::array<float, rows> inkedWidth {};
    for (std::size_t i = 0; i < counts.cells; ++i) {
        const int row = static_cast<int>(cells[i].gridPosition.y);
        if (row >= 0 && row < rows && cells[i].atlasBottomRight.x > cells[i].atlasTopLeft.x)
            inkedWidth[static_cast<std::size_t>(row)] += cells[i].widthCells;
    }
    CHECK_MSG(inkedWidth[0] == 6.0f, "the anchor's band is the doubled claim: three columns at two cells each");
    CHECK_MSG(inkedWidth[1] == inkedWidth[0], "the band below is as wide as the anchor's");
    CHECK_MSG(inkedWidth[2] == inkedWidth[0], "and so is the last");
    // The bands below sit on ordinary rows, whose own cells in the doubled
    // half are under the block. Drawn, their backgrounds come after the
    // block's tiles and cover the glyph's ink wherever it is baked into the
    // cell quads: the stem of a doubled T, on the cell-baked path.
    int coveringCells = 0;
    int bandTileWidthsOverOneCell = 0;
    for (std::size_t i = 0; i < counts.cells; ++i) {
        const int row = static_cast<int>(cells[i].gridPosition.y);
        const bool isInked = cells[i].atlasBottomRight.x > cells[i].atlasTopLeft.x;
        if (row > 0 && !isInked && cells[i].gridPosition.x >= 3.0f && cells[i].gridPosition.x < 6.0f)
            ++coveringCells;
        if (row > 0 && isInked && cells[i].widthCells > 1.0f)
            ++bandTileWidthsOverOneCell;
    }
    int instancesAtTheLetter = 0;
    for (std::size_t i = 0; i < counts.cells; ++i) {
        if (static_cast<int>(cells[i].gridPosition.y) == 1 && cells[i].gridPosition.x == 4.0f)
            ++instancesAtTheLetter;
    }
    CHECK_MSG(coveringCells == 0, "no cell of an undoubled row draws under the doubled block");
    CHECK_MSG(instancesAtTheLetter == 1, "the letter under the block draws nothing; only the block's tile is there");
    CHECK_MSG(bandTileWidthsOverOneCell == 0,
        "a band on an ordinary row tiles a row cell at a time: a two-cell tile there names the wide sheet");
}

// A block anchored past the start of a doubled row stands at twice its column
// on the band rows below it, and the storage it reserves there lies partly to
// the left of where it stands. That storage holds the block's continuations,
// which draw nothing of their own, so unless the block paints it a band row
// shows a hole of whatever the surface was cleared to: the black box under
// "ab" beside a doubled T on the portable renderer.
TEST(testABandBelowADoubledAnchorPaintsTheStorageItReservesBesideIt)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned scale2 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/2, /*widthCells=*/1 });
    const int columns = 12;
    const int rows = 2;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, fx.blank);
    grid[0].codePoint = 0x61u; // 'a'
    grid[1].codePoint = 0x62u; // 'b'
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns + 2];
        anchor.codePoint = 0x54u; // 'T'
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale2);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + 3];
        Ubiquitty::Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        continuationPen.extendedAttributes.setTextSizeSpecID(scale2);
        Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(continuation, continuationPen);
    }
    const std::uint8_t lineAttributes[rows] = { Ubiquitty::lineAttributeDoubleWidth, lineAttributeNormal };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(columns) * rows * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    // The band stands at visual columns 4 to 8; its storage is columns 2 and 3.
    // Every column of the band's row up to the block's right edge is drawn by
    // some instance.
    std::array<bool, 8> isCovered {};
    for (std::size_t i = 0; i < counts.cells; ++i) {
        if (static_cast<int>(cells[i].gridPosition.y) != 1)
            continue;
        for (int column = 0; column < 8; ++column) {
            const float centre = static_cast<float>(column) + 0.5f;
            if (centre >= cells[i].gridPosition.x && centre < cells[i].gridPosition.x + cells[i].widthCells)
                isCovered[static_cast<std::size_t>(column)] = true;
        }
    }
    for (int column = 0; column < 8; ++column)
        CHECK_MSG(isCovered[static_cast<std::size_t>(column)], "no column of the band row is left to the surface");
}

namespace {

// A grid holding sized blocks the way `writeSizedBlock` leaves them: per band
// an anchor carrying the code point and `Wide`, then continuations, each cell
// stamped with the spec and its band. Built on the cell-baked path.
// How a scene's backend draws a sized glyph: magnified from the cell's own
// slot, or from windows rasterised at the scale.
enum class SizedWindows : std::uint8_t {
    Magnified,
    Sharp,
};

struct SizedBlockScene {
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    int columns;
    int rows;
    std::vector<Ubiquitty::Cell> grid;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<CellInstance> cells;
    std::vector<DecorationInstance> decorations;
    InstanceCounts counts {};

    SizedBlockScene(int rowCount, int columnCount)
        : columns(columnCount)
        , rows(rowCount)
        , grid(static_cast<std::size_t>(rowCount) * columnCount, fx.blank)
        , lineAttributes(static_cast<std::size_t>(rowCount), lineAttributeNormal)
        , cells(static_cast<std::size_t>(rowCount) * columnCount * 3 + 16)
        , decorations(static_cast<std::size_t>(rowCount) * columnCount * 8)
    {
    }

    Ubiquitty::Cell& at(int row, int column) { return grid[static_cast<std::size_t>(row) * columns + column]; }

    // An `s` by `s` block of a narrow glyph anchored at (row, column).
    void placeBlock(int row, int column, int scale, std::uint32_t codePoint)
    {
        const unsigned id = table.intern(Ubiquitty::TextSizeSpec { static_cast<std::uint8_t>(scale), /*widthCells=*/1 });
        for (int band = 0; band < scale; ++band) {
            for (int offset = 0; offset < scale; ++offset) {
                Ubiquitty::Cell& cell = at(row + band, column + offset);
                Ubiquitty::Pen pen = cell.pen;
                pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, static_cast<std::uint8_t>(band));
                Ubiquitty::cellSetPen(cell, pen);
                if (offset == 0)
                    cell.codePoint = codePoint;
            }
        }
    }

    // A two-cell glyph at (row, column).
    void placeWide(int row, int column, std::uint32_t codePoint)
    {
        Ubiquitty::Cell& lead = at(row, column);
        Ubiquitty::Pen leadPen = lead.pen;
        leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        Ubiquitty::cellSetPen(lead, leadPen);
        lead.codePoint = codePoint;
        Ubiquitty::Cell& continuation = at(row, column + 1);
        Ubiquitty::Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        Ubiquitty::cellSetPen(continuation, continuationPen);
    }

    // The left edge of the rect the sharp window `tile` is given, apart from
    // any rect the fake atlas hands out.
    [[nodiscard]] static float sharpWindowLeft(int tile) { return 4096.0f + 8.0f * static_cast<float>(tile); }

    void build(SizedWindows windows = SizedWindows::Magnified)
    {
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &fx.pal;
        in.textSizeTable = &table;
        in.lineAttributes = lineAttributes.data();
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        FakeAtlas atlas;
        GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
        if (windows == SizedWindows::Sharp) {
            backend.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
                return AtlasGlyph { { sharpWindowLeft(tile.tile), 0.0f }, { sharpWindowLeft(tile.tile) + 8.0f, 16.0f }, true };
            };
        }
        counts = buildCellInstances(in, backend, buffers);
    }

    [[nodiscard]] std::span<const CellInstance> emitted() const { return { cells.data(), std::min(counts.cells, cells.size()) }; }

    [[nodiscard]] static bool isInked(const CellInstance& cell) { return cell.atlasBottomRight.x > cell.atlasTopLeft.x; }

    // Whether some instance of `row` covers the centre of visual column `column`.
    [[nodiscard]] bool isCovered(int row, int column) const
    {
        const float centre = static_cast<float>(column) + 0.5f;
        return std::ranges::any_of(emitted(), [&](const CellInstance& cell) {
            return static_cast<int>(cell.gridPosition.y) == row && centre >= cell.gridPosition.x && centre < cell.gridPosition.x + cell.widthCells;
        });
    }

    // The instance drawn last over the centre of visual column `column` of
    // `row`: what shows there, since the cell pass paints in emission order.
    [[nodiscard]] const CellInstance* topmost(int row, int column) const
    {
        const float centre = static_cast<float>(column) + 0.5f;
        const CellInstance* found = nullptr;
        for (const CellInstance& cell : emitted()) {
            if (static_cast<int>(cell.gridPosition.y) == row && centre >= cell.gridPosition.x && centre < cell.gridPosition.x + cell.widthCells)
                found = &cell;
        }
        return found;
    }

    // The left edge and total width of the inked tiles of `row`.
    [[nodiscard]] std::pair<float, float> inkedExtent(int row) const
    {
        float left = 1e9f;
        float right = -1e9f;
        for (const CellInstance& cell : emitted()) {
            if (static_cast<int>(cell.gridPosition.y) != row || !isInked(cell))
                continue;
            left = std::min(left, cell.gridPosition.x);
            right = std::max(right, cell.gridPosition.x + cell.widthCells);
        }
        return { left, right - left };
    }
};

} // namespace

// A band doubled by its anchor row stands at twice its column, so between the
// storage it reserves and where it stands lie ordinary cells of its row that
// are neither. They draw as any cell does. Hidden as everything left of the
// block's right edge, a letter there vanished and the surface showed through.
TEST(testOrdinaryCellsBetweenADoubledBandsStorageAndWhereItStandsStillDraw)
{
    SizedBlockScene scene(2, 12);
    scene.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    scene.placeBlock(0, 3, 2, 'T');
    scene.at(1, 5).codePoint = 'x';
    scene.build();

    // The band on row 1 has storage at columns 3 and 4 and stands at 6 to 10.
    const bool isLetterDrawn = std::ranges::any_of(scene.emitted(), [](const CellInstance& cell) {
        return static_cast<int>(cell.gridPosition.y) == 1 && cell.gridPosition.x == 5.0f && SizedBlockScene::isInked(cell);
    });
    CHECK_MSG(isLetterDrawn, "the letter at column 5, left of where the block stands, draws");
    for (int column = 0; column < 12; ++column)
        CHECK_MSG(scene.isCovered(1, column), "every column of the band row is drawn by some instance");
    int ordinaryCellsUnderTheBlock = 0;
    for (const CellInstance& cell : scene.emitted()) {
        if (static_cast<int>(cell.gridPosition.y) == 1 && !SizedBlockScene::isInked(cell) && cell.gridPosition.x >= 6.0f && cell.gridPosition.x < 10.0f)
            ++ordinaryCellsUnderTheBlock;
    }
    CHECK_MSG(ordinaryCellsUnderTheBlock == 0, "and no ordinary cell draws under the block");
}

// A sized block is one glyph, so every band takes its anchor row's doubling,
// whichever way the band's own row is set: a band on a DECDWL or DECDHL row
// under an undoubled anchor stays as wide as the band above it and stands
// under it. Doubled by its own row, the lower half of the glyph drew twice as
// wide as the upper and a column away from it.
//
// The band is tiled a cell at a time as its anchor is, not at its own row's
// pitch: in two-cell tiles a scale-3 band drew two of them over four cells
// under an anchor of three.
TEST(testABandTakesItsAnchorRowsWidthOnADoubledRowOfItsOwn)
{
    for (const std::uint8_t bandRowAttribute : { Ubiquitty::lineAttributeDoubleWidth, Ubiquitty::lineAttributeDoubleHeightTop }) {
        for (const int scale : { 2, 3 }) {
            SizedBlockScene scene(3, 14);
            scene.lineAttributes[1] = bandRowAttribute;
            scene.placeBlock(0, 2, scale, 'T');
            scene.build();

            const auto [anchorLeft, anchorWidth] = scene.inkedExtent(0);
            const auto [bandLeft, bandWidth] = scene.inkedExtent(1);
            CHECK_MSG(anchorLeft == 2.0f && anchorWidth == static_cast<float>(scale), "the anchor's band is its scale in cells at its column");
            CHECK_MSG(bandLeft == anchorLeft, "the band below starts under it");
            CHECK_MSG(bandWidth == anchorWidth, "and is as wide");
            // The storage the band reserves on its doubled row is its columns
            // doubled, from 4 on.
            for (int column = 4; column < 4 + 2 * scale; ++column)
                CHECK_MSG(scene.isCovered(1, column), "the storage the band reserves on its own row is painted");
        }
    }
}

// Drawn from sharp windows, the same band asks for the windows its anchor
// asks for and draws each over one cell. Counted at its own row's pitch it
// asked for the first window alone and drew the glyph's left half across the
// whole band.
TEST(testASharpBandOnADoubledRowDrawsTheWindowsItsAnchorDraws)
{
    SizedBlockScene scene(2, 12);
    scene.lineAttributes[1] = Ubiquitty::lineAttributeDoubleWidth;
    scene.placeBlock(0, 2, 2, 'T');
    scene.build(SizedWindows::Sharp);

    for (int row = 0; row < 2; ++row) {
        for (int window = 0; window < 2; ++window) {
            const CellInstance* tile = scene.topmost(row, 2 + window);
            REQUIRE_MSG(tile, "a tile stands at each cell of the band");
            CHECK_MSG(tile->widthCells == 1.0f, "one cell wide");
            CHECK_MSG(tile->atlasTopLeft.x == SizedBlockScene::sharpWindowLeft(window), "holding the window of its own cell");
            CHECK_MSG(tile->atlasBottomRight.x - tile->atlasTopLeft.x == 8.0f, "whole, a texel to a pixel");
        }
    }
}

// Two blocks side by side under a doubled anchor each stand at twice their
// column on the row below, so the second reserves storage where the first
// stands. The storage is painted only where no band stands: painted whole, the
// second block's reservation covered the right half of the first block's band.
TEST(testAReservedStorageIsNotPaintedOverAnotherBlocksBand)
{
    SizedBlockScene scene(2, 12);
    scene.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    scene.placeBlock(0, 0, 2, 'A');
    scene.placeBlock(0, 2, 2, 'B');
    scene.build();

    // On row 1 the first block stands at 0 to 4 and the second at 4 to 8.
    for (int column = 0; column < 8; ++column) {
        const CellInstance* shown = scene.topmost(1, column);
        REQUIRE_MSG(shown, "something is drawn at every column the bands stand at");
        CHECK_MSG(SizedBlockScene::isInked(*shown), "and what shows there is a tile of a band");
    }
}

// A cell of the band's row that a displaced band covers in part draws the part
// of itself the band leaves uncovered: its background and its glyph's share of
// that part, cut at the band's edge, and nothing under the band. Dropping the
// glyph left half a letter's column blank beside the band; drawing it whole drew
// over the band.
TEST(testACellHalfUnderADisplacedBandDrawsItsUncoveredPart)
{
    const auto inkedAt = [](const SizedBlockScene& scene, int row, int column) -> const CellInstance* {
        const CellInstance* shown = scene.topmost(row, column);
        return shown && SizedBlockScene::isInked(*shown) ? shown : nullptr;
    };
    // The fake atlas hands out square rects whose u and v ranges are equal, so
    // a rect cut across keeps its v range whole: the left half keeps the left
    // u edge and half the v span, the right half the right edge.
    const auto isLeftHalf = [](const CellInstance& cell) {
        return approx(cell.atlasTopLeft.x, cell.atlasTopLeft.y)
            && approx(cell.atlasBottomRight.x - cell.atlasTopLeft.x, 0.5f * (cell.atlasBottomRight.y - cell.atlasTopLeft.y));
    };
    const auto isRightHalf = [](const CellInstance& cell) {
        return approx(cell.atlasBottomRight.x, cell.atlasBottomRight.y)
            && approx(cell.atlasBottomRight.x - cell.atlasTopLeft.x, 0.5f * (cell.atlasBottomRight.y - cell.atlasTopLeft.y));
    };

    // A band on a doubled row under a plain anchor stands at 3 to 5, astride
    // the row's cells at 2 to 4 and 4 to 6.
    SizedBlockScene doubledRow(2, 14);
    doubledRow.lineAttributes[1] = Ubiquitty::lineAttributeDoubleWidth;
    doubledRow.placeBlock(0, 3, 2, 'T');
    doubledRow.at(1, 1).codePoint = 'x';
    doubledRow.at(1, 2).codePoint = 'y';
    doubledRow.build();
    for (int column = 0; column < 14; ++column)
        CHECK_MSG(doubledRow.isCovered(1, column), "every column of the doubled band row is painted");
    for (int column = 3; column < 5; ++column) {
        const CellInstance* shown = doubledRow.topmost(1, column);
        CHECK_MSG(shown && SizedBlockScene::isInked(*shown) && shown->widthCells == 1.0f, "the band shows where it stands");
    }
    const CellInstance* leftOfBand = inkedAt(doubledRow, 1, 2);
    const CellInstance* rightOfBand = inkedAt(doubledRow, 1, 5);
    REQUIRE_MSG(leftOfBand && rightOfBand, "each half-covered cell draws its glyph's uncovered half");
    CHECK_MSG(leftOfBand->gridPosition.x == 2.0f && leftOfBand->widthCells == 1.0f && isLeftHalf(*leftOfBand),
        "the x keeps the column left of the band and the left half of its glyph");
    CHECK_MSG(rightOfBand->gridPosition.x == 5.0f && rightOfBand->widthCells == 1.0f && isRightHalf(*rightOfBand),
        "the y keeps the column right of the band and the right half of its glyph");

    // A band under a doubled anchor stands at 6 to 10 on a plain row, with a
    // two-cell glyph astride each of its edges.
    SizedBlockScene plainRow(2, 14);
    plainRow.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    plainRow.placeBlock(0, 3, 2, 'T');
    plainRow.placeWide(1, 5, 0x4E00u);
    plainRow.placeWide(1, 9, 0x4E8Cu);
    plainRow.build();
    for (int column = 0; column < 14; ++column)
        CHECK_MSG(plainRow.isCovered(1, column), "every column of the plain band row is painted");
    for (int column = 6; column < 10; ++column) {
        const CellInstance* shown = plainRow.topmost(1, column);
        CHECK_MSG(shown && SizedBlockScene::isInked(*shown) && shown->gridPosition.x == static_cast<float>(column), "the band's own tile shows at each column it stands at");
    }
    const CellInstance* wideLeft = inkedAt(plainRow, 1, 5);
    const CellInstance* wideRight = inkedAt(plainRow, 1, 10);
    REQUIRE_MSG(wideLeft && wideRight, "each two-cell glyph astride an edge draws its uncovered column");
    CHECK_MSG(wideLeft->gridPosition.x == 5.0f && wideLeft->widthCells == 1.0f && isLeftHalf(*wideLeft),
        "the glyph left of the band draws its left half");
    CHECK_MSG(wideRight->gridPosition.x == 10.0f && wideRight->widthCells == 1.0f && isRightHalf(*wideRight),
        "the glyph right of the band draws its right half");
    for (const CellInstance& cell : plainRow.emitted()) {
        if (static_cast<int>(cell.gridPosition.y) != 1 || cell.widthCells <= 0.0f || cell.gridPosition.x >= 10.0f
            || cell.gridPosition.x + cell.widthCells <= 6.0f)
            continue;
        const bool isBandTile = cell.gridPosition.x >= 6.0f && cell.widthCells == 1.0f;
        CHECK_MSG(isBandTile, "nothing of the row's own cells is drawn under the band");
    }
}

// A block anchored in the right half of a DECDWL row stands past the grid's
// edge and draws nothing, but the storage its bands reserve on the rows below
// is on the grid and is still the block's to paint.
TEST(testABandStandingPastTheEdgeStillPaintsTheStorageItReserves)
{
    SizedBlockScene scene(2, 12);
    scene.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    scene.placeBlock(0, 6, 2, 'T');
    scene.build();

    for (int column = 6; column < 8; ++column)
        CHECK_MSG(scene.isCovered(1, column), "the band's storage on its row is painted");
    for (const CellInstance& cell : scene.emitted())
        CHECK_MSG(cell.gridPosition.x + cell.widthCells <= 12.0f, "and nothing is drawn past the grid");
}

// A doubled block straddling the right edge is cut at the edge: the tiles on
// the grid keep their own share of the glyph, and none is emitted past it.
// Spread over every tile, the clipped half of the glyph was drawn squeezed
// into a quarter of the block and again in the padding.
TEST(testADoubledBlockStraddlingTheEdgeIsCutThereAndKeepsItsTilesShares)
{
    SizedBlockScene scene(2, 10);
    scene.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    scene.placeBlock(0, 4, 2, 'T');
    scene.build();

    // The band on row 1 stands at 8 to 12 on a ten-column grid: its tiles at 8
    // and 9 are on the grid and hold the first two quarters of the glyph.
    std::vector<const CellInstance*> tiles;
    for (const CellInstance& cell : scene.emitted()) {
        CHECK_MSG(cell.gridPosition.x + cell.widthCells <= 10.0f, "nothing is drawn past the grid");
        if (static_cast<int>(cell.gridPosition.y) == 1 && SizedBlockScene::isInked(cell))
            tiles.push_back(&cell);
    }
    REQUIRE_MSG(tiles.size() == 2, "two tiles of the band are on the grid");
    std::ranges::sort(tiles, {}, [](const CellInstance* cell) { return cell->gridPosition.x; });
    const float firstSpan = tiles[0]->atlasBottomRight.x - tiles[0]->atlasTopLeft.x;
    const float secondSpan = tiles[1]->atlasBottomRight.x - tiles[1]->atlasTopLeft.x;
    CHECK_MSG(tiles[0]->gridPosition.x == 8.0f && tiles[1]->gridPosition.x == 9.0f, "at columns 8 and 9");
    CHECK_MSG(approx(firstSpan, secondSpan), "each holding an equal share of the glyph");
    CHECK_MSG(approx(tiles[1]->atlasTopLeft.x, tiles[0]->atlasBottomRight.x), "one after the other");

    // The same block wholly on the grid: four tiles, each a quarter. A tile
    // of the straddling block holds the same quarter, not an eighth.
    SizedBlockScene whole(2, 16);
    whole.lineAttributes[0] = Ubiquitty::lineAttributeDoubleWidth;
    whole.placeBlock(0, 4, 2, 'T');
    whole.build();
    float wholeSpan = 0.0f;
    for (const CellInstance& cell : whole.emitted()) {
        if (static_cast<int>(cell.gridPosition.y) == 1 && cell.gridPosition.x == 8.0f && SizedBlockScene::isInked(cell))
            wholeSpan = cell.atlasBottomRight.x - cell.atlasTopLeft.x;
    }
    CHECK_MSG(wholeSpan > 0.0f && approx(firstSpan, wholeSpan), "the share the same tile holds when the whole block is on the grid");
}

// Drawn sharp, the same block asks for windows of a glyph rasterised twice as
// wide as its scale, one per visual cell of the doubled claim, and draws no
// stretched crop of the double master: the crop resamples the 2x glyph across
// the claim and draws a soft, blurred T. A window may be asked
// for more than once a frame, so what is counted is the windows asked for.
namespace {
std::array<bool, maxSizedWindowsPerBand> doubledWindowsAsked {};
int plainWindowsAsked = 0;
}

TEST(testASizedBlockOnADoubledRowDrawsSharpDoubledWindows)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    const int columns = 12;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns), fx.blank);
    Ubiquitty::Cell& anchor = grid[0];
    anchor.codePoint = 0x54u; // 'T'
    Ubiquitty::Pen stamped = anchor.pen;
    stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
    stamped.extendedAttributes.setTextSizeSpecID(scale3);
    Ubiquitty::cellSetPen(anchor, stamped);
    for (int c = 1; c < 3; ++c) {
        Ubiquitty::Pen continuationPen = grid[static_cast<std::size_t>(c)].pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::cellSetPen(grid[static_cast<std::size_t>(c)], continuationPen);
    }
    const std::uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes;
    in.subpixelGlyphs = true;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;
    in.glyphDoubleSlotWidthInPixels = 16.0f;
    in.glyphDoubleSlotHeightInPixels = 32.0f;
    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) + 8);
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(columns) * 8);
    std::vector<GlyphInstance> glyphs(16);
    std::vector<GlyphInstance> doubles(4);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.doubleGlyphs = doubles.data();
    buffers.doubleGlyphCapacity = doubles.size();
    FakeAtlas atlas;
    GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
        if (tile.widthScale != 2)
            ++plainWindowsAsked;
        else if (tile.tile < maxSizedWindowsPerBand)
            doubledWindowsAsked[tile.tile] = true;
        return AtlasGlyph { { 8.0f * tile.tile, 0.0f }, { 8.0f * tile.tile + 8.0f, 16.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, backend, buffers);

    CHECK_MSG(std::ranges::count(doubledWindowsAsked, true) == 6, "one doubled window per visual cell of the six-cell claim");
    CHECK_MSG(plainWindowsAsked == 0, "and none drawn at the undoubled width");
    CHECK_MSG(counts.glyphs == 6, "each window is a quad of its own");
    CHECK_MSG(counts.doubleGlyphs == 0, "and the stretched double master is not drawn over them");
}

// A width key wider than the text reserves cells the glyph does not reach. The
// glyph keeps its scale's size at the block's left, as the sharp windows draw
// it, and the reserved cells beyond are background: slicing the glyph across the
// whole claim draws a `C` at s=2:w=2 twice as wide as the same `C` at w=1.
TEST(testASizedGlyphInAWiderClaimKeepsItsOwnWidth)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned scale2Width2 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/2, /*widthCells=*/2 });
    const int columns = 8;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns), fx.blank);
    Ubiquitty::Cell& anchor = grid[0];
    anchor.codePoint = 0x43u; // 'C'
    Ubiquitty::Pen stamped = anchor.pen;
    stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
    stamped.extendedAttributes.setTextSizeSpecID(scale2Width2);
    Ubiquitty::cellSetPen(anchor, stamped);
    for (int c = 1; c < 4; ++c) {
        Ubiquitty::Pen continuationPen = grid[static_cast<std::size_t>(c)].pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        continuationPen.extendedAttributes.setTextSizeSpecID(scale2Width2);
        Ubiquitty::cellSetPen(grid[static_cast<std::size_t>(c)], continuationPen);
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) + 8);
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(columns) * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    float inkLeft = 1e9f;
    float inkRight = -1e9f;
    float uSpan = 0.0f;
    for (std::size_t i = 0; i < std::min(counts.cells, cells.size()); ++i) {
        const CellInstance& cell = cells[i];
        if (cell.atlasBottomRight.x <= cell.atlasTopLeft.x)
            continue;
        inkLeft = std::min(inkLeft, cell.gridPosition.x);
        inkRight = std::max(inkRight, cell.gridPosition.x + cell.widthCells);
        uSpan += cell.atlasBottomRight.x - cell.atlasTopLeft.x;
    }
    CHECK_MSG(inkLeft == 0.0f, "the glyph starts at the block's left");
    CHECK_MSG(inkRight == 2.0f, "and spans the two cells its scale gives it, not the four the width key reserves");
    CHECK_MSG(uSpan > 0.0f, "the glyph is drawn");
}

namespace {

// The glyph-stream quads a 2x2 block of 'T' draws under `spec`, on 8x16 cells,
// with the subpixel stream on as the GPU arms run it.
struct FractionalScene {
    std::vector<GlyphInstance> glyphs;
    std::vector<GlyphInstance> doubleGlyphs;
    std::vector<OverlayInstance> boxes;
    std::vector<DecorationInstance> decorations;
    std::vector<CellInstance> cells;
    InstanceCounts counts {};
};

using SizedGlyphEntry = AtlasGlyph (*)(void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile);

struct FractionalOptions {
    std::uint8_t lineAttribute = lineAttributeNormal;
    std::uint32_t codePoint = 'T';
    bool hasDoubleSheet = false;
    bool hasBoxFills = false;
    bool hasRules = false;
    int columns = 0;
    float cellWidth = 8.0f;
};

FractionalScene buildFractionalBlock(Ubiquitty::TextSizeSpec spec, SizedGlyphEntry sizedGlyph = nullptr, FractionalOptions options = {})
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(spec);
    const int claimedColumns = spec.scale * std::max<int>(spec.widthCells, 1);
    const int columns = options.columns > 0 ? options.columns : claimedColumns * 2 + 4;
    const bool isDoubledPair = spec.scale == 1 && options.lineAttribute == lineAttributeDoubleHeightTop;
    const int rows = isDoubledPair ? 2 : spec.scale;
    std::vector<std::uint8_t> lineAttributes(static_cast<std::size_t>(rows), static_cast<std::uint8_t>(lineAttributeNormal));
    lineAttributes[0] = static_cast<std::uint8_t>(options.lineAttribute);
    if (isDoubledPair)
        lineAttributes[1] = static_cast<std::uint8_t>(lineAttributeDoubleHeightBottom);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, fx.blank);
    for (int band = 0; band < rows; ++band) {
        for (int offset = 0; offset < claimedColumns; ++offset) {
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + 1 + offset];
            Ubiquitty::Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            Ubiquitty::penSetTextBand(&pen, static_cast<std::uint8_t>(isDoubledPair ? 0 : band));
            if (options.hasRules) {
                pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
                pen.attributes.add(Ubiquitty::VT::Attribute::Strike);
            }
            Ubiquitty::cellSetPen(cell, pen);
            if (offset == 0)
                cell.codePoint = options.codePoint;
        }
    }
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes.data();
    in.subpixelGlyphs = true;
    in.metrics.cellWidth = options.cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.xHeight = 8.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 2.0f;
    in.metrics.strikeThickness = 1.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;
    in.glyphDoubleSlotWidthInPixels = 16.0f;
    in.glyphDoubleSlotHeightInPixels = 32.0f;
    FractionalScene scene;
    scene.cells.resize(static_cast<std::size_t>(columns) * rows * 3 + 16);
    scene.glyphs.resize(32);
    scene.decorations.resize(static_cast<std::size_t>(columns) * rows * 8);
    InstanceBuffers buffers { scene.cells.data(), scene.cells.size(), scene.decorations.data(), scene.decorations.size() };
    buffers.glyphs = scene.glyphs.data();
    buffers.glyphCapacity = scene.glyphs.size();
    if (options.hasDoubleSheet) {
        scene.doubleGlyphs.resize(32);
        buffers.doubleGlyphs = scene.doubleGlyphs.data();
        buffers.doubleGlyphCapacity = scene.doubleGlyphs.size();
    }
    if (options.hasBoxFills) {
        scene.boxes.resize(128);
        buffers.boxFills = scene.boxes.data();
        buffers.boxFillCapacity = scene.boxes.size();
    }
    FakeAtlas atlas;
    GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.sizedGlyph = sizedGlyph;
    scene.counts = buildCellInstances(in, backend, buffers);
    scene.glyphs.resize(std::min(scene.counts.glyphs, scene.glyphs.size()));
    scene.doubleGlyphs.resize(std::min(scene.counts.doubleGlyphs, scene.doubleGlyphs.size()));
    scene.boxes.resize(std::min(scene.counts.boxFills, scene.boxes.size()));
    scene.decorations.resize(std::min(scene.counts.decorations, scene.decorations.size()));
    return scene;
}

// Every sized raster the builder asked a backend for, in order.
std::vector<SizedTile> sizedRasterRequests;

// A backend that draws sized windows, each at a rect of its own far from any
// rect the fake atlas hands out for the cell's own glyph.
AtlasGlyph recordSizedRaster(void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile)
{
    sizedRasterRequests.push_back(tile);
    const float left = 4096.0f + 8.0f * static_cast<float>(sizedRasterRequests.size());
    return AtlasGlyph { { left, 0.0f }, { left + 8.0f, 16.0f }, true };
}

Ubiquitty::TextSizeSpec halfScaleSpec(std::uint8_t verticalAlign, std::uint8_t horizontalAlign)
{
    Ubiquitty::TextSizeSpec spec { /*scale=*/2, /*widthCells=*/1 };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    spec.verticalAlign = verticalAlign;
    spec.horizontalAlign = horizontalAlign;
    return spec;
}

} // namespace

// Kitty's `n`/`d` draw the text at n/d of the block's size, and `v`/`h` place
// that smaller area inside the s*w by s cells the block still claims. A 2x2
// block at one half is a one-cell glyph: centred, it stands half a cell in from
// the block's left and half a row down, and the two bands split it at their
// shared row boundary, each sampling its own half of the one master.
TEST(testAFractionalBlockDrawsCentredAtItsFraction)
{
    const FractionalScene scene = buildFractionalBlock(halfScaleSpec(/*v=*/2, /*h=*/2));
    CHECK_MSG(scene.glyphs.size() == 2, "one slice per band the smaller glyph reaches");
    if (scene.glyphs.size() != 2)
        return;
    const GlyphInstance& top = scene.glyphs[0].originInPixels.y < scene.glyphs[1].originInPixels.y ? scene.glyphs[0] : scene.glyphs[1];
    const GlyphInstance& bottom = &top == &scene.glyphs[0] ? scene.glyphs[1] : scene.glyphs[0];
    // The block's left is column 1, x = 8; centred, the 8-pixel glyph starts 4 in.
    CHECK_MSG(approx(top.originInPixels.x, 12.0f) && approx(bottom.originInPixels.x, 12.0f), "the glyph is centred across the block");
    CHECK_MSG(approx(top.sizeInPixels.x, 8.0f) && approx(bottom.sizeInPixels.x, 8.0f), "at half the block's glyph width");
    CHECK_MSG(approx(top.originInPixels.y, 8.0f) && approx(top.sizeInPixels.y, 8.0f), "the top band draws the glyph's upper half, from half a row down");
    CHECK_MSG(approx(bottom.originInPixels.y, 16.0f) && approx(bottom.sizeInPixels.y, 8.0f), "the bottom band draws its lower half, into its own row");
    CHECK_MSG(approx(top.atlasBottomRight.y, bottom.atlasTopLeft.y), "the two halves meet at the master's middle");
    CHECK_MSG(approx(top.atlasTopLeft.x, bottom.atlasTopLeft.x) && approx(top.atlasBottomRight.x, bottom.atlasBottomRight.x),
        "both sample the one master's full width");
}

// A fraction is drawn from a raster at the size it is drawn, never the cell's
// raster resampled: s=3 at one half asks for windows of the glyph at 1.5 times
// its size, two rows of two; s=4 at one half is the ordinary scale-2 raster,
// asked for as that scale; s=2 at one half is the cell's own raster, 1:1, and
// asks for none.
TEST(testAFractionalBlockAsksForItsRasterAtItsDrawnSize)
{
    Ubiquitty::TextSizeSpec threeHalves { /*scale=*/3, /*widthCells=*/1 };
    threeHalves.fractionNumerator = 1;
    threeHalves.fractionDenominator = 2;
    threeHalves.verticalAlign = 2;
    threeHalves.horizontalAlign = 2;
    sizedRasterRequests.clear();
    const FractionalScene scene = buildFractionalBlock(threeHalves, recordSizedRaster);
    CHECK_MSG(!sizedRasterRequests.empty(), "the backend is asked for the raster");
    bool isEveryRequestAtOneAndAHalf = true;
    std::array<bool, 4> isWindowAsked {};
    for (const SizedTile& tile : sizedRasterRequests) {
        isEveryRequestAtOneAndAHalf = isEveryRequestAtOneAndAHalf && tile.scale == 3 && tile.fractionNumerator == 1
            && tile.fractionDenominator == 2 && std::fabs(tile.rasterScale() - 1.5) < 1e-9;
        if (tile.band < 2 && tile.tile < 2)
            isWindowAsked[static_cast<std::size_t>(tile.band * 2 + tile.tile)] = true;
        else
            isEveryRequestAtOneAndAHalf = false;
    }
    CHECK_MSG(isEveryRequestAtOneAndAHalf, "every window is of the raster at 3 * 1/2, within its two rows of two");
    CHECK_MSG(std::ranges::all_of(isWindowAsked, [](bool isAsked) { return isAsked; }), "and every one of the four is asked for");
    bool isEveryQuadFromAWindow = !scene.glyphs.empty();
    for (const GlyphInstance& glyph : scene.glyphs)
        isEveryQuadFromAWindow = isEveryQuadFromAWindow && glyph.atlasTopLeft.x >= 4096.0f - 1.0f;
    CHECK_MSG(isEveryQuadFromAWindow, "and every quad samples a window, none the cell's own raster");
    // Centred in a 24 by 48 block, the 12 by 24 picture starts at 6, 12, so
    // its windows step from there, one cell each.
    float left = 1e9f;
    float top = 1e9f;
    for (const GlyphInstance& glyph : scene.glyphs) {
        left = std::min(left, glyph.originInPixels.x);
        top = std::min(top, glyph.originInPixels.y);
    }
    CHECK_MSG(approx(left, 8.0f + 6.0f) && approx(top, 12.0f), "the windows start at the area's origin");

    Ubiquitty::TextSizeSpec fourHalves { /*scale=*/4, /*widthCells=*/1 };
    fourHalves.fractionNumerator = 1;
    fourHalves.fractionDenominator = 2;
    sizedRasterRequests.clear();
    (void)buildFractionalBlock(fourHalves, recordSizedRaster);
    CHECK_MSG(!sizedRasterRequests.empty() && std::ranges::all_of(sizedRasterRequests, [](const SizedTile& tile) {
        return tile.scale == 2 && !tile.isFractional();
    }),
        "a fraction whose product is whole asks for that whole scale's raster");

    sizedRasterRequests.clear();
    const FractionalScene unit = buildFractionalBlock(halfScaleSpec(/*v=*/0, /*h=*/0), recordSizedRaster);
    CHECK_MSG(sizedRasterRequests.empty(), "a product of one asks for no sized raster");
    CHECK_MSG(unit.glyphs.size() == 1 && approx(unit.glyphs[0].sizeInPixels.x, 8.0f) && approx(unit.glyphs[0].sizeInPixels.y, 16.0f),
        "it draws the cell's own raster at its own size");
}

// Aligned top-left, the whole half-size glyph lands in the first band's row and
// the second band draws none of it; aligned bottom-right, the reverse.
TEST(testAFractionalBlockStandsWhereItsAlignmentPutsIt)
{
    const FractionalScene topLeft = buildFractionalBlock(halfScaleSpec(/*v=*/0, /*h=*/0));
    CHECK_MSG(topLeft.glyphs.size() == 1, "only the band the glyph reaches draws");
    if (topLeft.glyphs.size() == 1) {
        CHECK_MSG(approx(topLeft.glyphs[0].originInPixels.x, 8.0f), "at the block's left");
        CHECK_MSG(approx(topLeft.glyphs[0].originInPixels.y, 0.0f) && approx(topLeft.glyphs[0].sizeInPixels.y, 16.0f),
            "filling the top row");
    }
    const FractionalScene bottomRight = buildFractionalBlock(halfScaleSpec(/*v=*/1, /*h=*/1));
    CHECK_MSG(bottomRight.glyphs.size() == 1, "only the band the glyph reaches draws");
    if (bottomRight.glyphs.size() == 1) {
        CHECK_MSG(approx(bottomRight.glyphs[0].originInPixels.x, 16.0f), "against the block's right");
        CHECK_MSG(approx(bottomRight.glyphs[0].originInPixels.y, 16.0f) && approx(bottomRight.glyphs[0].sizeInPixels.y, 16.0f),
            "filling the bottom row");
    }
    for (const CellInstance& cell : topLeft.cells) {
        if (cell.widthCells > 0.0f && cell.gridPosition.x >= 1.0f && cell.gridPosition.x < 3.0f)
            CHECK_MSG(approx(cell.atlasTopLeft.x, cell.atlasBottomRight.x), "the block's cells carry background only");
    }
}

TEST(testFractionalBlocksComposeWithDoubledRowsOnBothAtlasRoutes)
{
    for (std::uint8_t line : { lineAttributeDoubleWidth, lineAttributeDoubleHeightTop, lineAttributeDoubleHeightBottom }) {
        for (bool hasDoubleSheet : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
            const FractionalScene scene = buildFractionalBlock(halfScaleSpec(2, 2), nullptr,
                { .lineAttribute = line, .hasDoubleSheet = hasDoubleSheet });
            const auto& glyphs = hasDoubleSheet ? scene.doubleGlyphs : scene.glyphs;
            REQUIRE(glyphs.size() == 2);
            CHECK(hasDoubleSheet ? scene.glyphs.empty() : scene.doubleGlyphs.empty());
            for (const GlyphInstance& glyph : glyphs) {
                CHECK(approx(glyph.originInPixels.x, 24.0f));
                CHECK(approx(glyph.sizeInPixels.x, 16.0f));
                CHECK(approx(glyph.sizeInPixels.y, 8.0f));
            }
            CHECK(approx(glyphs[0].originInPixels.y, 8.0f));
            CHECK(approx(glyphs[1].originInPixels.y, 16.0f));
        }
    }
}

TEST(testFractionalSingleRowBlocksRetainDoubleHeightPairHalves)
{
    auto spec = halfScaleSpec(2, 2);
    spec.scale = 1;
    for (bool hasDoubleSheet : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
        const FractionalScene scene = buildFractionalBlock(spec, nullptr,
            { .lineAttribute = lineAttributeDoubleHeightTop, .hasDoubleSheet = hasDoubleSheet });
        const auto& glyphs = hasDoubleSheet ? scene.doubleGlyphs : scene.glyphs;
        REQUIRE(glyphs.size() == 2);
        for (const GlyphInstance& glyph : glyphs) {
            CHECK(approx(glyph.originInPixels.x, 20.0f));
            CHECK(approx(glyph.sizeInPixels.x, 8.0f));
            CHECK(approx(glyph.sizeInPixels.y, 8.0f));
        }
        CHECK(approx(glyphs[0].originInPixels.y, 8.0f));
        CHECK(approx(glyphs[1].originInPixels.y, 16.0f));
        // The fake assigns each request a separate slot; the spans still each
        // take half of its complete master, not half of an already cut region.
        const float atlasHeight = hasDoubleSheet ? 0.02f : 7.0f;
        CHECK(approx(glyphs[0].atlasBottomRight.y - glyphs[0].atlasTopLeft.y, atlasHeight));
        CHECK(approx(glyphs[1].atlasBottomRight.y - glyphs[1].atlasTopLeft.y, atlasHeight));

        const FractionalScene clipped = buildFractionalBlock(spec, nullptr,
            { .lineAttribute = lineAttributeDoubleHeightBottom, .hasDoubleSheet = hasDoubleSheet });
        const auto& lowerHalf = hasDoubleSheet ? clipped.doubleGlyphs : clipped.glyphs;
        REQUIRE(lowerHalf.size() == 1);
        CHECK(approx(lowerHalf[0].originInPixels.y, 0));
        CHECK(approx(lowerHalf[0].sizeInPixels.y, 8));
        CHECK(approx(lowerHalf[0].atlasBottomRight.y - lowerHalf[0].atlasTopLeft.y, atlasHeight));
    }
}

TEST(testFractionalDoubledBlocksRequestDoubledNativeWindows)
{
    auto spec = halfScaleSpec(2, 2);
    spec.scale = 3;
    sizedRasterRequests.clear();
    const FractionalScene scene = buildFractionalBlock(spec, recordSizedRaster,
        { .lineAttribute = lineAttributeDoubleWidth, .hasDoubleSheet = true });
    REQUIRE(!sizedRasterRequests.empty());
    CHECK(std::ranges::all_of(sizedRasterRequests, [](SizedTile tile) {
        return tile.widthScale == 2 && std::fabs(tile.rasterScale() - 1.5) < 1e-9 && tile.tile < 3 && tile.band < 2;
    }));
    CHECK(std::ranges::any_of(sizedRasterRequests, [](SizedTile tile) { return tile.tile == 2; }));
    CHECK(scene.doubleGlyphs.empty());
    REQUIRE(!scene.glyphs.empty());
    CHECK(std::ranges::all_of(scene.glyphs, [](const GlyphInstance& glyph) { return glyph.atlasTopLeft.x >= 4096.0f; }));
}

TEST(testTrackingLeavesNativeSizedAndDoubledInkUnstretched)
{
    std::array<Ubiquitty::TextSizeSpec, 4> specifications {
        Ubiquitty::TextSizeSpec { 1, 1 }, Ubiquitty::TextSizeSpec { 3, 1 },
        halfScaleSpec(0, 0), halfScaleSpec(0, 0)
    };
    specifications[3].scale = 3;
    for (const auto& spec : specifications) {
        for (std::uint8_t line : { lineAttributeNormal, lineAttributeDoubleWidth, lineAttributeDoubleHeightTop }) {
            for (bool sharp : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
                const auto sizedGlyph = sharp ? recordSizedRaster : nullptr;
                sizedRasterRequests.clear();
                const auto reference = buildFractionalBlock(spec, sizedGlyph,
                    { .lineAttribute = line, .hasDoubleSheet = true });
                sizedRasterRequests.clear();
                const auto tracked = buildFractionalBlock(spec, sizedGlyph,
                    { .lineAttribute = line, .hasDoubleSheet = true, .cellWidth = 10.5f });
                REQUIRE(!reference.glyphs.empty() || !reference.doubleGlyphs.empty());
                const auto compare = [&](const auto& before, const auto& after) {
                    REQUIRE(before.size() == after.size());
                    for (std::size_t index = 0; index < before.size(); ++index) {
                        const auto& original = before[index];
                        const auto& moved = after[index];
                        CHECK(approx(moved.sizeInPixels.x, original.sizeInPixels.x));
                        CHECK(approx(moved.sizeInPixels.y, original.sizeInPixels.y));
                        CHECK(approx(moved.originInPixels.y, original.originInPixels.y));
                        // Every band uses the anchor row's doubled advance.
                        const float expectedShift = line != lineAttributeNormal ? 5.0f : 2.5f;
                        // Native windows snap the anchor; a fallback quad may
                        // retain its half pixel. Neither may move an internal seam.
                        const std::string placement = "scale " + std::to_string(spec.scale) + " fraction " + std::to_string(spec.fractionNumerator)
                            + " line " + std::to_string(line) + " sharp " + std::to_string(sharp) + " glyph " + std::to_string(index)
                            + " at " + std::to_string(original.originInPixels.x) + "," + std::to_string(original.originInPixels.y)
                            + " moves by " + std::to_string(moved.originInPixels.x - original.originInPixels.x)
                            + ", expected " + std::to_string(expectedShift);
                        CHECK_MSG(std::fabs(moved.originInPixels.x - original.originInPixels.x - expectedShift) <= 0.5f, placement.c_str());
                    }
                };
                compare(reference.glyphs, tracked.glyphs);
                compare(reference.doubleGlyphs, tracked.doubleGlyphs);
            }
        }
    }
}

TEST(testFractionalProceduralBoxesUseOneShapeAcrossTheirBands)
{
    for (std::uint8_t line : { lineAttributeNormal, lineAttributeDoubleWidth }) {
        const FractionalScene scene = buildFractionalBlock(halfScaleSpec(2, 2), nullptr,
            { .lineAttribute = line, .codePoint = 0x2588u, .hasBoxFills = true });
        REQUIRE(scene.boxes.size() == 2);
        CHECK(scene.glyphs.empty());
        const float widthScale = line == lineAttributeNormal ? 1.0f : 2.0f;
        for (const OverlayInstance& box : scene.boxes) {
            CHECK(approx(box.topLeftInPixels.x, 12.0f * widthScale));
            CHECK(approx(box.bottomRightInPixels.x, 20.0f * widthScale));
            CHECK(approx(box.bottomRightInPixels.y - box.topLeftInPixels.y, 8.0f));
        }
        CHECK(approx(scene.boxes[0].topLeftInPixels.y, 8.0f));
        CHECK(approx(scene.boxes[1].bottomRightInPixels.y, 24.0f));
    }
}

TEST(testFractionalRulesFollowTheDrawnAdvanceAndBaseline)
{
    for (std::uint8_t line : { lineAttributeNormal, lineAttributeDoubleWidth }) {
        const FractionalScene scene = buildFractionalBlock(halfScaleSpec(2, 2), nullptr,
            { .lineAttribute = line, .hasRules = true });
        REQUIRE(scene.decorations.size() == 2);
        const float widthScale = line == lineAttributeNormal ? 1.0f : 2.0f;
        for (const DecorationInstance& rule : scene.decorations) {
            CHECK(approx(rule.topLeftInPixels.x, 12.0f * widthScale));
            CHECK(approx(rule.bottomRightInPixels.x, 20.0f * widthScale));
            CHECK(approx(rule.bottomRightInPixels.y - rule.topLeftInPixels.y, 1.0f));
        }
        CHECK(std::ranges::any_of(scene.decorations, [](const DecorationInstance& rule) { return approx(rule.topLeftInPixels.y, 23.0f); }));
        CHECK(std::ranges::any_of(scene.decorations, [](const DecorationInstance& rule) { return approx(rule.topLeftInPixels.y, 17.0f); }));
    }
}

TEST(testFractionalBoxesAndRulesLeaveTheReservedAdvanceEmpty)
{
    auto spec = halfScaleSpec(2, 2);
    spec.widthCells = 3;
    const FractionalScene scene = buildFractionalBlock(spec, nullptr,
        { .codePoint = 0x2588u, .hasBoxFills = true, .hasRules = true });
    REQUIRE(scene.boxes.size() == 2 && scene.decorations.size() == 2);
    // The claim is 48px wide, starting at x=8. Its half-size area starts
    // 12px in; the one-cell glyph and its rules still advance only 8px.
    for (const OverlayInstance& box : scene.boxes) {
        CHECK(approx(box.topLeftInPixels.x, 20));
        CHECK(approx(box.bottomRightInPixels.x, 28));
    }
    for (const DecorationInstance& rule : scene.decorations) {
        CHECK(approx(rule.topLeftInPixels.x, 20));
        CHECK(approx(rule.bottomRightInPixels.x, 28));
    }
}

TEST(testFractionalDoubledInkAndRulesClipAtTheGridEdge)
{
    for (bool hasDoubleSheet : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
        const FractionalScene scene = buildFractionalBlock(halfScaleSpec(2, 1), nullptr,
            { .lineAttribute = lineAttributeDoubleWidth, .hasDoubleSheet = hasDoubleSheet, .hasRules = true, .columns = 5 });
        const auto& glyphs = hasDoubleSheet ? scene.doubleGlyphs : scene.glyphs;
        REQUIRE(glyphs.size() == 2 && scene.decorations.size() == 2);
        for (const GlyphInstance& glyph : glyphs) {
            CHECK(approx(glyph.originInPixels.x, 32));
            CHECK(approx(glyph.sizeInPixels.x, 8));
        }
        for (const DecorationInstance& rule : scene.decorations) {
            CHECK(approx(rule.topLeftInPixels.x, 32));
            CHECK(approx(rule.bottomRightInPixels.x, 40));
        }
    }
    const FractionalScene boxes = buildFractionalBlock(halfScaleSpec(2, 1), nullptr,
        { .lineAttribute = lineAttributeDoubleWidth, .codePoint = 0x2588u, .hasBoxFills = true, .columns = 5 });
    REQUIRE(boxes.boxes.size() == 2);
    for (const OverlayInstance& box : boxes.boxes) {
        CHECK(approx(box.topLeftInPixels.x, 32));
        CHECK(approx(box.bottomRightInPixels.x, 40));
    }
}

// A two-cell glyph at s=2 claims four cells by two rows; at one half it is its
// ordinary two cells, drawn on the wide stream from the wide page's cell-box
// rect, and its cells carry background only. Without the stream it would stay
// on the cell quad, stretched over the whole claim.
TEST(testAFractionalWideGlyphDrawsOnTheWideStreamAtItsFraction)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec = halfScaleSpec(/*v=*/2, /*h=*/2);
    spec.widthCells = 0;
    const unsigned id = table.intern(spec);
    constexpr int columns = 6;
    constexpr int rows = 2;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, fx.blank);
    for (int band = 0; band < rows; ++band) {
        for (int offset = 0; offset < 4; ++offset) {
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + offset];
            Ubiquitty::Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            Ubiquitty::penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            Ubiquitty::cellSetPen(cell, pen);
            if (offset == 0)
                cell.codePoint = 0x6771u; // '東'
        }
    }
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows * 3 + 16);
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> glyphs(8);
    std::vector<GlyphInstance> wides(8);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.wideGlyphs = wides.data();
    buffers.wideGlyphCapacity = wides.size();
    FakeAtlas atlas;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    CHECK_MSG(counts.wideGlyphs == 2, "one slice per band on the wide stream");
    CHECK_MSG(counts.glyphs == 0, "and none on the mono stream");
    for (std::size_t i = 0; i < std::min<std::size_t>(counts.wideGlyphs, wides.size()); ++i) {
        CHECK_MSG(approx(wides[i].originInPixels.x, 8.0f) && approx(wides[i].sizeInPixels.x, 16.0f),
            "two cells wide, centred across the four the block claims");
        CHECK_MSG(approx(wides[i].sizeInPixels.y, 8.0f), "each band holds half of the one-row glyph");
    }
    for (std::size_t i = 0; i < std::min(counts.cells, cells.size()); ++i)
        CHECK_MSG(approx(cells[i].atlasTopLeft.x, cells[i].atlasBottomRight.x), "the block's cells carry background only");
}

// The area is the block shrunk by n/d and moved by the slack `v` and `h` give
// it; no fraction, or a zero numerator, is the whole block.
TEST(testTheFractionalRenderAreaFollowsTheAlignmentKeys)
{
    Ubiquitty::TextSizeSpec spec { /*scale=*/3, /*widthCells=*/1 };
    CHECK_MSG(fractionalRenderArea(spec, { 30.0f, 60.0f }).isWholeBlock(), "no fraction is the whole block");
    spec.fractionDenominator = 3;
    CHECK_MSG(fractionalRenderArea(spec, { 30.0f, 60.0f }).isWholeBlock(), "a zero numerator is the whole block");
    spec.fractionNumerator = 2;
    FractionalRenderArea area = fractionalRenderArea(spec, { 30.0f, 60.0f });
    CHECK_MSG(approx(area.scale, 2.0f / 3.0f) && approx(area.offset.x, 0.0f) && approx(area.offset.y, 0.0f), "top-left by default");
    spec.verticalAlign = 1;
    spec.horizontalAlign = 2;
    area = fractionalRenderArea(spec, { 30.0f, 60.0f });
    CHECK_MSG(approx(area.offset.x, 5.0f) && approx(area.offset.y, 20.0f), "centred across, at the bottom");
    spec.verticalAlign = 2;
    spec.horizontalAlign = 1;
    area = fractionalRenderArea(spec, { 30.0f, 60.0f });
    CHECK_MSG(approx(area.offset.x, 10.0f) && approx(area.offset.y, 10.0f), "against the right, centred down");
}

// The sizing lives on the cell path. A glyph that leaves the cell for a stream
// of its own leaves the tiling behind with it, and the streams do not tile: the
// tile loop is a cell-path loop, and every own-sheet emit below it writes one
// quad per cell. So the question this answers is whether a sized block can
// reach one of those streams at all — the portable arm never does, because it
// runs with subpixelGlyphs off, and its golden therefore says nothing about an
// arm that runs with them on.
TEST(testASizedBlockOnTheSubpixelStreamKeepsItsWidth)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    const int rows = 3;

    // One scale-3 block stamped the way writeSizedBlock stamps it.
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x45u; // 'E'
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    auto build = [&](bool wantsSubpixel, std::vector<CellInstance>& cellsOut,
                     std::vector<GlyphInstance>& glyphsOut) {
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = &table;
        in.subpixelGlyphs = wantsSubpixel;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphSlotWidthInPixels = 8.0f;
        in.glyphSlotHeightInPixels = 16.0f;
        std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
        InstanceBuffers buffers { cellsOut.data(), cellsOut.size(), decos.data(), decos.size() };
        buffers.glyphs = glyphsOut.data();
        buffers.glyphCapacity = glyphsOut.size();
        FakeAtlas at;
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);
    };

    // Subpixel off: the cell path runs, and the anchor's tiles carry the block's
    // width. This is the portable arm, and it is the behaviour the golden holds.
    std::vector<CellInstance> plainCells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<GlyphInstance> plainGlyphs(16);
    const InstanceCounts plain = build(/*wantsSubpixel=*/false, plainCells, plainGlyphs);
    CHECK_MSG(plain.glyphs == 0, "with subpixelGlyphs off nothing leaves the cell path");
    float plainInkedWidth = 0.0f;
    for (std::size_t i = 0; i < plain.cells; ++i) {
        if (plainCells[i].atlasBottomRight.x > plainCells[i].atlasTopLeft.x)
            plainInkedWidth += plainCells[i].widthCells;
    }
    CHECK_MSG(plainInkedWidth > 0.0f, "the block draws something on the cell path");

    // Subpixel on: same grid, same table, same spec.
    std::vector<CellInstance> subCells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<GlyphInstance> subGlyphs(16);
    const InstanceCounts sub = build(/*wantsSubpixel=*/true, subCells, subGlyphs);

    // Whatever the streams do, the block must still cover the columns it claims.
    // Summing the inked width across both paths is what makes the two arms
    // comparable: a cell that handed its glyph away carries no UV, so counting
    // cells alone would call the handover a loss when it is a move.
    float subInkedWidth = 0.0f;
    for (std::size_t i = 0; i < sub.cells; ++i) {
        if (subCells[i].atlasBottomRight.x > subCells[i].atlasTopLeft.x)
            subInkedWidth += subCells[i].widthCells;
    }
    for (std::size_t i = 0; i < sub.glyphs; ++i)
        subInkedWidth += subGlyphs[i].sizeInPixels.x / 8.0f;

    const std::string covered = "a sized block covers the same width whichever stream draws it: "
                                "cell path "
        + std::to_string(plainInkedWidth) + " columns, subpixel stream "
        + std::to_string(subInkedWidth) + " over " + std::to_string(sub.glyphs) + " glyph(s)";
    CHECK_MSG(approx(subInkedWidth, plainInkedWidth), covered.c_str());
}

// A solid block is drawn by the box decomposition rather than from the atlas
// wherever the backend offers a box-fill stream: prefersProcedural covers
// 0x2580..0x259F, so U+2588 refuses the font's glyph there whatever it offers.
// Which arms those are is decided one level up — hasBoxSheet is
// `out.boxFills != nullptr` (:287), and SoftwareRenderer never sets that buffer,
// so the portable arm never enters this branch at all and draws a block through
// the ordinary glyph path. This case supplies the buffer, so it stands in for
// the arms that do. The path's rect is built from gridX to
// gridX + 1, one cell, which is right for an ordinary cell and wrong for a
// sized anchor: writeSizedBlock gives the continuations no code point, so if
// the anchor draws one cell the rest of the block draws nothing at all.
TEST(testASizedSolidBlockFillsTheColumnsItClaims)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    const int rows = 3;
    const float cellWidth = 8.0f;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x2588u; // FULL BLOCK
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<OverlayInstance> boxFills(64);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.boxFills = boxFills.data();
    buffers.boxFillCapacity = boxFills.size();
    FakeAtlas at;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);

    CHECK_MSG(counts.boxFills > 0, "a solid block emits box fills rather than an atlas glyph");

    // The rightmost edge any fill reaches, against the edge the block claims.
    // Measuring the extent rather than counting quads keeps this indifferent to
    // how the decomposition splits a fill.
    float rightmost = 0.0f;
    for (std::size_t i = 0; i < counts.boxFills && i < boxFills.size(); ++i)
        rightmost = std::max(rightmost, boxFills[i].bottomRightInPixels.x);

    const float claimed = static_cast<float>(Ubiquitty::claimedRunColumns(grid[0], &table, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow)) * cellWidth;
    const std::string reach = "a scale-3 block claims " + std::to_string(claimed)
        + "px and its fills reach " + std::to_string(rightmost) + "px";
    CHECK_MSG(rightmost >= claimed - 0.5f, reach.c_str());

    // And nothing past it. A continuation carries no code point, so any ink
    // beyond the claim is a cell drawing at an origin it does not own — which
    // is what a stray sliver beside a block looks like from the pixels.
    float strayLeft = 0.0f;
    for (std::size_t i = 0; i < counts.boxFills && i < boxFills.size(); ++i) {
        if (boxFills[i].topLeftInPixels.x >= claimed + 0.5f)
            strayLeft = std::max(strayLeft, boxFills[i].topLeftInPixels.x);
    }
    const std::string stray = "no fill starts past the claimed " + std::to_string(claimed)
        + "px; leftmost stray at " + std::to_string(strayLeft);
    CHECK_MSG(approx(strayLeft, 0.0f), stray.c_str());
}

// Two sized blocks in one band region, which is what a real feed makes and what
// a single-block case cannot see: a letter block and a solid block with plain
// cells between them. ubiquitty-tests decoded a macos render of exactly this
// shape and found ink two cells past the solid block that neither of us could
// place, so this asks whether one block's geometry reaches into what follows it.
TEST(testTwoSizedBlocksInARowStayInTheirOwnColumns)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 12;
    const int rows = 3;
    const float cellWidth = 8.0f;
    const int letterColumn = 0;
    const int blockColumn = 5;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    auto stampBlock = [&](int startColumn, std::uint32_t codePoint) {
        for (int band = 0; band < rows; ++band) {
            const std::size_t base = static_cast<std::size_t>(band) * columns + startColumn;
            Ubiquitty::Cell& anchor = grid[base];
            anchor.codePoint = codePoint;
            Ubiquitty::Pen stamped = anchor.pen;
            stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
            stamped.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(anchor, stamped);
            for (int c = 1; c < 3; ++c) {
                Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
                Ubiquitty::Pen continuationPen = continuation.pen;
                continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
                continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
                Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
                Ubiquitty::cellSetPen(continuation, continuationPen);
            }
        }
    };
    stampBlock(letterColumn, 0x45u); // 'E' — takes the glyph path
    stampBlock(blockColumn, 0x2588u); // FULL BLOCK — takes the box decomposition

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphReferenceAdvanceInPixels = cellWidth;
    in.glyphSlotWidthInPixels = cellWidth;
    in.glyphSlotHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<OverlayInstance> boxFills(64);
    std::vector<GlyphInstance> glyphs(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.boxFills = boxFills.data();
    buffers.boxFillCapacity = boxFills.size();
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    FakeAtlas at;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);

    // The solid block owns columns 5..8. Nothing it draws may land past that,
    // and nothing the letter draws may reach into it.
    const float blockLeft = static_cast<float>(blockColumn) * cellWidth;
    const float blockRight = blockLeft
        + static_cast<float>(Ubiquitty::claimedRunColumns(grid[static_cast<std::size_t>(blockColumn)], &table, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow)) * cellWidth;

    float pastBlock = 0.0f;
    for (std::size_t i = 0; i < counts.boxFills && i < boxFills.size(); ++i)
        if (boxFills[i].topLeftInPixels.x >= blockRight + 0.5f)
            pastBlock = std::max(pastBlock, boxFills[i].topLeftInPixels.x);
    const std::string beyond = "no box fill starts past the block's right edge at "
        + std::to_string(blockRight) + "px; leftmost at " + std::to_string(pastBlock);
    CHECK_MSG(approx(pastBlock, 0.0f), beyond.c_str());

    // The letter's quads belong to the letter's columns.
    float letterReach = 0.0f;
    for (std::size_t i = 0; i < counts.glyphs && i < glyphs.size(); ++i)
        letterReach = std::max(letterReach, glyphs[i].originInPixels.x + glyphs[i].sizeInPixels.x);
    const std::string reach = "the letter block stops before the solid block at "
        + std::to_string(blockLeft) + "px; it reaches " + std::to_string(letterReach);
    CHECK_MSG(letterReach <= blockLeft + 0.5f, reach.c_str());
}

// A sized WIDE glyph. `placeSizedGlyph` takes its column count from the code
// point, so a CJK anchor at scale 3 claims 3*2 = 6 columns, and the builder's
// isWide for a sized cell reads the code point's own width — so the cell hands
// its glyph to the wide sheet, which lays out a two-cell slot. Two of the three
// sizing paths needed the claim taught to them; this asks whether the third does.
TEST(testASizedWideGlyphCoversItsClaimOnTheWideSheet)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/0 });
    CHECK(scale3 != 0);

    const std::uint32_t cjk = 0x4E00u; // width 2
    CHECK_MSG(Ubiquitty::Unicode::Width::forCodePoint(cjk, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow) == 2, "the fixture's code point is wide");

    const int columns = 10;
    const int rows = 3;
    const float cellWidth = 8.0f;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * columns;
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = cjk;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 6; ++c) { // scale 3 * width 2
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = cellWidth;
    // Production geometry, not a round number: the Metal font geometry sets the
    // wide slot to 2 * the MONO slot, and a mono slot is the cell box plus a
    // margin on each side. A slot of exactly two cells would hide whether the
    // multiplier scales the margins along with the content, which is the one
    // way this can be wrong and still look plausible.
    const float margin = 1.0f;
    in.glyphMarginInPixels = { margin, margin };
    in.glyphWideSlotWidthInPixels = 2.0f * (cellWidth + 2.0f * margin);
    in.glyphWideSlotHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> wides(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.wideGlyphs = wides.data();
    buffers.wideGlyphCapacity = wides.size();
    FakeAtlas at;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);

    const float claimed = static_cast<float>(Ubiquitty::claimedRunColumns(grid[0], &table, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow)) * cellWidth;
    CHECK_MSG(approx(claimed, 6.0f * cellWidth), "a scale-3 wide glyph claims six columns");

    // Whichever stream drew it, the ink has to reach the claim's right edge.
    float rightmost = 0.0f;
    for (std::size_t i = 0; i < counts.cells; ++i)
        if (cells[i].atlasBottomRight.x > cells[i].atlasTopLeft.x)
            rightmost = std::max(rightmost, (cells[i].gridPosition.x + cells[i].widthCells) * cellWidth);
    for (std::size_t i = 0; i < counts.wideGlyphs && i < wides.size(); ++i)
        rightmost = std::max(rightmost, wides[i].originInPixels.x + wides[i].sizeInPixels.x);

    const std::string reach = "a scale-3 wide glyph claims " + std::to_string(claimed)
        + "px and its ink reaches " + std::to_string(rightmost) + "px over "
        + std::to_string(counts.wideGlyphs) + " wide quad(s)";
    CHECK_MSG(rightmost >= claimed - 0.5f, reach.c_str());

    // The quad's left bearing offset carries the same factor as its width. A
    // quad stretched to three slots whose origin backs off one margin starts its
    // ink two margins inside the columns it claims, and nothing above would
    // notice: the right edge still reaches the claim, so a one-sided check reads
    // the shift as correct. Measured against this same glyph at scale 1, which is the
    // only reading that does not restate the expression under test.
    Ubiquitty::TextSizeTable plainTable;
    const unsigned scale1 = plainTable.intern(Ubiquitty::TextSizeSpec { /*scale=*/1, /*widthCells=*/0 });
    std::vector<Ubiquitty::Cell> plainGrid(static_cast<std::size_t>(columns) * rows, blank);
    plainGrid[0].codePoint = cjk;
    {
        Ubiquitty::Pen wide = plainGrid[0].pen;
        wide.attributes.add(Ubiquitty::VT::Attribute::Wide);
        Ubiquitty::cellSetPen(plainGrid[0], wide);
        Ubiquitty::Cell& continuation = plainGrid[1];
        Ubiquitty::Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        Ubiquitty::cellSetPen(continuation, continuationPen);
    }
    CHECK(scale1 != 0);

    BuildInputs unsized = in;
    unsized.cells = plainGrid.data();
    unsized.textSizeTable = &plainTable;
    std::vector<CellInstance> unsizedCells(cells.size());
    std::vector<DecorationInstance> unsizedDecos(decos.size());
    std::vector<GlyphInstance> unsizedWides(16);
    InstanceBuffers unsizedBuffers { unsizedCells.data(), unsizedCells.size(),
        unsizedDecos.data(), unsizedDecos.size() };
    unsizedBuffers.wideGlyphs = unsizedWides.data();
    unsizedBuffers.wideGlyphCapacity = unsizedWides.size();
    FakeAtlas plainAtlas;
    const InstanceCounts plain = buildCellInstances(unsized, UbiquittyTest::glyphBackendFor(plainAtlas), unsizedBuffers);
    CHECK_MSG(plain.wideGlyphs > 0, "the unsized reference reaches the wide sheet too");

    // Both quads start at column 0, so the offset is the whole left edge.
    const float sizedOffset = -wides[0].originInPixels.x;
    const float unsizedOffset = -unsizedWides[0].originInPixels.x;
    const float slots = claimed / (2.0f * cellWidth);
    const std::string bearing = "the sized quad's left bearing scales with it: unsized "
        + std::to_string(unsizedOffset) + "px, sized " + std::to_string(sizedOffset)
        + "px, expected " + std::to_string(unsizedOffset * slots);
    CHECK_MSG(approx(sizedOffset, unsizedOffset * slots), bearing.c_str());

    // The other edge goes unasserted, and the reason is worth writing down because
    // the obvious way to assert it is wrong. A wide quad legitimately exceeds the
    // columns it covers: the slot carries a bearing allowance, so an UNSIZED wide
    // glyph already reaches 19px over a 16px column span at this geometry. Held
    // against the cell path's span, which has no allowance at all, a sized quad
    // therefore looks 11px too wide when it is carrying the same overhang scaled.
    // The two paths express bearings differently rather than placing ink
    // differently, so comparing one's quad with the other's columns measures the
    // difference in bookkeeping.
    //
    // What does survive that correction is small: the width scales by the claim
    // and the origin backs off one UNSCALED margin, so a scale-3 glyph sits about
    // 2px right of where a fully scaled slot would put it. That offset is not
    // pinned here.
}

// The last of the four own-sheet branches at :1227. Sizing is a statement about
// a CELL and DECDHL one about its ROW, and the band crop already says a sized
// cell on a doubled row takes the crop alone — so the two do meet, and the
// question is whether the double sheet's quad carries the claim the way the wide
// sheet's does. Measured rather than reasoned about, because a branch that
// reads as unreachable may not be.
TEST(testASizedCellOnADoubledRowCoversItsClaim)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 12;
    const float cellWidth = 8.0f;
    const float margin = 1.0f;

    std::vector<Ubiquitty::Cell> row(columns, blank);
    row[0].codePoint = 0x41u;
    Ubiquitty::Pen stamped = row[0].pen;
    stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
    stamped.extendedAttributes.setTextSizeSpecID(scale3);
    Ubiquitty::penSetTextBand(&stamped, 0);
    Ubiquitty::cellSetPen(row[0], stamped);
    for (int c = 1; c < 3; ++c) {
        Ubiquitty::Pen continuationPen = row[static_cast<std::size_t>(c)].pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&continuationPen, 0);
        Ubiquitty::cellSetPen(row[static_cast<std::size_t>(c)], continuationPen);
    }

    const uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };
    BuildInputs in;
    in.cells = row.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphMarginInPixels = { margin, margin };
    in.glyphReferenceAdvanceInPixels = cellWidth;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphDoubleSlotWidthInPixels = 2.0f * (cellWidth + 2.0f * margin);
    in.glyphDoubleSlotHeightInPixels = 2.0f * (16.0f + 2.0f * margin);

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * 8);
    std::vector<GlyphInstance> doubles(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.doubleGlyphs = doubles.data();
    buffers.doubleGlyphCapacity = doubles.size();
    FakeAtlas at;
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(at), buffers);

    // Whether the branch is reachable at all is half the answer, and the half
    // that was assumed twice before.
    const std::string reached = "a sized cell on a doubled row emits "
        + std::to_string(counts.doubleGlyphs) + " double quad(s)";
    if (counts.doubleGlyphs == 0) {
        // Unreachable in this configuration: the cell path drew it, which is
        // where the claim is already honoured. Recorded rather than asserted
        // away, so a change that starts routing here is visible.
        CHECK_MSG(counts.cells > 0, reached.c_str());
        return;
    }

    float rightmost = 0.0f;
    for (std::size_t i = 0; i < counts.doubleGlyphs && i < doubles.size(); ++i)
        rightmost = std::max(rightmost, doubles[i].originInPixels.x + doubles[i].sizeInPixels.x);
    const float claimed = static_cast<float>(Ubiquitty::claimedRunColumns(row[0], &table, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow)) * 2.0f * cellWidth;
    const std::string reach = "a scale-3 cell on a doubled row claims " + std::to_string(claimed)
        + "px and its double quad reaches " + std::to_string(rightmost);
    CHECK_MSG(rightmost >= claimed - 0.5f, reach.c_str());
}

// A sized block's bands each anchor the same code point, so a stream that emits
// per anchor draws the whole glyph once per row — the outcome that makes a replica
// which cannot band-crop flatten its sized cells (Sync/HistoryCache.cpp). Every mono
// stream avoids it by cropping each band to its own slice of the master; the
// colour path is exempt from the crop at :1141 because its quad is a centred
// square rather than the cell box, so it needs its own.
//
// Two properties, and the count alone establishes neither. Three quads is what
// an uncropped stream emits and what a cropped one emits — the difference is
// that uncropped, each of the three carries the WHOLE master at one cell's size,
// so the block shows three small emoji stacked, and cropped, the three carry
// consecutive thirds of one master at three cells' size. So this asserts the
// slices tile and the square grew, not how many instances there are.
TEST(testASizedEmojiBandCropsOneGrownGlyphAcrossItsRows)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/0 });
    CHECK(scale3 != 0);

    const std::uint32_t emoji = 0x1F600u; // GRINNING FACE, width 2
    const int columns = 12;
    const int rows = 3;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * columns;
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = emoji;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.attributes.add(Ubiquitty::VT::Attribute::Emoji);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 6; ++c) { // scale 3 * width 2
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> colours(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.colorGlyphs = colours.data();
    buffers.colorGlyphCapacity = colours.size();
    // CacheBackend rather than FakeAtlas: StrictAtlas aborts on colorGlyph by
    // design, so a fixture that does not model colour cannot reach this path at
    // all — which is the base refusing to answer a question it has no answer to.
    CacheBackend backend;
    const InstanceCounts counts = buildCellInstances(in, backend.backend(), buffers);

    const std::string quads = "a scale-3 emoji block emits "
        + std::to_string(counts.colorGlyphs) + " colour quad(s) over its "
        + std::to_string(rows) + " bands";
    CHECK_MSG(counts.colorGlyphs == static_cast<std::size_t>(rows), quads.c_str());
    if (counts.colorGlyphs != static_cast<std::size_t>(rows))
        return;

    // The square grew with the block. An unsized emoji at this cell takes
    // floor(min(2*8, 16)) = 16px; a scale-3 block takes three of those. Without
    // this the glyph draws at 1x, centred in a 3x box, while every sized letter
    // beside it stretches — which is the state the count cannot distinguish.
    const float unsized = Ubiquitty::Rendering::colourGlyphSquareInPixels(
        in.metrics.cellWidth, in.metrics.cellHeight);
    const float grown = unsized * static_cast<float>(rows);
    for (std::size_t i = 0; i < counts.colorGlyphs; ++i) {
        const std::string wide = "band " + std::to_string(i) + " draws "
            + std::to_string(colours[i].sizeInPixels.x) + "px wide, not the block's "
            + std::to_string(grown);
        CHECK_MSG(std::abs(colours[i].sizeInPixels.x - grown) <= 0.5f, wide.c_str());
    }

    // The bands tile the block: contiguous in the destination, so no seam or
    // overlap along a shared row boundary, and covering the square exactly.
    for (std::size_t i = 1; i < counts.colorGlyphs; ++i) {
        const float previousBottom = colours[i - 1].originInPixels.y + colours[i - 1].sizeInPixels.y;
        const std::string seam = "band " + std::to_string(i) + " starts at "
            + std::to_string(colours[i].originInPixels.y) + ", and band "
            + std::to_string(i - 1) + " ends at " + std::to_string(previousBottom);
        CHECK_MSG(std::abs(colours[i].originInPixels.y - previousBottom) <= 0.01f, seam.c_str());
    }

    // And they tile the MASTER, which is what separates a cropped band from an
    // uncropped one: three quads each carrying the whole sheet region draw three
    // emoji, and three carrying consecutive thirds draw one. Keyed on consecutiveness rather
    // than on the literal thirds, so the assertion still holds if the square is
    // bounded by the block's width and the outer bands fall away.
    for (std::size_t i = 1; i < counts.colorGlyphs; ++i) {
        const std::string slice = "band " + std::to_string(i) + " samples from v="
            + std::to_string(colours[i].atlasTopLeft.y) + ", and band "
            + std::to_string(i - 1) + " stops at v="
            + std::to_string(colours[i - 1].atlasBottomRight.y);
        CHECK_MSG(std::abs(colours[i].atlasTopLeft.y - colours[i - 1].atlasBottomRight.y) <= 0.0001f,
            slice.c_str());
        const std::string advanced = "band " + std::to_string(i)
            + " samples the same region as the band above it, so the master draws twice";
        CHECK_MSG(colours[i].atlasTopLeft.y > colours[i - 1].atlasTopLeft.y, advanced.c_str());
    }
}

// The scale-1 control for the case above. The band crop reduces to the unsized
// geometry when there is no block — one quad, the whole master, the unsized
// square — and a change that only ever ran against scale 3 could move
// every emoji in the terminal without a test saying so.
TEST(testAnUnsizedEmojiKeepsItsWholeMasterAndItsCellSquare)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    const std::uint32_t emoji = 0x1F600u; // GRINNING FACE, width 2
    const int columns = 12;
    const int rows = 1;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    Ubiquitty::Cell& anchor = grid[0];
    anchor.codePoint = emoji;
    Ubiquitty::Pen stamped = anchor.pen;
    stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
    stamped.attributes.add(Ubiquitty::VT::Attribute::Emoji);
    Ubiquitty::cellSetPen(anchor, stamped);
    Ubiquitty::Pen continuationPen = grid[1].pen;
    continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    Ubiquitty::cellSetPen(grid[1], continuationPen);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> colours(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.colorGlyphs = colours.data();
    buffers.colorGlyphCapacity = colours.size();
    CacheBackend backend;
    const InstanceCounts counts = buildCellInstances(in, backend.backend(), buffers);

    const std::string one = "an unsized emoji emits " + std::to_string(counts.colorGlyphs)
        + " colour quad(s)";
    CHECK_MSG(counts.colorGlyphs == 1, one.c_str());
    if (counts.colorGlyphs != 1)
        return;

    const float square = Ubiquitty::Rendering::colourGlyphSquareInPixels(
        in.metrics.cellWidth, in.metrics.cellHeight);
    const std::string side = "an unsized emoji draws " + std::to_string(colours[0].sizeInPixels.x)
        + "px, not its cell square " + std::to_string(square);
    CHECK_MSG(std::abs(colours[0].sizeInPixels.x - square) <= 0.5f, side.c_str());
    CHECK_MSG(std::abs(colours[0].sizeInPixels.y - square) <= 0.5f, side.c_str());

    // The whole master, uncropped: a band crop that leaked into the unsized path
    // would show up here as a fraction of the sheet region and nowhere else.
    const std::string whole = "an unsized emoji samples v=["
        + std::to_string(colours[0].atlasTopLeft.y) + ", "
        + std::to_string(colours[0].atlasBottomRight.y) + "]";
    CHECK_MSG(colours[0].atlasBottomRight.y > colours[0].atlasTopLeft.y, whole.c_str());
}

// A backend that rasterises at the block's size answers per window, and the
// colour stream draws one ordinary square quad per window instead of one
// magnified slice per band. The windows are asserted against the rects the
// backend returned — each encodes its own (band, tile) — so a stream that
// consulted the entry and then sampled the wrong window fails on the mapping,
// not just on the count. With the entry null the soft path stands, which the
// band-crop test above pins; together they hold both halves of the fallback
// contract.
TEST(testASharpSizedEmojiDrawsOneWindowPerSquare)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/0 });
    CHECK(scale3 != 0);

    const std::uint32_t emoji = 0x1F600u; // GRINNING FACE, width 2
    const int columns = 12;
    const int rows = 3;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * columns;
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = emoji;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.attributes.add(Ubiquitty::VT::Attribute::Emoji);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 6; ++c) {
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> colours(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.colorGlyphs = colours.data();
    buffers.colorGlyphCapacity = colours.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // The window's identity rides its rect: u encodes the tile column, v the
    // band row, both offset so a zero never reads as "unset".
    b.sizedColorGlyph = [](void*, ClusterSpan, SizedTile tile) {
        return AtlasGlyph { { static_cast<float>(10 + tile.tile), static_cast<float>(10 + tile.band) },
            { static_cast<float>(10 + tile.tile) + 1.0f, static_cast<float>(10 + tile.band) + 1.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    // Cell 8x16 makes the colour square 16, so a scale-3 block is 3x3 windows
    // of 16px, one quad each, aligned with the cell rows.
    const std::string quads = "a sharp scale-3 emoji emits "
        + std::to_string(counts.colorGlyphs) + " colour quad(s), wanting 9";
    CHECK_MSG(counts.colorGlyphs == 9, quads.c_str());
    if (counts.colorGlyphs != 9)
        return;

    for (std::size_t i = 0; i < 9; ++i) {
        const int tile = static_cast<int>(colours[i].atlasTopLeft.x) - 10;
        const int bandRow = static_cast<int>(colours[i].atlasTopLeft.y) - 10;
        const std::string window = "quad " + std::to_string(i) + " samples window (band "
            + std::to_string(bandRow) + ", tile " + std::to_string(tile) + ")";
        CHECK_MSG(bandRow >= 0 && bandRow < 3 && tile >= 0 && tile < 3, window.c_str());
        // The quad sits where its window's grid position says, 16px square.
        const float squareLeft = colours[0].originInPixels.x - 0.0f;
        const std::string placed = "quad " + std::to_string(i) + " at ("
            + std::to_string(colours[i].originInPixels.x) + ", " + std::to_string(colours[i].originInPixels.y)
            + ") for window (band " + std::to_string(bandRow) + ", tile " + std::to_string(tile) + ")";
        CHECK_MSG(approx(colours[i].originInPixels.x, squareLeft + 16.0f * static_cast<float>(tile)), placed.c_str());
        CHECK_MSG(approx(colours[i].originInPixels.y, 16.0f * static_cast<float>(bandRow)), placed.c_str());
        CHECK_MSG(approx(colours[i].sizeInPixels.x, 16.0f) && approx(colours[i].sizeInPixels.y, 16.0f), placed.c_str());
    }
}

// The subpixel stream's version of the same contract: a sized letter with a
// sharp backend emits one ordinary cell quad per claimed column, each sampling
// the window the backend returned, in place of the one quad stretching the
// band-cropped slot across the claim. The soft shape is pinned by
// testASizedBlockOnTheSubpixelStreamKeepsItsWidth above, which runs against a
// backend without the entry.
TEST(testASharpSizedLetterDrawsOneQuadPerColumn)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    const int rows = 3;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x45u; // 'E', width 1: the subpixel stream's case
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.subpixelGlyphs = true;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> glyphs(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    b.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
        return AtlasGlyph { { static_cast<float>(10 + tile.tile), static_cast<float>(10 + tile.band) },
            { static_cast<float>(10 + tile.tile) + 1.0f, static_cast<float>(10 + tile.band) + 1.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    // Three bands, three claimed columns each: nine cell-sized quads.
    const std::string quads = "a sharp scale-3 letter emits "
        + std::to_string(counts.glyphs) + " glyph quad(s), wanting 9";
    CHECK_MSG(counts.glyphs == 9, quads.c_str());
    if (counts.glyphs != 9)
        return;

    for (std::size_t i = 0; i < 9; ++i) {
        const int tile = static_cast<int>(glyphs[i].atlasTopLeft.x) - 10;
        const int bandRow = static_cast<int>(glyphs[i].atlasTopLeft.y) - 10;
        const std::string window = "quad " + std::to_string(i) + " samples window (band "
            + std::to_string(bandRow) + ", tile " + std::to_string(tile) + ")";
        CHECK_MSG(bandRow >= 0 && bandRow < 3 && tile >= 0 && tile < 3, window.c_str());
        // An ordinary cell quad: the unsized slot size, stepped one cell per
        // tile — not the slot stretched three columns wide.
        const std::string shape = "quad " + std::to_string(i) + " is "
            + std::to_string(glyphs[i].sizeInPixels.x) + "px wide, wanting the unsized 8";
        CHECK_MSG(approx(glyphs[i].sizeInPixels.x, 8.0f), shape.c_str());
        // No margin input in this fixture, so the backoff term is zero and a
        // band's quads sit exactly on their row.
        CHECK_MSG(approx(glyphs[i].originInPixels.y, 16.0f * static_cast<float>(bandRow)), shape.c_str());
    }
}

// Every band of a sized block on the subpixel stream is one picture's slice:
// each band asks for its windows at the block's one phase, and lands them at
// the same whole pixels, a fractional advance apart on average, each window as
// wide as the texels it shows. A window landed at a fraction of a pixel would
// show its texels between the device's pixels, blended at its own fraction,
// and the windows of a band would disagree at every join.
TEST(testEveryBandOfASubpixelBlockLandsItsWindowsAtOneOriginAndPhase)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    constexpr int scale = 3;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/scale, /*widthCells=*/1 });
    const int columns = 6;
    const int rows = scale;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, fx.blank);
    for (int band = 0; band < rows; ++band) {
        for (int c = 0; c < scale; ++c) {
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + 1 + c];
            cell.codePoint = c == 0 ? 0x58u : 0u;
            Ubiquitty::Pen pen = cell.pen;
            pen.attributes.add(c == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            Ubiquitty::penSetTextBand(&pen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(cell, pen);
        }
    }
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.subpixelGlyphs = true;
    in.originInPixels = { 0.3f, 0.0f };
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 7.6f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> glyphs(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    static std::array<std::uint32_t, scale> phaseOfBand {};
    phaseOfBand.fill(~0u);
    b.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t variant, GlyphPass, SizedTile tile) {
        phaseOfBand[tile.band] = variantPhase(variant);
        // Each window in its own row of slots, a cell box wide.
        const float left = 100.0f * static_cast<float>(tile.tile);
        const float top = 100.0f * static_cast<float>(tile.band);
        return AtlasGlyph { { left, top }, { left + 8.0f, top + 16.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);
    REQUIRE(counts.glyphs == static_cast<std::uint32_t>(scale * scale));

    for (int band = 1; band < scale; ++band) {
        const std::string phase = "band " + std::to_string(band) + " asks for phase " + std::to_string(phaseOfBand[static_cast<std::size_t>(band)])
            + ", band 0 for " + std::to_string(phaseOfBand[0]);
        CHECK_MSG(phaseOfBand[static_cast<std::size_t>(band)] == phaseOfBand[0], phase.c_str());
    }
    std::array<float, scale> leftOfWindow {};
    for (std::size_t i = 0; i < static_cast<std::size_t>(counts.glyphs); ++i) {
        const GlyphInstance& g = glyphs[i];
        const int tile = static_cast<int>(std::lround(g.atlasTopLeft.x / 100.0f));
        const int band = static_cast<int>(std::lround(g.atlasTopLeft.y / 100.0f));
        REQUIRE(tile >= 0 && tile < scale && band >= 0 && band < scale);
        const std::string where = "band " + std::to_string(band) + " window " + std::to_string(tile) + " lands at "
            + std::to_string(g.originInPixels.x) + ", " + std::to_string(g.sizeInPixels.x) + " wide";
        CHECK_MSG(g.originInPixels.x == std::floor(g.originInPixels.x) && g.sizeInPixels.x == std::floor(g.sizeInPixels.x), where.c_str());
        CHECK_MSG(approx(g.atlasBottomRight.x - g.atlasTopLeft.x, g.sizeInPixels.x), where.c_str());
        if (band == 0)
            leftOfWindow[static_cast<std::size_t>(tile)] = g.originInPixels.x;
        else
            CHECK_MSG(g.originInPixels.x == leftOfWindow[static_cast<std::size_t>(tile)], where.c_str());
        // The windows abut, so the block is the step times the windows wide to a pixel.
        if (tile + 1 < scale && band == 0) {
            const GlyphInstance& next = glyphs[i + 1];
            CHECK_MSG(approx(next.originInPixels.x, g.originInPixels.x + g.sizeInPixels.x), where.c_str());
        }
    }
    CHECK(std::fabs(leftOfWindow[scale - 1] - leftOfWindow[0] - (scale - 1) * 7.6f) <= 0.5f);
}

// The cell-baked path's version: with the subpixel pass off, a sized letter's
// anchor tiles its claim as CellInstances, and the sharp consult replaces each
// tile's slice of the magnified crop with its own window rect, asked for with
// the CellBaked pass. This is the arm the macOS snapshot tool exercises, so a
// regression here is what the sized-text golden would catch later and coarser.
TEST(testASharpSizedLetterBakesOneWindowPerCell)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    const int rows = 3;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x45u; // 'E'
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    // The subpixel pass off is the case: sized glyphs stay on the cell path.
    in.subpixelGlyphs = false;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    b.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass pass, SizedTile tile) {
        // The consult must name the baked flavour: a subpixel rect here would
        // be re-expanded by margins the cell quad never removes.
        if (pass != GlyphPass::CellBaked)
            return AtlasGlyph {};
        return AtlasGlyph { { static_cast<float>(10 + tile.tile), static_cast<float>(10 + tile.band) },
            { static_cast<float>(10 + tile.tile) + 1.0f, static_cast<float>(10 + tile.band) + 1.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    // Nine inked cells, each carrying its own window rect rather than a slice
    // of one master.
    int inked = 0;
    for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i) {
        if (cells[i].atlasBottomRight.x <= cells[i].atlasTopLeft.x)
            continue;
        ++inked;
        const int tile = static_cast<int>(cells[i].atlasTopLeft.x) - 10;
        const int bandRow = static_cast<int>(cells[i].atlasTopLeft.y) - 10;
        const std::string window = "cell at (" + std::to_string(cells[i].gridPosition.x) + ", "
            + std::to_string(cells[i].gridPosition.y) + ") samples window (band "
            + std::to_string(bandRow) + ", tile " + std::to_string(tile) + ")";
        CHECK_MSG(bandRow >= 0 && bandRow < 3 && tile >= 0 && tile < 3, window.c_str());
        CHECK_MSG(approx(cells[i].gridPosition.x, static_cast<float>(tile)), window.c_str());
        CHECK_MSG(approx(cells[i].gridPosition.y, static_cast<float>(bandRow)), window.c_str());
    }
    const std::string count = "a sharp baked scale-3 letter inks "
        + std::to_string(inked) + " cell(s), wanting 9";
    CHECK_MSG(inked == 9, count.c_str());
}

// The cell-baked path's two-cell version, for a backend with no wide stream:
// a sized wide glyph tiles its claim two cells at a time, and each tile takes
// the wide entry's window rect whole, asked for by band and tile. A backend
// that answers stands its sharp windows in for the slices of the magnified
// crop, which is the picture the portable arms drew from the 1x raster.
TEST(testASharpSizedWideGlyphBakesOneTwoCellWindowPerTile)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/0 });
    CHECK(scale3 != 0);

    const int columns = 8;
    const int rows = 3;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * columns;
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = 0x6771u; // 東, width 2
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 6; ++c) { // scale 3 * width 2
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.subpixelGlyphs = false;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    // No wide stream: the glyph stays on its cells.
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // Window (band, tile) answers a two-cell rect at (100 + 16 * tile, 100 + 16 * band).
    b.sizedWideGlyph = [](void*, std::uint32_t codePoint, FontFace, std::uint32_t, SizedTile tile) {
        if (codePoint != 0x6771u || tile.scale != 3 || tile.isFractional())
            return AtlasGlyph {};
        const float left = 100.0f + 16.0f * static_cast<float>(tile.tile);
        const float top = 100.0f + 16.0f * static_cast<float>(tile.band);
        return AtlasGlyph { { left, top }, { left + 16.0f, top + 16.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    int inked = 0;
    for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i) {
        if (cells[i].atlasBottomRight.x <= cells[i].atlasTopLeft.x)
            continue;
        ++inked;
        const float tileFromRect = (cells[i].atlasTopLeft.x - 100.0f) / 16.0f;
        const float bandFromRect = (cells[i].atlasTopLeft.y - 100.0f) / 16.0f;
        const std::string window = "cell at (" + std::to_string(cells[i].gridPosition.x) + ", "
            + std::to_string(cells[i].gridPosition.y) + ") " + std::to_string(cells[i].widthCells)
            + " wide samples (" + std::to_string(cells[i].atlasTopLeft.x) + ", " + std::to_string(cells[i].atlasTopLeft.y)
            + ")-(" + std::to_string(cells[i].atlasBottomRight.x) + ", " + std::to_string(cells[i].atlasBottomRight.y) + ")";
        // The window whole, 1:1, on the tile it was cut for.
        CHECK_MSG(approx(cells[i].widthCells, 2.0f), window.c_str());
        CHECK_MSG(approx(cells[i].gridPosition.x, 2.0f * tileFromRect), window.c_str());
        CHECK_MSG(approx(cells[i].gridPosition.y, bandFromRect), window.c_str());
        CHECK_MSG(approx(cells[i].atlasBottomRight.x - cells[i].atlasTopLeft.x, 16.0f), window.c_str());
        CHECK_MSG(approx(cells[i].atlasBottomRight.y - cells[i].atlasTopLeft.y, 16.0f), window.c_str());
    }
    const std::string count = "a sharp baked scale-3 wide glyph inks "
        + std::to_string(inked) + " cell(s), wanting 9";
    CHECK_MSG(inked == 9, count.c_str());
}

// The wide stream's version: one ordinary two-cell quad per claimed slot, each
// backing off ONE margin — the stretched path multiplies its backoff by the
// slots claimed because its whole quad is magnified, and a sharp window is
// not. The soft shape is pinned by testASizedWideGlyphCoversItsClaimOnTheWideSheet,
// which runs against a backend without the entry.
TEST(testASharpSizedWideGlyphDrawsOneSlotPerWindow)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/0 });
    CHECK(scale3 != 0);

    const std::uint32_t cjk = 0x4E00u; // width 2
    const int columns = 10;
    const int rows = 3;
    const float cellWidth = 8.0f;

    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * columns;
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = cjk;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 6; ++c) { // scale 3 * width 2
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = cellWidth;
    const float margin = 1.0f;
    in.glyphMarginInPixels = { margin, margin };
    in.glyphWideSlotWidthInPixels = 2.0f * (cellWidth + 2.0f * margin);
    in.glyphWideSlotHeightInPixels = 18.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 16);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> wides(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.wideGlyphs = wides.data();
    buffers.wideGlyphCapacity = wides.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // The full-slot rect of window `tile`, slots 20 wide and 18 tall, laid
    // out by tile along a row of band: the wide entry's own answer.
    b.sizedWideGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, SizedTile tile) {
        const float slotLeft = 20.0f * static_cast<float>(tile.tile);
        const float slotTop = 18.0f * static_cast<float>(tile.band);
        return AtlasGlyph { { slotLeft, slotTop }, { slotLeft + 20.0f, slotTop + 18.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    const std::string quads = "a sharp scale-3 wide glyph emits "
        + std::to_string(counts.wideGlyphs) + " wide quad(s), wanting 9";
    CHECK_MSG(counts.wideGlyphs == 9, quads.c_str());
    if (counts.wideGlyphs != 9)
        return;

    for (std::size_t i = 0; i < 9; ++i) {
        const int tile = static_cast<int>(std::lround((wides[i].atlasTopLeft.x + margin) / 20.0f));
        const int bandRow = static_cast<int>(wides[i].atlasTopLeft.y) / 18;
        const std::string window = "quad " + std::to_string(i) + " samples window (band "
            + std::to_string(bandRow) + ", tile " + std::to_string(tile) + ")";
        CHECK_MSG(bandRow >= 0 && bandRow < 3 && tile >= 0 && tile < 3, window.c_str());
        // Each window takes the two cells it steps by, the first keeping the
        // margin before it and the last the margin after: the windows abut
        // and no two paint one pixel.
        const bool isFirst = tile == 0;
        const bool isLast = tile == 2;
        const float wanted = 2.0f * cellWidth + (isFirst ? margin : 0.0f) + (isLast ? margin : 0.0f);
        const std::string shape = "quad " + std::to_string(i) + " at x="
            + std::to_string(wides[i].originInPixels.x) + " is "
            + std::to_string(wides[i].sizeInPixels.x) + "px wide, wanting " + std::to_string(wanted);
        CHECK_MSG(approx(wides[i].sizeInPixels.x, wanted), shape.c_str());
        CHECK_MSG(approx(wides[i].originInPixels.x,
                      2.0f * cellWidth * static_cast<float>(tile) - (isFirst ? margin : 0.0f)),
            shape.c_str());
        CHECK_MSG(approx(wides[i].originInPixels.y, 16.0f * static_cast<float>(bandRow) - (bandRow == 0 ? margin : 0.0f)), shape.c_str());
        if (i >= 3) {
            CHECK_MSG(approx(wides[i - 3].originInPixels.y + wides[i - 3].sizeInPixels.y, wides[i].originInPixels.y),
                "wide glyph bands meet without overpainting");
        }
        const float markerY = 18.0f * static_cast<float>(bandRow) + margin + 4.0f;
        const float screenY = wides[i].originInPixels.y + (markerY - wides[i].atlasTopLeft.y) * wides[i].sizeInPixels.y / (wides[i].atlasBottomRight.y - wides[i].atlasTopLeft.y);
        CHECK(approx(screenY, 16.0f * static_cast<float>(bandRow) + 4.0f));
        // The rect covers the window's cells a margin into its slot and stops
        // short of the slot's boundary.
        const float slotLeft = 20.0f * static_cast<float>(tile);
        CHECK_MSG(approx(wides[i].atlasTopLeft.x, slotLeft + margin - (isFirst ? margin : 0.0f)), "the rect starts at the window's cells");
        CHECK_MSG(approx(wides[i].atlasBottomRight.x, slotLeft + margin + 2.0f * cellWidth + (isLast ? margin : 0.0f)), "and ends at their far edge");
        CHECK_MSG(wides[i].atlasBottomRight.x <= slotLeft + 20.0f - 1.0f, "short of the slot boundary");
    }
}

namespace {

// Every fractional window the wide and colour entries were asked for.
std::vector<SizedTile> fractionalWideRequests;
std::vector<SizedTile> fractionalColourRequests;

// A three-band block of one spec over `columns` cells, the anchor at column 0
// of each band carrying `codePoint` and `attributes`.
std::vector<Ubiquitty::Cell> fractionalBlockGrid(const Ubiquitty::Cell& blank, unsigned specID, std::uint32_t codePoint,
    bool isEmoji, int columns, int claimedColumns)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * 3, blank);
    for (int band = 0; band < 3; ++band) {
        const std::size_t base = static_cast<std::size_t>(band) * static_cast<std::size_t>(columns);
        Ubiquitty::Cell& anchor = grid[base];
        anchor.codePoint = codePoint;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        if (isEmoji)
            stamped.attributes.add(Ubiquitty::VT::Attribute::Emoji);
        stamped.extendedAttributes.setTextSizeSpecID(specID);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < claimedColumns; ++c) {
            Ubiquitty::Cell& continuation = grid[base + static_cast<std::size_t>(c)];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(specID);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }
    return grid;
}

Ubiquitty::TextSizeSpec threeHalvesSpec(std::uint8_t widthCells)
{
    Ubiquitty::TextSizeSpec spec { /*scale=*/3, widthCells };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    return spec;
}

} // namespace

namespace {

// The slot table and convention the soft-band tests hand the mono binding.
std::array<GlyphUV, CacheBackend::monoSlots> softBandSlotTable;
SlotRectConvention softBandSlotConvention = SlotRectConvention::ContractedSlot;
bool hasSoftBandSlotTable = false;

// Slots 10 by 18 atlas pixels with a one pixel margin and a headroom column
// beside the 7 pixel cell box: a slot rect that is neither the cell box nor
// the slot a margin-free geometry makes them equal.
constexpr int softBandSlotWidth = 10;
constexpr int softBandSlotHeight = 18;

MonoAtlasBinding softBandMonoBinding(void* context)
{
    MonoAtlasBinding binding = CacheBackend::monoBinding(*static_cast<CacheBackend*>(context));
    binding.geometry.slotWidthInPixels = softBandSlotWidth;
    binding.geometry.slotHeightInPixels = softBandSlotHeight;
    binding.geometry.glyphWidthInPixels = 7;
    if (hasSoftBandSlotTable) {
        for (int slot = 0; slot < binding.slotCapacity; ++slot) {
            const GlyphUV padded = binding.geometry.atlasRectForSlotFull(slot, binding.slotCapacity);
            GlyphUV entry = padded;
            switch (softBandSlotConvention) {
            case SlotRectConvention::ContractedSlot:
                entry = binding.geometry.atlasRectForBuilder(slot, binding.slotCapacity);
                break;
            case SlotRectConvention::CellBox:
                entry = binding.geometry.atlasRectForSlot(slot, binding.slotCapacity);
                break;
            case SlotRectConvention::PaddedSlot:
                break;
            case SlotRectConvention::MeasuredInk:
                entry = { padded.minU + 2.0f, padded.minV + 3.0f, padded.maxU - 3.0f, padded.maxV - 2.0f };
                break;
            }
            softBandSlotTable[static_cast<std::size_t>(slot)] = entry;
        }
        binding.slotUV = softBandSlotTable.data();
        binding.slotUVConvention = softBandSlotConvention;
    }
    return binding;
}

enum class SoftBandStream {
    Cell,
    Subpixel,
};

enum class SoftBandGlyph {
    Narrow,
    Wide,
};

enum class SoftBandDecline {
    EntryAbsent,
    EntryAnswersNothing,
};

// A scale-3 block of 'E' whose sized windows are declined, so every band falls
// back to the crop of the 1x slot.
struct SoftBandScene {
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    std::vector<Ubiquitty::Cell> grid;
    std::vector<CellInstance> cells = std::vector<CellInstance>(64);
    std::vector<DecorationInstance> decos = std::vector<DecorationInstance>(256);
    std::vector<GlyphInstance> glyphs = std::vector<GlyphInstance>(16);
    std::vector<GlyphInstance> wides = std::vector<GlyphInstance>(16);
    InstanceCounts counts;

    void build(SoftBandStream stream, SoftBandGlyph kind, SoftBandDecline decline)
    {
        const bool isSubpixel = stream == SoftBandStream::Subpixel;
        const bool isWideGlyph = kind == SoftBandGlyph::Wide;
        const unsigned spec = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/static_cast<std::uint8_t>(isWideGlyph ? 0 : 1) });
        grid = fractionalBlockGrid(fx.blank, spec, isWideGlyph ? 0x6771u : 0x45u, /*isEmoji=*/false, 10, isWideGlyph ? 6 : 3);
        BuildInputs in;
        in.cells = grid.data();
        in.rows = 3;
        in.columns = 10;
        in.palette = &fx.pal;
        in.textSizeTable = &table;
        in.subpixelGlyphs = isSubpixel;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphBoxWidthInPixels = 7.0f;
        in.glyphMarginInPixels = { 1.0f, 1.0f };
        in.glyphSlotWidthInPixels = static_cast<float>(softBandSlotWidth);
        in.glyphSlotHeightInPixels = static_cast<float>(softBandSlotHeight);
        in.glyphWideSlotWidthInPixels = 20.0f;
        in.glyphWideSlotHeightInPixels = static_cast<float>(softBandSlotHeight);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        buffers.glyphs = glyphs.data();
        buffers.glyphCapacity = glyphs.size();
        buffers.wideGlyphs = wides.data();
        buffers.wideGlyphCapacity = wides.size();
        CacheBackend backend;
        GlyphBackend b = backend.backend();
        b.bindMonoAtlas = softBandMonoBinding;
        // The wide slot, margins included, 20 by 18 at slot (2, 2).
        b.wideGlyph = [](void*, std::uint32_t, FontFace) { return AtlasGlyph { { 40.0f, 36.0f }, { 60.0f, 54.0f }, true }; };
        const auto answersNothing = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile) { return AtlasGlyph {}; };
        const auto wideAnswersNothing = [](void*, std::uint32_t, FontFace, std::uint32_t, SizedTile) { return AtlasGlyph {}; };
        if (decline == SoftBandDecline::EntryAnswersNothing) {
            b.sizedGlyph = answersNothing;
            b.sizedWideGlyph = wideAnswersNothing;
        }
        counts = buildCellInstances(in, b, buffers);
    }
};

// Where a quad puts the source row `sourceV`, in destination pixels.
float screenRowOf(const GlyphInstance& quad, float sourceV)
{
    return quad.originInPixels.y + (sourceV - quad.atlasTopLeft.y) * quad.sizeInPixels.y / (quad.atlasBottomRight.y - quad.atlasTopLeft.y);
}

float screenColumnOf(const GlyphInstance& quad, float sourceU)
{
    return quad.originInPixels.x + (sourceU - quad.atlasTopLeft.x) * quad.sizeInPixels.x / (quad.atlasBottomRight.x - quad.atlasTopLeft.x);
}

// A band quad draws its own third of the cell box, registered with the bands
// beside it: the box's rows at a third of the way down land on the band's cell
// row, at the block's scale, and the margin rides only the outer bands.
void checkSoftBandQuads(const std::vector<GlyphInstance>& quads, std::size_t count, const char* arm)
{
    const std::string quadCount = std::string(arm) + " draws " + std::to_string(count) + " quad(s), wanting one per band";
    CHECK_MSG(count == 3, quadCount.c_str());
    if (count != 3)
        return;
    const float margin = 1.0f;
    const float boxHeight = static_cast<float>(softBandSlotHeight) - 2.0f * margin;
    const float slice = boxHeight / 3.0f;
    // Slots are cut on the atlas grid, so the first band's top edge is the
    // slot's own: that is the one fact read from the output, and it is checked
    // against the grid.
    const float slotTop = quads[0].atlasTopLeft.y;
    const float slotLeft = quads[0].atlasTopLeft.x;
    const std::string grid = std::string(arm) + " samples a slot on the atlas grid, at ("
        + std::to_string(slotLeft) + ", " + std::to_string(slotTop) + ")";
    CHECK_MSG(approx(std::fmod(slotTop, static_cast<float>(softBandSlotHeight)), 0.0f), grid.c_str());
    for (std::size_t band = 0; band < 3; ++band) {
        const GlyphInstance& quad = quads[band];
        const bool isFirst = band == 0;
        const bool isLast = band == 2;
        const float boxTop = slotTop + margin;
        const float bandTop = boxTop + slice * static_cast<float>(band);
        const float bandBottom = bandTop + slice;
        const std::string where = std::string(arm) + " band " + std::to_string(band) + " samples rows "
            + std::to_string(quad.atlasTopLeft.y) + ".." + std::to_string(quad.atlasBottomRight.y)
            + " into y=" + std::to_string(quad.originInPixels.y) + " height " + std::to_string(quad.sizeInPixels.y);
        CHECK_MSG(approx(quad.atlasTopLeft.y, bandTop - (isFirst ? margin : 0.0f)), where.c_str());
        CHECK_MSG(approx(quad.atlasBottomRight.y, bandBottom + (isLast ? margin : 0.0f)), where.c_str());
        // The band's rows meet its cell row's edges.
        CHECK_MSG(approx(screenRowOf(quad, bandTop), 16.0f * static_cast<float>(band)), where.c_str());
        CHECK_MSG(approx(screenRowOf(quad, bandBottom), 16.0f * static_cast<float>(band + 1)), where.c_str());
        // The cell box's left edge stands at the claim's left edge, however
        // many margins the magnified quad carries.
        CHECK_MSG(approx(screenColumnOf(quad, slotLeft + margin), 0.0f), where.c_str());
        CHECK_MSG(approx(quad.sizeInPixels.x / (quad.atlasBottomRight.x - quad.atlasTopLeft.x), 3.0f), where.c_str());
        // Every band is one row of the block's magnification, none a slot's.
        CHECK_MSG(approx(quad.sizeInPixels.y / (quad.atlasBottomRight.y - quad.atlasTopLeft.y), 3.0f), where.c_str());
    }
}

} // namespace

// A sized letter whose windows the backend declines is drawn from the 1x slot,
// each band cutting its own third of the cell box. The subpixel quad expands
// the rect it receives by the margins, so the crop has to be made on the slot
// that quad covers: cut on the box alone, every band stretched its slice over a
// slot's rows and the bands overlapped and gapped. The slot table's convention
// changes where the backend keeps its rects, and nothing about the picture.
TEST(testASoftSizedLetterOnTheSubpixelStreamCutsEachBandFromTheSlot)
{
    const SlotRectConvention conventions[] = { SlotRectConvention::ContractedSlot, SlotRectConvention::CellBox,
        SlotRectConvention::PaddedSlot, SlotRectConvention::MeasuredInk };
    const SoftBandDecline declines[] = { SoftBandDecline::EntryAbsent, SoftBandDecline::EntryAnswersNothing };
    for (const bool hasTable : { false, true }) {
        for (const SlotRectConvention convention : conventions) {
            if (!hasTable && convention != SlotRectConvention::ContractedSlot)
                continue;
            for (const SoftBandDecline decline : declines) {
                hasSoftBandSlotTable = hasTable;
                softBandSlotConvention = convention;
                SoftBandScene scene;
                scene.build(SoftBandStream::Subpixel, SoftBandGlyph::Narrow, decline);
                const std::string arm = std::string("the subpixel arm (table ") + (hasTable ? "on, convention " + std::to_string(static_cast<int>(convention)) : std::string("off"))
                    + (decline == SoftBandDecline::EntryAbsent ? ", entry absent)" : ", entry answers nothing)");
                checkSoftBandQuads(scene.glyphs, scene.counts.glyphs, arm.c_str());
            }
        }
    }
    hasSoftBandSlotTable = false;
    softBandSlotConvention = SlotRectConvention::ContractedSlot;
}

// The wide sheet hands over the whole slot, margins included, and its quad is
// one row tall whatever the scale: the same bands, cut from that slot.
TEST(testASoftSizedWideGlyphCutsEachBandFromTheSlot)
{
    for (const SoftBandDecline decline : { SoftBandDecline::EntryAbsent, SoftBandDecline::EntryAnswersNothing }) {
        SoftBandScene scene;
        scene.build(SoftBandStream::Subpixel, SoftBandGlyph::Wide, decline);
        // The wide slot's cell box begins a margin in, at (41, 37): the quads
        // sample the slot at (40, 36), a margin out.
        checkSoftBandQuads(scene.wides, scene.counts.wideGlyphs, "the wide arm");
    }
}

// The cell-baked arm takes the same rows of the same cell box the subpixel
// arm does, without the margin its quad has no use for.
TEST(testASoftSizedLetterOnTheCellPathTakesTheSubpixelArmsRows)
{
    SoftBandScene subpixel;
    subpixel.build(SoftBandStream::Subpixel, SoftBandGlyph::Narrow, SoftBandDecline::EntryAbsent);
    SoftBandScene baked;
    baked.build(SoftBandStream::Cell, SoftBandGlyph::Narrow, SoftBandDecline::EntryAbsent);
    CHECK_MSG(subpixel.counts.glyphs == 3, "the subpixel arm draws three bands");
    if (subpixel.counts.glyphs != 3)
        return;
    std::vector<CellInstance> inked;
    for (std::size_t i = 0; i < baked.counts.cells; ++i)
        if (baked.cells[i].atlasBottomRight.x > baked.cells[i].atlasTopLeft.x)
            inked.push_back(baked.cells[i]);
    CHECK_MSG(!inked.empty(), "the cell path draws the block");
    for (const CellInstance& cell : inked) {
        const std::size_t band = static_cast<std::size_t>(cell.gridPosition.y);
        const GlyphInstance& quad = subpixel.glyphs[band];
        const float margin = 1.0f;
        const float firstRow = quad.atlasTopLeft.y + (band == 0 ? margin : 0.0f);
        const float lastRow = quad.atlasBottomRight.y - (band == 2 ? margin : 0.0f);
        const std::string rows = "band " + std::to_string(band) + " on the cell path samples rows "
            + std::to_string(cell.atlasTopLeft.y) + ".." + std::to_string(cell.atlasBottomRight.y)
            + " where the subpixel arm shows " + std::to_string(firstRow) + ".." + std::to_string(lastRow);
        CHECK_MSG(approx(cell.atlasTopLeft.y, firstRow), rows.c_str());
        CHECK_MSG(approx(cell.atlasBottomRight.y, lastRow), rows.c_str());
    }
}

// A two-cell glyph at a fraction is drawn from the wide raster at its drawn
// size, as a narrow one is: s=3 at one half asks for the two-cell windows of
// the glyph at 1.5 times its size, two rows of two, and draws every quad from
// them. The magnified 1x wide slot mapped into the smaller area is a resample.
TEST(testAFractionalWideGlyphDrawsFromWindowsAtItsDrawnSize)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned specID = table.intern(threeHalvesSpec(/*widthCells=*/0));
    CHECK(specID != 0);
    const float cellWidth = 8.0f;
    const float margin = 1.0f;
    std::vector<Ubiquitty::Cell> grid = fractionalBlockGrid(fx.blank, specID, 0x6771u, /*isEmoji=*/false, 8, 6);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 3;
    in.columns = 8;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = cellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = cellWidth;
    in.glyphMarginInPixels = { margin, margin };
    in.glyphWideSlotWidthInPixels = 2.0f * (cellWidth + 2.0f * margin);
    in.glyphWideSlotHeightInPixels = 18.0f;

    std::vector<CellInstance> cells(64);
    std::vector<DecorationInstance> decos(256);
    std::vector<GlyphInstance> wides(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.wideGlyphs = wides.data();
    buffers.wideGlyphCapacity = wides.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // Full-slot rects far from any the fake atlas hands out, 20 by 18.
    b.sizedWideGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, SizedTile tile) {
        fractionalWideRequests.push_back(tile);
        const float slotLeft = 4096.0f + 20.0f * static_cast<float>(fractionalWideRequests.size());
        return AtlasGlyph { { slotLeft, 0.0f }, { slotLeft + 20.0f, 18.0f }, true };
    };
    fractionalWideRequests.clear();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    bool isEveryRequestAtOneAndAHalf = !fractionalWideRequests.empty();
    std::array<bool, 4> isWindowAsked {};
    for (const SizedTile& tile : fractionalWideRequests) {
        isEveryRequestAtOneAndAHalf = isEveryRequestAtOneAndAHalf && tile.isFractional() && std::fabs(tile.rasterScale() - 1.5) < 1e-9
            && tile.band < 2 && tile.tile < 2;
        if (tile.band < 2 && tile.tile < 2)
            isWindowAsked[static_cast<std::size_t>(tile.band * 2 + tile.tile)] = true;
    }
    CHECK_MSG(isEveryRequestAtOneAndAHalf, "every wide window asked for is of the raster at 3 * 1/2, within two rows of two");
    CHECK_MSG(std::ranges::all_of(isWindowAsked, [](bool isAsked) { return isAsked; }), "and all four are asked for");

    const std::size_t drawn = std::min(counts.wideGlyphs, wides.size());
    bool isEveryQuadFromAWindow = drawn > 0;
    float left = 1e9f;
    float top = 1e9f;
    float bottom = -1e9f;
    for (std::size_t i = 0; i < drawn; ++i) {
        isEveryQuadFromAWindow = isEveryQuadFromAWindow && wides[i].atlasTopLeft.x >= 4096.0f;
        left = std::min(left, wides[i].originInPixels.x);
        top = std::min(top, wides[i].originInPixels.y);
        bottom = std::max(bottom, wides[i].originInPixels.y + wides[i].sizeInPixels.y);
    }
    CHECK_MSG(isEveryQuadFromAWindow, "every wide quad samples a window, none the 1x slot");
    // Top-left aligned, the 24 by 24 picture of a 48 by 48 block starts at the
    // block's origin, its first window reaching a margin left; every band
    // draws only into its own row, so nothing reaches above row 0 or into the
    // third band.
    const std::string origin = "the windows start at (" + std::to_string(left) + ", " + std::to_string(top) + "), wanting (-1, 0)";
    CHECK_MSG(approx(left, -margin) && approx(top, 0.0f), origin.c_str());
    CHECK_MSG(bottom <= 32.0f + 0.001f, "and draw nothing into the third band");
}

// A colour cluster at a fraction draws the colour raster at its drawn size: s=3
// at one half is a square one and a half colour squares on a side, cut into
// windows of the source square, the last row and column partial.
TEST(testAFractionalColourClusterDrawsFromWindowsAtItsDrawnSize)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned specID = table.intern(threeHalvesSpec(/*widthCells=*/0));
    CHECK(specID != 0);
    std::vector<Ubiquitty::Cell> grid = fractionalBlockGrid(fx.blank, specID, 0x1F600u, /*isEmoji=*/true, 12, 6);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 3;
    in.columns = 12;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(64);
    std::vector<DecorationInstance> decos(256);
    std::vector<GlyphInstance> colours(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.colorGlyphs = colours.data();
    buffers.colorGlyphCapacity = colours.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // u encodes the tile column and v the band row, offset past any rect the
    // fake atlas hands out; each window is one texel per pixel of a 16 square.
    b.sizedColorGlyph = [](void*, ClusterSpan, SizedTile tile) {
        fractionalColourRequests.push_back(tile);
        const float u = 4096.0f + 16.0f * static_cast<float>(tile.tile);
        const float v = 16.0f * static_cast<float>(tile.band);
        return AtlasGlyph { { u, v }, { u + 16.0f, v + 16.0f }, true };
    };
    fractionalColourRequests.clear();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    bool isEveryRequestAtOneAndAHalf = !fractionalColourRequests.empty();
    for (const SizedTile& tile : fractionalColourRequests)
        isEveryRequestAtOneAndAHalf = isEveryRequestAtOneAndAHalf && tile.isFractional() && std::fabs(tile.rasterScale() - 1.5) < 1e-9
            && tile.band < 2 && tile.tile < 2;
    CHECK_MSG(isEveryRequestAtOneAndAHalf, "every colour window asked for is of the raster at 3 * 1/2, within two rows of two");

    const std::size_t drawn = std::min(counts.colorGlyphs, colours.size());
    CHECK_MSG(drawn == 4, "four windows draw the 24-pixel square");
    float drawnArea = 0.0f;
    for (std::size_t i = 0; i < drawn; ++i) {
        CHECK_MSG(colours[i].atlasTopLeft.x >= 4096.0f, "every colour quad samples a window, none the 1x master");
        // One texel per pixel: a window drawn at its own size, never scaled.
        const float texelsWide = colours[i].atlasBottomRight.x - colours[i].atlasTopLeft.x;
        const float texelsTall = colours[i].atlasBottomRight.y - colours[i].atlasTopLeft.y;
        CHECK_MSG(approx(texelsWide, colours[i].sizeInPixels.x) && approx(texelsTall, colours[i].sizeInPixels.y),
            "each window is drawn texel for pixel");
        drawnArea += colours[i].sizeInPixels.x * colours[i].sizeInPixels.y;
    }
    CHECK_MSG(approx(drawnArea, 24.0f * 24.0f), "and together they cover the 24-pixel square once");
}

namespace {

/// The fake atlas, recording which code point each cell asked for. The mono
/// cache answers the builder's probe, so a fresh cell reaches the atlas as a
/// rasterise call, once per distinct code point, in column order.
struct RecordingAtlas : FakeAtlas {
    std::vector<std::uint32_t> requested;
    bool rasteriseMonoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int)
    {
        requested.push_back(codePoint);
        return true;
    }
};

/// The text behind a run, answered the way the terminal answers it: the
/// anchor's cluster field names the run's whole text and a continuation names
/// nothing (Docs/proportional-text.md). The gate is the terminal's
/// ``isProportionalRunAnchor`` in test form — a sized cell that is not a
/// continuation — with the store replaced by the one string a case sets.
struct RunTextFixture {
    static inline std::vector<std::uint32_t> text;
    static int resolve(const Ubiquitty::Cell* cell, std::span<std::uint32_t> out, void*)
    {
        if (!cell)
            return 0;
        const Ubiquitty::Pen& pen = Ubiquitty::cellPen(*cell);
        if (pen.extendedAttributes.textSizeSpecID() == 0 || pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
            return 0;
        const std::size_t count = std::min(text.size(), out.size());
        for (std::size_t i = 0; i < count; ++i)
            out[i] = text[i];
        return static_cast<int>(count);
    }
    static void setLetters(std::uint32_t first, std::size_t count)
    {
        text.clear();
        for (std::size_t i = 0; i < count; ++i)
            text.push_back(first + static_cast<std::uint32_t>(i));
    }
};

} // namespace

/// The grid-face tier draws a run one code point per cell, its text read whole
/// from the anchor: cell k draws code point k however far past sixteen k
/// lies. A tier that read a run through the cluster pool's sixteen-code-point
/// entry drew the first sixteen and left the rest blank, so cell sixteen must
/// draw the seventeenth letter.
TEST(testTheGridFaceTierDrawsARunLongerThanAClusterEntry)
{
    const int rows = 1, columns = 20;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 20;
    const unsigned id = table.intern(spec);
    CHECK(id != 0);

    auto stamp = [&](std::vector<Ubiquitty::Cell>& grid, int column, std::uint32_t codePoint, bool isAnchor, std::uint16_t clusterID) {
        setCell(grid, columns, 0, column, codePoint,
            isAnchor ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(column)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
        cell.setClusterID(clusterID);
    };
    auto build = [&](std::vector<Ubiquitty::Cell>& grid, RecordingAtlas& atlas) {
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = &table;
        in.clusterResolver = &RunTextFixture::resolve;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns);
        std::vector<DecorationInstance> decorations(cells.size() * 8);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    };

    // Twenty letters over twenty cells, the anchor naming all twenty, so cell
    // one draws B and cell sixteen draws Q — the letter a sixteen-code-point
    // entry could not have carried.
    {
        RunTextFixture::setLetters('A', 20);
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
        stamp(grid, 0, 'A', true, 1);
        for (int column = 1; column < columns; ++column)
            stamp(grid, column, wideContinuationSentinelCodePoint, false, 0);
        RecordingAtlas atlas;
        build(grid, atlas);
        CHECK_MSG(atlas.requested.size() == 20, "every cell of the run asked for a glyph");
        CHECK_MSG(atlas.requested.size() >= 17 && atlas.requested[16] == 'Q', "cell sixteen draws the seventeenth letter");
        CHECK_MSG(atlas.requested.size() == 20 && atlas.requested[19] == 'T', "and the last cell the last one");
        CHECK_MSG(atlas.requested.size() >= 2 && atlas.requested[0] == 'A' && atlas.requested[1] == 'B', "the anchor's text leads");
    }
    // The control: the same run whose anchor names only sixteen letters draws
    // sixteen and nothing after, so the case above passes on the anchor's
    // text and not on a tier that draws every column.
    {
        RunTextFixture::setLetters('A', 16);
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
        stamp(grid, 0, 'A', true, 1);
        for (int column = 1; column < columns; ++column)
            stamp(grid, column, wideContinuationSentinelCodePoint, false, 0);
        RecordingAtlas atlas;
        build(grid, atlas);
        bool hasQ = false;
        for (const std::uint32_t codePoint : atlas.requested)
            hasQ = hasQ || codePoint == 'Q';
        CHECK_MSG(!hasQ, "a sixteen-letter run draws no seventeenth letter");
    }
}

// The slot is cut for the widest glyph the face overhangs to, so under a
// symbol fallback it is three cells wide while a window is one. A window's
// quad must be its cell and not its slot: slot-wide quads repainted each
// other's ink and sampled the next slot's edge column, which drew a dashed
// hairline through every scaled letter and one two cells past the block.
TEST(testASharpSizedWindowTakesItsCellNotItsSlot)
{
    CellFixture fx;
    TermPalette& pal = fx.pal;
    const Ubiquitty::Cell& blank = fx.blank;

    Ubiquitty::TextSizeTable table;
    const unsigned scale3 = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    CHECK(scale3 != 0);

    const int columns = 6;
    const int rows = 3;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell& anchor = grid[static_cast<std::size_t>(band) * columns];
        anchor.codePoint = 0x45u;
        Ubiquitty::Pen stamped = anchor.pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.extendedAttributes.setTextSizeSpecID(scale3);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        for (int c = 1; c < 3; ++c) {
            Ubiquitty::Cell& continuation = grid[static_cast<std::size_t>(band) * columns + c];
            Ubiquitty::Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            continuationPen.extendedAttributes.setTextSizeSpecID(scale3);
            Ubiquitty::penSetTextBand(&continuationPen, static_cast<uint8_t>(band));
            Ubiquitty::cellSetPen(continuation, continuationPen);
        }
    }

    // An 8px advance in a 27px slot with 2px margins: the overhang slot of a
    // fallback face, three cells and a headroom column wide.
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.subpixelGlyphs = true;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 27.0f;
    in.glyphSlotHeightInPixels = 20.0f;
    in.glyphMarginInPixels = { 2.0f, 2.0f };

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * rows + 8);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * rows * 8);
    std::vector<GlyphInstance> glyphs(32);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    // The cell-box rect of window `tile` in a slot 27 wide: the box starts a
    // margin in, at 27 * tile + 2.
    b.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
        const float slotLeft = 27.0f * static_cast<float>(tile.tile);
        const float slotTop = 20.0f * static_cast<float>(tile.band);
        return AtlasGlyph { { slotLeft + 2.0f, slotTop + 2.0f }, { slotLeft + 25.0f, slotTop + 18.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, b, buffers);
    CHECK_MSG(counts.glyphs == 9, "three bands of three windows");
    if (counts.glyphs != 9)
        return;
    for (std::size_t i = 0; i < 9; ++i) {
        const GlyphInstance& gi = glyphs[i];
        const int tile = static_cast<int>(gi.atlasTopLeft.x + 2.0f) / 27;
        const float slotLeft = 27.0f * static_cast<float>(tile);
        const bool isFirst = tile == 0;
        const bool isLast = tile == 2;
        const float wanted = 8.0f + (isFirst ? 2.0f : 0.0f) + (isLast ? 2.0f : 0.0f);
        const std::string width = "window " + std::to_string(tile) + " quad is " + std::to_string(gi.sizeInPixels.x)
            + "px wide, wanting its cell plus the margin it keeps (" + std::to_string(wanted) + ")";
        CHECK_MSG(approx(gi.sizeInPixels.x, wanted), width.c_str());
        // The quad begins at its cell, a margin earlier only for the first.
        const float wantedOrigin = 8.0f * static_cast<float>(tile) - (isFirst ? 2.0f : 0.0f);
        CHECK_MSG(approx(gi.originInPixels.x, wantedOrigin), "the quad sits on its own cell");
        // The rect covers the window's cell inside its slot and never reaches
        // the slot's boundary: the right edge stays at least a texel short.
        CHECK_MSG(approx(gi.atlasTopLeft.x, slotLeft + 2.0f - (isFirst ? 2.0f : 0.0f)), "the rect starts at the window's cell");
        CHECK_MSG(approx(gi.atlasBottomRight.x, slotLeft + 2.0f + 8.0f + (isLast ? 2.0f : 0.0f)), "and ends at its cell's far edge");
        CHECK_MSG(gi.atlasBottomRight.x <= slotLeft + 27.0f - 1.0f, "short of the slot boundary");
        const int band = static_cast<int>(i / 3);
        if (band > 0) {
            CHECK_MSG(approx(glyphs[i - 3].originInPixels.y + glyphs[i - 3].sizeInPixels.y, gi.originInPixels.y),
                "adjacent bands do not repaint each other's ink");
        }
        const float markerY = 20.0f * static_cast<float>(band) + 6.0f;
        const float screenY = gi.originInPixels.y + (markerY - gi.atlasTopLeft.y) * gi.sizeInPixels.y / (gi.atlasBottomRight.y - gi.atlasTopLeft.y);
        CHECK_MSG(approx(screenY, 16.0f * static_cast<float>(band) + 4.0f),
            "clipping a band preserves its texel-to-pixel mapping");
    }
}

namespace {

/// A backend with the per-glyph proportional route: shapes every code point
/// at a fixed advance and answers each glyph a wide slot, recording what it
/// was asked so a test can see the phase each glyph was drawn at.
struct GlyphRouteAtlas : FakeAtlas {
    struct Ask {
        std::uint32_t glyph;
        std::uint32_t phase;
    };
    std::vector<Ask> asked;
    GlyphSlotCache proportionalCache { 16, recommendedHashBuckets(16), 4 };
    int shaped = 0;
    int clusters = 0;
    std::uint32_t rejectedGlyph = 0;
    bool shouldShapeMissingGlyph = false;
    bool shouldReturnOversizedShape = false;
    bool hasOverlappingInk = false;
    bool omitsInkMeasurement = false;
    int proportionalSlotWidth = 18;
    int proportionalSlotHeight = 20;
    /// Where each glyph's ink ends in its slot, in pixels.
    int inkRight = 6;

    GlyphRouteAtlas()
    {
        proportionalCache.enableOwnedInkRecords(1, proportionalSlotWidth);
    }

    void beginFrame()
    {
        FakeAtlas::beginFrame();
        proportionalCache.beginFrame();
    }

    static ProportionalAtlasBinding binding(void* context)
    {
        auto* self = static_cast<GlyphRouteAtlas*>(context);
        return { &self->proportionalCache, 4, 4,
            self->proportionalSlotWidth, self->proportionalSlotHeight,
            16, { 1, 2 }, 8 };
    }
    /// The anchor carries a two-code-point cluster, and on a row the model
    /// did not flag it draws as the plain cell it then is: a cluster, which
    /// this fixture answers rather than refuses, and counts.
    AtlasGlyph clusterGlyph(std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t)
    {
        ++clusters;
        return AtlasGlyph { { 0.1f, 0.1f }, { 0.2f, 0.2f }, true };
    }
    static int shape(void* context, ClusterSpan codePoints, FontFace, std::uint32_t, ProportionalShapedGlyph* out, int capacity)
    {
        auto* self = static_cast<GlyphRouteAtlas*>(context);
        const int count = static_cast<int>(codePoints.size());
        if (count > capacity)
            return 0;
        for (int i = 0; i < count; ++i) {
            out[i] = ProportionalShapedGlyph {};
            out[i].glyph = self->shouldShapeMissingGlyph && i == 1
                ? 0u
                : 200u + static_cast<std::uint32_t>(i);
            out[i].advanceInPixels = 6.25f;
            out[i].offsetYInPixels = i == 1 ? 1.5f : 0.0f;
            out[i].cluster = static_cast<std::uint16_t>(i);
        }
        ++self->shaped;
        return self->shouldReturnOversizedShape ? capacity + 1 : count;
    }
    static bool rasterise(void* context, std::uint32_t glyphID, FontFace,
        std::uint32_t, std::uint32_t variant, int slot)
    {
        auto* self = static_cast<GlyphRouteAtlas*>(context);
        self->asked.push_back({ glyphID, Ubiquitty::Rendering::variantPhase(variant) });
        if (!self->omitsInkMeasurement) {
            self->proportionalCache.beginOwnedInkRecord(slot);
            self->proportionalCache.recordShapedGlyphInk(slot, 0, 0, 1,
                1, self->hasOverlappingInk ? 9 : self->inkRight, 0);
        }
        return glyphID != self->rejectedGlyph;
    }

    struct SizedAsk {
        std::uint32_t glyph;
        int window;
        int scale;
        int band;
        int widthScale;
        int slot;
    };
    std::vector<SizedAsk> sizedAsked;
    static bool rasteriseSized(void* context, std::uint32_t glyphID, FontFace,
        std::uint32_t, std::uint32_t, SizedTile tile, int slot)
    {
        static_cast<GlyphRouteAtlas*>(context)->sizedAsked.push_back({ glyphID, tile.tile, tile.scale, tile.band, tile.widthScale, slot });
        return true;
    }

    std::vector<SizedTile> stripSizesAsked;
    /// A strip drawn at the size it is asked for: the fixture's 32 by 20
    /// strip, standing `band` slot heights down from the block's top.
    static ProportionalStripPlacement stripAtSize(void* context, ClusterSpan, int,
        std::uint8_t, float, FontFace, std::uint32_t, std::uint32_t, int, SizedTile size)
    {
        static_cast<GlyphRouteAtlas*>(context)->stripSizesAsked.push_back(size);
        ProportionalStripPlacement placement;
        placement.strip = AtlasGlyph { { 40.0f, 20.0f }, { 72.0f, 40.0f }, true };
        placement.widthInPixels = 32.0f;
        placement.topInPixels = static_cast<float>(size.band) * 20.0f;
        placement.drawnWidthScale = static_cast<float>(size.scale * size.widthScale);
        placement.drawnScale = static_cast<float>(size.scale);
        return placement;
    }

    static ProportionalStripPlacement strip(void*, ClusterSpan, int,
        std::uint8_t, float, FontFace, std::uint32_t, std::uint32_t, int, SizedTile)
    {
        ProportionalStripPlacement placement;
        placement.strip = AtlasGlyph { { 40.0f, 20.0f }, { 72.0f, 40.0f }, true };
        placement.widthInPixels = 32.0f;
        return placement;
    }
};

} // namespace

/// An underlined run drawn glyph by glyph cuts its rule per cell, each cell
/// tapping the glyph that inks it most in the dedicated proportional sheet.
///
/// The run of the case below, underlined, with skip-ink on: four letters at
/// 6.25 px whose ink (columns 1–6 of an 18 px slot placed at 7, 13, 19, 25)
/// lies at 8–14, 14–20, 20–26, 26–32 on screen. Cell 1 (8–16) is inked most
/// by the first glyph, cell 2 (16–24) equally by the second and third — the
/// earlier wins a tie — cell 3 (24–32) by the fourth, and cell 4 (32–40) by
/// none, so its rule stays solid except for what its left neighbour reaches.
/// Each cell's tap maps the cell's own pitch onto that glyph's slot, in the
/// fourth sheet, so a rule reads the coverage of the glyph beneath it.
TEST(testAnUnderlinedPerGlyphRunCutsItsRulePerCellOnTheProportionalSheet)
{
    const int rows = 1, columns = 8;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::Underline }, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, { Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Underline }, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    // The run's four letters, named whole by the anchor — whose cluster field
    // carries the store id a real anchor carries.
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.skipInk = true;
    in.metrics.skipInkPadding = 1.0f;
    in.metrics.underlineThickness = 1.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    std::vector<GlyphInstance> strips(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    buffers.proportionalStrips = strips.data();
    buffers.proportionalStripCapacity = strips.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);
    CHECK_MSG(counts.proportionalGlyphs == 4, "the run draws glyph by glyph");
    CHECK_MSG(counts.proportionalStrips == 0, "and no strip");

    // One rule per cell of the claim, in column order.
    std::vector<const DecorationInstance*> rules;
    for (const DecorationInstance& d : std::span(decorations.data(), std::min(counts.decorations, decorations.size())))
        if (d.topLeftInPixels.x >= 8.0f && d.topLeftInPixels.x < 40.0f)
            rules.push_back(&d);
    CHECK_MSG(rules.size() == 4, "the claim's rule is emitted one cell at a time");
    if (rules.size() != 4)
        return;
    constexpr int proportionalSheet = 3;
    // The text is 25 px from the claim's left, so the last segment covers the
    // one pixel of cell 4 the text reaches.
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK_MSG(approx(rules[c]->topLeftInPixels.x, 8.0f + 8.0f * c), "at its cell's left edge");
        CHECK_MSG(approx(rules[c]->bottomRightInPixels.x - rules[c]->topLeftInPixels.x, c < 3 ? 8.0f : 1.0f), "as wide as the text in its cell");
        CHECK_MSG(rules[c]->skipInk > 0.5f, "cut by ink");
    }
    const auto selectors = [](const DecorationInstance& d) { return static_cast<int>(d.pad1.y + 0.5f); };
    // Cells 1–3 tap the proportional sheet themselves; cell 4 has no ink of its
    // own and taps only its left neighbour there.
    CHECK_MSG(selectors(*rules[0]) % 8 == proportionalSheet, "cell 1 taps the proportional sheet");
    CHECK_MSG(selectors(*rules[1]) % 8 == proportionalSheet, "cell 2 taps the proportional sheet");
    CHECK_MSG(selectors(*rules[2]) % 8 == proportionalSheet, "cell 3 taps the proportional sheet");
    CHECK_MSG(selectors(*rules[3]) % 8 == 0 && (selectors(*rules[3]) / 8) % 8 == proportionalSheet, "cell 4 has no glyph beneath it and reads its left neighbour's");
    CHECK_MSG((selectors(*rules[0]) / 64) % 8 == proportionalSheet, "cell 1's right neighbour is cell 2's glyph");
    CHECK_MSG((selectors(*rules[1]) / 8) % 8 == proportionalSheet && (selectors(*rules[1]) / 64) % 8 == proportionalSheet, "cell 2's neighbours are both on the sheet");
    // The tap maps the cell's pitch onto the glyph's slot: slot 0 begins at
    // atlas x 0 and its quad at screen x 7, so cell 1 (screen 8–16) reads
    // atlas 1–9; cell 3 reads the fourth glyph's slot (x 54, quad at 25) at
    // 53–61 — the cell's own pixels, one slot over from the glyph's.
    CHECK_MSG(approx(rules[0]->inkTopLeft.x, 1.0f) && approx(rules[0]->inkBottomRight.x, 9.0f), "cell 1's tap spans its pitch in the first glyph's slot");
    CHECK_MSG(approx(rules[2]->inkTopLeft.x, 53.0f) && approx(rules[2]->inkBottomRight.x, 61.0f), "cell 3's tap spans its pitch in the fourth glyph's slot");
    CHECK_MSG(approx(rules[0]->glyphSlotTopLeftUV.x, 0.0f) && approx(rules[0]->glyphSlotBottomRightUV.x, 18.0f), "clamped to the glyph's own slot");
}

namespace {

/// Whether a rule is cut where glyph ink crosses it.
enum class InkCut : std::uint8_t {
    Solid,
    CutByInk,
};

/// The rules a proportional run of four 6.25 px letters, claiming cells 1–4
/// of 8 px cells, draws at one alignment with the given decorations.
std::vector<DecorationInstance> proportionalRunRules(std::uint8_t horizontalAlign,
    Ubiquitty::VT::AttributeSet attributes, std::uint8_t underlineStyle, InkCut inkCut)
{
    const int rows = 1, columns = 8;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    spec.horizontalAlign = horizontalAlign;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', attributes | Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, attributes | Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        pen.underlineStyle = underlineStyle;
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.xHeight = 6.0f;
    in.metrics.skipInk = inkCut == InkCut::CutByInk;
    in.metrics.skipInkPadding = 1.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);
    CHECK_EQ(counts.proportionalGlyphs, 4u);
    decorations.resize(std::min(counts.decorations, decorations.size()));
    return decorations;
}

} // namespace

/// A proportional run's rules lie under its text, not across its claim.
///
/// Four letters at 6.25 px take 25 px of a 32 px claim at x 8–40: right
/// aligned, the pen stands 7 px in, so the text runs 15–40; centred, 3.5 px
/// in, 11.5–36.5. Every solid rule — the single and double underline, the
/// strikethrough, and the overline — spans exactly that. A patterned rule
/// keeps its phase on the cells' pitch, so it starts at the left edge of the
/// cell the pen stands in and ends where the text does.
TEST(testAProportionalRunsRulesSpanItsTextNotItsClaim)
{
    struct Alignment {
        std::uint8_t horizontalAlign;
        float textLeft;
        float textRight;
    };
    // Right aligned, then centred.
    constexpr Alignment alignments[] = {
        { 1, 15.0f, 40.0f },
        { 2, 11.5f, 36.5f },
    };
    constexpr float tolerance = 0.01f;
    for (const Alignment& alignment : alignments) {
        const auto solid = proportionalRunRules(alignment.horizontalAlign,
            { Ubiquitty::VT::Attribute::Underline, Ubiquitty::VT::Attribute::Strike, Ubiquitty::VT::Attribute::Overline },
            Ubiquitty::VT::underlineSingle, InkCut::Solid);
        CHECK_EQ(solid.size(), 3u);
        for (const DecorationInstance& rule : solid) {
            CHECK_NEAR(rule.topLeftInPixels.x, alignment.textLeft, tolerance);
            CHECK_NEAR(rule.bottomRightInPixels.x, alignment.textRight, tolerance);
        }

        const auto doubled = proportionalRunRules(alignment.horizontalAlign,
            Ubiquitty::VT::Attribute::Underline, Ubiquitty::VT::underlineDouble, InkCut::Solid);
        CHECK_EQ(doubled.size(), 2u);
        for (const DecorationInstance& rule : doubled) {
            CHECK_NEAR(rule.topLeftInPixels.x, alignment.textLeft, tolerance);
            CHECK_NEAR(rule.bottomRightInPixels.x, alignment.textRight, tolerance);
        }

        // A patterned rule starts at the left edge of the cell the pen stands
        // in, the claim's first here, and ends where the text does.
        for (const std::uint8_t style : { Ubiquitty::VT::underlineCurly, Ubiquitty::VT::underlineDotted, Ubiquitty::VT::underlineDashed }) {
            const auto patterned = proportionalRunRules(alignment.horizontalAlign, Ubiquitty::VT::Attribute::Underline, style, InkCut::Solid);
            CHECK_EQ(patterned.size(), 1u);
            if (patterned.size() != 1)
                continue;
            CHECK_NEAR(patterned[0].topLeftInPixels.x, 8.0f, tolerance);
            CHECK_NEAR(patterned[0].bottomRightInPixels.x, alignment.textRight, tolerance);
        }

        // Cut by ink, the rule is emitted a cell at a time over the cells the
        // text crosses, and the segments together span the text with no gap
        // and no overlap.
        const auto cut = proportionalRunRules(alignment.horizontalAlign, Ubiquitty::VT::Attribute::Underline, Ubiquitty::VT::underlineSingle, InkCut::CutByInk);
        CHECK_MSG(!cut.empty(), "a rule cut by ink");
        if (cut.empty())
            continue;
        float left = cut.front().topLeftInPixels.x;
        float right = cut.front().bottomRightInPixels.x;
        float covered = 0.0f;
        for (const DecorationInstance& segment : cut) {
            left = std::min(left, segment.topLeftInPixels.x);
            right = std::max(right, segment.bottomRightInPixels.x);
            covered += segment.bottomRightInPixels.x - segment.topLeftInPixels.x;
            // A segment samples its own part of its cell's tap: as many atlas
            // pixels as screen pixels.
            CHECK_NEAR(segment.inkBottomRight.x - segment.inkTopLeft.x, segment.bottomRightInPixels.x - segment.topLeftInPixels.x, tolerance);
        }
        CHECK_NEAR(left, alignment.textLeft, tolerance);
        CHECK_NEAR(right, alignment.textRight, tolerance);
        CHECK_NEAR(covered, alignment.textRight - alignment.textLeft, tolerance);
    }
}

/// A proportional anchor with the per-glyph route draws one wide quad per
/// glyph at the whole pixel of its own origin and the phase of its fraction,
/// and asks for no strip. Cell 1 is a run of four letters at 6.25 px in 8 px
/// cells: origins 8, 14.25, 20.5, 26.75 — phases 0, 2, 4, 6 of eight.
// A proportional run on a DECDWL row doubles with the row: it starts at its
// anchor's doubled column, every glyph stands twice as far along as the layout
// put it, and every quad is twice as wide, so the run spans twice the cells it
// claims. Laid out at its own width at the bare column, it drew over the
// doubled text before it and half the width of the row around it.
TEST(testAProportionalRunOnADoubledRowDoublesWithTheRow)
{
    const int rows = 1, columns = 16;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };
    const std::uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.lineAttributes = lineAttributes;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    std::vector<GlyphInstance> strips(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    buffers.proportionalStrips = strips.data();
    buffers.proportionalStripCapacity = strips.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    REQUIRE_MSG(counts.proportionalGlyphs == 4, "one quad per glyph");
    // Laid out at its own width, the four origins stand 0, 6.25, 12.5 and
    // 18.75 pixels along — the phases the undoubled run draws at. Doubled from
    // the anchor's doubled column 2, each stands at 16 plus twice that, placed
    // at its whole pixel less a doubled margin, and the claim of four cells
    // ends at 16 + 4 * 2 * 8.
    const float runStart = 2.0f * 8.0f;
    const float claimRightX = runStart + 4.0f * 2.0f * 8.0f;
    const float layoutOffset[4] = { 0.0f, 6.25f, 12.5f, 18.75f };
    for (std::size_t i = 0; i < 4; ++i) {
        const float quadLeft = std::floor(runStart + 2.0f * layoutOffset[i]) - 2.0f * 1.0f;
        CHECK_MSG(approx(proportionalGlyphs[i].originInPixels.x, quadLeft), "each glyph twice as far along, from the doubled column");
        CHECK_MSG(approx(proportionalGlyphs[i].sizeInPixels.x, std::min(2.0f * 18.0f, claimRightX - quadLeft)),
            "each quad twice the slot wide, cut where the doubled claim ends");
    }
}

// A selection drawn behind the text follows the cells of a proportional run
// rather than its anchor: the anchor's claim keeps its own background, and
// the selected cells of the claim take the selection colour as fills of
// their own, drawn after the claim and under the run's glyphs. Baked on the
// anchor alone, a selection ending inside the run showed nothing until it
// reached the run's first cell and then filled the whole claim.
TEST(testASelectionBehindTheTextFollowsTheCellsOfAProportionalRun)
{
    const int rows = 1, columns = 16;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    setCell(grid, columns, 0, 5, 'z', {}, colorDefault, colorDefault);
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    // Cells 2 and 3 of the run: neither its first cell nor its last.
    const SelectionRowSpan spans[] = { { 0, 2, 3 } };
    in.selectionSpans = spans;
    in.selectionSpanCount = 1;
    in.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };
    in.isSelectionBehindText = true;

    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    std::vector<GlyphInstance> strips(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    buffers.proportionalStrips = strips.data();
    buffers.proportionalStripCapacity = strips.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    const auto isSelectionColour = [&](const CellInstance& inst) {
        return approx(inst.background.x, 0.70f) && approx(inst.background.y, 0.84f) && approx(inst.background.z, 1.0f);
    };
    int selectedFillsAt[16] = {};
    int anchorInstances = 0;
    for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i) {
        const CellInstance& inst = cells[i];
        if (inst.gridPosition.y != 0.0f)
            continue;
        const int x = static_cast<int>(inst.gridPosition.x);
        if (x == 1)
            ++anchorInstances;
        if (isSelectionColour(inst) && x >= 0 && x < 16)
            selectedFillsAt[x] += static_cast<int>(inst.widthCells);
    }
    CHECK_MSG(anchorInstances == 1, "the run's first cell is emitted once");
    CHECK_MSG(selectedFillsAt[1] == 0, "the run's first cell is outside the selection and takes no fill");
    CHECK_MSG(selectedFillsAt[2] == 1 && selectedFillsAt[3] == 1, "without a pixel-rect stream, each selected cell of the run takes a fill of its own");
    CHECK_MSG(selectedFillsAt[4] == 0, "the run's last cell is outside the selection and takes no fill");
    CHECK_MSG(selectedFillsAt[5] == 0, "the plain cell beside the run is outside it too");
    CHECK_MSG(counts.proportionalGlyphs == 4, "the run's glyphs still draw, on their own pass over the fills");

    // With the pixel-rect stream, the fill is the selected letters' own
    // extent. Four 6.25 px letters in 8 px cells begin at 0, 6.25, 12.5 and
    // 18.75 from the run's first cell at column 1: a and b are the first
    // cell's, c the second's and d the third's, so the selected columns 2 and
    // 3 own c and d, and the fill runs from c's origin to the text's end —
    // 8 + 12.5 to 8 + 25 — not over the two cells' 16 to 32.
    std::vector<CellInstance> cellsWithRects(cells.size());
    std::vector<OverlayInstance> boxFills(8);
    InstanceBuffers buffersWithRects { cellsWithRects.data(), cellsWithRects.size(), decorations.data(), decorations.size() };
    buffersWithRects.proportionalGlyphs = proportionalGlyphs.data();
    buffersWithRects.proportionalGlyphCapacity = proportionalGlyphs.size();
    buffersWithRects.proportionalStrips = strips.data();
    buffersWithRects.proportionalStripCapacity = strips.size();
    buffersWithRects.boxFills = boxFills.data();
    buffersWithRects.boxFillCapacity = boxFills.size();
    GlyphRouteAtlas rectAtlas;
    GlyphBackend rectBackend = UbiquittyTest::glyphBackendFor(rectAtlas);
    rectBackend.shapeProportionalRun = &GlyphRouteAtlas::shape;
    rectBackend.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    rectBackend.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    rectBackend.context = &rectAtlas;
    rectBackend.proportionalStrip = nullptr;
    const InstanceCounts rectCounts = buildCellInstances(in, rectBackend, buffersWithRects);
    int colouredCells = 0;
    for (std::size_t i = 0; i < rectCounts.cells && i < cellsWithRects.size(); ++i)
        colouredCells += isSelectionColour(cellsWithRects[i]) ? 1 : 0;
    CHECK_MSG(colouredCells == 0, "with the pixel-rect stream no cell of the run carries the fill");
    REQUIRE_MSG(rectCounts.boxFills == 1, "one rect fills the selected letters");
    const OverlayInstance& fill = boxFills[0];
    CHECK_MSG(approx(fill.topLeftInPixels.x, std::round(8.0f + 12.5f)), "the fill starts at the first selected letter's origin");
    CHECK_MSG(approx(fill.bottomRightInPixels.x, std::round(8.0f + 25.0f)), "and ends at the text's end");
    CHECK_MSG(approx(fill.topLeftInPixels.y, 0.0f) && approx(fill.bottomRightInPixels.y, 16.0f), "over the whole row");
    CHECK_MSG(approx(fill.color.x, 0.70f) && approx(fill.color.y, 0.84f) && approx(fill.color.z, 1.0f), "in the selection colour");
    CHECK_MSG(approx(fill.color.w, 1.0f), "drawn whole over the run's tiles");

    // A translucent fill is the same wash a baked cell takes, mixed from the
    // run's background on the way in rather than blended on the target: the
    // plate under a run and the cells beside it are one colour.
    in.selectionColor = { 0.70f, 0.84f, 1.0f, 0.5f };
    const InstanceCounts washedCounts = buildCellInstances(in, rectBackend, buffersWithRects);
    REQUIRE(washedCounts.boxFills == 1);
    const Vec4 runBackground = cellsWithRects[0].background;
    CHECK_MSG(approx(boxFills[0].color.x, (runBackground.x + 0.70f) / 2.0f) && approx(boxFills[0].color.z, (runBackground.z + 1.0f) / 2.0f),
        "the plate is halfway from the run's background to the selection colour");
    CHECK_MSG(approx(boxFills[0].color.w, 1.0f), "and drawn whole");
    in.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };

    // A claim wider than its text has empty cells after the last letter, and a
    // selection running through them is continuous: past the text's end the
    // fill covers the selected cells of the claim to its edge. The same run
    // in a six-cell claim: columns 1..6, the text ending at 8 + 25 inside it.
    Ubiquitty::TextSizeSpec wideSpec {};
    wideSpec.isProportional = 1;
    wideSpec.runWidthCells = 6;
    const unsigned wideID = table.intern(wideSpec);
    std::vector<Ubiquitty::Cell> wideGrid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(wideGrid, columns, 0, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 7; ++c)
        setCell(wideGrid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 7; ++c) {
        Ubiquitty::Cell& cell = wideGrid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(wideID);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    wideGrid[1].setClusterID(1);
    BuildInputs wide = in;
    wide.cells = wideGrid.data();
    // From the d (column 3) through the claim's empty tail and past it.
    const SelectionRowSpan pastTheText[] = { { 0, 3, 9 } };
    wide.selectionSpans = pastTheText;
    std::vector<CellInstance> wideCells(cells.size());
    std::vector<OverlayInstance> wideFills(8);
    InstanceBuffers wideBuffers { wideCells.data(), wideCells.size(), decorations.data(), decorations.size() };
    wideBuffers.proportionalGlyphs = proportionalGlyphs.data();
    wideBuffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    wideBuffers.proportionalStrips = strips.data();
    wideBuffers.proportionalStripCapacity = strips.size();
    wideBuffers.boxFills = wideFills.data();
    wideBuffers.boxFillCapacity = wideFills.size();
    GlyphRouteAtlas wideAtlas;
    GlyphBackend wideBackend = UbiquittyTest::glyphBackendFor(wideAtlas);
    wideBackend.shapeProportionalRun = &GlyphRouteAtlas::shape;
    wideBackend.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    wideBackend.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    wideBackend.context = &wideAtlas;
    wideBackend.proportionalStrip = nullptr;
    const InstanceCounts wideCounts = buildCellInstances(wide, wideBackend, wideBuffers);
    REQUIRE_MSG(wideCounts.boxFills == 1, "one rect fills the run's selected part");
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, std::round(8.0f + 18.75f)), "from the d's origin");
    CHECK_MSG(approx(wideFills[0].bottomRightInPixels.x, 8.0f + 6.0f * 8.0f), "to the claim's edge, through the empty tail");

    // Selected only in the tail, after every letter: the tail's cells fill.
    const SelectionRowSpan tailOnly[] = { { 0, 5, 6 } };
    wide.selectionSpans = tailOnly;
    GlyphRouteAtlas tailAtlas;
    GlyphBackend tailBackend = UbiquittyTest::glyphBackendFor(tailAtlas);
    tailBackend.shapeProportionalRun = &GlyphRouteAtlas::shape;
    tailBackend.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    tailBackend.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    tailBackend.context = &tailAtlas;
    tailBackend.proportionalStrip = nullptr;
    const InstanceCounts tailCounts = buildCellInstances(wide, tailBackend, wideBuffers);
    REQUIRE_MSG(tailCounts.boxFills == 1, "one rect fills the selected tail");
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, 8.0f + 4.0f * 8.0f) && approx(wideFills[0].bottomRightInPixels.x, 8.0f + 6.0f * 8.0f),
        "the two tail cells, at the cell pitch");

    // An end standing inside a cell cuts that cell's letters where the copy
    // does. The first cell owns the a and the b (origins 0 and 6.25); a span
    // from it leaving one letter out starts at the b, and a span ending on it
    // keeping one letter ends at the b's origin.
    const auto runWith = [&](const SelectionRowSpan& span) {
        wide.selectionSpans = &span;
        GlyphRouteAtlas atlas;
        GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
        backend.shapeProportionalRun = &GlyphRouteAtlas::shape;
        backend.bindProportionalAtlas = &GlyphRouteAtlas::binding;
        backend.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
        backend.context = &atlas;
        backend.proportionalStrip = nullptr;
        return buildCellInstances(wide, backend, wideBuffers);
    };
    const SelectionRowSpan fromTheB { 0, 1, 2, 1, 0 };
    const InstanceCounts fromTheBCounts = runWith(fromTheB);
    REQUIRE_MSG(fromTheBCounts.boxFills == 1, "one rect fills from the cut");
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, std::round(8.0f + 6.25f)), "from the b's origin, not the cell's edge");
    CHECK_MSG(approx(wideFills[0].bottomRightInPixels.x, std::round(8.0f + 18.75f)), "to the d's origin, the letter after the second cell's c");

    const SelectionRowSpan onlyTheA { 0, 1, 1, 0, 1 };
    const InstanceCounts onlyTheACounts = runWith(onlyTheA);
    REQUIRE_MSG(onlyTheACounts.boxFills == 1, "one rect fills up to the cut");
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, 8.0f) && approx(wideFills[0].bottomRightInPixels.x, std::round(8.0f + 6.25f)),
        "the a alone, to the b's origin");

    // A centred run in a claim wider than its text, selected whole with its
    // row: the fill covers the claim from its first cell, the slack before
    // the first letter included, to its last.
    Ubiquitty::TextSizeSpec centredSpec {};
    centredSpec.isProportional = 1;
    centredSpec.runWidthCells = 6;
    centredSpec.horizontalAlign = 2;
    const unsigned centredID = table.intern(centredSpec);
    for (int c = 1; c < 7; ++c) {
        Ubiquitty::Cell& cell = wideGrid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(centredID);
        Ubiquitty::cellSetPen(cell, pen);
    }
    wideBuffers.boxFillCapacity = wideFills.size();
    const SelectionRowSpan wholeRow { 0, 0, 15 };
    const InstanceCounts centredCounts = runWith(wholeRow);
    REQUIRE_MSG(centredCounts.boxFills == 1, "one rect fills the centred run's selected claim");
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, 8.0f) && approx(wideFills[0].bottomRightInPixels.x, 8.0f + 6.0f * 8.0f),
        ("the whole claim, slack on both sides included: " + std::to_string(wideFills[0].topLeftInPixels.x) + ".." + std::to_string(wideFills[0].bottomRightInPixels.x)).c_str());

    // A first letter whose origin sits short of a cell in begins, by its
    // origin, in the first cell; a whole-row selection still lights the
    // claim from its first cell rather than from the letter. Centred in a
    // four-cell claim, the 25 px text begins 3.5 px in.
    centredSpec.runWidthCells = 4;
    const unsigned fourID = table.intern(centredSpec);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = wideGrid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(fourID);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    for (int c = 5; c < 7; ++c)
        wideGrid[static_cast<std::size_t>(c)] = Ubiquitty::defaultBlankCell;
    const InstanceCounts fourCounts = runWith(wholeRow);
    REQUIRE(fourCounts.boxFills == 1);
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, 8.0f),
        ("a whole-row selection lights the claim from its first cell: " + std::to_string(wideFills[0].topLeftInPixels.x)).c_str());
    CHECK_MSG(approx(wideFills[0].bottomRightInPixels.x, 8.0f + 4.0f * 8.0f), "to its last");

    // A row selected in two pieces, one of them before the claim and the other
    // reaching into it, fills the run as the second piece alone does: the
    // piece elsewhere on the row, and the cells between, light nothing in it.
    wideBuffers.boxFillCapacity = wideFills.size();
    const SelectionRowSpan tailOfTheClaim { 0, 3, 4 };
    const InstanceCounts singlePieceCounts = runWith(tailOfTheClaim);
    REQUIRE(singlePieceCounts.boxFills == 1);
    const float singlePieceLeft = wideFills[0].topLeftInPixels.x;
    const float singlePieceRight = wideFills[0].bottomRightInPixels.x;
    const SelectionRowSpan twoPieces[] = { { 0, 0, 0 }, { 0, 3, 4 } };
    wide.selectionSpans = twoPieces;
    wide.selectionSpanCount = 2;
    GlyphRouteAtlas twoPiecesAtlas;
    GlyphBackend twoPiecesBackend = UbiquittyTest::glyphBackendFor(twoPiecesAtlas);
    twoPiecesBackend.shapeProportionalRun = &GlyphRouteAtlas::shape;
    twoPiecesBackend.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    twoPiecesBackend.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    twoPiecesBackend.context = &twoPiecesAtlas;
    twoPiecesBackend.proportionalStrip = nullptr;
    const InstanceCounts twoPiecesCounts = buildCellInstances(wide, twoPiecesBackend, wideBuffers);
    REQUIRE(twoPiecesCounts.boxFills == 1);
    CHECK_MSG(approx(wideFills[0].topLeftInPixels.x, singlePieceLeft) && approx(wideFills[0].bottomRightInPixels.x, singlePieceRight),
        "the piece before the claim does not stretch the fill back over it");
    CHECK_MSG(singlePieceLeft > 8.0f, "the claim's first cell is not lit");
    wide.selectionSpanCount = 1;

    // A stream with no room left — a screen of braille dots fills it before
    // the run is reached — is told what it needs: the fill is counted and not
    // written, so the caller grows the stream and the next frame lights the
    // run. Dropped uncounted, the run would stay unlit on every frame.
    wideBuffers.boxFillCapacity = 0;
    const InstanceCounts fullCounts = runWith(wholeRow);
    CHECK_MSG(fullCounts.boxFills == 1, "the fill is counted past the capacity");
}

// A backend that draws doubled proportional glyphs wide, rather than leaving
// the builder to stretch the ordinary ones, is asked for slot-wide windows of
// each glyph's doubled raster, and each window is drawn one atlas pixel to one
// screen pixel from the doubled quad's left. Stretched instead, the quad would
// resample the regular letterforms twice as wide.
TEST(testADoubledProportionalRunDrawsWindowsOfItsWideRasterOnePixelToOne)
{
    const int rows = 1, columns = 16;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.rasteriseSizedProportionalGlyph = &GlyphRouteAtlas::rasteriseSized;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };
    const std::uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.lineAttributes = lineAttributes;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    // Each glyph's ink ends six pixels into its slot, twelve doubled: one
    // eighteen-pixel window holds it.
    REQUIRE_MSG(atlas.sizedAsked.size() == 4, "one window asked for per doubled glyph");
    REQUIRE_MSG(counts.proportionalGlyphs == 4, "and one quad drawn for each");
    const float runStart = 2.0f * 8.0f;
    const float claimRightX = runStart + 4.0f * 2.0f * 8.0f;
    const float layoutOffset[4] = { 0.0f, 6.25f, 12.5f, 18.75f };
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_MSG(atlas.sizedAsked[i].glyph == 200u + i && atlas.sizedAsked[i].window == 0, "the glyph's first window, in text order");
        CHECK_MSG(atlas.sizedAsked[i].scale == 1 && atlas.sizedAsked[i].band == 0 && atlas.sizedAsked[i].widthScale == 2, "of the one-row raster a DECDWL row draws");
        const float quadLeft = std::floor(runStart + 2.0f * layoutOffset[i]) - 2.0f * 1.0f;
        CHECK_MSG(approx(proportionalGlyphs[i].originInPixels.x, quadLeft), "from the doubled quad's left");
        const float width = std::min(18.0f, claimRightX - quadLeft);
        CHECK_MSG(approx(proportionalGlyphs[i].sizeInPixels.x, width), "a slot wide, not a stretched double slot");
        CHECK_MSG(approx(proportionalGlyphs[i].atlasBottomRight.x - proportionalGlyphs[i].atlasTopLeft.x, width),
            "one atlas pixel to one screen pixel");
    }
}

namespace {

// A proportional run of `letters` letters claiming four cells from storage
// column `anchorColumn` of a one-row, sixteen-column DECDWL grid, built
// through a `GlyphRouteAtlas` the caller configures.
struct DoubledRunScene {
    static constexpr int columns = 16;
    Ubiquitty::TermPalette pal;
    Ubiquitty::TextSizeTable table;
    std::vector<Ubiquitty::Cell> grid;
    std::vector<CellInstance> cells;
    std::vector<DecorationInstance> decorations;
    std::vector<GlyphInstance> proportionalGlyphs;
    std::vector<GlyphInstance> strips;
    GlyphRouteAtlas atlas;
    InstanceCounts counts {};

    DoubledRunScene(int anchorColumn, int letters, std::uint8_t horizontalAlign = 0)
        : grid(static_cast<std::size_t>(columns), Ubiquitty::defaultBlankCell)
        , cells(static_cast<std::size_t>(columns) * 3 + 8)
        , decorations(cells.size() * 8)
        , proportionalGlyphs(32)
        , strips(16)
    {
        pal.seedDefaults();
        Ubiquitty::TextSizeSpec spec {};
        spec.isProportional = 1;
        spec.runWidthCells = 4;
        spec.horizontalAlign = horizontalAlign;
        const unsigned id = table.intern(spec);
        setCell(grid, columns, 0, anchorColumn, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
        for (int c = anchorColumn + 1; c < anchorColumn + 4; ++c)
            setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
        for (int c = anchorColumn; c < anchorColumn + 4; ++c) {
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
            Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
            pen.extendedAttributes.setTextSizeSpecID(id);
            Ubiquitty::penSetTextBand(&pen, 0);
            Ubiquitty::cellSetPen(cell, pen);
        }
        RunTextFixture::setLetters('a', letters);
        grid[static_cast<std::size_t>(anchorColumn)].setClusterID(1);
    }

    /// How the backend draws the run.
    enum class Route {
        /// One strip over the run's cells.
        Strip,
        /// A quad per glyph, stretched where the run is doubled.
        Glyphs,
        /// A quad per glyph, drawn wide in windows where the run is doubled.
        GlyphsInDoubledWindows,
    };

    enum class ReferenceMetrics {
        Provided,
        Omitted
    };

    void build(Route route, float presentationScale = 1, ReferenceMetrics referenceMetrics = ReferenceMetrics::Provided)
    {
        GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
        b.context = &atlas;
        if (route != Route::Strip) {
            b.shapeProportionalRun = &GlyphRouteAtlas::shape;
            b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
            b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
            b.proportionalStrip = nullptr;
            if (route == Route::GlyphsInDoubledWindows)
                b.rasteriseSizedProportionalGlyph = &GlyphRouteAtlas::rasteriseSized;
        } else {
            b.proportionalStrip = &GlyphRouteAtlas::strip;
        }
        const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };
        const std::uint8_t lineAttributes[1] = { Ubiquitty::lineAttributeDoubleWidth };
        BuildInputs in;
        in.cells = grid.data();
        in.rows = 1;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = &table;
        in.clusterResolver = &RunTextFixture::resolve;
        in.rowFlags = rowFlags;
        in.lineAttributes = lineAttributes;
        in.subpixelGlyphs = true;
        in.subpixelPhaseCount = 8;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics = scaledCellMetrics(in.metrics, presentationScale);
        if (referenceMetrics == ReferenceMetrics::Provided) {
            in.glyphReferenceAdvanceInPixels = 8.0f;
            in.glyphReferenceLineHeightInPixels = 16.0f;
        }
        in.glyphBoxWidthInPixels = 8.0f;
        in.glyphWideSlotWidthInPixels = 18.0f;
        in.glyphWideSlotHeightInPixels = 16.0f;
        in.glyphMarginInPixels = { 1.0f, 0.0f };
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        buffers.proportionalGlyphs = proportionalGlyphs.data();
        buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
        buffers.proportionalStrips = strips.data();
        buffers.proportionalStripCapacity = strips.size();
        counts = buildCellInstances(in, b, buffers);
    }
};

} // namespace

TEST(uniformFitScalesProportionalStripsGlyphsAndDoubledWindows)
{
    for (const auto route : { DoubledRunScene::Route::Strip, DoubledRunScene::Route::Glyphs, DoubledRunScene::Route::GlyphsInDoubledWindows }) {
        DoubledRunScene natural(1, 4);
        natural.build(route);
        DoubledRunScene fitted(1, 4);
        fitted.build(route, 0.5f);
        const bool isStrip = route == DoubledRunScene::Route::Strip;
        const auto count = isStrip ? natural.counts.proportionalStrips : natural.counts.proportionalGlyphs;
        REQUIRE(count > 0);
        REQUIRE(count == (isStrip ? fitted.counts.proportionalStrips : fitted.counts.proportionalGlyphs));
        const auto& original = isStrip ? natural.strips : natural.proportionalGlyphs;
        const auto& scaled = isStrip ? fitted.strips : fitted.proportionalGlyphs;
        for (std::size_t index = 0; index < count; ++index) {
            CHECK(std::fabs(scaled[index].originInPixels.x - original[index].originInPixels.x * 0.5f) <= 0.5f);
            CHECK(approx(scaled[index].originInPixels.y, original[index].originInPixels.y * 0.5f));
            CHECK(approx(scaled[index].sizeInPixels.y, original[index].sizeInPixels.y * 0.5f));
            CHECK(std::fabs(scaled[index].sizeInPixels.x - original[index].sizeInPixels.x * 0.5f) <= 0.5f);
            CHECK(approx(scaled[index].atlasBottomRight.y - scaled[index].atlasTopLeft.y,
                original[index].atlasBottomRight.y - original[index].atlasTopLeft.y));
        }
    }
}

TEST(uniformFitKeepsNativeAtlasUnitsWithoutOptionalReferenceMetrics)
{
    for (const std::uint8_t alignment : { 0, 1, 2, 3 }) {
        for (const auto route : { DoubledRunScene::Route::Strip, DoubledRunScene::Route::Glyphs, DoubledRunScene::Route::GlyphsInDoubledWindows }) {
            DoubledRunScene explicitMetrics(1, 4, alignment);
            explicitMetrics.build(route, 0.5f);
            DoubledRunScene fallback(1, 4, alignment);
            fallback.build(route, 0.5f, DoubledRunScene::ReferenceMetrics::Omitted);
            const bool isStrip = route == DoubledRunScene::Route::Strip;
            const auto count = isStrip ? explicitMetrics.counts.proportionalStrips : explicitMetrics.counts.proportionalGlyphs;
            REQUIRE(count > 0);
            REQUIRE(count == (isStrip ? fallback.counts.proportionalStrips : fallback.counts.proportionalGlyphs));
            const auto& expected = isStrip ? explicitMetrics.strips : explicitMetrics.proportionalGlyphs;
            const auto& actual = isStrip ? fallback.strips : fallback.proportionalGlyphs;
            const auto same = [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; };
            for (std::size_t index = 0; index < count; ++index) {
                CHECK(same(actual[index].originInPixels, expected[index].originInPixels));
                CHECK(same(actual[index].sizeInPixels, expected[index].sizeInPixels));
                CHECK(same(actual[index].atlasTopLeft, expected[index].atlasTopLeft));
                CHECK(same(actual[index].atlasBottomRight, expected[index].atlasBottomRight));
            }
        }
    }
}

TEST(uniformFitScalesProceduralBoxStrokeBasisWithItsCell)
{
    CellFixture fixture;
    auto cell = fixture.blank;
    cell.codePoint = 0x2500;
    BuildInputs in;
    in.cells = &cell;
    in.rows = 1;
    in.columns = 1;
    in.palette = &fixture.pal;
    in.metrics.cellWidth = 32;
    // Both scales yield integral stroke widths, isolating the presentation
    // transform from the procedural glyph's device-pixel rounding.
    in.metrics.cellHeight = 48;
    in.glyphReferenceLineHeightInPixels = 48;
    CellInstance cells[8];
    OverlayInstance boxes[8];
    InstanceBuffers buffers;
    buffers.cells = cells;
    buffers.cellCapacity = std::size(cells);
    buffers.boxFills = boxes;
    buffers.boxFillCapacity = std::size(boxes);
    FakeAtlas atlas;
    auto backend = UbiquittyTest::glyphBackendFor(atlas);
    backend.boxGlyph = nullptr;
    const auto natural = buildCellInstances(in, backend, buffers);
    REQUIRE(natural.boxFills > 0 && natural.boxFills <= std::size(boxes));
    const std::vector<OverlayInstance> original(boxes, boxes + natural.boxFills);
    in.metrics = scaledCellMetrics(in.metrics, 0.5f);
    const auto fitted = buildCellInstances(in, backend, buffers);
    REQUIRE(fitted.boxFills == natural.boxFills);
    for (std::size_t index = 0; index < natural.boxFills; ++index) {
        CHECK(approx(boxes[index].topLeftInPixels.x, original[index].topLeftInPixels.x * 0.5f));
        CHECK(approx(boxes[index].topLeftInPixels.y, original[index].topLeftInPixels.y * 0.5f));
        CHECK(approx(boxes[index].bottomRightInPixels.x, original[index].bottomRightInPixels.x * 0.5f));
        CHECK(approx(boxes[index].bottomRightInPixels.y, original[index].bottomRightInPixels.y * 0.5f));
    }
}

// A doubled run whose claim runs past the grid's right edge is cut at the
// edge, by every route that draws it and in the background tiles under it.
// Cut only at its own doubled claim, it drew ink and background into the
// padding beside the grid.
TEST(testADoubledProportionalRunIsCutAtTheGridsEdge)
{
    // Anchored at storage column 6 the run stands at 12 and claims eight
    // doubled cells of a sixteen-column grid: four of them are on it.
    const float gridRight = 16.0f * 8.0f;
    for (const DoubledRunScene::Route route : { DoubledRunScene::Route::Glyphs, DoubledRunScene::Route::Strip }) {
        const bool usesGlyphRoute = route == DoubledRunScene::Route::Glyphs;
        DoubledRunScene scene(6, 4);
        scene.build(route);
        for (std::size_t i = 0; i < std::min(scene.counts.cells, scene.cells.size()); ++i)
            CHECK_MSG(scene.cells[i].gridPosition.x + scene.cells[i].widthCells <= 16.0f, "no cell tile reaches past the grid");
        const std::size_t quads = usesGlyphRoute ? scene.counts.proportionalGlyphs : scene.counts.proportionalStrips;
        const std::vector<GlyphInstance>& drawn = usesGlyphRoute ? scene.proportionalGlyphs : scene.strips;
        REQUIRE_MSG(quads > 0, "the run draws");
        for (std::size_t i = 0; i < quads; ++i) {
            const GlyphInstance& quad = drawn[i];
            CHECK_MSG(quad.originInPixels.x + quad.sizeInPixels.x <= gridRight + 0.01f, "no quad reaches past the grid");
        }
        if (!usesGlyphRoute) {
            // The fixture's strip is thirty-two atlas pixels drawn sixty-four
            // wide from x = 96: half of it is on the grid, and the quad shows
            // that half of the strip rather than the whole squeezed into it.
            CHECK_MSG(approx(scene.strips[0].sizeInPixels.x, 32.0f), "the strip quad stops at the edge");
            CHECK_MSG(approx(scene.strips[0].atlasBottomRight.x - scene.strips[0].atlasTopLeft.x, 16.0f), "showing the half of the strip that is on the grid");
        }
    }
}

// The windows a doubled glyph is cut into are counted with room for what the
// wide raster draws past twice the ordinary ink: a face widened on its width
// axis has tighter side bearings, so its glyph scaled to the doubled advance
// reaches a little further right than the ordinary glyph doubled. Counted from
// the doubled ink alone, a glyph ending at the slot's edge lost that overhang.
TEST(testADoubledProportionalGlyphEndingAtItsSlotsEdgeGetsTheNextWindow)
{
    DoubledRunScene scene(1, 1);
    // Ink to nine of eighteen pixels: doubled, exactly one slot.
    scene.atlas.inkRight = 9;
    scene.build(DoubledRunScene::Route::GlyphsInDoubledWindows);
    REQUIRE_MSG(scene.atlas.sizedAsked.size() == 2, "the window past the doubled ink's own is faulted as well");
    CHECK_MSG(scene.atlas.sizedAsked[0].window == 0 && scene.atlas.sizedAsked[1].window == 1, "in order");
    CHECK_MSG(scene.counts.proportionalGlyphs == 2, "and both are drawn");
}

namespace {

// The run of `DoubledRunScene` — four letters claiming four cells from storage
// column 1 — on both rows of a DECDHL pair, the way a pair holds its text:
// `ESC # 3` on row 0, `ESC # 4` on row 1, sixteen columns of 8 by 16 pixels.
// The fixture's proportional slot is 18 by 20 with a 1 by 2 margin, and its
// shaper sets the second glyph 1.5 pixels up.
struct DoubleHeightRunScene {
    static constexpr int rows = 2;
    static constexpr int columns = 16;
    Ubiquitty::TermPalette pal;
    Ubiquitty::TextSizeTable table;
    std::vector<Ubiquitty::Cell> grid;
    std::vector<CellInstance> cells;
    std::vector<DecorationInstance> decorations;
    std::vector<GlyphInstance> proportionalGlyphs;
    std::vector<GlyphInstance> strips;
    GlyphRouteAtlas atlas;
    InstanceCounts counts {};

    DoubleHeightRunScene()
        : grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell)
        , cells(static_cast<std::size_t>(rows) * columns * 3 + 8)
        , decorations(cells.size() * 8)
        , proportionalGlyphs(32)
        , strips(16)
    {
        pal.seedDefaults();
        Ubiquitty::TextSizeSpec spec {};
        spec.isProportional = 1;
        spec.runWidthCells = 4;
        const unsigned id = table.intern(spec);
        for (int row = 0; row < rows; ++row) {
            setCell(grid, columns, row, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
            for (int c = 2; c < 5; ++c)
                setCell(grid, columns, row, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
            for (int c = 1; c < 5; ++c) {
                Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(row) * columns + c];
                Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, 0);
                Ubiquitty::cellSetPen(cell, pen);
            }
            grid[static_cast<std::size_t>(row) * columns + 1].setClusterID(1);
        }
        RunTextFixture::setLetters('a', 4);
    }

    /// How the backend draws a doubled glyph.
    enum class Raster {
        /// The ordinary glyph, which the builder stretches.
        Ordinary,
        /// Windows of a raster drawn at the doubled size.
        Doubled,
    };

    void build(Raster raster)
    {
        const bool hasDoubledRaster = raster == Raster::Doubled;
        GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
        b.context = &atlas;
        b.shapeProportionalRun = &GlyphRouteAtlas::shape;
        b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
        b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
        b.proportionalStrip = nullptr;
        if (hasDoubledRaster)
            b.rasteriseSizedProportionalGlyph = &GlyphRouteAtlas::rasteriseSized;
        const std::uint8_t rowFlags[rows] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed), static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };
        const std::uint8_t lineAttributes[rows] = { Ubiquitty::lineAttributeDoubleHeightTop, Ubiquitty::lineAttributeDoubleHeightBottom };
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = &table;
        in.clusterResolver = &RunTextFixture::resolve;
        in.rowFlags = rowFlags;
        in.lineAttributes = lineAttributes;
        in.subpixelGlyphs = true;
        in.subpixelPhaseCount = 8;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphBoxWidthInPixels = 8.0f;
        in.glyphWideSlotWidthInPixels = 18.0f;
        in.glyphWideSlotHeightInPixels = 16.0f;
        in.glyphMarginInPixels = { 1.0f, 0.0f };
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        buffers.proportionalGlyphs = proportionalGlyphs.data();
        buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
        buffers.proportionalStrips = strips.data();
        buffers.proportionalStripCapacity = strips.size();
        counts = buildCellInstances(in, b, buffers);
    }

    // Laid out at its own width the four origins stand 0, 6.25, 12.5 and 18.75
    // pixels along; doubled from the anchor's doubled column 2 each stands at
    // 16 plus twice that, its quad a doubled margin before its whole pixel.
    static float quadLeft(int glyph)
    {
        constexpr float layoutOffset[4] = { 0.0f, 6.25f, 12.5f, 18.75f };
        return std::floor(16.0f + 2.0f * layoutOffset[glyph]) - 2.0f;
    }

    // The doubled slot's top: two margins, and twice the shaper's offset,
    // above the pair's top.
    static float quadTop(int glyph) { return -2.0f * (2.0f + (glyph == 1 ? 1.5f : 0.0f)); }

    // The doubled claim of four cells ends at 16 + 4 * 2 * 8.
    static constexpr float claimRightX = 80.0f;
    static constexpr float midline = 16.0f;
};

} // namespace

// A proportional run on a DECDHL pair is drawn as one picture twice the size
// both ways, standing on the pair's top row: every glyph twice as far along
// and twice as large, the top row drawing the part above the midline the two
// rows share and the bottom row the part below it — where an ordinary doubled
// glyph's halves meet. Doubled across only, as on a DECDWL row, each half drew
// two copies of the run at the row's own height, one above the other.
TEST(testAProportionalRunOnADoubleHeightPairDoublesBothWaysAndSplitsAtTheMidline)
{
    DoubleHeightRunScene scene;
    scene.build(DoubleHeightRunScene::Raster::Ordinary);
    REQUIRE_MSG(scene.counts.proportionalGlyphs == 8, "one quad per glyph on each row of the pair");
    for (int half = 0; half < 2; ++half) {
        for (int glyph = 0; glyph < 4; ++glyph) {
            const GlyphInstance& quad = scene.proportionalGlyphs[static_cast<std::size_t>(half * 4 + glyph)];
            const float left = DoubleHeightRunScene::quadLeft(glyph);
            const float top = DoubleHeightRunScene::quadTop(glyph);
            const float width = std::min(2.0f * 18.0f, DoubleHeightRunScene::claimRightX - left);
            CHECK_MSG(approx(quad.originInPixels.x, left), "twice as far along, from the doubled column");
            CHECK_MSG(approx(quad.sizeInPixels.x, width), "twice the slot wide, cut where the doubled claim ends");
            CHECK_MSG(approx(quad.atlasBottomRight.x - quad.atlasTopLeft.x, width / 2.0f), "sampling the slot at half a texel a pixel across");
            // The doubled slot is forty pixels tall from its top; the midline
            // cuts it, and each row samples the matching share of the slot.
            const float cutInSlot = (DoubleHeightRunScene::midline - top) / 2.0f;
            if (half == 0) {
                CHECK_MSG(approx(quad.originInPixels.y, top), "the top row's part starts at the doubled slot's top");
                CHECK_MSG(approx(quad.originInPixels.y + quad.sizeInPixels.y, DoubleHeightRunScene::midline), "and stops at the midline");
                CHECK_MSG(approx(quad.atlasBottomRight.y - quad.atlasTopLeft.y, cutInSlot), "showing the slot above the cut, twice as tall");
            } else {
                CHECK_MSG(approx(quad.originInPixels.y, DoubleHeightRunScene::midline), "the bottom row's part starts at the midline");
                CHECK_MSG(approx(quad.originInPixels.y + quad.sizeInPixels.y, top + 40.0f), "and ends at the doubled slot's bottom");
                CHECK_MSG(approx(quad.atlasBottomRight.y - quad.atlasTopLeft.y, 20.0f - cutInSlot), "showing the slot below the cut, twice as tall");
            }
        }
    }
}

// A backend that draws doubled proportional glyphs large is asked, on a
// DECDHL pair, for windows of a raster twice the size both ways — the upper
// slot height for the part above the midline, the lower for the part below —
// and each window is drawn one atlas pixel to one screen pixel, cut at the
// midline. A row asks only for the halves that reach its side: the second
// glyph, set 1.5 pixels up, reaches below its doubled slot's upper half by
// three pixels before the midline, so the top row draws a sliver of its lower
// half too, and the bottom row draws it from the slot the top row faulted.
TEST(testADoubleHeightProportionalRunDrawsHalvesOfARasterTwiceItsSize)
{
    DoubleHeightRunScene scene;
    scene.build(DoubleHeightRunScene::Raster::Doubled);
    const std::vector<GlyphRouteAtlas::SizedAsk>& asked = scene.atlas.sizedAsked;
    REQUIRE_MSG(asked.size() == 8, "four upper halves and four lower ones, the shared one faulted once");
    const int expectedBands[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const std::uint32_t expectedGlyphs[8] = { 200u, 201u, 201u, 202u, 203u, 200u, 202u, 203u };
    for (std::size_t i = 0; i < asked.size(); ++i) {
        CHECK_MSG(asked[i].glyph == expectedGlyphs[i] && asked[i].band == expectedBands[i], "the halves each row reaches, in text order");
        CHECK_MSG(asked[i].scale == 2 && asked[i].widthScale == 1, "of a raster twice the size both ways");
        CHECK_MSG(asked[i].window == 0, "one window across holds each glyph's doubled ink");
    }
    for (std::size_t i = 0; i < asked.size(); ++i)
        for (std::size_t j = i + 1; j < asked.size(); ++j)
            CHECK_MSG(asked[i].slot != asked[j].slot, "every half of every glyph in a slot of its own");

    REQUIRE_MSG(scene.counts.proportionalGlyphs == 9, "five quads on the top row, four on the bottom");
    const int quadGlyphs[9] = { 0, 1, 1, 2, 3, 0, 1, 2, 3 };
    for (std::size_t i = 0; i < 9; ++i) {
        const GlyphInstance& quad = scene.proportionalGlyphs[i];
        const int glyph = quadGlyphs[i];
        const bool isTopRow = i < 5;
        const bool isLowerHalf = i == 2 || !isTopRow;
        const float left = DoubleHeightRunScene::quadLeft(glyph);
        const float windowTop = DoubleHeightRunScene::quadTop(glyph) + (isLowerHalf ? 20.0f : 0.0f);
        CHECK_MSG(approx(quad.originInPixels.x, left), "from the doubled quad's left");
        CHECK_MSG(approx(quad.sizeInPixels.x, std::min(18.0f, DoubleHeightRunScene::claimRightX - left)), "a slot wide");
        CHECK_MSG(approx(quad.atlasBottomRight.x - quad.atlasTopLeft.x, quad.sizeInPixels.x), "one atlas pixel to one screen pixel across");
        CHECK_MSG(approx(quad.atlasBottomRight.y - quad.atlasTopLeft.y, quad.sizeInPixels.y), "and down");
        const float expectedTop = isTopRow ? windowTop : std::max(windowTop, DoubleHeightRunScene::midline);
        const float expectedBottom = isTopRow ? std::min(windowTop + 20.0f, DoubleHeightRunScene::midline) : windowTop + 20.0f;
        CHECK_MSG(approx(quad.originInPixels.y, expectedTop), "the window's top, or the midline below it");
        CHECK_MSG(approx(quad.originInPixels.y + quad.sizeInPixels.y, expectedBottom), "its bottom, or the midline above it");
    }
}

namespace {

// A proportional run of `letters` letters claiming four cells at OSC 66 scale
// `scale`, from column 2 of a sixteen-column grid of 8 by 16 pixel cells: a
// block of `scale` rows of `4 * scale` columns, each row a band's anchor and
// continuations. The fixture's proportional slot is 18 by 20 with a 1 by 2
// margin, and its shaper sets the second glyph 1.5 pixels up. At scale 2 the
// block stands where a DECDHL pair's run from storage column 1 stands, so it
// draws the pair's picture.
struct ScaledRunScene {
    static constexpr int columns = 16;
    int rows = 2;
    Ubiquitty::TermPalette pal;
    Ubiquitty::TextSizeTable table;
    std::vector<Ubiquitty::Cell> grid;
    std::vector<CellInstance> cells;
    std::vector<DecorationInstance> decorations;
    std::vector<GlyphInstance> proportionalGlyphs;
    std::vector<GlyphInstance> strips;
    GlyphRouteAtlas atlas;
    InstanceCounts counts {};
    /// The selection drawn behind the run's text, when nonempty, and the fills it lights.
    std::vector<SelectionRowSpan> selection;
    std::vector<OverlayInstance> fills = std::vector<OverlayInstance>(16);

    explicit ScaledRunScene(int scale, int letters = 4)
        : rows(scale)
        , grid(static_cast<std::size_t>(scale) * columns, Ubiquitty::defaultBlankCell)
        , cells(static_cast<std::size_t>(scale) * columns * 3 + 8)
        , decorations(cells.size() * 8)
        , proportionalGlyphs(96)
        , strips(16)
    {
        pal.seedDefaults();
        Ubiquitty::TextSizeSpec spec {};
        spec.isProportional = 1;
        spec.scale = static_cast<std::uint8_t>(scale);
        spec.runWidthCells = 4;
        const unsigned id = table.intern(spec);
        const int last = 2 + 4 * scale;
        for (int row = 0; row < rows; ++row) {
            setCell(grid, columns, row, 2, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
            for (int c = 3; c < last; ++c)
                setCell(grid, columns, row, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
            for (int c = 2; c < last; ++c) {
                Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(row) * columns + c];
                Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, static_cast<std::uint8_t>(row));
                Ubiquitty::cellSetPen(cell, pen);
            }
            grid[static_cast<std::size_t>(row) * columns + 2].setClusterID(1);
        }
        RunTextFixture::setLetters('a', static_cast<std::size_t>(letters));
    }

    /// How the backend draws the run.
    enum class Route {
        /// One strip, which the builder stretches.
        Strip,
        /// A strip drawn at the run's size, a slot height a band.
        StripAtSize,
        /// The ordinary glyphs, which the builder stretches.
        Glyphs,
        /// Windows of a raster drawn at the run's scale.
        GlyphsInSizedWindows,
    };

    void build(Route route)
    {
        GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
        b.context = &atlas;
        if (route == Route::Strip || route == Route::StripAtSize) {
            b.proportionalStrip = route == Route::Strip ? &GlyphRouteAtlas::strip : &GlyphRouteAtlas::stripAtSize;
        } else {
            b.shapeProportionalRun = &GlyphRouteAtlas::shape;
            b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
            b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
            b.proportionalStrip = nullptr;
            if (route == Route::GlyphsInSizedWindows)
                b.rasteriseSizedProportionalGlyph = &GlyphRouteAtlas::rasteriseSized;
        }
        const std::vector<std::uint8_t> rowFlags(static_cast<std::size_t>(rows), static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed));
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &pal;
        in.textSizeTable = &table;
        in.clusterResolver = &RunTextFixture::resolve;
        in.rowFlags = rowFlags.data();
        in.subpixelGlyphs = true;
        in.subpixelPhaseCount = 8;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphBoxWidthInPixels = 8.0f;
        in.glyphWideSlotWidthInPixels = 18.0f;
        in.glyphWideSlotHeightInPixels = 16.0f;
        in.glyphMarginInPixels = { 1.0f, 0.0f };
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        buffers.proportionalGlyphs = proportionalGlyphs.data();
        buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
        buffers.proportionalStrips = strips.data();
        buffers.proportionalStripCapacity = strips.size();
        if (!selection.empty()) {
            in.selectionSpans = selection.data();
            in.selectionSpanCount = static_cast<int>(selection.size());
            in.selectionColor = { 0.70f, 0.84f, 1.0f, 1.0f };
            in.isSelectionBehindText = true;
            buffers.boxFills = fills.data();
            buffers.boxFillCapacity = fills.size();
        }
        counts = buildCellInstances(in, b, buffers);
    }
};

} // namespace

// A selection behind the text of a scaled run fills the letters it holds, as
// it fills a run of one row's, on every band row the selection names: the
// selection's columns are read in the run's own cells, each `scale` columns,
// so the pieces over the run's second and third cells light the letters whose
// ink begins there and not the first two, at the same place on both rows,
// each fill its own row tall.
TEST(testASelectionBehindAScaledRunFillsTheHeldLettersOnEveryBandRow)
{
    ScaledRunScene scene(2);
    scene.selection = { { 0, 4, 7 }, { 1, 4, 7 } };
    scene.build(ScaledRunScene::Route::Glyphs);
    REQUIRE_MSG(scene.counts.boxFills == 2, "one fill over the held letters on each band row");
    const OverlayInstance& top = scene.fills[0];
    const OverlayInstance& bottom = scene.fills[1];
    CHECK(approx(top.topLeftInPixels.y, 0.0f) && approx(top.bottomRightInPixels.y, 16.0f));
    CHECK(approx(bottom.topLeftInPixels.y, 16.0f) && approx(bottom.bottomRightInPixels.y, 32.0f));
    CHECK_MSG(approx(top.topLeftInPixels.x, bottom.topLeftInPixels.x) && approx(top.bottomRightInPixels.x, bottom.bottomRightInPixels.x),
        "both rows light the same letters");
    // The fixture's letters stand 6.25 px apart in 8 px cells, their ink a
    // pixel in: the third begins in the second cell and the fourth in the
    // third, so those two are held, from the third's origin doubled from the
    // anchor at 16 to the text's end.
    CHECK_MSG(approx(top.topLeftInPixels.x, 16.0f + 2.0f * 12.5f), "from the third letter's doubled origin");
    CHECK_MSG(approx(top.bottomRightInPixels.x, 16.0f + 2.0f * 25.0f), "to the doubled text's end");
}

// Drag F of the verifier's sheet: the selection holds every letter of a
// scale-3 run by a measurer that put the last letter in the run's second
// cell, and names them; this layout draws it in the fourth. The letters the
// pieces name are lit, the last one too, on every band row, which the
// pieces' columns alone would leave dark.
TEST(testAScaledRunLightsTheLettersTheSelectionNamesWhereverItsLayoutDrawsThem)
{
    for (const auto route : { ScaledRunScene::Route::Glyphs, ScaledRunScene::Route::GlyphsInSizedWindows }) {
        ScaledRunScene scene(3);
        scene.selection = { { 0, 2, 7, 0, 0, 0, 4 }, { 1, 2, 7, 0, 0, 0, 4 }, { 2, 2, 7, 0, 0, 0, 4 } };
        scene.build(route);
        REQUIRE_MSG(scene.counts.boxFills == 3, "one fill on each band row");
        for (std::size_t band = 0; band < 3; ++band) {
            const OverlayInstance& fill = scene.fills[band];
            // The four letters at 6.25 px, tripled from the anchor at 16: from
            // the first's origin to the text's end, the fourth's ink with it.
            CHECK_MSG(approx(fill.topLeftInPixels.x, 16.0f), ("band " + std::to_string(band) + " from the first letter").c_str());
            CHECK_MSG(approx(fill.bottomRightInPixels.x, 16.0f + 3.0f * 25.0f), ("band " + std::to_string(band) + " to the last letter's end").c_str());
        }
    }
}

// A run at OSC 66 scale 2 is the picture a DECDHL pair's run is: every glyph
// twice as far along and twice as large, standing on the block's top row,
// each band row drawing the part of it inside its own row. Drawn as two runs
// of one row, each band repeated the whole run at the row's own height.
TEST(testAProportionalRunAtScaleTwoDrawsThePairsPictureAcrossItsBands)
{
    for (const auto route : { ScaledRunScene::Route::Glyphs, ScaledRunScene::Route::GlyphsInSizedWindows }) {
        ScaledRunScene scaled(2);
        scaled.build(route);
        DoubleHeightRunScene pair;
        pair.build(route == ScaledRunScene::Route::Glyphs ? DoubleHeightRunScene::Raster::Ordinary : DoubleHeightRunScene::Raster::Doubled);
        REQUIRE_MSG(scaled.counts.proportionalGlyphs == pair.counts.proportionalGlyphs, "as many quads as the pair draws");
        for (std::size_t i = 0; i < scaled.counts.proportionalGlyphs; ++i) {
            const GlyphInstance& mine = scaled.proportionalGlyphs[i];
            const GlyphInstance& theirs = pair.proportionalGlyphs[i];
            CHECK_MSG(approx(mine.originInPixels.x, theirs.originInPixels.x) && approx(mine.originInPixels.y, theirs.originInPixels.y),
                ("quad " + std::to_string(i) + " where the pair's stands").c_str());
            CHECK_MSG(approx(mine.sizeInPixels.x, theirs.sizeInPixels.x) && approx(mine.sizeInPixels.y, theirs.sizeInPixels.y), "as large");
            CHECK_MSG(approx(mine.atlasTopLeft.y, theirs.atlasTopLeft.y) && approx(mine.atlasBottomRight.y, theirs.atlasBottomRight.y), "showing the same rows of its slot");
        }
        REQUIRE(scaled.atlas.sizedAsked.size() == pair.atlas.sizedAsked.size());
        for (std::size_t i = 0; i < scaled.atlas.sizedAsked.size(); ++i) {
            const GlyphRouteAtlas::SizedAsk& asked = scaled.atlas.sizedAsked[i];
            CHECK_MSG(asked.scale == 2 && asked.widthScale == 1 && asked.band == pair.atlas.sizedAsked[i].band && asked.window == pair.atlas.sizedAsked[i].window,
                "the raster twice the size both ways, cut where the pair's is");
        }
    }
}

// At scale 3 the backend is asked for windows of a raster three times the
// size both ways, three slots tall: as many windows across as the tripled ink
// needs, from the slot heights that reach each band row. Every window is drawn
// one atlas pixel to one screen pixel and cut at the edges of the row it is
// drawn on, so no quad of one row reaches into another's band, and the three
// rows between them draw every slot height of every glyph. Two letters, so
// their windows fit the fixture's sixteen slots.
TEST(testAProportionalRunAtScaleThreeDrawsWindowsOfARasterThreeTimesItsSizePerBand)
{
    ScaledRunScene scene(3, 2);
    scene.build(ScaledRunScene::Route::GlyphsInSizedWindows);
    const std::vector<GlyphRouteAtlas::SizedAsk>& asked = scene.atlas.sizedAsked;
    REQUIRE(!asked.empty());
    bool hasBand[3] = {};
    for (const GlyphRouteAtlas::SizedAsk& ask : asked) {
        CHECK_MSG(ask.scale == 3 && ask.widthScale == 1 && ask.band < 3, "a raster three times the size both ways");
        // Six pixels of ink and a margin, tripled, need two eighteen-pixel windows.
        CHECK_MSG(ask.window < 2, "two windows across hold the tripled ink");
        hasBand[ask.band] = true;
    }
    CHECK_MSG(hasBand[0] && hasBand[1] && hasBand[2], "every slot height of the raster is drawn");
    for (std::size_t i = 0; i < asked.size(); ++i)
        for (std::size_t j = i + 1; j < asked.size(); ++j)
            CHECK_MSG(asked[i].slot != asked[j].slot, "every window in a slot of its own");
    REQUIRE(scene.counts.proportionalGlyphs > 0);
    const float claimRightX = 16.0f + 4.0f * 3.0f * 8.0f;
    for (std::size_t i = 0; i < scene.counts.proportionalGlyphs; ++i) {
        const GlyphInstance& quad = scene.proportionalGlyphs[i];
        const float top = quad.originInPixels.y;
        const float bottom = top + quad.sizeInPixels.y;
        REQUIRE(quad.sizeInPixels.y > 0.0f);
        // The band a quad is drawn in; the outer bands reach past the block
        // by the slot's margin, as an ordinary row's glyphs do.
        const int band = std::clamp(static_cast<int>(std::floor((top + bottom) / 2.0f / 16.0f)), 0, 2);
        CHECK_MSG(band == 0 || top >= 16.0f * static_cast<float>(band) - 0.001f, ("quad " + std::to_string(i) + " starts inside its band").c_str());
        CHECK_MSG(band == 2 || bottom <= 16.0f * static_cast<float>(band + 1) + 0.001f, ("quad " + std::to_string(i) + " ends inside its band").c_str());
        CHECK_MSG(approx(quad.atlasBottomRight.x - quad.atlasTopLeft.x, quad.sizeInPixels.x), "one atlas pixel to one screen pixel across");
        CHECK_MSG(approx(quad.atlasBottomRight.y - quad.atlasTopLeft.y, quad.sizeInPixels.y), "and down");
        CHECK_MSG(quad.originInPixels.x + quad.sizeInPixels.x <= claimRightX + 0.001f, "cut where the tripled claim ends");
    }
    // The first glyph's windows start three margins before its tripled pen.
    CHECK(approx(scene.proportionalGlyphs[0].originInPixels.x, 16.0f - 3.0f));
}

// A backend that draws the strip at the run's size is asked for it at that
// size, a slot height at a time: each band row draws the slot heights that
// reach into it, one texel a pixel, cut at its row. The fixture's slot height
// is 20 pixels on 16-pixel rows, so the top row draws the first slot height
// down to its edge and the bottom row the rest of it and the second.
TEST(testAScaledProportionalRunDrawsItsStripAtItsSizeAcrossItsBands)
{
    ScaledRunScene scene(2);
    scene.build(ScaledRunScene::Route::StripAtSize);
    REQUIRE(!scene.atlas.stripSizesAsked.empty());
    for (const SizedTile& size : scene.atlas.stripSizesAsked)
        CHECK_MSG(size.scale == 2 && size.widthScale == 1 && size.band < 2, "asked for at twice the size both ways");
    REQUIRE_MSG(scene.counts.proportionalStrips == 3, "one slot height on the top row, two on the bottom");
    const float tops[3] = { 0.0f, 16.0f, 20.0f };
    const float heights[3] = { 16.0f, 4.0f, 20.0f };
    const float texelTops[3] = { 20.0f, 36.0f, 20.0f };
    for (std::size_t i = 0; i < 3; ++i) {
        const GlyphInstance& slice = scene.strips[i];
        CHECK_MSG(approx(slice.originInPixels.x, 16.0f) && approx(slice.sizeInPixels.x, 32.0f), "from the anchor, one texel a pixel across");
        CHECK_MSG(approx(slice.originInPixels.y, tops[i]) && approx(slice.sizeInPixels.y, heights[i]), ("slice " + std::to_string(i) + " cut at its row").c_str());
        CHECK_MSG(approx(slice.atlasTopLeft.y, texelTops[i]) && approx(slice.atlasBottomRight.y - slice.atlasTopLeft.y, heights[i]), "and down");
    }
}

// A strip a backend draws one row tall is stretched the same both ways at a
// scale, as a DECDHL pair's is, from the block's top, each band row drawing
// the part of it inside its own row.
TEST(testAScaledProportionalRunStretchesItsStripAcrossItsBands)
{
    ScaledRunScene scene(2);
    scene.build(ScaledRunScene::Route::Strip);
    REQUIRE_MSG(scene.counts.proportionalStrips == 2, "one strip slice on each band row");
    const GlyphInstance& top = scene.strips[0];
    const GlyphInstance& bottom = scene.strips[1];
    // The fixture's strip is 32 by 20 texels from (40, 20); doubled it is 64
    // by 40 pixels from the block's top at x 16.
    CHECK(approx(top.originInPixels.x, 16.0f) && approx(top.sizeInPixels.x, 64.0f));
    CHECK(approx(top.originInPixels.y, 0.0f) && approx(top.sizeInPixels.y, 16.0f));
    CHECK(approx(top.atlasTopLeft.y, 20.0f) && approx(top.atlasBottomRight.y, 28.0f));
    CHECK(approx(bottom.originInPixels.x, 16.0f) && approx(bottom.sizeInPixels.x, 64.0f));
    CHECK(approx(bottom.originInPixels.y, 16.0f) && approx(bottom.sizeInPixels.y, 24.0f));
    CHECK(approx(bottom.atlasTopLeft.y, 28.0f) && approx(bottom.atlasBottomRight.y, 40.0f));
}

TEST(testAProportionalAnchorWithTheGlyphRouteDrawsEachGlyphAtItsOwnPhase)
{
    const int rows = 1, columns = 8;
    Ubiquitty::TermPalette pal;
    pal.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.runWidthCells = 4;
    const unsigned id = table.intern(spec);
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell);
    setCell(grid, columns, 0, 1, 'a', Ubiquitty::VT::Attribute::Wide, colorDefault, colorDefault);
    for (int c = 2; c < 5; ++c)
        setCell(grid, columns, 0, c, wideContinuationSentinelCodePoint, Ubiquitty::VT::Attribute::WideContinuation, colorDefault, colorDefault);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(id);
        Ubiquitty::penSetTextBand(&pen, 0);
        Ubiquitty::cellSetPen(cell, pen);
    }
    // The run's four letters, named whole by the anchor — whose cluster field
    // carries the store id a real anchor carries.
    RunTextFixture::setLetters('a', 4);
    grid[1].setClusterID(1);

    GlyphRouteAtlas atlas;
    GlyphBackend b = UbiquittyTest::glyphBackendFor(atlas);
    b.shapeProportionalRun = &GlyphRouteAtlas::shape;
    b.bindProportionalAtlas = &GlyphRouteAtlas::binding;
    b.rasteriseProportionalGlyph = &GlyphRouteAtlas::rasterise;
    b.context = &atlas;
    b.proportionalStrip = nullptr;
    const std::uint8_t rowFlags[1] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.textSizeTable = &table;
    in.clusterResolver = &RunTextFixture::resolve;
    in.rowFlags = rowFlags;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphBoxWidthInPixels = 8.0f;
    in.glyphWideSlotWidthInPixels = 18.0f;
    in.glyphWideSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 0.0f };
    std::vector<CellInstance> cells(static_cast<std::size_t>(rows) * columns + 8);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> proportionalGlyphs(16);
    std::vector<GlyphInstance> strips(16);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.proportionalGlyphs = proportionalGlyphs.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();
    buffers.proportionalStrips = strips.data();
    buffers.proportionalStripCapacity = strips.size();
    const InstanceCounts counts = buildCellInstances(in, b, buffers);

    CHECK_MSG(atlas.shaped == 1, "the run is shaped once");
    CHECK_MSG(counts.proportionalGlyphs == 4, "one dedicated quad per glyph");
    CHECK_MSG(counts.wideGlyphs == 0, "the wide stream stays independent");
    CHECK_MSG(counts.proportionalStrips == 0, "and no strip fallback");
    CHECK_MSG(counts.ligatureGlyphs == 0, "the ligature stream stays independent");
    CHECK_MSG(atlas.asked.size() == 4, "each glyph asked for by identity");
    const std::uint32_t wantedPhase[4] = { 0, 2, 4, 6 };
    const float wantedOrigin[4] = { 8.0f, 14.0f, 20.0f, 26.0f };
    // The run claims four cells from column 1, so its ink stops at x = 40:
    // Docs/proportional-text.md's rule that a run draws no further than the
    // cells it asked for, rather than over whatever its neighbour put in the
    // next one. Three quads fit a whole slot; the fourth opens at 25 and is
    // cut there. The claim is spelled from the geometry rather than as 40 so
    // that a change to the cell box moves the expectation with it.
    const float claimRightX = 8.0f + 4.0f * 8.0f;
    for (std::size_t i = 0; i < 4 && i < atlas.asked.size() && i < counts.proportionalGlyphs; ++i) {
        CHECK_MSG(atlas.asked[i].glyph == 200u + i, "in text order");
        CHECK_MSG(atlas.asked[i].phase == wantedPhase[i], "at the phase of its own origin");
        // The quad's left edge: the origin's whole pixel less the margin.
        const float quadLeft = wantedOrigin[i] - 1.0f;
        CHECK_MSG(approx(proportionalGlyphs[i].originInPixels.x, quadLeft), "placed at the origin's whole pixel");
        const float wantedWidth = std::min(18.0f, claimRightX - quadLeft);
        CHECK_MSG(approx(proportionalGlyphs[i].sizeInPixels.x, wantedWidth), "a whole proportional slot, or cut where the claim ends");
        // A cut quad that still showed the whole atlas entry would squeeze the
        // letter into the remaining pixels instead of cutting it, so the entry
        // gives up the same fraction the quad did.
        const float uvSpan = proportionalGlyphs[i].atlasBottomRight.x - proportionalGlyphs[i].atlasTopLeft.x;
        CHECK_MSG(approx(uvSpan, wantedWidth), "the atlas entry cut by the same fraction");
    }
    CHECK_MSG(approx(proportionalGlyphs[0].originInPixels.y, -2.0f), "the sheet's own vertical margin sets the quad top");
    CHECK_MSG(approx(proportionalGlyphs[1].originInPixels.y, -3.5f), "a y-up shaping offset moves ink upward on screen");

    const auto naturalMetrics = in.metrics;
    in.metrics = scaledCellMetrics(naturalMetrics, 0.5f);
    const auto fitted = buildCellInstances(in, b, buffers);
    REQUIRE(fitted.proportionalGlyphs == 4);
    CHECK(approx(proportionalGlyphs[0].originInPixels.x, 3.5f));
    CHECK(approx(proportionalGlyphs[0].originInPixels.y, -1.0f));
    CHECK(approx(proportionalGlyphs[1].originInPixels.y, -1.75f));
    CHECK(approx(proportionalGlyphs[0].sizeInPixels.x, 9.0f));
    CHECK(approx(proportionalGlyphs[0].sizeInPixels.y, 10.0f));
    in.metrics = naturalMetrics;

    // The grid's cell-baked path has no bearing on a proportional face's
    // fractional advances. Its dedicated sheet supplies its own phase grid.
    in.subpixelGlyphs = false;
    atlas.proportionalCache.reset();
    atlas.asked.clear();
    const InstanceCounts fixedGrid = buildCellInstances(in, b, buffers);
    CHECK_MSG(fixedGrid.proportionalGlyphs == 4, "fixed grid cells keep the proportional route");
    for (std::size_t index = 0; index < atlas.asked.size(); ++index) {
        CHECK_MSG(atlas.asked[index].phase == wantedPhase[index], "the proportional phase does not depend on the grid glyph route");
    }
    in.subpixelGlyphs = true;

    // Right-aligned, the same run's quads move by the slack: four letters
    // at 6.25 px in four cells of 8 leave 7 px, so the first origin is 15.
    Ubiquitty::TextSizeSpec rightSpec {};
    rightSpec.isProportional = 1;
    rightSpec.runWidthCells = 4;
    rightSpec.horizontalAlign = 1;
    const unsigned rightID = table.intern(rightSpec);
    for (int c = 1; c < 5; ++c) {
        Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(c)];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(rightID);
        Ubiquitty::cellSetPen(cell, pen);
    }
    atlas.asked.clear();
    atlas.shaped = 0;
    const InstanceCounts rightCounts = buildCellInstances(in, b, buffers);
    CHECK_MSG(rightCounts.proportionalGlyphs == 4, "a right-aligned run still draws every glyph");
    CHECK_MSG(rightCounts.proportionalGlyphs == 4 && approx(proportionalGlyphs[0].originInPixels.x, 15.0f - 1.0f), "moved right by the slack");

    // A failed glyph rejects the route as one atomic run: the two glyphs
    // before it are rolled back and the strip draws once, rather than being
    // darkened by a second copy of their coverage.
    atlas.proportionalCache.reset();
    atlas.rejectedGlyph = 202;
    atlas.asked.clear();
    b.proportionalStrip = &GlyphRouteAtlas::strip;
    const InstanceCounts rejected = buildCellInstances(in, b, buffers);
    CHECK_MSG(rejected.proportionalGlyphs == 0, "a rejected glyph rolls back the run prefix");
    CHECK_MSG(rejected.proportionalStrips == 1, "the rejected route draws through the strip fallback");
    CHECK_MSG(rejected.ligatureGlyphs == 0, "the proportional fallback never enters the ligature stream");

    // Glyph zero is the face's missing-glyph sentinel. It must never become a
    // resident blank proportional slot, and takes the same explicit fallback.
    atlas.proportionalCache.reset();
    atlas.rejectedGlyph = 0;
    atlas.shouldShapeMissingGlyph = true;
    const InstanceCounts missing = buildCellInstances(in, b, buffers);
    CHECK_MSG(missing.proportionalGlyphs == 0, "glyph zero is not cached as a blank slot");
    CHECK_MSG(missing.proportionalStrips == 1, "a missing glyph takes the strip fallback");
    atlas.shouldShapeMissingGlyph = false;

    // A backend cannot make Core read beyond the shaping buffer by returning
    // an impossible count. The whole run takes the drawable fallback.
    atlas.proportionalCache.reset();
    atlas.shouldReturnOversizedShape = true;
    const InstanceCounts oversizedShape = buildCellInstances(in, b, buffers);
    CHECK_MSG(oversizedShape.proportionalGlyphs == 0, "an oversized shape result emits no dedicated glyphs");
    CHECK_MSG(oversizedShape.proportionalStrips == 1, "an oversized shape result takes the strip fallback");
    atlas.shouldReturnOversizedShape = false;

    atlas.proportionalCache.reset();
    atlas.hasOverlappingInk = true;
    const InstanceCounts overlapping = buildCellInstances(in, b, buffers);
    CHECK_MSG(overlapping.proportionalGlyphs == 0, "overlapping coverage rolls back the whole dedicated run");
    CHECK_MSG(overlapping.proportionalStrips == 1, "overlapping glyphs composite once in the strip");
    atlas.hasOverlappingInk = false;

    atlas.proportionalCache.reset();
    atlas.omitsInkMeasurement = true;
    const InstanceCounts unmeasured = buildCellInstances(in, b, buffers);
    CHECK_MSG(unmeasured.proportionalGlyphs == 0, "unmeasured ink cannot promise overlap-safe quads");
    CHECK_MSG(unmeasured.proportionalStrips == 1, "unmeasured ink has a drawable fallback");
    atlas.omitsInkMeasurement = false;

    // The count keeps rising after a short output buffer fills. A caller can
    // grow to the exact shaped-glyph need and rebuild without dropping text.
    atlas.proportionalCache.reset();
    b.proportionalStrip = nullptr;
    buffers.proportionalGlyphCapacity = 2;
    const InstanceCounts shortBuffer = buildCellInstances(in, b, buffers);
    CHECK_MSG(shortBuffer.proportionalGlyphs == 4, "capacity overflow reports the full shaped-glyph need");
    buffers.proportionalGlyphCapacity = proportionalGlyphs.size();

    // A new scale owns a new cache and publishes its new physical slot. The
    // builder takes both from the binding rather than retaining the wide-sheet
    // geometry or the prior scale's cache.
    atlas.proportionalCache.reset();
    atlas.proportionalSlotWidth = 36;
    atlas.proportionalSlotHeight = 40;
    const InstanceCounts scaled = buildCellInstances(in, b, buffers);
    CHECK_MSG(scaled.proportionalGlyphs == 4, "the rebuilt scale still emits every glyph");
    CHECK_MSG(approx(proportionalGlyphs[0].sizeInPixels.y, 40.0f), "the dedicated binding owns scale-dependent height");
    CHECK_MSG(approx(proportionalGlyphs[0].sizeInPixels.x, 26.0f), "the scaled slot is clipped only at the run claim");
    atlas.proportionalSlotWidth = 18;
    atlas.proportionalSlotHeight = 20;

    // The same grid on a row without the flag draws no run at all: the
    // builder asks nothing of a row the model marked as holding none.
    const std::uint8_t plainFlags[1] = { 0 };
    in.rowFlags = plainFlags;
    atlas.asked.clear();
    atlas.shaped = 0;
    const InstanceCounts plain = buildCellInstances(in, b, buffers);
    CHECK_MSG(atlas.shaped == 0 && plain.proportionalGlyphs == 0, "an unflagged row shapes nothing");
    // The anchor's cluster field names its run's text; on a row the model
    // holds no run, the builder reads that field as the cluster it is, one
    // cell wide.
    CHECK_MSG(atlas.clusters == 1 && approx(cells[1].widthCells, 1.0f),
        "and its anchor draws as the plain cluster cell it is");
}

TEST(testDoubledTextPlacementDoesNotDependOnAtlasPadding)
{
    struct PaddedAtlas : FakeAtlas {
        IntSize slotSize;
        AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion region)
        {
            return doubleRegionUV(5, GridSize { 4, 2 }, slotSize, region);
        }
    };
    constexpr int columns = 8;
    constexpr float cellWidth = 8.0f;
    constexpr float cellHeight = 16.0f;
    CellFixture fixture;
    for (int scale : { 1, 3 }) {
        for (auto hasDoubleHeight : { false, true }) {
            const int rows = scale == 3 ? 3 : (hasDoubleHeight ? 2 : 1);
            TextSizeTable table;
            const unsigned specification = scale == 3
                ? table.intern(TextSizeSpec { 3, 1 })
                : 0;
            std::vector<Cell> grid(static_cast<std::size_t>(columns * rows), fixture.blank);
            std::vector<std::uint8_t> attributes(rows, lineAttributeDoubleWidth);
            for (int row = 0; row < rows; ++row) {
                if (hasDoubleHeight) {
                    attributes[row] = row % 2 == 0 ? lineAttributeDoubleHeightTop : lineAttributeDoubleHeightBottom;
                }
                Cell& anchor = grid[static_cast<std::size_t>(row * columns)];
                anchor.codePoint = 'T';
                if (specification) {
                    Pen pen = anchor.pen;
                    pen.attributes.add(VT::Attribute::Wide);
                    pen.extendedAttributes.setTextSizeSpecID(specification);
                    penSetTextBand(&pen, static_cast<std::uint8_t>(row));
                    cellSetPen(anchor, pen);
                }
            }
            for (int margin : { 1, 6, 12 }) {
                PaddedAtlas atlas;
                atlas.slotSize = { 2 * (8 + 2 * margin + 1), 2 * (16 + 2 * margin) };
                BuildInputs inputs;
                inputs.cells = grid.data();
                inputs.rows = rows;
                inputs.columns = columns;
                inputs.palette = &fixture.pal;
                inputs.lineAttributes = attributes.data();
                inputs.textSizeTable = &table;
                inputs.metrics.cellWidth = cellWidth;
                inputs.metrics.cellHeight = cellHeight;
                inputs.glyphMarginInPixels = { static_cast<float>(margin), static_cast<float>(margin) };
                inputs.glyphReferenceAdvanceInPixels = cellWidth;
                inputs.glyphReferenceLineHeightInPixels = cellHeight;
                inputs.glyphDoubleSlotWidthInPixels = static_cast<float>(atlas.slotSize.width);
                inputs.glyphDoubleSlotHeightInPixels = static_cast<float>(atlas.slotSize.height);
                std::vector<CellInstance> cells(columns * rows);
                std::vector<DecorationInstance> decorations(columns * rows * 8);
                std::vector<GlyphInstance> glyphs(rows);
                InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
                buffers.doubleGlyphs = glyphs.data();
                buffers.doubleGlyphCapacity = glyphs.size();
                const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
                CHECK(counts.doubleGlyphs == static_cast<std::size_t>(rows));
                for (int row = 0; row < rows; ++row) {
                    const auto& glyph = glyphs[row];
                    // A marker inside the raster's cell box must land at the
                    // same screen position regardless of surrounding padding.
                    const float markerX = static_cast<float>(atlas.slotSize.width + 2 * margin) + 6.0f;
                    const float markerY = static_cast<float>(atlas.slotSize.height + 2 * margin)
                        + (scale == 3 ? (static_cast<float>(row) + 0.25f) * 32.0f / 3.0f
                                      : (hasDoubleHeight ? static_cast<float>(row) * 16.0f + 4.0f : 8.0f));
                    const float screenX = glyph.originInPixels.x + (markerX - glyph.atlasTopLeft.x) * glyph.sizeInPixels.x / (glyph.atlasBottomRight.x - glyph.atlasTopLeft.x);
                    const float screenY = glyph.originInPixels.y + (markerY - glyph.atlasTopLeft.y) * glyph.sizeInPixels.y / (glyph.atlasBottomRight.y - glyph.atlasTopLeft.y);
                    CHECK(approx(screenX, 6.0f * static_cast<float>(scale)));
                    CHECK(approx(screenY, static_cast<float>(row) * cellHeight + 4.0f));
                    if (scale == 3 && row + 1 < rows) {
                        CHECK(approx(glyph.originInPixels.y + glyph.sizeInPixels.y,
                            glyphs[row + 1].originInPixels.y));
                    }
                }
                for (int row = 0; row < rows; ++row) {
                    grid[static_cast<std::size_t>(row * columns)].codePoint = 0x250C;
                }
                buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
                for (int row = 0; row < rows; ++row) {
                    CHECK(approx(glyphs[row].originInPixels.y, static_cast<float>(row) * cellHeight));
                    CHECK(approx(glyphs[row].sizeInPixels.y, cellHeight));
                    CHECK(approx(glyphs[row].originInPixels.x, 0.0f));
                    CHECK(approx(glyphs[row].sizeInPixels.x, 2.0f * cellWidth * static_cast<float>(scale)));
                    grid[static_cast<std::size_t>(row * columns)].codePoint = 'T';
                }
            }
        }
    }
}

TEST(testPowerlineSeparatorsMatchBackgroundCoverage)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 4;
    std::vector<Cell> grid(columns);
    setCell(grid, columns, 0, 0, ' ', {}, colorDefault, 1);
    setCell(grid, columns, 0, 1, 0xE0B0, {}, 1, 4);
    setCell(grid, columns, 0, 2, ' ', {}, colorDefault, 4);
    setCell(grid, columns, 0, 3, 0xE0B1, {}, 6, colorDefault);
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.preservesBackgroundTints = true;
    inputs.usesNativeTextVibrancy = true;
    inputs.uniformDefaultBackground = { 0x10 / 255.0f, 0x10 / 255.0f, 0x14 / 255.0f, 1 };
    std::array<CellInstance, columns> cells;
    std::array<OverlayInstance, columns> fills;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.boxFills = fills.data();
    buffers.boxFillCapacity = fills.size();
    FakeAtlas atlas;
    for (const bool isVibrant : { false, true }) {
        inputs.usesNativeBackgroundVibrancy = isVibrant;
        for (const float opacity : { 0.0f, 0.5f, 1.0f }) {
            inputs.backgroundOpacity = opacity;
            for (const auto codePoint : { 0xE0B0u, 0xE0B2u, 0xE0B4u, 0xE0B6u, 0xE0B8u, 0xE0BAu, 0xE0BCu, 0xE0BEu }) {
                grid[1].codePoint = codePoint;
                const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
                CHECK(counts.cells == columns);
                CHECK(counts.boxFills == 2);
                CHECK(approx(fills[0].color.x, cells[0].background.x));
                CHECK(approx(fills[0].color.y, cells[0].background.y));
                CHECK(approx(fills[0].color.z, cells[0].background.z));
                CHECK_MSG(approx(fills[0].color.w, cells[0].background.w),
                    "the filled separator has the adjacent background's coverage and native routing");
                CHECK(fills[0].usesBackgroundColors == 1);
                CHECK(approx(fills[0].background.x, cells[2].background.x));
                CHECK(approx(fills[0].background.y, cells[2].background.y));
                CHECK(approx(fills[0].background.z, cells[2].background.z));
                CHECK(approx(fills[0].background.w, cells[2].background.w));
                CHECK_MSG(approx(fills[1].color.w, cells[3].foreground.w),
                    "an outline separator follows foreground vibrancy");
            }
        }
    }
    inputs.usesNativeTextVibrancy = false;
    inputs.shouldEnhanceTextContrast = true;
    inputs.backgroundOpacity = 0;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(approx(fills[0].color.x, cells[0].background.x)
            && approx(fills[0].color.y, cells[0].background.y)
            && approx(fills[0].color.z, cells[0].background.z),
        "text contrast does not recolor a background separator");
    inputs.preservesBackgroundTints = false;
    inputs.usesNativeBackgroundVibrancy = false;
    inputs.backgroundOpacity = 1;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(approx(fills[0].color.x, cells[0].background.x)
            && approx(fills[0].color.y, cells[0].background.y)
            && approx(fills[0].color.z, cells[0].background.z),
        "opaque themes preserve Powerline background colors with perceptual text contrast enabled");
    inputs.usesNativeBackgroundVibrancy = true;
    inputs.preservesBackgroundTints = false;
    inputs.backgroundOpacity = 0.35f;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(fills[0].usesBackgroundColors == 1 && approx(fills[0].color.w, cells[0].background.w),
        "background vibrancy also matches separators in custom themes");
    grid[1].pen.attributes.add(VT::Attribute::Hidden);
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(fills[0].foregroundCoverage == 0);
    grid[1].pen.attributes.remove(VT::Attribute::Hidden);
    grid[1].codePoint = 0xE0B1;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK_MSG(fills[0].usesBackgroundColors == 0, "reused geometry does not retain background transition state");
}

TEST(testBlockElementsMatchBackgroundCoverage)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 3;
    std::vector<Cell> grid(columns);
    setCell(grid, columns, 0, 0, ' ', {}, colorDefault, 1);
    setCell(grid, columns, 0, 1, 0x2580, {}, 1, 4);
    setCell(grid, columns, 0, 2, ' ', {}, colorDefault, 4);
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.preservesBackgroundTints = true;
    inputs.usesNativeTextVibrancy = true;
    inputs.shouldEnhanceTextContrast = true;
    inputs.uniformDefaultBackground = { 0x10 / 255.0f, 0x10 / 255.0f, 0x14 / 255.0f, 1 };
    std::array<CellInstance, columns> cells;
    std::array<OverlayInstance, 32> fills;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.boxFills = fills.data();
    buffers.boxFillCapacity = fills.size();
    FakeAtlas atlas;
    for (const bool isVibrant : { false, true }) {
        inputs.usesNativeBackgroundVibrancy = isVibrant;
        for (const float opacity : { 0.0f, 0.5f, 1.0f }) {
            inputs.backgroundOpacity = opacity;
            inputs.usesNativeTextVibrancy = opacity < 1;
            for (unsigned codePoint = 0x2580; codePoint <= 0x259F; ++codePoint) {
                grid[1].codePoint = codePoint;
                const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
                CHECK(counts.cells == columns);
                CHECK(counts.boxFills > 0 && counts.boxFills <= fills.size());
                for (std::size_t index = 0; index < counts.boxFills; ++index) {
                    const auto& fill = fills[index];
                    CHECK(fill.usesBackgroundColors == 1);
                    CHECK(approx(fill.color.x, cells[0].background.x));
                    CHECK(approx(fill.color.y, cells[0].background.y));
                    CHECK(approx(fill.color.z, cells[0].background.z));
                    CHECK_MSG(approx(fill.color.w, cells[0].background.w), "block ink matches the adjacent fill's ordinary and vibrant coverage");
                    CHECK(approx(fill.background.x, cells[2].background.x));
                    CHECK(approx(fill.background.y, cells[2].background.y));
                    CHECK(approx(fill.background.z, cells[2].background.z));
                    CHECK(approx(fill.background.w, cells[2].background.w));
                }
            }
        }
    }
}

TEST(testNativeBackgroundVibrancyPreservesTmuxStatusTints)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 5;
    std::vector<Cell> grid(columns);
    for (int column = 0; column < columns - 1; ++column) {
        setCell(grid, columns, 0, column, column % 2 ? 'A' : ' ', {}, colorRGB, colorRGB);
        grid[column].pen.foregroundRed = 0x15;
        grid[column].pen.foregroundGreen = 0x17;
        grid[column].pen.foregroundBlue = 0x1C;
        const RGB8 tint = column < 2 ? RGB8 { 0x72, 0xA3, 0xE8 } : RGB8 { 0xE3, 0xBE, 0x7C };
        grid[column].pen.backgroundRed = tint.red;
        grid[column].pen.backgroundGreen = tint.green;
        grid[column].pen.backgroundBlue = tint.blue;
    }
    setCell(grid, columns, 0, columns - 1, ' ', {}, colorDefault, colorDefault);
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.uniformDefaultBackground = { 0x10 / 255.0f, 0x10 / 255.0f, 0x14 / 255.0f, 1 };
    std::array<CellInstance, columns> cells;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    FakeAtlas atlas;
    for (const float opacity : { 0.0f, 0.5f, 1.0f, 0.0f }) {
        inputs.backgroundOpacity = opacity;
        inputs.usesNativeBackgroundVibrancy = opacity < 1;
        const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
        CHECK(counts.cells == columns);
        CHECK(approx(cells.back().background.w, opacity));
        for (int column = 0; column < columns - 1; ++column) {
            const auto& background = cells[column].background;
            CHECK_MSG(std::abs(background.w) > 0.5f, "tmux status tints survive zero theme opacity on ordinary themes");
            CHECK((background.w < 0) == inputs.usesNativeBackgroundVibrancy);
            CHECK(approx(background.x, grid[column].pen.backgroundRed / 255.0f));
            CHECK(approx(background.y, grid[column].pen.backgroundGreen / 255.0f));
            CHECK(approx(background.z, grid[column].pen.backgroundBlue / 255.0f));
        }
    }
}

TEST(testNativeSemanticInkPreservesColoredAndPrimaryCells)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 5;
    std::vector<Cell> grid(columns);
    for (int column = 0; column < columns; ++column) {
        setCell(grid, columns, 0, column, 'A', {}, colorRGB, colorDefault);
    }
    const std::array<RGB8, columns> colors { RGB8 { 120, 120, 120 }, { 65, 65, 65 }, { 210, 120, 80 }, { 255, 210, 0 }, { 255, 255, 255 } };
    for (int column = 0; column < columns; ++column) {
        grid[column].pen.foregroundRed = colors[column].red;
        grid[column].pen.foregroundGreen = colors[column].green;
        grid[column].pen.foregroundBlue = colors[column].blue;
    }
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.metrics.baselineFromTop = 13;
    inputs.backgroundOpacity = 0;
    inputs.usesNativeTextVibrancy = true;
    inputs.uniformDefaultBackground = { 0x10 / 255.0f, 0x10 / 255.0f, 0x14 / 255.0f, 1 };
    inputs.nativeSecondaryInk = { 0.48f, 0.48f, 0.48f, 1 };
    inputs.nativeTertiaryInk = { 0.25f, 0.25f, 0.25f, 1 };
    std::array<CellInstance, columns> cells;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    FakeAtlas atlas;
    const auto counts = buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(counts.cells == columns);
    CHECK(cells[0].foreground.w == -1);
    CHECK(cells[0].foreground.x > cells[1].foreground.x);
    CHECK(cells[1].foreground.w == -1);
    CHECK(cells[1].foreground.x > 0);
    for (int column = 2; column < columns; ++column) {
        CHECK(cells[column].foreground.w == (column == 4 ? 1 : -1));
        CHECK(approx(cells[column].foreground.x, colors[column].red / 255.0f));
        CHECK(approx(cells[column].foreground.y, colors[column].green / 255.0f));
        CHECK(approx(cells[column].foreground.z, colors[column].blue / 255.0f));
    }
}

TEST(testGlassInkAndBackgroundRamps)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 32;
    std::vector<Cell> grid(columns);
    BuildInputs inputs;
    inputs.cells = grid.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.metrics.cellWidth = 8;
    inputs.metrics.cellHeight = 16;
    inputs.metrics.baselineFromTop = 13;
    inputs.backgroundOpacity = 0;
    inputs.usesNativeTextVibrancy = true;
    inputs.preservesBackgroundTints = true;
    inputs.uniformDefaultBackground = { 16 / 255.0f, 16 / 255.0f, 20 / 255.0f, 1 };
    inputs.nativeSecondaryInk = { 0.48f, 0.48f, 0.48f, 1 };
    inputs.nativeTertiaryInk = { 0.25f, 0.25f, 0.25f, 1 };
    std::array<CellInstance, columns> cells;
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    FakeAtlas atlas;
    for (int column = 0; column < columns; ++column) {
        setCell(grid, columns, 0, column, '*', {}, colorRGB, colorDefault);
        const auto gray = static_cast<uint8_t>(column * 5);
        grid[column].pen.foregroundRed = gray;
        grid[column].pen.foregroundGreen = gray;
        grid[column].pen.foregroundBlue = gray;
    }
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    for (int column = 0; column < columns; ++column) {
        CHECK(cells[column].foreground.w < 0);
        if (column) {
            CHECK(cells[column].foreground.x > cells[column - 1].foreground.x);
        }
    }
    for (int column = 0; column < columns; ++column) {
        setCell(grid, columns, 0, column, ' ', {}, colorDefault, colorRGB);
        const auto gray = static_cast<uint8_t>(24 + column * 7);
        grid[column].pen.backgroundRed = gray;
        grid[column].pen.backgroundGreen = gray;
        grid[column].pen.backgroundBlue = gray;
    }
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    for (int column = 0; column < columns; ++column) {
        CHECK(cells[column].background.w > 0);
        if (column) {
            CHECK(cells[column].background.w > cells[column - 1].background.w);
        }
    }
    setCell(grid, columns, 0, 0, 'A', {}, 0, colorDefault);
    setCell(grid, columns, 0, 1, 'A', VT::Attribute::Hidden, 0, colorDefault);
    setCell(grid, columns, 0, 2, 'A', {}, 0, 7);
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(cells[0].foreground.w < 0);
    CHECK(approx(cells[0].foreground.x, inputs.nativeTertiaryInk.x));
    CHECK(cells[1].foreground.w == 0);
    CHECK(cells[2].foreground.w > 0);
    CHECK(cells[2].foreground.x < 0.1f);
    CHECK(cells[2].background.w > 0.5f);
    const float selectionOpacity = cells[2].background.w;
    inputs.usesNativeTextVibrancy = false;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(cells[2].background.w == selectionOpacity);
    const auto ordinaryBackgrounds = cells;
    inputs.usesNativeBackgroundVibrancy = true;
    buildCellInstances(inputs, UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(cells[0].background.w == 0);
    for (int column = 2; column < columns; ++column) {
        CHECK(cells[column].background.w == -ordinaryBackgrounds[column].background.w);
        CHECK(cells[column].background.x == ordinaryBackgrounds[column].background.x);
        CHECK(cells[column].background.y == ordinaryBackgrounds[column].background.y);
        CHECK(cells[column].background.z == ordinaryBackgrounds[column].background.z);
    }
    CHECK(cells[2].foreground.w > 0);
    CHECK(cells[2].foreground.x < 0.1f);
}

// A healed letterform is drawn by its glyph id from a mono slot, and the
// underline under it follows that slot's measured ink past the cell box the
// way a code point's does. The `w` here is one underlined cell between two
// plain blanks, and its ink runs a pixel past each side of its cell: the rule
// spans the ink, and with the setting off it keeps the cell.
TEST(anUnderlineFollowsTheInkOfAGlyphDrawnByItsShapedIdentity)
{
    struct HealingAtlas : FakeAtlas {
        // Twelve-pixel slots around an eight-pixel glyph box, so a raster can
        // carry ink into the two-pixel margins on either side of its cell.
        std::vector<GlyphInkBox> inkBoxes { 1024 };
        int shapedRasterised = 0;
        MonoAtlasBinding bindMonoAtlas()
        {
            MonoAtlasBinding binding;
            binding.cache = &monoCache;
            binding.geometry = { /*columns=*/32, /*slotWidthInPixels=*/12, /*slotHeightInPixels=*/20,
                /*glyphWidthInPixels=*/8, /*glyphMarginInPixels=*/ { 2, 2 } };
            binding.slotCapacity = 1024;
            binding.inkBoxForSlot = inkBoxes.data();
            return binding;
        }
        // The whole slot inked, which with its guard texel is one pixel of
        // ink past each edge of the cell.
        bool rasteriseShapedGlyph(std::uint32_t, FontFace, std::uint32_t, int slot)
        {
            ++shapedRasterised;
            inkBoxes[static_cast<std::size_t>(slot)] = { { 0.0f, 2.0f }, { 12.0f, 18.0f } };
            return true;
        }
        void shapeLigatureRuns(std::span<const LigatureCell>, float, float, GlyphPass, std::span<LigatureRun> runs)
        {
            std::fill(runs.begin(), runs.end(), LigatureRun {});
            LigatureRun& run = runs[1];
            run.strip.present = true;
            run.strip.maxUV = { 96.0f, 20.0f };
            run.slotWidthInAtlasPixels = 96.0f;
            run.usedWidthInAtlasPixels = 12.0f;
            run.startColumn = 1;
            run.length = 1;
            run.hasShapedGlyph = true;
            run.shaped.glyphID = 77;
            run.shaped.coveredCells = 1;
        }
    };

    constexpr int columns = 3;
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(columns, defaultBlankCell);
    setCell(grid, columns, 0, 0, ' ', {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 1, 'w', VT::Attribute::Underline, colorDefault, colorDefault);
    setCell(grid, columns, 0, 2, ' ', {}, colorDefault, colorDefault);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &palette;
    in.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 20.0f;
    in.metrics.baselineFromTop = 15.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.glyphMarginInPixels = { 2.0f, 2.0f };
    in.glyphSlotWidthInPixels = 12.0f;
    in.glyphSlotHeightInPixels = 20.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 20.0f;

    struct Rule {
        float left = 0.0f;
        float right = 0.0f;
        std::size_t count = 0;
    };
    HealingAtlas atlas;
    auto underlineOf = [&](bool shouldFollowInk) {
        in.metrics.shouldUnderlineFollowInk = shouldFollowInk;
        std::vector<CellInstance> cells(columns);
        std::vector<DecorationInstance> decorations(columns * 8);
        std::vector<GlyphInstance> glyphs(columns * 2);
        std::vector<GlyphInstance> ligatures(columns);
        InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
        buffers.glyphs = glyphs.data();
        buffers.glyphCapacity = glyphs.size();
        buffers.ligatureGlyphs = ligatures.data();
        buffers.ligatureGlyphCapacity = ligatures.size();
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        CHECK_MSG(counts.ligatureGlyphs == 0, "the healed cell draws from its own slot and takes no strip slice");
        Rule rule;
        rule.count = counts.decorations;
        if (counts.decorations > 0) {
            rule.left = decorations[0].topLeftInPixels.x;
            rule.right = decorations[0].bottomRightInPixels.x;
        }
        return rule;
    };

    const Rule boxed = underlineOf(false);
    CHECK_MSG(atlas.shapedRasterised == 1, "the glyph is drawn by its shaped identity");
    CHECK_MSG(boxed.count == 1, "one rule under the one underlined cell");
    CHECK_MSG(approx(boxed.left, 8.0f) && approx(boxed.right, 16.0f), "with the setting off the rule keeps the cell box");

    const Rule followed = underlineOf(true);
    CHECK_MSG(followed.count == 1, "still one rule");
    CHECK_MSG(approx(followed.left, 7.0f), "the rule starts where the healed glyph's ink starts");
    CHECK_MSG(approx(followed.right, 17.0f), "and ends where its ink ends, past the cell's right edge");
}

namespace {

struct StripSliceRule {
    bool isDrawn = false;
    float sliceLeft = 0.0f;
    float sliceRight = 0.0f;
    int ruleCount = 0;
    float ruleLeft = 0.0f;
    float ruleRight = 0.0f;
};

// Underlines a lone overhanging `w` drawn as its slice of a measured strip,
// whose ledger entry is recorded the way a producer records it: the glyph's ink
// at x 1..11 of the slot — a pixel past each edge of the eight-pixel cell, the
// slot's pen sitting two margin pixels in — and the fringe the producer draws
// past that ink.
StripSliceRule underlinedStripSlice(int fringeInPixels)
{
    struct StripAtlas : FakeAtlas {
        GlyphSlotCache ledger { 4, recommendedHashBuckets(4), 4 };
        int slot = -1;
        void shapeLigatureRuns(std::span<const LigatureCell>, float, float, GlyphPass, std::span<LigatureRun> runs)
        {
            std::fill(runs.begin(), runs.end(), LigatureRun {});
            LigatureRun& run = runs[1];
            run.strip.present = true;
            run.strip.maxUV = { 96.0f, 20.0f };
            run.slotWidthInAtlasPixels = 96.0f;
            run.usedWidthInAtlasPixels = 12.0f;
            run.startColumn = 1;
            run.length = 1;
            attachOwnedInk(run, ledger.ownedInkForSlot(slot, 0));
        }
    };

    constexpr int columns = 3;
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(columns, defaultBlankCell);
    setCell(grid, columns, 0, 0, ' ', {}, colorDefault, colorDefault);
    setCell(grid, columns, 0, 1, 'w', VT::Attribute::Underline, colorDefault, colorDefault);
    setCell(grid, columns, 0, 2, ' ', {}, colorDefault, colorDefault);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &palette;
    in.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
    in.subpixelGlyphs = true;
    in.subpixelPhaseCount = 8;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 20.0f;
    in.metrics.baselineFromTop = 15.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.shouldUnderlineFollowInk = true;
    in.glyphMarginInPixels = { 2.0f, 2.0f };
    in.glyphSlotWidthInPixels = 12.0f;
    in.glyphSlotHeightInPixels = 20.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 20.0f;

    StripAtlas atlas;
    atlas.ledger.enableOwnedInkRecords(stripSlotCells, 96);
    bool isFresh = false;
    atlas.slot = atlas.ledger.slotForKey(1, &isFresh);
    atlas.ledger.beginOwnedInkRecord(atlas.slot);
    atlas.ledger.recordShapedGlyphInk(atlas.slot, 0, 0, 1, 1, 11, fringeInPixels);
    atlas.ledger.finalizeOwnedInkRecord(atlas.slot);

    std::vector<CellInstance> cells(columns);
    std::vector<DecorationInstance> decorations(columns * 8);
    std::vector<GlyphInstance> glyphs(columns * 2);
    std::vector<GlyphInstance> ligatures(columns);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    buffers.ligatureGlyphs = ligatures.data();
    buffers.ligatureGlyphCapacity = ligatures.size();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    StripSliceRule result;
    result.isDrawn = counts.ligatureGlyphs == 1;
    if (result.isDrawn) {
        result.sliceLeft = ligatures[0].originInPixels.x;
        result.sliceRight = ligatures[0].originInPixels.x + ligatures[0].sizeInPixels.x;
    }
    result.ruleCount = static_cast<int>(counts.decorations);
    if (result.ruleCount == 1) {
        result.ruleLeft = decorations[0].topLeftInPixels.x;
        result.ruleRight = decorations[0].bottomRightInPixels.x;
    }
    return result;
}

} // namespace

// A producer that measured its raster records the ink with no fringe, as the
// FreeType arm does: the slice it draws and the rule under it both span the
// ink, and a rule a pixel narrower each side would leave the letter's
// antialiased edge columns hanging past its ends.
TEST(anUnderlineFollowsTheOwnedInkOfAStripSlice)
{
    const StripSliceRule measured = underlinedStripSlice(0);
    CHECK_MSG(measured.isDrawn, "the cell draws its slice of the strip");
    CHECK_MSG(approx(measured.sliceLeft, 7.0f) && approx(measured.sliceRight, 17.0f),
        "and the slice spans the ink, a pixel past each side of the cell");
    CHECK_MSG(measured.ruleCount == 1, "one rule under the one underlined cell");
    CHECK_MSG(approx(measured.ruleLeft, 7.0f), "the rule starts at the slice's first inked column");
    CHECK_MSG(approx(measured.ruleRight, 17.0f), "and ends past its last one");
}

// A producer that takes the ink from the outline box, as the CoreText arm does,
// draws a pixel of fringe past it so an antialiased edge spilling over the box
// is not clipped. The fringe widens the slice and not the ink: the rule ends
// where the letter ends, exactly as on the arm that measured, and not a pixel
// past it at each side.
TEST(anUnderlineUnderAStripSliceEndsAtTheInkAndNotAtTheFringe)
{
    const StripSliceRule outlined = underlinedStripSlice(1);
    CHECK_MSG(outlined.isDrawn, "the cell draws its slice of the strip");
    CHECK_MSG(approx(outlined.sliceLeft, 6.0f) && approx(outlined.sliceRight, 18.0f),
        "and the slice carries the fringe, a pixel past the ink at each side");
    CHECK_MSG(outlined.ruleCount == 1, "one rule under the one underlined cell");
    CHECK_MSG(approx(outlined.ruleLeft, 7.0f), "the rule starts at the ink, inside the fringe");
    CHECK_MSG(approx(outlined.ruleRight, 17.0f), "and ends at the ink, inside the fringe");
}

// A doubled row of a proportional grid face, on a backend with no double
// sheet that cuts sized windows: the row is laid out as letters all the same.
// Each letter's cell stands at its pen with background alone, and the letter
// is drawn as the windows of its doubled master on the mono stream, stepped
// from the pen by the cell box, the first and last reaching the margin. A
// backend with no windows to cut keeps such a row on the pitch.
namespace {
struct ProportionalPrimaryShaper {
    static int shape(void*, std::span<const std::uint32_t> codePoints, FontFace, std::uint32_t, Ubiquitty::ProportionalShapedGlyph* out, int capacity)
    {
        if (static_cast<int>(codePoints.size()) > capacity)
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            out[index].glyph = codePoints[index];
            out[index].advanceInPixels = codePoints[index] == U'W' ? 12.0f : 4.0f;
            out[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
};

struct DoubledLetterScene {
    static constexpr int columns = 4;
    static constexpr int rows = 2;
    std::vector<CellInstance> cells { static_cast<std::size_t>(columns) * rows + 8 };
    std::vector<GlyphInstance> glyphs { 32 };
    InstanceCounts counts {};

    [[nodiscard]] const CellInstance* cellAt(float row, float x) const
    {
        for (std::size_t index = 0; index < counts.cells; ++index) {
            if (approx(cells[index].gridPosition.y, row) && approx(cells[index].gridPosition.x, x))
                return &cells[index];
        }
        return nullptr;
    }
};

/// Whether the backend under test cuts sized windows of a doubled master.
enum class DoubledMasterWindows : std::uint8_t {
    Cut,
    Absent,
};

DoubledLetterScene buildDoubledLetterScene(DoubledMasterWindows masterWindows)
{
    DoubledLetterScene scene;
    CellFixture fx;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(DoubledLetterScene::columns) * DoubledLetterScene::rows, fx.blank);
    for (int row = 0; row < DoubledLetterScene::rows; ++row) {
        grid[static_cast<std::size_t>(row) * DoubledLetterScene::columns].codePoint = U'W';
        grid[static_cast<std::size_t>(row) * DoubledLetterScene::columns + 1].codePoint = U'i';
    }
    const std::uint8_t lineAttributes[DoubledLetterScene::rows] = { Ubiquitty::lineAttributeNormal, Ubiquitty::lineAttributeDoubleWidth };

    BuildInputs in;
    in.cells = grid.data();
    in.rows = DoubledLetterScene::rows;
    in.columns = DoubledLetterScene::columns;
    in.palette = &fx.pal;
    in.lineAttributes = lineAttributes;
    in.subpixelGlyphs = true;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;
    in.glyphMarginInPixels = { 1.0f, 1.0f };

    std::vector<DecorationInstance> decos(static_cast<std::size_t>(DoubledLetterScene::columns) * DoubledLetterScene::rows * 8);
    InstanceBuffers buffers { scene.cells.data(), scene.cells.size(), decos.data(), decos.size() };
    buffers.glyphs = scene.glyphs.data();
    buffers.glyphCapacity = scene.glyphs.size();
    CacheBackend backend;
    GlyphBackend b = backend.backend();
    b.primaryFacePitch = FontMetrics::FacePitch::Proportional;
    b.shapePrimaryRun = ProportionalPrimaryShaper::shape;
    const SizedGlyphEntry windows = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
        return AtlasGlyph { { 10.0f + static_cast<float>(tile.tile), 10.0f + static_cast<float>(tile.band) },
            { 11.0f + static_cast<float>(tile.tile), 11.0f + static_cast<float>(tile.band) }, true };
    };
    b.sizedGlyph = masterWindows == DoubledMasterWindows::Cut ? windows : nullptr;
    scene.counts = buildCellInstances(in, b, buffers);
    return scene;
}
}

TEST(aDoubledLetterRowDrawsEachLetterAsMasterWindowsAtItsPen)
{
    const DoubledLetterScene scene = buildDoubledLetterScene(DoubledMasterWindows::Cut);

    // The plain row: W advances a cell and a half, i half a cell after it.
    const CellInstance* plainFirst = scene.cellAt(0.0f, 0.0f);
    const CellInstance* plainSecond = scene.cellAt(0.0f, 1.5f);
    CHECK_MSG(plainFirst && approx(plainFirst->widthCells, 1.5f), "the plain row's W stands at the origin, a cell and a half wide");
    CHECK_MSG(plainSecond && approx(plainSecond->widthCells, 0.5f), "and its i at the W's advance, half a cell wide");

    // The doubled row: the same letters at twice their advances, each cell
    // carrying background alone.
    const CellInstance* doubledFirst = scene.cellAt(1.0f, 0.0f);
    const CellInstance* doubledSecond = scene.cellAt(1.0f, 3.0f);
    CHECK_MSG(doubledFirst && approx(doubledFirst->widthCells, 3.0f), "the doubled row's W stands at the origin, three cells wide");
    CHECK_MSG(doubledSecond && approx(doubledSecond->widthCells, 1.0f), "and its i at twice the W's advance, a cell wide");
    CHECK_MSG(doubledFirst && approx(doubledFirst->atlasTopLeft.x, doubledFirst->atlasBottomRight.x), "the doubled W's cell carries no glyph");
    CHECK_MSG(doubledSecond && approx(doubledSecond->atlasTopLeft.x, doubledSecond->atlasBottomRight.x), "nor does the doubled i's");

    // Three windows a letter on the doubled row: the cell box of 6 steps from
    // the pen, the first reaching a margin left and the last a margin right,
    // which is where the master's far edge lies. Found by their row, since the
    // plain row's letters take the subpixel stream too.
    std::vector<const GlyphInstance*> doubled;
    for (std::size_t index = 0; index < scene.counts.glyphs && index < scene.glyphs.size(); ++index) {
        if (scene.glyphs[index].originInPixels.y > 8.0f)
            doubled.push_back(&scene.glyphs[index]);
    }
    const std::string count = "the doubled row emits " + std::to_string(doubled.size()) + " window quad(s), wanting 6";
    CHECK_MSG(doubled.size() == 6, count.c_str());
    if (doubled.size() != 6)
        return;
    const float pens[2] = { 0.0f, 24.0f };
    for (int letter = 0; letter < 2; ++letter) {
        for (int w = 0; w < 3; ++w) {
            const GlyphInstance& gi = *doubled[static_cast<std::size_t>(letter * 3 + w)];
            const float reachLeft = w == 0 ? 1.0f : 0.0f;
            const float reachRight = w == 2 ? 1.0f : 0.0f;
            const std::string where = "letter " + std::to_string(letter) + " window " + std::to_string(w) + " stands at "
                + std::to_string(gi.originInPixels.x) + ", " + std::to_string(gi.sizeInPixels.x) + " wide";
            CHECK_MSG(approx(gi.originInPixels.x, pens[letter] + 6.0f * static_cast<float>(w) - reachLeft), where.c_str());
            CHECK_MSG(approx(gi.sizeInPixels.x, 6.0f + reachLeft + reachRight), where.c_str());
            CHECK_MSG(approx(gi.originInPixels.y, 15.0f) && approx(gi.sizeInPixels.y, 16.0f), "a DECDWL window spans the slot's height, margins reached");
            // Window w's own rect, the margin's texels reached beside it.
            CHECK_MSG(approx(gi.atlasTopLeft.x, 10.0f + static_cast<float>(w) - reachLeft), where.c_str());
            CHECK_MSG(approx(gi.atlasBottomRight.x, 10.0f + static_cast<float>(w) + 6.0f + reachRight), where.c_str());
            CHECK_MSG(approx(gi.atlasTopLeft.y, 9.0f) && approx(gi.atlasBottomRight.y, 12.0f), where.c_str());
        }
    }
}

TEST(aDoubledLetterRowKeepsThePitchWithoutSizedWindows)
{
    const DoubledLetterScene scene = buildDoubledLetterScene(DoubledMasterWindows::Absent);
    const CellInstance* plainSecond = scene.cellAt(0.0f, 1.5f);
    CHECK_MSG(plainSecond && approx(plainSecond->widthCells, 0.5f), "the plain row is still laid out as letters");
    const CellInstance* doubledFirst = scene.cellAt(1.0f, 0.0f);
    const CellInstance* doubledSecond = scene.cellAt(1.0f, 2.0f);
    CHECK_MSG(doubledFirst && approx(doubledFirst->widthCells, 2.0f), "the doubled row's W keeps its doubled cell");
    CHECK_MSG(doubledSecond && approx(doubledSecond->widthCells, 2.0f), "and its i stands at the next one");
    std::size_t doubled = 0;
    for (std::size_t index = 0; index < scene.counts.glyphs && index < scene.glyphs.size(); ++index) {
        if (scene.glyphs[index].originInPixels.y > 8.0f)
            ++doubled;
    }
    CHECK_MSG(doubled == 0, "no window quad is emitted for a row kept on the pitch");
}

// An explicit width widens the block, not the glyph: `s=7:w=7` claims 49
// columns a band, and the glyph is seven of them. Counting windows across the
// block asked for 49, more than a band is cut into, and sent the block to the
// magnified crop, which drew it soft on the cell path and shredded on the
// subpixel stream. Counted across the glyph, every band is seven sharp
// windows, each sampling its own band and tile, and the spare columns draw
// nothing.
namespace {
struct ExplicitWidthBlock {
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    static constexpr int scale = 7;
    static constexpr int widthCells = 7;
    static constexpr int claim = scale * widthCells;
    static constexpr int columns = 60;
    std::vector<Ubiquitty::Cell> grid;

    ExplicitWidthBlock()
        : grid(static_cast<std::size_t>(columns) * scale, fx.blank)
    {
        const unsigned id = table.intern(Ubiquitty::TextSizeSpec { scale, widthCells });
        for (int band = 0; band < scale; ++band) {
            for (int c = 0; c < claim; ++c) {
                Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + c];
                Ubiquitty::Pen pen = cell.pen;
                pen.attributes.add(c == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, static_cast<uint8_t>(band));
                Ubiquitty::cellSetPen(cell, pen);
                if (c == 0)
                    cell.codePoint = 'X';
            }
        }
    }

    // Which instance stream draws the block's glyph.
    enum class Stream : std::uint8_t {
        Cell,
        Subpixel,
    };

    BuildInputs inputs(Stream stream)
    {
        BuildInputs in;
        in.cells = grid.data();
        in.rows = scale;
        in.columns = columns;
        in.palette = &fx.pal;
        in.textSizeTable = &table;
        in.subpixelGlyphs = stream == Stream::Subpixel;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphSlotWidthInPixels = 8.0f;
        in.glyphSlotHeightInPixels = 16.0f;
        return in;
    }

    // A window's rect names its band and tile, so a quad shows which it sampled.
    static GlyphBackend windowsNamingTheirTile(CacheBackend& backend)
    {
        GlyphBackend b = backend.backend();
        b.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
            return AtlasGlyph { { static_cast<float>(100 + tile.tile), static_cast<float>(100 + tile.band) },
                { static_cast<float>(100 + tile.tile) + 1.0f, static_cast<float>(100 + tile.band) + 1.0f }, true };
        };
        return b;
    }
};
}

TEST(testAnExplicitWidthBlockDrawsItsGlyphInSharpWindowsOnTheSubpixelStream)
{
    ExplicitWidthBlock block;
    BuildInputs in = block.inputs(ExplicitWidthBlock::Stream::Subpixel);
    std::vector<CellInstance> cells(static_cast<std::size_t>(ExplicitWidthBlock::columns) * ExplicitWidthBlock::scale * 2);
    std::vector<DecorationInstance> decos(cells.size() * 4);
    std::vector<GlyphInstance> glyphs(256);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    CacheBackend backend;
    const InstanceCounts counts = buildCellInstances(in, ExplicitWidthBlock::windowsNamingTheirTile(backend), buffers);

    const std::string count = "seven bands of seven windows: " + std::to_string(counts.glyphs) + " glyph quads";
    CHECK_MSG(counts.glyphs == 49, count.c_str());
    if (counts.glyphs != 49)
        return;
    for (std::size_t i = 0; i < 49; ++i) {
        const GlyphInstance& g = glyphs[i];
        const int band = static_cast<int>(i) / 7;
        const int tile = static_cast<int>(i) % 7;
        const std::string what = "quad " + std::to_string(i) + " samples band " + std::to_string(band) + " tile " + std::to_string(tile);
        CHECK_MSG(approx(g.atlasTopLeft.x, 100.0f + static_cast<float>(tile)) && approx(g.atlasTopLeft.y, 100.0f + static_cast<float>(band)), what.c_str());
        CHECK_MSG(approx(g.originInPixels.y, 16.0f * static_cast<float>(band)), "each band draws into its own row");
        CHECK_MSG(g.originInPixels.x < 8.0f * 7.0f, "no window stands past the glyph's seven columns");
    }
}

TEST(testAnExplicitWidthBlockDrawsItsGlyphInSharpWindowsOnTheCellPath)
{
    ExplicitWidthBlock block;
    BuildInputs in = block.inputs(ExplicitWidthBlock::Stream::Cell);
    std::vector<CellInstance> cells(static_cast<std::size_t>(ExplicitWidthBlock::columns) * ExplicitWidthBlock::scale * 2);
    std::vector<DecorationInstance> decos(cells.size() * 4);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    CacheBackend backend;
    const InstanceCounts counts = buildCellInstances(in, ExplicitWidthBlock::windowsNamingTheirTile(backend), buffers);

    int sharp = 0;
    int inkedPastTheGlyph = 0;
    for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i) {
        const CellInstance& c = cells[i];
        if (!(c.atlasBottomRight.x > c.atlasTopLeft.x))
            continue;
        const int column = static_cast<int>(std::lround(c.gridPosition.x));
        const int band = static_cast<int>(std::lround(c.gridPosition.y));
        if (column >= 7) {
            ++inkedPastTheGlyph;
            continue;
        }
        if (approx(c.atlasTopLeft.x, 100.0f + static_cast<float>(column)) && approx(c.atlasTopLeft.y, 100.0f + static_cast<float>(band)))
            ++sharp;
    }
    const std::string count = "every glyph column of every band samples its own window: " + std::to_string(sharp) + " of 49";
    CHECK_MSG(sharp == 49, count.c_str());
    CHECK_MSG(inkedPastTheGlyph == 0, "the explicit width's spare columns draw nothing");
}

namespace {
std::vector<SizedWindowStep> askedSteps;
}

// A window steps as far as the quad it lands on: a glyph-stream quad stands at
// the face's advance from the one before, a cell on the cell path at the grid's
// pitch. The builder names which in the tile it asks for, so every backend cuts
// the window the quad will show instead of one it would stretch.
TEST(testASizedWindowAsksToStepAsFarAsTheQuadItLandsOn)
{
    ExplicitWidthBlock block;
    const auto asked = [&](ExplicitWidthBlock::Stream stream) {
        askedSteps.clear();
        BuildInputs in = block.inputs(stream);
        std::vector<CellInstance> cells(static_cast<std::size_t>(ExplicitWidthBlock::columns) * ExplicitWidthBlock::scale * 2);
        std::vector<DecorationInstance> decos(cells.size() * 4);
        std::vector<GlyphInstance> glyphs(256);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        buffers.glyphs = glyphs.data();
        buffers.glyphCapacity = glyphs.size();
        CacheBackend backend;
        GlyphBackend recording = backend.backend();
        recording.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
            askedSteps.push_back(tile.step);
            return AtlasGlyph { { static_cast<float>(100 + tile.tile), static_cast<float>(100 + tile.band) },
                { static_cast<float>(100 + tile.tile) + 1.0f, static_cast<float>(100 + tile.band) + 1.0f }, true };
        };
        (void)buildCellInstances(in, recording, buffers);
        return askedSteps;
    };
    const std::vector<SizedWindowStep> stream = asked(ExplicitWidthBlock::Stream::Subpixel);
    const std::vector<SizedWindowStep> cell = asked(ExplicitWidthBlock::Stream::Cell);
    CHECK(!stream.empty());
    CHECK(!cell.empty());
    CHECK(std::ranges::all_of(stream, [](SizedWindowStep step) { return step == SizedWindowStep::FaceAdvance; }));
    CHECK(std::ranges::all_of(cell, [](SizedWindowStep step) { return step == SizedWindowStep::CellBox; }));
}

// A slot's cell box is the font's line height rounded up and the grid's rows
// are the line itself, so a band drawn at the box's height reaches past its row
// into the next band's, and the two blend the rows they share twice: a hairline
// across the glyph at every join, past its ink. Scale seven on a 31-row box
// whose line is 30.27 rows, over 30-pixel rows: each band's quad ends where the
// next begins, and every pixel row of the block shows the same share of a texel
// row, so the bands partition the glyph.
TEST(testSizedBandsAbutWhereTheCellBoxIsTallerThanTheLine)
{
    ExplicitWidthBlock block;
    BuildInputs in = block.inputs(ExplicitWidthBlock::Stream::Subpixel);
    constexpr float line = 30.27f;
    constexpr float box = 31.0f;
    constexpr float margin = 4.0f;
    in.metrics.cellWidth = 16.0f;
    in.metrics.cellHeight = 30.0f;
    in.glyphReferenceAdvanceInPixels = 16.0f;
    in.glyphReferenceLineHeightInPixels = line;
    in.glyphSlotWidthInPixels = 16.0f + 2.0f * margin;
    in.glyphSlotHeightInPixels = box + 2.0f * margin;
    in.glyphMarginInPixels = { margin, margin };
    std::vector<CellInstance> cells(static_cast<std::size_t>(ExplicitWidthBlock::columns) * ExplicitWidthBlock::scale * 2);
    std::vector<DecorationInstance> decos(cells.size() * 4);
    std::vector<GlyphInstance> glyphs(256);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    CacheBackend backend;
    GlyphBackend windows = backend.backend();
    windows.sizedGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
        // The cell box of the slot a window lives in, margins around it.
        const float top = 1000.0f * static_cast<float>(tile.band) + 4.0f;
        const float left = 1000.0f * static_cast<float>(tile.tile) + 4.0f;
        return AtlasGlyph { { left, top }, { left + 16.0f, top + 31.0f }, true };
    };
    const InstanceCounts counts = buildCellInstances(in, windows, buffers);
    REQUIRE(counts.glyphs == 49);

    const float pixelsPerTexelRow = in.metrics.cellHeight / line;
    for (std::size_t band = 0; band < ExplicitWidthBlock::scale; ++band) {
        const GlyphInstance& g = glyphs[band * 7];
        const bool isFirst = band == 0;
        const bool isLast = band + 1 == ExplicitWidthBlock::scale;
        const float top = g.originInPixels.y + (isFirst ? margin * pixelsPerTexelRow : 0.0f);
        const float bottom = g.originInPixels.y + g.sizeInPixels.y - (isLast ? margin * pixelsPerTexelRow : 0.0f);
        const std::string where = "band " + std::to_string(band) + " draws rows " + std::to_string(top) + " to " + std::to_string(bottom);
        CHECK_MSG(approx(top, 30.0f * static_cast<float>(band)), where.c_str());
        CHECK_MSG(approx(bottom, 30.0f * static_cast<float>(band + 1)), where.c_str());
        const float texelRows = g.atlasBottomRight.y - g.atlasTopLeft.y;
        CHECK_MSG(approx(texelRows * pixelsPerTexelRow, g.sizeInPixels.y), "a band's texel rows map to its quad at the one scale every band shares");
    }
}

namespace {

// A sized-window sheet of `capacity` slots behind the ordinary cache backend:
// a window is a slot keyed by its letter, band and tile, faulted on first ask
// and refused once every slot is live this frame, as GlyphSlotCache refuses.
// Each answer encodes its window, (100 + tile, 100 + band), so a quad that
// samples it is known to be sharp and which window it is.
struct BudgetedSizedSheet : CacheBackend {
    explicit BudgetedSizedSheet(int capacity)
        : sizedCache(capacity, recommendedHashBuckets(capacity), 1)
    {
    }

    GlyphSlotCache sizedCache;

    GlyphBackend sizedBackend()
    {
        GlyphBackend b = backend();
        b.beginFrame = [](void* context) {
            auto& self = static_cast<BudgetedSizedSheet&>(*static_cast<CacheBackend*>(context));
            ++self.frames;
            self.monoCache.beginFrame();
            self.colorCache.beginFrame();
            self.sizedCache.beginFrame();
        };
        b.sizedGlyph = [](void* context, std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
            auto& self = static_cast<BudgetedSizedSheet&>(*static_cast<CacheBackend*>(context));
            const GlyphSlotCache::Key key = (static_cast<GlyphSlotCache::Key>(codePoint) << 16) | (static_cast<GlyphSlotCache::Key>(tile.band) << 8) | tile.tile;
            bool isFresh = false;
            if (self.sizedCache.slotForKey(key, &isFresh) < 0)
                return AtlasGlyph {};
            return AtlasGlyph { { static_cast<float>(100 + tile.tile), static_cast<float>(100 + tile.band) },
                { static_cast<float>(101 + tile.tile), static_cast<float>(101 + tile.band) }, true };
        };
        return b;
    }
};

// Two scale-3 letters side by side, A in columns 0-2 and B in 3-5.
std::vector<Ubiquitty::Cell> twoScaleThreeLetters(Ubiquitty::TextSizeTable& table, const Ubiquitty::Cell& blank)
{
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    constexpr int columns = 6;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * 3, blank);
    for (int letter = 0; letter < 2; ++letter) {
        for (int band = 0; band < 3; ++band) {
            for (int offset = 0; offset < 3; ++offset) {
                Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + static_cast<std::size_t>(letter * 3 + offset)];
                cell.codePoint = offset == 0 ? static_cast<std::uint32_t>('A' + letter) : 0u;
                Ubiquitty::Pen pen = cell.pen;
                pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, static_cast<uint8_t>(band));
                Ubiquitty::cellSetPen(cell, pen);
            }
        }
    }
    return grid;
}

// How many sharp window quads each band of each letter drew, [letter][band].
using SharpWindowsPerBand = std::array<std::array<int, 3>, 2>;

std::string describeBands(const SharpWindowsPerBand& sharp)
{
    std::string text;
    for (int letter = 0; letter < 2; ++letter) {
        text += std::string(text.empty() ? "" : "; ") + static_cast<char>('A' + letter) + ":";
        for (int band = 0; band < 3; ++band)
            text += " " + std::to_string(sharp[static_cast<std::size_t>(letter)][static_cast<std::size_t>(band)]);
    }
    return text;
}

} // namespace

// A sheet that fills between two rows of a frame must not leave a block half
// sharp. Two scale-3 letters need nine windows each and the sheet holds
// thirteen, so both cannot be sharp; walked a row at a time, each letter's
// first two bands fit and its third did not, which drew the upper bands sharp
// and the lowest stretched. A block is decided whole: every band of a letter
// draws its three windows, or none does.
TEST(testEveryBandOfASizedBlockTakesOnePathOnAFullSheet)
{
    for (const bool drawsSubpixelGlyphs : { true, false }) {
        CellFixture fx;
        Ubiquitty::TextSizeTable table;
        std::vector<Ubiquitty::Cell> grid = twoScaleThreeLetters(table, fx.blank);
        BuildInputs in;
        in.cells = grid.data();
        in.rows = 3;
        in.columns = 6;
        in.palette = &fx.pal;
        in.textSizeTable = &table;
        in.subpixelGlyphs = drawsSubpixelGlyphs;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.glyphReferenceAdvanceInPixels = 8.0f;
        in.glyphReferenceLineHeightInPixels = 16.0f;
        in.glyphSlotWidthInPixels = 8.0f;
        in.glyphSlotHeightInPixels = 16.0f;

        std::vector<CellInstance> cells(6 * 3 * 4);
        std::vector<DecorationInstance> decos(6 * 3 * 8);
        std::vector<GlyphInstance> glyphs(64);
        InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
        buffers.glyphs = glyphs.data();
        buffers.glyphCapacity = glyphs.size();
        BudgetedSizedSheet sheet(13);
        const InstanceCounts counts = buildCellInstances(in, sheet.sizedBackend(), buffers);

        SharpWindowsPerBand sharp {};
        const auto count = [&](Vec2 atlasTopLeft, float x, float y) {
            if (atlasTopLeft.x < 100.0f)
                return;
            const int letter = static_cast<int>(x) / 3;
            const int band = static_cast<int>(y);
            if (letter >= 0 && letter < 2 && band >= 0 && band < 3)
                ++sharp[static_cast<std::size_t>(letter)][static_cast<std::size_t>(band)];
        };
        if (drawsSubpixelGlyphs) {
            for (std::size_t i = 0; i < counts.glyphs && i < glyphs.size(); ++i)
                count(glyphs[i].atlasTopLeft, (glyphs[i].originInPixels.x + 1.0f) / 8.0f, (glyphs[i].originInPixels.y + 1.0f) / 16.0f);
        } else {
            for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i)
                count(cells[i].atlasTopLeft, cells[i].gridPosition.x, cells[i].gridPosition.y);
        }

        const std::string bands = std::string(drawsSubpixelGlyphs ? "subpixel" : "cell-baked") + " sharp windows per band, " + describeBands(sharp);
        int sharpLetters = 0;
        for (const auto& letter : sharp) {
            const bool isWhole = letter[0] == 3 && letter[1] == 3 && letter[2] == 3;
            const bool isNone = letter[0] == 0 && letter[1] == 0 && letter[2] == 0;
            CHECK_MSG(isWhole || isNone, bands.c_str());
            sharpLetters += isWhole ? 1 : 0;
        }
        // The sheet holds one letter's nine windows, so one letter is sharp.
        CHECK_MSG(sharpLetters == 1, bands.c_str());
    }
}

namespace {

// The ordinary cache backend with its sized windows on the mono sheet the
// builder binds, the way the SoftwareRenderer and Vulkan keep them: windows
// and 1x glyphs compete for the same 256 slots.
struct SharedMonoSizedSheet : CacheBackend {
    GlyphBackend sharedBackend()
    {
        GlyphBackend b = backend();
        b.sizedGlyph = [](void* context, std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
            auto& self = *static_cast<CacheBackend*>(context);
            const GlyphSlotCache::Key key = (GlyphSlotCache::Key { 1 } << 40) | (static_cast<GlyphSlotCache::Key>(codePoint) << 16) | (static_cast<GlyphSlotCache::Key>(tile.band) << 8) | tile.tile;
            bool isFresh = false;
            if (self.monoCache.slotForKey(key, &isFresh) < 0)
                return AtlasGlyph {};
            // Past every slot of the 16x16 sheet, so a window is told from a 1x glyph.
            return AtlasGlyph { { static_cast<float>(1000 + tile.tile), static_cast<float>(1000 + tile.band) },
                { static_cast<float>(1001 + tile.tile), static_cast<float>(1001 + tile.band) }, true };
        };
        return b;
    }
};

} // namespace

// A block that does not fit gives back the slots its probe took. Thirty s=3
// letters need 270 windows and thirty 1x glyphs on a 256-slot sheet: the
// first twenty-five fit whole, ten slots each, and the twenty-sixth is
// refused five windows in. Kept, those five fill the sheet and the four
// letters after it lose their 1x glyph too and draw nothing. Given back,
// every letter draws: sharp whole, or from its 1x glyph on every band.
TEST(testARefusedSizedBlockLeavesItsSlotsToTheCellsAfterIt)
{
    CellFixture fx;
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/1 });
    constexpr int letters = 30;
    constexpr int columns = letters * 3;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * 3, fx.blank);
    for (int letter = 0; letter < letters; ++letter) {
        for (int band = 0; band < 3; ++band) {
            for (int offset = 0; offset < 3; ++offset) {
                Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(band) * columns + static_cast<std::size_t>(letter * 3 + offset)];
                cell.codePoint = offset == 0 ? static_cast<std::uint32_t>(0x100 + letter) : 0u;
                Ubiquitty::Pen pen = cell.pen;
                pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&pen, static_cast<uint8_t>(band));
                Ubiquitty::cellSetPen(cell, pen);
            }
        }
    }
    BuildInputs in;
    in.cells = grid.data();
    in.rows = 3;
    in.columns = columns;
    in.palette = &fx.pal;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;

    std::vector<CellInstance> cells(static_cast<std::size_t>(columns) * 3 * 4);
    std::vector<DecorationInstance> decos(static_cast<std::size_t>(columns) * 3 * 8);
    InstanceBuffers buffers { cells.data(), cells.size(), decos.data(), decos.size() };
    SharedMonoSizedSheet sheet;
    const InstanceCounts counts = buildCellInstances(in, sheet.sharedBackend(), buffers);

    // Per letter and band: did any cell of it carry ink, and was it sharp.
    std::array<std::array<int, 3>, letters> inked {};
    std::array<std::array<int, 3>, letters> sharp {};
    for (std::size_t i = 0; i < counts.cells && i < cells.size(); ++i) {
        const CellInstance& cell = cells[i];
        if (cell.atlasBottomRight.x <= cell.atlasTopLeft.x)
            continue;
        const int letter = static_cast<int>(cell.gridPosition.x) / 3;
        const int band = static_cast<int>(cell.gridPosition.y);
        if (letter < 0 || letter >= letters || band < 0 || band >= 3)
            continue;
        ++inked[static_cast<std::size_t>(letter)][static_cast<std::size_t>(band)];
        sharp[static_cast<std::size_t>(letter)][static_cast<std::size_t>(band)] += cell.atlasTopLeft.x >= 1000.0f ? 1 : 0;
    }
    int blankLetters = 0;
    int sharpLetters = 0;
    for (int letter = 0; letter < letters; ++letter) {
        const auto& ink = inked[static_cast<std::size_t>(letter)];
        const auto& windows = sharp[static_cast<std::size_t>(letter)];
        blankLetters += ink[0] == 0 || ink[1] == 0 || ink[2] == 0 ? 1 : 0;
        const bool isWhole = windows[0] == 3 && windows[1] == 3 && windows[2] == 3;
        const bool isNone = windows[0] == 0 && windows[1] == 0 && windows[2] == 0;
        const std::string path = "letter " + std::to_string(letter) + " draws " + std::to_string(windows[0]) + "/" + std::to_string(windows[1]) + "/"
            + std::to_string(windows[2]) + " sharp windows a band";
        CHECK_MSG(isWhole || isNone, path.c_str());
        sharpLetters += isWhole ? 1 : 0;
    }
    const std::string blank = std::to_string(blankLetters) + " of " + std::to_string(letters) + " letters have a band with no ink, wanting none";
    CHECK_MSG(blankLetters == 0, blank.c_str());
    const std::string fitted = std::to_string(sharpLetters) + " letters drawn sharp, wanting some but not all";
    CHECK_MSG(sharpLetters > 0 && sharpLetters < letters, fitted.c_str());
}
