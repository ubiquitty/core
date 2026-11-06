// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_dirty_rows — a clean row emits nothing, and that is the whole hazard.
//
// `BuildInputs::dirtyRows` exists for a retained target: the macOS grid cache
// draws an unchanged row from the texture it kept, so the builder skipping that
// row costs nothing and saves the whole per-row walk. A backend compositing a
// fresh frame out of the returned spans alone would instead drop every row it
// skipped, and the picture would be missing text rather than wrong — which
// reads as a stall, not as a bug.
//
// So the assertions are about ABSENCE, which is the thing a count cannot show:
// a frame that skipped half its rows and a frame whose cells happened to be
// blank produce the same instance total. Each case pins which rows arrived, by
// grid position, not how many.

#include "Harness.h"
#include "StrictAtlas.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TermPalette.h"

#include <set>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

constexpr int rows = 6;
constexpr int columns = 8;

struct NullAtlas : UbiquittyTest::StrictAtlas {
    void beginFrame() { }
    MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }
};

// Every row carries text and an underline, so a skipped row is missing a cell
// and a decoration — a builder that honoured damage for one stream and not the
// other would pass a cells-only assertion.
struct Scene {
    Ubiquitty::TermPalette palette;
    std::vector<Ubiquitty::Cell> cells { static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell };
    std::vector<std::uint8_t> damage = std::vector<std::uint8_t>(static_cast<std::size_t>(rows), 1);
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    BuildInputs in;
    InstanceBuffers out;
    NullAtlas atlas;

    Scene()
    {
        palette.seedDefaults();
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < columns; ++c) {
                Ubiquitty::Cell& cell = cells[static_cast<std::size_t>(r) * columns + c];
                cell.codePoint = static_cast<std::uint32_t>('a' + c);
                Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
                pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
                Ubiquitty::cellSetPen(cell, pen);
            }
        }
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
    }

    InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }

    // Which grid rows actually produced a cell instance.
    [[nodiscard]] std::set<int> rowsEmitted(std::size_t count) const
    {
        std::set<int> seen;
        for (std::size_t i = 0; i < count && i < cellsOut.size(); ++i)
            seen.insert(static_cast<int>(cellsOut[i].gridPosition.y));
        return seen;
    }
};

} // namespace

TEST(dirtyRowsSkipCleanRows)
{
    // --- Null damage builds everything, which is the safe default. ----------
    Scene all;
    const InstanceCounts full = all.build();
    CHECK_MSG(all.in.dirtyRows == nullptr, "the default is null, not an all-dirty array");
    CHECK_MSG(full.cells == static_cast<std::size_t>(rows) * columns, "a null damage array builds every row");
    CHECK(full.decorations > 0);

    // --- All-dirty matches null exactly. ------------------------------------
    //
    // If these disagree the damage path is doing something beyond skipping, and
    // every later case would be measuring that instead.
    Scene marked;
    marked.in.dirtyRows = marked.damage.data();
    const InstanceCounts everyRowDirty = marked.build();
    CHECK_MSG(everyRowDirty.cells == full.cells, "an all-dirty array is the same frame as no array");
    CHECK_MSG(everyRowDirty.decorations == full.decorations, "including its decorations");

    // --- A clean row emits nothing, and the survivors are the dirty ones. ---
    //
    // Asserted by grid position rather than by count: a frame missing rows 1
    // and 3 and a frame whose rows 1 and 3 were blank have the same total, and
    // only the positions distinguish them.
    Scene partial;
    partial.damage[1] = 0;
    partial.damage[3] = 0;
    partial.in.dirtyRows = partial.damage.data();
    const InstanceCounts skipped = partial.build();
    const std::set<int> seen = partial.rowsEmitted(skipped.cells);
    CHECK_MSG(seen.count(1) == 0, "a clean row emits no cell at all");
    CHECK_MSG(seen.count(3) == 0, "and neither does the second one");
    CHECK_MSG(seen.count(0) == 1 && seen.count(2) == 1 && seen.count(4) == 1 && seen.count(5) == 1,
        "every dirty row still emits");
    CHECK_MSG(skipped.cells == static_cast<std::size_t>(rows - 2) * columns,
        "and the total is exactly the dirty rows' worth");

    // --- Decorations are skipped with their row, not independently. ---------
    //
    // The streams are written by separate code in the same row body. A skip
    // that reached the cell pass but not the decoration pass would leave rules
    // floating on rows whose text was never drawn.
    CHECK_MSG(skipped.decorations < full.decorations,
        "a skipped row takes its decorations with it");
    CHECK_MSG(skipped.decorations == full.decorations / rows * (rows - 2),
        "and takes exactly its own share");

    // --- Every row clean emits an empty frame rather than a full one. -------
    //
    // The degenerate end of the contract, and the one a caller most easily
    // reaches by accident: an unchanged screen. Building everything here would
    // silently discard what the damage array exists to carry.
    Scene none;
    for (auto& d : none.damage)
        d = 0;
    none.in.dirtyRows = none.damage.data();
    const InstanceCounts empty = none.build();
    CHECK_MSG(empty.cells == 0, "an unchanged screen builds no cells");
    CHECK_MSG(empty.decorations == 0, "and no decorations");
}

UBIQUITTY_TEST_MAIN()
