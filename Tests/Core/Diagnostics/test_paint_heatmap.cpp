// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Diagnostics/PaintHeatmap.cpp and the capture's
// heat pass — the policy every frontend's heatmap reads.
//
// Pins:
//   - enabling resyncs: counters clear and the first frame is a baseline,
//     not a whole-grid flash;
//   - only cells whose captured content changed bump; a wholesale redraw of
//     the same content bumps nothing;
//   - the key is the captured cell, so a pen change is a change;
//   - a grid that changes shape keeps the keys it shares;
//   - a window opening clears the counters, a refresh decays inside the live
//     grid, zeroes outside it and reports an extent that tracks the grid;
//   - the capture feeds the shadow once enabled and not before.

#include "Core/Cell.h"
#include "Core/Diagnostics/PaintHeatmap.h"
#include "Core/Diagnostics/PerfCounters.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <vector>

namespace {

using namespace Ubiquitty;
namespace Heat = Ubiquitty::Diagnostics::PaintHeatmap;
namespace PC = Ubiquitty::Diagnostics::PerfCounters;
using Ubiquitty::Diagnostics::PaintHeatShadow;

std::vector<Cell> gridOf(int rows, int columns, std::uint32_t codePoint)
{
    std::vector<Cell> cells(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns), Cell {});
    for (auto& cell : cells)
        cell.codePoint = codePoint;
    return cells;
}

int bumpedCellCount(int rows, int columns)
{
    int bumped = 0;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            if (Heat::cellCount(row, column) != 0)
                ++bumped;
        }
    }
    return bumped;
}

TEST(testTheDiagnosticsBuildIsTheTestBuild)
{
    CHECK_MSG(Diagnostics::isPerfDiagnosticsBuild(), "the host tests compile UBIQUITTY_PERF_DIAG in");
}

TEST(testTheFirstFrameAfterEnablingIsABaseline)
{
    PC::reset();
    PC::noteCellPaint(3, 3);
    PaintHeatShadow shadow;
    shadow.enable();
    CHECK_MSG(Heat::cellCount(3, 3) == 0, "enabling clears the counters");

    auto grid = gridOf(4, 10, 'a');
    shadow.noteCapturedGrid(grid.data(), 4, 10);
    CHECK_MSG(bumpedCellCount(4, 10) == 0, "the first frame records keys and bumps nothing");
}

TEST(testOnlyChangedCellsBump)
{
    PaintHeatShadow shadow;
    shadow.enable();
    auto grid = gridOf(4, 10, 'a');
    shadow.noteCapturedGrid(grid.data(), 4, 10);

    for (int redraw = 0; redraw < 10; ++redraw)
        shadow.noteCapturedGrid(grid.data(), 4, 10);
    CHECK_MSG(bumpedCellCount(4, 10) == 0, "redrawing the same content is not churn");

    grid[static_cast<std::size_t>(2 * 10 + 7)].codePoint = 'b';
    shadow.noteCapturedGrid(grid.data(), 4, 10);
    CHECK(Heat::cellCount(2, 7) == PC::PaintBump);
    CHECK_MSG(bumpedCellCount(4, 10) == 1, "one changed cell bumps one counter");

    shadow.noteCapturedGrid(grid.data(), 4, 10);
    CHECK_MSG(Heat::cellCount(2, 7) == PC::PaintBump, "a cell that changed once bumps once");
}

TEST(testThePenIsPartOfTheContent)
{
    PaintHeatShadow shadow;
    shadow.enable();
    auto grid = gridOf(2, 4, 'x');
    shadow.noteCapturedGrid(grid.data(), 2, 4);
    grid[1].pen.foregroundIndex = 3;
    shadow.noteCapturedGrid(grid.data(), 2, 4);
    CHECK_MSG(Heat::cellCount(0, 1) == PC::PaintBump, "a recoloured cell reads as changed");
    CHECK(PaintHeatShadow::contentKey(grid[0]) != PaintHeatShadow::contentKey(grid[1]));
    CHECK(PaintHeatShadow::contentKey(grid[0]) == PaintHeatShadow::contentKey(grid[2]));
}

TEST(testReenablingResyncsAStaleShadow)
{
    PaintHeatShadow shadow;
    shadow.enable();
    auto before = gridOf(3, 5, 'a');
    shadow.noteCapturedGrid(before.data(), 3, 5);
    shadow.disable();
    CHECK(!shadow.isEnabled());

    auto after = gridOf(3, 5, 'z');
    shadow.noteCapturedGrid(after.data(), 3, 5);
    CHECK_MSG(bumpedCellCount(3, 5) == 0, "a disabled shadow notes nothing");

    shadow.enable();
    shadow.noteCapturedGrid(after.data(), 3, 5);
    CHECK_MSG(bumpedCellCount(3, 5) == 0, "content that changed while nobody watched is the new baseline, not a flash");
}

TEST(testAGridChangingShapeKeepsTheKeysItShares)
{
    PaintHeatShadow shadow;
    shadow.enable();
    auto narrow = gridOf(2, 4, 'a');
    shadow.noteCapturedGrid(narrow.data(), 2, 4);

    auto wide = gridOf(2, 6, 'a');
    shadow.noteCapturedGrid(wide.data(), 2, 6);
    CHECK_MSG(Heat::cellCount(0, 0) == 0 && Heat::cellCount(1, 3) == 0, "the shared cells kept their keys");
    CHECK_MSG(Heat::cellCount(0, 4) == PC::PaintBump && Heat::cellCount(1, 5) == PC::PaintBump,
        "the cells the widening added bump on sight");
    CHECK(bumpedCellCount(2, 6) == 4);
}

TEST(testTheWindowTracksTheLiveGrid)
{
    PC::noteCellPaint(0, 0);
    Heat::noteWindowOpened();
    CHECK_MSG(Heat::cellCount(0, 0) == 0, "a window opening shows activity since it opened");

    for (int paint = 0; paint < 64; ++paint)
        PC::noteCellPaint(0, 0);
    PC::noteCellPaint(30, 100);
    auto extent = Heat::refreshWindow(24, 80);
    CHECK(extent.rows == 24);
    CHECK(extent.columns == 80);
    CHECK(extent.widthInPoints == 80 * Heat::cellSizeInPoints());
    CHECK(extent.heightInPoints == 24 * Heat::cellSizeInPoints());
    CHECK_MSG(Heat::cellCount(0, 0) == 229, "Core decays the counters inside the grid");
    CHECK_MSG(Heat::cellCount(30, 100) == 0, "a counter outside the grid is zeroed rather than bleeding back in");

    extent = Heat::refreshWindow(24, 132);
    CHECK_MSG(extent.widthInPoints == 132 * Heat::cellSizeInPoints(), "DECCOLM widens the window");

    extent = Heat::refreshWindow(500, 500);
    CHECK_MSG(extent.rows == PC::MaxRows && extent.columns == PC::MaxColumns, "the extent stops at the counter grid");

    CHECK(Heat::cellColor(0, 0) == PC::colorFor(Heat::cellCount(0, 0)));
}

std::unique_ptr<Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

Rendering::CellMetrics unifontMetrics()
{
    Rendering::UnifontRasterizer rasterizer(1, 0);
    return rasterizer.cellMetrics();
}

TEST(testTheCaptureFeedsTheShadowOnceEnabled)
{
    auto terminal = makeTerminal(6, 20);
    Ubiquitty::processBytes(terminal.get(), "hello");
    Rendering::TerminalFrameInputs frame;
    Rendering::TerminalSnapshotOptions options;

    PC::reset();
    CHECK(!frame.isPaintHeatmapEnabled());
    CHECK(frame.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    Ubiquitty::processBytes(terminal.get(), "\r\nworld");
    CHECK(frame.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    CHECK_MSG(bumpedCellCount(6, 20) == 0, "a capture nobody watches bumps nothing");

    frame.enablePaintHeatmap();
    CHECK(frame.isPaintHeatmapEnabled());
    CHECK(frame.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    CHECK_MSG(bumpedCellCount(6, 20) == 0, "the capture after enabling is the baseline");

    Ubiquitty::processBytes(terminal.get(), "!");
    CHECK(frame.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    CHECK_MSG(Heat::cellCount(1, 5) == PC::PaintBump, "the cell the output wrote bumps");
    CHECK_MSG(bumpedCellCount(6, 20) == 1, "and nothing else does");

    frame.disablePaintHeatmap();
    Ubiquitty::processBytes(terminal.get(), "?");
    CHECK(frame.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    CHECK_MSG(bumpedCellCount(6, 20) == 1, "a disabled heatmap stops counting");
}

TEST(testAFrameAnotherCaptureTookFeedsTheShadow)
{
    auto terminal = makeTerminal(6, 20);
    Ubiquitty::processBytes(terminal.get(), "hello");
    Rendering::TerminalFrameInputs live;
    Rendering::TerminalFrameInputs boundary;
    Rendering::TerminalSnapshotOptions options;

    live.enablePaintHeatmap();
    CHECK(live.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    Ubiquitty::processBytes(terminal.get(), "\r\n#");
    CHECK(boundary.capture(*terminal, options, unifontMetrics(), Rendering::GlyphSlotGeometry {}));
    CHECK_MSG(bumpedCellCount(6, 20) == 0, "a capture without a heatmap of its own bumps nothing");

    live.notePaintHeatOf(boundary);
    CHECK_MSG(Heat::cellCount(1, 0) == PC::PaintBump, "the presented boundary frame's change is counted");
    CHECK(bumpedCellCount(6, 20) == 1);
}

} // namespace

UBIQUITTY_TEST_MAIN()
