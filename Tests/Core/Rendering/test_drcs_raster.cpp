// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_drcs_raster — pins the DECDLD / DRCS soft-font coverage path
// (Source/Core/Rendering/DRCSRaster.h) that turns a downloaded glyph matrix into
// the atlas coverage the macOS renderer blits for a cell carrying
// drcsCodePointBase + index. The point of the file is that a DRCS cell INKS: a
// tofu cell is coverage-empty, so every positive case here asserts ink at the
// bitmap's coordinates and blank everywhere else. Also pins the three
// no-glyph cases (undefined position, erased font, degenerate matrix) to a
// refusal the caller can turn into a background-only cell, and the DRCS glyph
// key's disjointness from the other keyspaces sharing the mono atlas. The last
// case links the live Core to run a real DECDLD download through the parser and
// rasterize the sentinel the printed cell carries — the sentinel arithmetic and
// the glyph store's indexing have to agree, and neither half proves that alone.

#include "Core/CellGrid.h"
#include "Core/Rendering/DRCSRaster.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>
#include <vector>

using Ubiquitty::Rendering::drcsGlyphKey;
using Ubiquitty::Rendering::rasterizeDRCSGlyph;
using Ubiquitty::Rendering::softFontHasGlyph;
using Ubiquitty::VT::drcsCodePointBase;
using Ubiquitty::VT::drcsMaxCells;
using Ubiquitty::VT::SoftFont;

// Slot geometry mirroring the macOS mono atlas: a cell box inset by the overhang
// margins the renderer passes as (cellX, cellY, cellWidth, cellHeight).
static constexpr int slotWidth = 20;
static constexpr int slotHeight = 40;
static constexpr int marginX = 2;
static constexpr int marginY = 3;
static constexpr int cellWidth = 16;
static constexpr int cellHeight = slotHeight - 2 * marginY; // 34

static bool raster(const SoftFont& font, int index, std::vector<std::uint8_t>& out)
{
    out.assign(static_cast<std::size_t>(slotWidth) * slotHeight, 0xAB); // poison: the raster must clear
    return rasterizeDRCSGlyph(font, index, { slotWidth, slotHeight }, { { marginX, marginY }, { cellWidth, cellHeight } }, out);
}

static std::uint8_t at(const std::vector<std::uint8_t>& buffer, int x, int y) { return buffer[static_cast<std::size_t>(y) * slotWidth + x]; }

static int inkCount(const std::vector<std::uint8_t>& buffer)
{
    int n = 0;
    for (std::uint8_t v : buffer)
        if (v != 0)
            ++n;
    return n;
}

static void feed(Ubiquitty::Terminal& t, const char* s) { Ubiquitty::processBytes(&t, s); }

// A font whose cell `index` is the given rows, MSB-free: bit (1u << col) = column col.
static SoftFont fontWith(int index, int matrixWidth, int matrixHeight, const std::uint16_t* rows)
{
    SoftFont font;
    font.valid = true;
    font.serial = 7;
    font.matrixWidth = static_cast<std::uint8_t>(matrixWidth);
    font.matrixHeight = static_cast<std::uint8_t>(matrixHeight);
    font.glyphSet[index] = true;
    for (int r = 0; r < matrixHeight; ++r)
        font.glyph[index][r] = rows[r];
    return font;
}

TEST(drcsRaster)
{

    std::vector<std::uint8_t> buffer;

    // --- a fully inked 8x8 matrix fills the cell box and nothing else ---
    {
        std::uint16_t rows[8];
        for (int r = 0; r < 8; ++r)
            rows[r] = 0x00FF; // columns 0-7
        const SoftFont font = fontWith(3, 8, 8, rows);

        CHECK(softFontHasGlyph(font, 3));
        CHECK(raster(font, 3, buffer));
        // Distinguishable from tofu: a missing glyph is coverage-empty, this is not.
        CHECK(inkCount(buffer) == cellWidth * cellHeight);
        // The cell box is saturated...
        CHECK(at(buffer, marginX, marginY) == 255);
        CHECK(at(buffer, marginX + cellWidth - 1, marginY + cellHeight - 1) == 255);
        // ...and the margins are cleared, so a glyph never spills into a neighbour.
        CHECK(at(buffer, 0, 0) == 0);
        CHECK(at(buffer, marginX - 1, marginY + 5) == 0);
        CHECK(at(buffer, marginX + cellWidth, marginY + 5) == 0);
        CHECK(at(buffer, marginX + 4, marginY - 1) == 0);
        CHECK(at(buffer, marginX + 4, slotHeight - 1) == 0);
    }

    // --- the bitmap's geometry survives the upscale (left half on, right half off) ---
    {
        std::uint16_t rows[8];
        for (int r = 0; r < 8; ++r)
            rows[r] = 0x000F; // columns 0-3 of 8
        const SoftFont font = fontWith(0, 8, 8, rows);

        CHECK(raster(font, 0, buffer));
        CHECK(inkCount(buffer) == (cellWidth / 2) * cellHeight); // exactly half, 8 of 16 columns
        for (int y = 0; y < cellHeight; ++y) {
            CHECK(at(buffer, marginX + cellWidth / 2 - 1, marginY + y) == 255);
            CHECK(at(buffer, marginX + cellWidth / 2, marginY + y) == 0);
        }
    }

    // --- row order is preserved: row 0 is the TOP of the cell ---
    {
        std::uint16_t rows[4] = { 0x000F, 0, 0, 0 }; // top band only
        const SoftFont font = fontWith(1, 4, 4, rows);

        CHECK(raster(font, 1, buffer));
        CHECK(at(buffer, marginX, marginY) == 255); // top of the cell box inked
        CHECK(at(buffer, marginX, marginY + cellHeight - 1) == 0); // bottom clear
        // 4 source rows over 34 destination rows: the top quarter, rounded up.
        CHECK(inkCount(buffer) == cellWidth * ((cellHeight + 3) / 4));
        // The band is 8.5 rows tall, so its ninth row is half covered and its
        // tenth is clear: area coverage, not a row that is whole here and
        // absent on the design's next row.
        for (int y = 0; y < 8; ++y)
            CHECK(at(buffer, marginX + 3, marginY + y) == 255);
        CHECK(at(buffer, marginX + 3, marginY + 8) > 100 && at(buffer, marginX + 3, marginY + 8) < 160);
        CHECK(at(buffer, marginX + 3, marginY + 9) == 0);
    }

    // --- a fractional upscale inks every design row alike ---
    {
        // One row in eight lit: over 34 rows each design row is 4.25 tall, and
        // nearest sampling gave the lit row four or five pixels depending on
        // which row it was. Now every design row covers the same area.
        for (int lit = 0; lit < 8; ++lit) {
            std::uint16_t rows[8] = {};
            rows[lit] = 0x00FF;
            const SoftFont font = fontWith(0, 8, 8, rows);
            CHECK(raster(font, 0, buffer));
            double area = 0.0;
            for (int y = 0; y < cellHeight; ++y)
                area += at(buffer, marginX, marginY + y) / 255.0;
            CHECK(area > 4.2 && area < 4.3);
        }
    }

    // --- downscale ORs the source span, so a 1-column stroke cannot vanish ---
    {
        std::uint16_t rows[16];
        for (int r = 0; r < 16; ++r)
            rows[r] = static_cast<std::uint16_t>(1u << 9); // a single column, 9 of 16
        const SoftFont font = fontWith(2, 16, 16, rows);

        std::vector<std::uint8_t> narrow(static_cast<std::size_t>(slotWidth) * slotHeight, 0);
        // Four destination columns for sixteen source columns: nearest-neighbour
        // sampling would step over column 9 entirely and paint nothing.
        CHECK(rasterizeDRCSGlyph(font, 2, { slotWidth, slotHeight }, { { 0, 0 }, { 4, 4 } }, narrow));
        int ink = 0;
        for (std::uint8_t v : narrow)
            if (v != 0)
                ++ink;
        CHECK(ink == 4); // one destination column, all four rows
        CHECK(narrow[2] == 255); // column 9/16 lands in destination column 2
    }

    // --- no glyph: refuse, and leave the buffer for the caller to skip ---
    {
        std::uint16_t rows[8];
        for (int r = 0; r < 8; ++r)
            rows[r] = 0x00FF;
        SoftFont font = fontWith(3, 8, 8, rows);

        // A position the host never downloaded.
        CHECK(!softFontHasGlyph(font, 4));
        CHECK(!raster(font, 4, buffer));
        // A font erased by RIS / a DECDLD Pe wipe keeps its bitmaps unreachable.
        font.valid = false;
        CHECK(!softFontHasGlyph(font, 3));
        CHECK(!raster(font, 3, buffer));
        // A degenerate matrix.
        font.valid = true;
        font.matrixWidth = 0;
        CHECK(!softFontHasGlyph(font, 3));
        CHECK(!raster(font, 3, buffer));
        // Out-of-range indices are refused, not clamped into the store.
        font.matrixWidth = 8;
        CHECK(!softFontHasGlyph(font, -1));
        CHECK(!softFontHasGlyph(font, drcsMaxCells));
        CHECK(!raster(font, drcsMaxCells, buffer));
        // A default-constructed font (no DECDLD ever) carries nothing.
        const SoftFont empty;
        CHECK(empty.serial == 0);
        CHECK(!softFontHasGlyph(empty, 0));
        CHECK(!raster(empty, 0, buffer));
    }

    // --- geometry the slot can't hold is refused rather than written out of bounds ---
    {
        std::uint16_t rows[8];
        for (int r = 0; r < 8; ++r)
            rows[r] = 0x00FF;
        const SoftFont font = fontWith(0, 8, 8, rows);
        std::vector<std::uint8_t> small(static_cast<std::size_t>(slotWidth) * slotHeight, 0);
        CHECK(!rasterizeDRCSGlyph(font, 0, { slotWidth, slotHeight }, { { marginX, marginY }, { slotWidth, cellHeight } }, small));
        CHECK(!rasterizeDRCSGlyph(font, 0, { slotWidth, slotHeight }, { { -1, 0 }, { 4, 4 } }, small));
        CHECK(!rasterizeDRCSGlyph(font, 0, { slotWidth, slotHeight }, { { 0, 0 }, { 0, 4 } }, small));
        CHECK(!rasterizeDRCSGlyph(font, 0, { 0, 0 }, { { 0, 0 }, { 4, 4 } }, small));
        // A destination with no room. This was `nullptr`; a span says the same
        // thing as an empty one, and says it without the caller being able to
        // claim bytes it does not have.
        CHECK(!rasterizeDRCSGlyph(font, 0, { slotWidth, slotHeight }, { { 0, 0 }, { 4, 4 } }, {}));
        // And a destination SHORTER than the slot, which the detached length
        // could not state at all: the callee compared against a number, not
        // against the buffer.
        std::vector<std::uint8_t> tooSmall(static_cast<std::size_t>(slotWidth) * slotHeight - 1, 0);
        CHECK(!rasterizeDRCSGlyph(font, 0, { slotWidth, slotHeight }, { { 0, 0 }, { 4, 4 } }, tooSmall));
    }

    // --- the atlas key separates fonts and positions, and dodges every neighbour ---
    {
        CHECK(drcsGlyphKey(7, 0) != drcsGlyphKey(7, 1)); // position is in the key
        CHECK(drcsGlyphKey(7, 0) != drcsGlyphKey(8, 0)); // so is the font's identity:
                                                         // the mono atlas is shared across panes
        // Bit 29 tags the DRCS keyspace; bits 30/31 belong to box and cluster keys.
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(drcsMaxCells); ++index) {
            const std::uint32_t key = drcsGlyphKey(0x3FFFFFu, index);
            CHECK((key & 0x20000000u) != 0);
            CHECK((key & 0xC0000000u) == 0);
            CHECK(key != Ubiquitty::Rendering::emptyGlyphKey);
            // A text key for the highest code point stays below bit 29.
            CHECK(key != Ubiquitty::Rendering::monoGlyphKey(0x10FFFF, 7, Ubiquitty::Rendering::FontFace::BoldItalic, 7));
            CHECK(key != Ubiquitty::Rendering::boxFontKey(0x259F, 7, Ubiquitty::Rendering::FontFace::BoldItalic));
        }
    }

    // --- the sentinel range the renderer branches on covers exactly the store ---
    {
        CHECK(drcsCodePointBase + drcsMaxCells - 1 < 0x10FFFFu); // still a code point
        // The renderer's per-cell test is this unsigned wrap; it must reject 'A'
        // and the code point one past the last DRCS position.
        CHECK(!((0x41u - drcsCodePointBase) < static_cast<std::uint32_t>(drcsMaxCells)));
        CHECK(((drcsCodePointBase + drcsMaxCells - 1) - drcsCodePointBase) < static_cast<std::uint32_t>(drcsMaxCells));
        CHECK(!(((drcsCodePointBase + drcsMaxCells) - drcsCodePointBase) < static_cast<std::uint32_t>(drcsMaxCells)));
    }

    // --- end to end: a DECDLD download reaches coverage through the cell ---
    // The join the two halves above are each blind to: the sentinel a printed DRCS
    // byte leaves in the cell must be the index the raster reads out of the store.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CHECK(Ubiquitty::ensureSoftFont(t).serial == 0); // no download yet: the snapshot carries nothing

        // Solid 8x8 glyph at cell 1 (Pcn=1 -> GL byte 0x21 '!'), designator '@'.
        feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
        const std::uint32_t firstSerial = Ubiquitty::ensureSoftFont(t).serial;
        CHECK(firstSerial != 0); // a committed download takes an identity
        feed(t, "\033(@!");

        const std::uint32_t codePoint = t.grid->cellAt(Ubiquitty::Row { 0 }, Ubiquitty::Column { 0 })->codePoint;
        // The renderer's per-cell test, byte for byte.
        CHECK(codePoint - drcsCodePointBase < static_cast<std::uint32_t>(drcsMaxCells));
        const int index = static_cast<int>(codePoint - drcsCodePointBase);
        const Ubiquitty::VT::SoftFont* downloaded = Ubiquitty::softFontOrNull(t);
        CHECK(downloaded != nullptr);
        CHECK(softFontHasGlyph(*downloaded, index));
        CHECK(raster(*downloaded, index, buffer));
        CHECK(inkCount(buffer) == cellWidth * cellHeight); // the solid glyph inks the whole cell box

        // A re-download takes a NEW serial, so the shared atlas retires the previous
        // rasterisations instead of serving them for the new bitmaps.
        feed(t, "\033P1;1;0;8;0;0;8;0{@BBBBBBBB\033\\");
        const std::uint32_t reloadedSerial = Ubiquitty::softFontOrNull(t)->serial;
        CHECK(reloadedSerial != firstSerial);
        CHECK(drcsGlyphKey(firstSerial, 1) != drcsGlyphKey(reloadedSerial, 1));

        // RIS erases the set, so a cell that outlives the font resolves to no glyph
        // — background only, which is the only safe answer once the bitmap is gone.
        Ubiquitty::hardReset(t);
        CHECK_MSG(Ubiquitty::softFontOrNull(t) == nullptr, "RIS releases the store");
        const Ubiquitty::VT::SoftFont erased {};
        CHECK(!softFontHasGlyph(erased, index));
        CHECK(!raster(erased, index, buffer));
    }
}

UBIQUITTY_TEST_MAIN()
