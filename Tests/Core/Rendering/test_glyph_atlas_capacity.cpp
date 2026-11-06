// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_glyph_atlas_capacity — pins how many slots a glyph atlas asks for.
//
// The failure this bounds is a missing character, not a slow frame. A glyph the
// atlas has no slot for draws as bare background, so a screenful of CJK prose
// or a mixed-script editor renders holes where text should be — and the two
// backends disagree, because the CPU renderer has no atlas to run out of.
//
// The Vulkan and Metal atlases both size from their screen by asking here, so
// the rule is one rule and a screen too big for it is a fact about the device
// clamp rather than about which backend is drawing.
//
// What is checked is the SHAPE of the request rather than particular numbers:
// that it grows with the screen, that it never falls under the floor, and that
// the doubling is really there — an atlas cut for exactly one screen has
// nothing evictable while a frame's slots are pinned, which is the state that
// draws blanks.

#include "Harness.h"

#include "Core/Rendering/CellDecisions.h"

using Ubiquitty::Rendering::atlasDirtyRowBand;
using Ubiquitty::Rendering::glyphAtlasNominalCapacity;

TEST(glyphAtlasNominalCapacityShape)
{
    constexpr int columns = 32;

    // --- A screen bigger than the floor is what decides, and it is doubled.
    //
    // 200x50 is a large-but-ordinary window: 10000 cells, so 10000 distinct
    // glyphs is reachable on one screen, about ten times a 1024 floor.
    {
        const int cells = 200 * 50;
        CHECK(glyphAtlasNominalCapacity(cells, columns) == 2 * cells);
        CHECK(glyphAtlasNominalCapacity(cells, columns) > 1024);
    }

    // --- It grows with the screen rather than sitting at a constant.
    {
        const int small = glyphAtlasNominalCapacity(80 * 24, columns);
        const int large = glyphAtlasNominalCapacity(400 * 100, columns);
        CHECK(large > small);
    }

    // --- Never under the floor, whatever the screen says.
    //
    // A few rows still cycle through more glyphs than they show at once as the
    // user scrolls, and a caller with no measurement at all gets the floor
    // rather than nothing.
    {
        CHECK(glyphAtlasNominalCapacity(0, columns) == columns * 128);
        CHECK(glyphAtlasNominalCapacity(1, columns) == columns * 128);
        CHECK(glyphAtlasNominalCapacity(80 * 4, columns) == columns * 128);
        // A negative is a caller that has not measured, not a screen.
        CHECK(glyphAtlasNominalCapacity(-1, columns) == columns * 128);
    }

    // --- The floor tracks the column count it is expressed in.
    {
        CHECK(glyphAtlasNominalCapacity(0, 64) > glyphAtlasNominalCapacity(0, 32));
        // A column count of zero would divide the floor to nothing; the request
        // stays positive so a grid can always be built from it.
        CHECK(glyphAtlasNominalCapacity(0, 0) > 0);
    }

    // --- Every request is positive, so the row count derived from it is too.
    {
        for (const int cells : { 0, 1, 100, 80 * 24, 200 * 50, 400 * 100 })
            CHECK(glyphAtlasNominalCapacity(cells, columns) > 0);
    }
}

TEST(atlasDirtyRowBandCoversItsSlots)
{
    // The rows a slot range sits in, which is what an atlas upload copies. A
    // band that is too small leaves a faulted glyph showing the previous
    // occupant of its slot; one that is too large is only wasted bytes. So the
    // property checked is COVERAGE first: every slot in the range must lie
    // inside the band, at every grid shape.
    constexpr int columns = 32;
    constexpr int slotHeight = 16;
    constexpr int atlasHeight = 16 * 40; // forty slot rows

    // --- One slot is one band.
    {
        const auto band = atlasDirtyRowBand(0, 0, columns, slotHeight, atlasHeight);
        CHECK(band.firstRow == 0);
        CHECK(band.rowCount == slotHeight);
    }

    // --- A slot in the third row names the third row.
    {
        const auto band = atlasDirtyRowBand(columns * 2, columns * 2, columns, slotHeight, atlasHeight);
        CHECK(band.firstRow == slotHeight * 2);
        CHECK(band.rowCount == slotHeight);
    }

    // --- The last slot of a row and the first of the next are two bands.
    {
        const auto band = atlasDirtyRowBand(columns - 1, columns, columns, slotHeight, atlasHeight);
        CHECK(band.firstRow == 0);
        CHECK(band.rowCount == slotHeight * 2);
    }

    // --- Coverage, exhaustively over the grid.
    //
    // For every range the band must contain every slot's rows. A formula that
    // is off by a row passes a hand-picked case and fails here.
    {
        for (int firstSlot = 0; firstSlot < columns * 5; firstSlot += 7) {
            for (int lastSlot = firstSlot; lastSlot < columns * 5; lastSlot += 5) {
                const auto band = atlasDirtyRowBand(firstSlot, lastSlot, columns, slotHeight, atlasHeight);
                const int firstSlotTop = (firstSlot / columns) * slotHeight;
                const int lastSlotBottom = ((lastSlot / columns) + 1) * slotHeight;
                CHECK(band.firstRow <= firstSlotTop);
                CHECK(band.firstRow + band.rowCount >= lastSlotBottom);
                CHECK(band.firstRow >= 0);
                CHECK(band.firstRow + band.rowCount <= atlasHeight);
            }
        }
    }

    // --- An empty range copies nothing.
    //
    // Zero rows, not the whole sheet: this is the steady state of a frame that
    // faulted no glyph, and answering it with the sheet would undo the change.
    {
        const auto band = atlasDirtyRowBand(5, 4, columns, slotHeight, atlasHeight);
        CHECK(band.rowCount == 0);
    }

    // --- A band never runs past the sheet.
    //
    // A clamped grid holds fewer slots than a caller may have marked, and a
    // range reaching past it would name rows the image does not have.
    {
        const auto band = atlasDirtyRowBand(0, columns * 100, columns, slotHeight, atlasHeight);
        CHECK(band.firstRow == 0);
        CHECK(band.rowCount == atlasHeight);
    }
    {
        const auto band = atlasDirtyRowBand(columns * 100, columns * 100, columns, slotHeight, atlasHeight);
        CHECK(band.rowCount == 0); // starts past the sheet: no rows of it are named
    }

    // --- Degenerate geometry names nothing rather than dividing by it.
    {
        CHECK(atlasDirtyRowBand(0, 1, 0, slotHeight, atlasHeight).rowCount == 0);
        CHECK(atlasDirtyRowBand(0, 1, columns, 0, atlasHeight).rowCount == 0);
        CHECK(atlasDirtyRowBand(0, 1, columns, slotHeight, 0).rowCount == 0);
        CHECK(atlasDirtyRowBand(-1, 1, columns, slotHeight, atlasHeight).rowCount == 0);
    }
}

UBIQUITTY_TEST_MAIN()

// The grid a coverage sheet is cut to for its screen: the nominal capacity at
// the sheet's column count, a wide sheet asking for half the slots a mono one
// does, and never more slots than the cache can index.
TEST(glyphSheetGridForScreenFollowsTheNominalCapacity)
{
    using Ubiquitty::Rendering::glyphSheetGridForScreen;
    using Ubiquitty::Rendering::maxGlyphSheetSlots;
    constexpr int limit = 16384;

    // --- A mono sheet holds twice the screen, at the columns asked for.
    {
        const auto fit = glyphSheetGridForScreen(200 * 50, 1, 32, { 10, 20 }, limit);
        CHECK(fit.valid);
        CHECK(fit.columns * fit.rows >= glyphAtlasNominalCapacity(200 * 50, 32));
        CHECK(fit.columns * fit.rows < glyphAtlasNominalCapacity(200 * 50, 32) + fit.columns);
    }

    // --- A slot two cells wide serves half the screen's cells.
    {
        const auto mono = glyphSheetGridForScreen(200 * 50, 1, 16, { 20, 20 }, limit);
        const auto wide = glyphSheetGridForScreen(200 * 50, 2, 16, { 20, 20 }, limit);
        CHECK(wide.columns * wide.rows * 2 <= mono.columns * mono.rows + 2 * wide.columns);
        CHECK(wide.columns * wide.rows >= 200 * 50);
    }

    // --- A screen past the index range is cut to it, never past it.
    {
        CHECK(glyphAtlasNominalCapacity(400 * 200, 32) == maxGlyphSheetSlots);
        const auto fit = glyphSheetGridForScreen(400 * 200, 1, 32, { 4, 4 }, limit);
        CHECK(fit.valid);
        CHECK(fit.columns * fit.rows <= maxGlyphSheetSlots);
    }

    // --- The texture limit still decides when it is the tighter bound.
    {
        const auto fit = glyphSheetGridForScreen(400 * 100, 1, 32, { 40, 80 }, 4096);
        CHECK(fit.valid);
        CHECK(fit.columns * 40 <= 4096);
        CHECK(fit.rows * 80 <= 4096);
    }
}
