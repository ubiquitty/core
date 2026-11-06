// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_bundled_rich_text.cpp — rich text blocks on the software renderer, set
// in the faces Core bundles (Core/Rendering/BundledRichText.cpp,
// Docs/rich-text.md "Rendering"): the table it publishes, a block drawn
// inside its rectangle and clipped there, the selection under the glyphs,
// the ellipsis, the monospace class face as the grid's own glyphs, and a
// static block costing a frame no raster.
//
// Assertions are structural (ink present or absent, where, and in which
// colour), never golden pixels: the glyph shapes belong to the bundled faces.

#include "TerminalFixture.h"

#include "Core/Rendering/BundledRichText.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Rendering/Unifont.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Drawing.h"
#include "Core/RichText/Layout.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty::RichText;
using Ubiquitty::Rendering::BundledRichText;
using Ubiquitty::Rendering::renderTerminalSnapshotRGBA;
using Ubiquitty::Rendering::TerminalSnapshotOptions;
using UbiquittyTest::feed;

namespace {

/// The portable tier's grid: GNU Unifont at twice its size, two pixels a
/// point, so a cell is 16 by 32 pixels and 8 by 16 points.
constexpr int unifontScale = 2;
constexpr double pixelsPerPoint = 2.0;
constexpr int cellWidth = 16;
constexpr int cellHeight = 32;

Ubiquitty::Rendering::CellMetrics gridCell()
{
    return Ubiquitty::Rendering::UnifontRasterizer(unifontScale, 0).cellMetrics();
}

TypographyFacts bundledFacts()
{
    return BundledRichText::typographyFacts(gridCell(), unifontScale, pixelsPerPoint);
}

/// A terminal laid out by the bundled faces, with its cursor hidden.
void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 40);
    CHECK(adoptTypography(t, bundledFacts()));
    feed(t, "\x1b[?25l");
}

TerminalSnapshotOptions frameOptions()
{
    TerminalSnapshotOptions options;
    options.drawCursor = false;
    options.style.unifontScale = unifontScale;
    return options;
}

struct Frame {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;

    [[nodiscard]] const std::uint8_t* pixel(int x, int y) const
    {
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    }
};

Frame render(const Ubiquitty::Terminal& t, const TerminalSnapshotOptions& options, BundledRichText* richText)
{
    Frame frame;
    frame.rgba = renderTerminalSnapshotRGBA(t, options, frame.width, frame.height, {}, nullptr, nullptr, richText);
    return frame;
}

bool isSamePixel(const std::uint8_t* one, const std::uint8_t* other)
{
    return one[0] == other[0] && one[1] == other[1] && one[2] == other[2];
}

/// The pixels two frames of one size disagree on, inside a box and outside it.
struct Difference {
    int inside = 0;
    int outside = 0;
};

Difference differenceOf(const Frame& one, const Frame& other, int left, int top, int right, int bottom)
{
    Difference difference;
    for (int y = 0; y < one.height; ++y) {
        for (int x = 0; x < one.width; ++x) {
            if (isSamePixel(one.pixel(x, y), other.pixel(x, y)))
                continue;
            if (x >= left && x < right && y >= top && y < bottom)
                ++difference.inside;
            else
                ++difference.outside;
        }
    }
    return difference;
}

Document read(std::string_view body)
{
    auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    CHECK(document.has_value());
    return document ? std::move(*document) : Document {};
}

/// The frame's capture as the raster takes it, for its blocks' geometry.
bool capture(const Ubiquitty::Terminal& t, const TerminalSnapshotOptions& options, Ubiquitty::Rendering::TerminalFrameInputs& frame)
{
    return frame.capture(t, options, gridCell(), Ubiquitty::Rendering::GlyphSlotGeometry {});
}

} // namespace

TEST(theTableHoldsTheSansSerifAndMonospaceClassFacesInFourStyles)
{
    const TypographyFacts facts = bundledFacts();
    CHECK(isAdoptable(facts));
    // The grid's 16 by 32 pixel cell at two pixels a point, its baseline
    // 13.5 points down, and the two terminal sizes: the proportional strike's
    // 12-point em and GNU Unifont's 16-point one.
    CHECK_EQ(facts.cellWidth, 8 * 64);
    CHECK_EQ(facts.cellHeight, 16 * 64);
    CHECK_EQ(facts.baseline, 864);
    CHECK_EQ(facts.proportionalSize, 12 * 64);
    CHECK_EQ(facts.monospaceSize, 16 * 64);
    // No shaper: Core lays the blocks out from the advances.
    CHECK_EQ(facts.shaping, 0);
    CHECK_EQ(facts.rendersBlocks, 1);
    REQUIRE(facts.faceCount == 8);
    for (std::size_t index = 0; index < 8; ++index) {
        CHECK(facts.faces[index].familyClass == (index < 4 ? FamilyClass::SansSerif : FamilyClass::Monospace));
        CHECK_EQ(facts.faces[index].styles, static_cast<std::uint8_t>(index % 4));
        CHECK_EQ(facts.faces[index].nameLength, 0);
    }
    const FaceMetrics& sans = facts.faces[0];
    const FaceMetrics& monospace = facts.faces[4];
    const auto advance = [](const FaceMetrics& face, std::uint32_t codePoint) { return face.advances[codePoint - firstTabledCodePoint]; };
    // Noto Sans is proportional; GNU Unifont sets every ASCII letter in half
    // its sixteen-row em, and fills that em from its ascent to its descent.
    CHECK(advance(sans, 'i') < advance(sans, 'n'));
    CHECK(advance(sans, 'n') < advance(sans, 'W'));
    CHECK(advance(facts.faces[1], 'n') >= advance(sans, 'n'));
    CHECK_EQ(advance(monospace, 'i'), 1024);
    CHECK_EQ(advance(monospace, 'W'), 1024);
    CHECK_EQ(monospace.ascent + monospace.descent, 2048);
    CHECK_EQ(monospace.lineHeight, 2048);
    CHECK_EQ(monospace.averageAdvance, 1024);
    // A code point the bundled outlines lack is GNU Unifont's glyph, and the
    // table measures it there.
    CHECK_EQ(advance(sans, 0x180), static_cast<std::uint16_t>(Ubiquitty::Rendering::Unifont::lookup(0x180).width * 128));
    // With no serif face published, a serif entry is set in the sans-serif
    // class face, and a monospace one in the grid's.
    const Document document = read("{\\rtf1{\\fonttbl{\\f0\\froman Times;}{\\f1\\fmodern Courier;}}\\f0 a\\f1 b}");
    REQUIRE(document.runs.size() == 2);
    CHECK(facts.faces[resolveFace(facts, document, document.runs[0])].familyClass == FamilyClass::SansSerif);
    CHECK(facts.faces[resolveFace(facts, document, document.runs[1])].familyClass == FamilyClass::Monospace);
}

TEST(aBlockIsDrawnInsideItsRectangleAndClippedAtItsEdge)
{
    // Without the layer a block's covered cells are its pen's background; with
    // it, ink lands inside the block's ten cells by two rows and nowhere else,
    // although the clipped line runs far past the block's right edge.
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;o=c:w=10:h=2;{\\rtf1\\fs24 A line that runs well past the right edge of its block}\x1b\\");
    const TerminalSnapshotOptions options = frameOptions();
    const Frame bare = render(t, options, nullptr);
    BundledRichText richText;
    const Frame drawn = render(t, options, &richText);
    REQUIRE(!bare.rgba.empty() && !drawn.rgba.empty());
    REQUIRE(bare.width == drawn.width && bare.height == drawn.height);
    REQUIRE(bare.width == 40 * cellWidth && bare.height == 8 * cellHeight);
    const Difference difference = differenceOf(bare, drawn, 0, 0, 10 * cellWidth, 2 * cellHeight);
    CHECK(difference.inside > 0);
    CHECK_EQ(difference.outside, 0);
    // The blank render has no ink in the block: every covered pixel is the
    // pen's background, the frame's corner.
    int bareInk = 0;
    for (int y = 0; y < 2 * cellHeight; ++y) {
        for (int x = 0; x < 10 * cellWidth; ++x)
            bareInk += isSamePixel(bare.pixel(x, y), bare.pixel(bare.width - 1, bare.height - 1)) ? 0 : 1;
    }
    CHECK_EQ(bareInk, 0);
}

TEST(aStaticBlockIsRasterisedOnceAndAReplacementAgain)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;i=1:w=12:h=1;{\\rtf1 Retained}\x1b\\");
    const TerminalSnapshotOptions options = frameOptions();
    BundledRichText richText;
    (void)render(t, options, &richText);
    CHECK_EQ(richText.rasterCount(), std::size_t { 1 });
    // Nothing about the block moved, so its raster is composited as it was.
    (void)render(t, options, &richText);
    CHECK_EQ(richText.rasterCount(), std::size_t { 1 });
    // Placing id 1 again is a new placement with a new serial and key.
    feed(t, "\x1b]7404;i=1:w=12:h=1;{\\rtf1 Replaced}\x1b\\");
    (void)render(t, options, &richText);
    CHECK_EQ(richText.rasterCount(), std::size_t { 2 });
}

TEST(theSelectionIsDrawnUnderTheGlyphsInTheToneOfASelectedCell)
{
    // Behind the text the selection is the opaque wash a selected cell takes:
    // the band's top-left corner, above the lowercase letters' ink, shows the
    // highlight's colour exactly.
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;w=20:h=2;{\\rtf1\\fs24 aaaa aaaa aaaa}\x1b\\");
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::selectBetweenPoints(selection, t, 1.0, 0.3, 6.0, 0.3);
    std::array<BlockSelection, 4> blocks {};
    const int count = Ubiquitty::Selection::blockSelections(selection, t, blocks);
    REQUIRE(count == 1);
    TerminalSnapshotOptions options = frameOptions();
    options.style.selectionStyle = Ubiquitty::Rendering::SelectionStyle::Native;
    options.style.platformSelectionColor = { 0.7f, 0.84f, 1.0f, 0.5f };
    options.richTextSelections = blocks;
    options.richTextSelectionCount = count;
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    REQUIRE(capture(t, options, frame));
    REQUIRE(frame.inputs().isSelectionBehindText);
    const auto highlights = frame.richTextHighlights();
    REQUIRE(!highlights.empty());
    const HighlightQuad& band = highlights[0];
    REQUIRE(band.alpha == 1.0f);

    BundledRichText richText;
    const Frame drawn = render(t, options, &richText);
    REQUIRE(!drawn.rgba.empty());
    const int x = static_cast<int>(std::lround(band.left)) + 1;
    const int y = static_cast<int>(std::lround(band.top)) + 1;
    REQUIRE(x < drawn.width && y < drawn.height);
    const std::uint8_t* pixel = drawn.pixel(x, y);
    CHECK(std::abs(pixel[0] - static_cast<int>(std::lround(band.red * 255.0f))) <= 1);
    CHECK(std::abs(pixel[1] - static_cast<int>(std::lround(band.green * 255.0f))) <= 1);
    CHECK(std::abs(pixel[2] - static_cast<int>(std::lround(band.blue * 255.0f))) <= 1);
    // Without the layer nothing draws the band.
    const Frame bare = render(t, options, nullptr);
    CHECK(!isSamePixel(bare.pixel(x, y), pixel));
}

TEST(anEllipsisIsDrawnWhereTheLayoutCutTheLastLine)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;o=e:w=10:h=1;{\\rtf1\\fs24 This sentence is far too long for one line of the block}\x1b\\");
    const TerminalSnapshotOptions options = frameOptions();
    Ubiquitty::Rendering::TerminalFrameInputs frame;
    REQUIRE(capture(t, options, frame));
    REQUIRE(frame.richTextQuads().size() == 1);
    const BlockQuad& quad = frame.richTextQuads()[0];
    const BlockDrawing& drawing = frame.richTextDrawing(0);
    const auto clusters = drawing.clusters();
    const auto ellipsis = std::ranges::find_if(clusters, [](const DrawnCluster& cluster) { return cluster.isEllipsis != 0; });
    REQUIRE(ellipsis != clusters.end());
    const int left = static_cast<int>(std::floor(quad.left + static_cast<float>(ellipsis->x) * quad.pixelsPerUnitX));
    const int right = static_cast<int>(std::ceil(quad.left + static_cast<float>(ellipsis->x + ellipsis->advance) * quad.pixelsPerUnitX));
    const int top = static_cast<int>(std::floor(quad.top + static_cast<float>(ellipsis->top) * quad.pixelsPerUnitY));
    const int bottom = static_cast<int>(std::ceil(quad.top + static_cast<float>(ellipsis->top + ellipsis->height) * quad.pixelsPerUnitY));

    const Frame bare = render(t, options, nullptr);
    BundledRichText richText;
    const Frame drawn = render(t, options, &richText);
    REQUIRE(!bare.rgba.empty() && !drawn.rgba.empty());
    int ink = 0;
    for (int y = std::max(top, 0); y < std::min(bottom, drawn.height); ++y) {
        for (int x = std::max(left, 0); x < std::min(right, drawn.width); ++x)
            ink += isSamePixel(bare.pixel(x, y), drawn.pixel(x, y)) ? 0 : 1;
    }
    CHECK(ink > 0);
}

TEST(aMonospaceRunAtTheGridsSizeDrawsTheGridsOwnGlyphs)
{
    // A terminal-mode monospace run takes the grid's size and baseline, so
    // GNU Unifont lands on whole texels: the block's `Ab` inks exactly the
    // pixels the grid's `Ab` on the row below does.
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b]7404;g=1:w=4:h=1:C=1;{\\rtf1{\\fonttbl{\\f0\\fmodern;}}\\f0 Ab}\x1b\\\x1b[2;1HAb");
    BundledRichText richText;
    const Frame drawn = render(t, frameOptions(), &richText);
    REQUIRE(!drawn.rgba.empty());
    const std::uint8_t* background = drawn.pixel(drawn.width - 1, drawn.height - 1);
    int blockInk = 0;
    int mismatches = 0;
    for (int y = 0; y < cellHeight; ++y) {
        for (int x = 0; x < 2 * cellWidth; ++x) {
            const bool isBlockInk = !isSamePixel(drawn.pixel(x, y), background);
            const bool isGridInk = !isSamePixel(drawn.pixel(x, y + cellHeight), background);
            blockInk += isBlockInk ? 1 : 0;
            mismatches += isBlockInk == isGridInk ? 0 : 1;
        }
    }
    CHECK(blockInk > 0);
    CHECK_EQ(mismatches, 0);
}

UBIQUITTY_TEST_MAIN()
