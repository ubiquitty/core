// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// lastDifferingCell against a one-cell-at-a-time loop, over every shape it can
// be asked. The kernel walks two cells to a step from the far end, so the places it
// can be wrong are the pairing (which of the two it reports), the tail when the
// span has odd length, and the empty answer.

#include "Harness.h"

#include "Core/CellScan.h"

#include <cstring>
#include <vector>

namespace {

// The definition, written the slow obvious way.
int referenceLastDiffering(const Ubiquitty::Cell* cells, int lo, int hi, const Ubiquitty::Cell& pattern)
{
    for (int index = hi; index >= lo; --index) {
        if (std::memcmp(&cells[index], &pattern, sizeof(Ubiquitty::Cell)) != 0)
            return index;
    }
    return lo - 1;
}

} // namespace

TEST(theKernelAgreesWithTheLoopAtEveryPositionAndLength)
{
    // Every span up to 40 cells, with the one differing cell placed at every
    // position in it, and the all-matching case for each length.
    constexpr int maxLength = 40;
    Ubiquitty::Cell pattern {};
    pattern.codePoint = 0;

    for (int length = 0; length <= maxLength; ++length) {
        for (int different = -1; different < length; ++different) {
            std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(maxLength + 8), pattern);
            if (different >= 0)
                row[static_cast<std::size_t>(different)].codePoint = 'x';
            const int hi = length - 1;
            const int mine = Ubiquitty::lastDifferingCell(row.data(), 0, hi, pattern);
            const int theirs = referenceLastDiffering(row.data(), 0, hi, pattern);
            CHECK(mine == theirs);
            if (mine != theirs) {
                std::printf("  length %d, differing at %d: kernel %d, loop %d\n", length, different, mine, theirs);
                return;
            }
        }
    }
}

TEST(theKernelAgreesWhenSeveralCellsDiffer)
{
    // The answer is the LAST one, so a span with many differences exercises the
    // pair reduction's choice rather than its detection.
    constexpr int length = 33;
    Ubiquitty::Cell pattern {};
    for (int mask = 0; mask < (1 << 8); ++mask) {
        std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(length), pattern);
        for (int bit = 0; bit < 8; ++bit) {
            if (mask & (1 << bit))
                row[static_cast<std::size_t>(bit * 4)].codePoint = 'a' + bit;
        }
        const int mine = Ubiquitty::lastDifferingCell(row.data(), 0, length - 1, pattern);
        const int theirs = referenceLastDiffering(row.data(), 0, length - 1, pattern);
        CHECK(mine == theirs);
        if (mine != theirs) {
            std::printf("  mask %d: kernel %d, loop %d\n", mask, mine, theirs);
            return;
        }
    }
}

TEST(theKernelAgreesFromEveryStartingOffset)
{
    // lo is not always zero: a band starts where the margin does, and the paired
    // step's alignment follows lo rather than the row.
    constexpr int length = 48;
    Ubiquitty::Cell pattern {};
    for (int lo = 0; lo < 8; ++lo) {
        for (int different = lo; different < length; ++different) {
            std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(length), pattern);
            row[static_cast<std::size_t>(different)].codePoint = 'q';
            const int mine = Ubiquitty::lastDifferingCell(row.data(), lo, length - 1, pattern);
            const int theirs = referenceLastDiffering(row.data(), lo, length - 1, pattern);
            CHECK(mine == theirs);
            if (mine != theirs) {
                std::printf("  lo %d, differing at %d: kernel %d, loop %d\n", lo, different, mine, theirs);
                return;
            }
        }
    }
}

TEST(theKernelNoticesADifferenceInAnyByteOfTheCell)
{
    // A cell differs by any of its thirty-two bytes, not only its code point.
    // Flipping each in turn proves the compare covers the whole cell.
    constexpr int length = 20;
    Ubiquitty::Cell pattern {};
    for (std::size_t byte = 0; byte < sizeof(Ubiquitty::Cell); ++byte) {
        std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(length), pattern);
        unsigned char* raw = reinterpret_cast<unsigned char*>(&row[7]);
        raw[byte] = static_cast<unsigned char>(raw[byte] ^ 0xFF);
        const int mine = Ubiquitty::lastDifferingCell(row.data(), 0, length - 1, pattern);
        CHECK(mine == 7);
        if (mine != 7) {
            std::printf("  byte %zu flipped: kernel said %d, expected 7\n", byte, mine);
            return;
        }
    }
}

TEST(anEmptySpanAndABackwardsSpanReportNothing)
{
    Ubiquitty::Cell pattern {};
    std::vector<Ubiquitty::Cell> row(8, pattern);
    row[0].codePoint = 'z';

    // hi below lo scans nothing, whatever the row holds.
    CHECK(Ubiquitty::lastDifferingCell(row.data(), 3, 2, pattern) == 2);
    CHECK(Ubiquitty::lastDifferingCell(row.data(), 0, -1, pattern) == -1);
    // A null row is the same answer rather than a read.
    CHECK(Ubiquitty::lastDifferingCell(nullptr, 0, 5, pattern) == -1);
    // A single cell, both ways.
    CHECK(Ubiquitty::lastDifferingCell(row.data(), 0, 0, pattern) == 0);
    CHECK(Ubiquitty::lastDifferingCell(row.data(), 1, 1, pattern) == 0);
}

UBIQUITTY_TEST_MAIN()
