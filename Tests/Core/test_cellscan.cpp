// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/CellPen.h"
#include "Core/CellScan.h"
#include "Harness.h"

#include <cstring>
#include <vector>

using Ubiquitty::Cell;
using Ubiquitty::firstDifferingCell;

namespace {

Cell patternCell()
{
    Cell c = Ubiquitty::defaultBlankCell;
    c.codePoint = 'x';
    Ubiquitty::Pen p = cellPen(c);
    p.foregroundIndex = 3;
    cellSetPen(c, p);
    return c;
}

// Perturb one byte at `offset` so a difference can be placed anywhere in the
// 32-byte record, not only in the fields a test happens to name.
Cell differingAt(const Cell& base, std::size_t offset)
{
    Cell c = base;
    reinterpret_cast<unsigned char*>(&c)[offset] ^= 0xFF;
    return c;
}

}

TEST(an_all_matching_run_reports_one_past_the_end)
{
    const Cell pattern = patternCell();
    for (int count : { 1, 2, 3, 8, 15, 16, 17, 200 }) {
        std::vector<Cell> row(static_cast<std::size_t>(count), pattern);
        CHECK_EQ(firstDifferingCell(row.data(), 0, count - 1, pattern), count);
    }
}

TEST(a_difference_is_found_at_every_index)
{
    const Cell pattern = patternCell();
    // Odd and even counts both matter: the vector step consumes pairs, so the
    // last cell of an odd run is only reached by the scalar tail.
    for (int count : { 1, 2, 3, 4, 5, 16, 17, 33, 200 }) {
        for (int at = 0; at < count; ++at) {
            std::vector<Cell> row(static_cast<std::size_t>(count), pattern);
            row[static_cast<std::size_t>(at)] = differingAt(pattern, 7);
            CHECK_EQ(firstDifferingCell(row.data(), 0, count - 1, pattern), at);
        }
    }
}

TEST(only_the_first_difference_is_reported)
{
    const Cell pattern = patternCell();
    std::vector<Cell> row(64, pattern);
    row[9] = differingAt(pattern, 1);
    row[10] = differingAt(pattern, 2);
    row[40] = differingAt(pattern, 3);
    CHECK_EQ(firstDifferingCell(row.data(), 0, 63, pattern), 9);
}

TEST(a_difference_in_any_byte_of_the_record_is_seen)
{
    const Cell pattern = patternCell();
    // The paired vector step ORs two cells before reducing; a byte that only
    // differs in the second cell of a pair must still be caught.
    for (std::size_t offset = 0; offset < sizeof(Cell); ++offset) {
        for (int at : { 0, 1, 2, 3 }) {
            std::vector<Cell> row(8, pattern);
            row[static_cast<std::size_t>(at)] = differingAt(pattern, offset);
            CHECK_EQ(firstDifferingCell(row.data(), 0, 7, pattern), at);
        }
    }
}

TEST(a_subrange_is_honoured_at_both_ends)
{
    const Cell pattern = patternCell();
    std::vector<Cell> row(32, pattern);
    row[0] = differingAt(pattern, 5); // before lo
    row[31] = differingAt(pattern, 5); // after hi
    CHECK_EQ(firstDifferingCell(row.data(), 1, 30, pattern), 31);

    row[17] = differingAt(pattern, 5);
    CHECK_EQ(firstDifferingCell(row.data(), 1, 30, pattern), 17);
    CHECK_EQ(firstDifferingCell(row.data(), 18, 30, pattern), 31); // past it
}

TEST(an_empty_or_absent_range_scans_nothing)
{
    const Cell pattern = patternCell();
    std::vector<Cell> row(4, pattern);
    CHECK_EQ(firstDifferingCell(row.data(), 2, 1, pattern), 2); // hi < lo
    CHECK_EQ(firstDifferingCell(nullptr, 0, 3, pattern), 0);
}

TEST(a_single_cell_run_is_decided_by_the_scalar_tail)
{
    const Cell pattern = patternCell();
    std::vector<Cell> row(1, pattern);
    CHECK_EQ(firstDifferingCell(row.data(), 0, 0, pattern), 1);
    row[0] = differingAt(pattern, 31); // last byte of the record
    CHECK_EQ(firstDifferingCell(row.data(), 0, 0, pattern), 0);
}

TEST(pairing_starts_at_lo_so_an_odd_start_is_covered)
{
    const Cell pattern = patternCell();
    // The vector step consumes pairs beginning at `lo`, so an odd `lo` pairs a
    // different set of cells than an even one and moves which index lands in
    // the scalar tail. Drive every start against every difference position.
    for (int lo = 0; lo < 6; ++lo) {
        for (int count : { 8, 9, 16, 17 }) {
            const int hi = lo + count - 1;
            for (int at = lo; at <= hi; ++at) {
                std::vector<Cell> row(static_cast<std::size_t>(hi + 2), pattern);
                row[static_cast<std::size_t>(at)] = differingAt(pattern, 13);
                CHECK_EQ(firstDifferingCell(row.data(), lo, hi, pattern), at);
            }
            std::vector<Cell> clean(static_cast<std::size_t>(hi + 2), pattern);
            CHECK_EQ(firstDifferingCell(clean.data(), lo, hi, pattern), hi + 1);
        }
    }
}

TEST(a_difference_in_the_second_cell_of_a_pair_is_not_masked_by_the_first)
{
    const Cell pattern = patternCell();
    // The paired step ORs both cells before reducing, then splits. A cell that
    // matches followed by one that differs must report the SECOND index, and
    // every byte offset must survive the or.
    for (std::size_t offset = 0; offset < sizeof(Cell); ++offset) {
        std::vector<Cell> row(4, pattern);
        row[1] = differingAt(pattern, offset);
        CHECK_EQ(firstDifferingCell(row.data(), 0, 3, pattern), 1);
        std::vector<Cell> both(4, pattern);
        both[0] = differingAt(pattern, offset);
        both[1] = differingAt(pattern, offset);
        CHECK_EQ(firstDifferingCell(both.data(), 0, 3, pattern), 0); // first wins
    }
}

TEST(an_all_zero_pattern_is_not_confused_with_a_match)
{
    // A zeroed pattern makes XOR-with-pattern equal the cell itself, so a
    // reduce that mishandled zero would report every cell as matching.
    // Value-initialized rather than memset: Cell.h states the type is not
    // memset-able, and a test that violates the rule it exercises proves
    // nothing about the rule.
    Cell zero {};
    std::vector<Cell> row(8, zero);
    CHECK_EQ(firstDifferingCell(row.data(), 0, 7, zero), 8);
    row[5] = differingAt(zero, 0);
    CHECK_EQ(firstDifferingCell(row.data(), 0, 7, zero), 5);
}

TEST(a_long_run_matches_the_scalar_definition_everywhere)
{
    // Differential test against the obvious implementation, over a length that
    // exercises many vector steps plus a tail.
    const Cell pattern = patternCell();
    for (int count : { 199, 200, 201 }) {
        for (int at = 0; at < count; at += 7) {
            std::vector<Cell> row(static_cast<std::size_t>(count), pattern);
            row[static_cast<std::size_t>(at)] = differingAt(pattern, static_cast<std::size_t>(at % sizeof(Cell)));
            int expected = count;
            for (int i = 0; i < count; ++i) {
                if (std::memcmp(&row[static_cast<std::size_t>(i)], &pattern, sizeof(Cell)) != 0) {
                    expected = i;
                    break;
                }
            }
            CHECK_EQ(firstDifferingCell(row.data(), 0, count - 1, pattern), expected);
        }
    }
}

UBIQUITTY_TEST_MAIN()
