// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Diagnostics/PerfCounters.cpp — the portable
// counter model behind the paint-frequency heatmap.
//
// Pins:
//   - noteCellPaint increments by PaintBump and saturates at 255.
//   - out-of-range row/column is a no-op (and snapshot of it reads 0).
//   - decay applies v*9/10 across boundary values.
//   - decay only walks the active grid; zeroOutsideGrid clears the
//     rest.
//   - colorFor(0) is near-black, colorFor(255) is near-white, and the
//     hue marches blue -> ... -> red across the range.

#include "Core/Diagnostics/PerfCounters.h"
#include <cstdio>

namespace PC = Ubiquitty::Diagnostics::PerfCounters;

static uint32_t redOf(uint32_t c) { return (c >> 16) & 0xFF; }
static uint32_t greenOf(uint32_t c) { return (c >> 8) & 0xFF; }
static uint32_t blueOf(uint32_t c) { return c & 0xFF; }

TEST(testNoteAndSaturate)
{
    PC::reset();
    CHECK(PC::snapshotCell(0, 0) == 0);
    PC::noteCellPaint(0, 0);
    CHECK(PC::snapshotCell(0, 0) == PC::PaintBump);
    // Enough bumps to overflow a byte — must clamp at 255, never wrap.
    for (int i = 0; i < 64; i++)
        PC::noteCellPaint(0, 0);
    CHECK(PC::snapshotCell(0, 0) == 255);
}

TEST(testOutOfRangeNoop)
{
    PC::reset();
    PC::noteCellPaint(-1, 0);
    PC::noteCellPaint(0, -1);
    // Out-of-grid paints are COUNTED, not dropped: a heatmap that cannot tell
    // "never painted" from "never measured" reports the blind region as cheap.
    PC::reset();
    CHECK(PC::unmeasuredPaints() == 0);
    PC::noteCellPaint(PC::MaxRows, 0);
    CHECK(PC::unmeasuredPaints() == 1);
    PC::noteCellPaint(0, PC::MaxColumns);
    CHECK(PC::unmeasuredPaints() == 2);
    PC::noteCellPaint(0, 0);
    CHECK_MSG(PC::unmeasuredPaints() == 2, "an in-grid paint must not count as unmeasured");
    PC::reset();
    CHECK_MSG(PC::unmeasuredPaints() == 0, "reset clears the unmeasured tally with the counters");

    PC::noteCellPaint(PC::MaxRows, 0);
    PC::noteCellPaint(0, PC::MaxColumns);
    // None of the above touched (0,0).
    CHECK(PC::snapshotCell(0, 0) == 0);
    // Out-of-range reads return 0 rather than indexing out of bounds.
    CHECK(PC::snapshotCell(-1, 0) == 0);
    CHECK(PC::snapshotCell(0, PC::MaxColumns) == 0);
}

TEST(testDecayBoundaries)
{
    PC::reset();
    // Drive (0,0) to 255 and (0,1) to 1 by hand-stepping through the
    // counter via repeated paints (PaintBump==8 so 255 saturates and
    // a single paint lands at 8 — decay both and check the formula).
    for (int i = 0; i < 64; i++)
        PC::noteCellPaint(0, 0);
    CHECK(PC::snapshotCell(0, 0) == 255);
    PC::decay(1, 2);
    // 255 * 9 / 10 == 229.
    CHECK(PC::snapshotCell(0, 0) == 229);

    // A low counter decays to 0 one tick at a time: 8 -> 7 -> 6 -> ...
    PC::reset();
    PC::noteCellPaint(0, 0); // == 8
    uint8_t prev = PC::snapshotCell(0, 0);
    for (int i = 0; i < 40; i++) {
        PC::decay(1, 1);
        uint8_t now = PC::snapshotCell(0, 0);
        CHECK(now <= prev);
        prev = now;
    }
    CHECK(PC::snapshotCell(0, 0) == 0);
}

TEST(testDecayOnlyActiveGrid)
{
    PC::reset();
    // A cell outside the decayed extent keeps its value.
    PC::noteCellPaint(5, 5);
    CHECK(PC::snapshotCell(5, 5) == PC::PaintBump);
    PC::decay(1, 1); // only (0,0) region
    CHECK(PC::snapshotCell(5, 5) == PC::PaintBump);
    // zeroOutsideGrid clears it.
    PC::zeroOutsideGrid(1, 1);
    CHECK(PC::snapshotCell(5, 5) == 0);
}

TEST(testColorForBoundaries)
{
    uint32_t cold = PC::colorFor(0);
    // near-black: all channels low and equal.
    CHECK(redOf(cold) < 0x40);
    CHECK(greenOf(cold) < 0x40);
    CHECK(blueOf(cold) < 0x40);
    CHECK(redOf(cold) == greenOf(cold));
    CHECK(greenOf(cold) == blueOf(cold));

    /// cool end (just above zero) leans blue.
    uint32_t cool = PC::colorFor(1);
    CHECK(blueOf(cool) > redOf(cool));

    /// hot end is near-white: every channel high.
    uint32_t hot = PC::colorFor(255);
    CHECK(redOf(hot) > 0xC0);
    CHECK(greenOf(hot) > 0xC0);
    CHECK(blueOf(hot) > 0xC0);

    /// the red band (128..191) is red-dominant.
    uint32_t mid = PC::colorFor(150);
    CHECK(redOf(mid) > greenOf(mid));
    CHECK(redOf(mid) > blueOf(mid));
}

UBIQUITTY_TEST_MAIN()
