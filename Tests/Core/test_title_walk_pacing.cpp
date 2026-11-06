// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// What makes the walk behind a title run. Where the kernel raises fork/exec/exit
// edges the edge is the whole signal; where it raises nothing, Core paces the
// walk itself. These pin the constants both arms of that depend on.

#include "Core/Title/WalkPacing.h"

#include <cstdio>

using namespace Ubiquitty::Title;

// The edge pacing is a DEFER, never a drop, and the three constants have to
// stand in that order for it to be one: a floor shorter than the settle would
// answer a tree still moving, and a cap shorter than the settle would answer
// every storm at the cap and never at rest.
TEST(testTheEdgePacingDefersRatherThanDrops)
{
    CHECK(edgeFloorMilliseconds > 0);
    CHECK_MSG(edgeSettleMilliseconds >= edgeFloorMilliseconds,
        "a settle shorter than the floor would answer a tree that is still moving");
    CHECK_MSG(edgeMaximumDeferMilliseconds > edgeSettleMilliseconds,
        "a cap at or below the settle would answer every storm at the cap and a quiet tree never");
    // The cap bounds a permanently churning tree to a walk rate, and that rate
    // has to be one a machine can pay: two a second, not ten.
    CHECK(1000 / edgeMaximumDeferMilliseconds <= 2);
}

// The fallback exists for a platform whose kernel announces nothing, and it is
// slow because it backstops a fact that changes rarely rather than sampling
// one that moves. Walking the process table is the expensive half of a title,
// and paying for it four times a second is most of what an idle terminal costs.
TEST(testTheFallbackWalkIsSlowEnoughToBeABackstop)
{
    CHECK_MSG(walkFallbackMilliseconds > 0, "a platform with no edges needs some signal");
    CHECK_MSG(walkFallbackMilliseconds >= 1000,
        "a fallback faster than a second is a sampling rate, and the walk reads the machine's process table");
    // Slower than the edge pacing by a wide margin: an edge-driven walk answers
    // a real change, and this one only asks whether there was one.
    CHECK(walkFallbackMilliseconds > edgeMaximumDeferMilliseconds);
}

UBIQUITTY_TEST_MAIN()
