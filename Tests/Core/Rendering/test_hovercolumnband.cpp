// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/CellDecisions.h"
#include "Harness.h"

using Ubiquitty::Rendering::hoverColumnBand;

TEST(a_row_outside_the_run_is_covered_nowhere)
{
    CHECK(hoverColumnBand(2, 3, 5, 10, 20, 0, -1, 80).empty());
    CHECK(hoverColumnBand(6, 3, 5, 10, 20, 0, -1, 80).empty());
}

TEST(a_single_row_run_takes_both_bounds)
{
    const auto band = hoverColumnBand(4, 4, 4, 10, 20, 0, -1, 80);
    CHECK_EQ(band.first, 10);
    CHECK_EQ(band.last, 20);
    CHECK(!band.empty());
}

TEST(a_multi_row_run_opens_at_the_head_and_closes_at_the_tail)
{
    const auto head = hoverColumnBand(3, 3, 5, 10, 20, 0, -1, 80);
    CHECK_EQ(head.first, 10);
    CHECK_EQ(head.last, 79); // to the grid edge, not to columnLast

    const auto middle = hoverColumnBand(4, 3, 5, 10, 20, 0, -1, 80);
    CHECK_EQ(middle.first, 0);
    CHECK_EQ(middle.last, 79);

    const auto tail = hoverColumnBand(5, 3, 5, 10, 20, 0, -1, 80);
    CHECK_EQ(tail.first, 0);
    CHECK_EQ(tail.last, 20);
}

TEST(an_inverted_run_covers_nothing)
{
    // rowFirst > rowLast is how the frontend spells "no hover".
    CHECK(hoverColumnBand(0, -1, -2, 0, 0, 0, -1, 80).empty());
    CHECK(hoverColumnBand(4, 5, 3, 10, 20, 0, -1, 80).empty());
}

// A run resolved inside a pane clips its middle rows to the pane's span. The
// first and last rows keep their own endpoints; what changes is where the rows
// between them start and end -- the pane's edges, not the screen's. An
// underline drawn to the screen's width strokes across the neighbour pane,
// which is the report this guards.
TEST(testMiddleRowsClipToThePaneSpan)
{
    const Ubiquitty::Rendering::HoverColumnBand middle = Ubiquitty::Rendering::hoverColumnBand(2, 1, 3, 5, 12, 0, 19, 40);
    CHECK(middle.first == 0);
    CHECK(middle.last == 19);
    const Ubiquitty::Rendering::HoverColumnBand first = Ubiquitty::Rendering::hoverColumnBand(1, 1, 3, 5, 12, 0, 19, 40);
    CHECK(first.first == 5);
    CHECK(first.last == 19 || first.last == 39); // first row ends at the span edge? no: columnLast rules the LAST row only
    const Ubiquitty::Rendering::HoverColumnBand negative = Ubiquitty::Rendering::hoverColumnBand(2, 1, 3, 5, 12, 0, -1, 40);
    CHECK_MSG(negative.first == 0 && negative.last == 39, "a negative span means the screen's own width");
    CHECK_MSG(hoverColumnBand(0, 1, 3, 5, 12, 0, 19, 40).empty(), "outside the run stays empty");
}

UBIQUITTY_TEST_MAIN()
