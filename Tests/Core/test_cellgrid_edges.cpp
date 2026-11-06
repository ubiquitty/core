// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_cellgrid_edges.cpp — CellGrid's clamp and recycle arms, driven
// directly. The VT tests reach this file through the terminal, which never
// hands it an off-grid coordinate or a styled scroll blank in most flows —
// so the clamps that keep a hostile or racing caller on the grid, and the
// single-line recycle paths with a NON-default blank, sat unexecuted. Every
// case seeds recognisable content and asserts the grid afterwards, because a
// clamp that "works" by writing the wrong cells still reads as a pass under
// an assertion-free call.

#include "Core/CellContent.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/LineStore.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

using namespace Ubiquitty;

namespace {

Cell defaultBlank()
{
    Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    return cell;
}

Cell letter(char c)
{
    Cell cell = defaultBlank();
    cell.codePoint = static_cast<std::uint32_t>(c);
    return cell;
}

// A blank carrying ink, so isDefaultBlank says no and the styled arms run.
Cell styledBlank()
{
    Cell cell = defaultBlank();
    cell.pen.backgroundIndex = 4;
    return cell;
}

// The visible row as ASCII, '.' for anything blank or non-ASCII.
std::string rowText(const CellGrid& grid, int row)
{
    std::string text;
    for (int column = 0; column < grid.columns(); ++column) {
        const Cell* cell = grid.cellAt(Row(row), Column(column));
        const bool printable = cell && cell->codePoint >= 0x20 && cell->codePoint < 0x7F;
        text.push_back(printable ? static_cast<char>(cell->codePoint) : '.');
    }
    return text;
}

void writeWord(CellGrid& grid, int row, const char* word)
{
    grid.writeASCIIRun(Row(row), Column(0), defaultBlank(), word);
}

CellGrid grid5x8(const char* const (&rows)[5])
{
    CellGrid grid;
    grid.init(5, 8);
    for (int row = 0; row < 5; ++row)
        writeWord(grid, row, rows[row]);
    return grid;
}

} // namespace

TEST(wideCodePointRunsMatchSpansAndPreserveIdempotence)
{
    const std::array<uint32_t, 4> codePoints { 0x4E00, 0x754C, 0xAC00, 0x20000 };
    for (const int firstColumn : { 0, 2 }) {
        for (const int count : { 2, 3 }) {
            for (const int preparation : { 0, 1, 2 }) {
                for (const int style : { 0, 1 }) {
                    CellGrid actual;
                    CellGrid expected;
                    actual.init(4, 8);
                    expected.init(4, 8);
                    for (auto* grid : { &actual, &expected }) {
                        writeWord(*grid, 0, "abcdefgh");
                        Cell joiner = defaultBlank();
                        joiner.codePoint = 0x200D;
                        grid->setCell(Row(3), Column(7), joiner);
                        if (preparation) {
                            grid->scrollRegion(Row(0), Row(3), 1, defaultBlank());
                            if (preparation == 2)
                                grid->setCellKnownDiff(Row(3), Column(0), letter('X'));
                        }
                    }
                    Cell cellTemplate = defaultBlank();
                    if (style) {
                        cellTemplate.pen.foregroundIndex = 2;
                        cellTemplate.pen.backgroundIndex = 4;
                        cellTemplate.pen.attributes.add(VT::Attribute::Blink);
                        cellTemplate.pen.extendedAttributes.setTextSizeSpecID(1);
                        cellTemplate.setLinkID(7);
                    }
                    std::array<Cell, 8> cells;
                    for (int index = 0; index < count; ++index) {
                        cells[index * 2] = cellTemplate;
                        cells[index * 2].codePoint = codePoints[index];
                        cells[index * 2].pen.attributes.add(VT::Attribute::Wide);
                        cells[index * 2 + 1] = cellTemplate;
                        cells[index * 2 + 1].codePoint = wideContinuationSentinelCodePoint;
                        cells[index * 2 + 1].pen.attributes.add(VT::Attribute::WideContinuation);
                    }
                    actual.writeWideCodePointRun(Row(3), Column(firstColumn), cellTemplate, std::span(codePoints).first(static_cast<std::size_t>(count)));
                    expected.setRowSpan(Row(3), Column(firstColumn), { cells.data(), count * 2 });
                    for (int column = 0; column < 8; ++column)
                        CHECK(std::memcmp(actual.logicalCellAt(Row(3), Column(column)), expected.logicalCellAt(Row(3), Column(column)), sizeof(Cell)) == 0);
                    CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
                    CHECK_EQ(actual.rowGeneration(3), expected.rowGeneration(3));
                    CHECK_EQ(actual.rowContentWidth(Row(3)), expected.rowContentWidth(Row(3)));
                    CHECK_EQ(actual.hasSizedCellHint(), expected.hasSizedCellHint());
                    CHECK_EQ(actual.blinkHintGeneration(), expected.blinkHintGeneration());
                    CHECK(actual.rowMeta(3).has(RowFlag::NeedsShaping) == expected.rowMeta(3).has(RowFlag::NeedsShaping));
                    CHECK(actual.rowMeta(3).has(RowFlag::HasMulticell));
                    const auto generation = actual.contentGeneration();
                    actual.writeWideCodePointRun(Row(3), Column(firstColumn), cellTemplate, std::span(codePoints).first(static_cast<std::size_t>(count)));
                    CHECK_EQ(actual.contentGeneration(), generation);
                }
            }
        }
    }
}

TEST(compactASCIIRowsMatchMaterializedWritesAndExpandWithoutLosingBytes)
{
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const int columns : { 1, 7, 31, 32, 33, 80 }) {
            CellGrid actual;
            CellGrid expected;
            actual.init(4, columns);
            expected.init(4, columns);
            actual.setAlternateActive(screen);
            expected.setAlternateActive(screen);
            for (int row = 0; row < 4; ++row)
                (void)expected.rowCells(Row(row));
            for (int step = 0; step < 20; ++step) {
                const int row = step % 4;
                const int first = (step * 7) % columns;
                const std::string text(static_cast<std::size_t>(1 + step % 9), static_cast<char>('a' + step));
                actual.writeASCIIRun(Row(row), Column(first), defaultBlank(), text);
                expected.writeASCIIRun(Row(row), Column(first), defaultBlank(), text);
                CHECK(actual.compactASCIIRow(Row(row)).has_value());
                CHECK_EQ(actual.rowContentWidth(Row(row)), expected.rowContentWidth(Row(row)));
                CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
                for (int column = 0; column < columns; ++column) {
                    const auto cell = actual.logicalCellValueAt(Row(row), Column(column));
                    CHECK(cell.has_value());
                    CHECK(std::memcmp(&*cell, expected.cellAt(Row(row), Column(column)), sizeof(Cell)) == 0);
                }
                CHECK(actual.compactASCIIRow(Row(row)).has_value());
            }
            for (int row = 0; row < 4; ++row) {
                const auto cells = actual.rowCells(Row(row));
                CHECK(!actual.compactASCIIRow(Row(row)).has_value());
                CHECK(std::memcmp(cells.data, expected.rowCells(Row(row)).data, static_cast<std::size_t>(columns) * sizeof(Cell)) == 0);
            }
            CHECK(!actual.logicalCellValueAt(Row(-1), Column(0)).has_value());
            CHECK(!actual.logicalCellValueAt(Row(0), Column(columns)).has_value());
        }
    }
}

TEST(logicalCellReadsPreserveCompactRowsAndBorrowMaterializedCells)
{
    const CellRead emptyRead;
    CHECK(emptyRead.get() == nullptr);
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const int columns : { 1, 7, 31, 32, 33, 80 }) {
            for (const bool shouldMaterialize : { false, true }) {
                CellGrid grid;
                grid.init(4, columns);
                grid.setAlternateActive(screen);
                std::string text;
                for (int column = 0; column < columns; ++column)
                    text.push_back(static_cast<char>('a' + column % 26));
                grid.writeASCIIRun(Row(0), Column(0), defaultBlank(), text);
                grid.writeASCIIRun(Row(1), Column(0), defaultBlank(), "X");
                if (shouldMaterialize) {
                    (void)grid.rowCells(Row(0));
                    (void)grid.rowCells(Row(1));
                }
                const auto generation = grid.contentGeneration();
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < columns; ++column) {
                        const auto read = [&] {
                            auto original = grid.logicalCellReadAt(Row(row), Column(column));
                            auto copy = original;
                            auto moved = std::move(copy);
                            original = grid.logicalCellReadAt(Row(2), Column(0));
                            copy = grid.logicalCellReadAt(Row(2), Column(0));
                            return moved;
                        }();
                        const auto* cell = read.get();
                        CHECK(cell != nullptr);
                        if (!cell)
                            return;
                        Cell expected = defaultBlank();
                        expected.codePoint = row == 0 ? static_cast<std::uint32_t>(text[static_cast<std::size_t>(column)])
                                                      : (column == 0 ? 'X' : 0);
                        CHECK(std::memcmp(cell, &expected, sizeof(Cell)) == 0);
                        if (shouldMaterialize)
                            CHECK(cell == grid.logicalCellAt(Row(row), Column(column)));
                    }
                    CHECK(grid.compactASCIIRow(Row(row)).has_value() != shouldMaterialize);
                }
                CHECK_EQ(grid.contentGeneration(), generation);
                for (const auto& [invalidRow, invalidColumn] : { std::pair { -1, 0 }, std::pair { 4, 0 }, std::pair { 0, -1 }, std::pair { 0, columns } }) {
                    const auto invalidRead = grid.logicalCellReadAt(Row(invalidRow), Column(invalidColumn));
                    CHECK(invalidRead.get() == nullptr);
                }
                const auto owned = grid.logicalCellValueAt(Row(0), Column(0));
                Cell replacement = letter('Z');
                replacement.pen.backgroundIndex = 4;
                grid.setCellKnownDiff(Row(0), Column(0), replacement);
                CHECK(owned.has_value());
                CHECK_EQ(owned->codePoint, static_cast<std::uint32_t>('a'));
                const auto replacementRead = grid.logicalCellReadAt(Row(0), Column(0));
                CHECK_EQ(replacementRead.get()->codePoint, static_cast<std::uint32_t>('Z'));
            }
        }
    }
}

TEST(logicalCellReadsHonorPartialDeferredBlankWindows)
{
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const bool shouldMaterialize : { false, true }) {
            CellGrid grid;
            grid.init(4, 8);
            grid.setAlternateActive(screen);
            for (int row = 0; row < 4; ++row) {
                if (shouldMaterialize)
                    (void)grid.rowCells(Row(row));
                grid.writeASCIIRun(Row(row), Column(0), defaultBlank(), "abcdefgh");
            }
            grid.scrollFullViewportUpOneDefaultBlank();
            const auto blank = defaultBlank();
            for (int column = 0; column < 8; ++column) {
                const auto read = grid.logicalCellReadAt(Row(3), Column(column));
                CHECK(read.get() != nullptr);
                CHECK(std::memcmp(read.get(), &blank, sizeof(Cell)) == 0);
            }
            Cell first = letter('X');
            first.pen.backgroundIndex = 4;
            grid.setCellKnownDiff(Row(3), Column(0), first);
            const auto generation = grid.contentGeneration();
            for (int column = 0; column < 8; ++column) {
                const auto read = grid.logicalCellReadAt(Row(3), Column(column));
                const auto* expected = column == 0 ? &first : &blank;
                CHECK(read.get() != nullptr);
                CHECK(std::memcmp(read.get(), expected, sizeof(Cell)) == 0);
            }
            CHECK_EQ(grid.contentGeneration(), generation);
        }
    }
}

TEST(compactASCIIRowsKeepRingIdentityHiddenColumnsAndComplexFallbacks)
{
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        CellGrid actual;
        CellGrid expected;
        actual.init(4, 40);
        expected.init(4, 40);
        actual.setAlternateActive(screen);
        expected.setAlternateActive(screen);
        for (int row = 0; row < 4; ++row) {
            (void)expected.rowCells(Row(row));
            const std::string text(40, static_cast<char>('A' + row));
            actual.writeASCIIRun(Row(row), Column(0), defaultBlank(), text);
            expected.writeASCIIRun(Row(row), Column(0), defaultBlank(), text);
        }
        for (int step = 0; step < 7; ++step) {
            actual.scrollFullViewportUpOneDefaultBlank();
            expected.scrollFullViewportUpOneDefaultBlank();
            (void)expected.rowCells(Row(3));
            actual.writeASCIIRun(Row(3), Column(0), defaultBlank(), "new row");
            expected.writeASCIIRun(Row(3), Column(0), defaultBlank(), "new row");
            CHECK(actual.compactASCIIRow(Row(3)).has_value());
        }
        for (auto* grid : { &actual, &expected }) {
            grid->resize(4, 8, Modes::ReflowMode::Fixed);
            grid->writeASCIIRun(Row(3), Column(0), defaultBlank(), "changed");
            grid->resize(4, 40, Modes::ReflowMode::Fixed);
            Cell styled = letter('X');
            styled.pen.attributes.add(VT::Attribute::Blink);
            styled.setLinkID(9);
            grid->setCell(Row(1), Column(10), styled);
            grid->scrollRegion(Row(1), Row(3), 1, defaultBlank());
        }
        const auto other = screen == ScreenMode::Primary ? ScreenMode::Alternate : ScreenMode::Primary;
        actual.setAlternateActive(other);
        expected.setAlternateActive(other);
        for (int row = 0; row < 4; ++row) {
            const auto a = actual.rowCells(screen, Row(row));
            const auto b = expected.rowCells(screen, Row(row));
            CHECK(std::memcmp(a.data, b.data, 40 * sizeof(Cell)) == 0);
        }
    }
}

TEST(compactASCIIScalarBlanksKeepIdempotenceAndContentWidth)
{
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const int preparation : { 0, 1, 2 }) {
            CellGrid actual;
            CellGrid expected;
            for (auto* grid : { &actual, &expected }) {
                grid->init(2, 40);
                grid->setAlternateActive(screen);
                if (preparation) {
                    grid->writeASCIIRun(Row(0), Column(0), defaultBlank(), std::string(40, 'x'));
                    if (preparation == 2) {
                        (void)grid->rowCells(Row(0));
                        grid->scrollFullViewportUpOneDefaultBlank();
                    }
                }
            }
            const Row row(preparation == 2 ? 1 : 0);
            (void)expected.rowCells(row);
            const auto generation = actual.contentGeneration();
            actual.setCell(row, Column(39), defaultBlank());
            expected.setCell(row, Column(39), defaultBlank());
            CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
            CHECK_EQ(actual.rowContentWidth(row), expected.rowContentWidth(row));
            if (preparation != 1)
                CHECK_EQ(actual.contentGeneration(), generation);
            CHECK(std::memcmp(actual.rowCells(row).data, expected.rowCells(row).data, 40 * sizeof(Cell)) == 0);
        }
    }
}

TEST(compactASCIIByteViewsRemainValidInputsForOverlappingWrites)
{
    for (const int firstColumn : { -1, 0, 1, 3, 6 }) {
        for (const int style : { 0, 1 }) {
            CellGrid actual;
            CellGrid expected;
            actual.init(2, 8);
            expected.init(2, 8);
            const std::string original = "ABCDEFGH";
            actual.writeASCIIRun(Row(0), Column(0), defaultBlank(), original);
            expected.writeASCIIRun(Row(0), Column(0), defaultBlank(), original);
            const auto bytes = actual.compactASCIIRow(Row(0));
            REQUIRE(bytes);
            Cell cellTemplate = style ? styledBlank() : defaultBlank();
            actual.writeASCIIRun(Row(0), Column(firstColumn), cellTemplate,
                std::string_view(reinterpret_cast<const char*>(bytes->data()), 6));
            expected.writeASCIIRun(Row(0), Column(firstColumn), cellTemplate, std::string_view(original).substr(0, 6));
            CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
            CHECK_EQ(actual.rowContentWidth(Row(0)), expected.rowContentWidth(Row(0)));
            CHECK(std::memcmp(actual.rowCells(Row(0)).data, expected.rowCells(Row(0)).data, 8 * sizeof(Cell)) == 0);
        }
    }
}

TEST(compactASCIIHistoryAppendMatchesCellsAcrossEvictionFoldingAndRollback)
{
    LineStore actual;
    LineStore expected;
    actual.init(400);
    expected.init(400);
    CellGrid grid;
    grid.init(2, 80);
    std::array<Cell, 80> cells;
    for (int row = 0; row < 1300; ++row) {
        grid.scrollFullViewportUpOneDefaultBlank();
        const int extent = row % 11 == 0 ? 0 : 40 + row % 41;
        if (extent > 0)
            grid.writeASCIIRun(Row(1), Column(0), defaultBlank(), std::string(static_cast<std::size_t>(extent), static_cast<char>('a' + row % 26)));
        const auto text = grid.compactASCIIRow(Row(1));
        REQUIRE(text.has_value());
        for (int column = 0; column < 80; ++column) {
            cells[static_cast<std::size_t>(column)] = defaultBlank();
            if (column < extent)
                cells[static_cast<std::size_t>(column)].codePoint = static_cast<std::uint32_t>('a' + row % 26);
        }
        const int length = row % 3 == 0 ? 80 : extent;
        const bool isWrapped = row % 3 == 0;
        const auto mark = static_cast<std::uint8_t>(row % 4);
        if (row % 7 == 0)
            actual.append(cells.data(), length, 0, mark, isWrapped, false);
        else
            actual.appendDefaultASCII(*text, length, 0, mark, isWrapped, false);
        CHECK(grid.compactASCIIRow(Row(1)).has_value());
        expected.append(cells.data(), length, 0, mark, isWrapped, false);
        if (row == 700) {
            actual.dropNewest(19);
            expected.dropNewest(19);
        }
    }
    (void)actual.foldColdBlocks(100);
    (void)expected.foldColdBlocks(100);
    CHECK(actual.foldedBlockCount() > 0);
    actual.dropOldest(17);
    expected.dropOldest(17);
    actual.dropNewest(23);
    expected.dropNewest(23);
    CHECK_EQ(actual.count(), expected.count());
    CHECK_EQ(actual.totalAppended(), expected.totalAppended());
    CHECK_EQ(actual.headLineNumber(), expected.headLineNumber());
    CHECK_EQ(actual.frontLineNumber(), expected.frontLineNumber());
    for (int row = 0; row < actual.count(); ++row) {
        const auto* a = actual.at(row);
        const auto* b = expected.at(row);
        CHECK_EQ(a->length, b->length);
        CHECK_EQ(a->wrapped, b->wrapped);
        CHECK_EQ(a->mark, b->mark);
        if (a->length > 0)
            CHECK(std::memcmp(a->cells, b->cells, static_cast<std::size_t>(a->length) * sizeof(Cell)) == 0);
    }
    const auto count = actual.totalAppended();
    const std::array<std::uint8_t, 2> invalid { 'x', 'y' };
    actual.appendDefaultASCII(invalid, 1, 0, 0, false, false);
    CHECK_EQ(actual.totalAppended(), count);
}

TEST(wideCodePointRunsReplaceWholeRowsAndRejectPartialPairs)
{
    CellGrid grid;
    grid.init(2, 8);
    Cell joiner = defaultBlank();
    joiner.codePoint = 0x200D;
    grid.setCell(Row(0), Column(7), joiner);
    CHECK(grid.rowMeta(0).has(RowFlag::NeedsShaping));
    const std::array<uint32_t, 4> codePoints { 0x4E00, 0x754C, 0xAC00, 0x20000 };
    grid.writeWideCodePointRun(Row(0), Column(0), defaultBlank(), codePoints);
    CHECK(!grid.rowMeta(0).has(RowFlag::NeedsShaping));
    CHECK_EQ(grid.rowContentWidth(Row(0)), 8);
    CHECK_EQ(grid.logicalCellAt(Row(0), Column(6))->codePoint, 0x20000u);
    const auto generation = grid.contentGeneration();
    for (const int column : { -1, 1, 7, 8 })
        grid.writeWideCodePointRun(Row(0), Column(column), defaultBlank(), codePoints);
    grid.writeWideCodePointRun(Row(-1), Column(0), defaultBlank(), codePoints);
    grid.writeWideCodePointRun(Row(2), Column(0), defaultBlank(), codePoints);
    CHECK_EQ(grid.contentGeneration(), generation);
}

TEST(narrowCodePointRunsMatchSpansAndPreserveIdempotence)
{
    const std::array<uint32_t, 8> letters { 0xE9, 0xF8, 0x1FB00, 0x20, 0xE0, 0xDF, 0x2500, 0x1FB3A };
    const std::array<uint32_t, 8> spaces { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
    for (const auto& codePoints : { letters, spaces }) {
        for (const int firstColumn : { 0, 2 }) {
            for (const int count : { 2, 4, 8 }) {
                if (count > 8 - firstColumn)
                    continue;
                for (const int preparation : { 0, 1, 2 }) {
                    for (const int style : { 0, 1 }) {
                        CellGrid actual;
                        CellGrid expected;
                        actual.init(4, 8);
                        expected.init(4, 8);
                        for (auto* grid : { &actual, &expected }) {
                            writeWord(*grid, 0, "abcdefgh");
                            Cell joiner = defaultBlank();
                            joiner.codePoint = 0x200D;
                            grid->setCell(Row(3), Column(7), joiner);
                            if (preparation) {
                                grid->scrollRegion(Row(0), Row(3), 1, defaultBlank());
                                if (preparation == 2)
                                    grid->setCellKnownDiff(Row(3), Column(0), letter('X'));
                            }
                        }
                        Cell cellTemplate = defaultBlank();
                        if (style) {
                            cellTemplate.pen.foregroundIndex = 2;
                            cellTemplate.pen.backgroundIndex = 4;
                            cellTemplate.pen.attributes.add(VT::Attribute::Blink);
                            cellTemplate.pen.extendedAttributes.setTextSizeSpecID(1);
                            cellTemplate.setLinkID(7);
                        }
                        std::array<Cell, 8> cells;
                        for (int index = 0; index < count; ++index) {
                            cells[index] = cellTemplate;
                            cells[index].codePoint = codePoints[index];
                        }
                        const auto run = std::span(codePoints).first(static_cast<std::size_t>(count));
                        actual.writeNarrowCodePointRun(Row(3), Column(firstColumn), cellTemplate, run);
                        expected.setRowSpan(Row(3), Column(firstColumn), { cells.data(), count });
                        for (int column = 0; column < 8; ++column)
                            CHECK(std::memcmp(actual.logicalCellAt(Row(3), Column(column)), expected.logicalCellAt(Row(3), Column(column)), sizeof(Cell)) == 0);
                        CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
                        CHECK_EQ(actual.rowGeneration(3), expected.rowGeneration(3));
                        CHECK_EQ(actual.rowContentWidth(Row(3)), expected.rowContentWidth(Row(3)));
                        CHECK_EQ(actual.hasSizedCellHint(), expected.hasSizedCellHint());
                        CHECK_EQ(actual.blinkHintGeneration(), expected.blinkHintGeneration());
                        CHECK(actual.rowMeta(3).has(RowFlag::NeedsShaping) == expected.rowMeta(3).has(RowFlag::NeedsShaping));
                        CHECK(actual.rowMeta(3).has(RowFlag::HasMulticell) == expected.rowMeta(3).has(RowFlag::HasMulticell));
                        const auto generation = actual.contentGeneration();
                        actual.writeNarrowCodePointRun(Row(3), Column(firstColumn), cellTemplate, run);
                        CHECK_EQ(actual.contentGeneration(), generation);
                    }
                }
            }
        }
    }
}

TEST(narrowCodePointRunsOwnTheirTemplateAndRejectInvalidGeometry)
{
    CellGrid grid;
    grid.init(2, 8);
    Cell styled = letter('x');
    styled.pen.foregroundIndex = 3;
    styled.setLinkID(11);
    grid.setCell(Row(0), Column(0), styled);
    Cell joiner = defaultBlank();
    joiner.codePoint = 0x200D;
    grid.setCell(Row(0), Column(7), joiner);
    CHECK(grid.rowMeta(0).has(RowFlag::NeedsShaping));
    const std::array<uint32_t, 8> codePoints { 0xE9, 0xF8, 0x1FB00, 0x20, 0xE0, 0xDF, 0x2500, 0x1FB3A };
    grid.writeNarrowCodePointRun(Row(0), Column(0), *grid.cellAt(Row(0), Column(0)), codePoints);
    CHECK(!grid.rowMeta(0).has(RowFlag::NeedsShaping));
    CHECK_EQ(grid.rowContentWidth(Row(0)), 8);
    for (int column = 0; column < 8; ++column) {
        styled.codePoint = codePoints[column];
        CHECK(std::memcmp(grid.logicalCellAt(Row(0), Column(column)), &styled, sizeof(Cell)) == 0);
    }
    const auto generation = grid.contentGeneration();
    for (const int column : { -1, 1, 7, 8 })
        grid.writeNarrowCodePointRun(Row(0), Column(column), defaultBlank(), codePoints);
    grid.writeNarrowCodePointRun(Row(-1), Column(0), defaultBlank(), codePoints);
    grid.writeNarrowCodePointRun(Row(2), Column(0), defaultBlank(), codePoints);
    grid.writeNarrowCodePointRun(Row(0), Column(0), defaultBlank(), {});
    CHECK_EQ(grid.contentGeneration(), generation);
}

TEST(narrowCodePointRunsKeepDeferredTemplatesAndRetainedTails)
{
    const std::array<uint32_t, 2> codePoints { 0xE9, 0xF8 };
    for (const int firstColumn : { 1, 3, 5 }) {
        CellGrid actual;
        CellGrid expected;
        actual.init(4, 10);
        expected.init(4, 10);
        for (auto* grid : { &actual, &expected }) {
            writeWord(*grid, 0, "abcdefghij");
            grid->resize(4, 4, Modes::ReflowMode::Fixed);
            grid->eraseRowRange(Row(0), Column(0), Column(3), defaultBlank());
            writeWord(*grid, 0, "abcd");
            grid->resize(4, 10, Modes::ReflowMode::Fixed);
            grid->scrollRegion(Row(0), Row(3), 1, defaultBlank());
            grid->setCellKnownDiff(Row(3), Column(0), letter('X'));
        }
        const Cell cellTemplate = styledBlank();
        std::array<Cell, 2> cells { cellTemplate, cellTemplate };
        cells[0].codePoint = codePoints[0];
        cells[1].codePoint = codePoints[1];
        actual.writeNarrowCodePointRun(Row(3), Column(firstColumn), cellTemplate, codePoints);
        expected.setRowSpan(Row(3), Column(firstColumn), { cells.data(), 2 });
        for (int column = 0; column < 10; ++column)
            CHECK(std::memcmp(actual.logicalCellAt(Row(3), Column(column)), expected.logicalCellAt(Row(3), Column(column)), sizeof(Cell)) == 0);
        CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
        CHECK_EQ(actual.rowContentWidth(Row(3)), expected.rowContentWidth(Row(3)));
    }

    for (const int templateColumn : { 0, 7 }) {
        CellGrid grid;
        grid.init(4, 8);
        Cell cellTemplate = styledBlank();
        cellTemplate.codePoint = 'x';
        cellTemplate.setLinkID(11);
        grid.setCell(Row(0), Column(templateColumn), cellTemplate);
        const Cell* borrowedTemplate = grid.cellAt(Row(0), Column(templateColumn));
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        // The borrowed cell is in a stale flank that the writer materializes.
        grid.writeNarrowCodePointRun(Row(3), Column(2), *borrowedTemplate, codePoints);
        for (int index = 0; index < 2; ++index) {
            cellTemplate.codePoint = codePoints[index];
            CHECK(std::memcmp(grid.logicalCellAt(Row(3), Column(index + 2)), &cellTemplate, sizeof(Cell)) == 0);
        }
    }
}

TEST(testSequentialSpansConsumeADeferredBlankWithoutLosingLiveCells)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());

    Cell lead = letter('W');
    lead.pen.attributes.add(VT::Attribute::Wide);
    lead.combining[0] = 0x0301;
    lead.setCombiningLength(1);
    Cell continuation = defaultBlank();
    continuation.pen.attributes.add(VT::Attribute::WideContinuation);
    Cell first[2] = { lead, continuation };
    grid.clearWideOverlapEdges(Row(3), Column(0), Column(1), defaultBlank());
    grid.setRowSpan(Row(3), Column(0), RowView { first, 2 });
    CHECK(grid.logicalCellAt(Row(3), Column(0))->combining[0] == 0x0301u);
    CHECK(grid.logicalCellAt(Row(3), Column(2))->codePoint == 0u);

    Cell second[2] = { letter('x'), letter('y') };
    grid.clearWideOverlapEdges(Row(3), Column(2), Column(3), defaultBlank());
    grid.setRowSpan(Row(3), Column(2), RowView { second, 2 });
    lead.combining[0] = 0x0302;
    grid.setCellKnownDiff(Row(3), Column(0), lead);
    CHECK(grid.logicalCellAt(Row(3), Column(0))->combining[0] == 0x0302u);
    CHECK(grid.logicalCellAt(Row(3), Column(3))->codePoint == 'y');
    CHECK(grid.logicalCellAt(Row(3), Column(9))->codePoint == 0u);
    const auto generation = grid.contentGeneration();
    Cell tail[6] = { defaultBlank(), defaultBlank(), defaultBlank(), defaultBlank(), defaultBlank(), defaultBlank() };
    grid.setRowSpan(Row(3), Column(4), RowView { tail, 6 });
    CHECK(grid.contentGeneration() == generation);
    CHECK(grid.rowContentWidth(Row(3)) == 4);
    CHECK(rowText(grid, 3) == "W.xy......");
}

TEST(testDeferredBlankSpanWritesStayIdempotent)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
    const auto generation = grid.contentGeneration();
    Cell blanks[2] = { defaultBlank(), defaultBlank() };
    grid.setRowSpan(Row(3), Column(0), RowView { blanks, 2 });
    grid.setRowSpan(Row(3), Column(2), RowView { blanks, 2 });
    CHECK(grid.contentGeneration() == generation);
    CHECK(grid.rowContentWidth(Row(3)) == 0);
    CHECK(grid.logicalCellAt(Row(3), Column(0))->codePoint == 0u);
    CHECK(grid.logicalCellAt(Row(3), Column(9))->codePoint == 0u);

    // Revisiting the live prefix must compare its blank cells normally.
    grid.setRowSpan(Row(3), Column(0), RowView { blanks, 2 });
    CHECK(grid.contentGeneration() == generation);
    CHECK(grid.rowContentWidth(Row(3)) == 0);
    CHECK(rowText(grid, 3) == "..........");
}

TEST(testASCIIRunsAfterDeferredPrefixMatchSpans)
{
    const std::string text = "abcd";
    std::array<std::uint16_t, 95> translation;
    for (std::size_t index = 0; index < translation.size(); ++index)
        translation[index] = static_cast<std::uint16_t>(0x0120 + index);
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const int firstColumn : { -2, 0, 1, 2, 4, 8 }) {
            for (const int style : { 0, 1 }) {
                for (const bool isTranslated : { false, true }) {
                    CellGrid actual;
                    CellGrid expected;
                    Cell prefix = letter('W');
                    prefix.pen.backgroundIndex = 5;
                    prefix.pen.attributes.add(VT::Attribute::Wide);
                    prefix.combining[0] = 0x0301;
                    prefix.setCombiningLength(1);
                    prefix.setLinkID(9);
                    Cell continuation = defaultBlank();
                    continuation.pen.attributes.add(VT::Attribute::WideContinuation);
                    for (auto* grid : { &actual, &expected }) {
                        grid->init(4, 10);
                        grid->setAlternateActive(screen);
                        writeWord(*grid, 0, "abcdefghij");
                        grid->scrollRegion(Row(0), Row(3), 1, defaultBlank());
                        grid->writeWideCell(Row(3), Column(0), prefix, continuation, defaultBlank());
                    }
                    Cell cellTemplate = defaultBlank();
                    if (style) {
                        cellTemplate.pen.foregroundIndex = 2;
                        cellTemplate.pen.backgroundIndex = 4;
                        cellTemplate.pen.attributes.add(VT::Attribute::Blink);
                        cellTemplate.pen.extendedAttributes.setTextSizeSpecID(1);
                        cellTemplate.setLinkID(7);
                    }
                    std::array<Cell, 4> cells;
                    for (std::size_t index = 0; index < text.size(); ++index) {
                        cells[index] = cellTemplate;
                        cells[index].codePoint = isTranslated ? translation[text[index] - 0x20] : static_cast<std::uint32_t>(text[index]);
                    }
                    const auto writeRun = [&] {
                        if (isTranslated)
                            actual.writeTranslatedRun(Row(3), Column(firstColumn), cellTemplate, text, translation.data());
                        else
                            actual.writeASCIIRun(Row(3), Column(firstColumn), cellTemplate, text);
                    };
                    writeRun();
                    expected.setRowSpan(Row(3), Column(firstColumn), { cells.data(), static_cast<int>(cells.size()) });
                    for (int column = 0; column < 10; ++column)
                        CHECK(std::memcmp(actual.logicalCellAt(Row(3), Column(column)), expected.logicalCellAt(Row(3), Column(column)), sizeof(Cell)) == 0);
                    CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
                    CHECK_EQ(actual.rowGeneration(3), expected.rowGeneration(3));
                    CHECK_EQ(actual.rowContentWidth(Row(3)), expected.rowContentWidth(Row(3)));
                    CHECK_EQ(actual.hasSizedCellHint(), expected.hasSizedCellHint());
                    CHECK_EQ(actual.blinkHintGeneration(), expected.blinkHintGeneration());
                    const auto generation = actual.contentGeneration();
                    writeRun();
                    CHECK_EQ(actual.contentGeneration(), generation);
                }
            }
        }
    }
}

TEST(testASCIIRunsAfterDeferredPrefixPreserveRetainedColumns)
{
    for (const int firstColumn : { 1, 3, 5 }) {
        CellGrid grid;
        grid.init(4, 10);
        writeWord(grid, 0, "abcdefghij");
        grid.resize(4, 4, Modes::ReflowMode::Fixed);
        grid.eraseRowRange(Row(0), Column(0), Column(3), defaultBlank());
        writeWord(grid, 0, "abcd");
        grid.resize(4, 10, Modes::ReflowMode::Fixed);
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        grid.setCellKnownDiff(Row(3), Column(0), letter('X'));
        const auto generation = grid.contentGeneration();

        // Only columns [1, 4) are still deferred; the old fixed-width tail is live.
        grid.writeASCIIRun(Row(3), Column(firstColumn), styledBlank(), "xy");
        std::string expected = "X...efghij";
        expected.replace(static_cast<std::size_t>(firstColumn), 2, "xy");
        CHECK(rowText(grid, 3) == expected);
        CHECK_EQ(grid.contentGeneration(), generation + 1);
        CHECK_EQ(grid.rowContentWidth(Row(3)), firstColumn + 2);
        for (int column = 0; column < 10; ++column) {
            const auto* cell = grid.logicalCellAt(Row(3), Column(column));
            CHECK_EQ(cell->pen.backgroundIndex, column >= firstColumn && column < firstColumn + 2 ? 4 : colorDefault);
        }
    }
}

TEST(testASCIIRunsAfterDeferredPrefixBlankHiddenStaleColumns)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.resize(4, 6, Modes::ReflowMode::Fixed);
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
    grid.setCellKnownDiff(Row(3), Column(0), letter('X'));
    grid.writeASCIIRun(Row(3), Column(2), defaultBlank(), "abcdef");
    CHECK(rowText(grid, 3) == "X.abcd");
    grid.resize(4, 10, Modes::ReflowMode::Fixed);
    CHECK(rowText(grid, 3) == "X.abcd....");
    CHECK_EQ(grid.rowContentWidth(Row(3)), 6);
}

TEST(testSpanCrossingADeferredPrefixComparesLogicalCells)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
    grid.setCellKnownDiff(Row(3), Column(0), letter('X'));
    const auto generation = grid.contentGeneration();

    // These bytes match physical storage, but b/c replace logical blanks.
    // Treating the mixed span as an unchanged repaint loses its damage and width.
    Cell span[3] = { letter('X'), letter('b'), letter('c') };
    grid.setRowSpan(Row(3), Column(0), RowView { span, 3 });
    CHECK(grid.contentGeneration() == generation + 1);
    CHECK(grid.rowContentWidth(Row(3)) == 3);
    CHECK(rowText(grid, 3) == "Xbc.......");
    grid.setRowSpan(Row(3), Column(0), RowView { span, 3 });
    CHECK(grid.contentGeneration() == generation + 1);
}

TEST(testSpanPastADeferredWindowComparesRetainedFixedWidthContent)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.resize(4, 4, Modes::ReflowMode::Fixed);
    grid.eraseRowRange(Row(0), Column(0), Column(3), defaultBlank());
    writeWord(grid, 0, "ab");
    grid.resize(4, 10, Modes::ReflowMode::Fixed);
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
    CHECK(grid.logicalCellAt(Row(3), Column(0))->codePoint == 0u);
    CHECK(grid.logicalCellAt(Row(3), Column(5))->codePoint == 'f');
    const auto generation = grid.contentGeneration();

    Cell blanks[2] = { defaultBlank(), defaultBlank() };
    grid.setRowSpan(Row(3), Column(5), RowView { blanks, 2 });
    CHECK(grid.contentGeneration() == generation + 1);
    CHECK(grid.rowContentWidth(Row(3)) == 7);
    CHECK(rowText(grid, 3) == "....e..hij");
}

TEST(testWideOverlapAtADeferredPrefixStillClearsTheLivePartner)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcdefghij");
    grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
    Cell lead = letter('W');
    lead.pen.attributes.add(VT::Attribute::Wide);
    Cell continuation = defaultBlank();
    continuation.pen.attributes.add(VT::Attribute::WideContinuation);
    Cell wide[2] = { lead, continuation };
    grid.setRowSpan(Row(3), Column(0), RowView { wide, 2 });

    grid.clearWideOverlapEdges(Row(3), Column(1), Column(2), defaultBlank());
    Cell replacement[2] = { letter('x'), letter('y') };
    grid.setRowSpan(Row(3), Column(1), RowView { replacement, 2 });
    CHECK(rowText(grid, 3) == ".xy.......");
    CHECK(!grid.cellAt(Row(3), Column(0))->pen.attributes.contains(VT::Attribute::Wide));
}

TEST(testWideOverlapPastADeferredWindowClearsRetainedPartners)
{
    for (int column : { 6, 7, 8 }) {
        CellGrid grid;
        grid.init(4, 10);
        writeWord(grid, 0, "abcdefghij");
        Cell lead = letter('W');
        lead.pen.attributes.add(VT::Attribute::Wide);
        Cell continuation = defaultBlank();
        continuation.pen.attributes.add(VT::Attribute::WideContinuation);
        Cell wide[3] = { lead, continuation, continuation };
        grid.setRowSpan(Row(0), Column(6), RowView { wide, 3 });
        grid.resize(4, 4, Modes::ReflowMode::Fixed);
        grid.eraseRowRange(Row(0), Column(0), Column(3), defaultBlank());
        writeWord(grid, 0, "ab");
        grid.resize(4, 10, Modes::ReflowMode::Fixed);
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        const auto generation = grid.contentGeneration();

        // The deferred prefix is blank; this chain beyond it is live. A write
        // on its lead, middle or final cell must clear every partner outside it.
        grid.clearWideOverlapEdges(Row(3), Column(column), Column(column), defaultBlank());
        CHECK(grid.contentGeneration() > generation);
        for (int partner : { 6, 7, 8 }) {
            if (partner != column) {
                const Cell* cell = grid.logicalCellAt(Row(3), Column(partner));
                CHECK(cell->codePoint == 0u);
                CHECK(!cell->pen.attributes.containsAny({ VT::Attribute::Wide, VT::Attribute::WideContinuation }));
            }
        }
        CHECK(grid.logicalCellAt(Row(3), Column(0))->codePoint == 0u);
        Cell replacement[1] = { letter('x') };
        grid.setRowSpan(Row(3), Column(column), RowView { replacement, 1 });
        CHECK(grid.cellAt(Row(3), Column(column))->codePoint == 'x');
    }
}

TEST(testFullWidthOverlapLeavesWideFragmentsAndBlankStylesUnchanged)
{
    CellGrid grid;
    grid.init(4, 6);
    Cell lead = letter('W');
    lead.pen.attributes.add(VT::Attribute::Wide);
    Cell continuation = defaultBlank();
    continuation.pen.attributes.add(VT::Attribute::WideContinuation);
    Cell cells[6] = { continuation, letter('a'), lead, continuation, letter('b'), lead };
    grid.setRowSpan(Row(0), Column(0), RowView { cells, 6 });
    const auto generation = grid.contentGeneration();

    // The caller owns the complete row, including its malformed edge fragments.
    // No partner outside it can be erased or claim width under either blank style.
    for (const auto& blank : { defaultBlank(), styledBlank() }) {
        grid.clearWideOverlapEdges(Row(0), Column(0), Column(5), blank);
        CHECK(grid.contentGeneration() == generation);
        CHECK(grid.rowContentWidth(Row(0)) == 6);
        for (int column = 0; column < 6; ++column) {
            const Cell* cell = grid.cellAt(Row(0), Column(column));
            CHECK(cell->codePoint == cells[column].codePoint);
            CHECK(cell->pen.attributes == cells[column].pen.attributes);
            CHECK(cell->pen.backgroundIndex == cells[column].pen.backgroundIndex);
        }
    }
}

TEST(testScrollDownRecyclesWithBothBlankKinds)
{
    // n = -1 over the full viewport is the reverse-index fast path: the top
    // row is revealed, everything else moves down, the bottom falls off.
    CellGrid plain = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    plain.scrollRegion(Row(0), Row(4), -1, defaultBlank());
    CHECK(rowText(plain, 0) == "........");
    CHECK(rowText(plain, 1) == "aaaa....");
    CHECK(rowText(plain, 4) == "dddd....");

    // The styled blank takes the immediate-fill arm rather than the deferral:
    // the revealed row carries the blank's background on every cell.
    CellGrid styled = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    styled.scrollRegion(Row(0), Row(4), -1, styledBlank());
    CHECK(rowText(styled, 0) == "........");
    CHECK(styled.cellAt(Row(0), Column(7))->pen.backgroundIndex == 4);
    CHECK(rowText(styled, 1) == "aaaa....");

    // The same pair upward: n = 1 with the styled blank fills the revealed
    // bottom row eagerly.
    CellGrid up = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    up.scrollRegion(Row(0), Row(4), 1, styledBlank());
    CHECK(rowText(up, 0) == "bbbb....");
    CHECK(rowText(up, 3) == "eeee....");
    CHECK(rowText(up, 4) == "........");
    CHECK(up.cellAt(Row(4), Column(0))->pen.backgroundIndex == 4);
}

TEST(testScrollRegionDegeneratesAndFullClears)
{
    // A zero shift changes nothing. (Out-of-range margins are an asserted
    // precondition — the terminal clamps before calling — so they are not
    // probed here.)
    CellGrid grid = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    grid.scrollRegion(Row(0), Row(4), 0, defaultBlank());
    CHECK(rowText(grid, 0) == "aaaa....");
    CHECK(rowText(grid, 4) == "eeee....");

    // A shift at least the region's height clears it whole, both directions.
    grid.scrollRegion(Row(1), Row(3), 9, defaultBlank());
    CHECK(rowText(grid, 0) == "aaaa....");
    CHECK(rowText(grid, 1) == "........");
    CHECK(rowText(grid, 2) == "........");
    CHECK(rowText(grid, 3) == "........");
    CHECK(rowText(grid, 4) == "eeee....");

    CellGrid down = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    down.scrollRegion(Row(1), Row(3), -9, styledBlank());
    CHECK(rowText(down, 1) == "........");
    CHECK(down.cellAt(Row(2), Column(0))->pen.backgroundIndex == 4);
    CHECK(rowText(down, 4) == "eeee....");
}

TEST(testRegionScrollBothDirections)
{
    // A margin region (not the full viewport) moves only its own rows.
    CellGrid grid = grid5x8({ "aaaa", "bbbb", "cccc", "dddd", "eeee" });
    grid.scrollRegion(Row(1), Row(3), 1, defaultBlank());
    CHECK(rowText(grid, 0) == "aaaa....");
    CHECK(rowText(grid, 1) == "cccc....");
    CHECK(rowText(grid, 2) == "dddd....");
    CHECK(rowText(grid, 3) == "........");
    CHECK(rowText(grid, 4) == "eeee....");

    grid.scrollRegion(Row(1), Row(3), -2, defaultBlank());
    CHECK(rowText(grid, 1) == "........");
    CHECK(rowText(grid, 2) == "........");
    CHECK(rowText(grid, 3) == "cccc....");
    CHECK(rowText(grid, 4) == "eeee....");
}

TEST(testASCIIRunsReplaceMixedTailsWithoutLosingIdempotence)
{
    const std::string text = "abcdef";
    std::array<std::uint16_t, 95> translation;
    for (std::size_t index = 0; index < translation.size(); ++index)
        translation[index] = static_cast<std::uint16_t>(0x0120 + index);
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const int style : { 0, 1 }) {
            for (const bool isTranslated : { false, true }) {
                for (const int firstCellMatch : { 0, 1, 2 }) {
                    Cell cellTemplate = defaultBlank();
                    if (style) {
                        cellTemplate.pen.foregroundIndex = 3;
                        cellTemplate.pen.backgroundIndex = 4;
                        cellTemplate.pen.attributes.add(VT::Attribute::Blink);
                        cellTemplate.pen.extendedAttributes.setTextSizeSpecID(1);
                        cellTemplate.setLinkID(12);
                    }
                    std::array<Cell, 6> desired;
                    for (std::size_t index = 0; index < text.size(); ++index) {
                        desired[index] = cellTemplate;
                        const auto byte = static_cast<unsigned char>(text[index]);
                        desired[index].codePoint = isTranslated ? translation[byte - 0x20] : byte;
                    }
                    std::array<Cell, 10> original;
                    for (std::size_t index = 0; index < original.size(); ++index) {
                        original[index] = letter('Z');
                        original[index].pen.backgroundIndex = static_cast<std::uint8_t>(index);
                        original[index].setLinkID(static_cast<std::uint16_t>(index + 1));
                        original[index].combining[0] = 0x0301;
                        original[index].setCombiningLength(1);
                    }
                    // Exercise a changed code point with the same tail, a different
                    // tail, and a matching first cell followed by later differences.
                    original[1] = desired[0];
                    if (firstCellMatch != 2)
                        original[1].codePoint = '?';
                    if (firstCellMatch == 1)
                        original[1].pen.foregroundIndex = 9;
                    CellGrid actual;
                    CellGrid expected;
                    for (auto* grid : { &actual, &expected }) {
                        grid->init(2, 10);
                        grid->setAlternateActive(screen);
                        grid->setRowSpan(Row(1), Column(0), { original.data(), static_cast<int>(original.size()) });
                        grid->clearBlinkHintIfClean();
                    }
                    const auto writeRun = [&] {
                        if (isTranslated)
                            actual.writeTranslatedRun(Row(1), Column(1), cellTemplate, text, translation.data());
                        else
                            actual.writeASCIIRun(Row(1), Column(1), cellTemplate, text);
                    };
                    writeRun();
                    expected.setRowSpan(Row(1), Column(1), { desired.data(), static_cast<int>(desired.size()) });
                    for (int column = 0; column < 10; ++column)
                        CHECK(std::memcmp(actual.logicalCellAt(Row(1), Column(column)), expected.logicalCellAt(Row(1), Column(column)), sizeof(Cell)) == 0);
                    CHECK_EQ(actual.contentGeneration(), expected.contentGeneration());
                    CHECK_EQ(actual.rowGeneration(1), expected.rowGeneration(1));
                    CHECK_EQ(actual.rowContentWidth(Row(1)), expected.rowContentWidth(Row(1)));
                    CHECK_EQ(actual.hasBlinkHint(), expected.hasBlinkHint());
                    CHECK_EQ(actual.blinkHintGeneration(), expected.blinkHintGeneration());
                    CHECK_EQ(actual.hasSizedCellHint(), expected.hasSizedCellHint());
                    actual.clearBlinkHintIfClean();
                    const auto generation = actual.contentGeneration();
                    writeRun();
                    CHECK_EQ(actual.contentGeneration(), generation);
                    CHECK(!actual.hasBlinkHint());
                }
            }
        }
    }
}

TEST(testASCIIRunTemplateCanAliasItsDestination)
{
    for (const int templateColumn : { 1, 4, 6 }) {
        CellGrid grid;
        grid.init(2, 8);
        Cell cellTemplate = letter('T');
        cellTemplate.pen.foregroundIndex = 3;
        cellTemplate.pen.backgroundIndex = 4;
        cellTemplate.pen.attributes.add(VT::Attribute::Blink);
        cellTemplate.setLinkID(17);
        cellTemplate.combining[0] = 0x0301;
        cellTemplate.setCombiningLength(1);
        grid.setCell(Row(1), Column(templateColumn), cellTemplate);
        const Cell* borrowedTemplate = grid.logicalCellAt(Row(1), Column(templateColumn));
        CHECK(borrowedTemplate != nullptr);
        grid.writeASCIIRun(Row(1), Column(1), *borrowedTemplate, "abcdef");
        for (int index = 0; index < 6; ++index) {
            Cell expected = cellTemplate;
            expected.codePoint = static_cast<std::uint32_t>('a' + index);
            CHECK(std::memcmp(grid.logicalCellAt(Row(1), Column(index + 1)), &expected, sizeof(Cell)) == 0);
        }
        CHECK_EQ(grid.logicalCellAt(Row(1), Column(0))->codePoint, 0u);
        CHECK_EQ(grid.logicalCellAt(Row(1), Column(7))->codePoint, 0u);
        CHECK_EQ(grid.rowContentWidth(Row(1)), 7);
        CHECK(grid.hasBlinkHint());
    }
}

TEST(testWritersClampToTheGrid)
{
    CellGrid grid;
    grid.init(3, 6);

    // An ASCII run starting left of the grid keeps its on-grid tail.
    grid.writeASCIIRun(Row(0), Column(-2), defaultBlank(), "abcdef");
    CHECK(rowText(grid, 0) == "cdef..");
    // One starting past the right edge writes nothing.
    grid.writeASCIIRun(Row(0), Column(6), defaultBlank(), "zz");
    CHECK(rowText(grid, 0) == "cdef..");
    // One overrunning the right edge is clipped there.
    grid.writeASCIIRun(Row(1), Column(4), defaultBlank(), "wxyz");
    CHECK(rowText(grid, 1) == "....wx");
    // A shift past the whole text writes nothing.
    grid.writeASCIIRun(Row(2), Column(-9), defaultBlank(), "ab");
    CHECK(rowText(grid, 2) == "......");
    // An off-grid row is dropped.
    grid.writeASCIIRun(Row(9), Column(0), defaultBlank(), "ab");

    // The span writer clamps the same four ways.
    Cell span[6] = { letter('p'), letter('q'), letter('r'), letter('s'), letter('t'), letter('u') };
    grid.setRowSpan(Row(2), Column(-2), RowView { span, 6 });
    CHECK(rowText(grid, 2) == "rstu..");
    grid.setRowSpan(Row(2), Column(4), RowView { span, 6 });
    CHECK(rowText(grid, 2) == "rstupq");
    grid.setRowSpan(Row(2), Column(-8), RowView { span, 6 });
    CHECK(rowText(grid, 2) == "rstupq");

    // The screen-addressed span writer takes the same clamps for a screen
    // that is not the active one.
    grid.setRowSpan(ScreenMode::Alternate, Row(1), Column(-2), RowView { span, 6 });
    grid.setRowSpan(ScreenMode::Alternate, Row(1), Column(5), RowView { span, 6 });
    grid.setAlternateActive(ScreenMode::Alternate);
    CHECK(rowText(grid, 1) == "rstu.p");
    grid.setAlternateActive(ScreenMode::Primary);

    // Fills and erases with degenerate ranges change nothing.
    grid.fillCellRun(Row(1), Column(4), letter('m'), 9);
    CHECK(rowText(grid, 1) == "....mm");
    grid.fillCellRun(Row(1), Column(2), letter('n'), 0);
    CHECK(rowText(grid, 1) == "....mm");
    grid.eraseRowRange(Row(1), Column(4), Column(9), defaultBlank());
    CHECK(rowText(grid, 1) == "......");
}

UBIQUITTY_TEST_MAIN()

// ---- per-row bookkeeping and its off-grid guards ----------------------
// Every one of these takes a raw row index from a caller that may be a frame
// behind the grid: a resize between a frontend reading a row count and asking
// about a row is ordinary, so each entry has to answer rather than index.

TEST(testRowBookkeepingAnswersOffGridRows)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "abcd");

    // Content width: on-grid rows report what was written; an off-grid row
    // reports the full width rather than reading past the ring.
    CHECK(grid.rowContentWidth(Row(0)) == 4);
    CHECK(grid.rowContentWidth(Row(-1)) == grid.columns());
    CHECK(grid.rowContentWidth(Row(4)) == grid.columns());
    CHECK(grid.rowContentWidth(Row(9999)) == grid.columns());

    // The metadata setters drop an off-grid row instead of writing one.
    grid.setLineAttribute(-1, lineAttributeDoubleWidth);
    grid.setLineAttribute(9999, lineAttributeDoubleWidth);
    grid.setRowWrapped(-1, true);
    grid.setRowWrapped(9999, true);
    grid.setRowMark(-1, 3);
    grid.setRowMark(9999, 3);
    grid.resetRowMeta(-1);
    grid.resetRowMeta(9999);
    // The on-grid rows they might have corrupted are untouched.
    for (int r = 0; r < grid.rows(); ++r) {
        CHECK(grid.rowMeta(r).lineAttribute == lineAttributeNormal);
        CHECK(!grid.rowMeta(r).has(RowFlag::Wrapped));
        CHECK(grid.rowMeta(r).oscMark == 0);
    }
    CHECK(rowText(grid, 0) == "abcd......");

    // A setter writing the value a row already holds is a no-op rather than a
    // second dirty mark.
    grid.setLineAttribute(1, lineAttributeNormal);
    grid.setRowWrapped(1, false);
    grid.setRowMark(1, 0);
    CHECK(grid.rowMeta(1).lineAttribute == lineAttributeNormal);

    // And the real writes land.
    grid.setLineAttribute(2, lineAttributeDoubleWidth);
    grid.setRowWrapped(2, true);
    grid.setRowMark(2, 5);
    CHECK(grid.rowMeta(2).lineAttribute == lineAttributeDoubleWidth);
    CHECK(grid.rowMeta(2).has(RowFlag::Wrapped));
    CHECK(grid.rowMeta(2).oscMark == 5);
    grid.resetRowMeta(2);
    CHECK(grid.rowMeta(2).lineAttribute == lineAttributeNormal);
    CHECK(!grid.rowMeta(2).has(RowFlag::Wrapped));
    CHECK(grid.rowMeta(2).oscMark == 0);
}

// The damage entries a renderer drives directly: a single row, and a band
// whose edges the caller has not clamped.
TEST(testDamageMarkingClampsItsOwnRange)
{
    CellGrid grid;
    grid.init(6, 12);
    writeWord(grid, 3, "row3");
    PaintSession warm = grid.beginPaint();
    PaintRun run;
    while (warm.nextDirtyRun(&run))
        warm.ackRun(run);

    // The band entry is private; every public mutator that damages more than
    // one row goes through it. A scroll whose region is the whole grid marks
    // the full band; a banded one marks its own rows and no others.
    grid.scrollRegion(Row(0), Row(grid.rows() - 1), 1, defaultBlank());
    grid.scrollRegion(Row(1), Row(3), -1, defaultBlank());
    grid.eraseRowRange(Row(2), Column(0), Column(grid.columns() - 1), defaultBlank());
    CHECK(grid.hasUnpaintedWork());

    // A full-viewport scroll is published as a pending scroll rather than as
    // dirty runs — the blitter moves the pixels — so the session is drained
    // in that order, and the band the scroll exposed comes back as runs.
    int rowsSeen = 0;
    PaintSession session = grid.beginPaint();
    Screen::PendingScroll scroll;
    while (session.nextPendingScroll(&scroll))
        session.ackScroll(scroll);
    bool rowTouched[6] = {};
    while (session.nextDirtyRun(&run)) {
        rowTouched[run.row.value()] = true;
        session.ackRun(run);
    }
    for (const bool touched : rowTouched)
        if (touched)
            ++rowsSeen;
    CHECK(rowsSeen > 0);
}

// The shaped-row cache: a lookup before anything is stored allocates nothing,
// a stored row comes back only for the same identity, and every refusal is on
// an argument the caller could plausibly pass.
TEST(testTheShapedRowCacheRefusesEveryMismatch)
{
    CellGrid grid;
    grid.init(4, 8);
    Cell shaped[8];
    for (int i = 0; i < 8; ++i)
        shaped[i] = letter(static_cast<char>('a' + i));
    Cell out[8];

    // Nothing stored yet: the cache does not exist, so every lookup misses.
    CHECK(!grid.lookupShapedRow(0, 0, out, 8));

    // Arguments a caller can get wrong, before the cache exists and after.
    grid.storeShapedRow(-1, 0, shaped, 8); // negative row
    grid.storeShapedRow(0, 0, nullptr, 8); // no cells
    grid.storeShapedRow(0, 0, shaped, 0); // no width
    grid.storeShapedRow(0, 0, shaped, maxColumns + 1); // past the widest grid
    CHECK(!grid.lookupShapedRow(0, 0, out, 8));

    // A real store, and the identity that reads it back.
    grid.storeShapedRow(2, 7, shaped, 8);
    CHECK(grid.lookupShapedRow(2, 7, out, 8));
    CHECK(out[0].codePoint == static_cast<std::uint32_t>('a'));

    // Each field of the identity, wrong in turn.
    CHECK(!grid.lookupShapedRow(3, 7, out, 8)); // another row
    CHECK(!grid.lookupShapedRow(2, 8, out, 8)); // another generation
    CHECK(!grid.lookupShapedRow(2, 7, out, 4)); // another width
    CHECK(!grid.lookupShapedRow(-1, 7, out, 8)); // off the grid
    CHECK(!grid.lookupShapedRow(2, 7, nullptr, 8)); // nowhere to write
    CHECK(!grid.lookupShapedRow(2, 7, out, 0)); // no width
    CHECK(!grid.lookupShapedRow(2, 7, out, maxColumns + 1));

    // A write to the row bumps its generation, so the entry stored under the
    // old one no longer answers — the cached shaping described cells that
    // have since changed.
    grid.storeShapedRow(1, grid.rowGeneration(1), shaped, 8);
    CHECK(grid.lookupShapedRow(1, grid.rowGeneration(1), out, 8));
    const std::uint32_t generationBefore = grid.rowGeneration(1);
    grid.setCell(Row(1), Column(0), letter('Z'));
    CHECK(grid.rowGeneration(1) != generationBefore);
    // The caller asks with the row's CURRENT generation, which the stored
    // entry no longer carries: the cached shaping described cells that have
    // since changed, so it misses.
    CHECK(!grid.lookupShapedRow(1, grid.rowGeneration(1), out, 8));

    // A full-grid mutation drops the whole cache through its epoch rather
    // than bumping every row.
    grid.storeShapedRow(1, grid.rowGeneration(1), shaped, 8);
    CHECK(grid.lookupShapedRow(1, grid.rowGeneration(1), out, 8));
    grid.scrollRegion(Row(0), Row(grid.rows() - 1), 1, defaultBlank());
    CHECK(!grid.lookupShapedRow(1, grid.rowGeneration(1), out, 8));

    // A generation for a row that does not exist is zero rather than a read
    // past the array.
    CHECK(grid.rowGeneration(-1) == 0);
    CHECK(grid.rowGeneration(99999) == 0);
}

// A paint session is moved, not copied: the moved-from one must stop driving
// the grid, or two sessions acknowledge the same damage and the second clears
// runs the first never painted.
// The shaping gate decides whether a row takes the bidi/cursive path on its way
// to the screen, and that path costs a reshape where a plain row costs a copy.
// Every ZWJ emoji sequence carries a joiner, so an ordinary screen arms the gate
// on rows that later hold nothing but ASCII.
TEST(aRowThatNoLongerCarriesAJoinerLosesItsShapingGate)
{
    const auto joiner = [](std::uint32_t codePoint) {
        Cell c = defaultBlank();
        c.codePoint = codePoint;
        return c;
    };

    {
        CellGrid grid;
        grid.init(4, 8);
        Cell span[2] = { joiner(0x200Du), letter('a') };
        grid.setRowSpan(Row(0), Column(0), RowView { span, 2 });
        CHECK_MSG(grid.rowMeta(0).has(Ubiquitty::RowFlag::NeedsShaping), "a joiner arms the gate");

        // Erase the line, which is what a shell does before redrawing it.
        grid.eraseRowRange(Row(0), Column(0), Column(grid.columns() - 1), defaultBlank());
        CHECK_MSG(!grid.rowMeta(0).has(Ubiquitty::RowFlag::NeedsShaping), "erasing the row disarms it");
    }
    {
        // A whole-row span of plain cells answers for the row too.
        CellGrid grid;
        grid.init(4, 8);
        Cell armed[1] = { joiner(0x0627u) }; // Arabic alef
        grid.setRowSpan(Row(1), Column(0), RowView { armed, 1 });
        CHECK(grid.rowMeta(1).has(Ubiquitty::RowFlag::NeedsShaping));

        Cell plain[8];
        for (Cell& c : plain)
            c = letter('x');
        grid.setRowSpan(Row(1), Column(0), RowView { plain, 8 });
        CHECK_MSG(!grid.rowMeta(1).has(Ubiquitty::RowFlag::NeedsShaping), "a whole-row plain span disarms it");
    }
    {
        // The control, and the reason the clear is conditioned on covering the
        // row: a partial write says nothing about the columns it did not touch,
        // and clearing on one would leave a joiner elsewhere on the row unshaped.
        CellGrid grid;
        grid.init(4, 8);
        Cell armed[1] = { joiner(0x200Du) };
        grid.setRowSpan(Row(2), Column(7), RowView { armed, 1 });
        CHECK(grid.rowMeta(2).has(Ubiquitty::RowFlag::NeedsShaping));

        Cell plain[4];
        for (Cell& c : plain)
            c = letter('y');
        grid.setRowSpan(Row(2), Column(0), RowView { plain, 4 });
        CHECK_MSG(grid.rowMeta(2).has(Ubiquitty::RowFlag::NeedsShaping), "a partial span leaves the gate armed");

        grid.eraseRowRange(Row(2), Column(0), Column(3), defaultBlank());
        CHECK_MSG(grid.rowMeta(2).has(Ubiquitty::RowFlag::NeedsShaping), "a partial erase leaves it armed");
    }
}

TEST(testAMovedPaintSessionStopsDrivingTheGrid)
{
    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 1, "moved");

    PaintSession original = grid.beginPaint();
    PaintSession moved = std::move(original);

    // The moved-from session yields nothing and acknowledges nothing.
    PaintRun run;
    CHECK(!original.nextDirtyRun(&run));
    Screen::PendingScroll scroll;
    CHECK(!original.nextPendingScroll(&scroll));
    original.ackScroll(scroll);

    // The moved-to session sees the damage.
    int runs = 0;
    while (moved.nextDirtyRun(&run)) {
        moved.ackRun(run);
        ++runs;
    }
    CHECK(runs > 0);

    // Move-assignment carries the same discipline.
    writeWord(grid, 2, "again");
    PaintSession first = grid.beginPaint();
    PaintSession second = grid.beginPaint();
    second = std::move(first);
    CHECK(!first.nextDirtyRun(&run));
    while (second.nextDirtyRun(&run))
        second.ackRun(run);
}

// A run breaks where its pen changes, and an RGB pen's components are part of
// that pen: two cells naming RGB with different channels are different runs,
// or a paint run carries one colour over cells the terminal gave another.
TEST(testRunsBreakOnEveryPenComponent)
{
    CellGrid grid;
    grid.init(2, 8);

    const auto rgbCell = [](char c, std::uint8_t red, std::uint8_t green, std::uint8_t blue, bool background) {
        Cell cell = letter(c);
        if (background) {
            cell.pen.backgroundIndex = colorRGB;
            cell.pen.backgroundRed = red;
            cell.pen.backgroundGreen = green;
            cell.pen.backgroundBlue = blue;
        } else {
            cell.pen.foregroundIndex = colorRGB;
            cell.pen.foregroundRed = red;
            cell.pen.foregroundGreen = green;
            cell.pen.foregroundBlue = blue;
        }
        return cell;
    };

    // Row 0: two RGB foregrounds differing in one channel each, then two RGB
    // backgrounds doing the same.
    grid.setCell(Row(0), Column(0), rgbCell('a', 10, 20, 30, false));
    grid.setCell(Row(0), Column(1), rgbCell('b', 99, 20, 30, false)); // red differs
    grid.setCell(Row(0), Column(2), rgbCell('c', 99, 99, 30, false)); // green differs
    grid.setCell(Row(0), Column(3), rgbCell('d', 99, 99, 99, false)); // blue differs
    grid.setCell(Row(1), Column(0), rgbCell('e', 10, 20, 30, true));
    grid.setCell(Row(1), Column(1), rgbCell('f', 99, 20, 30, true));
    grid.setCell(Row(1), Column(2), rgbCell('g', 99, 99, 30, true));
    grid.setCell(Row(1), Column(3), rgbCell('h', 99, 99, 99, true));

    int runs = 0;
    PaintSession session = grid.beginPaint();
    PaintRun run;
    while (session.nextDirtyRun(&run)) {
        session.ackRun(run);
        ++runs;
    }
    // Four distinct pens per row cannot ride fewer than four runs each.
    CHECK(runs >= 8);
}

// init and resize take their geometry from a frontend, so both ends of both
// dimensions are clamped rather than trusted.
TEST(testGeometryIsClampedAtBothEnds)
{
    CellGrid tiny;
    tiny.init(0, 0);
    CHECK(tiny.rows() >= 1 && tiny.columns() >= 1);
    tiny.init(-5, -5);
    CHECK(tiny.rows() >= 1 && tiny.columns() >= 1);

    CellGrid huge;
    huge.init(maxRows + 100, maxColumns + 100);
    CHECK(huge.rows() == maxRows && huge.columns() == maxColumns);

    CellGrid grid;
    grid.init(4, 10);
    writeWord(grid, 0, "keepme");
    grid.resize(0, 0, Modes::ReflowMode::Fixed);
    CHECK(grid.rows() >= 1 && grid.columns() >= 1);
    grid.resize(-3, -3, Modes::ReflowMode::Fixed);
    CHECK(grid.rows() >= 1 && grid.columns() >= 1);
    grid.resize(maxRows + 7, maxColumns + 7, Modes::ReflowMode::Fixed);
    CHECK(grid.rows() == maxRows && grid.columns() == maxColumns);
    // And back to something ordinary: the grid still takes writes.
    grid.resize(3, 6, Modes::ReflowMode::Fixed);
    // The clamped round trip kept the row's content, clipped to the width:
    // "keepme" survives and the new write lands on top of its head.
    CHECK(rowText(grid, 0) == "keepme");
    writeWord(grid, 0, "abc");
    CHECK(rowText(grid, 0) == "abcpme");
}

// The screen-addressed span writer clamps the same four ways the active one
// does, on the screen that is not being drawn.
TEST(testTheInactiveSpanWriterClampsToo)
{
    CellGrid grid;
    grid.init(3, 6);
    Cell span[6] = { letter('p'), letter('q'), letter('r'), letter('s'), letter('t'), letter('u') };

    // A shift past the whole span writes nothing, and one starting at the
    // right edge is clipped to nothing. (An off-grid row and a null span are
    // preconditions the Debug asserts catch, not clamps.)
    grid.setRowSpan(ScreenMode::Alternate, Row(0), Column(-9), RowView { span, 6 });
    grid.setRowSpan(ScreenMode::Alternate, Row(0), Column(6), RowView { span, 6 });
    grid.setAlternateActive(ScreenMode::Alternate);
    CHECK(rowText(grid, 0) == "......");
    grid.setAlternateActive(ScreenMode::Primary);

    // The inactive erase clamps its columns and takes both blank kinds. (An
    // inverted range and an off-grid row are preconditions its Debug asserts
    // catch rather than clamps.)
    grid.setRowSpan(ScreenMode::Alternate, Row(1), Column(0), RowView { span, 6 });
    grid.eraseRowRangeInactive(Row(1), Column(-3), Column(2), defaultBlank());
    grid.eraseRowRangeInactive(Row(1), Column(4), Column(99), styledBlank());
    grid.setAlternateActive(ScreenMode::Alternate);
    CHECK(rowText(grid, 1) == "...s..");
    grid.setAlternateActive(ScreenMode::Primary);
}

// ---- the styled-blank arms of every scroll shape ----------------------
// A scroll under a non-default blank writes the recycled rows rather than
// deferring them: the deferral is only sound for the default blank, whose
// bytes the [filled, columns) tail already holds. Each shape has its own
// recycle arm, and a terminal that only ever scrolls under the default blank
// never runs any of them.

TEST(testEveryScrollShapeRecyclesUnderAStyledBlank)
{
    const auto seed = [](CellGrid& grid) {
        for (int r = 0; r < grid.rows(); ++r) {
            char word[8] = { 'r', static_cast<char>('0' + r), 'x', 'y', 0 };
            writeWord(grid, r, word);
        }
    };
    // A row is fully styled when every column carries the blank's ink.
    const auto isStyledRow = [](const CellGrid& grid, int row) {
        for (int c = 0; c < grid.columns(); ++c) {
            const Cell* cell = grid.cellAt(Row(row), Column(c));
            if (!cell || cell->pen.backgroundIndex != 4)
                return false;
        }
        return true;
    };

    // (a) Full viewport, up one — the ring-origin recycle at the bottom.
    {
        CellGrid grid;
        grid.init(5, 8);
        seed(grid);
        grid.scrollRegion(Row(0), Row(4), 1, styledBlank());
        CHECK(rowText(grid, 0) == "r1xy....");
        CHECK(isStyledRow(grid, 4));
    }

    // (b) Full viewport, down one — the same recycle at the top.
    {
        CellGrid grid;
        grid.init(5, 8);
        seed(grid);
        grid.scrollRegion(Row(0), Row(4), -1, styledBlank());
        CHECK(rowText(grid, 1) == "r0xy....");
        CHECK(isStyledRow(grid, 0));
    }

    // (c) Full viewport by more than one row, both directions: the multi-row
    // ring shift, which recycles a band rather than a single row.
    {
        CellGrid grid;
        grid.init(6, 8);
        seed(grid);
        grid.scrollRegion(Row(0), Row(5), 2, styledBlank());
        CHECK(rowText(grid, 0) == "r2xy....");
        CHECK(isStyledRow(grid, 4) && isStyledRow(grid, 5));

        seed(grid);
        grid.scrollRegion(Row(0), Row(5), -2, styledBlank());
        CHECK(rowText(grid, 2) == "r0xy....");
        CHECK(isStyledRow(grid, 0) && isStyledRow(grid, 1));
    }

    // (d) A band inside the viewport, both directions: the memmove path,
    // which recycles inside the band and leaves the rows outside alone.
    {
        CellGrid grid;
        grid.init(6, 8);
        seed(grid);
        grid.scrollRegion(Row(1), Row(4), 2, styledBlank());
        CHECK(rowText(grid, 0) == "r0xy...."); // outside the band
        CHECK(rowText(grid, 5) == "r5xy....");
        CHECK(rowText(grid, 1) == "r3xy....");
        CHECK(isStyledRow(grid, 3) && isStyledRow(grid, 4));

        seed(grid);
        grid.scrollRegion(Row(1), Row(4), -2, styledBlank());
        CHECK(rowText(grid, 0) == "r0xy....");
        CHECK(rowText(grid, 3) == "r1xy....");
        CHECK(isStyledRow(grid, 1) && isStyledRow(grid, 2));
    }

    // (e) A shift at or past the region's height is a full-region erase, in
    // both directions and under both blanks.
    {
        CellGrid grid;
        grid.init(6, 8);
        seed(grid);
        grid.scrollRegion(Row(1), Row(3), 9, styledBlank());
        CHECK(rowText(grid, 0) == "r0xy....");
        CHECK(isStyledRow(grid, 1) && isStyledRow(grid, 2) && isStyledRow(grid, 3));
        CHECK(rowText(grid, 4) == "r4xy....");

        seed(grid);
        grid.scrollRegion(Row(2), Row(5), -9, defaultBlank());
        CHECK(rowText(grid, 1) == "r1xy....");
        CHECK(rowText(grid, 2) == "........");
        CHECK(rowText(grid, 5) == "........");
    }
}

// A wide run spanning a window's edge orphans the half left outside it: the
// surviving half draws as a chain into a cell that no longer continues it.
// Clearing that spill is the grid's, on the two entries the terminal calls
// before it overwrites a window — the writers themselves do not, because the
// caller knows which of its writes can land mid-chain and which cannot.
TEST(testWideRunsSpillingAWindowAreCleared)
{
    const auto wide = [](char c) {
        Cell cell = letter(c);
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::Wide);
        cellSetPen(cell, pen);
        return cell;
    };
    const auto continuation = []() {
        Cell cell = defaultBlank();
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::WideContinuation);
        cellSetPen(cell, pen);
        return cell;
    };
    const auto isPartOfChain = [](const CellGrid& grid, int row, int column) {
        const Cell* cell = grid.cellAt(Row(row), Column(column));
        return cell
            && cellPen(*cell).attributes.containsAny({ VT::Attribute::Wide, VT::Attribute::WideContinuation });
    };
    // No half may be left without its partner.
    const auto hasNoOrphanHalf = [&](const CellGrid& grid, int row) {
        for (int c = 0; c < grid.columns(); ++c) {
            const Cell* cell = grid.cellAt(Row(row), Column(c));
            if (!cell)
                continue;
            const VT::AttributeSet attributes = cellPen(*cell).attributes;
            if (attributes.contains(VT::Attribute::WideContinuation)) {
                if (c == 0 || !isPartOfChain(grid, row, c - 1))
                    return false;
            } else if (attributes.contains(VT::Attribute::Wide)) {
                if (c + 1 >= grid.columns() || !isPartOfChain(grid, row, c + 1))
                    return false;
            }
        }
        return true;
    };
    const auto plantPairs = [&](CellGrid& grid, int row) {
        grid.setCell(Row(row), Column(2), wide('W'));
        grid.setCell(Row(row), Column(3), continuation());
        grid.setCell(Row(row), Column(5), wide('X'));
        grid.setCell(Row(row), Column(6), continuation());
    };

    for (int pass = 0; pass < 2; ++pass) {
        const Cell blank = pass == 0 ? defaultBlank() : styledBlank();

        // A window over [3,5] takes the tail of the first pair and the head of
        // the second: clearWideOverlap steps the partners standing OUTSIDE it,
        // and the caller overwrites the window itself — so the row is whole
        // only once both halves of the bargain are done.
        CellGrid grid;
        grid.init(3, 9);
        plantPairs(grid, 0);
        CHECK(hasNoOrphanHalf(grid, 0));
        grid.clearWideOverlap(Row(0), Column(3), Column(5), blank);
        CHECK(!isPartOfChain(grid, 0, 2)); // the first pair's head, outside
        CHECK(!isPartOfChain(grid, 0, 6)); // the second pair's tail, outside
        grid.fillCellRun(Row(0), Column(3), letter('#'), 3); // the caller's write
        CHECK(hasNoOrphanHalf(grid, 0));

        // The edges-only entry, for a caller that overwrites the whole window
        // itself: only the two partners standing outside it are its business.
        plantPairs(grid, 1);
        grid.clearWideOverlapEdges(Row(1), Column(3), Column(5), blank);
        grid.fillCellRun(Row(1), Column(3), letter('#'), 3); // the caller's own write
        CHECK(hasNoOrphanHalf(grid, 1));

        // A window that touches neither chain leaves both whole.
        plantPairs(grid, 2);
        grid.clearWideOverlap(Row(2), Column(8), Column(8), blank);
        CHECK(hasNoOrphanHalf(grid, 2));
        CHECK(isPartOfChain(grid, 2, 2) && isPartOfChain(grid, 2, 3));

        // Both entries clamp their window rather than reading past the row.
        grid.clearWideOverlap(Row(2), Column(-4), Column(1), blank);
        grid.clearWideOverlap(Row(2), Column(7), Column(99), blank);
        grid.clearWideOverlapEdges(Row(2), Column(-4), Column(1), blank);
        grid.clearWideOverlapEdges(Row(2), Column(7), Column(99), blank);
        CHECK(hasNoOrphanHalf(grid, 2));
    }
}

// Damage bands accumulate across one paint cycle: a second band wider on
// either end widens the range, and one inside it changes nothing. A renderer
// reading a range narrower than the damage leaves a stale strip on screen.
TEST(testDamageBandsAccumulateAcrossACycle)
{
    CellGrid grid;
    grid.init(8, 10);
    for (int r = 0; r < grid.rows(); ++r)
        writeWord(grid, r, "seed");
    PaintSession warm = grid.beginPaint();
    PaintRun run;
    Screen::PendingScroll scroll;
    while (warm.nextPendingScroll(&scroll))
        warm.ackScroll(scroll);
    while (warm.nextDirtyRun(&run))
        warm.ackRun(run);

    // A middle band, then one inside it, then ones past each end.
    grid.scrollRegion(Row(3), Row(4), 1, defaultBlank());
    grid.scrollRegion(Row(3), Row(3), 1, defaultBlank()); // inside
    grid.scrollRegion(Row(1), Row(4), 1, defaultBlank()); // past the top
    grid.scrollRegion(Row(3), Row(6), 1, defaultBlank()); // past the bottom

    bool rowTouched[8] = {};
    PaintSession session = grid.beginPaint();
    while (session.nextPendingScroll(&scroll))
        session.ackScroll(scroll);
    while (session.nextDirtyRun(&run)) {
        rowTouched[run.row.value()] = true;
        session.ackRun(run);
    }
    // The union of every band reported damage, not just the last one: rows
    // above the last band's top are in the report.
    int lowestTouched = grid.rows();
    int highestTouched = -1;
    for (int r = 0; r < grid.rows(); ++r)
        if (rowTouched[r]) {
            if (r < lowestTouched)
                lowestTouched = r;
            highestTouched = r;
        }
    CHECK(highestTouched >= 0);
    CHECK(lowestTouched < 3); // the band starting at row 1 is in the union
}

// The shaped-row cache is invalidated two ways, and which one runs depends on
// how much of the grid moved: a band bumps each row's generation, a full-grid
// mutation drops the whole cache through its epoch. A band that bumped every
// row instead would cost the same as the epoch and keep the stale entries.
TEST(testTheShapedCacheIsDroppedByBandAndByEpoch)
{
    CellGrid grid;
    grid.init(6, 8);
    for (int r = 0; r < grid.rows(); ++r)
        writeWord(grid, r, "shaped");
    Cell shaped[8];
    for (int i = 0; i < 8; ++i)
        shaped[i] = letter(static_cast<char>('a' + i));
    Cell out[8];

    // A band: only the rows inside it lose their entries.
    for (int r = 0; r < grid.rows(); ++r)
        grid.storeShapedRow(r, grid.rowGeneration(r), shaped, 8);
    grid.scrollRegion(Row(2), Row(3), 1, defaultBlank());
    CHECK(!grid.lookupShapedRow(2, grid.rowGeneration(2), out, 8));
    CHECK(grid.lookupShapedRow(5, grid.rowGeneration(5), out, 8));

    // A full-region clear covers the whole grid: every entry goes.
    for (int r = 0; r < grid.rows(); ++r)
        grid.storeShapedRow(r, grid.rowGeneration(r), shaped, 8);
    grid.scrollRegion(Row(0), Row(grid.rows() - 1), grid.rows() + 3, defaultBlank());
    for (int r = 0; r < grid.rows(); ++r)
        CHECK(!grid.lookupShapedRow(r, grid.rowGeneration(r), out, 8));
}

// Sticky hints ride every writer: a renderer that skips the blink pass on a
// grid whose hint is false, or the shaping pass on a row whose flag is
// unset, must never be told false by a write that set neither.
TEST(testEveryWriterSetsTheStickyHints)
{
    const auto blinking = [](char c) {
        Cell cell = letter(c);
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::Blink);
        cellSetPen(cell, pen);
        return cell;
    };
    // An Arabic base latches the row's shaping gate; a Latin one does not.
    const auto cursive = []() {
        Cell cell = defaultBlank();
        cell.codePoint = 0x0633u; // ARABIC LETTER SEEN
        return cell;
    };

    // (a) The single-cell writers latch the shaping gate on a cursive base.
    {
        CellGrid grid;
        grid.init(3, 8);
        CHECK(!grid.rowMeta(0).has(RowFlag::NeedsShaping));
        grid.setCell(Row(0), Column(0), cursive());
        CHECK(grid.rowMeta(0).has(RowFlag::NeedsShaping));

        // The known-different writer takes the same path with the memcmp
        // skipped, so it has to latch the flag itself.
        CHECK(!grid.rowMeta(1).has(RowFlag::NeedsShaping));
        grid.setCellKnownDiff(Row(1), Column(0), cursive());
        CHECK(grid.rowMeta(1).has(RowFlag::NeedsShaping));

        // A Latin write leaves the gate alone.
        grid.setCell(Row(2), Column(0), letter('a'));
        CHECK(!grid.rowMeta(2).has(RowFlag::NeedsShaping));
    }

    // (b) Every bulk writer sets the blink hint.
    {
        CellGrid grid;
        grid.init(4, 8);
        CHECK(!grid.hasBlinkHint());
        grid.writeASCIIRun(Row(0), Column(0), blinking(' '), "blink");
        CHECK(grid.hasBlinkHint());
    }
    {
        CellGrid grid;
        grid.init(4, 8);
        // The careful path: the run's first cell already matches, so the
        // writer falls through to the per-cell compare, and the hint has to
        // ride that branch too. Both writes carry the same blinking template,
        // so it is the first cell's match — not the pen — that selects it.
        grid.writeASCIIRun(Row(0), Column(0), blinking(' '), "abcde");
        CHECK(grid.hasBlinkHint());
        grid.clearBlinkHintIfClean();
        grid.writeASCIIRun(Row(0), Column(0), blinking(' '), "abcXe");
        CHECK(grid.hasBlinkHint());

        // An identical repaint through the same path changes nothing.
        grid.clearBlinkHintIfClean();
        grid.writeASCIIRun(Row(0), Column(0), blinking(' '), "abcXe");
        CHECK(!grid.hasBlinkHint());
    }
    {
        CellGrid grid;
        grid.init(4, 8);
        grid.fillCellRun(Row(1), Column(2), blinking('#'), 3);
        CHECK(grid.hasBlinkHint());
    }
    {
        // The hint carries the generation it was armed at, so a viewer that
        // scanned the grid can tell whether its scan predates the blink. Every
        // writer that arms the flag must stamp it: a path that set one without
        // the other would publish a stamp older than the blink it describes,
        // which is the staleness a retracting viewer relies on being absent.
        //
        // Each case paints first so the generation has left zero. Without that
        // the assertion is vacuous — an unstamped hint reads zero, and so does
        // the generation of a grid nothing has painted.
        auto paintThrough = [](CellGrid& grid) {
            PaintSession session = grid.beginPaint();
            PaintRun run;
            while (session.nextDirtyRun(&run))
                session.ackRun(run);
        };
        {
            CellGrid grid;
            grid.init(4, 8);
            grid.setCell(Row(0), Column(0), letter('a'));
            paintThrough(grid);
            CHECK_MSG(grid.currentDamageGeneration() != 0, "the paint moved the generation off zero");
            CHECK_MSG(grid.blinkHintGeneration() == 0, "an unarmed hint carries no generation");

            grid.setCell(Row(0), Column(1), blinking('b'));
            CHECK(grid.hasBlinkHint());
            CHECK_MSG(grid.blinkHintGeneration() == grid.currentDamageGeneration(), "setCell stamps");
        }
        {
            CellGrid grid;
            grid.init(4, 8);
            grid.setCell(Row(0), Column(0), letter('a'));
            paintThrough(grid);
            grid.writeASCIIRun(Row(1), Column(0), blinking(' '), "abc");
            CHECK(grid.hasBlinkHint());
            CHECK_MSG(grid.blinkHintGeneration() == grid.currentDamageGeneration(), "writeASCIIRun stamps");
        }
        {
            CellGrid grid;
            grid.init(4, 8);
            grid.setCell(Row(0), Column(0), letter('a'));
            paintThrough(grid);
            grid.fillCellRun(Row(2), Column(0), blinking('#'), 3);
            CHECK(grid.hasBlinkHint());
            CHECK_MSG(grid.blinkHintGeneration() == grid.currentDamageGeneration(), "fillCellRun stamps");
        }
        {
            CellGrid grid;
            grid.init(4, 8);
            grid.setCell(Row(0), Column(0), letter('a'));
            paintThrough(grid);
            Cell span[2] = { blinking('x'), letter('y') };
            grid.setRowSpan(Row(3), Column(0), RowView { span, 2 });
            CHECK(grid.hasBlinkHint());
            CHECK_MSG(grid.blinkHintGeneration() == grid.currentDamageGeneration(), "setRowSpan stamps");
        }
        {
            // The control: a write carrying no Blink neither arms the hint nor
            // moves the stamp, so the cases above are not passing because every
            // write stamps.
            CellGrid grid;
            grid.init(4, 8);
            grid.setCell(Row(0), Column(0), letter('a'));
            paintThrough(grid);
            grid.setCell(Row(1), Column(0), letter('b'));
            CHECK_MSG(!grid.hasBlinkHint(), "a plain write leaves the hint unarmed");
            CHECK_MSG(grid.blinkHintGeneration() == 0, "a plain write leaves the stamp unset");
        }
    }
    {
        CellGrid grid;
        grid.init(4, 8);
        Cell span[3] = { blinking('x'), letter('y'), letter('z') };
        grid.setRowSpan(Row(2), Column(1), RowView { span, 3 });
        CHECK(grid.hasBlinkHint());

        // A blinking wide pair through the span writer: the pair takes the
        // direct row-buffer write, which latches the hint off the lead half.
        grid.clearBlinkHintIfClean();
        Cell lead = blinking(0);
        lead.codePoint = 0x4E00u;
        Pen leadPen = cellPen(lead);
        leadPen.attributes.add(VT::Attribute::Wide);
        cellSetPen(lead, leadPen);
        Cell tail = defaultBlank();
        Pen tailPen = cellPen(tail);
        tailPen.attributes.add(VT::Attribute::WideContinuation);
        tailPen.attributes.add(VT::Attribute::Blink);
        cellSetPen(tail, tailPen);
        Cell pair[2] = { lead, tail };
        grid.setRowSpan(Row(3), Column(0), RowView { pair, 2 });
        CHECK(grid.hasBlinkHint());
    }

    // (f) The rectangle scroll reads the hints to decide whether the span it moves
    // needs searching for these attributes at all. A clear hint states the grid
    // holds no such cell, so the search is skipped — and the span must still
    // arrive whole, which is what the skip is easiest to get wrong.
    {
        CellGrid grid;
        grid.init(4, 8);
        writeWord(grid, 1, "plain");
        CHECK(!grid.hasBlinkHint());
        CHECK(!grid.hasSizedCellHint());
        grid.scrollRectangleCells(0, 3, 0, 4, 1, defaultBlank(), RectangleScrollDirection::Up);
        // Row 1's text is now row 0's, and neither hint was invented on the way.
        CHECK(grid.cellAt(Row(0), Column(0))->codePoint == static_cast<std::uint32_t>('p'));
        CHECK(grid.cellAt(Row(0), Column(4))->codePoint == static_cast<std::uint32_t>('n'));
        CHECK(!grid.hasBlinkHint());
        CHECK(!grid.hasSizedCellHint());
    }
    {
        // The same move with a blinking cell in the span: the hint is set, so the
        // search runs and the moved row is still reported as carrying blink.
        CellGrid grid;
        grid.init(4, 8);
        grid.writeASCIIRun(Row(1), Column(0), blinking(' '), "blink");
        CHECK(grid.hasBlinkHint());
        grid.scrollRectangleCells(0, 3, 0, 4, 1, defaultBlank(), RectangleScrollDirection::Up);
        CHECK(grid.cellAt(Row(0), Column(0))->codePoint == static_cast<std::uint32_t>('b'));
        CHECK(grid.hasBlinkHint());
    }
}

// A recycled row's blank is deferred rather than written, so the next write
// to it has to materialise the part it is not covering — a writer that
// trusted the stored bytes would draw the previous occupant's tail beside its
// own text.
TEST(testWritesOntoADeferredBlankRowMaterialiseAroundThemselves)
{
    // (a) The fill writer lands on the deferred row.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank()); // row 3 recycles, deferred
        grid.fillCellRun(Row(3), Column(4), letter('#'), 2);
        CHECK(rowText(grid, 3) == "....##....");
    }

    // (b) The ASCII writer, which has its own flank materialisation.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        grid.writeASCIIRun(Row(3), Column(3), defaultBlank(), "new");
        CHECK(rowText(grid, 3) == "...new....");
    }

    // (c) A second scroll while a deferral is live: the first row's blank has
    // to be resolved before the second takes the slot, or one of the two rows
    // keeps the stale bytes.
    {
        CellGrid grid;
        grid.init(4, 10);
        for (int r = 0; r < 4; ++r)
            writeWord(grid, r, "abcdefghij");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        CHECK(rowText(grid, 2) == "..........");
        CHECK(rowText(grid, 3) == "..........");
        // And a write lands cleanly on the newest deferred row.
        grid.writeASCIIRun(Row(3), Column(0), defaultBlank(), "zz");
        CHECK(rowText(grid, 3) == "zz........");
    }

    // (e) The span writer lands on the deferred row: the flanks materialise,
    // the span's own window is stored directly.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell span[3] = { letter('N'), letter('E'), letter('W') };
        grid.setRowSpan(Row(3), Column(4), RowView { span, 3 });
        CHECK(rowText(grid, 3) == "....NEW...");
        CHECK(grid.rowContentWidth(Row(3)) == 7);
    }

    // (f) A span of pure default blanks onto the deferred row is the
    // idempotent case: bytes materialise, hints stay untouched.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell span[2] = { defaultBlank(), defaultBlank() };
        grid.setRowSpan(Row(3), Column(2), RowView { span, 2 });
        CHECK(rowText(grid, 3) == "..........");
        CHECK(grid.rowContentWidth(Row(3)) == 0);
    }

    // (g) The wide-pair writer lands on the deferred row.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell lead = defaultBlank();
        lead.codePoint = 0x4E2D; // 中
        Pen leadPen = lead.pen;
        leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cellSetPen(lead, leadPen);
        Cell continuation = defaultBlank();
        continuation.codePoint = wideContinuationSentinelCodePoint;
        Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        cellSetPen(continuation, continuationPen);
        grid.writeWideCell(Row(3), Column(4), lead, continuation, defaultBlank());
        CHECK(grid.cellAt(Row(3), Column(4))->codePoint == 0x4E2Du);
        CHECK(grid.cellAt(Row(3), Column(5))->codePoint == wideContinuationSentinelCodePoint);
        // Stale flank bytes materialised as blanks around the pair.
        CHECK(rowText(grid, 3) == "..........");
        CHECK(grid.rowContentWidth(Row(3)) == 6);
    }

    // (h) logicalCellAt answers blank on the deferred row without
    // materialising it: the stored bytes stay stale until a write lane runs.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        for (int c = 0; c < grid.columns(); ++c)
            CHECK(grid.logicalCellAt(Row(3), Column(c))->codePoint == 0u);
        // The materialising reader still sees blanks afterwards (resolution
        // happened at the latest here, not earlier through the probe).
        CHECK(rowText(grid, 3) == "..........");
    }

    // (i) Sequential wide pairs land exactly at the stale edge: each consumes
    // its window in place and the tail stays deferred until read.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        const auto widePair = [](std::uint32_t codePoint) {
            Cell lead = defaultBlank();
            lead.codePoint = codePoint;
            Pen leadPen = lead.pen;
            leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
            cellSetPen(lead, leadPen);
            Cell continuation = defaultBlank();
            continuation.codePoint = wideContinuationSentinelCodePoint;
            Pen continuationPen = continuation.pen;
            continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
            cellSetPen(continuation, continuationPen);
            return std::pair<Cell, Cell> { lead, continuation };
        };
        const auto [leadA, continuationA] = widePair(0x4E2D);
        const auto [leadB, continuationB] = widePair(0x6587);
        const auto [leadC, continuationC] = widePair(0x5B57);
        grid.writeWideCell(Row(3), Column(0), leadA, continuationA, defaultBlank());
        grid.writeWideCell(Row(3), Column(2), leadB, continuationB, defaultBlank());
        // Mid-consumption, the logical probe answers content below the edge
        // and blank at and past it.
        CHECK(grid.logicalCellAt(Row(3), Column(0))->codePoint == 0x4E2Du);
        CHECK(grid.logicalCellAt(Row(3), Column(2))->codePoint == 0x6587u);
        CHECK(grid.logicalCellAt(Row(3), Column(4))->codePoint == 0u);
        CHECK(grid.logicalCellAt(Row(3), Column(9))->codePoint == 0u);
        grid.writeWideCell(Row(3), Column(4), leadC, continuationC, defaultBlank());
        // The materializing reader agrees: pairs, then blanks -- never the
        // recycled occupant's bytes.
        CHECK(grid.cellAt(Row(3), Column(0))->codePoint == 0x4E2Du);
        CHECK(grid.cellAt(Row(3), Column(2))->codePoint == 0x6587u);
        CHECK(grid.cellAt(Row(3), Column(4))->codePoint == 0x5B57u);
        for (int c = 6; c < 10; ++c)
            CHECK(grid.cellAt(Row(3), Column(c))->codePoint == 0u);
        CHECK(grid.rowContentWidth(Row(3)) == 6);
    }

    // (j) A wide pair off the consumed edge gives the deferral up: the row
    // resolves whole, live pairs stand, the gap and tail read blank.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell lead = defaultBlank();
        lead.codePoint = 0x4E2D;
        Pen leadPen = lead.pen;
        leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cellSetPen(lead, leadPen);
        Cell continuation = defaultBlank();
        continuation.codePoint = wideContinuationSentinelCodePoint;
        Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        cellSetPen(continuation, continuationPen);
        grid.writeWideCell(Row(3), Column(0), lead, continuation, defaultBlank()); // edge: start -> 2
        grid.writeWideCell(Row(3), Column(6), lead, continuation, defaultBlank()); // off the edge: resolves
        CHECK(grid.cellAt(Row(3), Column(0))->codePoint == 0x4E2Du);
        CHECK(grid.cellAt(Row(3), Column(6))->codePoint == 0x4E2Du);
        for (int c : { 2, 3, 4, 5, 8, 9 })
            CHECK(grid.cellAt(Row(3), Column(c))->codePoint == 0u);
    }

    // (k) An ASCII run after partial consumption preserves the live pair and
    // blanks the stale flanks around its write.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell lead = defaultBlank();
        lead.codePoint = 0x4E2D;
        Pen leadPen = lead.pen;
        leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cellSetPen(lead, leadPen);
        Cell continuation = defaultBlank();
        continuation.codePoint = wideContinuationSentinelCodePoint;
        Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        cellSetPen(continuation, continuationPen);
        grid.writeWideCell(Row(3), Column(0), lead, continuation, defaultBlank()); // start -> 2
        grid.writeASCIIRun(Row(3), Column(5), defaultBlank(), "ab");
        CHECK(grid.cellAt(Row(3), Column(0))->codePoint == 0x4E2Du);
        CHECK(rowText(grid, 3) == ".....ab...");
        for (int c : { 2, 3, 4, 7, 8, 9 })
            CHECK(grid.cellAt(Row(3), Column(c))->codePoint == 0u);
    }

    // (l) A second scroll while a partially-consumed deferral is live: the
    // recycle of the next slot resolves the old deferral first, so the pair
    // written before the scroll never resurfaces on either row.
    {
        CellGrid grid;
        grid.init(4, 10);
        // Seed the TOP row: the full-viewport scroll advances the origin and
        // recycles the old top slot as the new bottom row, so row 0 is the
        // content whose stale bytes the deferral must mask.
        writeWord(grid, 0, "oldcontent");
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank());
        Cell lead = defaultBlank();
        lead.codePoint = 0x4E2D;
        Pen leadPen = lead.pen;
        leadPen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cellSetPen(lead, leadPen);
        Cell continuation = defaultBlank();
        continuation.codePoint = wideContinuationSentinelCodePoint;
        Pen continuationPen = continuation.pen;
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        cellSetPen(continuation, continuationPen);
        grid.writeWideCell(Row(3), Column(0), lead, continuation, defaultBlank()); // start -> 2
        grid.scrollRegion(Row(0), Row(3), 1, defaultBlank()); // next slot recycles; old deferral resolves
        CHECK(rowText(grid, 2) == "..........");
        CHECK(rowText(grid, 3) == "..........");
        for (int c = 0; c < 10; ++c)
            CHECK(grid.cellAt(Row(3), Column(c))->codePoint == 0u);
        grid.writeWideCell(Row(3), Column(0), lead, continuation, defaultBlank());
        CHECK(grid.cellAt(Row(3), Column(0))->codePoint == 0x4E2Du);
    }

    // (d) A banded scroll of one row in each direction under a styled blank:
    // the recycled row is written rather than deferred, because the tail the
    // deferral trusts holds the DEFAULT blank's bytes and this one does not.
    {
        CellGrid grid;
        grid.init(5, 8);
        for (int r = 0; r < 5; ++r) {
            char word[8] = { 'r', static_cast<char>('0' + r), 0 };
            writeWord(grid, r, word);
        }
        grid.scrollRegion(Row(1), Row(3), 1, styledBlank());
        CHECK(rowText(grid, 1) == "r2......");
        for (int c = 0; c < grid.columns(); ++c)
            CHECK(grid.cellAt(Row(3), Column(c))->pen.backgroundIndex == 4);

        grid.scrollRegion(Row(1), Row(3), -1, styledBlank());
        for (int c = 0; c < grid.columns(); ++c)
            CHECK(grid.cellAt(Row(1), Column(c))->pen.backgroundIndex == 4);
        CHECK(rowText(grid, 2) == "r2......");
    }
}

// ---- residue ledger --------------------------------------------------
// What stays dark in CellGrid.cpp after these cases, and why each arm cannot
// execute rather than merely does not:
//
// - Every clamp that sits behind a Debug assert on the same condition
//   (setCell, setCellKnownDiff, both setRowSpan overloads, eraseRowRange,
//   eraseRowRangeInactive, fillCellRun): in the coverage build the assert
//   fires first, so the clamp below it is unreachable by construction. It is
//   the release-build guard for a caller a frame behind the grid.
// - The scroll-up half of scrollRegion's single-row full-viewport branch:
//   the `n == 1` route above it hands every such scroll to
//   scrollFullViewportUpOne, and the only geometry that could skip that route
//   (a one-row grid) takes the full-region-clear path instead, which the
//   branch excludes. Twenty-four lines that no input can reach — a redundancy
//   worth removing, not testing, and flagged for the file's owner rather than
//   deleted here.
// - Every arm of markDirtyRange but the epoch drop: all five call sites pass
//   either the full viewport band or (0, rows-1), so the band is always valid,
//   always the whole grid, and never wider than the one before it. The
//   clamps, the per-row generation bump and the widen arms are unreachable
//   while that stays true.
// - The private helpers' own bounds checks (physicalRowSlot, normalizeRowRing,
//   noteFilled, rowContentWidth's negative arm, resolveDeferredBlankSlot's
//   count clamp): each is called only from inside this file, by code that has
//   already bounded its arguments.
// - allocBuffers' minimum-of-one clamps and roundUpCapacity's ceiling: init
//   and resize clamp the geometry to [1, max] before either is called.
// - The first-allocation branch of ensureCapacity: every grid these tests
//   build has already allocated through init.
// - assertInvariants' failure returns: each one describes a corrupted grid,
//   which no public entry can produce — they are the debug oracle's answers,
//   not states to construct.
