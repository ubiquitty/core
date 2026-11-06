// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "Harness.h"

#include "Core/Rendering/ProportionalRun.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using Ubiquitty::ProportionalShapedGlyph;
using Ubiquitty::Rendering::layoutProportionalRun;
using Ubiquitty::Rendering::ProportionalGlyphPlacement;
using Ubiquitty::Rendering::proportionalRunCaretAt;
using Ubiquitty::Rendering::proportionalRunCellOwners;
using Ubiquitty::Rendering::proportionalRunCodePointAt;
using Ubiquitty::Rendering::ProportionalRunLayout;

namespace {

// A run shaped one glyph per code point at the advance given, with no
// offsets: what a plain Latin word shapes to.
std::vector<ProportionalShapedGlyph> shapeEvenly(std::span<const std::uint32_t> text, float advance)
{
    std::vector<ProportionalShapedGlyph> shaped;
    for (std::size_t i = 0; i < text.size(); ++i) {
        ProportionalShapedGlyph glyph;
        glyph.glyph = static_cast<std::uint32_t>(100 + i);
        glyph.advanceInPixels = advance;
        glyph.cluster = static_cast<std::uint16_t>(i);
        shaped.push_back(glyph);
    }
    return shaped;
}

std::vector<std::uint32_t> text(const char* ascii)
{
    std::vector<std::uint32_t> out;
    for (const char* p = ascii; *p; ++p)
        out.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(*p)));
    return out;
}

bool near(double a, double b) { return a > b - 1e-4 && a < b + 1e-4; }

} // namespace

TEST(aLeftSetRunPlacesEachGlyphAtTheSumOfTheAdvancesBeforeIt)
{
    const std::vector<std::uint32_t> run = text("abc");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 6.5f);
    std::array<ProportionalGlyphPlacement, 8> placements {};
    const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 4, 0, 8.0, placements);
    CHECK_EQ(layout.glyphCount, 3);
    CHECK(near(layout.widthInPixels, 19.5));
    CHECK(near(layout.penOffsetInPixels, 0.0));
    CHECK(near(placements[0].xInPixels, 0.0));
    CHECK(near(placements[1].xInPixels, 6.5));
    CHECK(near(placements[2].xInPixels, 13.0));
    CHECK(placements[2].glyph == 102 && placements[2].cluster == 2);
    // A shaper's own offset rides the glyph and not the pen after it.
    std::vector<ProportionalShapedGlyph> offset = shaped;
    offset[1].offsetXInPixels = -1.0f;
    const ProportionalRunLayout shifted = layoutProportionalRun(offset, run, 4, 0, 8.0, placements);
    CHECK(near(placements[1].xInPixels, 5.5));
    CHECK(near(placements[2].xInPixels, 13.0));
    CHECK(near(shifted.widthInPixels, 19.5));
}

TEST(rightAndCentreAlignmentMoveTheWholeRunByTheSlack)
{
    const std::vector<std::uint32_t> run = text("abc");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 6.0f);
    std::array<ProportionalGlyphPlacement, 8> placements {};
    // 4 cells of 8: 32 available, 18 used, 14 slack.
    const ProportionalRunLayout right = layoutProportionalRun(shaped, run, 4, 1, 8.0, placements);
    CHECK(near(right.penOffsetInPixels, 14.0));
    CHECK(near(placements[0].xInPixels, 14.0));
    CHECK(near(placements[2].xInPixels, 26.0));
    const ProportionalRunLayout centred = layoutProportionalRun(shaped, run, 4, 2, 8.0, placements);
    CHECK(near(centred.penOffsetInPixels, 7.0));
    CHECK(near(placements[0].xInPixels, 7.0));
    // A run wider than its cells has no slack to move by.
    const ProportionalRunLayout overflow = layoutProportionalRun(shaped, run, 2, 1, 8.0, placements);
    CHECK(near(overflow.penOffsetInPixels, 0.0));
}

TEST(justificationOpensEachInteriorSpaceByTheSameAmount)
{
    // "a b c": three letters and two spaces at 4 px each, 20 px of text in
    // 4 cells of 8 = 32, so 12 px of slack opens each gap by 6.
    const std::vector<std::uint32_t> run = text("a b c");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 4.0f);
    std::array<ProportionalGlyphPlacement, 8> placements {};
    const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 4, 3, 8.0, placements);
    CHECK(near(layout.widthInPixels, 32.0));
    CHECK(near(placements[0].xInPixels, 0.0)); // a
    CHECK(near(placements[1].xInPixels, 4.0)); // the space itself does not move
    CHECK(near(placements[2].xInPixels, 14.0)); // b: 8 + one gap of 6
    CHECK(near(placements[4].xInPixels, 28.0)); // c: 16 + two gaps
    // Leading and trailing spaces are not gaps: " a b " opens only the one
    // between the letters.
    const std::vector<std::uint32_t> padded = text(" a b ");
    const std::vector<ProportionalShapedGlyph> paddedShaped = shapeEvenly(padded, 4.0f);
    const ProportionalRunLayout paddedLayout = layoutProportionalRun(paddedShaped, padded, 4, 3, 8.0, placements);
    CHECK(near(paddedLayout.widthInPixels, 32.0));
    CHECK(near(placements[1].xInPixels, 4.0)); // a
    CHECK(near(placements[3].xInPixels, 24.0)); // b: 12 + one gap of 12
}

TEST(justificationOffsetsFollowSourceBoundariesAndReuseStorage)
{
    const std::array<std::uint32_t, 8> source { ' ', 'A', ' ', 0x1D11Eu, ' ', ' ', 'B', ' ' };
    Ubiquitty::Rendering::ProportionalSourceOffsets offsets;
    Ubiquitty::Rendering::proportionalJustificationOffsets(source, 2.5, offsets);
    const std::array<double, 9> expected { 0, 0, 0, 2.5, 2.5, 5, 7.5, 7.5, 7.5 };
    CHECK(offsets.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(near(offsets[index], expected[index]));
    }
    const double* storage = offsets.data();
    Ubiquitty::Rendering::proportionalJustificationOffsets(source, 0.0, offsets);
    CHECK(offsets.data() == storage);
    for (double offset : offsets) {
        CHECK(offset == 0.0);
    }
    Ubiquitty::Rendering::proportionalJustificationOffsets({}, 2.5, offsets);
    CHECK(offsets.size() == 1 && offsets[0] == 0.0);
}

TEST(justificationOpensASourceSpaceOnceWhenGlyphsShareItsCluster)
{
    const std::array<std::uint32_t, 3> source { 'A', ' ', 'B' };
    auto shaped = shapeEvenly(source, 4.0f);
    shaped[1].advanceInPixels = 2.0f;
    ProportionalShapedGlyph secondSpaceGlyph = shaped[1];
    shaped.insert(shaped.begin() + 2, secondSpaceGlyph);
    std::array<ProportionalGlyphPlacement, 4> placements {};
    const auto layout = layoutProportionalRun(shaped, source, 3, 3, 8.0, placements);
    CHECK(near(layout.widthInPixels, 24.0));
    CHECK(near(placements[1].xInPixels, 4.0));
    CHECK(near(placements[2].xInPixels, 6.0));
    CHECK(near(placements[3].xInPixels, 20.0));
}

TEST(justificationOffsetsRemainSourceBasedWhenGlyphClustersReorder)
{
    const auto source = text("A B C");
    auto shaped = shapeEvenly(source, 4.0f);
    shaped[1].cluster = 3;
    shaped[3].cluster = 1;
    std::array<ProportionalGlyphPlacement, 5> placements {};
    const auto layout = layoutProportionalRun(shaped, source, 5, 3, 8.0, placements);
    const std::array<double, 5> expected { 0, 14, 18, 12, 36 };
    CHECK(near(layout.widthInPixels, 40.0));
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(near(placements[index].xInPixels, expected[index]));
    }
}

TEST(everyCodePointBelongsToTheCellItsInkBeginsIn)
{
    // Twelve letters at 6 px in 4 cells of 8: the origins fall at 0,6,12,…
    // so cells own 2,1,1,2,… — and the map says exactly that.
    const std::vector<std::uint32_t> run = text("abcdefghijkl");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 6.0f);
    std::array<ProportionalGlyphPlacement, 16> placements {};
    const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 9, 0, 8.0, placements);
    std::array<std::uint16_t, 16> owners {};
    proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), 12), layout, 12, 9, 8.0, owners);
    const std::array<std::uint16_t, 12> wanted { 0, 0, 1, 2, 3, 3, 4, 5, 6, 6, 7, 8 };
    for (std::size_t i = 0; i < 12; ++i)
        CHECK_EQ(static_cast<int>(owners[i]), static_cast<int>(wanted[i]));
    // A right-aligned run's owners follow its ink — "ab" at 6 px in 4 cells
    // sits at 20 px, so a is cell 2 and b is cell 3: the first code point's
    // cell is where its ink begins like every other's, however far the
    // alignment put it from the anchor.
    const std::vector<std::uint32_t> pair = text("ab");
    const std::vector<ProportionalShapedGlyph> pairShaped = shapeEvenly(pair, 6.0f);
    const ProportionalRunLayout right = layoutProportionalRun(pairShaped, pair, 4, 1, 8.0, placements);
    proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), 2), right, 2, 4, 8.0, owners);
    CHECK_EQ(static_cast<int>(owners[0]), 2);
    CHECK_EQ(static_cast<int>(owners[1]), 3);
    // A code point a ligature absorbed rides the ligature's cell: "fi" as
    // one glyph at cluster 0, then "x".
    const std::vector<std::uint32_t> lig = text("fix");
    std::vector<ProportionalShapedGlyph> ligShaped;
    ProportionalShapedGlyph fi;
    fi.glyph = 7;
    fi.advanceInPixels = 9.0f;
    fi.cluster = 0;
    ProportionalShapedGlyph x;
    x.glyph = 8;
    x.advanceInPixels = 6.0f;
    x.cluster = 2;
    ligShaped.push_back(fi);
    ligShaped.push_back(x);
    const ProportionalRunLayout ligLayout = layoutProportionalRun(ligShaped, lig, 3, 0, 8.0, placements);
    proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), 2), ligLayout, 3, 3, 8.0, owners);
    CHECK_EQ(static_cast<int>(owners[0]), 0);
    CHECK_EQ(static_cast<int>(owners[1]), 0);
    CHECK_EQ(static_cast<int>(owners[2]), 1);
    // The owner never leaves the claim: a run overflowing its cells clamps
    // to the last one rather than naming a cell the run does not hold.
    const ProportionalRunLayout squeezed = layoutProportionalRun(shaped, run, 2, 0, 8.0, placements);
    proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), 12), squeezed, 12, 2, 8.0, owners);
    CHECK_EQ(static_cast<int>(owners[11]), 1);
    // A full-row claim keeps its last cell addressable rather than folding it
    // into an eight-bit index.
    const ProportionalRunLayout fullRow = layoutProportionalRun(pairShaped, pair, 1024, 1, 8.0, placements);
    proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), 2), fullRow, 2, 1024, 8.0, owners);
    CHECK_EQ(static_cast<int>(owners[0]), 1022);
    CHECK_EQ(static_cast<int>(owners[1]), 1023);
}

// A long packed row: 600 glyphs in a 1024-cell claim, then 1024, each at the
// index it was shaped at. The placements keep source order, the cluster an
// index a byte cannot hold, and the owners the cells past column 255 — the
// widths the protocol allows and an eight-bit anything would fold.
TEST(aLongPackedRowKeepsItsOrderAndOwnersPastTheByteBoundaries)
{
    for (const int count : { 600, 1024 }) {
        std::vector<std::uint32_t> run;
        for (int i = 0; i < count; ++i)
            run.push_back(static_cast<std::uint32_t>('a' + (i % 26)));
        // Eight pixels each in eight-pixel cells: glyph i begins in cell i.
        const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 8.0f);
        std::vector<ProportionalGlyphPlacement> placements(static_cast<std::size_t>(count));
        const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 1024, 0, 8.0, placements);
        CHECK_EQ(layout.glyphCount, count);
        for (int i = 0; i < count; ++i) {
            CHECK_EQ(static_cast<int>(placements[static_cast<std::size_t>(i)].cluster), i);
            CHECK(near(placements[static_cast<std::size_t>(i)].xInPixels, 8.0 * i));
            if (i > 0)
                CHECK(placements[static_cast<std::size_t>(i)].xInPixels > placements[static_cast<std::size_t>(i - 1)].xInPixels);
        }
        std::vector<std::uint16_t> owners(static_cast<std::size_t>(count));
        proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(count)), layout, count, 1024, 8.0, owners);
        CHECK_EQ(static_cast<int>(owners[255]), 255);
        CHECK_EQ(static_cast<int>(owners[256]), 256);
        CHECK_EQ(static_cast<int>(owners[599]), 599);
        CHECK_EQ(static_cast<int>(owners[static_cast<std::size_t>(count - 1)]), count - 1);
        // The pixel past column 255 names the code point there, and the last
        // pixel of the claim the last one.
        CHECK_EQ(proportionalRunCodePointAt(std::span<const ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(count)), layout, count, 256.0 * 8.0 + 1.0), 256);
        CHECK_EQ(proportionalRunCodePointAt(std::span<const ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(count)), layout, count, count * 8.0 - 1.0), count - 1);
    }
}

TEST(aPixelNamesTheCodePointWhoseGlyphHoldsIt)
{
    const std::vector<std::uint32_t> run = text("abc");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 6.0f);
    std::array<ProportionalGlyphPlacement, 8> placements {};
    const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 4, 2, 8.0, placements);
    const std::span<const ProportionalGlyphPlacement> placed(placements.data(), 3);
    // Centred: 7 px of slack before the text. Before it is the first letter.
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 2.0), 0);
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 7.5), 0);
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 13.5), 1);
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 24.9), 2);
    // Past the text names the end: a caret dropped there lands after the
    // last letter, not on it.
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 25.0), 3);
    CHECK_EQ(proportionalRunCodePointAt(placed, layout, 3, 60.0), 3);
    // An empty layout answers the start.
    CHECK_EQ(proportionalRunCodePointAt(placed, ProportionalRunLayout {}, 3, 10.0), 0);
}

// The hit test names the letter a pixel is on; a caret sits between letters,
// so the same pixel rounds to the nearer of the letter's two edges.
TEST(aPixelNamesTheCaretItIsNearestAndSaysWhichWayItRounded)
{
    const std::vector<std::uint32_t> run = text("abc");
    const std::vector<ProportionalShapedGlyph> shaped = shapeEvenly(run, 6.0f);
    std::array<ProportionalGlyphPlacement, 8> placements {};
    const ProportionalRunLayout layout = layoutProportionalRun(shaped, run, 4, 0, 8.0, placements);
    const std::span<const ProportionalGlyphPlacement> placed(placements.data(), 3);
    // Left-set: the glyphs stand at 0, 6 and 12, and the text ends at 18.
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 0.0).codePointIndex, 0);
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 2.9).codePointIndex, 0);
    CHECK(!proportionalRunCaretAt(placed, layout, 3, 2.9).didRoundForward);
    // Past a glyph's middle the caret is the one after it, and the answer
    // records the direction so a grapheme correction can follow the pixel.
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 3.1).codePointIndex, 1);
    CHECK(proportionalRunCaretAt(placed, layout, 3, 3.1).didRoundForward);
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 8.9).codePointIndex, 1);
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 9.1).codePointIndex, 2);
    // The last glyph's far edge is the position after the text.
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 15.1).codePointIndex, 3);
    CHECK_EQ(proportionalRunCaretAt(placed, layout, 3, 60.0).codePointIndex, 3);
    // An empty layout answers the start.
    CHECK_EQ(proportionalRunCaretAt(placed, ProportionalRunLayout {}, 3, 10.0).codePointIndex, 0);
}

UBIQUITTY_TEST_MAIN()
