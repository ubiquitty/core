// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_gridsnap.cpp — the live-resize quantiser (Core/Rendering/GridSnap.h): how a window
// extent becomes a whole number of cells, when the screen's edge suspends that,
// and which end of an axis a press is dragging.
//
// Both macOS shells resize one grid through these, so a difference here is two
// shells reporting two sizes for one session.

#include "Core/Rendering/GridSnap.h"

using namespace Ubiquitty::Rendering;

namespace {
// A 7pt cell with 12pt of reserve, on which every whole number of cells is a
// whole point, and a 12.3pt reserve for the cases where the rounding has
// something to do: a whole number of cells plus it lands between the points a
// frame is applied on.
constexpr double cell = 7.0;
constexpr double reserve = 12.0;
constexpr double awkwardReserve = 12.3;
}

TEST(testSnapExtentToWholeCells)
{
    // Exactly ten cells stays exactly ten cells.
    CHECK_EQ(snapExtentToWholeCells(10 * cell + reserve, cell, reserve, 1000), 10 * cell + reserve);

    // Between cell counts, nearest wins in both directions.
    CHECK_EQ(snapExtentToWholeCells(10 * cell + reserve + 1.0, cell, reserve, 1000), 10 * cell + reserve);
    CHECK_EQ(snapExtentToWholeCells(10 * cell + reserve + 6.0, cell, reserve, 1000), 11 * cell + reserve);

    // Never below one cell, whatever the drag asks for. A zero-column grid has
    // no size to report and the caller would divide by it.
    CHECK_EQ(snapExtentToWholeCells(0.0, cell, reserve, 1000), cell + reserve);
    CHECK_EQ(snapExtentToWholeCells(-500.0, cell, reserve, 1000), cell + reserve);

    // Never above the limit, so a drag across a wall of displays cannot ask for
    // a grid the cell store refuses to allocate.
    CHECK_EQ(snapExtentToWholeCells(100000.0, cell, reserve, 80), 80 * cell + reserve);

    // An unmeasured font leaves the proposal alone rather than snapping to a
    // cell count derived from a zero.
    CHECK_EQ(snapExtentToWholeCells(123.4, 0.0, reserve, 1000), 123.4);
    CHECK_EQ(snapExtentToWholeCells(123.4, -1.0, reserve, 1000), 123.4);
}

TEST(testSnapRoundsUpToAWholePoint)
{
    // Nine 7pt cells plus a 12.3pt reserve is 75.3pt. The window server applies
    // a frame on whole points: rounded to nearest, 75pt, an extent the caller
    // divides back into EIGHT cells, dropping the row the snap chose. Rounded
    // up it is 76pt, and the 0.7pt over is blank under the last row.
    const double snapped = snapExtentToWholeCells(9 * cell + awkwardReserve, cell, awkwardReserve, 1000);
    CHECK_EQ(snapped, 76.0);
    CHECK(snapped >= 9 * cell + awkwardReserve);
    // The overshoot it buys is under one point, not a cell.
    CHECK(snapped - (9 * cell + awkwardReserve) < 1.0);
}

namespace {
// One axis of a step, on the cell and reserve above unless a case names its
// own reserve, with no screen bound unless a case names one.
DragAxisRequest dragAxis(double proposed, double current, double available = 0.0, double axisReserve = reserve)
{
    DragAxisRequest request;
    request.proposed = proposed;
    request.current = current;
    request.cellExtent = cell;
    request.reserve = axisReserve;
    request.cellLimit = 1000;
    request.available = available;
    return request;
}
}

TEST(testQuantiseDragExtentSnapsFromTheLattice)
{
    // A window on the lattice is quantised as the snap quantises, both ways.
    const double tenCells = 10 * cell + reserve;
    CHECK_EQ(quantiseDragExtent(dragAxis(tenCells - 2.0, tenCells)), tenCells);
    CHECK_EQ(quantiseDragExtent(dragAxis(tenCells - 5.0, tenCells)), 9 * cell + reserve);
    CHECK_EQ(quantiseDragExtent(dragAxis(tenCells + 5.0, tenCells)), 11 * cell + reserve);

    // An extent a hair off the lattice is still on it: a frame is applied on
    // whole points, so the hair is the arithmetic's and never the window's.
    CHECK_EQ(quantiseDragExtent(dragAxis(tenCells - 5.0, tenCells + 0.25)), 9 * cell + reserve);

    // An unmeasured font leaves the proposal alone.
    DragAxisRequest unmeasured = dragAxis(123.4, 130.0);
    unmeasured.cellExtent = 0.0;
    CHECK_EQ(quantiseDragExtent(unmeasured), 123.4);
}

TEST(testQuantiseDragExtentSnapsFromTheFrameTheWindowTook)
{
    // Ten 7pt cells and a 12.3pt reserve is 82.3pt; the lattice extent is 83pt,
    // and 83pt is the frame the window server applies. On a lattice of device
    // pixels the extent is 82.5pt at 2x, the applied 83pt reads back half a
    // point above it, and the quantiser takes the window for one off the
    // lattice: every step of a drag then follows the pointer through the cell
    // and snaps at its end, instead of moving by whole cells.
    const double applied = 83.0;
    CHECK_EQ(quantiseDragExtent(dragAxis(applied + 2.0, applied, 0.0, awkwardReserve)), applied);
    CHECK_EQ(quantiseDragExtent(dragAxis(applied - 2.0, applied, 0.0, awkwardReserve)), applied);

    // Past the half cell it moves by a whole one: nine cells is 75.3pt, applied
    // as 76pt; eleven is 89.3pt, applied as 90pt.
    CHECK_EQ(quantiseDragExtent(dragAxis(applied - 5.0, applied, 0.0, awkwardReserve)), 76.0);
    CHECK_EQ(quantiseDragExtent(dragAxis(applied + 5.0, applied, 0.0, awkwardReserve)), 90.0);

    // And the frame it hands back is one the window takes, so a step from that
    // frame starts on the lattice too.
    CHECK_EQ(quantiseDragExtent(dragAxis(90.0 + 2.0, 90.0, 0.0, awkwardReserve)), 90.0);
}

TEST(testQuantiseDragExtentFollowsThePointerThroughTheSlack)
{
    // Ten cells and four points of slack: a filled window's extent.
    const double below = 10 * cell + reserve;
    const double above = 11 * cell + reserve;
    const double filled = below + 4.0;

    // Shrinking, every extent down to the lattice passes through, the lattice
    // itself included, so the edge follows the pointer and lands on it.
    for (const double step : { 0.5, 1.0, 2.5, 4.0 })
        CHECK_EQ(quantiseDragExtent(dragAxis(filled - step, filled)), filled - step);
    // A step part-way through the slack is off the lattice too, so the next
    // one follows the pointer from there.
    CHECK_EQ(quantiseDragExtent(dragAxis(below + 1.0, below + 2.5)), below + 1.0);

    // Past the lattice the drag is quantised again.
    CHECK_EQ(quantiseDragExtent(dragAxis(below - 2.0, filled)), below);
    CHECK_EQ(quantiseDragExtent(dragAxis(below - 5.0, filled)), 9 * cell + reserve);
    CHECK_EQ(quantiseDragExtent(dragAxis(below - 1.0, below)), below);

    // Growing, the slack runs up to the next lattice extent.
    CHECK_EQ(quantiseDragExtent(dragAxis(filled + 1.5, filled)), filled + 1.5);
    CHECK_EQ(quantiseDragExtent(dragAxis(above, filled)), above);
    CHECK_EQ(quantiseDragExtent(dragAxis(above + 2.0, filled)), above);
}

TEST(testQuantiseDragExtentEndsFlushWithTheScreen)
{
    // Room for ten cells and five points more.
    const double lastFitting = 10 * cell + reserve;
    const double available = lastFitting + 5.0;
    const double start = 7 * cell + reserve;

    // Short of the last whole cell: the lattice.
    CHECK_EQ(quantiseDragExtent(dragAxis(lastFitting - 2.0, start, available)), lastFitting);
    CHECK_EQ(quantiseDragExtent(dragAxis(lastFitting - 5.0, start, available)), 9 * cell + reserve);

    // Through the partial cell the pointer is followed, so the window can end
    // flush instead of giving the fraction back to the desktop.
    CHECK_EQ(quantiseDragExtent(dragAxis(lastFitting + 2.0, start, available)), lastFitting + 2.0);
    CHECK_EQ(quantiseDragExtent(dragAxis(available, start, available)), available);

    // Past the edge: clamped to it, never beyond.
    CHECK_EQ(quantiseDragExtent(dragAxis(available + 50.0, start, available)), available);

    // No bound known, an unplaced window: the lattice, however far.
    CHECK_EQ(quantiseDragExtent(dragAxis(available + 50.0, start, 0.0)), 18 * cell + reserve);
    CHECK_EQ(quantiseDragExtent(dragAxis(available + 50.0, start, -3.0)), 18 * cell + reserve);
}

TEST(testDragEdgeForPress)
{
    constexpr double low = 100.0;
    constexpr double high = 400.0;
    constexpr double tolerance = 32.0;

    CHECK(dragEdgeForPress(105.0, low, high, tolerance) == DragEdge::Low);
    CHECK(dragEdgeForPress(395.0, low, high, tolerance) == DragEdge::High);

    // Inside, far from both: no handle was grabbed.
    CHECK(dragEdgeForPress(250.0, low, high, tolerance) == DragEdge::Neither);

    // Just outside the window still counts — a press lands on the frame, which
    // is outside the content box it is measured against.
    CHECK(dragEdgeForPress(low - 10.0, low, high, tolerance) == DragEdge::Low);
    CHECK(dragEdgeForPress(high + 10.0, low, high, tolerance) == DragEdge::High);

    // A window narrower than twice the tolerance has both edges in range at
    // once; nearest decides, so a press resolves to one end rather than to the
    // first one tested.
    CHECK(dragEdgeForPress(110.0, 100.0, 140.0, tolerance) == DragEdge::Low);
    CHECK(dragEdgeForPress(130.0, 100.0, 140.0, tolerance) == DragEdge::High);
}

TEST(testAvailableExtentForDrag)
{
    // A window at [200, 600] on a screen visible across [0, 1000].
    constexpr double windowLow = 200.0;
    constexpr double windowHigh = 600.0;
    constexpr double visibleLow = 0.0;
    constexpr double visibleHigh = 1000.0;

    // Dragging the low edge grows leftward from a fixed high edge, so the room
    // is measured from that high edge to the screen's low bound.
    CHECK_EQ(availableExtentForDrag(DragEdge::Low, windowLow, windowHigh, visibleLow, visibleHigh), 600.0);

    // Dragging the high edge grows rightward from a fixed low edge.
    CHECK_EQ(availableExtentForDrag(DragEdge::High, windowLow, windowHigh, visibleLow, visibleHigh), 800.0);

    // No edge sampled keeps the historical assumption rather than answering
    // zero, which would read as "no bound" and suspend the flush entirely.
    CHECK_EQ(availableExtentForDrag(DragEdge::Neither, windowLow, windowHigh, visibleLow, visibleHigh), 800.0);
}

UBIQUITTY_TEST_MAIN()
