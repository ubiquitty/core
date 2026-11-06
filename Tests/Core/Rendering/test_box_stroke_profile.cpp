// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_box_stroke_profile.cpp — the measured stroke profile and the strokes
// drawn from it.
//
// The profile exists so a procedural box character stitches with the strokes
// the font draws itself, and the tests pin that from both ends: the scanner's
// fold from a reference bitmap to a band, and the builder's use of the band —
// the same centreline and thickness for a healed character as for the rule it
// continues. The dentistry corners ride along because they are the family the
// procedural path exists for: no monospace face ships them, and the system
// cascade substitutes a hairline from a face matching nothing else on screen.

#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxGlyphRaster.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Unicode/Width.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Ubiquitty::Rendering;
using namespace Ubiquitty::Rendering::BoxDrawing;

namespace {

constexpr int cellWidth = 10;
constexpr int cellHeight = 20;

// A synthetic reference bitmap: full-width horizontal bands at the given row
// ranges, everything else clear.
std::vector<std::uint8_t> horizontalBands(std::initializer_list<std::pair<int, int>> bands)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(cellWidth) * cellHeight, 0);
    for (const auto& [first, last] : bands) {
        for (int y = first; y <= last; ++y)
            for (int x = 0; x < cellWidth; ++x)
                coverage[static_cast<std::size_t>(y) * cellWidth + x] = 255;
    }
    return coverage;
}

// The columns of a rasterised glyph that carry ink at the cell's mid height.
std::vector<int> inkedColumnsAtRow(const std::vector<std::uint8_t>& coverage, int y)
{
    std::vector<int> columns;
    for (int x = 0; x < cellWidth; ++x) {
        if (coverage[static_cast<std::size_t>(y) * cellWidth + x] >= 128)
            columns.push_back(x);
    }
    return columns;
}

// Above the centre, where an elbow has only the arm it shares with a rule.
// The centre row carries the other arm as well, and a comparison there reads
// the two arms as a disagreement about one band.
constexpr int upperArmRow = cellHeight / 4;

std::vector<int> inkedRowsAtColumn(const std::vector<std::uint8_t>& coverage, int column)
{
    std::vector<int> rows;
    for (int y = 0; y < cellHeight; ++y) {
        if (coverage[static_cast<std::size_t>(y) * cellWidth + column] >= 128)
            rows.push_back(y);
    }
    return rows;
}

std::vector<std::uint8_t> rasterise(unsigned codePoint, const BoxStrokeProfile* profile)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(cellWidth) * cellHeight, 0);
    CHECK(rasterizeBoxGlyph(codePoint, { cellWidth, cellHeight }, { cellWidth, cellHeight },
        static_cast<float>(cellHeight), coverage, profile));
    return coverage;
}

} // namespace

/// One stroke folds to its centre and thickness, in unit cell coordinates.
TEST(scanFoldsASingleStrokeToItsBand)
{
    const std::vector<std::uint8_t> coverage = horizontalBands({ { 10, 11 } });
    BoxStrokeProfile profile;
    foldBoxReferenceScan(&profile, 0x2500, coverage, cellWidth, cellHeight);
    CHECK(profile.lightHorizontal.isValid);
    CHECK(profile.lightHorizontal.thicknessInCell == 2.0f / cellHeight);
    CHECK(profile.lightHorizontal.centerInCell == 11.0f / cellHeight);
    CHECK(!profile.lightVertical.isValid); // one reference feeds one band
}

/// A rail pair folds to rails, gap, and their shared midpoint.
TEST(scanFoldsADoubleRuleToRailsAndGap)
{
    const std::vector<std::uint8_t> coverage = horizontalBands({ { 8, 9 }, { 12, 13 } });
    BoxStrokeProfile profile;
    foldBoxReferenceScan(&profile, 0x2550, coverage, cellWidth, cellHeight);
    CHECK(profile.doubleHorizontal.isValid);
    CHECK(profile.doubleHorizontal.railThicknessInCell == 2.0f / cellHeight);
    CHECK(profile.doubleHorizontal.gapInCell == 2.0f / cellHeight);
    CHECK(profile.doubleHorizontal.centerInCell == 11.0f / cellHeight);
}

/// A bitmap the scanner does not understand leaves the band invalid.
///
/// Invalid falls back to the em rule; a mis-measured band would mis-draw
/// every healed character, so refusing is the only safe answer.
TEST(scanRefusesWhatItCannotName)
{
    BoxStrokeProfile profile;
    foldBoxReferenceScan(&profile, 0x2500, horizontalBands({}), cellWidth, cellHeight);
    CHECK(!profile.lightHorizontal.isValid); // blank

    foldBoxReferenceScan(&profile, 0x2500, horizontalBands({ { 2, 3 }, { 8, 9 }, { 14, 15 } }), cellWidth, cellHeight);
    CHECK(!profile.lightHorizontal.isValid); // three runs

    foldBoxReferenceScan(&profile, 0x2550, horizontalBands({ { 4, 4 }, { 10, 15 } }), cellWidth, cellHeight);
    CHECK(!profile.doubleHorizontal.isValid); // rails nothing like each other
}

// A reference bitmap with per-row coverage values, for the antialiased
// cases the flat-band helper cannot express.
std::vector<std::uint8_t> gradedHorizontalBand(std::initializer_list<std::pair<int, std::uint8_t>> rows)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(cellWidth) * cellHeight, 0);
    for (const auto& [row, value] : rows) {
        for (int x = 0; x < cellWidth; ++x)
            coverage[static_cast<std::size_t>(row) * cellWidth + x] = value;
    }
    return coverage;
}

/// An antialiased stroke folds to its optical mass, not its threshold extent.
///
/// A 255+153 row pair spans two threshold rows but weighs 1.6px; the extent
/// answer (2px, painted solid) is how a healed rule ends up visibly heavier
/// than the font's own antialiased rule beside it.
TEST(scanFoldsAnAntialiasedStrokeToItsMass)
{
    const std::vector<std::uint8_t> coverage = gradedHorizontalBand({ { 10, 255 }, { 11, 153 } });
    BoxStrokeProfile profile;
    foldBoxReferenceScan(&profile, 0x2500, coverage, cellWidth, cellHeight);
    CHECK(profile.lightHorizontal.isValid);
    const float mass = profile.lightHorizontal.thicknessInCell * cellHeight;
    CHECK_MSG(mass > 1.55f && mass < 1.65f, "thickness is the coverage integral, not the run length");
    // The centroid leans toward the heavier row: (1.0*10.5 + 0.6*11.5)/1.6.
    const float center = profile.lightHorizontal.centerInCell * cellHeight;
    CHECK_MSG(center > 10.8f && center < 10.95f, "the centre is the ink centroid");
}

/// A fractional band draws its measured mass with alpha edges, not whole rows.
///
/// The subject is an edge line, the family that carries the exact measurement:
/// a stub's band is rounded to whole pixels because every join closes against
/// it, and rounding 1.6px to two full rows is how a healed rule ends up
/// heavier than the font's own. The control is the whole-pixel profile beside
/// it: the same code must degenerate to the hard band a hard-edged reference
/// measured, or every Unifont-profiled arm re-mints.
TEST(aFractionalBandRendersItsMeasuredMass)
{
    const int column = cellWidth / 2;
    auto weigh = [](const std::vector<std::uint8_t>& coverage, float& mass, int& full, int& partial) {
        mass = 0.0f;
        full = 0;
        partial = 0;
        for (int y = 0; y < cellHeight; ++y) {
            const std::uint8_t pixel = coverage[static_cast<std::size_t>(y) * cellWidth + column];
            mass += static_cast<float>(pixel) / 255.0f;
            if (pixel == 255)
                ++full;
            else if (pixel != 0)
                ++partial;
        }
    };

    BoxStrokeProfile profile;
    profile.lightHorizontal.isValid = true;
    profile.lightHorizontal.centerInCell = 0.25f;
    profile.lightHorizontal.thicknessInCell = 1.6f / cellHeight;
    float mass = 0.0f;
    int full = 0;
    int partial = 0;
    weigh(rasterise(0x23BB, &profile), mass, full, partial);
    CHECK_MSG(mass > 1.5f && mass < 1.7f, "the drawn column weighs what the reference weighed");
    CHECK_MSG(partial > 0, "a 1.6px band has alpha edges rather than whole rows only");

    BoxStrokeProfile wholeProfile;
    wholeProfile.lightHorizontal.isValid = true;
    wholeProfile.lightHorizontal.centerInCell = 0.25f;
    wholeProfile.lightHorizontal.thicknessInCell = 2.0f / cellHeight;
    float hardMass = 0.0f;
    int hardFull = 0;
    int hardPartial = 0;
    weigh(rasterise(0x23BB, &wholeProfile), hardMass, hardFull, hardPartial);
    CHECK_MSG(hardFull == 2 && hardPartial == 0, "a whole-pixel band keeps the hard form a hard reference measured");
}

/// An elbow's vertical continues the `│` it stacks under, band for band.
///
/// This is the stitch the whole profile exists for, asserted at its sharpest:
/// the columns `└` inks above its corner are exactly the columns `│` inks, both
/// under a measured profile whose centre sits off the geometric middle and
/// under no profile at all.
TEST(anElbowSharesTheVerticalBandOfTheRuleItContinues)
{
    CHECK(inkedColumnsAtRow(rasterise(0x2514, nullptr), upperArmRow) == inkedColumnsAtRow(rasterise(0x2502, nullptr), upperArmRow));

    BoxStrokeProfile profile;
    profile.lightVertical.isValid = true;
    // Off the geometric middle, so a builder that ignored the measured
    // centre and kept the cell-centre snap would still pass the equality
    // below — both arms would agree at the default — and the control after
    // it is what catches that.
    profile.lightVertical.centerInCell = 0.3f;
    profile.lightVertical.thicknessInCell = 3.0f / cellWidth;
    const std::vector<int> elbow = inkedColumnsAtRow(rasterise(0x2514, &profile), upperArmRow);
    CHECK(elbow == inkedColumnsAtRow(rasterise(0x2502, &profile), upperArmRow));
    CHECK_MSG(elbow != inkedColumnsAtRow(rasterise(0x2502, nullptr), upperArmRow),
        "the measured centre moved the band, so the agreement above is not vacuous");
}

/// An elbow's horizontal arm shares the rule's band, not a thicker one.
///
/// The arm reaches the cell edge it points at, so the far column on that side
/// is where the two must agree: an elbow drawn from a heavier basis than the
/// `─` it meets shows up as a different set of inked rows there, and a join
/// between them then steps.
TEST(anElbowSharesTheHorizontalBandOfTheRule)
{
    const int farRightColumn = cellWidth - 1;
    const int farLeftColumn = 0;
    const std::vector<int> rule = inkedRowsAtColumn(rasterise(0x2500, nullptr), farRightColumn);
    CHECK(inkedRowsAtColumn(rasterise(0x250C, nullptr), farRightColumn) == rule);
    CHECK(inkedRowsAtColumn(rasterise(0x2514, nullptr), farRightColumn) == rule);
    CHECK(inkedRowsAtColumn(rasterise(0x2510, nullptr), farLeftColumn) == rule);
    CHECK(inkedRowsAtColumn(rasterise(0x2518, nullptr), farLeftColumn) == rule);
}

/// An edge line hugs its edge from inside the cell.
///
/// A band centred on the edge itself would half-clip to a hairline and paint
/// the neighbouring cell with the other half.
TEST(anEdgeLineSitsFlushInsideItsCell)
{
    const std::vector<std::uint8_t> left = rasterise(0x23B8, nullptr);
    const int y = cellHeight / 2;
    CHECK_MSG(left[static_cast<std::size_t>(y) * cellWidth + 0] >= 128, "the left vertical reaches column zero");

    const std::vector<std::uint8_t> right = rasterise(0x23B9, nullptr);
    CHECK_MSG(right[static_cast<std::size_t>(y) * cellWidth + (cellWidth - 1)] >= 128, "the right vertical reaches the last column");

    const std::vector<std::uint8_t> bottom = rasterise(0x23BD, nullptr);
    int inkedRowsPastCell = 0;
    for (int x = 0; x < cellWidth; ++x)
        if (bottom[static_cast<std::size_t>(cellHeight - 1) * cellWidth + x] >= 128)
            ++inkedRowsPastCell;
    CHECK_MSG(inkedRowsPastCell == cellWidth, "scan line 9 reaches the bottom row across the cell");
}

/// A diagonal reaches its intended corners without touching the opposite ones.
TEST(aDiagonalEndsOnItsCorners)
{
    const std::vector<std::uint8_t> slash = rasterise(0x2571, nullptr); // U+2571, lower left to upper right
    const auto inked = [&slash](int x, int y) { return slash[static_cast<std::size_t>(y) * cellWidth + x] >= 128; };

    CHECK_MSG(inked(0, cellHeight - 1), "the lower-left corner carries ink");
    CHECK_MSG(inked(cellWidth - 1, 0), "the upper-right corner carries ink");

    // The corners the stroke never runs to. A cap spilling past the ends
    // reaches these before it reaches anything a reader would call wrong, so
    // they are where an untrimmed capsule shows up first.
    CHECK_MSG(!inked(0, 0), "the upper-left corner is clear");
    CHECK_MSG(!inked(cellWidth - 1, cellHeight - 1), "the lower-right corner is clear");
}

/// Every row of a diagonal carries its stroke through the cell.
TEST(aDiagonalTilesWithTheCellBelowIt)
{
    const std::vector<std::uint8_t> backslash = rasterise(0x2572, nullptr); // U+2572, upper left to lower right
    const auto inked = [&backslash](int x, int y) { return backslash[static_cast<std::size_t>(y) * cellWidth + x] >= 128; };

    CHECK_MSG(inked(0, 0), "the upper-left corner carries ink");
    CHECK_MSG(inked(cellWidth - 1, cellHeight - 1), "the lower-right corner carries ink");

    // Every row holds some of the stroke: a trim deep enough to open a seam at
    // a join takes rows off one end first.
    int emptyRows = 0;
    for (int y = 0; y < cellHeight; ++y) {
        bool isRowInked = false;
        for (int x = 0; x < cellWidth && !isRowInked; ++x)
            isRowInked = inked(x, y);
        if (!isRowInked)
            ++emptyRows;
    }
    CHECK_MSG(emptyRows == 0, "the stroke spans every row of its cell");
}

/// Tiled diagonal coverage matches one uninterrupted stroke across the join.
TEST(diagonalJoinsPreserveStrokeCoverage)
{
    constexpr int margin = 4;
    constexpr std::array sizes { Ubiquitty::IntSize { 7, 17 }, Ubiquitty::IntSize { 14, 34 }, Ubiquitty::IntSize { 15, 33 } };
    for (const auto cell : sizes) {
        const float weight = cell.width < 10 ? 1.0f : 2.0f;
        BoxStrokeProfile profile;
        profile.lightVertical = { true, 0.5f, weight / cell.width };
        profile.lightHorizontal = { true, 0.5f, weight / cell.height };
        const Ubiquitty::IntSize slot { cell.width + 2 * margin, cell.height + 2 * margin };
        for (const unsigned codePoint : { 0x2571U, 0x2572U }) {
            std::vector<std::uint8_t> mask(static_cast<std::size_t>(slot.width) * slot.height);
            CHECK(rasterizeBoxGlyph(codePoint, slot, cell, cell.height, mask, &profile));
            const auto coverageAt = [&](int x, int y) -> float {
                if (x < 0 || y < 0 || x >= slot.width || y >= slot.height)
                    return 0.0f;
                const int column = codePoint == 0x2571 ? slot.width - 1 - x : x;
                return mask[static_cast<std::size_t>(y) * slot.width + column] / 255.0f;
            };
            const float halfWidth = std::max(0.5f, weight * 0.4f);
            const float length = std::hypot(static_cast<float>(cell.width), static_cast<float>(cell.height));
            int mismatches = 0;
            for (int y = cell.height - 3; y < cell.height + 3; ++y) {
                for (int x = cell.width - 4; x < cell.width + 4; ++x) {
                    const float first = coverageAt(x + margin, y + margin);
                    const float second = coverageAt(x + margin - cell.width, y + margin - cell.height);
                    const float composed = first + second * (1.0f - first);
                    const float distance = std::abs((x + 0.5f) * cell.height - (y + 0.5f) * cell.width) / length;
                    const float expected = std::clamp(halfWidth + 0.5f - distance, 0.0f, 1.0f);
                    if (std::abs(composed - expected) > 1.5f / 255.0f)
                        ++mismatches;
                }
            }
            CHECK_MSG(mismatches == 0, "joining cells neither lose stroke coverage nor darken the antialiased edge");
        }
    }
}

UBIQUITTY_TEST_MAIN()
