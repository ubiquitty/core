// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_subpixelphase — pins the portable horizontal subpixel-phase selection
// (Source/Core/Rendering/SubpixelPhase.h: phaseForPen) to the exact floor + lround
// carry the macOS Metal renderer uses, so every
// frontend snaps the glyph pen origin + picks the phase bucket identically and
// the phase bits fed into the shared glyph key match. Pure C++, links
// standalone. Also checks subpixelPhaseCount stays within the key's 3-bit
// phase field.

#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/SubpixelPhase.h"

#include <cstdio>

using Ubiquitty::Rendering::phaseForPen;
using Ubiquitty::Rendering::SubpixelPhase;
using Ubiquitty::Rendering::subpixelPhaseCount;

static bool eq(SubpixelPhase r, int snapped, int phase) { return r.snappedPenX == snapped && r.phase == phase; }

TEST(subpixelphaseCases)
{

    const int N = subpixelPhaseCount; // 8

    // phaseCount must fit the glyph key's 3-bit phase field (phase & 0x7).
    CHECK(subpixelPhaseCount <= 8);

    // --- exact pixel boundary: phase 0, no fraction ---
    CHECK(eq(phaseForPen(10.0f, N), 10, 0));
    CHECK(eq(phaseForPen(0.0f, N), 0, 0));

    // --- one phase bucket in (1/8 px) ---
    CHECK(eq(phaseForPen(10.125f, N), 10, 1)); // 0.125*8 = 1.0
    CHECK(eq(phaseForPen(10.25f, N), 10, 2)); // 0.25*8 = 2.0
    CHECK(eq(phaseForPen(10.5f, N), 10, 4)); // 0.5*8 = 4.0
    CHECK(eq(phaseForPen(10.875f, N), 10, 7)); // 0.875*8 = 7.0

    // --- nearest-rounding to a bucket ---
    CHECK(eq(phaseForPen(10.06f, N), 10, 0)); // 0.06*8 = 0.48 -> 0
    CHECK(eq(phaseForPen(10.07f, N), 10, 1)); // 0.07*8 = 0.56 -> 1
    CHECK(eq(phaseForPen(10.9f, N), 10, 7)); // 0.9*8 = 7.2 -> 7

    // --- carry: a fraction that rounds up to a whole pixel advances the
    //     pixel origin and wraps the phase to 0 ---
    CHECK(eq(phaseForPen(10.9375f, N), 11, 0)); // 0.9375*8 = 7.5 -> 8 -> carry
    CHECK(eq(phaseForPen(10.999f, N), 11, 0)); // ~7.99 -> 8 -> carry
    CHECK(eq(phaseForPen(0.99f, N), 1, 0)); // carry off a sub-pixel near 1

    // --- negative pen X: floor pulls the base down; phase stays in range ---
    {
        // -0.5: floor = -1, frac = 0.5, 0.5*8 = 4 -> {-1, 4}
        CHECK(eq(phaseForPen(-0.5f, N), -1, 4));
        // -0.0625: floor = -1, frac = 0.9375, *8 = 7.5 -> 8 -> carry -> {0, 0}
        CHECK(eq(phaseForPen(-0.0625f, N), 0, 0));
    }

    // --- a different phaseCount (e.g. 4) still buckets + carries correctly ---
    CHECK(eq(phaseForPen(5.25f, 4), 5, 1)); // 0.25*4 = 1
    CHECK(eq(phaseForPen(5.5f, 4), 5, 2)); // 0.5*4 = 2
    CHECK(eq(phaseForPen(5.875f, 4), 6, 0)); // 0.875*4 = 3.5 -> 4 -> carry
}

// The Floor rounding mirrors a rasteriser that quantises pens itself: the
// phase is the bucket the pen lies in, never the nearest one, so it never
// carries and a pen just under a bucket edge stays in the bucket below —
// which is where CoreText would draw it, and where the nearest rule puts
// some columns one bucket to the right.
TEST(floorRoundingStaysInTheBucket)
{
    using Ubiquitty::Rendering::PhaseRounding;
    CHECK(eq(phaseForPen(10.0f, 2, PhaseRounding::Floor), 10, 0));
    CHECK(eq(phaseForPen(10.49f, 2, PhaseRounding::Floor), 10, 0)); // nearest: 1
    CHECK(eq(phaseForPen(10.5f, 2, PhaseRounding::Floor), 10, 1));
    CHECK(eq(phaseForPen(10.99f, 2, PhaseRounding::Floor), 10, 1)); // nearest: carry to 11
    CHECK(eq(phaseForPen(7.66f, 3, PhaseRounding::Floor), 7, 1)); // 0.66*3 = 1.98 -> 1
    CHECK(eq(phaseForPen(7.67f, 3, PhaseRounding::Floor), 7, 2));
    CHECK(eq(phaseForPen(3.999f, 1, PhaseRounding::Floor), 3, 0)); // whole-pixel grid
    // Negative pens floor the base and keep the phase in range.
    CHECK(eq(phaseForPen(-0.25f, 4, PhaseRounding::Floor), -1, 3));
    // The default is still the portable nearest rule.
    CHECK(eq(phaseForPen(10.49f, 2), 10, 1));
}

// CoreText's pen grid by device pixel size, measured by counting distinct
// masks over 1/128-pixel steps and bisecting the crossings: fifths to
// 8.33px, quarters to 11.11px, thirds to 16.67px, halves to 33.33px, whole
// pixels above — i.e. the grid changes where size crosses 100 / (3 n).
TEST(nativePhaseCountFollowsCoreTextsGrid)
{
    using Ubiquitty::Rendering::nativeSubpixelPhaseCount;
    CHECK(nativeSubpixelPhaseCount(6.0) == 5);
    CHECK(nativeSubpixelPhaseCount(8.0) == 5);
    CHECK(nativeSubpixelPhaseCount(8.3) == 5);
    CHECK(nativeSubpixelPhaseCount(8.4) == 4);
    CHECK(nativeSubpixelPhaseCount(11.0) == 4);
    CHECK(nativeSubpixelPhaseCount(11.2) == 3);
    CHECK(nativeSubpixelPhaseCount(12.0) == 3); // 12pt at 1x
    CHECK(nativeSubpixelPhaseCount(16.0) == 3);
    CHECK(nativeSubpixelPhaseCount(16.6) == 3);
    CHECK(nativeSubpixelPhaseCount(16.7) == 2);
    CHECK(nativeSubpixelPhaseCount(24.0) == 2); // 12pt at 2x
    CHECK(nativeSubpixelPhaseCount(26.0) == 2); // 13pt at 2x
    CHECK(nativeSubpixelPhaseCount(33.0) == 2);
    CHECK(nativeSubpixelPhaseCount(33.4) == 1);
    CHECK(nativeSubpixelPhaseCount(36.0) == 1);
    CHECK(nativeSubpixelPhaseCount(72.0) == 1);
    // Every grid fits the key's 3-bit phase field and the portable count.
    for (double px = 1.0; px <= 200.0; px += 0.5)
        CHECK(nativeSubpixelPhaseCount(px) >= 1 && nativeSubpixelPhaseCount(px) <= subpixelPhaseCount);
    // A degenerate size degrades to one phase rather than to a huge count.
    CHECK(nativeSubpixelPhaseCount(0.0) == 1);
    CHECK(nativeSubpixelPhaseCount(-3.0) == 1);
}

UBIQUITTY_TEST_MAIN()
