// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The refusal seam: a run the sheet grants draws as one strip, a run the sheet
// refuses falls back to the per-cell mono path — unhealed and unligated. The
// two outcomes are DIFFERENT PICTURES of identical cells, which is why a
// refusal is only acceptable under genuine exhaustion: a backend whose grants
// vary frame to frame flips text the user never touched. Live, that reads as
// ligatures and healed letterforms toggling several times a second while a
// spinner runs, settling at whichever state the last grant left.
//
// No other test exercises the refused branch: every fixture that shapes at all
// grants every run, so this file is what names what draws when
// `isLigatureCovered`'s strip.present gate is false.

#include "Harness.h"

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TermPalette.h"
#include "StrictAtlas.h"

#include <cstring>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int rows = 1;
constexpr int columns = 12;

/// A shaping backend with a grant knob: the granted shape mirrors a real
/// backend's answer, the refusal mirrors it exactly as LigatureSheet spells it
/// — the runs stay zero-length, because a refused candidate writes nothing.
struct GrantOrRefuseAtlas : UbiquittyTest::StrictAtlas {
    bool shouldGrantStrips = true;
    int shapedRuns = 0;

    void beginFrame() { }
    MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t)
    {
        AtlasGlyph glyph;
        glyph.present = true;
        glyph.maxUV = { 0.125f, 0.125f };
        return glyph;
    }

    void shapeLigatureRuns(std::span<const LigatureCell> row, float, float, GlyphPass, std::span<LigatureRun> runs)
    {
        for (LigatureRun& run : runs)
            run.length = 0;
        const int width = static_cast<int>(std::min(row.size(), runs.size()));
        identifyLigatureRuns(row.data(), width, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace, [&](const LigatureRunCandidate& candidate) {
            ++shapedRuns;
            if (!shouldGrantStrips)
                return; // the refusal: the candidate's entries stay zeroed
            LigatureRun run;
            run.strip.present = true;
            run.strip.minUV = { 0.0f, 0.0f };
            run.strip.maxUV = { 0.5f, 0.25f };
            run.usedWidthInAtlasPixels = 8.0f * static_cast<float>(candidate.length);
            run.slotWidthInAtlasPixels = 96.0f;
            run.startColumn = candidate.startColumn;
            run.length = candidate.length;
            for (int column = candidate.startColumn; column < candidate.startColumn + candidate.length; ++column)
                runs[static_cast<std::size_t>(column)] = run;
        });
    }
};

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    std::vector<GlyphInstance> ligatureOut { static_cast<std::size_t>(rows) * columns };
    BuildInputs in;
    InstanceBuffers out;
    GrantOrRefuseAtlas atlas;

    Scene()
    {
        palette.seedDefaults();
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;

        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
        out.decorations = decorationsOut.data();
        out.decorationCapacity = decorationsOut.size();
        out.ligatureGlyphs = ligatureOut.data();
        out.ligatureGlyphCapacity = ligatureOut.size();

        static const char text[columns + 1] = "abc == illm";
        for (int column = 0; column < columns; ++column)
            cells[static_cast<std::size_t>(column)].codePoint = static_cast<std::uint32_t>(text[column]);
    }

    InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }
};

} // namespace

// A granted run emits strip slices; the same cells refused emit none and take
// the mono path instead. Both facts in one test, because either alone is
// satisfiable by a backend that never shapes.
TEST(aRefusedRunFallsBackToTheMonoPath)
{
    Scene granted;
    const InstanceCounts one = granted.build();
    CHECK_MSG(granted.atlas.shapedRuns > 0, "the row shaped a run at all");
    CHECK_MSG(one.ligatureGlyphs > 0, "a granted run draws as a strip");

    Scene refused;
    refused.atlas.shouldGrantStrips = false;
    const InstanceCounts two = refused.build();
    CHECK_MSG(refused.atlas.shapedRuns > 0, "the refused row still reached the shaper");
    CHECK_MSG(two.ligatureGlyphs == 0, "a refused run draws no strip");
    CHECK_MSG(two.cells >= one.cells, "its cells fall back per cell rather than vanishing");
}

// The column rule two producers share: the ink ledger folds a glyph into its
// column through it, and the shaped-answer producer names the column it
// publishes through it. The boundaries are the whole content — a run is shaped
// with context on both flanks, so a cluster index counts from the first context
// entry while a column counts from the first drawn one, and an off-by-one here
// gives a drawn cell a neighbouring chunk's ink or hands a context cluster a
// quad in the margin the insets exist to keep it out of.
TEST(aClusterNamesADrawnColumnOrNoneAtAll)
{
    // Eight cells of context on the left, as ligatureContextCells gives.
    CHECK_MSG(shapedGlyphColumn(8, 8, 16) == 0, "the first drawn cluster is column zero");
    CHECK_MSG(shapedGlyphColumn(23, 8, 16) == 15, "and the last is the run's final column");
    CHECK_MSG(shapedGlyphColumn(7, 8, 16) == -1, "the cluster just before the run is context");
    CHECK_MSG(shapedGlyphColumn(24, 8, 16) == -1, "and the one just after it is too");

    // A run shaped with no context at all — the row's first cell — still counts
    // from zero rather than from the flank it does not have.
    CHECK(shapedGlyphColumn(0, 0, 4) == 0);
    CHECK(shapedGlyphColumn(3, 0, 4) == 3);
    CHECK(shapedGlyphColumn(4, 0, 4) == -1);

    // A zero-length run names no column, which is the case a bare subtraction
    // answers with a negative index rather than with a refusal.
    CHECK(shapedGlyphColumn(8, 8, 0) == -1);
}

// A frame with no ligature sheet reads no run, and the run span is why: it is a
// thread_local resized only by a frame that bound a sheet, so its length is the
// last such frame's column count and it outlives every frame after. A read that
// escapes the shaping flag therefore does not fail on its own merits — it fails
// or passes on what some earlier frame in the same thread happened to leave.
//
// Which makes the order here the subject rather than the setting. Sizing the
// span narrow first and drawing wide second is what puts a wider frame's columns
// past the end; run the other way round, or run this case after any wide sheet
// frame at all, and the same escaped read lands inside a stale entry, reports
// length zero, and pins nothing. That is not a hypothetical about the harness:
// without the narrow frame this case runs fifth behind four frames that size the
// span to the full width, and passes with the defect present and a checking
// library underneath.
//
// It is also the shipping sequence, not a contrivance. A window narrows while a
// row ligates and widens after it stops; the span keeps the narrow width and the
// wide row indexes past it.
//
// Under a library that checks, an escaped read aborts here. Under this build it
// reads a neighbour, so the counts carry the claim: a frame that binds no sheet
// draws every column per cell, slices no strip, and never reaches the shaper.
TEST(aFrameWithNoLigatureSheetDrawsPerCell)
{
    // Narrow and sheeted: this is what leaves the span shorter than the frame
    // that follows. Sizing is the whole purpose of this build.
    Scene narrow;
    narrow.in.columns = 4;
    const InstanceCounts sizing = narrow.build();
    CHECK_MSG(sizing.cells > 0, "the narrow frame drew, so it sized the run span to four");

    Scene wide;
    wide.out.ligatureGlyphs = nullptr;
    wide.out.ligatureGlyphCapacity = 0;
    const InstanceCounts counts = wide.build();
    CHECK_MSG(counts.ligatureGlyphs == 0, "no sheet, no strip slices");
    CHECK_MSG(counts.cells == columns, "and every column of the wider frame still draws its own cell");
    CHECK_MSG(wide.atlas.shapedRuns == 0, "the shaper is never reached without a sheet to fill");
}

// A producer that reports only strips leaves the shaped answer unclaimed, and
// the row renders from its strips alone.
//
// Two answers come out of one shaping pass — the strip, keyed by the run's
// text, and the glyph the shaper chose, keyed by the face's own identity — and
// every backend in the tree fills only the first. A default of claimed instead
// of unclaimed would send those backends' rows down a path holding glyph id
// zero, which is a real glyph in most faces rather than an absence.
TEST(aStripOnlyProducerClaimsNoShapedGlyph)
{
    Scene scene;
    const InstanceCounts counts = scene.build();
    CHECK_MSG(scene.atlas.shapedRuns > 0, "the row shaped a run at all");
    CHECK_MSG(counts.ligatureGlyphs > 0, "and drew it as a strip");

    const LigatureRun untouched;
    CHECK_MSG(!untouched.hasShapedGlyph, "a run says it carries no shaped glyph until a producer says otherwise");
    CHECK_MSG(untouched.shaped.glyphID == 0, "and the unclaimed answer names no glyph");
    CHECK_MSG(untouched.shaped.coveredCells == 1,
        "an answer describes a substitution unless the producer widens it: zero would read as a cell some "
        "other cluster covers, which is the one value that makes a consumer draw nothing");
}

// A claimed single-cell answer draws from its own slot and takes no strip
// slice, and a claimed multi-cell one does neither.
//
// The width is the whole subject. A shaped identity is fetched only where it
// covers one cell, and that bound is not a simplification to be relaxed when
// somebody has a spare afternoon: the entry's ink is the mono table's entry for
// the slot it lands in, which describes one cell and its margins. Widen it and
// the ink readers —
// the underline that follows ink past the cell box, the skip-ink band — ask the
// mono table for a glyph that is not in it, and answer zero rather than
// answering that they do not know. A rule drawn through a descender is what that
// looks like on screen.
//
// So the width bound is what keeps the ink readers answering from a table that
// holds the glyph they are asked about, and until an entry carries per-column
// ink this case is what says so. It fails the moment the bound moves, which is
// the point: the widening is a real one to want, and the thing to notice about
// it is that ink has to travel in the same answer.
TEST(aShapedIdentityIsFetchedOnlyForTheWidthItCanDescribe)
{
    // A producer that claims the answer, at a width the case sets.
    struct ClaimingAtlas : GrantOrRefuseAtlas {
        std::uint8_t claimedCells = 1;
        int identityFetches = 0;

        // The identity is slotted on the mono sheet, so the sheet is bound:
        // without a cache the builder has nowhere to key it and never asks.
        GlyphSlotCache monoCache { 64, recommendedHashBuckets(64), 8 };
        MonoAtlasBinding bindMonoAtlas()
        {
            MonoAtlasBinding binding;
            binding.cache = &monoCache;
            binding.geometry = { /*columns=*/8, /*slotWidthInPixels=*/10, /*slotHeightInPixels=*/16,
                /*glyphWidthInPixels=*/8, /*glyphMarginInPixels=*/ { 1, 0 } };
            binding.slotCapacity = 64;
            return binding;
        }

        bool rasteriseShapedGlyph(std::uint32_t, FontFace, std::uint32_t, int)
        {
            ++identityFetches;
            return true;
        }

        void shapeLigatureRuns(std::span<const LigatureCell> row, float penX, float penResidual, GlyphPass pass, std::span<LigatureRun> runs)
        {
            GrantOrRefuseAtlas::shapeLigatureRuns(row, penX, penResidual, pass, runs);
            for (LigatureRun& run : runs) {
                if (run.length == 0)
                    continue;
                run.hasShapedGlyph = true;
                run.shaped.glyphID = 42;
                run.shaped.coveredCells = claimedCells;
            }
        }
    };

    struct ClaimingScene : Scene {
        ClaimingAtlas atlas;
        InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }
    };

    ClaimingScene single;
    single.atlas.claimedCells = 1;
    const InstanceCounts one = single.build();
    CHECK_MSG(single.atlas.identityFetches > 0, "a one-cell answer is fetched from the sheet keyed by its identity");
    CHECK_MSG(one.cells > 0, "and the row still draws");

    ClaimingScene wide;
    wide.atlas.claimedCells = 2;
    const InstanceCounts two = wide.build();
    CHECK_MSG(wide.atlas.identityFetches == 0,
        "a multi-cell answer is not fetched, because the entry carries no per-column ink for its consumers to read");
    CHECK_MSG(two.ligatureGlyphs > 0, "so the run keeps the strip it drew, which does carry owned ink");
}

// Two grants over identical cells are byte-identical: whatever the sheet's
// internal state, the instances a row builds are a function of the row. This
// is the determinism the flip violated, pinned where a unit test can hold it.
TEST(aGrantedRebuildIsByteIdentical)
{
    Scene first;
    const InstanceCounts one = first.build();
    Scene second;
    const InstanceCounts two = second.build();
    CHECK_MSG(one.cells == two.cells && one.ligatureGlyphs == two.ligatureGlyphs,
        "the second build emits the same instance counts");
    CHECK_MSG(std::memcmp(first.cellsOut.data(), second.cellsOut.data(), one.cells * sizeof(CellInstance)) == 0,
        "and the cell instances are byte-identical");
    CHECK_MSG(std::memcmp(first.ligatureOut.data(), second.ligatureOut.data(), one.ligatureGlyphs * sizeof(GlyphInstance)) == 0,
        "as are the strip slices");
}

UBIQUITTY_TEST_MAIN()
