// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_ligature_cluster_conjunction — a column that is shaped and marked at
// once, and the rule that keeps it from being both.
//
// Two decisions meet on one cell. `ligatureCellEligible` asks whether a column
// may join a shaped run. The cluster read at the glyph-supply seam asks whether
// a cell carries marks, from `combiningLength()` and `clusterID()`.
//
// A run is drawn as one strip and each covered cell takes a slice of it instead
// of a glyph of its own — right, because the strip already contains the
// character. It is right for the whole cell only when the strip contains the
// whole cell, and `LigatureCell` carries a base code point and an opaque shape
// identity. Marks are in neither. A shaper handed a distilled row cannot draw an
// acute nobody told it about, so a column that was both shaped and marked lost
// its marks and rendered as a bare base.
//
// A real shaper forms such a run, not only this fixture. On the macOS renderer
// the acute of `e+U+0301` is drawn in `a b e+U+0301 c d`, where no ligature
// forms, and lost in `! = e+U+0301`, `e+U+0301 ! =` and `! = e+U+0301 = =`.
//
// The first is the control: same cell, same marks, same font, and the only
// variable is whether a ligature forms anywhere in the run. Position was not the
// trigger — what mattered was that a strip formed over the run the column
// belonged to. And since the predicate admits 0x20..0x7E, a run is a maximal
// span of printable ASCII, so a marked letter anywhere in a token like `x!=y` or
// in a path fragment sat inside one.
//
// The rule is that a marked column is ineligible, so it never enters shaping and
// the per-glyph path draws it whole. Run shape changes only in that the run
// splits at the marked column, and since ligation candidates are operator
// characters a marked letter is never interior to an operator sequence — so a
// split there cannot remove a ligature whose characters sit on one side of it.
//
// What these cases have to hold apart is the rule from its overreach: a
// predicate that answered false for everything would also keep every mark. So
// the unmarked columns are asserted still eligible in the same row, and the
// cluster the seam receives is asserted whole rather than merely present.

#include "Harness.h"

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TermPalette.h"
#include "StrictAtlas.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

constexpr int rows = 1;
constexpr int columns = 5;

// U+0301 COMBINING ACUTE ACCENT over an ASCII base. The base has to be ASCII or
// the column is ineligible for its code point and the rule under test is never
// the reason for the answer.
constexpr std::uint32_t combiningAcute = 0x0301u;

/// Whether the fixture's shaper reports a run over the row.
enum class Shaping : bool {
    Off = false,
    On = true
};

// Records what Core distilled and what it asked for, and reports a run over the
// columns Core offered when the scene says to.
//
// It shapes the eligible span rather than the whole row, because a backend
// cannot shape a column distilled to zero — it was handed no character for it.
// A fixture that covered every column regardless would be reporting a run no
// shaper could produce, and would answer about itself rather than about the
// builder.
struct RecordingLigatureAtlas : UbiquittyTest::StrictAtlas {
    std::vector<LigatureCell> distilled;
    std::vector<std::vector<std::uint32_t>> clusterSequences;
    int monoCalls = 0;
    Shaping shaping = Shaping::On;

    void beginFrame() { }
    MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t)
    {
        ++monoCalls;
        AtlasGlyph glyph;
        glyph.present = true;
        glyph.maxUV = { 0.125f, 0.125f };
        return glyph;
    }

    AtlasGlyph clusterGlyph(std::span<const std::uint32_t> codePoints, FontFace, std::uint32_t, std::uint32_t)
    {
        clusterSequences.emplace_back(codePoints.begin(), codePoints.end());
        AtlasGlyph glyph;
        glyph.present = true;
        glyph.maxUV = { 0.125f, 0.125f };
        return glyph;
    }

    void shapeLigatureRuns(std::span<const LigatureCell> row, float, float, GlyphPass, std::span<LigatureRun> runs)
    {
        distilled.assign(row.begin(), row.end());
        // The span may arrive holding the previous shaped row's runs; the
        // shaper owns the whole of it, as every real backend does.
        for (LigatureRun& run : runs)
            run.length = 0;
        if (shaping == Shaping::Off)
            return;

        // Core's own walk decides which columns a run covers, so the stub
        // tracks the run-splitting contract by construction: where a run
        // starts, where a shape boundary or refused column splits it, the
        // operator floor and the chunk cap are the one implementation every
        // real backend calls, and an eligibility change lands here without
        // anyone re-deriving it. What stays faked is what this tier does not
        // have — a rasterised strip; only coverage is asserted on.
        const int width = static_cast<int>(std::min(row.size(), runs.size()));
        identifyLigatureRuns(row.data(), width, LigatureParticipation::OperatorRuns, LigatureFaceMixing::SplitByFace, [&](const LigatureRunCandidate& candidate) {
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

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    std::vector<GlyphInstance> ligatureOut { static_cast<std::size_t>(rows) * columns };
    BuildInputs in;
    InstanceBuffers out;
    RecordingLigatureAtlas atlas;

    Scene()
    {
        palette.seedDefaults();
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;

        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
        out.decorations = decorationsOut.data();
        out.decorationCapacity = decorationsOut.size();
        // The shaping route is gated on the caller offering the stream:
        // `hasLigatureSheet` reads `out.ligatureGlyphs != nullptr`. Without it
        // the row is never distilled and the pairing cannot arise.
        out.ligatureGlyphs = ligatureOut.data();
        out.ligatureGlyphCapacity = ligatureOut.size();
    }

    /// Fills the row with operator characters and marks one of them.
    ///
    /// The row is all printable ASCII, which is the span a real run covers; the
    /// marked column takes an ASCII base so its code point is not the reason it
    /// is refused.
    ///
    /// - Parameter markedColumn: Where the combining mark sits.
    void placeMarkAt(int markedColumn)
    {
        static const char row[columns + 1] = "!==!=";
        for (int column = 0; column < columns; ++column)
            cells[static_cast<std::size_t>(column)].codePoint = static_cast<std::uint32_t>(row[column]);

        Cell& marked = cells[static_cast<std::size_t>(markedColumn)];
        marked.codePoint = 'e';
        marked.combining[0] = combiningAcute;
        marked.setCombiningLength(1);
    }

    InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }
};

/// Builds a scene with one marked column and reports how many clusters reached
/// the seam.
///
/// - Parameters:
///   - markedColumn: Where the combining mark sits.
///   - shaping: Whether the shaper reports a run over the row.
/// - Returns: The number of `clusterGlyph` calls the build made.
int clustersReachingTheSeam(int markedColumn, Shaping shaping)
{
    Scene scene;
    scene.atlas.shaping = shaping;
    scene.placeMarkAt(markedColumn);
    scene.build();
    return static_cast<int>(scene.atlas.clusterSequences.size());
}

} // namespace

// A marked column does not enter shaping, so the mark cannot be lost inside a
// strip that never held it.
//
// Zero is the refusal: no eligible code point can hold it, which is why the
// distilled row needs no second field to mean "skip me".
TEST(aMarkedColumnIsIneligibleForShaping)
{
    Scene scene;
    scene.placeMarkAt(2);
    scene.build();

    CHECK_EQ(static_cast<int>(scene.atlas.distilled.size()), columns);
    CHECK_EQ(static_cast<long long>(scene.atlas.distilled[2].codePoint), 0LL);
}

// The refusal is about the mark and not about the row.
//
// Without this, a predicate that answered false for every column would satisfy
// every other case here: no column shapes, so no mark is ever lost. The
// unmarked columns in the same row have to stay eligible, or the rule has eaten
// ligature shaping rather than corrected it.
TEST(theUnmarkedColumnsOfTheSameRowStillShape)
{
    Scene scene;
    scene.placeMarkAt(2);
    scene.build();

    CHECK_EQ(static_cast<long long>(scene.atlas.distilled[0].codePoint), static_cast<long long>('!'));
    CHECK_EQ(static_cast<long long>(scene.atlas.distilled[1].codePoint), static_cast<long long>('='));
    CHECK_EQ(static_cast<long long>(scene.atlas.distilled[3].codePoint), static_cast<long long>('!'));
    CHECK_EQ(static_cast<long long>(scene.atlas.distilled[4].codePoint), static_cast<long long>('='));
}

// The cluster reaches the seam wherever the marked column sits.
//
// All three positions rather than one, because the renderer measurement found
// the loss whether the ligature sat before the marked column, after it, or on
// both sides. A case at one position would leave "only the cell adjacent to a
// ligature is affected" standing, which is a smaller and wrong reading of what
// was reachable.
TEST(aMarkedColumnInsideAShapedRunKeepsItsClusterAtEveryPosition)
{
    CHECK_EQ(clustersReachingTheSeam(0, Shaping::On), 1); // the run's first column
    CHECK_EQ(clustersReachingTheSeam(2, Shaping::On), 1); // inside it
    CHECK_EQ(clustersReachingTheSeam(columns - 1, Shaping::On), 1); // its last
}

// The two arms agree, which is the shape of the rule holding.
//
// Whether a shaper offers a run over the row no longer changes what the marked
// cell hands the seam. A builder that reintroduced the pairing would separate
// these two, which is what makes running both worth more than running either.
TEST(shapingOnAndOffHandTheSeamTheSameCluster)
{
    CHECK_EQ(clustersReachingTheSeam(0, Shaping::Off), 1);
    CHECK_EQ(clustersReachingTheSeam(2, Shaping::Off), 1);
    CHECK_EQ(clustersReachingTheSeam(columns - 1, Shaping::Off), 1);
}

// The cluster is whole, so the cases above are about the marks arriving rather
// than about some cluster arriving.
TEST(theClusterHandedToTheSeamCarriesTheBaseAndTheMark)
{
    Scene scene;
    scene.placeMarkAt(2);
    scene.build();

    // REQUIRE rather than CHECK: the assertions below index the sequence, and a
    // regression that hands the seam nothing would abort the binary before the
    // other cases ran — a crash names one case where five verdicts were owed.
    REQUIRE_MSG(scene.atlas.clusterSequences.size() == 1, "the marked cell reached the seam exactly once");
    CHECK_EQ(static_cast<int>(scene.atlas.clusterSequences[0].size()), 2);
    CHECK_EQ(static_cast<long long>(scene.atlas.clusterSequences[0][0]), static_cast<long long>('e'));
    CHECK_EQ(static_cast<long long>(scene.atlas.clusterSequences[0][1]), static_cast<long long>(combiningAcute));
}

UBIQUITTY_TEST_MAIN()
