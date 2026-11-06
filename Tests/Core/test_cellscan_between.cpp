// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// firstDifferingCellBetween answers what a scalar memcmp walk answers.
//
// It is the damage walk's rejection scan: two rows in, the first column where
// they stop agreeing out. A wrong answer is not a crash — it is a run that
// starts a column late, so a repaint leaves a stale cell on screen, or a run
// that starts early and repaints what was already right. Neither shows in a
// test that only asks whether the rows differ at all.
//
// The paired NEON step reads two cells at once, so the cases that matter are
// the ones where the answer falls on the second cell of a pair, on the last
// cell of a row, and on a row whose length leaves the tail loop something to
// do. Those are driven exhaustively here rather than sampled.

#include "Harness.h"

#include "Core/Cell.h"
#include "Core/CellScan.h"

#include <cstring>
#include <vector>

using namespace Ubiquitty;

namespace {

// The reference: the same question asked one cell at a time.
int scalarFirstDifference(const Cell* left, const Cell* right, int lo, int hi)
{
    for (int index = lo; index <= hi; ++index) {
        if (std::memcmp(&left[index], &right[index], sizeof(Cell)) != 0)
            return index;
    }
    return hi + 1;
}

Cell cellWith(std::uint32_t codePoint)
{
    Cell cell {};
    cell.codePoint = codePoint;
    return cell;
}

} // namespace

TEST(firstDifferingCellBetweenMatchesTheScalarWalkAtEveryPosition)
{
    // Every row length up to a few pairs past the step, and within each, every
    // position the difference can sit at — including none at all.
    for (int length = 1; length <= 9; ++length) {
        std::vector<Cell> left(static_cast<std::size_t>(length));
        for (int index = 0; index < length; ++index)
            left[static_cast<std::size_t>(index)] = cellWith(static_cast<std::uint32_t>('a' + index));

        // Rows that agree whole: the answer is one past the end, which is what
        // tells the walk the row is clean.
        std::vector<Cell> same = left;
        CHECK(firstDifferingCellBetween(left.data(), same.data(), 0, length - 1) == length);
        CHECK(firstDifferingCellBetween(left.data(), same.data(), 0, length - 1)
            == scalarFirstDifference(left.data(), same.data(), 0, length - 1));

        for (int differsAt = 0; differsAt < length; ++differsAt) {
            std::vector<Cell> right = left;
            right[static_cast<std::size_t>(differsAt)] = cellWith(0x2588); // a different glyph
            CHECK(firstDifferingCellBetween(left.data(), right.data(), 0, length - 1) == differsAt);
            CHECK(firstDifferingCellBetween(left.data(), right.data(), 0, length - 1)
                == scalarFirstDifference(left.data(), right.data(), 0, length - 1));

            // From every start, so a walk resuming mid-row gets the same answer
            // as one that never stopped.
            for (int lo = 0; lo < length; ++lo) {
                CHECK(firstDifferingCellBetween(left.data(), right.data(), lo, length - 1)
                    == scalarFirstDifference(left.data(), right.data(), lo, length - 1));
            }
        }
    }
}

TEST(firstDifferingCellBetweenSeesADifferenceInAnyByteOfTheCell)
{
    // A Cell is thirty-two bytes and the step compares all of them; a scan that
    // watched only the code point would miss a pen change, which is exactly the
    // repaint the walk exists to schedule.
    std::vector<Cell> left(4);
    for (auto& cell : left)
        cell = cellWith('x');

    for (std::size_t byteIndex = 0; byteIndex < sizeof(Cell); ++byteIndex) {
        std::vector<Cell> right = left;
        auto* bytes = reinterpret_cast<std::uint8_t*>(&right[2]);
        bytes[byteIndex] = static_cast<std::uint8_t>(bytes[byteIndex] ^ 0x40);
        const int found = firstDifferingCellBetween(left.data(), right.data(), 0, 3);
        CHECK(found == scalarFirstDifference(left.data(), right.data(), 0, 3));
        // Byte-identical after the flip means the flip was a no-op on padding;
        // otherwise the difference is at cell 2 and nowhere earlier.
        if (std::memcmp(&left[2], &right[2], sizeof(Cell)) != 0)
            CHECK(found == 2);
    }
}

TEST(firstDifferingCellBetweenRefusesDegenerateRanges)
{
    std::vector<Cell> left(2, cellWith('q'));
    std::vector<Cell> right(2, cellWith('r'));
    // A high below the low scans nothing and answers the low, which is what the
    // walk reads as "no run here".
    CHECK(firstDifferingCellBetween(left.data(), right.data(), 1, 0) == 1);
    CHECK(firstDifferingCellBetween(nullptr, right.data(), 0, 1) == 0);
    CHECK(firstDifferingCellBetween(left.data(), nullptr, 0, 1) == 0);
}

UBIQUITTY_TEST_MAIN()
