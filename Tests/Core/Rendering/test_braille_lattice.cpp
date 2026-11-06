// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_braille_lattice — braille dots sit on one grid, not inside their cells.
//
// The property under test is the PITCH, and it is measured over a run of cells
// rather than inside one: a lattice can be perfect within every cell and still
// wobble across their boundaries, which is the failure the sub-grid derivation
// exists to prevent and the one a single-cell assertion cannot see.
//
// Every case here uses a fractional cell advance. At a whole-pixel advance
// every placement rule agrees and the file would pass against an implementation
// that reads the cell's own snapped rect — the arrangement this is a test of.

#include "Core/Rendering/BrailleLattice.h"

#include <cmath>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

// 14.4 x 30.6: a real advance from a 13pt face at 2x, fractional on both axes.
constexpr float fractionalCellWidth = 14.4f;
constexpr float fractionalCellHeight = 30.6f;

BrailleLattice fractionalLattice()
{
    return { /*originX=*/0.0f, /*originY=*/0.0f, fractionalCellWidth, fractionalCellHeight };
}

/// Every dot column's left edge across `cells` cells, in order.
std::vector<float> columnEdges(const BrailleLattice& lattice, int cells)
{
    std::vector<float> edges;
    for (int cell = 0; cell < cells; ++cell) {
        for (int column = 0; column < brailleDotColumns; ++column)
            edges.push_back(brailleDotRect(lattice, cell, 0, column, 0).left);
    }
    return edges;
}

/// Every dot row's top edge down `rows` rows, in order.
std::vector<float> rowEdges(const BrailleLattice& lattice, int rows)
{
    std::vector<float> edges;
    for (int row = 0; row < rows; ++row) {
        for (int dot = 0; dot < brailleDotRows; ++dot)
            edges.push_back(brailleDotRect(lattice, 0, row, 0, dot).top);
    }
    return edges;
}

/// The largest gap between consecutive entries minus the smallest.
///
/// Horizontal positions vary by at most one text phase; vertical positions
/// retain the fractional quarter-line pitch.
float pitchSpread(const std::vector<float>& edges)
{
    float smallest = 1e9f;
    float largest = -1e9f;
    for (std::size_t i = 1; i < edges.size(); ++i) {
        const float gap = edges[i] - edges[i - 1];
        smallest = gap < smallest ? gap : smallest;
        largest = gap > largest ? gap : largest;
    }
    return largest - smallest;
}

/// The largest entry minus the smallest.
///
/// For a list that already holds the gaps rather than the positions they are
/// measured between, where the question is whether one number describes them
/// all.
///
/// - Parameter values: The measurements to compare.
/// - Returns: Their range.
float spread(const std::vector<float>& values)
{
    float smallest = 1e9f;
    float largest = -1e9f;
    for (float value : values) {
        smallest = value < smallest ? value : smallest;
        largest = value > largest ? value : largest;
    }
    return largest - smallest;
}

} // namespace

TEST(everyPatternUsesCirclesOnTheSameLattice)
{
    const BrailleLattice lattice = fractionalLattice();
    for (std::uint32_t pattern = brailleBlankCodePoint; pattern < brailleEndCodePoint; ++pattern) {
        OverlayInstance dots[brailleDotsPerCell] {};
        for (auto& dot : dots) {
            dot.cornerStyle = 1.0f;
        }
        const int count = emitBrailleDots(pattern, lattice, 0, 0, { 1, 1, 1, 1 }, dots, brailleDotsPerCell);
        int index = 0;
        for (int column = 0; column < brailleDotColumns; ++column) {
            for (int row = 0; row < brailleDotRows; ++row) {
                if (!isBrailleDotRaised(pattern, column, row)) {
                    continue;
                }
                const BrailleDotRect expected = brailleDotRect(lattice, 0, 0, column, row);
                CHECK(index < count);
                const auto& dot = dots[index++];
                CHECK_EQ(dot.plA.x, expected.left);
                CHECK_EQ(dot.plA.y, expected.top);
                CHECK_EQ(dot.cornerRadius, (expected.right - expected.left) * 0.5f);
                CHECK_EQ(dot.cornerStyle, 0.0f);
            }
        }
        CHECK_EQ(index, count);
    }
}

TEST(invalidStyleSerializesAsTheDefault)
{
    CHECK(brailleStyleToString(static_cast<BrailleStyle>(-1)) == brailleStyleToString(defaultBrailleStyle));
}

TEST(theLatticeTracksTheTextCenter)
{
    const BrailleLattice grid { 0.0f, 0.0f, 16.0f, 32.0f };
    const BrailleLattice aligned = textAlignedBrailleLattice(grid, 25.0f, 14.0f);
    const BrailleDotRect top = brailleDotRect(aligned, 0, 0, 0, 0);
    const BrailleDotRect bottom = brailleDotRect(aligned, 0, 0, 0, 3);
    CHECK_EQ((top.top + bottom.bottom) * 0.5f, 18.0f);
    CHECK_EQ(aligned.originY, 2.0f);
    CHECK_EQ(textAlignedBrailleLattice(grid, 21.0f, 14.0f).originY, -2.0f);
    CHECK_EQ(textAlignedBrailleLattice(grid, 25.0f, 0.0f).originY, 0.0f);
}

TEST(guideStylesPreserveRaisedDotsAndLimitGuidesToTheirRows)
{
    for (const BrailleStyle style : { BrailleStyle::Outline6Dot, BrailleStyle::Outline8Dot, BrailleStyle::Pinpoint6Dot, BrailleStyle::Pinpoint8Dot }) {
        BrailleLattice lattice = fractionalLattice();
        lattice.style = style;
        const bool isSixDot = style == BrailleStyle::Outline6Dot || style == BrailleStyle::Pinpoint6Dot;
        const bool isOutline = style == BrailleStyle::Outline6Dot || style == BrailleStyle::Outline8Dot;
        for (std::uint32_t pattern = brailleBlankCodePoint; pattern < brailleEndCodePoint; ++pattern) {
            OverlayInstance dots[brailleDotsPerCell] {};
            const int count = emitBrailleDots(pattern, lattice, 2, 3, { 1, 1, 1, 1 }, dots, brailleDotsPerCell);
            int index = 0;
            for (int column = 0; column < brailleDotColumns; ++column) {
                for (int row = 0; row < brailleDotRows; ++row) {
                    const bool raised = isBrailleDotRaised(pattern, column, row);
                    if (!raised && isSixDot && row == 3) {
                        continue;
                    }
                    const auto expected = brailleDotRect(lattice, 2, 3, column, row);
                    CHECK(index < count);
                    const auto& dot = dots[index++];
                    CHECK(std::fabs(dot.topLeftInPixels.x + dot.bottomRightInPixels.x - expected.left - expected.right) < 0.001f);
                    CHECK(std::fabs(dot.topLeftInPixels.y + dot.bottomRightInPixels.y - expected.top - expected.bottom) < 0.001f);
                    CHECK_EQ(dot.cornerStyle, 0.0f);
                    CHECK_EQ(dot.strokeWidth > 0.0f, !raised && isOutline);
                    if (raised) {
                        CHECK_EQ(dot.plA.x, expected.left);
                        CHECK_EQ(dot.cornerRadius, brailleDotSide(lattice) * 0.5f);
                    } else if (!isOutline) {
                        CHECK(dot.cornerRadius < brailleDotSide(lattice) * 0.5f);
                    }
                }
            }
            CHECK_EQ(index, count);
        }
    }
}

TEST(textAlignmentPreservesTheLatticeAndContainsEveryDot)
{
    for (float height : { 8.0f, 30.6f, 31.0f, 32.0f, 33.0f, 35.0f, 47.3f }) {
        const BrailleLattice grid { 0.3f, 0.7f, fractionalCellWidth, height };
        for (float baseline : { 0.0f, height * 0.8f, height }) {
            const BrailleLattice aligned = textAlignedBrailleLattice(grid, baseline, 12.0f);
            CHECK(pitchSpread(rowEdges(aligned, 20)) < 0.001f);
            CHECK(pitchSpread(columnEdges(aligned, 40)) <= 0.126f);
            for (int row = 0; row < 20; ++row) {
                for (int dotRow = 0; dotRow < brailleDotRows; ++dotRow) {
                    const BrailleDotRect original = brailleDotRect(grid, 0, row, 0, dotRow);
                    const BrailleDotRect dot = brailleDotRect(aligned, 0, row, 0, dotRow);
                    CHECK(std::fabs(dot.top - original.top - (aligned.originY - grid.originY)) < 0.001f);
                    CHECK(dot.top >= grid.originY + row * height - 0.001f);
                    CHECK(dot.bottom <= grid.originY + (row + 1) * height + 0.001f);
                }
            }
        }
    }
}

TEST(rasterSubcellsTileWithoutSeamsAndSquaresKeepTheirGaps)
{
    BrailleLattice lattice = fractionalLattice();
    lattice.originX = 0.3f;
    lattice.originY = 0.7f;
    lattice.style = BrailleStyle::Raster;
    CHECK_EQ(textAlignedBrailleLattice(lattice, 24.0f, 14.0f).originY, lattice.originY);
    for (int row = 0; row < 20; ++row) {
        for (int cell = 0; cell < 40; ++cell) {
            const auto left = brailleDotRect(lattice, cell, row, 0, 0);
            const auto right = brailleDotRect(lattice, cell, row, 1, 0);
            const auto next = brailleDotRect(lattice, cell + 1, row, 0, 0);
            CHECK_EQ(left.right, right.left);
            CHECK_EQ(right.right, next.left);
            for (int dot = 0; dot < brailleDotRows; ++dot) {
                const auto upper = brailleDotRect(lattice, cell, row, 0, dot);
                const auto lower = brailleDotRect(lattice, cell, row + (dot == 3), 0, (dot + 1) % 4);
                CHECK_EQ(upper.bottom, lower.top);
            }
        }
    }
    lattice.style = BrailleStyle::Square;
    CHECK_EQ(brailleDotSide(lattice), 5.0f);
    CHECK(pitchSpread(rowEdges(lattice, 20)) < 0.001f);
    CHECK(pitchSpread(columnEdges(lattice, 40)) <= 0.126f);
    OverlayInstance dots[brailleDotsPerCell] {};
    CHECK_EQ(emitBrailleDots(0x28FF, lattice, 0, 0, { 1, 1, 1, 1 }, dots, brailleDotsPerCell), brailleDotsPerCell);
    for (const auto& dot : dots) {
        CHECK_EQ(dot.cornerRadius, 0.0f);
        CHECK_EQ(dot.strokeWidth, 0.0f);
    }
}

TEST(theDotPitchUsesTheTextPhasePolicyWithoutAccumulatingError)
{
    for (const auto rounding : { PhaseRounding::Nearest, PhaseRounding::Floor }) {
        for (int phases : { 1, 2, 3, 4, 5, 8 }) {
            for (float width : { 9.375f, 14.4f, 17.03125f }) {
                for (float origin : { -0.7f, 0.0f, 0.3f, 37.875f }) {
                    BrailleLattice lattice { origin, 0.7f, width, fractionalCellHeight };
                    lattice.phaseCount = phases;
                    lattice.phaseRounding = rounding;
                    for (int index = 0; index < 1024; ++index) {
                        const auto dot = brailleDotRect(lattice, index / 2, 0, index % 2, 0);
                        const float ideal = origin + (index + 0.5f) * (width / 2.0f);
                        const auto textPhase = phaseForPen(ideal, phases, rounding);
                        const float expected = textPhase.snappedPenX + static_cast<float>(textPhase.phase) / phases;
                        CHECK(std::fabs((dot.left + dot.right) * 0.5f - expected) < 0.002f);
                        const float error = (dot.left + dot.right) * 0.5f - ideal;
                        CHECK(std::fabs(error) <= (rounding == PhaseRounding::Floor ? 1.0f : 0.5f) / phases + 0.002f);
                        if (rounding == PhaseRounding::Floor) {
                            CHECK(error <= 0.002f);
                        }
                    }
                    for (int index = 0; index < 512; ++index) {
                        const auto dot = brailleDotRect(lattice, 0, index / 4, 0, index % 4);
                        const float expected = lattice.originY + (index + 0.5f) * lattice.cellHeight * 0.25f;
                        CHECK(std::fabs((dot.top + dot.bottom) * 0.5f - expected) < 0.002f);
                    }
                }
            }
        }
    }
}

TEST(everyDotIsOneSizeAndTheGapsBetweenThemAreEven)
{
    const BrailleLattice lattice = fractionalLattice();
    const float side = brailleDotSide(lattice);
    CHECK_MSG(side >= 1.0f && side == std::floor(side), "a dot is a whole number of pixels on a side");

    // One size, everywhere on the page. A dot sized from its own snapped
    // sub-cell would alternate 3 and 4 pixels at this advance, and a field of
    // braille would come out mottled with its pitch perfectly even.
    for (int cell = 0; cell < 20; ++cell) {
        for (int column = 0; column < brailleDotColumns; ++column) {
            for (int row = 0; row < brailleDotRows; ++row) {
                const BrailleDotRect dot = brailleDotRect(lattice, cell, cell % 3, column, row);
                CHECK(dot.right - dot.left == side);
                CHECK(std::fabs(dot.bottom - dot.top - side) < 0.001f);
            }
        }
    }

    // The gap across a cell boundary is the gap inside a cell. This is the
    // property a face cannot hold and the reason this file exists: Unifont sets
    // its braille 6 pixels apart inside the cell and 10 across the boundary on
    // a 16 pixel cell, so a plot drawn through it is ruled into cells.
    std::vector<float> horizontal;
    for (int cell = 0; cell < 20; ++cell) {
        const BrailleDotRect inner = brailleDotRect(lattice, cell, 0, 0, 0);
        const BrailleDotRect outer = brailleDotRect(lattice, cell, 0, 1, 0);
        const BrailleDotRect next = brailleDotRect(lattice, cell + 1, 0, 0, 0);
        horizontal.push_back(outer.left - inner.right);
        horizontal.push_back(next.left - outer.right);
    }
    CHECK_MSG(spread(horizontal) <= 0.126f,
        "the horizontal gap is one number whether the two dots share a cell or not");

    std::vector<float> vertical;
    for (int row = 0; row < 20; ++row) {
        for (int dot = 0; dot + 1 < brailleDotRows; ++dot) {
            const BrailleDotRect upper = brailleDotRect(lattice, 0, row, 0, dot);
            const BrailleDotRect lower = brailleDotRect(lattice, 0, row, 0, dot + 1);
            vertical.push_back(lower.top - upper.bottom);
        }
        const BrailleDotRect last = brailleDotRect(lattice, 0, row, 0, brailleDotRows - 1);
        const BrailleDotRect firstOfNext = brailleDotRect(lattice, 0, row + 1, 0, 0);
        vertical.push_back(firstOfNext.top - last.bottom);
    }
    CHECK_MSG(spread(vertical) < 0.001f, "and the vertical gap likewise, down through the rows");
}

TEST(theLatticeIsCentredOnItsCellRatherThanInsetByABearing)
{
    const BrailleLattice lattice = fractionalLattice();

    // The dots are spread over the whole cell, so the space before the first
    // and after the last is half a gap each. That is what carries the pitch
    // through the boundary: the two margins meet and make one ordinary gap. A
    // face keeps its dots grouped and puts the rest of the advance outside
    // them, which is where its pitch jumps.
    for (int cell = 0; cell < 8; ++cell) {
        const BrailleDotRect first = brailleDotRect(lattice, cell, 1, 0, 0);
        const BrailleDotRect last = brailleDotRect(lattice, cell, 1, brailleDotColumns - 1, 0);
        const float cellLeft = std::round(static_cast<float>(cell) * fractionalCellWidth);
        const float cellRight = std::round(static_cast<float>(cell + 1) * fractionalCellWidth);
        const float lead = first.left - cellLeft;
        const float trail = cellRight - last.right;
        CHECK_MSG(std::fabs(lead - trail) <= 1.0f,
            "the lattice sits centred in its cell rather than against one edge");
        const float inner = last.left - first.right;
        CHECK_MSG(std::fabs((lead + trail) - inner) <= 1.0f,
            "and the two margins together make one gap, so the run reads through the cell boundary");
    }
}

TEST(theDotBitsFollowTheBlocksOrderRatherThanARasters)
{
    // Dots 1..6 number DOWN the left column then down the right, and dots 7
    // and 8 were appended later as the bottom row. Read as a raster — left to
    // right, top to bottom — the last two land in the wrong place, and only on
    // the characters that use them, so a plot drawn with six dots would look
    // right and one drawn with eight would not.
    CHECK(isBrailleDotRaised(0x2801, 0, 0)); // dot 1: left column, top
    CHECK(isBrailleDotRaised(0x2802, 0, 1)); // dot 2: left, second
    CHECK(isBrailleDotRaised(0x2804, 0, 2)); // dot 3: left, third
    CHECK(isBrailleDotRaised(0x2808, 1, 0)); // dot 4: RIGHT column, top
    CHECK(isBrailleDotRaised(0x2810, 1, 1)); // dot 5
    CHECK(isBrailleDotRaised(0x2820, 1, 2)); // dot 6
    CHECK(isBrailleDotRaised(0x2840, 0, 3)); // dot 7: bottom left
    CHECK(isBrailleDotRaised(0x2880, 1, 3)); // dot 8: bottom right

    // Each of those raises exactly one dot, which is what stops a bit landing
    // in two places from passing the checks above.
    for (int bit = 0; bit < 8; ++bit) {
        int raised = 0;
        for (int column = 0; column < brailleDotColumns; ++column) {
            for (int row = 0; row < brailleDotRows; ++row) {
                if (isBrailleDotRaised(brailleBlankCodePoint + (1u << bit), column, row))
                    ++raised;
            }
        }
        CHECK_MSG(raised == 1, "one bit raises one dot");
    }

    // The ends of the block: the blank raises nothing, the full pattern raises
    // everything, and a character either side is not braille at all.
    for (int column = 0; column < brailleDotColumns; ++column) {
        for (int row = 0; row < brailleDotRows; ++row) {
            CHECK(!isBrailleDotRaised(0x2800, column, row));
            CHECK(isBrailleDotRaised(0x28FF, column, row));
        }
    }
    CHECK(!isBraille(0x27FF));
    CHECK(isBraille(0x2800));
    CHECK(isBraille(0x28FF));
    CHECK(!isBraille(0x2900));
}

UBIQUITTY_TEST_MAIN()
