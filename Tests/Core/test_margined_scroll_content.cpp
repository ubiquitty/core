// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What a margined scroll leaves in the grid, checked against a reference that
// does not share any of the grid's machinery.
//
// A scroll confined to horizontal margins moves a column band and must leave
// every cell outside it alone. Comparing the grid against itself — a retained
// copy against a re-read — cannot see a move that took the wrong cells, because
// both sides come from the same model. So the expectation here is computed
// independently: a plain array of characters, scrolled by the definition of the
// operation rather than by the code under test.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

// The reference screen: one character per cell, ' ' for blank.
struct ReferenceScreen {
    int rows = 0;
    int columns = 0;
    std::vector<char> cells;

    void reset(int rowCount, int columnCount)
    {
        rows = rowCount;
        columns = columnCount;
        cells.assign(static_cast<std::size_t>(rows * columns), ' ');
    }
    char& at(int row, int column) { return cells[static_cast<std::size_t>(row * columns + column)]; }
    [[nodiscard]] char at(int row, int column) const { return cells[static_cast<std::size_t>(row * columns + column)]; }

    void write(int row, int column, const std::string& text)
    {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int target = column + static_cast<int>(i);
            if (row >= 0 && row < rows && target >= 0 && target < columns)
                at(row, target) = text[i];
        }
    }

    // The operation's definition: the band moves up one row inside the region,
    // the revealed row goes blank, and nothing outside the band is touched.
    void scrollBandUp(int top, int bottom, int left, int right)
    {
        for (int row = top; row < bottom; ++row) {
            for (int column = left; column <= right; ++column)
                at(row, column) = at(row + 1, column);
        }
        for (int column = left; column <= right; ++column)
            at(bottom, column) = ' ';
    }
};

// The grid's own answer, read one cell at a time.
char cellCharacter(Ubiquitty::Terminal& t, int row, int column)
{
    const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
    if (!cell)
        return '?';
    const std::uint32_t codePoint = cell->codePoint;
    if (codePoint == 0)
        return ' ';
    if (codePoint < 0x20 || codePoint > 0x7E)
        return '#';
    return static_cast<char>(codePoint);
}

// Reports the first disagreement, so a failure names a cell rather than a count.
bool screensAgree(Ubiquitty::Terminal& t, const ReferenceScreen& reference, const char* what)
{
    for (int row = 0; row < reference.rows; ++row) {
        for (int column = 0; column < reference.columns; ++column) {
            const char mine = cellCharacter(t, row, column);
            const char theirs = reference.at(row, column);
            if (mine != theirs) {
                std::printf("  %s: row %d column %d — grid has '%c', reference has '%c'\n",
                    what, row, column, mine, theirs);
                return false;
            }
        }
    }
    return true;
}

std::string csi(const std::string& body) { return "\x1b[" + body; }

} // namespace

TEST(aMarginedScrollMovesTheBandAndNothingElse)
{
    const int rows = 12, columns = 40;
    const int left = 5, right = 20; // 0-based band
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    ReferenceScreen reference;
    reference.reset(rows, columns);

    // Content on both sides of the band, so a move that reaches outside shows.
    for (int row = 0; row < rows; ++row) {
        const std::string outsideLeft = "L" + std::to_string(row);
        const std::string inside = "band" + std::to_string(row);
        const std::string outsideRight = "R" + std::to_string(row);
        feed(t, csi(std::to_string(row + 1) + ";1H") + outsideLeft);
        reference.write(row, 0, outsideLeft);
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(left + 1) + "H") + inside);
        reference.write(row, left, inside);
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(right + 2) + "H") + outsideRight);
        reference.write(row, right + 1, outsideRight);
    }
    CHECK(screensAgree(t, reference, "after setup"));

    // Margins over the band, region over rows 0..rows-1, cursor at the bottom.
    feed(t, "\x1b[?69h" + csi(std::to_string(left + 1) + ";" + std::to_string(right + 1) + "s"));
    feed(t, csi("1;" + std::to_string(rows) + "r"));
    feed(t, csi(std::to_string(rows) + ";" + std::to_string(left + 1) + "H"));

    for (int step = 0; step < 6; ++step) {
        feed(t, "\n");
        reference.scrollBandUp(0, rows - 1, left, right);
        CHECK(screensAgree(t, reference, "after scroll"));
    }
}

TEST(aMarginedScrollIsUnaffectedByContentOutsideTheBand)
{
    // A neighbouring pane's column — tmux draws a divider — sits outside the
    // band and must change neither what the scroll moves nor what it leaves.
    const int rows = 10, columns = 40;
    const int left = 0, right = 15;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    ReferenceScreen reference;
    reference.reset(rows, columns);

    for (int row = 0; row < rows; ++row) {
        const std::string inside = "row" + std::to_string(row);
        feed(t, csi(std::to_string(row + 1) + ";1H") + inside);
        reference.write(row, 0, inside);
        // The divider, well past the band's right edge.
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(right + 3) + "H") + "|");
        reference.write(row, right + 2, "|");
    }
    CHECK(screensAgree(t, reference, "after setup"));

    feed(t, "\x1b[?69h" + csi(std::to_string(left + 1) + ";" + std::to_string(right + 1) + "s"));
    feed(t, csi("1;" + std::to_string(rows) + "r"));
    feed(t, csi(std::to_string(rows) + ";1H"));

    for (int step = 0; step < 5; ++step) {
        feed(t, "\n");
        reference.scrollBandUp(0, rows - 1, left, right);
        CHECK(screensAgree(t, reference, "after scroll beside a divider"));
    }
}

TEST(aMarginedScrollHandlesRaggedLineLengths)
{
    // Rows whose content ends at different columns inside the band: the move
    // must carry each row's own extent and blank what the row above did not
    // reach.
    const int rows = 14, columns = 50;
    const int left = 4, right = 40;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    ReferenceScreen reference;
    reference.reset(rows, columns);

    for (int row = 0; row < rows; ++row) {
        // Lengths that rise and fall, so a shorter row lands on a longer one
        // and the other way about.
        const int length = 1 + ((row * 7) % (right - left));
        const std::string inside(static_cast<std::size_t>(length), static_cast<char>('a' + (row % 26)));
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(left + 1) + "H") + inside);
        reference.write(row, left, inside);
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(right + 2) + "H") + "|");
        reference.write(row, right + 1, "|");
    }
    CHECK(screensAgree(t, reference, "after ragged setup"));

    feed(t, "\x1b[?69h" + csi(std::to_string(left + 1) + ";" + std::to_string(right + 1) + "s"));
    feed(t, csi("1;" + std::to_string(rows) + "r"));

    for (int step = 0; step < 10; ++step) {
        // A fresh line of its own length written at the bottom, then a feed —
        // which is what a pane printing output actually does.
        const int length = 1 + ((step * 11) % (right - left));
        const std::string fresh(static_cast<std::size_t>(length), static_cast<char>('A' + (step % 26)));
        feed(t, csi(std::to_string(rows) + ";" + std::to_string(left + 1) + "H") + fresh);
        reference.write(rows - 1, left, fresh);
        CHECK(screensAgree(t, reference, "after writing a ragged line"));

        feed(t, "\n");
        reference.scrollBandUp(0, rows - 1, left, right);
        CHECK(screensAgree(t, reference, "after ragged scroll"));
    }
}

TEST(aMarginedScrollBlanksWhatAShorterRowDoesNotCover)
{
    // The specific shape a bound on the copy could get wrong: a long row scrolls
    // onto a position, then a short row scrolls onto the same one. The tail the
    // long row left must go.
    const int rows = 6, columns = 30;
    const int left = 2, right = 25;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    ReferenceScreen reference;
    reference.reset(rows, columns);

    feed(t, "\x1b[?69h" + csi(std::to_string(left + 1) + ";" + std::to_string(right + 1) + "s"));
    feed(t, csi("1;" + std::to_string(rows) + "r"));

    const std::string longLine(20, 'X');
    const std::string shortLine(3, 'y');

    feed(t, csi(std::to_string(rows) + ";" + std::to_string(left + 1) + "H") + longLine);
    reference.write(rows - 1, left, longLine);
    feed(t, "\n");
    reference.scrollBandUp(0, rows - 1, left, right);
    CHECK(screensAgree(t, reference, "after the long line scrolled"));

    feed(t, csi(std::to_string(rows) + ";" + std::to_string(left + 1) + "H") + shortLine);
    reference.write(rows - 1, left, shortLine);
    feed(t, "\n");
    reference.scrollBandUp(0, rows - 1, left, right);
    CHECK(screensAgree(t, reference, "after the short line scrolled onto it"));

    // And again, so the short row itself becomes the one being covered.
    feed(t, "\n");
    reference.scrollBandUp(0, rows - 1, left, right);
    CHECK(screensAgree(t, reference, "after the band drained"));
}

TEST(aMarginedScrollSurvivesARandomisedSession)
{
    // A long run of writes and scrolls at varied widths, compared cell by cell
    // against the reference after every step. A defect that needs a particular
    // sequence of extents to appear will be met here.
    const int rows = 16, columns = 60;
    const int left = 6, right = 45;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    ReferenceScreen reference;
    reference.reset(rows, columns);

    // A column outside the band on every row, as a neighbouring pane provides.
    for (int row = 0; row < rows; ++row) {
        feed(t, csi(std::to_string(row + 1) + ";" + std::to_string(right + 3) + "H") + "|");
        reference.write(row, right + 2, "|");
    }

    feed(t, "\x1b[?69h" + csi(std::to_string(left + 1) + ";" + std::to_string(right + 1) + "s"));
    feed(t, csi("1;" + std::to_string(rows) + "r"));

    std::uint32_t state = 0x1234567u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return (state >> 16) & 0x7FFF;
    };

    for (int step = 0; step < 120; ++step) {
        const int length = static_cast<int>(next() % static_cast<std::uint32_t>(right - left + 1));
        if (length > 0) {
            const std::string text(static_cast<std::size_t>(length), static_cast<char>('a' + (step % 26)));
            feed(t, csi(std::to_string(rows) + ";" + std::to_string(left + 1) + "H") + text);
            reference.write(rows - 1, left, text);
        }
        feed(t, "\n");
        reference.scrollBandUp(0, rows - 1, left, right);
        if (!screensAgree(t, reference, "during the randomised session")) {
            std::printf("  diverged at step %d (line length %d)\n", step, length);
            CHECK(false);
            return;
        }
    }
    CHECK(true);
}

UBIQUITTY_TEST_MAIN()
