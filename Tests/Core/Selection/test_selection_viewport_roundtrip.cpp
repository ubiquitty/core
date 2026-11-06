// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Base/Geometry.h"

// test_selection_viewport_roundtrip.cpp — pins the visible-cell round-trip that
// the live drag-selection relies on: the pointer resolves to a VISIBLE (row,col),
// the gesture stores it in ABSOLUTE buffer space (scrollback + screen), and the
// renderer maps it back to a VISIBLE row to lay the highlight quad. If the encode
// and decode disagree on how scrollback / viewOffset fold into the row, the
// highlight drifts off the pointer — the rectangular-selection-offset class of bug.
//
// The two ends modelled here are byte-faithful to the shipping code:
//   ENCODE — the Swift surface's selection:
//            absolute = visibleRow + history - viewOffset, with history forced to
//            0 on the alternate screen.
//   DECODE — the Swift surface renderer + OverlayGeometry::cellSpanQuad
//            visibleRow = absolute - history + viewOffset, with history read from
//            the render snapshot's scrollbackCount.
//
// The real Selection model (begin/update/getBoundsWide/rowSpan) sits between them,
// in char and rectangular mode, so a mode-specific divergence would surface as a
// failed round-trip. Links Selection.cpp + OverlayGeometry.cpp; no Terminal, no AppKit.

#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/PaddingGeometry.h"
#include "Core/Selection/Selection.h"

#include <cstdio>

// A geometry + scroll state shared by encode and decode. `history` is the live
// scrollbackCount; `viewOffset` is how far the viewport is scrolled up into it
// (0 == live tail). `onAlternate` mirrors isUsingAlternateScreen: the gesture forces history to 0
// there (the encode side), while the renderer still reads the raw snapshot
// scrollbackCount (the decode side) — exactly the live asymmetry.
struct ViewState {
    int rows;
    int columns;
    int history; // scrollbackCount
    int viewOffset; // 0 == live tail, >0 == scrolled into history
    bool onAlternate;
    float cellWidth;
    float cellHeight;
    Ubiquitty::FloatPoint origin;
};

// ENCODE: visible row → absolute row, exactly as the live gesture / caret paths
// do. history is zeroed on the alternate screen
// (`isUsingAlternateScreen ? 0 : scrollbackCount`).
static int encodeAbsoluteRow(const ViewState& v, int visibleRow)
{
    const int history = v.onAlternate ? 0 : v.history;
    return visibleRow + history - v.viewOffset;
}

// DECODE: absolute row → visible row, as UBRenderer buildSelectionOverlays maps
// it. history mirrors the ENCODE side — zeroed on the alternate screen so the
// round-trip is exact. The raw snapshot scrollbackCount is the primary's
// surviving count, and reading it here drifts every alternate-screen highlight
// off the pointer. Returns -1 when the row is off-screen.
static int decodeVisibleRow(const ViewState& v, int absoluteRow)
{
    const int history = v.onAlternate ? 0 : v.history;
    const int vrow = absoluteRow - history + v.viewOffset;
    if (vrow < 0 || vrow >= v.rows)
        return -1;
    return vrow;
}

// Run one drag: press at visible (r0,c0), drag to visible (r1,c1) in `mode`, then
// resolve the highlight for every screen row and confirm each highlighted quad
// lands on the visible cells the pointer swept. Returns the highlighted visible
// rows + their column spans via the out-params for the caller to assert.
struct ResolvedSpan {
    int vrow;
    float minX, maxX, minY, maxY;
};

// Cell accessor backing the on-screen rows with solid content (every cell a word
// byte), so char-mode's lastPrintedColumn rule reports a full row and never
// void-snaps a mid-row selection to whole lines. Absolute row → visible row uses
// the same firstAbsoluteRow the renderer's snapshot accessor does. Off-screen reads are
// blank (the viewport is all that's painted).
struct ContentGrid {
    int rows;
    int columns;
    int firstAbsoluteRow;
};
static uint32_t contentCellAt(void* p, int absoluteRow, int column)
{
    auto* g = static_cast<ContentGrid*>(p);
    const int vr = absoluteRow - g->firstAbsoluteRow;
    if (vr < 0 || vr >= g->rows || column < 0 || column >= g->columns)
        return 0;
    return (uint16_t)'x'; // a word byte → printed cell on every column
}

static void runDrag(const ViewState& v, Ubiquitty::SelectionMode mode, int r0, int c0, int r1, int c1, ResolvedSpan* out, int* outCount)
{
    const int totalRows = v.history + v.rows; // gopTotalRows (absolute space)
    const int decodeHistory = v.onAlternate ? 0 : v.history; // FIXED decode: matches encode on alternate
    ContentGrid grid { v.rows, v.columns, decodeHistory - v.viewOffset };

    Ubiquitty::SelectionState sel;
    Ubiquitty::Selection::init(&sel);
    // gopBeginCharacter(begin) at first drag, then gopUpdate(update) — both convert
    // the visible row through encodeAbsoluteRow, mirroring the gesture ops.
    Ubiquitty::Selection::begin(&sel, totalRows, v.columns, encodeAbsoluteRow(v, r0), c0, mode);
    Ubiquitty::Selection::update(&sel, totalRows, v.columns, encodeAbsoluteRow(v, r1), c1);

    Ubiquitty::SelectionBounds b;
    const bool ok = Ubiquitty::Selection::getBoundsWide(&sel, totalRows, v.columns, contentCellAt, &grid, /*attributesAt*/ nullptr, nullptr, &b,
        /*rowWrapped*/ nullptr, nullptr);
    CHECK(ok);

    int n = 0;
    for (int r = b.row0; r <= b.row1; ++r) {
        const int vrow = decodeVisibleRow(v, r);
        if (vrow < 0)
            continue; // scrolled out of the viewport
        Ubiquitty::Selection::RowSpan rs = Ubiquitty::Selection::rowSpan(&b, sel.mode, r, v.columns, contentCellAt, &grid, /*attributesAt*/ nullptr, nullptr);
        if (!rs.selected || rs.column1 < rs.column0)
            continue;
        Ubiquitty::Rendering::OverlayQuad q = Ubiquitty::Rendering::cellSpanQuad(rs.column0, rs.column1, vrow, { v.cellWidth, v.cellHeight }, v.origin, { 0, 0, 0, 1 });
        out[n].vrow = vrow;
        out[n].minX = q.minX;
        out[n].maxX = q.maxX;
        out[n].minY = q.minY;
        out[n].maxY = q.maxY;
        ++n;
    }
    *outCount = n;
}

// The pixel rect the POINTER swept for visible cell (vrow, col): this is what the
// highlight must sit under. Same forward map cellSpanQuad uses.
static void pointerCellRect(const ViewState& v, int vrow, int col, float* minX, float* maxX, float* minY, float* maxY)
{
    *minX = col * v.cellWidth + v.origin.x;
    *maxX = (col + 1) * v.cellWidth + v.origin.x;
    *minY = vrow * v.cellHeight + v.origin.y;
    *maxY = (vrow + 1) * v.cellHeight + v.origin.y;
}

static bool approx(float a, float b) { return (a - b < 1e-3f) && (b - a < 1e-3f); }

// A single-cell drag (press and release on the same visible cell) must highlight
// exactly that visible cell — for both modes, on the primary buffer with
// scrollback present. This is the heart of the reported bug: the highlight under
// the pointer.
TEST(testSingleCellPrimaryScrollback)
{
    ViewState v { /*rows*/ 24, /*columns*/ 80, /*history*/ 500, /*viewOffset*/ 0,
        /*onAlternate*/ false, /*cellWidth*/ 9.0f, /*cellHeight*/ 18.0f, /*origin*/ { 4.0f, 6.0f } };

    const int vr = 10, vc = 30;
    float pMinX, pMaxX, pMinY, pMaxY;
    pointerCellRect(v, vr, vc, &pMinX, &pMaxX, &pMinY, &pMaxY);

    for (Ubiquitty::SelectionMode mode : { Ubiquitty::SelectionMode::Character, Ubiquitty::SelectionMode::Rectangle }) {
        ResolvedSpan spans[64];
        int count = 0;
        runDrag(v, mode, vr, vc, vr, vc, spans, &count);
        CHECK(count == 1);
        if (count == 1) {
            CHECK(spans[0].vrow == vr);
            CHECK(approx(spans[0].minX, pMinX));
            CHECK(approx(spans[0].maxX, pMaxX));
            CHECK(approx(spans[0].minY, pMinY));
            CHECK(approx(spans[0].maxY, pMaxY));
        }
    }
    printf("testSingleCellPrimaryScrollback done\n");
}

// signedCellAtWindowPoint (rect drag's row sampler) vs cellAtWindowPoint (regular
// path's sampler) must resolve the same window point to the same VISIBLE row when
// the point is inside the grid — they differ only in gridFloor a point that has
// left the top/bottom edge. A column shift here would offset the rect selection.
TEST(testSignedVsClampedResolveAgree)
{
    using Ubiquitty::CellPosition;
    using Ubiquitty::Rendering::pointToCell;
    const float originX = 4.0f, originY = 6.0f, cellWidth = 9.0f, cellHeight = 18.0f;
    const int columns = 80, rows = 24;

    // Sweep interior points: both clampRow settings must agree on row and column.
    for (int gx = 0; gx < columns * (int)cellWidth; gx += 7) {
        for (int gy = 0; gy < rows * (int)cellHeight; gy += 11) {
            const float px = originX + gx, py = originY + gy;
            CellPosition clamped = pointToCell({ px, py }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
            CellPosition signedC = pointToCell({ px, py }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Unclamped);
            CHECK(clamped.row == signedC.row);
            CHECK(clamped.column == signedC.column);
        }
    }
    // A point above the grid: the column still agrees; only the row clamps differ
    // (the auto-scroll signal), which is the intended divergence.
    CellPosition above = pointToCell({ originX + 35.0f, originY - 40.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Unclamped);
    CellPosition aboveClamped = pointToCell({ originX + 35.0f, originY - 40.0f }, { originX, originY }, { cellWidth, cellHeight }, { columns, rows }, Ubiquitty::Rendering::RowClamp::Clamped);
    CHECK(above.column == aboveClamped.column); // column never differs
    CHECK(above.row < 0); // signed leaks negative
    CHECK(aboveClamped.row == 0); // clamped pins to 0
    printf("testSignedVsClampedResolveAgree done\n");
}

// Multi-row block on the primary buffer, scrolled up into history (viewOffset>0).
// Every highlighted visible row must align to the cells the pointer covered, and
// the rectangular block must keep the same column span on every row.
TEST(testRectBlockScrolledIntoHistory)
{
    ViewState v { 24, 80, 500, /*viewOffset*/ 7, false, 9.0f, 18.0f, { 4.0f, 6.0f } };
    const int vr0 = 4, vc0 = 10, vr1 = 9, vc1 = 25;

    ResolvedSpan spans[64];
    int count = 0;
    runDrag(v, Ubiquitty::SelectionMode::Rectangle, vr0, vc0, vr1, vc1, spans, &count);
    CHECK(count == (vr1 - vr0 + 1)); // every swept visible row is highlighted

    for (int i = 0; i < count; ++i) {
        const int vrow = vr0 + i;
        CHECK(spans[i].vrow == vrow);
        // Column block: identical [vc0,vc1] span on every row, under the pointer.
        const float wantMinX = vc0 * v.cellWidth + v.origin.x;
        const float wantMaxX = (vc1 + 1) * v.cellWidth + v.origin.x;
        const float wantMinY = vrow * v.cellHeight + v.origin.y;
        const float wantMaxY = (vrow + 1) * v.cellHeight + v.origin.y;
        CHECK(approx(spans[i].minX, wantMinX));
        CHECK(approx(spans[i].maxX, wantMaxX));
        CHECK(approx(spans[i].minY, wantMinY));
        CHECK(approx(spans[i].maxY, wantMaxY));
    }
    printf("testRectBlockScrolledIntoHistory done\n");
}

// Regression guard for alternate-screen scrollback: the gesture encodes with history
// forced to 0 (gopHistory's isUsingAlternateScreen branch), and the renderer must decode
// with history zeroed on alternate too, although the snapshot's scrollbackCount is not
// (the primary's scrollback survives the screen switch). Decoding with the raw count while
// the underlying primary has scrollback drifts the highlight by `history` rows —
// off-screen here. A full-screen alternate app over a FRESH shell (history 0) is safe
// either way, which is why the gallery cannot show the drift.
TEST(testAlternateScreenWithPrimaryScrollback)
{
    ViewState v { 24, 80, /*history*/ 500, /*viewOffset*/ 0, /*onAlternate*/ true, 9.0f, 18.0f, { 4.0f, 6.0f } };
    const int vr = 12, vc = 40;
    float pMinX, pMaxX, pMinY, pMaxY;
    pointerCellRect(v, vr, vc, &pMinX, &pMaxX, &pMinY, &pMaxY);

    ResolvedSpan spans[64];
    int count = 0;
    runDrag(v, Ubiquitty::SelectionMode::Rectangle, vr, vc, vr, vc, spans, &count);
    // Decode also zeroes history on alternate, so the cell is highlighted
    // exactly under the pointer.
    CHECK(count == 1);
    if (count == 1) {
        CHECK(spans[0].vrow == vr);
        CHECK(approx(spans[0].minX, pMinX));
        CHECK(approx(spans[0].minY, pMinY));
    }
    printf("testAltScreenWithPrimaryScrollback done\n");
}

UBIQUITTY_TEST_MAIN()
