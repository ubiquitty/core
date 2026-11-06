// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Pins scrollExposedRows — the newly-blank band a scroll leaves within its
// region. The macOS snapshot capture marks this band dirty so the scroll blit's
// cleared exposed rows always repaint, even when the incremental dirty runs drop
// them (scroll coalescing / re-encode) — the top/top-left "missing lines" bug.

#include "Core/Screen/PendingScroll.h"

#include <cstdio>

TEST(pendingScroll)
{

    using Ubiquitty::Screen::scrollExposedRows;

    // No shift -> empty band (nothing exposed).
    CHECK(scrollExposedRows(0, 9, 0).isEmpty());
    CHECK(scrollExposedRows(5, 5, 0).isEmpty());

    // Content up (shift > 0) exposes blanks at the BOTTOM of the region.
    {
        auto e = scrollExposedRows(0, 9, 3);
        CHECK(e.firstRow == 7 && e.lastRow == 9);
    }
    {
        auto e = scrollExposedRows(0, 9, 1);
        CHECK(e.firstRow == 9 && e.lastRow == 9);
    }
    {
        auto e = scrollExposedRows(2, 7, 2);
        CHECK(e.firstRow == 6 && e.lastRow == 7);
    }

    // Content DOWN (shift < 0) exposes blanks at the TOP — the reported locality.
    {
        auto e = scrollExposedRows(0, 9, -3);
        CHECK(e.firstRow == 0 && e.lastRow == 2);
    }
    {
        auto e = scrollExposedRows(0, 9, -1);
        CHECK(e.firstRow == 0 && e.lastRow == 0);
    }
    {
        auto e = scrollExposedRows(2, 7, -2);
        CHECK(e.firstRow == 2 && e.lastRow == 3);
    }

    // |shift| at or beyond the region height exposes the WHOLE region.
    {
        auto e = scrollExposedRows(0, 9, 10);
        CHECK(e.firstRow == 0 && e.lastRow == 9);
    }
    {
        auto e = scrollExposedRows(0, 9, -10);
        CHECK(e.firstRow == 0 && e.lastRow == 9);
    }
    {
        auto e = scrollExposedRows(0, 9, 100);
        CHECK(e.firstRow == 0 && e.lastRow == 9);
    }
    {
        auto e = scrollExposedRows(5, 5, 1);
        CHECK(e.firstRow == 5 && e.lastRow == 5);
    }
    {
        auto e = scrollExposedRows(5, 5, -1);
        CHECK(e.firstRow == 5 && e.lastRow == 5);
    }

    // A sub-region band stays within [topRow, botRow], both directions.
    {
        auto e = scrollExposedRows(3, 8, -4);
        CHECK(e.firstRow == 3 && e.lastRow == 6);
    }
    {
        auto e = scrollExposedRows(3, 8, 4);
        CHECK(e.firstRow == 5 && e.lastRow == 8);
    }

    // Degenerate region (topRow > botRow) -> empty, never a stray range.
    CHECK(scrollExposedRows(5, 2, -1).isEmpty());
}

UBIQUITTY_TEST_MAIN()
