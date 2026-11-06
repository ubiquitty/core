// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_box_edge_stretch — the 9-slice edge stretch that makes a font's
// box-drawing glyph tile with its neighbours.
//
// A font draws ─ as a stroke inside its advance, with bearing on both sides. Two
// of them side by side leave a gap at the cell boundary, and the terminal draws a
// dashed line where the user asked for a solid one. The stretch replicates the
// stroke outward from a clean near-edge sample so it reaches the cell edge, and
// must do so without moving the stroke off the row the font chose or thickening
// it — a seam and a smear look equally wrong and have opposite causes.
//
// Everything here is an R8 coverage buffer, so the assertions are on pixels
// rather than on a rendered frame: this is the layer where the geometry is
// decided, and the golden-image tests above it cannot say which of the two
// failures they are seeing.

#include "Harness.h"

#include "Core/Rendering/BoxEdgeStretch.h"

#include <vector>

using Ubiquitty::Rendering::boxReachEdges;
using Ubiquitty::Rendering::edgeStretchBoxScratch;
using Ubiquitty::Rendering::translateScratchVertical;

namespace {

// Box-drawing code points, named so a failure says which glyph rather than
// which number.
constexpr unsigned horizontalLine = 0x2500; // ─
constexpr unsigned verticalLine = 0x2502; // │
constexpr unsigned cross = 0x253C; // ┼
constexpr unsigned notABoxGlyph = 0x41; // 'A'

std::uint8_t& at(std::vector<std::uint8_t>& px, int width, int x, int y)
{
    return px[static_cast<std::size_t>(y) * width + x];
}

} // namespace

TEST(reachedEdgesFollowTheGlyphsStrokes)
{
    bool hasLeftReach = false, hasRightReach = false, hasTopReach = false, hasBottomReach = false;

    CHECK(boxReachEdges(horizontalLine, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach));
    CHECK_MSG(hasLeftReach && hasRightReach, "a horizontal line connects to its left and right neighbours");
    CHECK_MSG(!hasTopReach && !hasBottomReach, "and to nothing above or below, or a column of them would smear");

    CHECK(boxReachEdges(verticalLine, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach));
    CHECK_MSG(hasTopReach && hasBottomReach, "a vertical line connects above and below");
    CHECK_MSG(!hasLeftReach && !hasRightReach, "and not sideways");

    CHECK(boxReachEdges(cross, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach));
    CHECK_MSG(hasLeftReach && hasRightReach && hasTopReach && hasBottomReach, "a cross reaches all four edges");
}

TEST(aGlyphThatIsNotABoxReachesNothing)
{
    // The caller skips the stretch entirely on false. Reporting true with all
    // four edges clear would be the same no-op by a longer route, and would put
    // every letter through the slice path to find that out.
    bool hasLeftReach = true, hasRightReach = true, hasTopReach = true, hasBottomReach = true;
    CHECK_MSG(!boxReachEdges(notABoxGlyph, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach),
        "a letter has no box form, so the caller is told to skip the stretch");
    CHECK_MSG(!hasLeftReach && !hasRightReach && !hasTopReach && !hasBottomReach,
        "the out-parameters are cleared even on the refusal, so a caller reading them is not misled");
}

TEST(aVerticalTranslationMovesRowsAndClearsWhatItVacates)
{
    const int width = 4, height = 4;
    auto fresh = [&] {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                at(px, width, x, y) = static_cast<std::uint8_t>(10 * (y + 1));
        return px;
    };

    // Down two: row 0 and row 1 are vacated, and the old rows 0-1 land at 2-3.
    std::vector<std::uint8_t> down = fresh();
    translateScratchVertical(down.data(), width, height, 2);
    CHECK_MSG(at(down, width, 0, 0) == 0 && at(down, width, 0, 1) == 0,
        "vacated rows are cleared, not hasLeftReach holding what was there");
    CHECK(at(down, width, 0, 2) == 10 && at(down, width, 0, 3) == 20);

    // Up one: the hasTopReach row is discarded and the hasBottomReach is vacated.
    std::vector<std::uint8_t> up = fresh();
    translateScratchVertical(up.data(), width, height, -1);
    CHECK(at(up, width, 0, 0) == 20 && at(up, width, 0, 2) == 40);
    CHECK_MSG(at(up, width, 0, 3) == 0, "the row shifted off the hasBottomReach leaves zero behind");

    // Zero is identity. Asserted because the centring shift is zero for most
    // fonts, so this is the case that runs in production.
    std::vector<std::uint8_t> still = fresh();
    const std::vector<std::uint8_t> before = still;
    translateScratchVertical(still.data(), width, height, 0);
    CHECK_MSG(still == before, "a zero shift is not a copy through a rounding path");

    // A shift past the buffer clears it rather than reading out of bounds.
    std::vector<std::uint8_t> gone = fresh();
    translateScratchVertical(gone.data(), width, height, height + 3);
    bool hasInk = false;
    for (std::uint8_t v : gone)
        hasInk = hasInk || v != 0;
    CHECK_MSG(!hasInk, "a shift past the buffer empties it");
}

TEST(theStrokeReachesTheCellEdgeAndTheCentreIsUntouched)
{
    // A horizontal stroke the font drew inset from both sides: ink on row 3,
    // columns 2..5 of an 8x8 buffer whose cell box is the whole thing.
    const int width = 8, height = 8;
    const int strokeRow = 3;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
    for (int x = 2; x <= 5; ++x)
        at(px, width, x, strokeRow) = 200;

    edgeStretchBoxScratch(px.data(), width, height, /*gx0*/ 0, /*gx1*/ width,
        /*gy0*/ 0, /*gy1*/ height, /*reachL*/ true, /*reachR*/ true,
        /*reachT*/ false, /*reachB*/ false);

    CHECK_MSG(at(px, width, 0, strokeRow) != 0, "the stroke reaches the hasLeftReach cell edge");
    CHECK_MSG(at(px, width, width - 1, strokeRow) != 0, "and the right one");

    // The centre the font drew is what the stretch exists to preserve.
    for (int x = 2; x <= 5; ++x)
        CHECK_MSG(at(px, width, x, strokeRow) == 200, "the font's own centre samples are unchanged");

    // The row is the whole claim. A stretch that widened the stroke would put
    // ink on a neighbouring row, and a column of these would read as a smear
    // rather than a line — which no assertion on the stroke row can catch.
    bool hasInkOffTheStrokeRow = false;
    for (int y = 0; y < height; ++y) {
        if (y == strokeRow)
            continue;
        for (int x = 0; x < width; ++x)
            hasInkOffTheStrokeRow = hasInkOffTheStrokeRow || at(px, width, x, y) != 0;
    }
    CHECK_MSG(!hasInkOffTheStrokeRow, "the stretch stays on the row the font chose");
}

TEST(reachingNoEdgeLeavesTheBufferAlone)
{
    // The refusal path. A glyph whose strokes reach nothing must come back
    // byte-identical, or every non-connecting box character is quietly redrawn.
    const int width = 8, height = 8;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
    for (int x = 2; x <= 5; ++x)
        at(px, width, x, 3) = 200;
    const std::vector<std::uint8_t> before = px;

    edgeStretchBoxScratch(px.data(), width, height, 0, width, 0, height,
        /*reachL*/ false, /*reachR*/ false, /*reachT*/ false, /*reachB*/ false);

    CHECK_MSG(px == before, "no edge reached is a no-op, byte for byte");
}

TEST(aDoubleRuleKeepsItsGapWhileReachingTheRightEdge)
{
    const int width = 12, height = 8;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
    for (int y = 0; y < height; ++y) {
        at(px, width, 5, y) = 200;
        at(px, width, 6, y) = 200;
        at(px, width, 8, y) = 200;
        at(px, width, 9, y) = 150;
    }

    edgeStretchBoxScratch(px.data(), width, height, /*gx0*/ 0, /*gx1*/ width,
        /*gy0*/ 0, /*gy1*/ height, /*reachL*/ false, /*reachR*/ true,
        /*reachT*/ false, /*reachB*/ false);

    for (int y = 0; y < height; ++y) {
        CHECK_MSG(at(px, width, width - 1, y) != 0,
            "the outer rail reaches the right cell edge");
        CHECK_MSG(at(px, width, width - 1, y) == 200,
            "a semi-transparent fringe is replaced by the fully drawn rail behind it");
        CHECK_MSG(at(px, width, 7, y) == 0,
            "the gap between the inner and outer rails stays clear");
        CHECK_MSG(at(px, width, 5, y) == 200 && at(px, width, 6, y) == 200,
            "the inner rail stays where the font drew it");
    }
}

TEST(aDoubleRuleKeepsItsGapWhileReachingTheBottomEdge)
{
    const int width = 8, height = 12;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
    for (int x = 0; x < width; ++x) {
        at(px, width, x, 5) = 200;
        at(px, width, x, 6) = 200;
        at(px, width, x, 8) = 200;
        at(px, width, x, 9) = 150;
    }

    edgeStretchBoxScratch(px.data(), width, height, /*gx0*/ 0, /*gx1*/ width,
        /*gy0*/ 0, /*gy1*/ height, /*reachL*/ false, /*reachR*/ false,
        /*reachT*/ false, /*reachB*/ true);

    for (int x = 0; x < width; ++x) {
        CHECK_MSG(at(px, width, x, height - 1) != 0,
            "the outer rail reaches the bottom cell edge");
        CHECK_MSG(at(px, width, x, height - 1) == 200,
            "a semi-transparent fringe row is replaced by the fully drawn rail above it");
        CHECK_MSG(at(px, width, x, 7) == 0,
            "the gap between the inner and outer rails stays clear");
        CHECK_MSG(at(px, width, x, 5) == 200 && at(px, width, x, 6) == 200,
            "the inner rail stays where the font drew it");
    }
}

TEST(dashesRefuseTheStretchWholesale)
{
    // A dash's decomposition emits edge-reaching stubs like the solid line's,
    // so the geometry probe alone would stretch it — and 9-slicing a dash
    // stretches its end segments, which staggers the pattern under fractional
    // advance and closes the gaps that make it a dash. The refusal is keyed
    // on the code point, ahead of the geometry, so a dash stays exactly as
    // the font drew it while still taking the family's centring shift.
    for (unsigned codePoint = 0x2504; codePoint <= 0x250B; ++codePoint) {
        bool hasLeftReach = true, hasRightReach = true, hasTopReach = true, hasBottomReach = true;
        CHECK_MSG(!boxReachEdges(codePoint, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach),
            "a dash reaches no edge");
        CHECK_MSG(!hasLeftReach && !hasRightReach && !hasTopReach && !hasBottomReach,
            "every flag comes back clear for a dash");
    }
    for (unsigned codePoint = 0x254C; codePoint <= 0x254F; ++codePoint) {
        bool hasLeftReach = true, hasRightReach = true, hasTopReach = true, hasBottomReach = true;
        CHECK_MSG(!boxReachEdges(codePoint, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach),
            "a double dash reaches no edge");
        CHECK_MSG(!hasLeftReach && !hasRightReach && !hasTopReach && !hasBottomReach,
            "every flag comes back clear for a double dash");
    }
    // The solid neighbour still reaches: the refusal is the dash's, not the range's.
    bool hasLeftReach = false, hasRightReach = false, hasTopReach = false, hasBottomReach = false;
    CHECK_MSG(boxReachEdges(0x2500, &hasLeftReach, &hasRightReach, &hasTopReach, &hasBottomReach),
        "the solid line still stretches");
    CHECK_MSG(hasLeftReach && hasRightReach, "a horizontal line reaches both horizontal edges");
}

UBIQUITTY_TEST_MAIN()
