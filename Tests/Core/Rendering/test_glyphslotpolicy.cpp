// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_glyphslotpolicy — pins the portable glyph-slot SIZING policy
// (Rendering::computeGlyphSlot): macOS sizes its slots with it and Linux feeds
// its FreeType bbox through the same rule. The half-excess fallback fold, the
// per-side cell / half-line clamps, and the slot dims (cell + 2*margin + 1 wide, ceil(line) +
// 2*margin tall) are the one contract every frontend resolves identically — the
// core's "decide the canvas" half of the no-clip overflow pass.

#include "Core/Rendering/GlyphSlotPolicy.h"

#include <cstdio>

using Ubiquitty::Rendering::computeGlyphSlot;
using Ubiquitty::Rendering::GlyphOverhangInputs;
using Ubiquitty::Rendering::GlyphSlot;

// A 10x19 cell (DejaVu Sans Mono 16px, the Linux probe geometry): advance 10,
// ascent 15, descent 4, line 19, primary bbox exactly the cell box (no overhang).
static GlyphOverhangInputs baseCell()
{
    GlyphOverhangInputs in;
    in.advanceWidth = 10.0;
    in.ascent = 15.0;
    in.descent = 4.0;
    in.lineHeight = 19.0;
    in.primaryBboxX = 0.0;
    in.primaryBboxY = -4.0; // bottom = descent
    in.primaryBboxWidth = 10.0; // right = advance
    in.primaryBboxHeight = 19.0; // top = ascent
    return in;
}

TEST(glyphslotpolicy)
{

    // No overhang at all -> zero margin, slot = cell box + 1px subpixel headroom.
    {
        const GlyphSlot s = computeGlyphSlot(baseCell());
        CHECK(s.glyphWidthInPixels == 10);
        CHECK(s.marginInPixels.width == 0);
        CHECK(s.marginInPixels.height == 0);
        CHECK(s.slotWidthInPixels == 11); // 10 + 2*0 + 1
        CHECK(s.slotHeightInPixels == 19); // ceil(19) + 2*0
    }

    // Nerd-Font fallback 1.6x wider (bbox 16 vs advance 10): the half-excess fold
    // gives a 3px margin each side -> 17px slot, baseline-aligned (no vertical grow).
    {
        GlyphOverhangInputs in = baseCell();
        in.hasFallback = true;
        in.fallbackBboxX = 0.0;
        in.fallbackBboxY = -4.0;
        in.fallbackBboxWidth = 16.0;
        in.fallbackBboxHeight = 19.0;
        const GlyphSlot s = computeGlyphSlot(in);
        CHECK(s.marginInPixels.width == 3); // ceil((16-10)*0.5)
        CHECK(s.marginInPixels.height == 0);
        CHECK(s.slotWidthInPixels == 17); // 10 + 2*3 + 1
        CHECK(s.slotHeightInPixels == 19);
    }

    // Horizontal clamp: a pathological 100px-wide fallback can't blow up the
    // atlas — the margin caps at one cell (glyphWidthInPixels), not 45.
    {
        GlyphOverhangInputs in = baseCell();
        in.hasFallback = true;
        in.fallbackBboxY = -4.0;
        in.fallbackBboxWidth = 100.0;
        in.fallbackBboxHeight = 19.0;
        const GlyphSlot s = computeGlyphSlot(in);
        CHECK(s.marginInPixels.width == 10); // clamped to glyphWidthInPixels
        CHECK(s.slotWidthInPixels == 31); // 10 + 2*10 + 1
    }

    // Vertical clamp: a tall fallback caps the vertical margin at half a line
    // each side (lineHeight*0.5, truncated to int), so the slot at most doubles.
    {
        GlyphOverhangInputs in = baseCell();
        in.hasFallback = true;
        in.fallbackBboxWidth = 10.0; // no horizontal grow
        in.fallbackBboxY = -20.0;
        in.fallbackBboxHeight = 60.0; // top 40, well past ascent 15
        const GlyphSlot s = computeGlyphSlot(in);
        CHECK(s.marginInPixels.height == 9); // (int)(19 * 0.5)
        CHECK(s.slotHeightInPixels == 37); // 19 + 2*9
    }

    // Primary face overhang (no fallback): ink past the advance grows the slot too.
    {
        GlyphOverhangInputs in = baseCell();
        in.primaryBboxX = -2.0; // 2px left overhang
        in.primaryBboxWidth = 14.0; // 2px right overhang past advance 10
        const GlyphSlot s = computeGlyphSlot(in);
        CHECK(s.marginInPixels.width == 2);
        CHECK(s.slotWidthInPixels == 15); // 10 + 2*2 + 1
    }

    // glyphWidthInPixels = ceil(advance), floored at 1.
    {
        GlyphOverhangInputs in = baseCell();
        in.advanceWidth = 10.3;
        CHECK(computeGlyphSlot(in).glyphWidthInPixels == 11);
        in.advanceWidth = 0.0;
        CHECK(computeGlyphSlot(in).glyphWidthInPixels == 1);
    }
}

UBIQUITTY_TEST_MAIN()
