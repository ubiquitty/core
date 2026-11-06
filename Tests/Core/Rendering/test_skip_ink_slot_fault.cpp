// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_skip_ink_slot_fault — a slot faulted is a slot filled.
//
// `GlyphSlotCache::slotForKey` allocates on a miss and reports `fresh`, and the
// contract is that the caller then rasterises into what it took. The cache
// cannot enforce it: a slot allocated and abandoned is indistinguishable from
// one holding a glyph with no coverage, which is what a space looks like.
//
// The skip-ink mask taps the NEIGHBOURING cells' coverage, so it reaches a
// column the walk has not arrived at yet. A tap that faults without filling
// therefore does not merely waste a slot — it consumes the fault that cell was
// going to use. Its own fetch finds the key resident, is told it is not fresh,
// and never rasterises. The cell draws blank, on a path where blank is legal.
//
// So the assertion is on the RATIO of rasterise calls to distinct keys, not on
// pixels: every distinct glyph must be drawn exactly once, whoever faulted it.

#include "Harness.h"
#include "StrictAtlas.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/TermPalette.h"

#include <set>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

constexpr int rows = 1;
constexpr int columns = 12;
constexpr int atlasColumns = 16;
constexpr int atlasCapacity = atlasColumns * 16;

// Counts what it was asked to draw, and remembers which slots it filled, so the
// test can ask both "was every fault honoured" and "did any cell end up reading
// a slot nobody drew into".
struct CountingAtlas : UbiquittyTest::StrictAtlas {
    GlyphSlotCache cache { atlasCapacity, recommendedHashBuckets(atlasCapacity), atlasColumns };
    GlyphSlotGeometry geometry = [] {
        GlyphSlotGeometry g {};
        g.columns = atlasColumns;
        g.slotWidthInPixels = 8;
        g.slotHeightInPixels = 16;
        g.glyphWidthInPixels = 8;
        return g;
    }();
    int rasteriseCalls = 0;
    std::set<int> filledSlots;

    void beginFrame() { cache.beginFrame(); }
    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &cache;
        binding.geometry = geometry;
        binding.slotCapacity = atlasCapacity;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int slot)
    {
        ++rasteriseCalls;
        filledSlots.insert(slot);
        return true;
    }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }
};

// Whether the frame's decorations cut around glyph ink, which is the mask that
// taps the neighbouring columns.
enum class SkipInk {
    Off,
    On,
};

// One row of DISTINCT underlined characters. Distinct matters: a row of one
// repeated character shares a single key, so every cell after the first is a
// legitimate cache hit and the defect hides completely.
struct Scene {
    Ubiquitty::TermPalette palette;
    std::vector<Ubiquitty::Cell> cells { static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    BuildInputs in;
    InstanceBuffers out;
    CountingAtlas atlas;

    explicit Scene(SkipInk skipInk)
    {
        palette.seedDefaults();
        for (int c = 0; c < columns; ++c) {
            Ubiquitty::Cell& cell = cells[static_cast<std::size_t>(c)];
            cell.codePoint = static_cast<std::uint32_t>('a' + c);
            Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
            pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
            Ubiquitty::cellSetPen(cell, pen);
        }
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;
        in.metrics.underlineDrop = 1.0f;
        in.metrics.skipInk = skipInk == SkipInk::On;
        in.metrics.skipInkPadding = 1.0f;
        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
        out.decorations = decorationsOut.data();
        out.decorationCapacity = decorationsOut.size();
    }

    void build() { buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }
};

} // namespace

TEST(skipInkFaultsAreFilled)
{
    // --- The control: skip-ink off draws each distinct glyph once. ---------
    //
    // Establishes what "all of them" is, from the same builder, so the case
    // below is a comparison rather than a hardcoded count that rots when the
    // scene changes.
    Scene control(SkipInk::Off);
    control.build();
    CHECK_MSG(control.atlas.rasteriseCalls == columns,
        "without skip-ink every distinct character is rasterised exactly once");

    // --- Skip-ink must not change how many glyphs get drawn. ---------------
    //
    // The mask reads neighbouring coverage; it does not decide which glyphs
    // exist. A tap that faults a slot and leaves it empty shows up here as a
    // DEFICIT, because the cell it stole from never faults again.
    Scene masked(SkipInk::On);
    masked.build();
    CHECK_MSG(masked.atlas.rasteriseCalls == control.atlas.rasteriseCalls,
        "skip-ink taps must fill every slot they fault, not consume a cell's only fault");

    // --- Every slot the cells reference was actually drawn into. -----------
    //
    // The count above can be satisfied while a tap fills slot A and the cell
    // ends up reading slot B. This asserts the cells and the rasteriser agree
    // on which slots carry ink.
    bool hasUnfilledSlot = false;
    for (int c = 0; c < columns; ++c) {
        bool isFresh = false;
        const std::uint32_t key = monoGlyphKey(masked.cells[static_cast<std::size_t>(c)].codePoint, 0u, FontFace {}, 0u);
        const int slot = masked.atlas.cache.slotForKey(key, &isFresh);
        if (slot >= 0 && masked.atlas.filledSlots.count(slot) == 0)
            hasUnfilledSlot = true;
    }
    CHECK_MSG(!hasUnfilledSlot, "no cell resolves to a slot nothing rasterised into");
}

UBIQUITTY_TEST_MAIN()
