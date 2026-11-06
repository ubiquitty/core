// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_display_shadow.cpp — public BB-shadow surface (per-cell rw +
// row/band invalidation + view-offset shift) added to CellGrid so the
// PAL's scrollback view-offset painter can do its own dirty diff
// against BB pixels without paying a full-screen repaint per
// scrollbar tick.
//
// Covers:
//   - readShadowCell: nullptr when shadow invalid; nullptr OOB;
//     non-null after a paint cycle validates the shadow.
//   - writeShadowCell: round-trips through readShadowCell; OOB is no-op.
//   - markShadowValid: flips m_bbShadowValid so subsequent reads land.
//   - invalidateRowShadow: zeros a row + sets it dirty for the next
//     paint, bumping hasUnpaintedWork.
//   - shiftShadowForViewOffset: positive n shifts content up,
//     negative n shifts content down, zero / out-of-band-size shifts
//     are no-ops, invalid-shadow precondition skips the work.
//
// White-box note: the shadow array is private. The tests probe it
// indirectly via readShadowCell after seeding with writeShadowCell
// + markShadowValid. Since both surfaces live on the same in-memory
// buffer, observing one through the other is a fair proxy for the
// effect a PAL caller would see.

#include "Core/CellGrid.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace Ubiquitty;

static Cell makeBlank()
{
    Cell c;
    c = {};
    c.codePoint = ' ';
    c.pen.foregroundIndex = colorDefault;
    c.pen.backgroundIndex = colorDefault;
    return c;
}

static Cell makeCharacter(uint16_t ch)
{
    Cell c = makeBlank();
    c.codePoint = ch;
    return c;
}

/// Drain a paint session: ack any pending scroll + every dirty run.
/// Mirrors what the PAL would do — leaves CellGrid in the post-paint
/// state where m_bbShadowValid is true.
static void drainPaint(CellGrid& d)
{
    PaintSession ses = d.beginPaint();
    Screen::PendingScroll sc;
    while (ses.nextPendingScroll(&sc))
        ses.ackScroll(sc);
    PaintRun run;
    while (ses.nextDirtyRun(&run))
        ses.ackRun(run);
}

/// ---------- readShadowCell ----------------------------------------

TEST(test_readShadowCell_nullptr_when_invalid)
{
    /// Post-invalidate: shadow bytes can't be trusted; readShadowCell
    /// must return nullptr regardless of coords.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.invalidateShadow();
    CHECK(d.readShadowCell(0, 0) == nullptr);
    CHECK(d.readShadowCell(5, 10) == nullptr);
    CHECK(d.readShadowCell(23, 79) == nullptr);
}

TEST(test_readShadowCell_nonnull_after_paint_validates)
{
    /// init() seeds the shadow + flips the valid bit; the first paint
    /// here just confirms the post-init state. readShadowCell on an
    /// in-bounds cell yields a non-null pointer.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    const Cell* p = d.readShadowCell(3, 7);
    CHECK(p != nullptr);
    // And after a real paint cycle of a setCell write, the shadow at
    // that cell matches the value we wrote.
    Cell X = makeCharacter('X');
    d.setCell(Row(3), Column(7), X);
    drainPaint(d);
    p = d.readShadowCell(3, 7);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->codePoint == 'X');
}

TEST(test_readShadowCell_out_of_bounds_returns_null)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    // Negative row / column.
    CHECK(d.readShadowCell(-1, 0) == nullptr);
    CHECK(d.readShadowCell(0, -1) == nullptr);
    // Past the active geometry.
    CHECK(d.readShadowCell(24, 0) == nullptr);
    CHECK(d.readShadowCell(0, 80) == nullptr);
    CHECK(d.readShadowCell(100, 100) == nullptr);
}

/// ---------- writeShadowCell ---------------------------------------

TEST(test_writeShadowCell_round_trips_via_read)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    Cell Y = makeCharacter('Y');
    d.writeShadowCell(5, 11, Y);
    const Cell* got = d.readShadowCell(5, 11);
    CHECK(got != nullptr);
    if (got) {
        CHECK(got->codePoint == 'Y');
        CHECK(std::memcmp(got, &Y, sizeof(Cell)) == 0);
    }
    /// Neighbouring cells unaffected.
    const Cell* left = d.readShadowCell(5, 10);
    const Cell* right = d.readShadowCell(5, 12);
    CHECK(left != nullptr);
    CHECK(right != nullptr);
    if (left)
        CHECK(left->codePoint != 'Y');
    if (right)
        CHECK(right->codePoint != 'Y');
}

TEST(test_writeShadowCell_out_of_bounds_is_noop)
{
    /// OOB writes must not crash and must not touch any in-bounds
    /// cell. Probe by sampling a few in-bounds cells before / after
    /// a barrage of OOB writes.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    Cell snapshot[3];
    snapshot[0] = *d.readShadowCell(0, 0);
    snapshot[1] = *d.readShadowCell(12, 40);
    snapshot[2] = *d.readShadowCell(23, 79);
    Cell tainted = makeCharacter('Z');
    d.writeShadowCell(-1, 0, tainted);
    d.writeShadowCell(0, -1, tainted);
    d.writeShadowCell(24, 0, tainted);
    d.writeShadowCell(0, 80, tainted);
    d.writeShadowCell(100, 100, tainted);
    CHECK(std::memcmp(d.readShadowCell(0, 0), &snapshot[0], sizeof(Cell)) == 0);
    CHECK(std::memcmp(d.readShadowCell(12, 40), &snapshot[1], sizeof(Cell)) == 0);
    CHECK(std::memcmp(d.readShadowCell(23, 79), &snapshot[2], sizeof(Cell)) == 0);
}

/// ---------- markShadowValid ---------------------------------------

TEST(test_markShadowValid_flips_flag)
{
    /// After invalidateShadow, readShadowCell returns nullptr. After
    /// the PAL has filled the shadow via writeShadowCell on every
    /// cell, it calls markShadowValid() and readShadowCell becomes
    /// usable again.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    d.invalidateShadow();
    CHECK(d.readShadowCell(0, 0) == nullptr);
    // Touch a few cells via writeShadowCell — writes succeed even
    // with shadow invalid because writeShadowCell guards on OOB only.
    Cell A = makeCharacter('A');
    d.writeShadowCell(2, 3, A);
    // Reads still nullptr because the flag is still false.
    CHECK(d.readShadowCell(2, 3) == nullptr);
    d.markShadowValid();
    /// Reads now land — and pick up the byte we just wrote.
    const Cell* p = d.readShadowCell(2, 3);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->codePoint == 'A');
}

/// ---------- invalidateRowShadow -----------------------------------

TEST(test_invalidateRowShadow_zeros_row_and_dirties)
{
    /// Seed row 7 of the shadow via setCell+paint (drives the cells
    /// and the shadow). Then invalidateRowShadow(7): the shadow row
    /// is zeroed, and the row is marked dirty so hasUnpaintedWork
    /// flips to true and the next paint touches row 7. Other rows
    /// are unaffected.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    for (int c = 0; c < 5; ++c)
        d.setCell(Row(7), Column(c), makeCharacter(uint16_t('a' + c)));
    drainPaint(d);
    CHECK(!d.hasUnpaintedWork());
    /// Pre-state: shadow has the painted content.
    const Cell* pre = d.readShadowCell(7, 0);
    CHECK(pre != nullptr);
    if (pre)
        CHECK(pre->codePoint == 'a');
    // Invalidate row 7.
    d.invalidateRowShadow(7);
    /// Shadow row 7 is zeroed across every column. (Cell() default
    /// constructs to all-zero bytes — including ch == 0, which the
    /// Cell struct treats as a sentinel "no glyph".)
    const Cell zero = {};
    for (int c = 0; c < d.columns(); ++c) {
        const Cell* p = d.readShadowCell(7, c);
        CHECK(p != nullptr);
        if (p)
            CHECK(std::memcmp(p, &zero, sizeof(Cell)) == 0);
    }
    // Dirty state bumped.
    CHECK(d.hasUnpaintedWork());
    /// Next paint surfaces row 7 as dirty (the iterator finds active
    /// row != zeroed shadow on every populated cell).
    bool sawRow7 = false;
    bool sawOtherRow = false;
    {
        PaintSession ses = d.beginPaint();
        Screen::PendingScroll sc;
        while (ses.nextPendingScroll(&sc))
            ses.ackScroll(sc);
        PaintRun run;
        while (ses.nextDirtyRun(&run)) {
            if (run.row.value() == 7)
                sawRow7 = true;
            else
                sawOtherRow = true;
            ses.ackRun(run);
        }
    }
    CHECK(sawRow7);
    CHECK(!sawOtherRow);
}

TEST(test_invalidateRowShadow_out_of_bounds_is_noop)
{
    /// OOB calls must not crash and must not bump dirty state.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    CHECK(!d.hasUnpaintedWork());
    d.invalidateRowShadow(-1);
    d.invalidateRowShadow(24);
    d.invalidateRowShadow(9999);
    CHECK(!d.hasUnpaintedWork());
}

// ---------- shiftShadowForViewOffset ------------------------------

/// Seed every row's column 0 with a unique marker so we can verify
/// shifts by reading shadow[r][0] before/after.
static void seedShadowMarkers(CellGrid& d, int firstRow, int lastRow)
{
    for (int r = firstRow; r <= lastRow; ++r) {
        Cell mark = makeCharacter(uint16_t('A' + (r - firstRow)));
        d.writeShadowCell(r, 0, mark);
    }
}

TEST(test_shiftShadowForViewOffset_positive_moves_content_up)
{
    /// Same convention as PaintSession::ackScroll: positive n shifts
    /// content up (row r gets row r+n's bytes). Band [3..9], n=2:
    /// row 3 ← row 5, row 4 ← row 6, … , row 7 ← row 9. Rows 8 and 9
    /// are the "newly exposed" bottom — shadowShiftRows leaves them
    /// untouched (the caller is responsible for repainting).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    /// Pre-state sanity.
    const Cell* p3 = d.readShadowCell(3, 0);
    CHECK(p3 && p3->codePoint == 'A');
    const Cell* p5 = d.readShadowCell(5, 0);
    CHECK(p5 && p5->codePoint == 'C');
    d.shiftShadowForViewOffset(3, 9, 2);
    // Row 3 now holds what row 5 had (C).
    p3 = d.readShadowCell(3, 0);
    CHECK(p3 != nullptr);
    if (p3)
        CHECK(p3->codePoint == 'C');
    /// Row 7 now holds what row 9 had (G).
    const Cell* p7 = d.readShadowCell(7, 0);
    CHECK(p7 != nullptr);
    if (p7)
        CHECK(p7->codePoint == 'G');
}

TEST(test_shiftShadowForViewOffset_negative_moves_content_down)
{
    /// n=-2 over [3..9]: row 9 ← row 7, row 8 ← row 6, …, row 5 ← row 3.
    /// Rows 3 and 4 are the newly-exposed top (left untouched).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    /// Pre-state.
    const Cell* p9 = d.readShadowCell(9, 0);
    CHECK(p9 && p9->codePoint == 'G');
    d.shiftShadowForViewOffset(3, 9, -2);
    // Row 9 now holds what row 7 had (E).
    p9 = d.readShadowCell(9, 0);
    CHECK(p9 != nullptr);
    if (p9)
        CHECK(p9->codePoint == 'E');
    /// Row 5 now holds what row 3 had (A).
    const Cell* p5 = d.readShadowCell(5, 0);
    CHECK(p5 != nullptr);
    if (p5)
        CHECK(p5->codePoint == 'A');
}

TEST(test_shiftShadowForViewOffset_zero_n_is_noop)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    /// Snapshot every marker, shift by 0, verify nothing moved.
    uint16_t before[7];
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        CHECK(p != nullptr);
        before[r - 3] = p ? p->codePoint : 0;
    }
    d.shiftShadowForViewOffset(3, 9, 0);
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        CHECK(p != nullptr);
        if (p)
            CHECK(p->codePoint == before[r - 3]);
    }
}

TEST(test_shiftShadowForViewOffset_shift_larger_than_band_is_noop)
{
    /// Band [3..9] is 7 rows; band-height is bot-top = 6. shadowShiftRows
    /// rejects shift > bot-top (line: `if (shift == 0 || shift > bot - top) return;`).
    /// shift=7 must be a no-op.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    uint16_t before[7];
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        before[r - 3] = p ? p->codePoint : 0;
    }
    d.shiftShadowForViewOffset(3, 9, 7);
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        CHECK(p != nullptr);
        if (p)
            CHECK(p->codePoint == before[r - 3]);
    }
    // Same check for negative-direction overshoot.
    d.shiftShadowForViewOffset(3, 9, -7);
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        CHECK(p != nullptr);
        if (p)
            CHECK(p->codePoint == before[r - 3]);
    }
}

TEST(test_shiftShadowForViewOffset_noop_when_shadow_invalid)
{
    /// Precondition: shadow valid. shadowShiftRows bails on
    /// !m_bbShadowValid, which means the cells we seeded via
    /// writeShadowCell (still in m_bbShadow bytes — the flag is the
    /// only gate) are not actually shifted. Verify by checking that
    /// the byte at row 3 / column 0 remained 'A' after a shift request
    /// issued while the flag was false. After re-marking valid, the
    /// shadow bytes still hold the unshifted seed (since the shift
    /// never ran).
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    d.invalidateShadow();
    // Shadow flag false; readShadowCell returns nullptr — but the
    // underlying bytes are still there.
    d.shiftShadowForViewOffset(3, 9, 2);
    d.markShadowValid();
    /// The seed at row 3 column 0 (originally 'A') should still be 'A'
    /// because the shift was rejected.
    const Cell* p = d.readShadowCell(3, 0);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->codePoint == 'A');
}

TEST(test_shiftShadowForViewOffset_invalid_band_is_noop)
{
    /// top > bot, or bot >= m_rows, must be rejected gracefully.
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    seedShadowMarkers(d, 3, 9);
    uint16_t before[7];
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        before[r - 3] = p ? p->codePoint : 0;
    }
    d.shiftShadowForViewOffset(9, 3, 1); // top > bot
    d.shiftShadowForViewOffset(-1, 5, 1); // top < 0
    d.shiftShadowForViewOffset(0, 24, 1); // bot == m_rows
    d.shiftShadowForViewOffset(0, 999, 1); // bot way past
    for (int r = 3; r <= 9; ++r) {
        const Cell* p = d.readShadowCell(r, 0);
        CHECK(p != nullptr);
        if (p)
            CHECK(p->codePoint == before[r - 3]);
    }
}

// ---------- damageGeneration (dual-buffer divergence guard) --------

/// A paint session that acks a run advanced the shadow baseline, so the
/// generation bumps exactly once; a paint with nothing dirty leaves it
/// unchanged (so it never spuriously invalidates a peer snapshot buffer).
TEST(test_damageGeneration_bumps_only_on_consumed_damage)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d); // settle any post-init damage
    const std::uint64_t g0 = d.currentDamageGeneration();

    // No-op paint (nothing dirty) must not bump.
    drainPaint(d);
    CHECK(d.currentDamageGeneration() == g0);

    // A cell write then paint acks a run -> exactly one bump.
    d.setCell(Row(2), Column(3), makeCharacter('X'));
    drainPaint(d);
    CHECK(d.currentDamageGeneration() == g0 + 1);

    // Another no-op paint -> still no bump.
    drainPaint(d);
    CHECK(d.currentDamageGeneration() == g0 + 1);

    // A second distinct write -> second bump.
    d.setCell(Row(4), Column(5), makeCharacter('Y'));
    drainPaint(d);
    CHECK(d.currentDamageGeneration() == g0 + 2);
}

/// The dual-buffer hazard in miniature: two independent drains over the same
/// CellGrid each consume damage, so the generation advances once PER drain —
/// the signal a second snapshot buffer uses to detect a drain it did not
/// perform moved the shared shadow (Rendering::captureCanReuse).
TEST(test_damageGeneration_advances_once_per_consuming_drain)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    drainPaint(d);
    const std::uint64_t g0 = d.currentDamageGeneration();

    d.setCell(Row(1), Column(1), makeCharacter('A'));
    drainPaint(d);
    d.setCell(Row(2), Column(2), makeCharacter('B'));
    drainPaint(d);
    CHECK(d.currentDamageGeneration() == g0 + 2);
}

/// Acking a scroll also advances the baseline (the shadow shifts), so a paint
/// that consumes a scroll bumps the generation too.
TEST(test_damageGeneration_bumps_on_scroll_ack)
{
    std::unique_ptr<CellGrid> _dp(new CellGrid());
    CellGrid& d = *_dp;
    d.init(24, 80);
    for (int r = 0; r < 5; ++r)
        d.setCell(Row(r), Column(0), makeCharacter(uint16_t('a' + r)));
    drainPaint(d);
    const std::uint64_t g0 = d.currentDamageGeneration();

    Cell blank = makeBlank();
    d.scrollRegion(Row(0), Row(4), 1, blank); // queue pending scroll + dirty band
    drainPaint(d); // acks the scroll and the exposed-row run
    CHECK(d.currentDamageGeneration() > g0);
}

UBIQUITTY_TEST_MAIN()
