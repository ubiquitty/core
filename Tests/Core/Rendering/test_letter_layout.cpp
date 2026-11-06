// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "Harness.h"

// test_letter_layout.cpp — a row set in a proportional primary face
// (Source/Core/Rendering/LetterLayout.{h,cpp}): letters stand at the shaper's
// advances, cell-bound glyphs keep their cells inside the row, and a pixel
// maps back to the column under it.

#include "Core/Rendering/LetterLayout.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using Ubiquitty::ProportionalShapedGlyph;
using Ubiquitty::Rendering::FontFace;
using Ubiquitty::Rendering::layoutRowLetters;
using Ubiquitty::Rendering::LetterCell;
using Ubiquitty::Rendering::LetterCellKind;
using Ubiquitty::Rendering::letterColumnAt;
using Ubiquitty::Rendering::LetterExtent;
using Ubiquitty::Rendering::LetterLayoutScratch;
using Ubiquitty::Rendering::letterXInCells;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

/// A shaper whose advances are a function of the code point: `i` is narrow,
/// `W` wide, everything else the digit's width, and a mark adds nothing. It
/// kerns `AV` by closing the pair one pixel. It records what it was asked.
struct FakeShaper {
    std::vector<std::vector<std::uint32_t>> runsAsked;
    std::vector<FontFace> facesAsked;
    bool shouldRefuse = false; // bool-ok: one test's switch
    bool fusesFI = false; // bool-ok: one test's switch

    static double advanceOf(std::uint32_t codePoint)
    {
        if (codePoint == U'i' || codePoint == U'l')
            return 4.0;
        if (codePoint == U'W' || codePoint == U'm')
            return 16.0;
        if (codePoint == 0x0301)
            return 0.0;
        if (codePoint == U' ')
            return 5.0;
        return 10.0;
    }

    static int shape(void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t, ProportionalShapedGlyph* out, int capacity)
    {
        auto& self = *static_cast<FakeShaper*>(context);
        self.runsAsked.emplace_back(codePoints.begin(), codePoints.end());
        self.facesAsked.push_back(face);
        if (self.shouldRefuse || static_cast<int>(codePoints.size()) > capacity)
            return 0;
        int written = 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            if (self.fusesFI && index + 1 < codePoints.size() && codePoints[index] == U'f' && codePoints[index + 1] == U'i') {
                out[written].glyph = 999;
                out[written].advanceInPixels = 13.0f;
                out[written].cluster = static_cast<std::uint16_t>(index);
                ++written;
                ++index;
                continue;
            }
            out[written].glyph = codePoints[index];
            double advance = advanceOf(codePoints[index]);
            if (codePoints[index] == U'A' && index + 1 < codePoints.size() && codePoints[index + 1] == U'V')
                advance -= 1.0;
            out[written].advanceInPixels = static_cast<float>(advance);
            out[written].cluster = static_cast<std::uint16_t>(index);
            ++written;
        }
        return written;
    }
};

/// A row of letters from ASCII, every cell in one pen.
struct Row {
    std::vector<LetterCell> cells;
    std::vector<std::uint32_t> pool;

    void letter(std::uint32_t codePoint, std::uint32_t runKey = 1, FontFace face = FontFace::Regular)
    {
        LetterCell cell;
        cell.kind = LetterCellKind::Letter;
        cell.face = face;
        cell.runKey = runKey;
        cell.clusterStart = static_cast<std::uint32_t>(pool.size());
        cell.clusterCount = 1;
        pool.push_back(codePoint);
        cells.push_back(cell);
    }

    void letters(const char* ascii, std::uint32_t runKey = 1)
    {
        for (const char* p = ascii; *p; ++p)
            letter(static_cast<std::uint32_t>(static_cast<unsigned char>(*p)), runKey);
    }

    void cellBound(float widthCells, int continuations = 0)
    {
        LetterCell cell;
        cell.kind = LetterCellKind::CellBound;
        cell.widthCells = widthCells;
        cells.push_back(cell);
        for (int k = 0; k < continuations; ++k) {
            LetterCell continuation;
            continuation.kind = LetterCellKind::Continuation;
            cells.push_back(continuation);
        }
    }

    void empty(int count = 1)
    {
        for (int k = 0; k < count; ++k)
            cells.emplace_back();
    }

    std::vector<LetterExtent> layout(FakeShaper& shaper, double cellAdvance = 10.0, double widthScale = 1.0, bool* didShape = nullptr)
    {
        std::vector<LetterExtent> out(cells.size());
        LetterLayoutScratch scratch;
        const bool didShapeRow = layoutRowLetters(cells, pool, cellAdvance, widthScale, &FakeShaper::shape, &shaper, scratch, out);
        if (didShape)
            *didShape = didShapeRow;
        return out;
    }
};

} // namespace

TEST(lettersStandAtTheShapersAdvancesNotTheCellPitch)
{
    Row row;
    row.letters("Wii");
    FakeShaper shaper;
    bool didShape = false;
    const auto letters = row.layout(shaper, 10.0, 1.0, &didShape);
    CHECK(didShape);
    CHECK(near(letters[0].leftInPixels, 0.0) && near(letters[0].advanceInPixels, 16.0));
    CHECK(near(letters[1].leftInPixels, 16.0) && near(letters[1].advanceInPixels, 4.0));
    CHECK(near(letters[2].leftInPixels, 20.0) && near(letters[2].advanceInPixels, 4.0));
    // One run, shaped once, in the pen's face.
    CHECK_EQ(shaper.runsAsked.size(), static_cast<std::size_t>(1));
    CHECK_EQ(shaper.runsAsked[0].size(), static_cast<std::size_t>(3));
}

TEST(aKernMovesTheLetterAfterIt)
{
    Row row;
    row.letters("AVA");
    FakeShaper shaper;
    const auto letters = row.layout(shaper);
    CHECK(near(letters[0].advanceInPixels, 9.0));
    CHECK(near(letters[1].leftInPixels, 9.0));
    CHECK(near(letters[2].leftInPixels, 19.0));
}

TEST(aRunBreaksWhereThePenChanges)
{
    Row row;
    row.letters("ab", 1);
    row.letters("cd", 2);
    row.letter(U'e', 2, FontFace::Bold);
    FakeShaper shaper;
    const auto letters = row.layout(shaper);
    CHECK_EQ(shaper.runsAsked.size(), static_cast<std::size_t>(3));
    CHECK_EQ(shaper.runsAsked[0].size(), static_cast<std::size_t>(2));
    CHECK_EQ(shaper.runsAsked[1].size(), static_cast<std::size_t>(2));
    CHECK_EQ(shaper.runsAsked[2].size(), static_cast<std::size_t>(1));
    CHECK(shaper.facesAsked[2] == FontFace::Bold);
    // The pen flows across the breaks.
    CHECK(near(letters[2].leftInPixels, 20.0));
    CHECK(near(letters[4].leftInPixels, 40.0));
}

TEST(aCellBoundGlyphKeepsItsCellsInsideTheRow)
{
    Row row;
    row.letters("ii");
    row.cellBound(2.0f, 1); // a wide glyph: lead and continuation
    row.letters("i");
    row.cellBound(1.0f); // a box-drawing cell
    row.letters("W");
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0);
    CHECK(near(letters[1].leftInPixels, 4.0));
    // The wide lead takes both cells at the pitch, after the letters' 8 px.
    CHECK(near(letters[2].leftInPixels, 8.0) && near(letters[2].advanceInPixels, 20.0));
    // Its continuation takes the second cell's slice, so a pointer there names it.
    CHECK(near(letters[3].leftInPixels, 18.0) && near(letters[3].advanceInPixels, 10.0));
    CHECK(near(letters[4].leftInPixels, 28.0) && near(letters[4].advanceInPixels, 4.0));
    CHECK(near(letters[5].leftInPixels, 32.0) && near(letters[5].advanceInPixels, 10.0));
    CHECK(near(letters[6].leftInPixels, 42.0) && near(letters[6].advanceInPixels, 16.0));
    // The letters either side of a cell-bound glyph are separate runs.
    CHECK_EQ(shaper.runsAsked.size(), static_cast<std::size_t>(3));
}

TEST(anEmptyCellKeepsThePitchSoAnAddressedColumnStaysWhereTheGridPutIt)
{
    Row row;
    row.letters("ii");
    row.empty(3);
    row.letters("i");
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0);
    CHECK(near(letters[2].leftInPixels, 8.0) && near(letters[2].advanceInPixels, 10.0));
    CHECK(near(letters[4].leftInPixels, 28.0));
    CHECK(near(letters[5].leftInPixels, 38.0) && near(letters[5].advanceInPixels, 4.0));
}

TEST(aWrittenSpaceIsALetterWithTheFacesSpaceAdvance)
{
    Row row;
    row.letters("a b");
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0);
    CHECK(near(letters[1].advanceInPixels, 5.0));
    CHECK(near(letters[2].leftInPixels, 15.0));
}

TEST(aMarkAddsNothingToItsBase)
{
    Row row;
    row.letter(U'a');
    // e with a combining acute: one cell, two code points.
    LetterCell cell;
    cell.kind = LetterCellKind::Letter;
    cell.runKey = 1;
    cell.clusterStart = static_cast<std::uint32_t>(row.pool.size());
    cell.clusterCount = 2;
    row.pool.push_back(U'e');
    row.pool.push_back(0x0301);
    row.cells.push_back(cell);
    row.letter(U'b');
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0);
    CHECK_EQ(shaper.runsAsked[0].size(), static_cast<std::size_t>(4));
    CHECK(near(letters[1].leftInPixels, 10.0) && near(letters[1].advanceInPixels, 10.0));
    CHECK(near(letters[2].leftInPixels, 20.0));
}

TEST(aCellTheShaperFusedStandsAtItsNeighboursEndWithNoAdvance)
{
    Row row;
    row.letters("fix");
    FakeShaper shaper;
    shaper.fusesFI = true;
    const auto letters = row.layout(shaper, 10.0);
    CHECK(near(letters[0].leftInPixels, 0.0) && near(letters[0].advanceInPixels, 13.0));
    CHECK(near(letters[1].leftInPixels, 13.0) && near(letters[1].advanceInPixels, 0.0));
    CHECK(near(letters[2].leftInPixels, 13.0) && near(letters[2].advanceInPixels, 10.0));
    // A pixel over the ligature names the f; the i has no extent to name.
    CHECK_EQ(letterColumnAt(letters, 12.0), 0);
    CHECK_EQ(letterColumnAt(letters, 13.5), 2);
}

TEST(aRefusedRunKeepsTheGridsPitch)
{
    Row row;
    row.letters("Wii");
    FakeShaper shaper;
    shaper.shouldRefuse = true;
    bool didShape = true;
    const auto letters = row.layout(shaper, 10.0, 1.0, &didShape);
    CHECK(!didShape);
    for (std::size_t column = 0; column < letters.size(); ++column)
        CHECK(near(letters[column].leftInPixels, 10.0 * static_cast<double>(column)) && near(letters[column].advanceInPixels, 10.0));
}

TEST(aRowWithNoShaperIsTheGrid)
{
    Row row;
    row.letters("Wii");
    std::vector<LetterExtent> out(row.cells.size());
    LetterLayoutScratch scratch;
    CHECK(!layoutRowLetters(row.cells, row.pool, 10.0, 1.0, nullptr, nullptr, scratch, out));
    CHECK(near(out[2].leftInPixels, 20.0) && near(out[2].advanceInPixels, 10.0));
}

TEST(aDoubledRowTakesEveryAdvanceTwice)
{
    Row row;
    row.letters("Wi");
    row.cellBound(2.0f); // a cell-bound glyph already carries the doubling in its cells
    row.empty();
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0, 2.0);
    CHECK(near(letters[0].advanceInPixels, 32.0));
    CHECK(near(letters[1].leftInPixels, 32.0) && near(letters[1].advanceInPixels, 8.0));
    CHECK(near(letters[2].leftInPixels, 40.0) && near(letters[2].advanceInPixels, 20.0));
    CHECK(near(letters[3].leftInPixels, 60.0) && near(letters[3].advanceInPixels, 20.0));
}

TEST(aPixelMapsToTheLetterUnderItAndToItsNearestBoundary)
{
    Row row;
    row.letters("Wii"); // 0..16, 16..20, 20..24
    FakeShaper shaper;
    const auto letters = row.layout(shaper, 10.0);
    CHECK_EQ(letterColumnAt(letters, -3.0), 0);
    CHECK_EQ(letterColumnAt(letters, 15.9), 0);
    CHECK_EQ(letterColumnAt(letters, 16.0), 1);
    CHECK_EQ(letterColumnAt(letters, 21.0), 2);
    CHECK_EQ(letterColumnAt(letters, 50.0), 2);
    // The position in cells: the column plus the fraction of its letter.
    CHECK(near(letterXInCells(letters, 8.0), 0.5));
    CHECK(near(letterXInCells(letters, 17.0), 1.25));
    // The ladder rounds that to the nearest boundary: a press on the right
    // half of the W stands after it, as it would on a monospace row.
    CHECK_EQ(static_cast<int>(std::floor(letterXInCells(letters, 12.0) + 0.5)), 1);
    CHECK_EQ(static_cast<int>(std::floor(letterXInCells(letters, 17.0) + 0.5)), 1);
    CHECK_EQ(static_cast<int>(std::floor(letterXInCells(letters, 19.0) + 0.5)), 2);
    CHECK(near(letterXInCells(letters, 50.0), 3.0));
    CHECK(near(letterXInCells(letters, -3.0), 0.0));
    CHECK_EQ(letterColumnAt({}, 4.0), 0);
    CHECK(near(letterXInCells({}, 4.0), 0.0));
}

UBIQUITTY_TEST_MAIN()
