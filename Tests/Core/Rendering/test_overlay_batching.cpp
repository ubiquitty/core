// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_overlay_batching — which overlay quads may be drawn without ordering
// between them.
//
// A backend that blends by reading the attachment back needs a barrier between
// two quads only when one can read what the other wrote. Getting that wrong in
// the permissive direction is a race: the cursor blends against whatever the
// selection quad under it had written at the moment it was sampled, which is a
// flicker that depends on the driver and reproduces on one machine in ten.
//
// So the property under test is one-directional. Every case that matters asks
// whether quads that meet are kept apart — a run that is too short only
// costs a barrier, and a run that is too long costs correctness. The cases are
// written to fail if the rule ever becomes more permissive, including at the
// boundary, where two rects share an edge and floating-point rounding decides
// whether they share a pixel.

#include "Harness.h"

#include "Core/Rendering/OverlayBatching.h"

#include <vector>

using Ubiquitty::Rendering::disjointOverlayRun;
using Ubiquitty::Rendering::OverlayInstance;
using Ubiquitty::Rendering::overlayQuadsMeet;

namespace {

OverlayInstance quadAt(float left, float top, float right, float bottom)
{
    OverlayInstance quad {};
    quad.topLeftInPixels = { left, top };
    quad.bottomRightInPixels = { right, bottom };
    return quad;
}

// One span per row, the shape a dragged selection emits.
std::vector<OverlayInstance> selectionRows(int rowCount, float rowHeight)
{
    std::vector<OverlayInstance> quads;
    for (int row = 0; row < rowCount; ++row)
        quads.push_back(quadAt(0.0f, row * rowHeight, 640.0f, (row + 1) * rowHeight - 1.0f));
    return quads;
}

} // namespace

TEST(overlayBatchingKeepsMeetingQuadsApart)
{
    // --- A selection is one run.
    //
    // This is the case the batching exists for: sixty rows drawn as sixty
    // draws with fifty-nine barriers between them, none of which orders
    // anything, because no two rows share a pixel.
    {
        const std::vector<OverlayInstance> rows = selectionRows(60, 16.0f);
        CHECK(disjointOverlayRun(rows, 0) == rows.size());
    }

    // --- A quad that lands on an earlier one ends the run before itself.
    //
    // The cursor, drawn last over the selection. The run must stop at it so
    // the barrier falls between the selection and the cursor.
    {
        std::vector<OverlayInstance> quads = selectionRows(4, 16.0f);
        quads.push_back(quadAt(100.0f, 20.0f, 108.0f, 34.0f)); // over rows 1-2
        CHECK(disjointOverlayRun(quads, 0) == 4);
        // And the cursor alone is the next run.
        CHECK(disjointOverlayRun(quads, 4) == 1);
    }

    // --- It is checked against every quad in the run, not just the last.
    //
    // A quad can clear its immediate predecessor and still land on one from
    // earlier in the run. Checking only the previous quad passes the
    // selection case and races here.
    {
        std::vector<OverlayInstance> quads;
        quads.push_back(quadAt(0.0f, 0.0f, 100.0f, 10.0f));
        quads.push_back(quadAt(0.0f, 200.0f, 100.0f, 210.0f)); // far from the first
        quads.push_back(quadAt(50.0f, 5.0f, 60.0f, 8.0f)); // inside the FIRST
        CHECK(disjointOverlayRun(quads, 0) == 2);
    }

    // --- Touching counts as meeting.
    //
    // Two rects sharing an edge. The top-left rule says they do not share a
    // pixel, but these are floats that arrived through a rounding, and the
    // cost of being wrong here is a race while the cost of being careful is
    // one barrier.
    {
        const std::vector<OverlayInstance> abutting {
            quadAt(0.0f, 0.0f, 100.0f, 16.0f),
            quadAt(0.0f, 16.0f, 100.0f, 32.0f),
        };
        CHECK(overlayQuadsMeet(abutting[0], abutting[1]));
        CHECK(disjointOverlayRun(abutting, 0) == 1);
    }

    // --- Meeting is symmetric, which is what makes the scan order-free.
    {
        const OverlayInstance left = quadAt(0.0f, 0.0f, 50.0f, 50.0f);
        const OverlayInstance right = quadAt(25.0f, 25.0f, 75.0f, 75.0f);
        CHECK(overlayQuadsMeet(left, right));
        CHECK(overlayQuadsMeet(right, left));

        const OverlayInstance apart = quadAt(200.0f, 200.0f, 250.0f, 250.0f);
        CHECK(!overlayQuadsMeet(left, apart));
        CHECK(!overlayQuadsMeet(apart, left));
    }

    // --- Every quad is covered exactly once by successive runs.
    //
    // A run of zero would loop a caller forever, and a run reaching past the
    // end would draw instances that are not there.
    {
        std::vector<OverlayInstance> quads = selectionRows(6, 16.0f);
        quads.push_back(quadAt(0.0f, 0.0f, 640.0f, 96.0f)); // covers them all
        quads.push_back(quadAt(0.0f, 0.0f, 10.0f, 10.0f));

        std::size_t index = 0;
        std::size_t runs = 0;
        while (index < quads.size()) {
            const std::size_t run = disjointOverlayRun(quads, index);
            CHECK(run > 0);
            CHECK(index + run <= quads.size());
            index += run;
            ++runs;
        }
        CHECK(index == quads.size());
        CHECK(runs > 1); // the covering quad forces a boundary
    }

    // --- Past the end is an empty run, not a step backwards.
    {
        const std::vector<OverlayInstance> rows = selectionRows(3, 16.0f);
        CHECK(disjointOverlayRun(rows, rows.size()) == 0);
        CHECK(disjointOverlayRun({}, 0) == 0);
    }
}

UBIQUITTY_TEST_MAIN()
