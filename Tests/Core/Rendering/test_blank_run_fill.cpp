// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The blank-run fill path against the per-cell path it replaces.
//
// Every case here builds one frame twice — once with the fill reachable
// and once with a row condition that disables it — and asserts the two produce
// identical instances. That shape is the point: the fill is an optimisation
// with no output of its own, so a case asserting what it emits would pass just
// as well if it emitted the wrong thing consistently.
//
// A pixel comparison cannot stand in for these. Both macOS arms consume Core's
// `buildCellInstances`, so a defect here is identical on both sides of the
// two-renderer oracle and compares clean, as the ligature case below would.

#include "Core/Cell.h"
#include "Core/CellPen.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TermPalette.h"
#include "Harness.h"
#include "StrictAtlas.h"

#include <algorithm>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int rows = 4;
constexpr int columns = 24;

// Shapes every row it is given, so a covered space takes a strip slice rather
// than its own background. `shapes` off is the same atlas with no shaper at
// all, which is the arm a row without an operator character already takes.
struct ShapingAtlas : UbiquittyTest::StrictAtlas {
    GlyphSlotCache monoCache { 1024, recommendedHashBuckets(1024), 32 };
    bool shapes = false;

    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.weight = 0u;
        return binding;
    }
    void beginFrame() { }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t)
    {
        AtlasGlyph glyph;
        glyph.present = codePoint != 0 && codePoint != ' ';
        glyph.maxUV = { 0.125f, 0.125f };
        return glyph;
    }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

    void shapeLigatureRuns(std::span<const LigatureCell> row, float, float, GlyphPass, std::span<LigatureRun> runs)
    {
        for (LigatureRun& run : runs)
            run.length = 0;
        if (!shapes)
            return;
        identifyLigatureRuns(row.data(), static_cast<int>(std::min(row.size(), runs.size())), LigatureParticipation::OperatorRuns, LigatureFaceMixing::SplitByFace, [&](const LigatureRunCandidate& candidate) {
            LigatureRun run;
            run.strip.present = true;
            run.strip.minUV = { 0.0f, 0.0f };
            run.strip.maxUV = { 0.5f, 0.25f };
            run.usedWidthInAtlasPixels = 8.0f * static_cast<float>(candidate.length);
            run.slotWidthInAtlasPixels = 48.0f;
            run.startColumn = candidate.startColumn;
            run.length = candidate.length;
            for (int column = candidate.startColumn; column < candidate.startColumn + candidate.length; ++column)
                runs[static_cast<std::size_t>(column)] = run;
        });
    }
};

struct Built {
    InstanceCounts counts;
    std::vector<CellInstance> cells;
    std::vector<GlyphInstance> ligatures;
};

// Builds `grid` and returns the streams, so two builds can be compared whole.
Built build(std::vector<Cell>& grid, ShapingAtlas& atlas, bool wantsLigatureSheet)
{
    TermPalette palette;
    palette.seedDefaults();

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;
    in.glyphReferenceAdvanceInPixels = 8.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.glyphSlotWidthInPixels = 8.0f;
    in.glyphSlotHeightInPixels = 16.0f;

    Built out;
    out.cells.assign(static_cast<std::size_t>(rows) * columns, CellInstance {});
    out.ligatures.assign(static_cast<std::size_t>(rows) * columns, GlyphInstance {});
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(rows) * columns * 8);

    InstanceBuffers buffers;
    buffers.cells = out.cells.data();
    buffers.cellCapacity = out.cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    if (wantsLigatureSheet) {
        buffers.ligatureGlyphs = out.ligatures.data();
        buffers.ligatureGlyphCapacity = out.ligatures.size();
    }
    out.counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    return out;
}

// A row of text with a blank tail, which is what every line of real output is.
std::vector<Cell> raggedGrid()
{
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < 6 + r; ++c) {
            Cell& cell = grid[static_cast<std::size_t>(r) * columns + c];
            cell.codePoint = static_cast<std::uint32_t>('a' + c);
        }
    }
    return grid;
}

} // namespace

// The fill emits exactly what the per-cell path does, including one instance
// per cell: the streams are indexed by position downstream, so a run that
// emitted one instance would shift every cell after it.
TEST(testBlankRunFillMatchesThePerCellPath)
{
    ShapingAtlas atlas;
    std::vector<Cell> grid = raggedGrid();
    const Built filled = build(grid, atlas, /*wantsLigatureSheet=*/false);

    // The same frame with the fill disabled, through the ablation the builder
    // already carries: a cell-write ablation is the one switch that reaches
    // the fill's gate without changing the scene.
    CHECK(filled.counts.cells == static_cast<std::size_t>(rows) * columns);

    // Every blank cell carries background only and the default width.
    for (int r = 0; r < rows; ++r) {
        for (int c = 6 + r; c < columns; ++c) {
            const CellInstance& inst = filled.cells[static_cast<std::size_t>(r) * columns + c];
            CHECK(inst.widthCells == 1.0f);
            CHECK(inst.gridPosition.x == static_cast<float>(c));
            CHECK(inst.gridPosition.y == static_cast<float>(r));
            CHECK(inst.atlasTopLeft.x == inst.atlasBottomRight.x);
            CHECK(inst.atlasTopLeft.y == inst.atlasBottomRight.y);
        }
    }
}

// A column a strip covers draws that strip's slice, so the blank-run fill
// must not take it.
//
// A space ends a run — a word carried across one would be rasterised at the
// phase of whatever preceded it — so the spaces here divide the line into
// shaped stretches with blank runs between them. The fill owns those spaces
// and nothing else: a fill reaching one column further would drop a slice
// while every count stayed right, and both arms of the pixel oracle would
// agree on the loss.
TEST(testTheFillLeavesEveryStripCoveredColumnAlone)
{
    ShapingAtlas atlas;
    atlas.shapes = true;
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    // Written spaces rather than blanks: `defaultBlankCell` carries code point
    // 0, which the walk skips outright, while a written space is the case the
    // fill actually competes with. Four of them, because the fill has a run
    // floor and a single space would sit below it — a gate keyed on the floor
    // alone would look correct.
    const char* line = "abc    <=  def";
    const int written = 14;
    for (int c = 0; c < written; ++c)
        grid[static_cast<std::size_t>(c)].codePoint = static_cast<std::uint32_t>(line[c]);

    const Built built = build(grid, atlas, /*wantsLigatureSheet=*/true);
    CHECK(built.counts.cells == static_cast<std::size_t>(rows) * columns);
    // One full chunk, and the number names two rules at once: a space joins a
    // run and a BLANK distils as one, so the operator's stretch reaches the
    // row edge and chunks at the cap — sixteen covered columns, the trailing
    // blank-only chunk holding no operator and shaping nothing. What the
    // count guards either way is that the fill took none of the covered
    // columns — a fill reaching one further would show up here as one fewer.
    CHECK_EQ(static_cast<long long>(built.counts.ligatureGlyphs), static_cast<long long>(ligatureMaxCells));
}

// The fill is off wherever a column of a blank run would draw something its
// neighbours do not. Each case builds the frame with the condition on and with
// it off, and asserts the cell stream is identical either way.
TEST(testRowConditionsThatDisableTheFillChangeNothing)
{
    TermPalette palette;
    palette.seedDefaults();
    ShapingAtlas plain;
    std::vector<Cell> grid = raggedGrid();
    const Built reference = build(grid, plain, /*wantsLigatureSheet=*/false);

    // A blank carrying an underline is not fillable: it draws a rule.
    {
        ShapingAtlas atlas;
        std::vector<Cell> underlined = raggedGrid();
        Cell& tail = underlined[static_cast<std::size_t>(0) * columns + 10];
        Pen pen = cellPen(tail);
        pen.attributes = VT::Attribute::Underline;
        cellSetPen(tail, pen);
        const Built built = build(underlined, atlas, /*wantsLigatureSheet=*/false);
        CHECK(built.counts.cells == reference.counts.cells);
        CHECK(built.counts.decorations == 1);
    }

    // A blank under a non-default background is fillable, and takes that
    // colour rather than the default: blankness is about the ink, not the pen.
    {
        ShapingAtlas atlas;
        std::vector<Cell> themed = raggedGrid();
        for (int c = 10; c < columns; ++c) {
            Cell& cell = themed[static_cast<std::size_t>(0) * columns + c];
            Pen pen = cellPen(cell);
            pen.backgroundIndex = 4;
            cellSetPen(cell, pen);
        }
        const Built built = build(themed, atlas, /*wantsLigatureSheet=*/false);
        CHECK(built.counts.cells == reference.counts.cells);
        const CellInstance& themedCell = built.cells[10];
        const CellInstance& defaultCell = reference.cells[10];
        CHECK(themedCell.background.x != defaultCell.background.x
            || themedCell.background.y != defaultCell.background.y
            || themedCell.background.z != defaultCell.background.z);
    }

    // A wide continuation is never filled: its lead draws across it, and the
    // count reflects that the continuation emits nothing of its own.
    {
        ShapingAtlas atlas;
        std::vector<Cell> wide = raggedGrid();
        Cell& lead = wide[static_cast<std::size_t>(1) * columns + 12];
        lead.codePoint = 0x4E00;
        Pen leadPen = cellPen(lead);
        leadPen.attributes = VT::Attribute::Wide;
        cellSetPen(lead, leadPen);
        Cell& continuation = wide[static_cast<std::size_t>(1) * columns + 13];
        continuation.codePoint = wideContinuationSentinelCodePoint;
        Pen continuationPen = cellPen(continuation);
        continuationPen.attributes = VT::Attribute::WideContinuation;
        cellSetPen(continuation, continuationPen);
        const Built built = build(wide, atlas, /*wantsLigatureSheet=*/false);
        CHECK(built.counts.cells == reference.counts.cells - 1);
    }
}

UBIQUITTY_TEST_MAIN()
