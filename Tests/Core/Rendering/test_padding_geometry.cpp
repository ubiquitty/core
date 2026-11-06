// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_padding_geometry.cpp — portable padding/grid geometry (Rendering/
// PaddingGeometry.h): the cell-size resolver (natural-advance cap +
// DECCOLM-132 compression + snap-to-device-pixel floor) plus the forward and
// inverse maps between window pixels and cell coordinates. Pins the geometry
// the macOS Metal renderer hand-rolls across six call sites so the Linux
// Vulkan frontend computes clicks, SGR-1016 pixel reports, IME anchors, and
// snapshot sizes identically. Header-only, links standalone.

#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/PaddingGeometry.h"
#include "Core/Rendering/RemoteViewport.h"
#include "Core/Session/WindowPlacement.h"

#include <cstdio>
#include <limits>

using Ubiquitty::CellPosition;
using Ubiquitty::FloatRect;
using Ubiquitty::FloatSize;
using Ubiquitty::GridSize;
using Ubiquitty::IntPoint;
using Ubiquitty::IntSize;
using Ubiquitty::Rendering::cellRect;
using Ubiquitty::Rendering::ColumnMode;
using Ubiquitty::Rendering::EdgeInsets;
using Ubiquitty::Rendering::fitGridToViewport;
using Ubiquitty::Rendering::GridFitRequest;
using Ubiquitty::Rendering::gridFromViewport;
using Ubiquitty::Rendering::gridVerticalReserve;
using Ubiquitty::Rendering::pointToCaret;
using Ubiquitty::Rendering::pointToCell;
using Ubiquitty::Rendering::pointToDevicePixel;
using Ubiquitty::Rendering::reportedDevicePixel;
using Ubiquitty::Rendering::ReportOrigin;
using Ubiquitty::Rendering::resolveCellSize;
using Ubiquitty::Rendering::resolveGridContentRect;
using Ubiquitty::Rendering::surfaceSizeForGrid;
using Ubiquitty::Rendering::viewportForGrid;

static bool approx(float a, float b) { return (a - b < 1e-3f) && (b - a < 1e-3f); }

TEST(oversubscribedPaddingLeavesExactlyZeroVisibleContent)
{
    for (const float scale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        // The actual native Pan viewport: left padding alone exceeds its
        // width, with ordinary right padding still present.
        const auto content = resolveGridContentRect({ 358 * scale, 222 * scale },
            { .left = 359 * scale, .right = 6 * scale, .top = 223 * scale, .bottom = 6 * scale }, 0);
        CHECK_EQ(content.size.width, 0.0f);
        CHECK_EQ(content.size.height, 0.0f);
        CHECK(content.origin.x >= 0 && content.origin.x <= 358 * scale);
        CHECK(content.origin.y >= 0 && content.origin.y <= 222 * scale);
    }
    // Independent edge rounding can leave either a narrow sliver or a
    // negative extent, depending on the axis and asymmetric edge ratio.
    for (const float width : { 401.0f, 402.0f, 406.0f }) {
        const auto content = resolveGridContentRect({ width, width },
            { .left = width + 2, .right = 16, .top = 16, .bottom = width + 2 }, 7);
        CHECK_EQ(content.size.width, 0.0f);
        CHECK_EQ(content.size.height, 0.0f);
    }
}

TEST(remotePanFollowsOnlyTheCursorOutsideTheVisibleRectangle)
{
    using namespace Ubiquitty::Rendering;
    RemotePanRequest request { .viewport = { 400, 200 }, .cell = { 10, 20 }, .grid = { 100, 40 }, .cursor = { .row = 9, .column = 39 }, .previousOffset = {} };
    CHECK(remotePanOffset(request) == Ubiquitty::FloatPoint {});
    request.cursor = { .row = 39, .column = 99 };
    auto offset = remotePanOffset(request);
    CHECK(approx(offset.x, 600));
    CHECK(approx(offset.y, 600));
    request.previousOffset = offset;
    request.cursor = { .row = 31, .column = 65 };
    CHECK(remotePanOffset(request) == offset);
    request.cursor = { .row = 1, .column = 2 };
    offset = remotePanOffset(request);
    CHECK(approx(offset.x, 20));
    CHECK(approx(offset.y, 20));
}

TEST(remotePanRetainsHistoryPositionButClampsAfterResize)
{
    using namespace Ubiquitty::Rendering;
    RemotePanRequest request { .viewport = { 400, 200 }, .cell = { 10, 20 }, .grid = { 100, 40 }, .cursor = {}, .previousOffset = { 600, 600 }, .shouldFollowCursor = false };
    CHECK(remotePanOffset(request) == request.previousOffset);
    request.viewport = { 800, 700 };
    const auto offset = remotePanOffset(request);
    CHECK(approx(offset.x, 200));
    CHECK(approx(offset.y, 100));
    request.viewport = { 1200, 900 };
    CHECK(remotePanOffset(request) == Ubiquitty::FloatPoint {});
}

TEST(remotePanUsesOneTransformAtEveryBackingScale)
{
    using namespace Ubiquitty::Rendering;
    for (const float scale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        RemotePanRequest request { .viewport = { 400 * scale, 200 * scale }, .cell = { 10 * scale, 20 * scale }, .grid = { 100, 40 }, .cursor = { .row = 39, .column = 99 }, .previousOffset = {} };
        const auto offset = remotePanOffset(request);
        CHECK(approx(offset.x / scale, 600));
        CHECK(approx(offset.y / scale, 600));
        const Ubiquitty::FloatPoint origin { 8 * scale - offset.x, 12 * scale - offset.y };
        const auto rectangle = cellRect(request.cursor, request.cell, { .left = origin.x, .top = origin.y }, 0);
        const auto hit = pointToCell({ rectangle.origin.x + request.cell.width / 2, rectangle.origin.y + request.cell.height / 2 }, origin, request.cell, request.grid, RowClamp::Unclamped);
        CHECK(hit == request.cursor);
    }
}

TEST(remotePanRejectsInvalidGeometryAndKeepsAnOversizedCellsLeadingEdge)
{
    using namespace Ubiquitty::Rendering;
    RemotePanRequest request { .viewport = { 4, 10 }, .cell = { 10, 20 }, .grid = { 100, 40 }, .cursor = { .row = 4, .column = 3 }, .previousOffset = {} };
    const auto offset = remotePanOffset(request);
    CHECK(approx(offset.x, 30));
    CHECK(approx(offset.y, 80));
    request.cell = { 0, -1 };
    CHECK(remotePanOffset(request) == Ubiquitty::FloatPoint {});
    request.cell = { 10, 20 };
    request.viewport = { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() };
    CHECK(remotePanOffset(request) == Ubiquitty::FloatPoint {});
    request.viewport = { 400, 200 };
    request.previousOffset = { std::numeric_limits<float>::quiet_NaN(), -20 };
    request.shouldFollowCursor = false;
    CHECK(remotePanOffset(request) == Ubiquitty::FloatPoint {});
}

// resolveCellSize: with a positive natural advance and no compression/snap the
// cell is the natural advance (the grid does not stretch to fill).
TEST(testCellsizeNaturalAdvance)
{
    FloatRect content;
    content.size.width = 800.0f;
    content.size.height = 600.0f;
    // Natural advance 9.6 x 20.0; window holds 80x24 with slack.
    FloatSize c = resolveCellSize(content, { /*columns*/ 80, /*rows*/ 24 }, { /*naturalAdvanceWidth*/ 9.6f, /*naturalAdvanceHeight*/ 20.0f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, /*backingScale*/ 2.0f);
    CHECK(approx(c.width, 9.6f)); // Not 800/80 == 10.0 — natural, not stretched
    CHECK(approx(c.height, 20.0f));
    printf("testCellsizeNaturalAdvance done\n");
}

// resolveCellSize: when natural advance is unavailable (<=0) fall back to
// stretch-to-fill (content / count), mirroring the renderer's naturalCell<=0
// guard.
TEST(testCellsizeStretchFallback)
{
    FloatRect content;
    content.size.width = 800.0f;
    content.size.height = 600.0f;
    FloatSize c = resolveCellSize(content, { 80, 24 }, { /*naturalAdvanceWidth*/ 0.0f, /*naturalAdvanceHeight*/ 0.0f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, 1.0f);
    CHECK(approx(c.width, 10.0f)); // 800 / 80
    CHECK(approx(c.height, 25.0f)); // 600 / 24
    printf("testCellsizeStretchFallback done\n");
}

// DECCOLM-132: a 132-column grid in an 800px-wide window must compress the cell
// below its natural advance so all 132 columns fit (Monaco-style in-place 132).
TEST(testCellsizeDeccolm132Compression)
{
    FloatRect content;
    content.size.width = 800.0f;
    content.size.height = 600.0f;
    // Natural advance 10.0 would need 1320px for 132 columns — overflow.
    FloatSize c = resolveCellSize(content, { /*columns*/ 132, /*rows*/ 24 }, { /*naturalAdvanceWidth*/ 10.0f, /*naturalAdvanceHeight*/ 20.0f }, ColumnMode::Columns132, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, 1.0f);
    CHECK(approx(c.width, 800.0f / 132.0f)); // compressed to fit
    CHECK(c.width < 10.0f);
    // With columns132 OFF the same geometry keeps the natural advance (overflow
    // becomes clip/margin, not compression).
    FloatSize off = resolveCellSize(content, { 132, 24 }, { 10.0f, 20.0f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, 1.0f);
    CHECK(approx(off.width, 10.0f));
    // DECCOLM never STRETCHES: if the window is wide enough the natural advance
    // wins (fitWidth >= natural, so the min keeps natural).
    FloatRect wide;
    wide.size.width = 2000.0f;
    wide.size.height = 600.0f;
    FloatSize w = resolveCellSize(wide, { 132, 24 }, { 10.0f, 20.0f }, ColumnMode::Columns132, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, 1.0f);
    CHECK(approx(w.width, 10.0f));
    printf("testCellsizeDeccolm132Compression done\n");
}

// snap-to-device-pixel: the cell HEIGHT floors to a whole device pixel for crisp
// row boundaries; width is untouched. A fractional height like 20.7 -> 20.
TEST(testCellsizeSnapDevicePixel)
{
    FloatRect content;
    content.size.width = 800.0f;
    content.size.height = 600.0f;
    FloatSize snapped = resolveCellSize(content, { 80, 24 }, { 9.6f, /*naturalAdvanceHeight*/ 20.7f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Snapped, 1.0f);
    CHECK(approx(snapped.height, 20.0f)); // floored
    CHECK(approx(snapped.width, 9.6f)); // width unsnapped
    FloatSize unsnapped = resolveCellSize(content, { 80, 24 }, { 9.6f, 20.7f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Unsnapped, 1.0f);
    CHECK(approx(unsnapped.height, 20.7f));
    // Snap never drops a cell below 1px.
    FloatSize tiny = resolveCellSize(content, { 80, 24 }, { 9.6f, /*naturalAdvanceHeight*/ 0.5f }, ColumnMode::Standard, Ubiquitty::Rendering::DevicePixelSnap::Snapped, 1.0f);
    CHECK(approx(tiny.height, 1.0f));
    printf("testCellsizeSnapDevicePixel done\n");
}

// gridFromViewport: floor the padding-subtracted span by the cell size; clamp to
// >= 1 even when the window is too small to hold a single cell.
TEST(testGridfromviewportBasic)
{
    EdgeInsets insets;
    insets.left = 4.0f;
    insets.right = 4.0f;
    insets.top = 4.0f;
    insets.bottom = 4.0f;
    // 808 wide: (808 - 8)/10 = 80 columns exactly. reservedHeight = top+bottom = 8.
    // 488 tall: (488 - 8)/20 = 24 rows exactly.
    GridSize d = gridFromViewport({ /*viewportWidth*/ 808.0f, /*viewportHeight*/ 488.0f }, insets, /*reservedHeight*/ 8.0f, { /*cellWidth*/ 10.0f, /*cellHeight*/ 20.0f });
    CHECK(d.columns == 80);
    CHECK(d.rows == 24);
    // A few px of slack floors down, not up.
    GridSize slack = gridFromViewport({ 815.0f, 495.0f }, insets, 8.0f, { 10.0f, 20.0f });
    CHECK(slack.columns == 80); // (815-8)/10 = 80.7 -> 80
    CHECK(slack.rows == 24); // (495-8)/20 = 24.35 -> 24
    // Tiny window clamps to a 1x1 grid (default clampToMinimumGrid).
    GridSize tiny = gridFromViewport({ 2.0f, 2.0f }, insets, 8.0f, { 10.0f, 20.0f });
    CHECK(tiny.columns == 1);
    CHECK(tiny.rows == 1);
    // clampToMinimumGrid=false (the Linux "keep prior grid" path): the same tiny
    // window yields raw floored dims below 1, so the caller's own gate can reject
    // it instead of adopting a degenerate 1x1.
    GridSize raw = gridFromViewport({ 2.0f, 2.0f }, insets, 8.0f, { 10.0f, 20.0f }, Ubiquitty::Rendering::GridFloor::None);
    CHECK(raw.columns < 1);
    CHECK(raw.rows < 1);
    // A comfortable window is unaffected by the flag.
    GridSize rawFits = gridFromViewport({ 808.0f, 488.0f }, insets, 8.0f, { 10.0f, 20.0f }, Ubiquitty::Rendering::GridFloor::None);
    CHECK(rawFits.columns == 80);
    CHECK(rawFits.rows == 24);
    printf("testGridfromviewportBasic done\n");
}

// Round-trip: a grid sized from a viewport, fed back through viewportForGrid,
// must produce a viewport the same grid sizing recovers. With cells dividing the
// content evenly and reservedHeight == top+bottom+extraTop, this is exact.
TEST(testGridfromviewportRoundtrip)
{
    EdgeInsets insets;
    insets.left = 6.0f;
    insets.right = 6.0f;
    insets.top = 6.0f;
    insets.bottom = 6.0f;
    const float extraTop = 0.0f;
    const float cellWidth = 8.0f;
    const float cellHeight = 16.0f;
    const float reservedHeight = insets.top + insets.bottom + extraTop;

    // Start from an arbitrary viewport, derive the grid, rebuild the minimal
    // viewport, and re-derive: the column/row count is stable.
    GridSize a = gridFromViewport({ 660.0f, 410.0f }, insets, reservedHeight, { cellWidth, cellHeight });
    FloatSize v = viewportForGrid({ a.columns, a.rows }, { cellWidth, cellHeight }, insets, extraTop);
    GridSize b = gridFromViewport({ v.width, v.height }, insets, reservedHeight, { cellWidth, cellHeight });
    CHECK(a.columns == b.columns);
    CHECK(a.rows == b.rows);

    // viewportForGrid is the literal inverse: extent = count*cell + padding(+extra).
    FloatSize ext = viewportForGrid({ 80, 24 }, { cellWidth, cellHeight }, insets, extraTop);
    CHECK(approx(ext.width, 80.0f * cellWidth + insets.left + insets.right));
    CHECK(approx(ext.height, 24.0f * cellHeight + insets.top + insets.bottom + extraTop));
    // extraTop grows only the height (top reservation), never the width.
    FloatSize withExtra = viewportForGrid({ 80, 24 }, { cellWidth, cellHeight }, insets, /*extraTop*/ 10.0f);
    CHECK(approx(withExtra.width, ext.width));
    CHECK(approx(withExtra.height, ext.height + 10.0f));
    printf("testGridfromviewportRoundtrip done\n");
}

// pointToCell: the inverse hit-test. Grid origin folds in left inset / (top +
// extraTop). Cover inset edge, interior, last col/row, and the clamp behavior.
TEST(testPointtocellBoundaries)
{
    // 80x24 grid, 10x20 cells, origin at (4, 4+2 extraTop = 6).
    const float originX = 4.0f;
    const float originY = 6.0f;
    const float cellWidth = 10.0f;
    const float cellHeight = 20.0f;
    const int columns = 80;
    const int rows = 24;

    // A point exactly on the grid origin is cell (0,0).
    CellPosition o = pointToCell({ originX, originY }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(o.row == 0 && o.column == 0);

    // A point just inside the inset (left of origin) clamps to column 0.
    CellPosition leftOfGrid = pointToCell({ originX - 3.0f, originY }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(leftOfGrid.column == 0);

    // Interior: 35px right of origin / 50px below = column 3, row 2.
    CellPosition mid = pointToCell({ originX + 35.0f, originY + 50.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(mid.column == 3); // floor(35/10)
    CHECK(mid.row == 2); // floor(50/20)

    // The last cell's far edge clamps to the last column/row, not column 80.
    CellPosition last = pointToCell({ originX + columns * cellWidth + 100.0f, originY + rows * cellHeight + 100.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(last.column == columns - 1);
    CHECK(last.row == rows - 1);

    // clampRow == false lets a point above the grid read a NEGATIVE row (the
    // auto-scroll case) while the column still clamps.
    CellPosition above = pointToCell({ originX + 35.0f, originY - 25.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Unclamped);
    CHECK(above.row == -2); // floor(-25/20) = -2 (floors toward -inf, not 0)
    CHECK(above.column == 3);
    // Same point WITH clampRow snaps the row back to 0.
    CellPosition aboveClamped = pointToCell({ originX + 35.0f, originY - 25.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(aboveClamped.row == 0);
    // A point below the grid with clampRow == false reads a row past the bottom.
    CellPosition below = pointToCell({ originX, originY + rows * cellHeight + 5.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Unclamped);
    CHECK(below.row == rows); // floor((rows*cellHeight+5)/cellHeight) = rows
    printf("testPointtocellBoundaries done\n");
}

// pointToDevicePixel: 1-based device px in the client's columns·cellPixelWidth
// space (CSI 14t), from the fractional cell position × integer cellPixelWidth.
TEST(testPointtodevicepixel)
{
    const float originX = 4.0f; // points
    const float originY = 6.0f;
    const float scale = 2.0f;
    const int textAreaW = 1600, textAreaH = 1200;
    // TRUE backing pixels: floor((point-origin)*scale)+1. 10pt right → floor(20)+1=21,
    // 20pt down → floor(40)+1=41 — the space the grid is actually drawn in.
    IntPoint p = pointToDevicePixel({ originX + 10.0f, originY + 20.0f }, { originX, originY }, scale, { textAreaW, textAreaH });
    CHECK(p.x == 21 && p.y == 41);
    // On the origin: 0 → floor(0)+1 = 1.
    IntPoint o = pointToDevicePixel({ originX, originY }, { originX, originY }, scale, { textAreaW, textAreaH });
    CHECK(o.x == 1 && o.y == 1);
    // Left/above the origin clamps to 1 (never 0 or negative).
    IntPoint before = pointToDevicePixel({ originX - 5.0f, originY - 5.0f }, { originX, originY }, scale, { textAreaW, textAreaH });
    CHECK(before.x == 1 && before.y == 1);
    // Past the right/bottom edge (padding or fullscreen sub-cell remainder) clamps to
    // the published true drawn text-area extent, not beyond.
    IntPoint past = pointToDevicePixel({ originX + 2000.0f, originY + 2000.0f }, { originX, originY }, scale, { textAreaW, textAreaH });
    CHECK(past.x == 1600 && past.y == 1200);
    printf("testPointtodevicepixel done\n");
}

// reportedDevicePixel: the basis both arms hand their facts to. The grid
// origin under text, the drawable's corner under DECSET 7402, and no clamp of
// its own — the ladder clamps to what CSI 14 t advertises.
TEST(testReporteddevicepixelPicksTheOriginTheModeCallsFor)
{
    const float originX = 4.0f;
    const float originY = 6.0f;
    const float scale = 2.0f;
    const IntPoint padded = reportedDevicePixel({ originX + 10.0f, originY + 20.0f }, { originX, originY }, scale, ReportOrigin::Grid);
    CHECK(padded.x == 21 && padded.y == 41);
    // Under the mode the same point is a padding-width further from the
    // corner the picture fills from.
    const IntPoint edge = reportedDevicePixel({ originX + 10.0f, originY + 20.0f }, { originX, originY }, scale, ReportOrigin::Drawable);
    CHECK(edge.x == 29 && edge.y == 53);
    // Unclamped on the far edges: the ladder owns that against the extent the
    // terminal advertises.
    const IntPoint far = reportedDevicePixel({ originX + 2000.0f, originY + 2000.0f }, { originX, originY }, scale, ReportOrigin::Grid);
    CHECK(far.x == 4001 && far.y == 4001);
    // Still never below one.
    const IntPoint before = reportedDevicePixel({ 0.0f, 0.0f }, { originX, originY }, scale, ReportOrigin::Grid);
    CHECK(before.x == 1 && before.y == 1);
}

// cellRect: forward layout for cursor/IME — origin folds in insets + extraTop.
TEST(testCellrectForward)
{
    EdgeInsets insets;
    insets.left = 4.0f;
    insets.right = 4.0f;
    insets.top = 4.0f;
    insets.bottom = 4.0f;
    const float extraTop = 2.0f;
    FloatRect r = cellRect({ /*row*/ 3, /*column*/ 5 }, { /*cellWidth*/ 10.0f, /*cellHeight*/ 20.0f }, insets, extraTop);
    CHECK(approx(r.origin.x, 4.0f + 5.0f * 10.0f)); // 54
    CHECK(approx(r.origin.y, 4.0f + 2.0f + 3.0f * 20.0f)); // 66
    CHECK(approx(r.size.width, 10.0f));
    CHECK(approx(r.size.height, 20.0f));
    // cellRect is the forward of pointToCell: a point inside the cell maps back.
    CellPosition back = pointToCell({ r.origin.x + 1.0f, r.origin.y + 1.0f }, { insets.left, insets.top + extraTop }, { 10.0f, 20.0f }, { 80, 24 }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(back.column == 5 && back.row == 3);
    printf("testCellrectForward done\n");
}

// surfaceSizeForGrid: (count*cell + paddingSum)*scale, rounded, floor 1, clamp
// 16384.
TEST(testSurfacesizeClamps)
{
    // 80x24 grid, 8x16 cells, 2x4 padding sums, scale 2: width =
    // (80*8 + 2)*2 = 1284; height = (24*16 + 4)*2 = 776.
    IntSize s = surfaceSizeForGrid({ /*columns*/ 80, /*rows*/ 24 }, { /*cellWidth*/ 8.0f, /*cellHeight*/ 16.0f }, { /*paddingSumX*/ 2.0f, /*paddingSumY*/ 4.0f }, /*scale*/ 2.0f);
    CHECK(s.width == 1284);
    CHECK(s.height == 776);

    // A zero grid still produces at least a 1x1 texture.
    IntSize floored = surfaceSizeForGrid({ 0, 0 }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 1.0f);
    CHECK(floored.width == 1);
    CHECK(floored.height == 1);

    // A pathological geometry clamps to the 16384 GPU ceiling.
    IntSize huge = surfaceSizeForGrid({ 100000, 100000 }, { 8.0f, 16.0f }, { 0.0f, 0.0f }, 2.0f);
    CHECK(huge.width == 16384);
    CHECK(huge.height == 16384);

    // Rounding: 8.3px cell over 1 column rounds 8.3 -> 8 (round half away from 0).
    IntSize rounded = surfaceSizeForGrid({ 1, 1 }, { 8.3f, 16.6f }, { 0.0f, 0.0f }, 1.0f);
    CHECK(rounded.width == 8); // round(8.3)
    CHECK(rounded.height == 17); // round(16.6)
    printf("testSurfacesizeClamps done\n");
}

// resolveGridContentRect (shipped) interop: the inverse of viewportForGrid and
// the basis pointToCell's origin folds in — confirm the shipped forward map and
// the new helpers agree on the grid origin.
TEST(testGridcontentrectInterop)
{
    EdgeInsets insets;
    insets.left = 4.0f;
    insets.right = 6.0f;
    insets.top = 4.0f;
    insets.bottom = 6.0f;
    FloatRect content = resolveGridContentRect({ 800.0f, 600.0f }, insets, /*extraTop*/ 10.0f);
    CHECK(approx(content.origin.x, 4.0f));
    CHECK(approx(content.origin.y, 4.0f + 10.0f)); // top inset + extraTop
    // pointToCell using that origin lands cell (0,0) at the grid's top-left.
    CellPosition topLeft = pointToCell({ content.origin.x, content.origin.y }, { content.origin.x, content.origin.y }, { 10.0f, 20.0f }, { 80, 24 }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(topLeft.row == 0 && topLeft.column == 0);
    printf("testGridcontentrectInterop done\n");
}

// Backing-scale coherence (the retina<->non-retina drag bug): the grid count is
// derived from the viewport in POINTS, while the renderer fills a drawable in
// DEVICE PIXELS with cells of cellPoints*scale. The drawable's pixel size and the
// cell scale must come from the same backing scale or the grid covers only the
// matching fraction. Pin that columns*cellWidthInPixels + paddingX ~=
// drawableWidth at both scale 1 and scale 2, and that mixing scales (drawable at
// 2x, cells at 1x) reproduces the top-left-quarter under-fill.
TEST(testBackingScaleGridFillsDrawable)
{
    // 800x600pt window, 1pt per-edge padding, 8x16pt natural cell -> 99 cols, 37 rows.
    const float boundsWidthInPoints = 800.0f, boundsHeightInPoints = 600.0f;
    const float paddingInPoints = 1.0f; // per edge
    const float cellWidthInPoints = 8.0f, cellHeightInPoints = 16.0f;
    EdgeInsets insetsInPoints;
    insetsInPoints.left = insetsInPoints.right = paddingInPoints;
    insetsInPoints.top = insetsInPoints.bottom = paddingInPoints;
    const float reserveVerticalInPoints = insetsInPoints.top + insetsInPoints.bottom;
    const GridSize dims = gridFromViewport({ boundsWidthInPoints, boundsHeightInPoints }, insetsInPoints, reserveVerticalInPoints, { cellWidthInPoints, cellHeightInPoints });

    // At a coherent scale the grid extent (count*cell*scale + padding*scale) lands
    // within one cell of the drawable (bounds*scale) on each axis: the slack is the
    // sub-cell remainder, never a half-drawable gap.
    const float scales[2] = { 1.0f, 2.0f };
    for (int si = 0; si < 2; ++si) {
        const float scale = scales[si];
        const float drawableWidth = boundsWidthInPoints * scale;
        const float drawableHeight = boundsHeightInPoints * scale;
        const float gridWidth = dims.columns * (cellWidthInPoints * scale) + 2.0f * paddingInPoints * scale;
        const float gridHeight = dims.rows * (cellHeightInPoints * scale) + 2.0f * paddingInPoints * scale;
        CHECK(gridWidth <= drawableWidth + 1e-3f);
        CHECK(gridWidth >= drawableWidth - cellWidthInPoints * scale - 1e-3f);
        CHECK(gridHeight <= drawableHeight + 1e-3f);
        CHECK(gridHeight >= drawableHeight - cellHeightInPoints * scale - 1e-3f);
    }

    // A mismatched scale: drawable sized at 2x (stale retina) while cells size at
    // 1x (the live non-retina contentsScale). The grid then fills ~half each axis -> the
    // top-left quarter. This asserts the FAILURE mode so a regression that reuses a
    // mismatched scale is caught: the live-scale grid is far below the (too-large)
    // drawable.
    const float staleDrawableWidth = boundsWidthInPoints * 2.0f;
    const float liveCellGridWidth = dims.columns * (cellWidthInPoints * 1.0f) + 2.0f * paddingInPoints * 1.0f;
    CHECK(liveCellGridWidth < staleDrawableWidth * 0.55f); // covers only ~the left half
    printf("testBackingScaleGridFillsDrawable done\n");
}

// pointToCaret: the boundary the point is NEAREST, which is what makes a drag
// behave like a text field rather than like a grid of cells.
TEST(testPointtocaretRoundsToTheNearestBoundary)
{
    // 80x24 grid, 10x20 cells, origin at (4, 6).
    const Ubiquitty::FloatPoint origin { 4.0f, 6.0f };
    const FloatSize cell { 10.0f, 20.0f };
    const GridSize grid { 80, 24 };

    // The left edge of cell 3 is x = 34. A point just inside it is nearer the
    // boundary before the cell than the one after, so the caret is 3 — where
    // pointToCell answers 3 for the whole width of that cell.
    CHECK(pointToCaret({ 35.0f, 26.0f }, origin, cell, grid).caret == 3);
    CHECK(pointToCell({ 35.0f, 26.0f }, origin, cell, grid, Ubiquitty::Rendering::RowClamp::Clamped).column == 3);

    // Past the cell's midpoint the caret moves on and the cell does not: this
    // is the whole difference, and it is what lets one character be selected.
    CHECK(pointToCaret({ 40.0f, 26.0f }, origin, cell, grid).caret == 4);
    CHECK(pointToCell({ 40.0f, 26.0f }, origin, cell, grid, Ubiquitty::Rendering::RowClamp::Clamped).column == 3);

    // The row is the one the point falls in, floored like a cell's.
    CHECK(pointToCaret({ 35.0f, 26.0f }, origin, cell, grid).row == 1);
    CHECK(pointToCaret({ 35.0f, 45.0f }, origin, cell, grid).row == 1);
    CHECK(pointToCaret({ 35.0f, 47.0f }, origin, cell, grid).row == 2);

    // There is one more caret than there are cells, because a caret sits
    // between characters: the last is at `columns`, past the last cell.
    CHECK(pointToCaret({ 4.0f + 80.0f * 10.0f, 26.0f }, origin, cell, grid).caret == 80);
    CHECK(pointToCaret({ 100000.0f, 26.0f }, origin, cell, grid).caret == 80);

    // Both axes clamp into the grid: a caret drag reads its out-of-grid rows
    // from pointToCell instead, which is what the walk past an edge needs.
    CHECK(pointToCaret({ -100.0f, -100.0f }, origin, cell, grid).caret == 0);
    CHECK(pointToCaret({ -100.0f, -100.0f }, origin, cell, grid).row == 0);
    CHECK(pointToCaret({ 35.0f, 100000.0f }, origin, cell, grid).row == 23);

    // A degenerate cell answers the origin rather than dividing by zero.
    CHECK(pointToCaret({ 35.0f, 26.0f }, origin, { 0.0f, 0.0f }, grid).caret == 0);
    printf("testPointtocaretRoundsToTheNearestBoundary done\n");
}

// gridVerticalReserve: the padding on both edges plus the chrome above the
// grid, the chrome clamped into what the padding leaves.
TEST(testGridverticalreserveAddsTheChromeToThePadding)
{
    EdgeInsets insets;
    insets.top = 4.0f;
    insets.bottom = 6.0f;
    CHECK(approx(gridVerticalReserve(insets, /*extraTop*/ 0.0f, /*contentHeight*/ 500.0f), 10.0f));
    CHECK(approx(gridVerticalReserve(insets, 12.0f, 500.0f), 22.0f));
    // A negative band is nothing, never a height handed back.
    CHECK(approx(gridVerticalReserve(insets, -12.0f, 500.0f), 10.0f));
    // A window shorter than its own chrome reserves what is left of it, so
    // the fit subtracts a height rather than adding one.
    CHECK(approx(gridVerticalReserve(insets, 40.0f, 30.0f), 30.0f));
    // Shorter than the padding alone: the padding stands and the band is
    // dropped whole, which the row floor below is what saves.
    CHECK(approx(gridVerticalReserve(insets, 40.0f, 4.0f), 10.0f));
    printf("testGridverticalreserveAddsTheChromeToThePadding done\n");
}

// fitGridToViewport: the reserve comes off the height before the division, so
// a grid never claims a row the box it is laid out in cannot hold.
TEST(testFitgridtoviewportReservesTheVerticalPadding)
{
    GridFitRequest request;
    request.insets.left = 8.0f;
    request.insets.right = 8.0f;
    request.insets.top = 0.0f;
    request.insets.bottom = 8.0f;
    request.cell = { 10.0f, 20.0f };
    // 494 tall: the naive division holds 24 rows with 14 left over, and 14 is
    // less than the 8 the padding needs plus the 20 a row needs — so the box
    // holds 24 only if the padding is not reserved. This is the shape the
    // Swift surface was resizing to: one row past the bottom edge, drawn into
    // the padding and clipped by the window.
    request.viewport = { 816.0f, 494.0f };
    CHECK(gridFromViewport(request.viewport, request.insets, 0.0f, request.cell).rows == 24);
    CHECK(fitGridToViewport(request).rows == 24);
    // 486 tall: 24 rows need 480, and 480 + 8 exceeds it. The reserve is what
    // sees that.
    request.viewport = { 816.0f, 486.0f };
    CHECK(gridFromViewport(request.viewport, request.insets, 0.0f, request.cell).rows == 24);
    CHECK(fitGridToViewport(request).rows == 23);
    // The width is the same question, and `gridFromViewport` already asked it:
    // 816 less the two 8s is 800, which is 80 columns.
    CHECK(fitGridToViewport(request).columns == 80);
    printf("testFitgridtoviewportReservesTheVerticalPadding done\n");
}

// fitGridToViewport: DECCOLM's hold on the width, and the clamp into the grid
// a resize can hold.
TEST(testFitgridtoviewportHoldsTheWidthAndClampsToStorage)
{
    GridFitRequest request;
    request.cell = { 10.0f, 20.0f };
    request.viewport = { 800.0f, 480.0f };
    CHECK(fitGridToViewport(request).columns == 80);
    // Held: the application's count crosses unchanged however wide the window
    // is, and only the rows follow it.
    request.isColumnCountHeld = true;
    request.heldColumns = 132;
    CHECK(fitGridToViewport(request).columns == 132);
    CHECK(fitGridToViewport(request).rows == 24);
    // A window no resize could be represented at, clamped to what the grid
    // holds rather than reported as asked: a frontend that laid its chrome out
    // against the ask would lay it out against a grid the terminal does not
    // have.
    request.isColumnCountHeld = false;
    request.viewport = { 40000.0f, 40000.0f };
    CHECK(fitGridToViewport(request).columns == Ubiquitty::maxColumns);
    CHECK(fitGridToViewport(request).rows == Ubiquitty::maxRows);
    // And the floor: a window too small for a cell is represented at the size
    // a resize floors to, not at one.
    request.viewport = { 1.0f, 1.0f };
    CHECK(fitGridToViewport(request).columns == Ubiquitty::minColumns);
    CHECK(fitGridToViewport(request).rows == Ubiquitty::minRows);
    printf("testFitgridtoviewportHoldsTheWidthAndClampsToStorage done\n");
}

// isViewportLaidOut: a view with no frame yet names no grid. Its fit is the
// floor, and a thin client that reported the floor resized the host's session
// to two columns — every window mirroring it then followed the host down.
TEST(testAnUnlaidOutViewportNamesNoGrid)
{
    GridFitRequest request;
    request.cell = { 10.0f, 20.0f };
    request.insets = { 0.0f, 8.0f, 8.0f, 8.0f };
    CHECK(!Ubiquitty::Rendering::isViewportLaidOut(request.viewport));
    CHECK(!Ubiquitty::Rendering::isViewportLaidOut({ 816.0f, 0.0f }));
    CHECK(!Ubiquitty::Rendering::isViewportLaidOut({ 0.0f, 494.0f }));
    CHECK(fitGridToViewport(request).columns == Ubiquitty::minColumns);
    // A pane dragged smaller than a cell has been laid out, and its floor is a
    // grid the person chose.
    CHECK(Ubiquitty::Rendering::isViewportLaidOut({ 1.0f, 1.0f }));
    CHECK(Ubiquitty::Rendering::isViewportLaidOut({ 816.0f, 494.0f }));
    printf("testAnUnlaidOutViewportNamesNoGrid done\n");
}

// classifyCellMetricChange: the window that chose its own size follows the
// font; the one the window manager sized, and a split, hold and reflow.
TEST(testCellMetricChangeIsClassifiedFromThePreChangeGeometry)
{
    using Ubiquitty::Rendering::CellMetricChangeResponse;
    Ubiquitty::Rendering::CellMetricChangeSnapshot snapshot;
    snapshot.isValid = true;
    snapshot.grid = { 80, 24 };
    snapshot.cell = { 7.2f, 15.0f };
    snapshot.insets = { 4.0f, 4.0f, 2.0f, 2.0f };
    // Exactly the grid's extent: the terminal chose this size.
    snapshot.content = Ubiquitty::Rendering::viewportForGrid(snapshot.grid, snapshot.cell, snapshot.insets, 0.0f);
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::WindowFollowsGrid);
    // A point of AppKit alignment is inside the tolerance.
    snapshot.content.width += 1.5f;
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::WindowFollowsGrid);
    // A sub-cell remainder is not: the window manager sized this one.
    snapshot.content.width += 3.0f;
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::GridReflowsInWindow);
    snapshot.content = Ubiquitty::Rendering::viewportForGrid(snapshot.grid, snapshot.cell, snapshot.insets, 0.0f);
    // Pinned or split, a clean multiple still holds.
    snapshot.isPinned = true;
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::GridReflowsInWindow);
    snapshot.isPinned = false;
    snapshot.isSplit = true;
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::GridReflowsInWindow);
    snapshot.isSplit = false;
    // No snapshot at all — a window with no cell yet — reflows rather than
    // sizing a window from nothing.
    snapshot.isValid = false;
    CHECK(Ubiquitty::Rendering::classifyCellMetricChange(snapshot) == CellMetricChangeResponse::GridReflowsInWindow);
    printf("testCellMetricChangeIsClassifiedFromThePreChangeGeometry done\n");
}

// anchoredWindowFrame: the top-left corner stays, and the frame is nudged
// inside the visible area on whichever edge it would leave.
TEST(testAnchoredWindowFrameKeepsTheTopLeftAndStaysOnScreen)
{
    using Ubiquitty::Rendering::WindowYAxis;
    const FloatRect visible { { 0.0f, 25.0f }, { 1440.0f, 875.0f } };
    const FloatRect frame { { 100.0f, 300.0f }, { 600.0f, 400.0f } };
    // Grown in place: x stays, and with y up the top edge (maxY = 700) stays,
    // so the origin moves down by the growth.
    FloatRect grown = Ubiquitty::Rendering::anchoredWindowFrame(frame, { 700.0f, 500.0f }, visible, WindowYAxis::Up);
    CHECK(grown.origin.x == 100.0f && grown.size.width == 700.0f && grown.size.height == 500.0f);
    CHECK(grown.origin.y + grown.size.height == 700.0f);
    // Grown past the right edge: nudged left to sit flush.
    grown = Ubiquitty::Rendering::anchoredWindowFrame(frame, { 1400.0f, 400.0f }, visible, WindowYAxis::Up);
    CHECK(grown.origin.x + grown.size.width == 1440.0f);
    // Grown past the bottom (y up: below the menu bar's excluded strip): nudged up.
    grown = Ubiquitty::Rendering::anchoredWindowFrame(frame, { 600.0f, 800.0f }, visible, WindowYAxis::Up);
    CHECK(grown.origin.y == 25.0f);
    // Y down: the top edge is minY, so a grown frame keeps its origin.
    grown = Ubiquitty::Rendering::anchoredWindowFrame(frame, { 700.0f, 500.0f }, visible, WindowYAxis::Down);
    CHECK(grown.origin.x == 100.0f && grown.origin.y == 300.0f);
    printf("testAnchoredWindowFrameKeepsTheTopLeftAndStaysOnScreen done\n");
}

TEST(remoteFitUsesOneScaleAndNeverCrossesTheSuppliedFontFloor)
{
    const GridSize grid { 200, 60 };
    for (const float backingScale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        const FloatSize cell { 10 * backingScale, 20 * backingScale };
        const FloatSize viewport { 800 * backingScale, 480 * backingScale };
        const auto fitted = Ubiquitty::Rendering::remoteFitPlacement(viewport, cell, grid, 0.25f);
        CHECK(approx(fitted.scale, 0.4f));
        CHECK(approx(fitted.origin.x, 0));
        CHECK(approx(fitted.origin.y, 0));
        const auto limited = Ubiquitty::Rendering::remoteFitPlacement(viewport, cell, grid, 0.5f);
        CHECK(limited.scale == 0.5f);
        CHECK(approx(limited.origin.x, -100 * backingScale));
        CHECK(approx(limited.origin.y, -60 * backingScale));
        const FloatSize drawnCell { cell.width * fitted.scale, cell.height * fitted.scale };
        const CellPosition expected { 31, 127 };
        const Ubiquitty::FloatPoint point { fitted.origin.x + (expected.column + 0.5f) * drawnCell.width,
            fitted.origin.y + (expected.row + 0.5f) * drawnCell.height };
        const auto hit = pointToCell(point, fitted.origin, drawnCell, grid, Ubiquitty::Rendering::RowClamp::Clamped);
        CHECK(hit.row == expected.row);
        CHECK(hit.column == expected.column);
    }
    CHECK(grid.columns == 200);
    CHECK(grid.rows == 60);
}

TEST(remoteFitCentersWithoutEnlargingAndKeepsEmptyGeometryFinite)
{
    const auto fit = Ubiquitty::Rendering::remoteFitPlacement({ 1000, 700 }, { 10, 20 }, { 80, 24 }, 0.5f);
    CHECK(fit.scale == 1);
    CHECK(fit.origin.x == 100);
    CHECK(fit.origin.y == 110);
    const auto empty = Ubiquitty::Rendering::remoteFitPlacement({ 0, 0 }, { 10, 20 }, { 80, 24 }, 0.5f);
    CHECK(empty.scale == 0.5f);
    CHECK(empty.origin.x == -200);
    CHECK(empty.origin.y == -120);
    for (const float invalid : { 0.0f, -1.0f, 2.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
        const auto refused = Ubiquitty::Rendering::remoteFitPlacement({ 400, 240 }, { 10, 20 }, { 80, 24 }, invalid);
        CHECK(refused.scale == 1);
        CHECK(refused.origin.x == 0 && refused.origin.y == 0);
    }
}

TEST(uniformPresentationScalesInkAndDecorationsAlongsideBothCellAxes)
{
    Ubiquitty::Rendering::CellMetrics natural;
    natural.cellWidth = 10;
    natural.cellHeight = 20;
    natural.baselineFromTop = 16;
    natural.underlineThickness = 2;
    natural.underlineDrop = 4;
    natural.underlineThicknessBold = 4;
    natural.underlineDropBold = 6;
    natural.underlineOffset = 2;
    natural.underlineCurlyThickness = 3;
    natural.strikeThickness = 2;
    natural.strikeThicknessBold = 4;
    natural.xHeight = 12;
    natural.skipInkPadding = 2;
    natural.atlasToDevicePixels = 1;
    natural.deviceScale = 2;
    natural.underlineStrokeLift = 1;
    natural.skipInk = true;
    natural.shouldUnderlineFollowInk = true;
    const auto scaled = Ubiquitty::Rendering::scaledCellMetrics(natural, 0.5f);
    CHECK(scaled.cellWidth == 5 && scaled.cellHeight == 10);
    CHECK(scaled.baselineFromTop == 8);
    CHECK(scaled.underlineThickness == 1 && scaled.underlineDrop == 2);
    CHECK(scaled.underlineThicknessBold == 2 && scaled.underlineDropBold == 3);
    CHECK(scaled.underlineOffset == 1 && scaled.underlineCurlyThickness == 1.5f);
    CHECK(scaled.strikeThickness == 1 && scaled.strikeThicknessBold == 2);
    CHECK(scaled.xHeight == 6 && scaled.skipInkPadding == 1);
    CHECK(scaled.atlasToDevicePixels == 0.5f && scaled.deviceScale == 1);
    CHECK(scaled.presentationScale == 0.5f && scaled.underlineStrokeLift == 0.5f);
    CHECK(scaled.skipInk && scaled.shouldUnderlineFollowInk);
    CHECK(natural.cellWidth == 10 && natural.cellHeight == 20);
}

TEST(aNonOwnerPictureHasOneInvertibleTransformForEveryPrimitive)
{
    using namespace Ubiquitty::Rendering;
    RemoteViewportRequest request {
        .policy = RemoteViewportPolicy::Fit, .viewport = { 400, 300 }, .cell = { 10, 20 }, .grid = { 100, 30 }, .cursor = { 20, 80 }, .minimumScale = 0.25f, .isGeometryOwnedElsewhere = true
    };
    const auto fit = remoteViewportPlacement(request);
    CHECK(approx(fit.scale, 0.4f));
    CHECK(approx(fit.origin.x, 0));
    CHECK(approx(fit.origin.y, 30));
    // Cell ink, cursor, image and overlay corners all use this same map.
    for (const Ubiquitty::FloatPoint picture : { Ubiquitty::FloatPoint { 0, 0 }, { 1000, 600 }, { 805, 410 }, { 137, 281 } }) {
        const auto inverse = fit.picturePoint(fit.viewportPoint(picture));
        CHECK(approx(inverse.x, picture.x));
        CHECK(approx(inverse.y, picture.y));
    }
    const auto point = fit.viewportPoint({ 805, 410 });
    const auto cell = pointToCell(fit.picturePoint(point), {}, request.cell, request.grid, RowClamp::Clamped);
    CHECK_EQ(cell.row, 20);
    CHECK_EQ(cell.column, 80);
    const auto pixel = pointToDevicePixel(fit.picturePoint(point), {}, 1, { 1000, 600 });
    CHECK_EQ(pixel.x, 806);
    CHECK_EQ(pixel.y, 411);
    request.minimumScale = 0.5f;
    const auto floor = remoteViewportPlacement(request);
    CHECK_EQ(floor.scale, 0.5f);
    CHECK_EQ(floor.origin.x, -50);
    CHECK_EQ(floor.origin.y, 0);
    CHECK_EQ(request.grid.columns, 100);
    CHECK_EQ(request.grid.rows, 30);
}

TEST(aNonOwnerPanFollowsOnlyTheVisibleLiveCursor)
{
    using namespace Ubiquitty::Rendering;
    RemoteViewportRequest request {
        .policy = RemoteViewportPolicy::Pan, .viewport = { 400, 300 }, .cell = { 10, 20 }, .grid = { 100, 30 }, .cursor = { 20, 80 }, .isGeometryOwnedElsewhere = true
    };
    const auto live = remoteViewportPlacement(request);
    CHECK_EQ(live.scale, 1);
    CHECK_EQ(live.origin.x, -410);
    CHECK_EQ(live.origin.y, -120);
    request.previousPanOffset = live.panOffset;
    request.cursor = { 0, 0 };
    request.shouldFollowCursor = false;
    const auto history = remoteViewportPlacement(request);
    CHECK_EQ(history.origin.x, live.origin.x);
    CHECK_EQ(history.origin.y, live.origin.y);
    request.shouldFollowCursor = true;
    const auto tail = remoteViewportPlacement(request);
    CHECK_EQ(tail.origin.x, 0);
    CHECK_EQ(tail.origin.y, 0);
}

TEST(aPictureOwnerAndProgramViewportKeepTheirOwnGeometry)
{
    using namespace Ubiquitty::Rendering;
    RemoteViewportRequest request {
        .policy = RemoteViewportPolicy::Letterbox, .viewport = { 400, 300 }, .cell = { 10, 20 }, .grid = { 100, 30 }, .isGeometryOwnedElsewhere = true
    };
    const auto letterbox = remoteViewportPlacement(request);
    CHECK_EQ(letterbox.scale, 1);
    CHECK_EQ(letterbox.origin.x, -300);
    CHECK_EQ(letterbox.origin.y, -150);
    request.isGeometryOwnedElsewhere = false;
    const auto owner = remoteViewportPlacement(request);
    CHECK_EQ(owner.scale, 1);
    CHECK_EQ(owner.origin.x, 0);
    CHECK_EQ(owner.origin.y, 0);
    request.isGeometryOwnedElsewhere = true;
    request.isProgramViewport = true;
    request.policy = RemoteViewportPolicy::Fit;
    request.minimumScale = 0.25f;
    const auto program = remoteViewportPlacement(request);
    CHECK_EQ(program.scale, 1);
    CHECK_EQ(program.origin.x, 0);
    CHECK_EQ(program.origin.y, 0);
    request.isProgramViewport = false;
    request.viewport.width = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = remoteViewportPlacement(request);
    CHECK_EQ(invalid.scale, 1);
    CHECK_EQ(invalid.origin.x, 0);
    CHECK_EQ(invalid.origin.y, 0);
}

UBIQUITTY_TEST_MAIN()
