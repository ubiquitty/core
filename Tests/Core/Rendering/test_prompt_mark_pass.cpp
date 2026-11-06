// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_prompt_mark_pass — the builder's OSC 133 bracket pass.
//
// The pass draws nothing under five separate conditions, and four of them are
// states a caller reaches by accident rather than on purpose: no marks, a
// fontSize left at zero, gutters too narrow, and a row whose command never ran.
// A scene that satisfies any of them agrees with a build that has no mark pass
// at all, which is why the render-parity run reports zero for this pass while
// proving nothing about it — its scenes carry no marks.
//
// So the negative cases come first and each one is reached from the same
// positive fixture, changing one field. A case that draws nothing for two
// reasons at once cannot tell which gate held.

#include "Harness.h"

#include "StrictAtlas.h"
#include <algorithm>
#include <cmath>

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Screen/MarkValues.h"
#include "Core/TermPalette.h"

#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

// Every glyph absent: this lane is about overlay geometry, and a cell that
// resolves no glyph still walks the same row loop the mark pass hangs off.
struct NullAtlas : UbiquittyTest::StrictAtlas {
    void beginFrame() { }
    MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }
};

constexpr int rows = 6;
constexpr int columns = 10;

// A grid inset in a wider surface, so both gutters have room. Grid is 80px
// wide inside a 120px viewport at x=20, leaving 20px each side.
struct Fixture {
    Ubiquitty::TermPalette palette;
    std::vector<Ubiquitty::Cell> cells { static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell };
    std::vector<std::uint8_t> marks = std::vector<std::uint8_t>(static_cast<std::size_t>(rows), 0);
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<OverlayInstance> overlaysOut { 64 };
    BuildInputs in;
    InstanceBuffers out;
    NullAtlas atlas;

    Fixture()
    {
        palette.seedDefaults();
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.deviceScale = 1.0f;
        in.metrics.atlasToDevicePixels = 1.0f;
        in.originInPixels = { 20.0f, 0.0f };
        in.viewportSizeInPixels = { 120.0f, static_cast<float>(rows) * 16.0f };
        in.cellHeightPoints = 16.0;
        in.promptMarks.rowMarks = marks.data();
        in.promptMarks.fontSize = 12;
        in.promptMarks.fontAscent = 10.0f;
        in.promptMarks.fontDescent = 3.0f;
        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
        out.overlays = overlaysOut.data();
        out.overlayCapacity = overlaysOut.size();
    }

    std::size_t overlayCount() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out).overlays; }

    // The bracket's drawn height, for a case comparing geometry rather than
    // presence. Zero when nothing was emitted.
    float bracketHeight()
    {
        const std::size_t count = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out).overlays;
        if (count == 0)
            return 0.0f;
        float lowest = overlaysOut[0].topLeftInPixels.y;
        float highest = overlaysOut[0].bottomRightInPixels.y;
        for (std::size_t i = 1; i < count; ++i) {
            lowest = std::min(lowest, overlaysOut[i].topLeftInPixels.y);
            highest = std::max(highest, overlaysOut[i].bottomRightInPixels.y);
        }
        return highest - lowest;
    }
};

} // namespace

TEST(promptMarkPass)
{
    // --- The positive case, established first so the rest can subtract from it.
    std::size_t drawn = 0;
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        drawn = f.overlayCount();
        CHECK_MSG(drawn > 0, "a row whose command ran draws brackets");
        CHECK_MSG(drawn % 3 == 0, "each bracket is a spine plus two stubs");
    }

    // --- No marks at all is how a caller says "alternate screen". ----------
    //
    // The builder is never told which screen it is drawing; marks belong to the
    // primary, and a backend showing the alternate passes none. If this ever
    // draws, every full-screen application grows brackets down both sides.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.rowMarks = nullptr;
        CHECK_MSG(f.overlayCount() == 0, "absent marks draw nothing");
    }

    // --- A row that carries no executed bit. -------------------------------
    //
    // The bit is promoted at the A->C transition, so a prompt sitting there
    // having run nothing is the common case and must stay bare.
    {
        Fixture f; // every mark left zero
        CHECK_MSG(f.overlayCount() == 0, "a prompt that ran no command draws nothing");
    }

    // --- fontSize left at zero. --------------------------------------------
    //
    // The obligation a caller most easily half-fills: marks wired, face metrics
    // forgotten. Drawing from a zero point size would place brackets against
    // geometry derived from nothing rather than refusing.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.fontSize = 0;
        CHECK_MSG(f.overlayCount() == 0, "no face metrics means no marks rather than degenerate ones");
    }

    // --- A surface drawn edge to edge has nowhere to put them. -------------
    //
    // This is what a frontend with no padding gets without asking: the geometry
    // resolver reports zero per row when neither gutter has width. Checked with
    // marks present and metrics valid, so the gutter is the only thing missing.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.originInPixels = { 0.0f, 0.0f };
        f.in.viewportSizeInPixels = { static_cast<float>(columns) * 8.0f, static_cast<float>(rows) * 16.0f };
        CHECK_MSG(f.overlayCount() == 0, "no gutter means no brackets");
    }

    // --- Marks scale with the row count, not with the grid. ----------------
    //
    // Two marked rows draw twice one row's worth. A pass that resolved geometry
    // per frame but emitted per cell would pass every case above and fail here.
    {
        Fixture f;
        f.marks[1] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.marks[4] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        CHECK_MSG(f.overlayCount() == drawn * 2, "two marked rows draw twice one row's brackets");
    }

    // --- Where the brackets land, not merely how many. ---------------------
    //
    // Every case above counts. A count survives any error that keeps the number
    // of quads: the grid extent and the cell extent are each a pair of one type,
    // and exchanging a pair's members moves every bracket while emitting exactly
    // as many. The fixture's cell is 8 wide and 16 tall for this case's sake —
    // a square one agrees with its own transposition.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        const std::size_t count = f.overlayCount();
        CHECK_MSG(count > 0, "the positional case needs something drawn");

        const float gridLeft = 20.0f;
        const float gridRight = gridLeft + static_cast<float>(columns) * 8.0f;
        const float rowTop = 2.0f * 16.0f;
        const float rowBottom = rowTop + 16.0f;

        bool didDrawLeftGutter = false;
        bool didDrawRightGutter = false;
        for (std::size_t i = 0; i < count; ++i) {
            const Vec2 topLeft = f.overlaysOut[i].topLeftInPixels;
            const Vec2 bottomRight = f.overlaysOut[i].bottomRightInPixels;
            const bool isInLeftGutter = topLeft.x >= 0.0f && bottomRight.x <= gridLeft;
            const bool isInRightGutter = topLeft.x >= gridRight && bottomRight.x <= 120.0f;
            CHECK_MSG(isInLeftGutter || isInRightGutter, "a bracket lies in a gutter, never over the grid");
            didDrawLeftGutter = didDrawLeftGutter || isInLeftGutter;
            didDrawRightGutter = didDrawRightGutter || isInRightGutter;
            CHECK_MSG(topLeft.y >= rowTop && bottomRight.y <= rowBottom,
                "a bracket stays within its marked row's band");
        }
        CHECK_MSG(didDrawLeftGutter && didDrawRightGutter, "both gutters are drawn, not one twice");
    }

    // --- A failed command is a different colour, not a different shape. ----
    {
        Fixture ok;
        ok.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        const std::size_t okCount = ok.overlayCount();
        Fixture bad;
        bad.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted | Ubiquitty::Screen::osc133MarkCmdFailed;
        CHECK_MSG(bad.overlayCount() == okCount, "a failed command draws the same bracket");
        bool hasColorDifference = false;
        for (std::size_t i = 0; i < okCount; ++i) {
            const Vec4& a = ok.overlaysOut[i].color;
            const Vec4& b = bad.overlaysOut[i].color;
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w)
                hasColorDifference = true;
        }
        CHECK_MSG(hasColorDifference, "and in a different colour");
    }

    // --- Capacity is a stop. -----------------------------------------------
    //
    // The count returned is what the caller would need, so a caller sizing its
    // buffer from it must still not have been written past.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        for (auto& overlay : f.overlaysOut)
            overlay.topLeftInPixels = { -1234.0f, -1234.0f };
        f.out.overlayCapacity = 1;
        f.overlayCount();
        CHECK_MSG(f.overlaysOut[1].topLeftInPixels.x == -1234.0f, "nothing is written past the capacity");
    }
}

// The overlay span a caller has to allocate, and the reason it is arithmetic in
// Core rather than a number in each frontend.
//
// An undersized span is silently correct-looking: the builder counts every
// instance it wanted and skips the ones past capacity, so the caller reads a
// count it clamps and the loss reaches the screen as a missing bracket. There is
// no error anywhere. A span of `rows + 1` is short by six per executed row once
// the mark pass runs, and a CPU path sized that way drops every bracket past the
// first row, so every renderer sizes its span with `maxOverlaysForRows`.
TEST(maxPromptMarkOverlays)
{
    Fixture f;
    for (std::size_t row = 0; row < f.marks.size(); ++row)
        f.marks[row] = Ubiquitty::Screen::osc133MarkCmdExecuted;

    // Room to overflow into: the count has to be what the builder WANTED, and a
    // span sized to the answer cannot distinguish "fits" from "was truncated".
    const std::size_t wanted = f.overlayCount();
    CHECK_MSG(wanted <= f.out.overlayCapacity, "the fixture's span must not itself be the limit here");

    CHECK_MSG(wanted <= maxOverlaysForRows(rows),
        "the ceiling must cover a grid whose every row draws in both gutters");

    // The negative that makes the case worth having. Without it a grid small
    // enough passes against any ceiling, a `rows + 1` span included.
    CHECK_MSG(wanted > static_cast<std::size_t>(rows) + 1,
        "this grid must exceed the old rows+1 span, or the case cannot show why it changed");

    // Nothing is drawn without marks, so the ceiling is a bound and not a cost:
    // a frontend that never sets promptMarks allocates more and writes the same.
    for (std::size_t row = 0; row < f.marks.size(); ++row)
        f.marks[row] = 0;
    CHECK_EQ(static_cast<int>(f.overlayCount()), 0);

    // Every term of the sum at once. The ceiling is 1 + rows * (1 + marks), and
    // marks alone exercise the last term only — a change to the cursor or the
    // per-row selection term leaves the case above passing. All three passes
    // write into one span, so the worst case is all three firing on one frame.
    Fixture busy;
    std::vector<SelectionRowSpan> everyRowSelected;
    for (int row = 0; row < rows; ++row) {
        busy.marks[static_cast<std::size_t>(row)] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        everyRowSelected.push_back(SelectionRowSpan { row, 0, columns - 1 });
    }
    busy.in.selectionSpans = everyRowSelected.data();
    busy.in.selectionSpanCount = static_cast<int>(everyRowSelected.size());
    busy.in.selectionColor = { 0.2f, 0.4f, 0.9f, 0.35f };
    busy.in.cursor.row = rows / 2;
    busy.in.cursor.column = 1;
    busy.in.cursor.mixA = 1.0f;
    busy.in.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    busy.in.cursor.focused = true;
    // Sized past the ceiling so what is measured is what the builder wanted.
    std::vector<OverlayInstance> roomy(maxOverlaysForRows(rows) * 4);
    busy.out.overlays = roomy.data();
    busy.out.overlayCapacity = roomy.size();

    const std::size_t busiest = busy.overlayCount();
    CHECK_MSG(busiest > wanted, "marks, selection and cursor together exceed marks alone");
    CHECK_MSG(busiest <= maxOverlaysForRows(rows),
        "the ceiling must cover every pass that writes into the span, not only the marks");
}

TEST(promptMarkAtlasScaleIsCarried)
{
    // The mark geometry scales by the ATLAS OVERSAMPLE — how many atlas pixels a
    // device pixel spans — and that number is carried rather than derived from
    // the cell metrics. `atlasToDevicePixels` measures the atlas against the
    // CELL, so DECCOLM-132 compressing the grid into a fixed window moves it
    // while the oversample has not changed, and a ratio taken from it comes out
    // wrong by the compression.
    //
    // Every other case in this file runs at scale 1, where a carried oversample
    // and one recovered from the metrics agree exactly. That is why this one
    // varies the scale and holds the metrics still: at the default the two are
    // indistinguishable, and the error appears only in the mode nobody renders
    // while checking.
    float heightAtOne = 0.0f;
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.atlasScale = 1.0f;
        heightAtOne = f.bracketHeight();
        CHECK_MSG(heightAtOne > 0.0f, "the brackets are drawn, so their height means something");
    }

    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        // The oversample doubles; the cell does not. A backend rasterising at 2x
        // and drawing into the same cell is the shipping macOS configuration.
        f.in.promptMarks.atlasScale = 2.0f;
        const float heightAtTwo = f.bracketHeight();
        CHECK_MSG(heightAtTwo > 0.0f, "brackets still draw at a doubled oversample");
        CHECK_MSG(heightAtTwo != heightAtOne,
            "the oversample reaches the geometry — equal heights mean it was ignored");
    }

    // Zero is one, for a backend rasterising at device resolution with no
    // oversample to declare. Without this a caller that never set the field
    // would collapse every bracket to nothing.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.atlasScale = 0.0f;
        CHECK_MSG(f.bracketHeight() == heightAtOne, "an undeclared oversample draws as one");
    }
}

// The mark centres on the face's own `[` when the face measured one, and on
// the metrics' midpoint when it did not. Faces disagree about where their
// bracket sits against that midpoint, some below it and some above, so a mark
// centred on the metrics reads high on one face and low on another; the
// bracket glyph is the one thing in a face designed to frame a line of its
// own text.
TEST(promptMarkCentresOnTheFacesBracket)
{
    auto centreOf = [](Fixture& f) {
        const std::size_t count = buildCellInstances(f.in, UbiquittyTest::glyphBackendFor(f.atlas), f.out).overlays;
        CHECK(count > 0);
        float lowest = f.overlaysOut[0].topLeftInPixels.y;
        float highest = f.overlaysOut[0].bottomRightInPixels.y;
        for (std::size_t i = 0; i < count; ++i) {
            lowest = std::min(lowest, f.overlaysOut[i].topLeftInPixels.y);
            highest = std::max(highest, f.overlaysOut[i].bottomRightInPixels.y);
        }
        return 0.5f * (lowest + highest);
    };
    // Unmeasured: the metrics' midpoint, with the optical nudge, stands.
    float unmeasured = 0.0f;
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        unmeasured = centreOf(f);
    }
    // A bracket whose box centres exactly on the metrics' midpoint: ascent 10
    // and descent 3 put the midpoint 3.5 above the baseline; a bracket from
    // 9 above to 2 below centres there too, so the mark moves only by the
    // nudge the measured path drops.
    float onTheMidpoint = 0.0f;
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.bracketInkTopInPixels = 9.0f;
        f.in.promptMarks.bracketInkBottomInPixels = 2.0f;
        onTheMidpoint = centreOf(f);
    }
    CHECK_MSG(std::fabs(onTheMidpoint - unmeasured) <= 1.0f,
        "a bracket on the metrics' midpoint leaves the mark within the nudge of where it was");
    // A bracket sitting low in its em, as Monaspace's does: the mark follows
    // it down — y grows downward, so lower is a larger y.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.bracketInkTopInPixels = 7.0f;
        f.in.promptMarks.bracketInkBottomInPixels = 4.0f;
        const float low = centreOf(f);
        CHECK_MSG(low > onTheMidpoint + 1.5f, "a low bracket pulls the mark down");
    }
    // And one sitting high, as Fira Code's does, pulls it up.
    {
        Fixture f;
        f.marks[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
        f.in.promptMarks.bracketInkTopInPixels = 11.0f;
        f.in.promptMarks.bracketInkBottomInPixels = 0.0f;
        const float high = centreOf(f);
        CHECK_MSG(high < onTheMidpoint - 1.5f, "a high bracket pulls the mark up");
    }
}

UBIQUITTY_TEST_MAIN()
