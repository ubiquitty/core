// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_display.cpp — CellGrid class in isolation.
//
// Covers these bug classes:
//
//   - Shadow drift after partial paint: SetCell + paint of run A
//     followed by writes in run B must not falsely sync run B's
//     shadow on the first paint.
//   - Scroll desync: ScrollRegion must shift m_bbShadow in lockstep
//     with the queued PendingScroll's AckScroll.
//   - Intra-row gap leak: a row with sparse dirty runs must yield
//     each run separately so the PAL's flushRect equals the painted
//     area exactly.
//   - alternate-screen toggle / clear: cells_ == blank but m_bbShadow holds
//     old content must surface via the iterator.
//   - selection-only flips: cells unchanged but visual repaint
//     needed must surface with selectionOnly=true.
//
// Tests are white-box: a real PAL would only expose painted pixels,
// and the property these bugs violate is internal bookkeeping, so the
// tests drive the CellGrid API and assert on iteration
// output. A "virtual BB" Cell array tracks what AckRun would have
// painted, and the final assertion is that virtualBB == cells_ for
// every cell — which is the property each of these corruption bugs
// violates.

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

using namespace Ubiquitty;

// Property check: after every paint cycle, the row-dirty hint must be
// consistent with the back-buffer shadow + cell + selection state. The
// underlying CellGrid::assertInvariants is only compiled in debug; the
// macro degrades to a no-op under NDEBUG so the tests still build.
#ifndef NDEBUG
#define TEST_ASSERT_DISPLAY_INVARIANTS(d)                                                   \
    do {                                                                                    \
        if (!(d).assertInvariants()) {                                                      \
            fprintf(stderr, "FAIL: %s:%d display invariants broken\n", __FILE__, __LINE__); \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)
#else
#define TEST_ASSERT_DISPLAY_INVARIANTS(d) ((void)0)
#endif

static Cell makeBlank()
{
    Cell c;
    c = {};
    c.codePoint = 0;
    c.pen.foregroundIndex = colorDefault;
    c.pen.backgroundIndex = colorDefault;
    c.pen.underlineIndex = colorDefault; // matches penReset + CellGrid's seed blank
    return c;
}

static Cell makeCharacter(uint16_t ch)
{
    Cell c = makeBlank();
    c.codePoint = ch;
    return c;
}

/// Simulate one paint cycle as the PAL would: drain scrolls, then
/// iterate runs, ack each. Mirror what the PAL writes to BB in a
/// shadow array we control so the test can verify (virtualBB ==
/// cells_) afterwards.
static void drainPaint(CellGrid& d, Cell virtualBB[maxRows][maxColumns])
{
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc)) {
        /// PAL would blit BB pixels; here we shift virtualBB.
        const int top = sc.topRow.value();
        const int bot = sc.botRow.value();
        const int shift = sc.shiftRows > 0 ? sc.shiftRows : -sc.shiftRows;
        if (shift > 0 && shift <= bot - top) {
            if (sc.shiftRows > 0) {
                for (int r = top; r <= bot - shift; ++r)
                    memcpy(virtualBB[r], virtualBB[r + shift], maxColumns * sizeof(Cell));
            } else {
                for (int r = bot; r >= top + shift; --r)
                    memcpy(virtualBB[r], virtualBB[r - shift], maxColumns * sizeof(Cell));
            }
        }
        ses.ackScroll(sc);
    }
    PaintRun run;
    while (ses.nextDirtyRun(&run)) {
        /// PAL paints glyphs; here we copy cells into virtualBB.
        const int r = run.row.value();
        const int c0 = run.firstColumn.value();
        const int c1 = run.lastColumn.value();
        for (int c = c0; c <= c1; ++c)
            virtualBB[r][c] = *d.cellAt(Row(r), Column(c));
        ses.ackRun(run);
    }
    // Property: after a fully drained + acked paint, no row's dirty hint
    // still claims work the grid no longer has.
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// True iff virtualBB matches cells_ everywhere in the active area.
static int countMismatches(const CellGrid& d, const Cell vbb[maxRows][maxColumns])
{
    int n = 0;
    for (int r = 0; r < d.rows(); ++r) {
        for (int c = 0; c < d.columns(); ++c) {
            const Cell* a = d.cellAt(Row(r), Column(c));
            if (a && memcmp(a, &vbb[r][c], sizeof(Cell)) != 0)
                ++n;
        }
    }
    return n;
}

static void initVBB(Cell vbb[maxRows][maxColumns], const Cell& fill)
{
    for (int r = 0; r < maxRows; ++r)
        for (int c = 0; c < maxColumns; ++c)
            vbb[r][c] = fill;
}

/// Owns one blank-filled virtual back-buffer.
///
/// `Cell` is not trivially default-constructible, so every
/// `new Cell[maxRows][maxColumns]` expression costs about 0.3 seconds of
/// compile-time constant evaluation — one initialization per element, across
/// the whole grid. Tests allocate through this class so the translation unit
/// pays that once.
class VirtualBackBuffer {
public:
    explicit VirtualBackBuffer(const Cell& fill)
        : m_cells(new Cell[maxRows][maxColumns])
    {
        initVBB(m_cells.get(), fill);
    }

Cell (*get()) [maxColumns] { return m_cells.get(); }

private : std::unique_ptr<Cell[][maxColumns]> m_cells;
};

/// ---------- tests -------------------------------------------------

/// A paint that leaves a yielded run unacked is detected.
///
/// `CellGrid` computes this on every PaintSession teardown — `fullySynced =
/// (m_yieldedRuns == m_ackedRuns)`, folded into `m_paintInvariantBroken` — and
/// this case reads the answer. A detector that runs on every paint in every
/// debug build and whose verdict nobody reads is the one state a correctness
/// check must not be in: the cost is paid and the benefit is not taken.
///
/// What it guards is named at the computation: a run the PAL never acks leaves
/// `m_bbShadow` describing pixels that were never drawn, so the next paint's
/// shadow-compare skips cells that differ and the screen keeps stale ink.
///
/// Both arms, on independent grids because the flag is sticky by design — a
/// clean paint after a leaky one must not clear it, so the arms cannot share a
/// grid and still mean anything.
TEST(testAnUnackedPaintRunBreaksThePaintInvariant)
{
    {
        std::unique_ptr<CellGrid> clean(new CellGrid());
        clean->init(24, 80);
        VirtualBackBuffer vbbStorage(makeBlank());
        Cell(*vbb)[maxColumns] = vbbStorage.get();
        clean->setCell(Row(5), Column(10), makeCharacter('A'));
        drainPaint(*clean, vbb);
        // The null. A paint that acked every run must leave the flag clear;
        // without measuring that, the arm below would read the same whether the
        // detector reports a leak or reports true for every paint.
        CHECK(!clean->paintInvariantBroken());
    }

    {
        std::unique_ptr<CellGrid> leaky(new CellGrid());
        leaky->init(24, 80);
        leaky->setCell(Row(5), Column(10), makeCharacter('A'));
        leaky->setCell(Row(6), Column(10), makeCharacter('B'));
        int yielded = 0;
        {
            PaintSession session = leaky->beginPaint();
            Screen::PendingScroll scroll;
            while (session.nextPendingScroll(&scroll))
                session.ackScroll(scroll);
            PaintRun run;
            while (session.nextDirtyRun(&run)) {
                ++yielded;
                // Every run but the first is acked, so the mismatch is exactly
                // one — the smallest leak the counter can see.
                if (yielded > 1)
                    session.ackRun(run);
            }
        }
        CHECK(yielded >= 1); // the grid really produced work to leave unacked
        CHECK(leaky->paintInvariantBroken());
    }
}

TEST(testSetCellPaintThenMatch)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    Cell A = makeCharacter('A');
    d.setCell(Row(5), Column(10), A);
    drainPaint(d, vbb);
    CHECK(countMismatches(d, vbb) == 0);
}

TEST(testIdempotentSetNoRepaint)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Establish a known cell value, paint it (shadow now matches).
    Cell A = makeCharacter('A');
    d.setCell(Row(3), Column(7), A);
    drainPaint(d, vbb);
    // Now set the same cell to the same value — no dirty work.
    d.setCell(Row(3), Column(7), A);
    int yielded = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 0);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testEraseRowRangeOnlyDirtiesChanged)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Sparse content row: 'foo' at 0..2, 'bar' at 10..12, blanks
    // between.
    d.setCell(Row(4), Column(0), makeCharacter('f'));
    d.setCell(Row(4), Column(1), makeCharacter('o'));
    d.setCell(Row(4), Column(2), makeCharacter('o'));
    d.setCell(Row(4), Column(10), makeCharacter('b'));
    d.setCell(Row(4), Column(11), makeCharacter('a'));
    d.setCell(Row(4), Column(12), makeCharacter('r'));
    drainPaint(d, vbb);
    CHECK(countMismatches(d, vbb) == 0);
    // EraseRowRange across the whole row. Cells 3..9 + 13..79 were
    // already blank — those don't change. Cells 0..2 + 10..12 do.
    Cell blank = makeBlank();
    d.eraseRowRange(Row(4), Column(0), Column(79), blank);
    /// Iterate: must yield two runs, [0..2] and [10..12].
    int yielded = 0;
    int firstColumns[8] = { -1 };
    int lastColumns[8] = { -1 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (yielded < 8) {
                firstColumns[yielded] = run.firstColumn.value();
                lastColumns[yielded] = run.lastColumn.value();
            }
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 2);
    CHECK(firstColumns[0] == 0 && lastColumns[0] == 2);
    CHECK(firstColumns[1] == 10 && lastColumns[1] == 12);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testScrollShiftsShadowInLockstep)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    Cell blank = makeBlank();
    // Fill some rows with distinct content.
    for (int r = 5; r < 10; ++r) {
        Cell c = makeCharacter(uint16_t('A' + r));
        d.setCell(Row(r), Column(0), c);
    }
    drainPaint(d, vbb);
    CHECK(countMismatches(d, vbb) == 0);
    // Scroll the whole screen up by 1. cells_ shifts. The paint
    // session yields a pending scroll first (virtualBB shifts on
    // AckScroll), then any newly-exposed bottom row as dirty.
    d.scrollRegion(Row(0), Row(23), 1, blank);
    drainPaint(d, vbb);
    // virtualBB should now exactly match cells_ after the shift +
    // bottom row blank.
    CHECK(countMismatches(d, vbb) == 0);
}

TEST(testDeferredBlankSurvivesMultiLineScroll)
{
    /// Regression guard for the deferred recycle blank (CellGrid.h
    /// m_deferredBlankSlot): a single-line full-viewport scroll DEFERS the
    /// recycled row's physical blank; a multi-line scroll then moves
    /// m_rowOrigin again without touching that row. The deferred record is
    /// physical-slot-addressed, so origin moves must not re-aim it — every
    /// read afterwards must still see logically blank cells, never the
    /// scrolled-away line's stale bytes.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    // Fill every row full-width with distinct content so the recycled
    // rows carry a wide stale prefix.
    for (int r = 0; r < 24; ++r)
        for (int c = 0; c < 80; ++c)
            d.setCell(Row(r), Column(c), makeCharacter(uint16_t('A' + (r % 26))));
    // One-line scroll: defers the blank for the recycled bottom row.
    d.scrollRegion(Row(0), Row(23), 1, blank);
    // Multi-line scroll while the deferral is still pending.
    d.scrollRegion(Row(0), Row(23), 3, blank);
    // The four revealed bottom rows must read blank cell-by-cell.
    for (int r = 20; r < 24; ++r)
        for (int c = 0; c < 80; ++c) {
            const Cell* cell = d.cellAt(Row(r), Column(c));
            CHECK(cell && cell->codePoint == 0);
        }
    // And one more single-line scroll + write resolves cleanly: the new
    // bottom row shows exactly the written prefix, blanks beyond it.
    d.scrollRegion(Row(0), Row(23), 1, blank);
    for (int c = 0; c < 10; ++c)
        d.setCell(Row(23), Column(c), makeCharacter(uint16_t('z')));
    for (int c = 10; c < 80; ++c) {
        const Cell* cell = d.cellAt(Row(23), Column(c));
        CHECK(cell && cell->codePoint == 0);
    }
}

TEST(testDeferredBlankSurvivesBandedScroll)
{
    /// The sibling above covers a deferral crossing a FULL-VIEWPORT scroll, which
    /// moves m_rowOrigin. This covers one crossing a BANDED scroll, which does not:
    /// a scroll region with top != 0 takes the generic branch, and that branch
    /// reorders m_rowPtrs with memmove rather than by moving the origin.
    ///
    /// Its only guard is normalizeRowRing, which returns early when the origin is
    /// already 0 — the ring needs no rotating, so it resolves nothing. But origin 0
    /// with a deferral outstanding is ordinary: every m_rows-th full-viewport scroll
    /// wraps the origin back to 0 and records a fresh deferral in the same step. The
    /// record is physical-slot-addressed, so the memmove re-aims it at whichever row
    /// lands in that slot.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();

    // 24 single-line full-viewport scrolls: the origin wraps 0 -> 23 -> 0, ending
    // where it started with the last scroll's blank still deferred. Each iteration
    // writes the row that is about to recycle, so the deferral has a stale prefix
    // to hide.
    for (int i = 0; i < 24; ++i) {
        for (int c = 0; c < 80; ++c)
            d.setCell(Row(23), Column(c), makeCharacter(static_cast<uint16_t>('a' + (i % 26))));
        d.scrollRegion(Row(0), Row(23), 1, blank);
    }

    // DECSTBM 2;24 then SU 3 — top != 0, so the generic branch runs.
    d.scrollRegion(Row(1), Row(23), 3, blank);

    // The rows the band revealed must read blank. Unresolved, the deferral either
    // blanks a row that should hold content or leaves this one showing the stale
    // prefix it was recorded to erase.
    // Row 23 was logically blank going in — its blank was the deferred one — and a
    // three-row scroll of the band carries it to row 20. Row 20 must therefore read
    // blank. Unresolved, the record still names slot 23, so reading row 20 never
    // triggers it and the row hands back the prefix it was recorded to erase.
    for (int c = 0; c < 80; ++c) {
        const Cell* cell = d.cellAt(Row(20), Column(c));
        CHECK(cell && cell->codePoint == 0);
    }
    // The three rows the band recycled are blanked by the scroll itself, so they
    // hold either way: they are here to show the case is not "everything is
    // blank", which would pass without testing anything.
    for (int r = 21; r < 24; ++r)
        for (int c = 0; c < 80; ++c) {
            const Cell* cell = d.cellAt(Row(r), Column(c));
            CHECK(cell && cell->codePoint == 0);
        }
    // And the rows above the band's reach still hold their content.
    for (int c = 0; c < 80; ++c) {
        const Cell* cell = d.cellAt(Row(0), Column(c));
        CHECK(cell && cell->codePoint != 0);
    }
}

TEST(testClearAfterContentNoLeak)
{
    /// Reproduces the user's ls -la → clear scenario. Cells filled
    /// with content, paint, then full-screen erase, paint again.
    /// virtualBB must show blanks everywhere — no leaks.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Fill rows 0..15 with sparse content.
    for (int r = 0; r < 16; ++r) {
        for (int c = 30; c < 50; ++c)
            d.setCell(Row(r), Column(c), makeCharacter(uint16_t('A' + (c - 30))));
    }
    drainPaint(d, vbb);
    // Now erase every row (the Ubiquitty::Screen::eraseInDisplay(2) equivalent).
    Cell blank = makeBlank();
    for (int r = 0; r < d.rows(); ++r)
        d.eraseRow(Row(r), blank);
    drainPaint(d, vbb);
    // Every cell must read as blank in virtualBB and cells_.
    CHECK(countMismatches(d, vbb) == 0);
    for (int r = 0; r < d.rows(); ++r)
        for (int c = 0; c < d.columns(); ++c)
            CHECK(vbb[r][c].codePoint == 0);
}

TEST(testSelectionOnlyRun)
{
    /// Selection-state flip on cells whose content didn't change
    /// yields a selectionOnly=true run.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Put some content + paint.
    for (int c = 0; c < 10; ++c)
        d.setCell(Row(5), Column(c), makeCharacter(uint16_t('a' + c)));
    drainPaint(d, vbb);
    // No content change: just flip selection on columns 2..7.
    d.markSelectionFlipped(Row(5), Column(2), Column(7));
    int yielded = 0;
    bool sawSelectionOnly = false;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++yielded;
            if (run.selectionOnly)
                sawSelectionOnly = true;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 1);
    CHECK(sawSelectionOnly);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testResizeInvalidatesShadow)
{
    /// Resize after paint must cause every cell to be treated as
    /// dirty on the next paint (BB pixels are unreliable post-resize).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    d.setCell(Row(0), Column(0), makeCharacter('X'));
    drainPaint(d, vbb);
    // No content has changed yet, but after a Resize the shadow is
    // invalid and the next paint sees every cell as dirty.
    d.resize(25, 80);
    int yielded = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++yielded;
            ses.ackRun(run);
        }
    }
    // >0 runs (every row of every column == many runs).
    CHECK(yielded > 0);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// resize() must clear m_anyDirtyInRow entries past the new
/// row count, otherwise a later grow-back surfaces rows as dirty
/// without a mutation having set them — and assertInvariants() flags
/// the stale-hint drift.
TEST(testResizeShrinkThenGrowNoStaleDirty)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(50, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    // Dirty a row that will be lost in the shrink.
    d.setCell(Row(40), Column(0), makeCharacter('Z'));
    drainPaint(d, vbb);
    // Shrink: row 40 is gone, but the row-dirty hint at index 40
    // must also be false now or assertInvariants would see drift
    // after grow-back (active vs shadow mismatch impossible because
    // resize invalidates shadow, but the row-clean hint must still
    // be consistent post-paint).
    d.resize(24, 80);
    drainPaint(d, vbb);
    // Grow back. Any row that was untouched between the shrink and
    // the grow must reflect the synced shadow — no stale hint.
    d.resize(50, 80);
    drainPaint(d, vbb);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// setRowSpan must not walk past the caller's span buffer
/// when a deeply-negative firstColumn would shift src past span.count:
/// it bails when shift >= span.count rather than doing src += -lo
/// unchecked. We can't directly detect an OOB read in a
/// portable way, but exercising the path under -fsanitize=address
/// would surface it; here we assert the call returns cleanly and
/// doesn't mutate any cells.
TEST(test_setRowSpan_deeply_negative_firstColumn)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    Cell span[4];
    for (int i = 0; i < 4; ++i)
        span[i] = makeCharacter(uint16_t('A' + i));
    // firstColumn = -100, count = 4. Compensation would do src += 100
    // — way past the 4-cell span. The call must bail.
    RowView view;
    view.data = span;
    view.count = 4;
    d.setRowSpan(Row(3), Column(-100), view);
    // Row 3 should still match the blank baseline.
    for (int c = 0; c < 80; ++c) {
        const Cell* got = d.cellAt(Row(3), Column(c));
        CHECK(got != nullptr);
        CHECK(got->codePoint == 0);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// cellRangeRect right-edge pixel value must fit in a short for any
/// frontend whose coordinates are signed 16-bit. A frontend with float
/// coordinates (the macOS/Metal renderer) has no such limit. Pin a 240-column
/// worst case explicitly rather than the compiled maxColumns. The runtime assert
/// inside cellRangeRect is debug-only; this pre-flights the worst case.
TEST(test_cellRangeRect_no_short_overflow)
{
    /// 240-column reference ceiling. Worst-case charWidth at 24pt Monaco
    /// ≈ 14; leave headroom and try 32. m_marginLeft is typically 4.
    const int referenceMaxColumns = 240;
    const int marginLeft = 4;
    const int charWidth = 32;
    const int lastColumn = referenceMaxColumns - 1;
    const int rightInPixels = marginLeft + (lastColumn + 1) * charWidth;
    CHECK(rightInPixels < 32768); // 4 + 240*32 = 7684 — fits easily
    /// Confirm overflow boundary: a hypothetical charWidth that would
    /// overflow is detectable arithmetically.
    const int huge = 200; // would yield 4 + 240*200 = 48004 > 32767
    const int wouldOverflow = marginLeft + (lastColumn + 1) * huge;
    CHECK(wouldOverflow >= 32768);
}

TEST(testIntraRowSparseRunsHaveCorrectBounds)
{
    /// Regression: a band/run union of sparse dirty cells that produces one
    /// wide flush rect includes blank gaps, and the gaps leak stale BB pixels. PaintSession must yield
    /// each run separately so the PAL's flushRect equals painted
    /// area exactly.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    // Set cells (5, 3), (5, 10), (5, 20). Three runs.
    d.setCell(Row(5), Column(3), makeCharacter('X'));
    d.setCell(Row(5), Column(10), makeCharacter('Y'));
    d.setCell(Row(5), Column(20), makeCharacter('Z'));
    int yielded = 0;
    int firsts[8] = { -1 };
    int lasts[8] = { -1 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (yielded < 8) {
                firsts[yielded] = run.firstColumn.value();
                lasts[yielded] = run.lastColumn.value();
            }
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 3);
    CHECK(firsts[0] == 3 && lasts[0] == 3);
    CHECK(firsts[1] == 10 && lasts[1] == 10);
    CHECK(firsts[2] == 20 && lasts[2] == 20);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// Regression: an emoji cell adjacent
/// to a text cell must split the dirty band into separate runs.
/// Attribute::Emoji is in attrRunBreakMask, so a row
/// like [ T T E T T E T T ] does not coalesce into one run and the renderer
/// does not pay 4× RGB{Fore,Back}Color per emoji inside a merged run.
/// Each emoji boundary breaks the run, so the renderer pays the
/// Pause/Resume cost once per emoji cell at run-edge granularity.
///
/// Same-pen plain-text adjacency must still coalesce (sanity baseline).
TEST(testEmojiBreaksRun)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    // Baseline: 8 plain text cells with identical pen → single run.
    for (int c = 0; c < 8; ++c)
        d.setCell(Row(7), Column(c), makeCharacter(uint16_t('a' + c)));
    int baselineRuns = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++baselineRuns;
            ses.ackRun(run);
        }
    }
    CHECK(baselineRuns == 1);
    // Mixed: text/emoji/text/emoji/text — 5 runs at the boundaries.
    Cell emoji = makeCharacter(0x2B50); // ⭐
    emoji.pen.attributes = Ubiquitty::VT::Attribute::Emoji;
    d.setCell(Row(9), Column(0), makeCharacter('A'));
    d.setCell(Row(9), Column(1), makeCharacter('B'));
    d.setCell(Row(9), Column(2), emoji);
    d.setCell(Row(9), Column(3), makeCharacter('C'));
    d.setCell(Row(9), Column(4), makeCharacter('D'));
    d.setCell(Row(9), Column(5), emoji);
    d.setCell(Row(9), Column(6), makeCharacter('E'));
    d.setCell(Row(9), Column(7), makeCharacter('F'));
    int mixedRuns = 0;
    int firsts[8] = { -1 };
    int lasts[8] = { -1 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (mixedRuns < 8) {
                firsts[mixedRuns] = run.firstColumn.value();
                lasts[mixedRuns] = run.lastColumn.value();
            }
            ++mixedRuns;
            ses.ackRun(run);
        }
    }
    CHECK(mixedRuns == 5);
    CHECK(firsts[0] == 0 && lasts[0] == 1); // "AB"
    CHECK(firsts[1] == 2 && lasts[1] == 2); // emoji
    CHECK(firsts[2] == 3 && lasts[2] == 4); // "CD"
    CHECK(firsts[3] == 5 && lasts[3] == 5); // emoji
    CHECK(firsts[4] == 6 && lasts[4] == 7); // "EF"
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

// =====================================================================
// Dirty-tracking + BB-shadow lifecycle + PaintSession iterator tests.
//
// These tests exercise the optimization invariants documented in
// CellGrid.h: m_anyDirtyInRow, m_anyMutationsSinceLastPaint,
// m_bbShadowValid, m_fullRedrawRequested, hasUnpaintedWork, and the
// PaintSession iterator's contract. The private state is observed
// indirectly through:
//   - assertInvariants()   (debug-only, returns true iff every row
//                           whose hint is clean actually matches
//                           shadow + has no selection-dirty bits)
//   - hasUnpaintedWork()   (true iff any of the four "dirty" flags
//                           or the shadow-invalid bit is set)
//   - PaintSession iteration output
//
// Where the spec describes a state the public API cannot reach — "the first
// paint after init yields every cell", when init() seeds the shadow so that
// paint yields none — the case asserts what the API does, with the reason the
// spec's wording does not apply beside it.
// =====================================================================

/// Run one PaintSession to completion, ack'ing every yielded run.
/// Returns the number of runs yielded.
static int paintAndAckAll(CellGrid& d)
{
    int yielded = 0;
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc))
        ses.ackScroll(sc);
    PaintRun run;
    while (ses.nextDirtyRun(&run)) {
        ++yielded;
        ses.ackRun(run);
    }
    return yielded;
}

/// --- 1. Basic dirty tracking --------------------------------------

TEST(test_setCell_marks_row_dirty)
{
    /// After a cell write, hasUnpaintedWork() must be true and the
    /// row's dirty hint must drive PaintSession to yield a run on
    /// that row. We probe the row-dirty hint indirectly: after the
    /// write, beginPaint + nextDirtyRun must produce a run on row r.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Drain the post-init state to a clean baseline.
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    Cell A = makeCharacter('A');
    d.setCell(Row(7), Column(3), A);
    CHECK(d.hasUnpaintedWork());
    /// Iterate and confirm row 7 yields a run.
    bool sawRow7 = false;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 7)
                sawRow7 = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow7);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_setCell_identical_value_is_noop)
{
    /// Write a cell + paint + ack so shadow matches. Re-write the
    /// same value: cellEqual fast-path inside setCell must skip the
    /// mutation entirely — no dirty bit, no unpainted work.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell A = makeCharacter('A');
    d.setCell(Row(4), Column(12), A);
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    // Re-write identical value.
    d.setCell(Row(4), Column(12), A);
    CHECK(!d.hasUnpaintedWork());
    /// And no run should be yielded.
    const int yielded = paintAndAckAll(d);
    CHECK(yielded == 0);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_setCell_changed_value_dirties)
{
    /// Write 'A', paint, ack. Then write 'B' at same coords — row
    /// must re-dirty.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(5), Column(2), makeCharacter('A'));
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    d.setCell(Row(5), Column(2), makeCharacter('B'));
    CHECK(d.hasUnpaintedWork());
    int sawRow5Cells = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 5)
                sawRow5Cells += run.lastColumn.value() - run.firstColumn.value() + 1;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow5Cells == 1);
    /// And the cell now holds 'B'.
    const Cell* got = d.cellAt(Row(5), Column(2));
    CHECK(got != nullptr && got->codePoint == 'B');
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testUnrelatedRowStaysClean)
{
    /// Write only to row 5; rows 0..4 and 6..23 must not yield runs.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    (void)paintAndAckAll(d);
    d.setCell(Row(5), Column(0), makeCharacter('X'));
    int rowsTouched[24] = { 0 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            const int rv = run.row.value();
            if (rv >= 0 && rv < 24)
                ++rowsTouched[rv];
            ses.ackRun(run);
        }
    }
    for (int r = 0; r < 24; ++r) {
        if (r == 5)
            CHECK(rowsTouched[r] == 1);
        else
            CHECK(rowsTouched[r] == 0);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_eraseRowRange_only_diff_dirties)
{
    /// Row already filled with blanks; eraseRowRange with the same
    /// blank value must be a no-op (idempotency check inside
    /// eraseRowRange). Then eraseRowRange with a different blank
    /// (different background) must dirty the row.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    // Erase a row that's already blank.
    d.eraseRowRange(Row(3), Column(0), Column(79), blank);
    CHECK(!d.hasUnpaintedWork());
    const int yielded1 = paintAndAckAll(d);
    CHECK(yielded1 == 0);
    // Now erase with a different background → must dirty the row.
    Cell tinted = blank;
    tinted.pen.backgroundIndex = colorRGB;
    tinted.pen.backgroundRed = 0x80;
    tinted.pen.backgroundGreen = 0x00;
    tinted.pen.backgroundBlue = 0x00;
    d.eraseRowRange(Row(3), Column(0), Column(79), tinted);
    CHECK(d.hasUnpaintedWork());
    int rowsTouched[24] = { 0 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            const int rv = run.row.value();
            if (rv >= 0 && rv < 24)
                ++rowsTouched[rv];
            ses.ackRun(run);
        }
    }
    CHECK(rowsTouched[3] >= 1);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// --- 2. PaintSession iterator -------------------------------------

TEST(testPaintsessionYieldsDirtyRuns)
{
    /// Three disjoint runs on row 5: columns 2..4, 10..12, 20..22.
    /// Iterator must yield exactly three PaintRun records with
    /// matching column ranges.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    (void)paintAndAckAll(d);
    for (int c = 2; c <= 4; ++c)
        d.setCell(Row(5), Column(c), makeCharacter('a'));
    for (int c = 10; c <= 12; ++c)
        d.setCell(Row(5), Column(c), makeCharacter('b'));
    for (int c = 20; c <= 22; ++c)
        d.setCell(Row(5), Column(c), makeCharacter('c'));
    int yielded = 0;
    int firsts[8] = { -1 }, lasts[8] = { -1 };
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (yielded < 8) {
                firsts[yielded] = run.firstColumn.value();
                lasts[yielded] = run.lastColumn.value();
            }
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 3);
    CHECK(firsts[0] == 2 && lasts[0] == 4);
    CHECK(firsts[1] == 10 && lasts[1] == 12);
    CHECK(firsts[2] == 20 && lasts[2] == 22);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testPaintsessionMergesAdjacentDiffCells)
{
    /// Contiguous cells at 5,6,7,8 with the same pen — one run, not four.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    (void)paintAndAckAll(d);
    for (int c = 5; c <= 8; ++c)
        d.setCell(Row(9), Column(c), makeCharacter(uint16_t('A' + (c - 5))));
    int yielded = 0;
    int f0 = -1, l0 = -1;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (yielded == 0) {
                f0 = run.firstColumn.value();
                l0 = run.lastColumn.value();
            }
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 1);
    CHECK(f0 == 5 && l0 == 8);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testPaintsessionSkipsCleanRows)
{
    /// Mutate only row 5; iterate; the iterator should yield exactly
    /// one run on row 5 and never produce a run for any other row.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    (void)paintAndAckAll(d);
    d.setCell(Row(5), Column(40), makeCharacter('Z'));
    int yielded = 0;
    bool onlyRow5 = true;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() != 5)
                onlyRow5 = false;
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 1);
    CHECK(onlyRow5);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testPaintsessionYieldsFullRowAfterFullredrawRequest)
{
    /// After a successful paint, request a full redraw. The next
    /// PaintSession must yield runs spanning every cell of every row,
    /// even though no cells changed.
    ///
    /// After requestFullRedraw, the iterator drives `full = true`,
    /// meaning every cell is treated as dirty regardless of shadow.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Put a tiny bit of content so the shadow has something other
    // than the seeded blank to compare against, then paint to sync.
    d.setCell(Row(0), Column(0), makeCharacter('a'));
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());

    d.requestFullRedraw();
    CHECK(d.isFullRedrawRequested());
    CHECK(d.hasUnpaintedWork());

    int rowsCovered[24] = { 0 };
    bool fullFlagSeen = false;
    {
        PaintSession ses = d.beginPaint();
        fullFlagSeen = ses.isFullRedraw();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            const int rv = run.row.value();
            if (rv >= 0 && rv < 24)
                rowsCovered[rv] += run.lastColumn.value() - run.firstColumn.value() + 1;
            ses.ackRun(run);
        }
    }
    CHECK(fullFlagSeen);
    // Every row of an 80-column display should have been covered
    // by yielded runs totalling 80 cells.
    for (int r = 0; r < 24; ++r)
        CHECK(rowsCovered[r] == 80);
    // requestFullRedraw flag auto-clears once a session starts.
    CHECK(!d.isFullRedrawRequested());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testPaintsessionDestructClearsDirty)
{
    /// Write rows 5 + 7; complete a PaintSession (iterate + ack every
    /// run); on dtor, the row-dirty hints for 5 and 7 are cleared
    /// (assertInvariants must see clean rows matching shadow) and
    /// hasUnpaintedWork must be false.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    (void)paintAndAckAll(d);
    d.setCell(Row(5), Column(1), makeCharacter('m'));
    d.setCell(Row(7), Column(2), makeCharacter('n'));
    CHECK(d.hasUnpaintedWork());
    // Scope the PaintSession so its dtor runs before the asserts.
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run))
            ses.ackRun(run);
    }
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

// --- 3. Shadow lifecycle ------------------------------------------

/// Verifies that `init()` leaves the shadow valid and the first paint empty.
///
/// A brand-new `CellGrid` before `init()` is unusable (rows=0), so the spec's
/// "fresh Display, m_bbShadowValid is false; the FIRST paint should yield every
/// cell" describes a state the public API never hands out: `init()` seeds the
/// shadow to match the active screen's blank cells, which is what
/// `CellGrid::init` means by "first paint yields zero dirty runs because active
/// cells == m_bbShadow already". Cold start is reached through
/// `invalidateShadow()` or `resize()`, each covered separately below.
TEST(testShadowIsValidAfterInit)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // hasUnpaintedWork is false right after init: shadow is valid,
    // mutations flag is false, no pending scroll, no full redraw.
    CHECK(!d.hasUnpaintedWork());
    /// The first paint yields zero runs (post-init contract).
    int yielded = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 0);
}

TEST(testShadowBecomesValidAfterFullPaint)
{
    /// Invalidate the shadow explicitly, then run a paint session
    /// to completion. After dtor, hasUnpaintedWork must be false —
    /// which requires m_bbShadowValid to be true (the only way for
    /// that flag to be false in the hasUnpaintedWork formula).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(0), Column(0), makeCharacter('!'));
    d.invalidateShadow();
    CHECK(d.hasUnpaintedWork());
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testShadowDiffAfterSingleCellChange)
{
    /// After a full paint + ack, the shadow is valid. Change exactly
    /// one cell; the new PaintSession must yield exactly one run
    /// covering that one cell.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Pre-seed some content + paint so the shadow has structure.
    for (int c = 0; c < 10; ++c)
        d.setCell(Row(11), Column(c), makeCharacter(uint16_t('a' + c)));
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    // Change one cell.
    d.setCell(Row(11), Column(5), makeCharacter('Z'));
    int yielded = 0;
    int f = -1, l = -1, rrow = -1;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (yielded == 0) {
                rrow = run.row.value();
                f = run.firstColumn.value();
                l = run.lastColumn.value();
            }
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 1);
    CHECK(rrow == 11);
    CHECK(f == 5 && l == 5);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testShadowInvalidatedByResize)
{
    /// After a full paint, m_bbShadowValid is true. Resize forces it
    /// back to false: hasUnpaintedWork must be true even with no
    /// content change.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(0), Column(0), makeCharacter('@'));
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    d.resize(24, 80); // same shape, but resize invalidates shadow
    CHECK(d.hasUnpaintedWork());
    /// Next paint must yield runs (full redraw + invalid shadow).
    const int yielded = paintAndAckAll(d);
    CHECK(yielded > 0);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_shadow_invalidated_by_requestFullRedraw)
{
    /// requestFullRedraw flips m_fullRedrawRequested and sets the
    /// mutations bit. Even though the shadow is still valid, the
    /// iterator's `full = m_fullRedraw || !m_bbShadowValid` will see
    /// full=true and yield every cell.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(2), Column(2), makeCharacter('p'));
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    d.requestFullRedraw();
    CHECK(d.hasUnpaintedWork());
    CHECK(d.isFullRedrawRequested());
    /// Expect every cell yielded.
    int totalCellsYielded = 0;
    bool fullFlag = false;
    {
        PaintSession ses = d.beginPaint();
        fullFlag = ses.isFullRedraw();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            totalCellsYielded += run.lastColumn.value() - run.firstColumn.value() + 1;
            ses.ackRun(run);
        }
    }
    CHECK(fullFlag);
    CHECK(totalCellsYielded == 24 * 80);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// --- 4. Geometry + grow -------------------------------------------

TEST(testResizeGrowBlanksRevealedCells)
{
    /// CellGrid at 5×10. Grow to 5×20. The new columns 10..19 of every
    /// row must read as blank (not stale alternate-screen garbage). The
    /// production code does not explicitly blank revealed cells on
    /// grow — they're seeded blank at init() and stay that way until
    /// a mutator overwrites them. Verify that contract.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(5, 10);
    // Put non-blank content in every cell of the 5x10 area.
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 10; ++c)
            d.setCell(Row(r), Column(c), makeCharacter('K'));
    (void)paintAndAckAll(d);
    d.resize(5, 20);
    CHECK(d.rows() == 5);
    CHECK(d.columns() == 20);
    // Cells 10..19 of every row must be blank.
    for (int r = 0; r < 5; ++r) {
        for (int c = 10; c < 20; ++c) {
            const Cell* got = d.cellAt(Row(r), Column(c));
            CHECK(got != nullptr);
            if (got)
                CHECK(got->codePoint == 0);
        }
    }
    // Old content in 0..9 must still be there.
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 10; ++c) {
            const Cell* got = d.cellAt(Row(r), Column(c));
            CHECK(got != nullptr);
            if (got)
                CHECK(got->codePoint == 'K');
        }
    }
    // Resize-induced full redraw should drain cleanly.
    (void)paintAndAckAll(d);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testResizeShrinkDropsOutOfRangeDirtyBits)
{
    /// Dirty row 9 of a 10-row display, then shrink to 5 rows. Rows
    /// 5..9 are no longer in range; the iterator must not yield runs
    /// for them. assertInvariants must still pass (rows 5..9 are not
    /// within m_rows, but the existing implementation memsets the tail
    /// to zero on shrink).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(10, 40);
    (void)paintAndAckAll(d);
    d.setCell(Row(9), Column(5), makeCharacter('q'));
    // Shrink.
    d.resize(5, 40);
    CHECK(d.rows() == 5);
    int outOfRangeRuns = 0;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() >= 5)
                ++outOfRangeRuns;
            ses.ackRun(run);
        }
    }
    CHECK(outOfRangeRuns == 0);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_assertInvariants_passes_after_clean_paint)
{
    /// Write, paint, ack. assertInvariants must return true.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(3), Column(3), makeCharacter('Q'));
    d.setCell(Row(12), Column(40), makeCharacter('R'));
    d.setCell(Row(20), Column(70), makeCharacter('S'));
    (void)paintAndAckAll(d);
#ifndef NDEBUG
    CHECK(d.assertInvariants());
#endif
}

/// --- 5. hasUnpaintedWork ------------------------------------------

TEST(test_hasUnpaintedWork_true_after_mutation_false_after_paint)
{
    /// Fresh + post-init: hasUnpaintedWork false (init seeds shadow
    /// and clears all flags). After a cell write: true. After a full
    /// paint + ack: false.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    CHECK(!d.hasUnpaintedWork());
    d.setCell(Row(8), Column(8), makeCharacter('w'));
    CHECK(d.hasUnpaintedWork());
    (void)paintAndAckAll(d);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(test_hasUnpaintedWork_true_with_only_selection_flip)
{
    /// No cell mutation; just markSelectionFlipped. hasUnpaintedWork
    /// must be true because selection-dirty bits are unpainted work
    /// (markSelectionFlipped sets m_anyMutationsSinceLastPaint and
    /// m_anyDirtyInRow[r]).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    CHECK(!d.hasUnpaintedWork());
    d.markSelectionFlipped(Row(6), Column(10), Column(20));
    CHECK(d.hasUnpaintedWork());
    /// The yielded run should be selectionOnly.
    bool sawSelectionOnly = false;
    {
        PaintSession ses = d.beginPaint();
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.selectionOnly)
                sawSelectionOnly = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawSelectionOnly);
    CHECK(!d.hasUnpaintedWork());
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// ================================================================
/// Scroll-coalescing + pending-scroll lifecycle.
///
/// These tests pin the public-visible behavior of:
///
///   CellGrid::scrollRegion(top, bot, n, blank)
///
/// and the way PaintSession::nextPendingScroll surfaces the
/// coalesced PendingScroll. The optimization: consecutive
/// scrollRegion calls with identical [top..bot] and same direction
/// sign coalesce into one PendingScroll with delta == sum(n_i) —
/// lets the renderer issue one blit instead of N.
///
/// Different [top..bot], or opposite direction, both fall into the
/// "replace" branch (see CellGrid::scrollRegion in CellGrid.cpp ≈
/// line 406-411). That means the pending scroll's prior delta is
/// silently dropped and replaced — there is no "flush-and-apply"
/// step that drains the old scroll into m_bbShadow first. The
/// cells_ model is already correct (each scrollRegion mutates
/// cells_ unconditionally); only the BB-pixel shift that would have
/// been issued for the prior pending got squashed.
///
/// Tests below pin both the coalescing path and the lifecycle
/// (PaintSession yields scroll → AckScroll → next session sees no
/// scroll).
///
/// Helper: peekPendingScroll uses a fresh PaintSession (immediately
/// dropped, so we never AckScroll — m_pendingScroll stays valid).
/// Returns true + fills out if a pending scroll is queued; false
/// otherwise. Because PaintSession's dtor sets m_bbShadowValid=true
/// iff !m_pendingScroll.valid, peeking does not corrupt state.

static bool peekPendingScroll(CellGrid& d, Screen::PendingScroll* out)
{
    PaintSession ses = d.beginPaint();
    if (ses.nextPendingScroll(out)) {
        // Do not ackScroll — let the session drop the scroll
        // still-queued in CellGrid so the next real paint can
        // see it.
        return true;
    }
    return false;
}

/// (1) Three same-direction scrolls in [0..9] must merge into one
/// PendingScroll with shiftRows == +3.
TEST(testThreeScrollsMergeIntoOne)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.scrollRegion(Row(0), Row(9), 1, blank);
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    CHECK(got);
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    CHECK(sc.shiftRows == 3);
}

/// (2) Three same-region negative scrolls merge to delta == -3.
TEST(testNegativeScrollsMergeIntoOne)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), -1, blank);
    d.scrollRegion(Row(0), Row(9), -1, blank);
    d.scrollRegion(Row(0), Row(9), -1, blank);
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    CHECK(got);
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    CHECK(sc.shiftRows == -3);
}

/// (3) Different [top..bot] region cannot merge: the new scroll
/// replaces the pending entry. Cells in both regions have already
/// been mutated by their respective scrollRegion calls; only the
/// BB-shift for the first region is lost (production gap — the
/// renderer never sees a blit for the dropped scroll).
///
/// Pinned behavior: the second scrollRegion's parameters become the
/// one visible PendingScroll.
TEST(testMixedTopBotDoesNotMerge)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.scrollRegion(Row(5), Row(15), 1, blank);
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    CHECK(got);
    // Production: pending is REPLACED by the second call. The first
    // call's BB-shift is silently dropped. Pin this so a future
    // "flush-and-apply" change cannot land without editing this test.
    CHECK(sc.topRow.value() == 5);
    CHECK(sc.botRow.value() == 15);
    CHECK(sc.shiftRows == 1);
}

/// (4) A scroll(0..9, +1), then setCell on a row OUTSIDE the region
/// (row 12) must leave the pending scroll intact and mark row 12
/// dirty for paint. Row-dirty hint is internal; we observe it
/// through the iterator: after acking the scroll, exactly one
/// dirty run on row 12 (plus whatever the scroll's revealed-row
/// dirties, see the earlier dirty-tracking test). We isolate this by setting up the
/// setCell on row 12 after the scroll and asserting:
///   - pending scroll still valid + matches the scroll parameters
///   - row 12 surfaces as dirty in the paint iterator
TEST(testScrollThenUnrelatedWriteKeepsPending)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.setCell(Row(12), Column(0), makeCharacter('X'));
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    CHECK(sc.shiftRows == 1);
    /// Drain in a fresh session: ack the scroll, then iterate runs;
    /// row 12 must appear.
    bool sawRow12 = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc2;
        while (ses.nextPendingScroll(&sc2))
            ses.ackScroll(sc2);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 12)
                sawRow12 = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow12);
}

/// Alternate-screen flip must drop the queued scroll before any new
/// scrollRegion arrives. Clearing `valid` alone is not enough: the
/// post-flip scroll's coalescing test would pass the activeIndex check
/// vacuously. With the tag the new scroll always starts fresh.
TEST(testAlternateFlipThenScrollStartsFreshPending)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    // Queue a scroll on the primary screen, then flip to alternate and
    // scroll the same band there. The pending entry must reflect the
    // alternate scroll alone — not the sum of primary + alternate.
    d.scrollRegion(Row(0), Row(9), 2, blank);
    d.setAlternateActive(ScreenMode::Alternate);
    d.scrollRegion(Row(0), Row(9), 3, blank);
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    CHECK(sc.shiftRows == 3); // not 5 — alternate-flip dropped the prior delta
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// Full-region clear must reset the pending entry so the post-clear
/// synthetic ±region delta doesn't stack on top of a prior partial
/// shift. Coalesced, scroll(+1) immediately followed by scroll(+region)
/// would give delta = region + 1 (clamped to region but logically
/// over-shifted).
TEST(testFullRegionClearReplacesPendingScroll)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    const int top = 0, bot = 9;
    const int region = bot - top + 1;
    d.scrollRegion(Row(top), Row(bot), 1, blank); // normal +1
    // Full-region clear: shift >= region forces the synthetic
    // ±region delta path.
    d.scrollRegion(Row(top), Row(bot), region + 5, blank);
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.topRow.value() == top);
    CHECK(sc.botRow.value() == bot);
    // Delta is exactly +region (the synthetic full-clear value), not
    // region + 1.
    CHECK(sc.shiftRows == region);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// (5) Scroll then a setCell at a row INSIDE the scroll region:
/// pin the current behavior. Production: setCell does not flush
/// the pending scroll; the pending scroll stays valid, the cell
/// write goes into cells_ at its new position (the model was
/// already shifted by scrollRegion), and the row's dirty hint
/// gets set. So after the scroll + setCell, peekPendingScroll
/// still surfaces the scroll.
TEST(testScrollThenWriteInsideRegionBreaksCoalesce)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.setCell(Row(3), Column(0), makeCharacter('Y'));
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    // Production: cell write does not flush; scroll remains.
    CHECK(got);
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    CHECK(sc.shiftRows == 1);
    // A further same-region same-direction scroll should still
    // coalesce — the inner setCell didn't break the merge.
    d.scrollRegion(Row(0), Row(9), 1, blank);
    Screen::PendingScroll sc2 {};
    CHECK(peekPendingScroll(d, &sc2));
    CHECK(sc2.shiftRows == 2);
}

/// (6) PaintSession ordering: scroll the region, also write a
/// cell on row 12 (outside region). The iterator must yield the
/// PendingScroll before any PaintRun. This is the PAL contract —
/// BB-pixels shifted first so subsequent run rasterization sees
/// the post-shift backdrop.
TEST(testPaintsessionYieldsPendingscrollBeforeRuns)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.setCell(Row(12), Column(0), makeCharacter('Z'));
    // Paint to flush row 12 so the next session only has the
    // scroll-plus-revealed-row to deal with.
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    drainPaint(d, vbb);
    // Now: scroll and a write outside the region.
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.setCell(Row(12), Column(0), makeCharacter('W'));
    // Iterate; verify ordering.
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    bool gotScrollFirst = ses.nextPendingScroll(&sc);
    CHECK(gotScrollFirst);
    // At this point no run has been yielded yet — invariant.
    ses.ackScroll(sc);
    int runsAfterScroll = 0;
    PaintRun run;
    while (ses.nextDirtyRun(&run)) {
        ++runsAfterScroll;
        ses.ackRun(run);
    }
    CHECK(runsAfterScroll >= 1);
}

/// (7) Pure scroll with no other mutations: PaintSession yields
/// exactly one PendingScroll, and the newly-blanked row(s) come
/// through as dirty runs (because shadowShiftRows shifts the
/// shadow but the bottom of the region is now blank — and the
/// pre-scroll cells_ row at that position was content).
TEST(testPaintsessionYieldsPendingscrollEvenWithNoCellDiff)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    // Initially everything is blank: m_bbShadow == cells_. A
    // scrollRegion in this state still queues a PendingScroll
    // (cells_ shifted; shadow not yet — only the iterator's
    // AckScroll catches the shadow up).
    d.scrollRegion(Row(0), Row(9), 1, blank);
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    bool got = ses.nextPendingScroll(&sc);
    CHECK(got);
    ses.ackScroll(sc);
    /// Post-ack, since both cells_ and shadow are now-all-blank in
    /// the band, no PaintRuns should be yielded.
    int runs = 0;
    PaintRun run;
    while (ses.nextDirtyRun(&run)) {
        ++runs;
        ses.ackRun(run);
    }
    // The comment above states the invariant; this asserts it. `runs` is a
    // local that only ever increments, so `>= 0` held for every behaviour of
    // PaintSession — including a full redraw, which yields one run per row.
    //
    // What this cannot see, and it belongs to a case that seeds content: with
    // the band blank on both sides, shifting blanks over blanks is
    // unobservable, so shadowShiftRows becoming a no-op would still leave the
    // two equal and yield nothing. This case's subject is that a scroll queues
    // a PendingScroll with no cell diff, and seeding content to catch that
    // would remove the condition the case is named for.
    CHECK_EQ(runs, 0);
}

/// (8) AckScroll plus session destruct clears the pending scroll.
/// A subsequent fresh paint sees no pending.
TEST(testPaintsessionAckPendingscrollClearsIt)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    // First session: drain + ack.
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run))
            ses.ackRun(run);
    }
    // Second session: no pending.
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    CHECK(!got);
}

/// (9) Contract: when the PaintSession yields the pending scroll,
/// the BB shadow is still in PRE-scroll state. The PAL blits
/// the back-buffer (pre-shift → post-shift), then AckScroll shifts
/// m_bbShadow in lockstep. We can't peek directly at m_bbShadow
/// from a test, but we can verify the lockstep contract by:
///
///   - fill cells, paint (shadow == cells, both have content)
///   - scrollRegion(+1) on the full screen → cells_ shifted up
///   - peek pending: confirm it's there
///   - simulate PAL: ackScroll, then drain runs into a virtual BB
///   - assert virtualBB == cells_ (the lockstep test).
///
/// This proves: at the moment of nextPendingScroll, the model
/// (cells_) is one shift ahead of the shadow (and ahead of BB).
/// AckScroll closes the gap.
TEST(testPaintsessionAfterScrollShadowIsPreScroll)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Fill rows 0..9 with distinct chars; sync shadow via paint.
    for (int r = 0; r < 10; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + r)));
    drainPaint(d, vbb);
    // No drift right after a fully-drained paint.
    CHECK(countMismatches(d, vbb) == 0);
    // Scroll up by 1. cells_ shifts immediately.
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    // Pending scroll surfaces from PaintSession (peek).
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.shiftRows == 1);
    // Now drive a real paint: drainPaint applies the same shift
    // to vbb on ackScroll. After it returns, vbb must match
    // cells_ exactly — that's the lockstep proof.
    drainPaint(d, vbb);
    CHECK(countMismatches(d, vbb) == 0);
}

/// (10) Opposite direction within the same region. Production:
/// the merge guard requires same sign, so an opposite-sign scroll
/// falls into the `else { replace }` branch — the prior pending
/// is silently dropped (no flush-and-apply), and the new pending
/// takes its place. Net visual: cells_ is still correct (both
/// scrolls mutated it), but the BB only ever sees the SECOND
/// scroll's blit — the first one's pixel shift is lost. Pin
/// the replace behavior.
TEST(testOppositeDirectionWithinSameRegion)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 2, blank);
    d.scrollRegion(Row(0), Row(9), -1, blank);
    Screen::PendingScroll sc {};
    bool got = peekPendingScroll(d, &sc);
    CHECK(got);
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 9);
    // Replace branch wins: the new pending matches the SECOND
    // call's n (-1), not a net delta (+1).
    CHECK(sc.shiftRows == -1);
}

/// (11) Direction change via a different region: same replace
/// branch — different top/bot and opposite sign. Pending is
/// the second call's parameters.
TEST(testDirectionChangeViaUnrelatedRegion)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.scrollRegion(Row(5), Row(15), -1, blank);
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.topRow.value() == 5);
    CHECK(sc.botRow.value() == 15);
    CHECK(sc.shiftRows == -1);
}

/// (12) Delta gridFloor. scrollRegion(0..4, n=10) — region height is
/// 5. Production clamps shift to region size and enters the
/// "full-region erase" path (every row blanked, shift = region).
/// After: cells in rows 0..4 are all blank; pending scroll, if any,
/// has |shiftRows| == 5.
TEST(testDeltaOverflowClampsToRegionHeight)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Seed content so we can confirm the full-blank path.
    for (int r = 0; r < 5; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + r)));
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(4), 10, blank);
    // All rows 0..4 must now be blank in cells_.
    for (int r = 0; r < 5; ++r) {
        const Cell* c = d.cellAt(Row(r), Column(0));
        CHECK(c != nullptr);
        CHECK(c->codePoint == 0);
    }
    // Pending scroll, if any, has |shift| == region.
    // A full-region erase queues the shift like any other, with the magnitude
    // clamped to the region's height rather than the delta asked for.
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    const int absShift = sc.shiftRows > 0 ? sc.shiftRows : -sc.shiftRows;
    CHECK(absShift == 5);
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 4);
}

/// (13) Write inside scrolling region after pending scroll. The
/// write targets the cell's NEW (post-shift) position — because
/// scrollRegion already mutated cells_ before setCell ran. We
/// verify by reading the cell back: it has the value we just wrote.
/// No flush is triggered.
TEST(testWriteInsideScrollingRegionAfterPendingScroll)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    // Pending is queued. Write inside the region.
    Cell mark = makeCharacter('M');
    d.setCell(Row(5), Column(7), mark);
    const Cell* got = d.cellAt(Row(5), Column(7));
    CHECK(got != nullptr);
    CHECK(got->codePoint == 'M');
    // Pending scroll is still there — write did not flush.
    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.shiftRows == 1);
}

/// (14) "DECSTBM in spirit": apply a scrollRegion limited to
/// rows 5..15, and verify rows outside that band stay clean.
/// This is the CellGrid-level analogue of DECSTBM: only the
/// specified band is touched by scrollRegion. We don't link
/// VT.cpp here (test_display links CellGrid.cpp only), so the
/// test directly drives scrollRegion.
TEST(testScrollWithinDECSTBMDoesNotDirtyOutside)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Seed every row with a unique char so untouched rows can be
    // verified to stay equal to vbb (which we'll only shift in the
    // scroll band).
    for (int r = 0; r < 24; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + (r % 26))));
    drainPaint(d, vbb);
    CHECK(countMismatches(d, vbb) == 0);
    Cell blank = makeBlank();
    d.scrollRegion(Row(5), Row(15), 1, blank);
    /// Iterate: any dirty run on rows < 5 or > 15 indicates
    /// leakage of the scroll's effect to rows outside the band.
    bool sawOutside = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            const int r = run.row.value();
            if (r < 5 || r > 15)
                sawOutside = true;
            ses.ackRun(run);
        }
    }
    CHECK(!sawOutside);
}

/// Neovim-style viewport scroll: the editable body is rows 0..N-2 and
/// the bottom row is a pinned statusline. The body scroll must recycle
/// only inside that band; the statusline cell and row metadata stay put.
TEST(testTopBandScrollKeepsStatusRowPinned)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(6, 8);
    for (int r = 0; r < 6; ++r) {
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + r)));
        d.rowMeta(r).oscMark = static_cast<uint8_t>(r + 1);
        d.rowMeta(r).set(Ubiquitty::RowFlag::Wrapped, static_cast<bool>(r % 2));
    }

    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(4), 1, blank);

    const Cell* row0 = d.cellAt(Row(0), Column(0));
    const Cell* row3 = d.cellAt(Row(3), Column(0));
    const Cell* row4 = d.cellAt(Row(4), Column(0));
    const Cell* row5 = d.cellAt(Row(5), Column(0));
    CHECK(row0 != nullptr && row0->codePoint == 'B');
    CHECK(row3 != nullptr && row3->codePoint == 'E');
    CHECK(row4 != nullptr && row4->codePoint == 0);
    CHECK(row5 != nullptr && row5->codePoint == 'F');
    CHECK(d.rowMeta(0).oscMark == 2);
    CHECK(d.rowMeta(0).has(Ubiquitty::RowFlag::Wrapped) == static_cast<bool>(1));
    CHECK(d.rowMeta(3).oscMark == 5);
    CHECK(d.rowMeta(4).oscMark == 0);
    CHECK(d.rowMeta(4).has(Ubiquitty::RowFlag::Wrapped) == false);
    CHECK(d.rowMeta(5).oscMark == 6);
    CHECK(d.rowMeta(5).has(Ubiquitty::RowFlag::Wrapped) == static_cast<bool>(1));

    Screen::PendingScroll sc {};
    CHECK(peekPendingScroll(d, &sc));
    CHECK(sc.topRow.value() == 0);
    CHECK(sc.botRow.value() == 4);
    CHECK(sc.shiftRows == 1);
}

/// (15) After scrollRegion(+1), the newly-revealed bottom row
/// (in cells_, the last row of the region, now blank) must
/// surface as dirty in the iterator — because pre-scroll its
/// shadow held content.
TEST(testScrollMarksRevealedRowsDirty)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Fill rows 0..9 with content + sync shadow.
    for (int r = 0; r < 10; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + r)));
    drainPaint(d, vbb);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    /// After ackScroll, vbb is shifted; vbb[9][0] holds old content
    /// from vbb[10] in the helper's shift, but the band-bottom
    /// row in cells_ is now blank — so the iterator should report
    /// row 9.
    bool sawRow9 = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 9)
                sawRow9 = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow9);
}

/// (16) Full-screen scroll: row 0 ends up holding what was at row 1;
/// the last row is blank. Uses a 5×5 grid for clarity.
TEST(testFullScreenScrollDropsTopRow)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(5, 5);
    VirtualBackBuffer vbbStorage(makeBlank());
    Cell(*vbb)[maxColumns] = vbbStorage.get();
    // Each row's column 0 = unique marker.
    for (int r = 0; r < 5; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('A' + r)));
    drainPaint(d, vbb);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(4), 1, blank);
    /// Row 0 was 'A'; after shift, it's 'B'. Row 4 is blank.
    const Cell* row0 = d.cellAt(Row(0), Column(0));
    CHECK(row0 != nullptr);
    CHECK(row0->codePoint == 'B');
    const Cell* row3 = d.cellAt(Row(3), Column(0));
    CHECK(row3 != nullptr);
    CHECK(row3->codePoint == 'E');
    const Cell* row4 = d.cellAt(Row(4), Column(0));
    CHECK(row4 != nullptr);
    CHECK(row4->codePoint == 0);
}

/// (17) Alternate-screen sanity: a scroll on the primary screen leaves
/// the alternate screen's cells untouched. We switch to alternate
/// after, set a marker, switch back, scroll, switch to alternate
/// again, and verify the marker is still there.
TEST(testScrollKeepsAlternateScreenUntouched)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    // Stamp marker on alternate.
    d.setAlternateActive(ScreenMode::Alternate);
    d.setCell(Row(7), Column(3), makeCharacter('Q'));
    d.setAlternateActive(ScreenMode::Primary);
    // Primary scroll.
    d.scrollRegion(Row(0), Row(9), 1, blank);
    // Verify alternate is untouched.
    d.setAlternateActive(ScreenMode::Alternate);
    const Cell* got = d.cellAt(Row(7), Column(3));
    CHECK(got != nullptr);
    CHECK(got->codePoint == 'Q');
}

/// (18) requestFullRedraw after scrollRegion. A full repaint rewrites every
/// cell, so requestFullRedraw drops the queued (not-yet-blitted) scroll instead
/// of leaving pixels for the next paint to immediately overwrite: full-redraw
/// requested and pending scroll cleared.
TEST(testFullRedrawRequestAfterScrollCollapses)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(9), 1, blank);
    d.requestFullRedraw();
    // Both flags set.
    CHECK(d.isFullRedrawRequested());
    Screen::PendingScroll sc {};
    bool stillPending = peekPendingScroll(d, &sc);
    // requestFullRedraw subsumes the queued scroll: a full repaint rewrites
    // every cell, so the not-yet-blitted scroll is dropped rather than blitting
    // pixels the repaint immediately paints over.
    CHECK(!stillPending);
}

// ===================================================================
// Wide-cell atomicity + selection-dirty bitmap tests.
//
// Coverage:
//   - writeWideCell writes the lead/cont pair atomically with the
//     Attribute::Wide / Attribute::WideContinuation contract.
//   - clearWideOverlap pre-emptively breaks adjacent wide pairs that
//     narrow writes would orphan.
//   - Pen-equality invariant between lead and cont (asserted in
//     debug; documented for release).
//   - markSelectionFlipped sets the selection-dirty bitmap and
//     drives PaintSession to yield selectionOnly runs without
//     dirtying cell content.
//
// White-box note: m_selectionDirty / m_anyDirtyInRow / m_anySelectionDirty
// are private. We probe them indirectly through hasUnpaintedWork(),
// assertInvariants() (DEBUG), and the PaintSession iterator's
// (row-skipped vs not) behaviour, plus the selectionOnly flag on
// yielded runs.
//
// Edge cases now handled gracefully (invoked, not skipped):
//   - writeWideCell at the right margin (c == columns-1) rejects the
//     write cleanly (no room for the cont half) instead of asserting.
//   - writeWideCell canonicalizes the cont half's colors to the lead's
//     rather than asserting pen-equality (which silently corrupted under
//     NDEBUG). See test_writewidecell_{at_right_margin_is_rejected,
//     canonicalizes_cont_pen}.
// ===================================================================

/// Build a wide-cell lead with the given char + identical pen as cont.
static Cell makeWideLead(uint16_t ch)
{
    Cell c = makeBlank();
    c.codePoint = ch;
    c.pen.attributes = Ubiquitty::VT::Attribute::Wide;
    return c;
}

/// Build the cont half of a wide pair — sentinel char 0, Attribute::WideContinuation,
/// pen identical to the lead returned by makeWideLead.
static Cell makeWideCont()
{
    Cell c = makeBlank();
    c.codePoint = 0;
    c.pen.attributes = Ubiquitty::VT::Attribute::WideContinuation;
    return c;
}

/// True iff (r, c) is currently treated as dirty by the iterator —
/// i.e. beginPaint would yield a run covering it. Probes the iterator
/// without consuming any acks (the session is dropped without
/// ackRun, so this is a peek).
static bool isRowDirty(CellGrid& d, int row)
{
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc))
        ses.ackScroll(sc);
    PaintRun run;
    while (ses.nextDirtyRun(&run)) {
        if (run.row.value() == row)
            return true;
        ses.ackRun(run); // unacked yields make the dtor flag shadow drift on the no-match path
    }
    return false;
}

/// Drain all queued work without writing to a virtualBB. Use to flush
/// dirty state between assertions when we only care about subsequent
/// iterator behaviour.
static void flushPaint(CellGrid& d)
{
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc))
        ses.ackScroll(sc);
    PaintRun run;
    while (ses.nextDirtyRun(&run))
        ses.ackRun(run);
}

/// ---------- writeWideCell atomicity -------------------------------

TEST(testWritewidecellWritesBothHalves)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    Cell lead = makeWideLead(0x4E2D); // 中
    Cell cont = makeWideCont();
    Cell blank = makeBlank();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    const Cell* gotLead = d.cellAt(Row(5), Column(10));
    const Cell* gotCont = d.cellAt(Row(5), Column(11));
    CHECK(gotLead != nullptr);
    CHECK(gotCont != nullptr);
    if (gotLead) {
        CHECK(gotLead->codePoint == 0x4E2D);
        CHECK(gotLead->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(!gotLead->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    }
    if (gotCont) {
        CHECK(gotCont->codePoint == 0);
        CHECK(gotCont->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(!gotCont->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    }
}

TEST(testWritewidecellMarksRowDirty)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    // Sanity: row 5 starts clean (no dirty runs yielded).
    CHECK(!isRowDirty(d, 5));
    Cell lead = makeWideLead(0x3042); // あ
    Cell cont = makeWideCont();
    Cell blank = makeBlank();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    // hasUnpaintedWork must be true post-mutation.
    CHECK(d.hasUnpaintedWork());
    // The iterator must yield row 5 as dirty.
    CHECK(isRowDirty(d, 5));
}

TEST(testWritewidecellIdempotent)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell lead = makeWideLead(0x3044); // い
    Cell cont = makeWideCont();
    Cell blank = makeBlank();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    flushPaint(d);
    // Second write of the same pair: cells already match, no work.
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    int yielded = 0;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++yielded;
            ses.ackRun(run);
        }
    }
    CHECK(yielded == 0);
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testWritewidecellBreaksLeftNeighborPair)
{
    /// Pre-state: wide pair at (5, 9..10). Then writeWideCell at
    /// (5, 10..11) — the new lead overwrites the previous cont half;
    /// the previous lead at column 9 is now an orphan and must be blanked
    /// by writeWideCell's left-break logic.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell leadA = makeWideLead('A');
    Cell contA = makeWideCont();
    d.writeWideCell(Row(5), Column(9), leadA, contA, blank);
    flushPaint(d);
    Cell leadB = makeWideLead('B');
    Cell contB = makeWideCont();
    d.writeWideCell(Row(5), Column(10), leadB, contB, blank);
    const Cell* column9 = d.cellAt(Row(5), Column(9));
    const Cell* col10 = d.cellAt(Row(5), Column(10));
    const Cell* col11 = d.cellAt(Row(5), Column(11));
    CHECK(column9 != nullptr);
    if (column9) {
        // Orphan lead blanked.
        CHECK(!column9->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(column9->codePoint == 0);
    }
    // New pair intact.
    if (col10) {
        CHECK(col10->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(col10->codePoint == 'B');
    }
    if (col11) {
        CHECK(col11->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    }
}

TEST(testWritewidecellBreaksRightNeighborPair)
{
    /// Pre-state: wide pair at (5, 11..12). Then writeWideCell at
    /// (5, 10..11) — the new cont overwrites the previous lead at
    /// column 11; the previous cont at column 12 is now an orphan and must
    /// be blanked.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell leadA = makeWideLead('A');
    Cell contA = makeWideCont();
    d.writeWideCell(Row(5), Column(11), leadA, contA, blank);
    flushPaint(d);
    Cell leadB = makeWideLead('B');
    Cell contB = makeWideCont();
    d.writeWideCell(Row(5), Column(10), leadB, contB, blank);
    const Cell* col10 = d.cellAt(Row(5), Column(10));
    const Cell* col11 = d.cellAt(Row(5), Column(11));
    const Cell* col12 = d.cellAt(Row(5), Column(12));
    if (col10) {
        CHECK(col10->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(col10->codePoint == 'B');
    }
    if (col11) {
        CHECK(col11->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    }
    CHECK(col12 != nullptr);
    if (col12) {
        // Orphan cont blanked.
        CHECK(!col12->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(col12->codePoint == 0);
    }
}

TEST(testWritewidecellAtRightMarginIsRejected)
{
    /// A wide pair needs two columns; at the right margin (the last column)
    /// there's no room for the continuation half, so writeWideCell rejects the
    /// write cleanly — neither half lands and the column stays blank. (It used
    /// to assert `ci + 1 < m_columns` and abort under debug.)
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell lead = makeWideLead('W');
    Cell cont = makeWideCont();
    d.writeWideCell(Row(3), Column(79), lead, cont, blank); // 79 = last column
    const Cell* last = d.cellAt(Row(3), Column(79));
    // In range on an 80-column grid, so null means the grid is short and the
    // checks below would otherwise be skipped rather than failed.
    CHECK_MSG(last != nullptr, "the last column is addressable");
    if (last) {
        // Unchanged: no lead landed (still blank, no wide attribute).
        CHECK(!last->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(last->codePoint == 0);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// ---------- clearWideOverlap --------------------------------------

TEST(testClearwideoverlapBreaksPairWithLeadInRange)
{
    /// Pair at (5, 10..11). clearWideOverlap(5, 10, 10) — the lead is
    /// inside the range; the cont at column 11 must also be blanked
    /// (orphan cont would render as garbage).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell lead = makeWideLead('A');
    Cell cont = makeWideCont();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    flushPaint(d);
    d.clearWideOverlap(Row(5), Column(10), Column(10), blank);
    const Cell* col10 = d.cellAt(Row(5), Column(10));
    const Cell* col11 = d.cellAt(Row(5), Column(11));
    if (col10) {
        // Note: clearWideOverlap only blanks the *neighbor*, not the
        // cell whose attribute triggered the break. The caller (a narrow
        // write) overwrites col10 itself; col11 (the orphan cont)
        // is what this function blanks.
        CHECK(col10->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // lead untouched
    }
    CHECK(col11 != nullptr);
    if (col11) {
        // Orphan cont must be blanked.
        CHECK(!col11->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(col11->codePoint == 0);
    }
}

TEST(testClearwideoverlapBreaksPairWithContInRange)
{
    /// Pair at (5, 10..11). clearWideOverlap(5, 11, 11) — the cont is
    /// inside the range; the lead at column 10 must also be blanked.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell lead = makeWideLead('A');
    Cell cont = makeWideCont();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    flushPaint(d);
    d.clearWideOverlap(Row(5), Column(11), Column(11), blank);
    const Cell* col10 = d.cellAt(Row(5), Column(10));
    const Cell* col11 = d.cellAt(Row(5), Column(11));
    CHECK(col10 != nullptr);
    if (col10) {
        // Orphan lead blanked.
        CHECK(!col10->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(col10->codePoint == 0);
    }
    if (col11) {
        // The cont half itself is left untouched by clearWideOverlap
        // — caller overwrites it.
        CHECK(col11->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    }
}

TEST(testClearwideoverlapIdempotentOnCleanRange)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    Cell blank = makeBlank();
    // No wide pairs on row 5 — clearWideOverlap is a no-op.
    d.clearWideOverlap(Row(5), Column(3), Column(20), blank);
    // No work should be pending.
    CHECK(!d.hasUnpaintedWork());
    CHECK(!isRowDirty(d, 5));
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testClearwideoverlapMarksOnlyAffectedRows)
{
    /// clearWideOverlap on row 5 must not dirty rows 4 or 6.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell lead = makeWideLead('A');
    Cell cont = makeWideCont();
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    flushPaint(d);
    d.clearWideOverlap(Row(5), Column(10), Column(10), blank);
    // Row 5 dirty; rows 4 and 6 clean.
    CHECK(isRowDirty(d, 5));
    CHECK(!isRowDirty(d, 4));
    CHECK(!isRowDirty(d, 6));
}

/// ---------- Pen-equality invariant --------------------------------

TEST(testWritewidecellCanonicalizesContPen)
{
    /// The continuation half is forced to carry the lead's fg/bg colors so a
    /// half-cell repaint can't drift color across the glyph (the
    /// half-emoji-color-drift bug class). A caller that supplies a cont with
    /// mismatched colors gets them corrected — not a debug abort or, under
    /// NDEBUG, silent corruption. attributes (WideContinuation) are left as supplied.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();

    Cell lead = makeBlank();
    lead.codePoint = 'W';
    lead.pen.attributes = Ubiquitty::VT::Attribute::Wide;
    lead.pen.foregroundIndex = 5;
    lead.pen.backgroundIndex = 7;
    lead.pen.foregroundRed = 0xAB;
    lead.pen.foregroundGreen = 0xCD;
    lead.pen.foregroundBlue = 0xEF;
    lead.pen.backgroundRed = 0x12;
    lead.pen.backgroundGreen = 0x34;
    lead.pen.backgroundBlue = 0x56;

    // Every color field differs from the lead's, so the assertions below
    // fail on any field writeWideCell forgets to canonicalize.
    Cell cont = makeBlank();
    cont.pen.attributes = Ubiquitty::VT::Attribute::WideContinuation;
    cont.pen.foregroundIndex = 9;
    cont.pen.backgroundIndex = 1;
    cont.pen.foregroundRed = 0x00;
    cont.pen.foregroundGreen = 0x11;
    cont.pen.foregroundBlue = 0x22;
    cont.pen.backgroundRed = 0x33;
    cont.pen.backgroundGreen = 0x44;
    cont.pen.backgroundBlue = 0x55;

    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    const Cell* gotCont = d.cellAt(Row(5), Column(11));
    CHECK_MSG(gotCont != nullptr, "the continuation column is addressable");
    if (gotCont) {
        // Colors canonicalized to the lead's; the WideContinuation attribute kept.
        CHECK(gotCont->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(gotCont->pen.foregroundIndex == 5);
        CHECK(gotCont->pen.backgroundIndex == 7);
        CHECK(gotCont->pen.foregroundRed == 0xAB && gotCont->pen.foregroundGreen == 0xCD && gotCont->pen.foregroundBlue == 0xEF);
        CHECK(gotCont->pen.backgroundRed == 0x12 && gotCont->pen.backgroundGreen == 0x34 && gotCont->pen.backgroundBlue == 0x56);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testWritewidecellWithMatchingPenPasses)
{
    /// Pen identical except attributes (Attribute::Wide vs Attribute::WideContinuation) — the
    /// normal happy-path case. Verifies the assert isn't over-strict.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    Cell blank = makeBlank();
    Cell lead = makeBlank();
    lead.codePoint = 'W';
    lead.pen.attributes = { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::Bold }; // extra attribute OK
    lead.pen.foregroundIndex = 5;
    lead.pen.backgroundIndex = 7;
    lead.pen.foregroundRed = 0xAB;
    lead.pen.foregroundGreen = 0xCD;
    lead.pen.foregroundBlue = 0xEF;
    lead.pen.backgroundRed = 0x12;
    lead.pen.backgroundGreen = 0x34;
    lead.pen.backgroundBlue = 0x56;
    Cell cont = lead;
    cont.codePoint = 0;
    cont.pen.attributes = { Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Bold };
    d.writeWideCell(Row(5), Column(10), lead, cont, blank);
    const Cell* gotLead = d.cellAt(Row(5), Column(10));
    const Cell* gotCont = d.cellAt(Row(5), Column(11));
    CHECK_MSG(gotLead != nullptr && gotCont != nullptr, "both halves are addressable");
    if (gotLead) {
        CHECK(gotLead->pen.foregroundIndex == 5);
        CHECK(gotLead->pen.backgroundIndex == 7);
        CHECK(gotLead->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(gotLead->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    }
    if (gotCont) {
        CHECK(gotCont->pen.foregroundIndex == 5);
        CHECK(gotCont->pen.backgroundIndex == 7);
        CHECK(gotCont->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(gotCont->pen.foregroundRed == 0xAB && gotCont->pen.foregroundGreen == 0xCD && gotCont->pen.foregroundBlue == 0xEF);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

/// ---------- Selection-dirty bitmap --------------------------------

TEST(testMarkselectionflippedSetsRowRange)
{
    /// After markSelectionFlipped(5, 3, 7): hasUnpaintedWork true, row
    /// 5 yields a PaintRun spanning columns 3..7 with selectionOnly.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    CHECK(!d.hasUnpaintedWork());
    d.markSelectionFlipped(Row(5), Column(3), Column(7));
    CHECK(d.hasUnpaintedWork());
    int runs = 0;
    int firstColumn = -1, lastColumn = -1, runRow = -1;
    bool sawSelectionOnly = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (runs == 0) {
                firstColumn = run.firstColumn.value();
                lastColumn = run.lastColumn.value();
                runRow = run.row.value();
            }
            if (run.selectionOnly)
                sawSelectionOnly = true;
            ++runs;
            ses.ackRun(run);
        }
    }
    CHECK(runs == 1);
    CHECK(runRow == 5);
    CHECK(firstColumn == 3);
    CHECK(lastColumn == 7);
    CHECK(sawSelectionOnly);
}

TEST(testMarkselectionflippedDoesNotDirtyCells)
{
    /// markSelectionFlipped must not mutate cell content. After the
    /// call: cells equal their pre-flip values; the yielded run has
    /// selectionOnly=true.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Establish baseline content.
    for (int c = 0; c < 10; ++c)
        d.setCell(Row(5), Column(c), makeCharacter(uint16_t('a' + c)));
    flushPaint(d);
    Cell before[10];
    for (int c = 0; c < 10; ++c)
        before[c] = *d.cellAt(Row(5), Column(c));
    d.markSelectionFlipped(Row(5), Column(2), Column(7));
    // Cells byte-equal to before-snapshot.
    for (int c = 0; c < 10; ++c) {
        const Cell* now = d.cellAt(Row(5), Column(c));
        CHECK(now != nullptr);
        if (now)
            CHECK(memcmp(now, &before[c], sizeof(Cell)) == 0);
    }
    /// Yielded run is selection-only — no content was dirtied.
    bool allSelectionOnly = true;
    int runs = 0;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++runs;
            if (!run.selectionOnly)
                allSelectionOnly = false;
            ses.ackRun(run);
        }
    }
    CHECK(runs == 1);
    CHECK(allSelectionOnly);
}

TEST(testMarkselectionflippedUnrelatedRowStaysClean)
{
    /// Flip row 5 only — rows 4 and 6 must not yield runs.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    d.markSelectionFlipped(Row(5), Column(0), Column(10));
    bool sawRow4 = false, sawRow6 = false, sawRow5 = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 4)
                sawRow4 = true;
            if (run.row.value() == 5)
                sawRow5 = true;
            if (run.row.value() == 6)
                sawRow6 = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow5);
    CHECK(!sawRow4);
    CHECK(!sawRow6);
}

TEST(test_clearSelectionDirty_resets_all_rows)
{
    /// Flip rows 3, 5, 7. clearSelectionDirty() (public alias for the
    /// private clearAllSelectionDirty) clears every row's bitmap.
    /// Verify by paint-iteration emitting zero selection-only runs.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    d.markSelectionFlipped(Row(3), Column(0), Column(5));
    d.markSelectionFlipped(Row(5), Column(2), Column(7));
    d.markSelectionFlipped(Row(7), Column(10), Column(20));
    CHECK(d.hasUnpaintedWork());
    d.clearSelectionDirty();
    /// No selection-only runs should be yielded for any row.
    int selectionRuns = 0;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.selectionOnly)
                ++selectionRuns;
            ses.ackRun(run);
        }
    }
    CHECK(selectionRuns == 0);
    // Note: clearSelectionDirty does not reset m_anyDirtyInRow (that
    // hint is over-claiming after the clear; the iterator's scan
    // downgrades it on next paint when it finds no dirty cell). The
    // mutations-since-last-paint sticky bit also remains. assertInvariants
    // holds only after a full paint has drained the over-claimed hint:
    // the iterator's per-cell rescan is what clears it, and drainPaint
    // above ran that rescan.
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
}

TEST(testSelectionflippedYieldedByPaintsession)
{
    /// markSelectionFlipped on a range of cells that already match
    /// the shadow → PaintSession yields exactly that range with
    /// selectionOnly=true (content didn't change).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    // Populate then paint so shadow == cells.
    for (int c = 5; c <= 12; ++c)
        d.setCell(Row(8), Column(c), makeCharacter(uint16_t('a' + c - 5)));
    flushPaint(d);
    d.markSelectionFlipped(Row(8), Column(6), Column(10));
    int matchedRuns = 0;
    int firstColumn = -1, lastColumn = -1;
    bool selectionOnly = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 8) {
                ++matchedRuns;
                firstColumn = run.firstColumn.value();
                lastColumn = run.lastColumn.value();
                selectionOnly = run.selectionOnly;
            }
            ses.ackRun(run);
        }
    }
    CHECK(matchedRuns == 1);
    CHECK(firstColumn == 6);
    CHECK(lastColumn == 10);
    CHECK(selectionOnly);
}

/// ---------- Interaction between selection + cell dirty ------------

TEST(testCellWriteDoesNotSetSelectionDirty)
{
    /// setCell touches m_anyDirtyInRow but does not touch m_selectionDirty.
    /// Probe: the yielded run for the touched cell must have
    /// selectionOnly=false (it's a content-dirty run, not selection).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    d.setCell(Row(5), Column(10), makeCharacter('X'));
    bool anySelectionOnly = false;
    int runs = 0;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            ++runs;
            if (run.selectionOnly)
                anySelectionOnly = true;
            ses.ackRun(run);
        }
    }
    CHECK(runs >= 1);
    CHECK(!anySelectionOnly);
}

TEST(testSelectionDirtyAloneMakesHasunpaintedworkTrue)
{
    /// No cell mutation — only markSelectionFlipped. hasUnpaintedWork
    /// must return true. (The sticky m_anyMutationsSinceLastPaint flag
    /// is set inside markSelectionFlipped.)
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    flushPaint(d);
    CHECK(!d.hasUnpaintedWork());
    d.markSelectionFlipped(Row(5), Column(3), Column(7));
    CHECK(d.hasUnpaintedWork());
}

TEST(test_assertInvariants_catches_selection_drift)
{
    /// Sequence:
    ///   1. write cells on row 5
    ///   2. paint + ack — row 5 now clean (shadow == active)
    ///   3. markSelectionFlipped on row 5
    ///   4. do not paint
    ///
    /// The mutator set m_anyDirtyInRow[5] = true, so assertInvariants
    /// sees a "dirty" row and doesn't probe further — it returns true.
    /// That's fine for correctness (the hint is not over-claiming: row 5
    /// has selection-dirty bits, which is work). The interesting case is
    /// the reverse: a row marked clean that has selection bits set
    /// would return false. We can't easily fabricate that through the
    /// public API (mutators always set m_anyDirtyInRow), so this test
    /// verifies the simpler claim: post-flip + no-paint, the
    /// invariants still hold and hasUnpaintedWork is true.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(5), Column(0), makeCharacter('A'));
    flushPaint(d);
    d.markSelectionFlipped(Row(5), Column(2), Column(5));
    // No paint. Invariants should hold because the row is *marked*
    // dirty (the assertInvariants check skips dirty rows).
    TEST_ASSERT_DISPLAY_INVARIANTS(d);
    CHECK(d.hasUnpaintedWork());
}

/// ---------- DECBLINK animator hint --------------------------------

static Cell makeBlinkCharacter(uint16_t ch)
{
    Cell c = makeCharacter(ch);
    c.pen.attributes.add(Ubiquitty::VT::Attribute::Blink);
    return c;
}

TEST(test_blinkHint_false_after_init)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    CHECK(!d.hasBlinkHint());
}

TEST(test_blinkHint_set_by_setCell_with_attr_blink)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    CHECK(!d.hasBlinkHint());
    d.setCell(Row(0), Column(0), makeBlinkCharacter('A'));
    CHECK(d.hasBlinkHint());
}

TEST(test_blinkHint_set_by_setRowSpan_with_attr_blink)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    CHECK(!d.hasBlinkHint());
    Cell row[3] = { makeCharacter('a'), makeBlinkCharacter('b'), makeCharacter('c') };
    RowView span;
    span.data = row;
    span.count = 3;
    d.setRowSpan(Row(1), Column(0), span);
    CHECK(d.hasBlinkHint());
}

TEST(test_blinkHint_sticky_until_clear_called)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    d.setCell(Row(0), Column(0), makeBlinkCharacter('A'));
    CHECK(d.hasBlinkHint());
    // Overwrite with non-blink — hint is conservative, stays true.
    d.setCell(Row(0), Column(0), makeCharacter('A'));
    CHECK(d.hasBlinkHint());
    // Caller invokes clearBlinkHintIfClean after a scan confirms no
    // blink cells remain; hint drops.
    d.clearBlinkHintIfClean();
    CHECK(!d.hasBlinkHint());
    // Subsequent plain writes don't re-arm the hint.
    d.setCell(Row(1), Column(1), makeCharacter('B'));
    CHECK(!d.hasBlinkHint());
    // But a blink write does.
    d.setCell(Row(2), Column(2), makeBlinkCharacter('C'));
    CHECK(d.hasBlinkHint());
}

/// Shape of the shared blink-alpha curve (Ubiquitty::blinkAlpha). The text
/// blink (SGR 5/6) and the cursor caret read the same function: a step ramp
/// with one solid hold, symmetric quarter-step fade-out, an off hold, and a
/// symmetric step-up — 60 ticks per slow-blink cycle, 30 for fast.
TEST(test_blinkAlpha_period_and_bounds)
{
    /// Output is always in [0, 1], hits both endpoints once per cycle, and
    /// repeats with the requested period.
    bool sawZero = false;
    bool sawOne = false;
    for (uint32_t t = 0; t < Cursor::Blink::caretPeriodTicks; ++t) {
        const float a = blinkAlpha(t, Cursor::Blink::caretPeriodTicks);
        CHECK(a >= 0.0f && a <= 1.0f);
        if (a == 0.0f)
            sawZero = true;
        if (a == 1.0f)
            sawOne = true;
    }
    CHECK(sawZero);
    CHECK(sawOne);
    // Period-stable: alpha at t == alpha at t + period.
    for (uint32_t t = 0; t < 8; ++t) {
        CHECK(blinkAlpha(t, Cursor::Blink::caretPeriodTicks) == blinkAlpha(t + Cursor::Blink::caretPeriodTicks, Cursor::Blink::caretPeriodTicks));
    }
}

/// GtkText's caret blink (gtk/gtktext.c, blink_cb and blink_alpha): a lit
/// quarter, a linear fade out, a dark quarter and a linear fade in; input holds
/// the cycle back half a period; the caret is lit for good past the timeout.
TEST(test_toolkitBlinkPhase_follows_gtktext)
{
    using Cursor::Blink::ToolkitBlink;
    using Cursor::Blink::toolkitBlinkDelayMicroseconds;
    using Cursor::Blink::toolkitBlinkPhase;
    const ToolkitBlink blink { .isEnabled = true, .periodMilliseconds = 1200, .timeoutSeconds = 10 };
    CHECK(toolkitBlinkDelayMicroseconds(blink, true) == 600000);
    CHECK(toolkitBlinkDelayMicroseconds(blink, false) == 0);

    // Just typed: the cycle starts in 600 ms and its first quarter is lit.
    auto phase = toolkitBlinkPhase(blink, -600000, 0);
    CHECK(phase.alpha == 1.0f && phase.microsecondsUntilChange == 900000);
    phase = toolkitBlinkPhase(blink, 150000, 750000);
    CHECK(phase.alpha == 1.0f && phase.microsecondsUntilChange == 150000);
    // Halfway through the fade out, and through the fade in: a fade changes
    // every frame.
    phase = toolkitBlinkPhase(blink, 450000, 1050000);
    CHECK(phase.alpha == 0.5f && phase.microsecondsUntilChange == 0);
    phase = toolkitBlinkPhase(blink, 1050000, 1650000);
    CHECK(phase.alpha == 0.5f && phase.microsecondsUntilChange == 0);
    // Dark until three quarters.
    phase = toolkitBlinkPhase(blink, 600000, 1200000);
    CHECK(phase.alpha == 0.0f && phase.microsecondsUntilChange == 300000);
    // The next cycle repeats the first.
    phase = toolkitBlinkPhase(blink, 1200000, 1800000);
    CHECK(phase.alpha == 1.0f && phase.microsecondsUntilChange == 300000);
    // A dark stretch the timeout cuts short wakes just past the timeout, when
    // the caret comes back for good.
    phase = toolkitBlinkPhase(blink, 600000, 9900000);
    CHECK(phase.alpha == 0.0f && phase.microsecondsUntilChange == 100001);
    phase = toolkitBlinkPhase(blink, 600000, 10000001);
    CHECK(phase.alpha == 1.0f && phase.microsecondsUntilChange < 0);
    // gtk-cursor-blink off: lit, and nothing to wake for.
    phase = toolkitBlinkPhase({ .isEnabled = false }, 600000, 0);
    CHECK(phase.alpha == 1.0f && phase.microsecondsUntilChange < 0);
}

TEST(test_toolkitBlinkPaintPhase_hasBoundedStepsAndNoIdleFramesPastTimeout)
{
    using Cursor::Blink::ToolkitBlink;
    using Cursor::Blink::toolkitBlinkPaintPhase;
    const ToolkitBlink blink { .isEnabled = true, .periodMilliseconds = 1200, .timeoutSeconds = 10 };
    float lastAlpha = 1.0f;
    int changes = 0;
    for (std::int64_t milliseconds = 0; milliseconds < 1200; ++milliseconds) {
        const auto phase = toolkitBlinkPaintPhase(blink, milliseconds * 1000, milliseconds * 1000);
        if (phase.alpha != lastAlpha) {
            ++changes;
            lastAlpha = phase.alpha;
        }
        CHECK(phase.microsecondsUntilChange != 0);
    }
    CHECK_EQ(changes, 15);

    for (std::int64_t milliseconds = 10001; milliseconds < 60000; milliseconds += 50) {
        const auto phase = toolkitBlinkPaintPhase(blink, milliseconds * 1000, milliseconds * 1000);
        CHECK(phase.alpha == 1.0f);
        CHECK(phase.microsecondsUntilChange < 0);
    }
    const auto cutShort = toolkitBlinkPaintPhase(blink, 450000, 9990000);
    CHECK(cutShort.alpha == 0.5f && cutShort.microsecondsUntilChange == 10001);
}

TEST(test_blinkAlpha_slow_matches_cursor_curve)
{
    // SGR 5 (slow) uses the cursor period: the helper must reproduce
    // Cursor::Blink::alphaAt exactly across a full cycle.
    for (uint32_t t = 0; t < Cursor::Blink::caretPeriodTicks; ++t) {
        CHECK(blinkAlpha(t, Cursor::Blink::caretPeriodTicks) == Cursor::Blink::alphaAt(t));
    }
}

TEST(test_blinkAlpha_fast_is_half_period)
{
    /// SGR 6 (fast) cycles at half the period: alpha(t, 30) == alpha(2t, 60).
    /// Sample across the fast period to confirm the entire shape, not just
    /// one anchor frame.
    const uint32_t fastPeriod = Cursor::Blink::caretPeriodTicks / 2;
    for (uint32_t t = 0; t < fastPeriod; ++t) {
        CHECK(blinkAlpha(t, fastPeriod) == Cursor::Blink::alphaAt(2 * t));
    }
}

TEST(test_blinkAlpha_monotone_fade_within_segment)
{
    // The step-down quartet (1.00 → 0.75 → 0.50 → 0.25 → 0.00) must be
    // non-increasing; the step-up quartet must be non-decreasing.
    float prev = blinkAlpha(0, Cursor::Blink::caretPeriodTicks);
    bool gotOff = false;
    for (uint32_t t = 1; t < Cursor::Blink::caretPeriodTicks; ++t) {
        const float a = blinkAlpha(t, Cursor::Blink::caretPeriodTicks);
        if (!gotOff) {
            CHECK(a <= prev); // fade-out then off
            if (a == 0.0f)
                gotOff = true;
        } else {
            CHECK(a >= prev); // recovery
        }
        prev = a;
    }
}

TEST(testBlinkNextChangeMatchesCurve)
{
    const Cursor::Blink::Animation originalStyle = Cursor::Blink::animation();
    for (Cursor::Blink::Animation style : { Cursor::Blink::Animation::Fade, Cursor::Blink::Animation::OnOff }) {
        Cursor::Blink::setAnimation(style);
        for (uint32_t period : { Cursor::Blink::caretPeriodTicks, Cursor::Blink::caretPeriodTicks / 2 }) {
            for (uint32_t tick = 0; tick < period; ++tick) {
                const float alpha = blinkAlpha(tick, period);
                const uint32_t delay = ticksUntilBlinkAlphaChange(tick, period);
                CHECK(delay > 0 && delay <= period);
                for (uint32_t before = 1; before < delay; ++before) {
                    CHECK(blinkAlpha(tick + before, period) == alpha);
                }
                CHECK(blinkAlpha(tick + delay, period) != alpha);
            }
        }
    }
    Cursor::Blink::setAnimation(originalStyle);
    CHECK(ticksUntilBlinkAlphaChange(Cursor::Blink::caretPeriodTicks - 1, Cursor::Blink::caretPeriodTicks) == 1);
    CHECK(ticksUntilBlinkAlphaChange(0, 0) == 0);

    Cursor::Blink::setAnimation(Cursor::Blink::Animation::Fade);
    uint32_t tick = 0;
    uint32_t fireCount = 0;
    while (tick < Cursor::Blink::caretPeriodTicks * 5) {
        tick += ticksUntilBlinkAlphaChange(tick, Cursor::Blink::caretPeriodTicks);
        ++fireCount;
    }
    CHECK(tick == Cursor::Blink::caretPeriodTicks * 5);
    CHECK(fireCount == 40);
    Cursor::Blink::setAnimation(originalStyle);
}

/// The CellGrid-level combiner samples the two text-blink periods together
/// (SGR 5 slow, SGR 6 fast). Each period is checked against Cursor::Blink::alphaAt
/// directly — pair-consistency with ticksUntilAnimationRedraw alone would
/// stay green if both halves dropped the same period. Fast blink is the
/// same curve at double rate (test_blinkAlpha_fast_is_half_period), so
/// alphaAt(2t) is the fast oracle.
TEST(testNeedsAnimationRedrawMatchesBothCurves)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    const Cursor::Blink::Animation originalStyle = Cursor::Blink::animation();
    for (Cursor::Blink::Animation style : { Cursor::Blink::Animation::Fade, Cursor::Blink::Animation::OnOff }) {
        Cursor::Blink::setAnimation(style);
        for (uint32_t last = 0; last < Cursor::Blink::caretPeriodTicks; ++last) {
            for (uint32_t dt = 0; dt <= 2 * Cursor::Blink::caretPeriodTicks; ++dt) {
                const uint32_t now = last + dt;
                const bool didSlowCross = Cursor::Blink::alphaAt(now) != Cursor::Blink::alphaAt(last);
                const bool didFastCross = Cursor::Blink::alphaAt(2 * now) != Cursor::Blink::alphaAt(2 * last);
                CHECK(needsAnimationRedraw(&d, now, last) == (didSlowCross || didFastCross));
            }
        }
    }
    Cursor::Blink::setAnimation(originalStyle);
    // Null grid: no redraw — part of the contract, beside ticksUntil's 0.
    CHECK(!needsAnimationRedraw(nullptr, 1, 0));
}

/// The schedule half of the pair: the delay is the earliest tick at which
/// needsAnimationRedraw goes true. One earlier wakes a frame that draws
/// nothing; one later drops a blink step. The frontends' animation timers
/// sleep exactly this number, so the boundary is the contract. The upper
/// bound pins that the fast period is consulted: within any half-period
/// window the fast curve always steps.
TEST(testTicksUntilAnimationRedrawIsTheFirstCrossing)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    const Cursor::Blink::Animation originalStyle = Cursor::Blink::animation();
    for (Cursor::Blink::Animation style : { Cursor::Blink::Animation::Fade, Cursor::Blink::Animation::OnOff }) {
        Cursor::Blink::setAnimation(style);
        for (uint32_t tick = 0; tick < Cursor::Blink::caretPeriodTicks; ++tick) {
            const uint32_t delay = ticksUntilAnimationRedraw(&d, tick);
            CHECK(delay >= 1 && delay <= Cursor::Blink::caretPeriodTicks / 2);
            for (uint32_t before = 0; before < delay; ++before) {
                CHECK(!needsAnimationRedraw(&d, tick + before, tick));
            }
            CHECK(needsAnimationRedraw(&d, tick + delay, tick));
        }
    }
    Cursor::Blink::setAnimation(originalStyle);
    CHECK(ticksUntilAnimationRedraw(nullptr, 7) == 0);
}

TEST(test_resolveRenderCell_blink_alpha_population)
{
    // No Attribute::Blink → blinkAlpha == 1.0 regardless of tick.
    Cell plain = makeCharacter('A');
    const RenderCell r1 = resolveRenderCell(&plain, 17);
    CHECK(r1.blinkAlpha == 1.0f);
    CHECK(r1.codePoint == 'A');
    // Attribute::Blink slow: equals blinkAlpha(t, caretPeriodTicks).
    Cell blink = makeCharacter('B');
    blink.pen.attributes.add(Ubiquitty::VT::Attribute::Blink);
    const RenderCell r2 = resolveRenderCell(&blink, 42);
    CHECK(r2.blinkAlpha == blinkAlpha(42, Cursor::Blink::caretPeriodTicks));
    // Attribute::Blink + ExtendedAttribute::BlinkFast → half-period sample.
    Cell fast = makeCharacter('C');
    fast.pen.attributes.add(Ubiquitty::VT::Attribute::Blink);
    fast.pen.extendedAttributes.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    const RenderCell r3 = resolveRenderCell(&fast, 42);
    CHECK(r3.blinkAlpha == blinkAlpha(42, Cursor::Blink::caretPeriodTicks / 2));
    /// Null input is safe and produces a fully-opaque blank.
    const RenderCell r4 = resolveRenderCell(nullptr, 0);
    CHECK(r4.codePoint == 0);
    CHECK(r4.blinkAlpha == 1.0f);
}

TEST(test_blinkHint_unchanged_by_plain_writes)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 10);
    d.setCell(Row(0), Column(0), makeCharacter('X'));
    d.setCell(Row(1), Column(1), makeCharacter('Y'));
    Cell rowCells[3] = { makeCharacter('p'), makeCharacter('q'), makeCharacter('r') };
    RowView span;
    span.data = rowCells;
    span.count = 3;
    d.setRowSpan(Row(2), Column(0), span);
    CHECK(!d.hasBlinkHint());
}

// rowCells out-of-range returns the empty RowView; the in-range
// path lands the row pointer + column count.
TEST(testRowCellsOutOfRangeYieldsTheEmptyView)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(3, 4);
    auto rv = d.rowCells(Row(-1));
    CHECK(rv.data == nullptr);
    CHECK(rv.count == 0);
    rv = d.rowCells(Row(99));
    CHECK(rv.data == nullptr);
    rv = d.rowCells(Row(0));
    CHECK(rv.data != nullptr);
    CHECK(rv.count == 4);
}

// PaintSession move-assignment: surfaces the operator= path
// (constructor-flavoured move already runs in the iterator tests).
TEST(testPaintSessionMoveAssignmentLeavesTheSessionUsable)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(2, 4);
    d.setCell(Row(0), Column(0), makeCharacter('Z'));
    PaintSession a = d.beginPaint();
    PaintSession b = d.beginPaint(); // second begin returns moved-out lvalue
    b = std::move(a);
    // The moved-into session owns the grid's paint state, so it yields the run
    // the write dirtied. Compiling and not crashing is what the operator= path
    // would demonstrate on its own, and a session that came out inert would do
    // both.
    PaintRun run {};
    CHECK(b.nextDirtyRun(&run));
}

// cellAt(ScreenMode) reads a SPECIFIC screen regardless of which is
// active: the snapshot serializes the frozen primary while the
// alternate screen is displayed. Distinct content per screen + an active
// switch proves the per-screen read targets the right store, not the
// active one.
TEST(testCellAtForOneScreenIgnoresWhichIsActive)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.setCell(Row(2), Column(3), makeCharacter('P')); // primary active
    CHECK(d.cellAt(Row(2), Column(3))->codePoint == 'P');
    d.setAlternateActive(ScreenMode::Alternate);
    d.setCell(Row(2), Column(3), makeCharacter('A')); // alternate now active
    CHECK(d.cellAt(Row(2), Column(3))->codePoint == 'A'); // active read = alternate
    CHECK(d.cellAt(ScreenMode::Alternate, Row(2), Column(3))->codePoint == 'A');
    CHECK(d.cellAt(ScreenMode::Primary, Row(2), Column(3))->codePoint == 'P'); // frozen primary
    d.setAlternateActive(ScreenMode::Primary);
    CHECK(d.cellAt(ScreenMode::Primary, Row(2), Column(3))->codePoint == 'P');
    CHECK(d.cellAt(ScreenMode::Alternate, Row(2), Column(3))->codePoint == 'A'); // frozen alternate
    CHECK(d.cellAt(ScreenMode::Primary, Row(99), Column(0)) == nullptr);
    CHECK(d.cellAt(ScreenMode::Alternate, Row(0), Column(999)) == nullptr);
}

// setRowSpan(ScreenMode) writes a SPECIFIC screen — the write mirror of
// cellAt(ScreenMode), used by the snapshot apply to reconstruct the frozen
// primary. The inactive write lands without disturbing the active screen;
// the ScreenMode==active overload delegates to the paint-tracked path.
TEST(testSetRowSpanForOneScreenWritesThatScreen)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(4, 8);
    d.setAlternateActive(ScreenMode::Alternate); // alternate active; primary inactive
    d.setCell(Row(0), Column(0), makeCharacter('Z')); // active alternate content
    Cell primaryRow[3] = { makeCharacter('x'), makeCharacter('y'), makeCharacter('z') };
    RowView span { primaryRow, 3 };
    d.setRowSpan(ScreenMode::Primary, Row(0), Column(0), span); // write the inactive primary
    CHECK(d.cellAt(ScreenMode::Primary, Row(0), Column(0))->codePoint == 'x');
    CHECK(d.cellAt(ScreenMode::Primary, Row(0), Column(2))->codePoint == 'z');
    CHECK(d.cellAt(Row(0), Column(0))->codePoint == 'Z'); // active alternate untouched
    d.setAlternateActive(ScreenMode::Primary); // flip: the written row is now active
    CHECK(d.cellAt(Row(0), Column(1))->codePoint == 'y');
    // ScreenMode == active delegates to the paint-tracked write.
    Cell activeRow[2] = { makeCharacter('A'), makeCharacter('B') };
    RowView span2 { activeRow, 2 };
    d.setRowSpan(ScreenMode::Primary, Row(2), Column(0), span2); // primary now active
    CHECK(d.cellAt(Row(2), Column(0))->codePoint == 'A');
    CHECK(d.cellAt(Row(2), Column(1))->codePoint == 'B');
}

UBIQUITTY_TEST_MAIN()
