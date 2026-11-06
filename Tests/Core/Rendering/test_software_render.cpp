// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include <memory>

// test_software_render — drives the portable SoftwareRenderer end-to-end (the
// shared buildCellInstances() + a real CPU R8 Unifont atlas + the
// mix(background,foreground,coverage) compositor) over a canned grid, asserting composited
// pixels: a space cell is pure background, a glyph cell carries its foreground
// where Unifont has ink (and background in an empty corner), and a wide cell's
// second half is painted (the continuation isn't a gap).

#include "Core/Rendering/SizedMaster.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include "Core/Image/TerminalImage.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/SoftFont.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {
constexpr int renderCellWidth = 8;
constexpr int renderCellHeight = 16;

struct Pixel {
    int red, green, blue, alpha;
};

Pixel at(const std::vector<std::uint8_t>& rgba, int width, int x, int y)
{
    const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
    return { rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3] };
}

void setCell(std::vector<Ubiquitty::Cell>& grid, int columns, int row, int column, uint32_t ch, uint16_t attributes, uint16_t foregroundIndex, uint16_t backgroundIndex)
{
    Ubiquitty::Cell& c = grid[static_cast<std::size_t>(row) * columns + column];
    c.codePoint = ch;
    c.pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(attributes);
    c.pen.foregroundIndex = foregroundIndex;
    c.pen.backgroundIndex = backgroundIndex;
    c.pen.underlineIndex = colorDefault;
}

int resolveTerminalCluster(const Ubiquitty::Cell* cell, std::span<std::uint32_t> codePoints, void* context)
{
    return Ubiquitty::cellClusterCodePoints(static_cast<const Ubiquitty::Terminal*>(context), cell, codePoints.data(), static_cast<int>(codePoints.size()));
}

// A viewport smaller than the grid it was built for — a window shrunk before
// the grid follows it — leaves the cells at the right and bottom reaching past
// the surface. What a composite writes past the end of a row lands at the start
// of the next one, and what it writes past the last row lands outside the
// buffer entirely, so the clip is what stands between a stale viewport and a
// heap write.
TEST(testRenderClipsCellsReachingPastTheViewport)
{
    constexpr int rows = 4;
    constexpr int columns = 8;
    std::vector<Ubiquitty::Cell> grid(rows * columns, Ubiquitty::defaultBlankCell);
    // The columns and the row that fall outside the viewport are backed green,
    // everything the viewport holds red. Green anywhere in the frame is then a
    // cell drawn from outside the surface into it.
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const bool isOutsideViewport = column >= columns - 2 || row >= rows - 1;
            setCell(grid, columns, row, column, ' ', 0, 7, isOutsideViewport ? 2 : 1);
        }
    }

    Ubiquitty::TermPalette palette;
    palette.seedDefaults();

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    // Two columns and one row short of what the grid needs.
    in.viewportSizeInPixels = { static_cast<float>((columns - 2) * renderCellWidth), static_cast<float>((rows - 1) * renderCellHeight - renderCellHeight / 2) };

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(in, rgba, width, height));
    CHECK(width == (columns - 2) * renderCellWidth);
    CHECK(height == (rows - 1) * renderCellHeight - renderCellHeight / 2);
    CHECK(rgba.size() == static_cast<std::size_t>(width) * height * 4);

    int greenInsideTheViewport = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel p = at(rgba, width, x, y);
            if (p.green > p.red)
                ++greenInsideTheViewport;
        }
    }
    CHECK_MSG(greenInsideTheViewport == 0,
        "the frame wears the colour of the cells outside the viewport, which is a span drawn off the end of its row");
}

// A base letter with a combining acute is one cell carrying a cluster. Drawn
// through the cluster resolver the cell shows the mark; drawn as its base code
// point alone it does not, so the two renders differ in their pixels while
// agreeing on their size.
TEST(aCombiningMarkReachesTheCompositeThroughTheClusterResolver)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 1, 2);
    Ubiquitty::processBytes(&terminal, std::string_view("e\xCC\x81", 3));

    std::array<Ubiquitty::Cell, 2> cells {};
    Ubiquitty::Screen::getDisplayRow(terminal, 0, 0, cells.data(), static_cast<int>(cells.size()));
    REQUIRE(cells[0].combiningLength() == 1);

    const auto renderRow = [&](const Ubiquitty::Cell* row, BuildInputs::ClusterResolver resolver, std::vector<std::uint8_t>& rgba) {
        BuildInputs inputs;
        inputs.cells = row;
        inputs.clusterResolver = resolver;
        inputs.clusterResolverContext = resolver ? &terminal : nullptr;
        inputs.rows = 1;
        inputs.columns = 2;
        inputs.palette = &terminal.palette;
        SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
        int width = 0;
        int height = 0;
        return renderer.render(inputs, rgba, width, height);
    };

    std::vector<std::uint8_t> withMark;
    CHECK(renderRow(cells.data(), resolveTerminalCluster, withMark));

    std::array<Ubiquitty::Cell, 2> baseOnly = cells;
    baseOnly[0].setCombiningLength(0);
    baseOnly[0].setClusterID(0);
    std::vector<std::uint8_t> withoutMark;
    CHECK(renderRow(baseOnly.data(), nullptr, withoutMark));

    CHECK_EQ(withMark.size(), withoutMark.size());
    CHECK_MSG(withMark != withoutMark, "the acute drew no pixel the bare letter did not");
}

// The showcase failure ubiquitty-frontends filed as `text_variation_selectors`.
// VS16 widens its base and tags the cell emoji, so the cell is wide and carries
// a cluster at once — the one combination where the page a UV is read against
// and the page it was written to disagree. Such a cell draws nothing at all on
// a renderer where its base code point alone draws fine.
//
// Ink is counted as difference from the render's own background rather than
// against a brightness threshold: this theme's background is light, so a
// threshold picked from the ink's side counts every pixel and cannot fail.
TEST(testVariationSelectorSequencesInk)
{
    struct Sample {
        const char* label;
        const char* bare;
        const char* selected;
    };
    const Sample samples[] = {
        { "U+26A0 warning sign", "\xE2\x9A\xA0", "\xE2\x9A\xA0\xEF\xB8\x8F" },
        { "U+2764 heart", "\xE2\x9D\xA4", "\xE2\x9D\xA4\xEF\xB8\x8F" },
        { "U+2709 envelope", "\xE2\x9C\x89", "\xE2\x9C\x89\xEF\xB8\x8F" },
        // Not an emoji: CJK is wide on its own and takes no Emoji attribute, so
        // it belongs here only because the defect is about width and clustering
        // rather than about emoji.
        { "U+4E00 CJK", "\xE4\xB8\x80", "\xE4\xB8\x80\xEF\xB8\x8F" },
    };

    auto inkOf = [](const char* utf8) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 1, 4);
        Ubiquitty::processBytes(&terminal, std::string_view(utf8));

        Ubiquitty::Cell row[4] {};
        Ubiquitty::Screen::getDisplayRow(terminal, 0, 0, row, 4);
        BuildInputs in;
        in.cells = row;
        in.clusterResolver = resolveTerminalCluster;
        in.clusterResolverContext = &terminal;
        in.rows = 1;
        in.columns = 4;
        in.palette = &terminal.palette;

        SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        if (!renderer.render(in, pixels, width, height) || pixels.size() < 4)
            return -1L;
        long lit = 0;
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
            lit += (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2]) ? 1 : 0;
        return lit;
    };

    for (const Sample& sample : samples) {
        CHECK_MSG(inkOf(sample.bare) > 0, sample.label);
        CHECK_MSG(inkOf(sample.selected) > 0, sample.label);
    }
}

} // namespace

namespace {
// Inline-image draw: place a solid-red DEC sixel on a Terminal, hand the
// renderer the Terminal's image / placement tables via BuildInputs, and assert the
// image region is red (the sixel) over a dark background while a cell outside the
// image keeps its background — the sample + straight-alpha over-blend + clip the
// CPU image pass adds.
TEST(testImageDraw)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 4, 8);

    // reg 1 = 100% red, select it, repeat the all-six-bits column 16x -> a
    // 16x6 solid-red image anchored at home (cell row 0).
    const char sixel[] = "\x1bP0;0;0q#1;2;100;0;0#1!16~\x1b\\";
    Ubiquitty::processBytes(t.get(), sixel);
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);

    TermPalette pal;
    pal.seedDefaults();
    const int rows = t->rows;
    const int columns = t->columns;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    in.placements = t->images.placements.data();
    in.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;
    in.viewOffset = 0;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(in, rgba, width, height));

    // (3,3) is inside the 16x6-px image at home -> red. (3, renderCellHeight+1) is row 1,
    // below the one-cell-tall image -> still the dark default background.
    const Pixel inside = at(rgba, width, 3, 3);
    CHECK(inside.red > 200 && inside.green < 60 && inside.blue < 60 && inside.alpha == 255);
    const Pixel below = at(rgba, width, 3, renderCellHeight + 1);
    CHECK(below.red == 0x10 && below.green == 0x10 && below.blue == 0x14 && below.alpha == 255);

    t.reset();
}

// A focused block over an inline image is drawn over the picture: the cursor
// tier is composited after the over-text image band, as the overlays are, so
// the cursor stays visible on a cell a picture covers. Composited in with the
// grid streams, the tier went under the image and the cursor vanished on
// every cell a sixel reached.
TEST(testABlockCursorIsDrawnOverAnInlineImage)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 4, 8);
    const char sixel[] = "\x1bP0;0;0q#1;2;100;0;0#1!16~\x1b\\";
    Ubiquitty::processBytes(t.get(), sixel);
    REQUIRE(Ubiquitty::Image::placementCount(t.get()) == 1);

    TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(t->rows) * t->columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    BuildInputs in;
    in.cells = grid.data();
    in.rows = t->rows;
    in.columns = t->columns;
    in.palette = &pal;
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    in.placements = t->images.placements.data();
    in.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;
    // A solid green block on the image's first cell.
    in.cursor.row = 0;
    in.cursor.column = 0;
    in.cursor.shape = Ubiquitty::Cursor::Shape::Block;
    in.cursor.focused = true;
    in.cursor.mixA = 1.0f;
    in.cursor.rgb = { 0.0f, 1.0f, 0.0f, 1.0f };

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));
    const Pixel underTheCursor = at(rgba, width, 3, 3);
    CHECK_MSG(underTheCursor.green > 200 && underTheCursor.red < 60 && underTheCursor.blue < 60,
        "the cursor's fill shows where the picture would be");
    const Pixel besideIt = at(rgba, width, renderCellWidth + 3, 3);
    CHECK_MSG(besideIt.red > 200 && besideIt.green < 60, "and the picture shows beside the cursor");
    t.reset();
}

// Inline-image z-bands, as the agent screenshot draws them (a z<0
// image must composite over the real CELL background, not black, and stay under
// the glyphs; a z>=0 image must composite over the glyphs). Drives the full
// Ubiquitty::Terminal pipeline the agent screenshot uses (processBytes for SGR + kitty images +
// glyphs, then getDisplayRow → SoftwareRenderer), so it exercises the exact path
// terminal_screenshot does.
TEST(testImageZBands)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 1, 12);

    // q=2 silences the kitty response. f=32 s=1 v=1 → one RGBA source texel
    // tiled across the 4x1 target cells. /wAAgA== = FF 00 00 80 (red, a=128);
    // AP8AgA== = 00 FF 00 80 (green, a=128).
    auto feed = [&](const char* s) { Ubiquitty::processBytes(t.get(), s); };
    // Eight glyph cells carrying a BLUE background (SGR 44): "XXXXXXXX" so the
    // cell background under the text is blue, not the default dark. Reset, then
    // cursor back to home (\r) to anchor the images over those cells.
    feed("\x1b[44mXXXXXXXX\x1b[0m\r");
    // RED 50%-alpha image, 4x1 cells, z=-1 (under the text), anchored at column 0.
    feed("\x1b_Ga=T,f=32,s=1,v=1,c=4,r=1,z=-1,q=2;/wAAgA==\x1b\\");
    // Cursor to column 5 (1-based), then GREEN 50%-alpha image z=1 (over text).
    feed("\x1b[1;5H");
    feed("\x1b_Ga=T,f=32,s=1,v=1,c=4,r=1,z=1,q=2;AP8AgA==\x1b\\");

    CHECK(Ubiquitty::Image::placementCount(t.get()) == 2);

    TermPalette pal;
    pal.seedDefaults();
    const int rows = t->rows;
    const int columns = t->columns;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (int row = 0; row < rows; ++row)
        Ubiquitty::Screen::getDisplayRow(*t, 0, row, &grid[static_cast<std::size_t>(row) * columns], columns);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    in.placements = t->images.placements.data();
    in.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;
    in.viewOffset = 0;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(in, rgba, width, height));

    // ANSI blue background = 0,0,238 (pal.ansi[4]); red image src = 255,0,0 a=128.
    // Under-text band over the blue cell bg: out = 0.5*red + 0.5*blue ≈
    // (128, 0, 119) — a purple, not pure red and not black. The X glyph (C0C0C0)
    // is drawn after this band, so glyph-ink pixels read gray; the GLYPH-FREE
    // pixels read the pure blended image-over-bg color. Compositing the z<0
    // image over black (no cell bg) leaves blue ~0; this asserts the blue
    // background shows through.
    {
        bool foundBlendedPurple = false;
        for (int y = 0; y < renderCellHeight && !foundBlendedPurple; ++y)
            for (int x = 0; x < 4 * renderCellWidth; ++x) {
                const Pixel p = at(rgba, width, x, y);
                // ~128 red, ~119 blue (the bg!), ~0 green: the image-over-bg blend
                // on a glyph-free pixel. Pure red (no bg) would have blue ~0;
                // a black-bg blend would also have blue ~0 here.
                if (p.red > 100 && p.red < 160 && p.blue > 90 && p.blue < 150 && p.green < 30) {
                    foundBlendedPurple = true;
                    break;
                }
            }
        CHECK(foundBlendedPurple); // z<0 image composited over the blue cell bg, not black
    }

    // The glyphs in the under-text band still read on top: somewhere in the red
    // band a pixel carries the (light, C0C0C0) X foreground mixed in — green
    // climbs well above the band's green~0 only where glyph ink lands. The image
    // did not bury the text.
    {
        bool foundGlyph = false;
        for (int y = 0; y < renderCellHeight && !foundGlyph; ++y)
            for (int x = 0; x < 4 * renderCellWidth; ++x) {
                const Pixel p = at(rgba, width, x, y);
                // The purple band has green ~0; the X ink raises green sharply.
                if (p.green > 60) {
                    foundGlyph = true;
                    break;
                }
            }
        CHECK(foundGlyph); // text survives under a z<0 image
    }

    // Over-text band (green, z=1): columns 4..7 = x in [32,64). The green image
    // covers the X glyphs. The blended-over result is green-dominant everywhere
    // in the band — even on a glyph pixel, since the image draws after the text.
    // green src = 0,255,0 a=128 over (X glyph C0C0C0 or blue bg) → green ~128 and
    // green is the max channel. Sample a corner (over the blue bg there).
    {
        const Pixel green0 = at(rgba, width, 4 * renderCellWidth, 0);
        CHECK(green0.green > 100); // ~128 green from the over-text image
        CHECK(green0.green > green0.red && green0.green > green0.blue); // green-dominant
        CHECK(green0.alpha == 255);
        // Even on a pixel where the X glyph has ink, green wins (image over text).
        bool glyphBuried = true;
        for (int y = 0; y < renderCellHeight; ++y)
            for (int x = 4 * renderCellWidth; x < 8 * renderCellWidth; ++x) {
                const Pixel p = at(rgba, width, x, y);
                if (!(p.green >= p.red && p.green >= p.blue)) {
                    glyphBuried = false;
                    break;
                }
            }
        CHECK(glyphBuried); // text does not poke through a z>=0 image
    }

    t.reset();
}

// The display rows of `t` at the live tail, one row after another.
std::vector<Ubiquitty::Cell> displayRows(const Ubiquitty::Terminal& t)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(t.rows) * t.columns);
    for (int row = 0; row < t.rows; ++row)
        Ubiquitty::Screen::getDisplayRow(t, 0, row, &grid[static_cast<std::size_t>(row) * t.columns], t.columns);
    return grid;
}

bool isSamePixel(Pixel a, Pixel b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}

// A picture placed at z<0 sits between the cells' ground and their ink, which
// on the cell-baked path are painted by the same quad. Rendered with and
// without the picture, the frame differs only where the picture shows: each
// pixel the glyphs ink is the same ink either way, each pixel they leave bare
// is the picture's, and the cells it does not reach are byte for byte as they
// were. Unifont's coverage is whole, so an inked pixel is the foreground
// whatever lies beneath it. Composited after the cells, the picture covered
// the text instead.
TEST(testANegativeZPictureDrawsUnderCellBakedText)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 2, 8);
    const auto feed = [&](const char* bytes) { Ubiquitty::processBytes(t.get(), bytes); };
    feed("XXXX\r\n\x1b[44mXXXX\x1b[0m\x1b[H");
    // One opaque red texel, /wAA = FF 00 00, stretched over four cells of the
    // first row, beneath the text.
    feed("\x1b_Ga=T,f=24,s=1,v=1,c=4,r=1,z=-1,C=1,q=2;/wAA\x1b\\");
    REQUIRE(Ubiquitty::Image::placementCount(t.get()) == 1);

    TermPalette palette;
    palette.seedDefaults();
    const std::vector<Ubiquitty::Cell> grid = displayRows(*t);
    BuildInputs in;
    in.cells = grid.data();
    in.rows = t->rows;
    in.columns = t->columns;
    in.palette = &palette;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> plain;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, plain, width, height));
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    in.placements = t->images.placements.data();
    in.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;
    std::vector<std::uint8_t> pictured;
    REQUIRE(renderer.render(in, pictured, width, height));
    REQUIRE(pictured.size() == plain.size());

    const Pixel ground = at(plain, width, 6 * renderCellWidth + 3, 3);
    const Pixel red { 255, 0, 0, 255 };
    int ink = 0;
    int buriedInk = 0;
    int pictureMissing = 0;
    int changedElsewhere = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel before = at(plain, width, x, y);
            const Pixel after = at(pictured, width, x, y);
            const bool isUnderPicture = y < renderCellHeight && x < 4 * renderCellWidth;
            if (!isUnderPicture) {
                changedElsewhere += isSamePixel(before, after) ? 0 : 1;
            } else if (isSamePixel(before, ground)) {
                pictureMissing += isSamePixel(after, red) ? 0 : 1;
            } else {
                ++ink;
                buriedInk += isSamePixel(after, before) ? 0 : 1;
            }
        }
    }
    REQUIRE_MSG(ink > 0, "the four letters ink their cells");
    CHECK_MSG(buriedInk == 0, "every pixel the letters ink is their ink, over the picture");
    CHECK_MSG(pictureMissing == 0, "every pixel the letters leave bare is the picture's");
    CHECK_MSG(changedElsewhere == 0, "the cells the picture does not reach render as they did without it");
}

// A glyph drawn as its own quad mixes its antialiased edge over what lies
// beneath it. Over a picture at z<0 that is the picture, not the cell
// background the picture replaced: mixed over the background, every partly
// covered pixel would wear a fringe of it. The stem's coverage is 60, 220
// and 90 across its three columns; white ink over a green picture keeps every
// pixel's green whole and puts exactly the coverage in its red.
TEST(testANegativeZPictureShowsThroughAGlyphsAntialiasedEdge)
{
    struct StemRasterizer final : GlyphRasterizer {
        CellMetrics cellMetrics() const override
        {
            CellMetrics metrics;
            metrics.cellWidth = 8.0f;
            metrics.cellHeight = 16.0f;
            metrics.baselineFromTop = 13.0f;
            return metrics;
        }
        bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverage) override
        {
            std::fill_n(coverage, static_cast<std::size_t>(slotWidth) * slotHeight, 0);
            for (int row = 3; row < 12; ++row) {
                coverage[row * slotWidth + 2] = 60;
                coverage[row * slotWidth + 3] = 220;
                coverage[row * slotWidth + 4] = 90;
            }
            return true;
        }
    } rasterizer;
    constexpr int columns = 16;
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 1, columns);
    // One opaque green texel, AP8A = 00 FF 00, under the whole row.
    Ubiquitty::processBytes(t.get(), "\x1b_Ga=T,f=24,s=1,v=1,c=16,r=1,z=-1,C=1,q=2;AP8A\x1b\\");
    REQUIRE(Ubiquitty::Image::placementCount(t.get()) == 1);

    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> cells(columns, defaultBlankCell);
    for (auto& cell : cells) {
        cell.pen.foregroundIndex = 15;
        cell.pen.backgroundIndex = 0;
    }
    cells[1].codePoint = 'l';
    BuildInputs inputs;
    inputs.cells = cells.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.subpixelGlyphs = true;
    inputs.metrics = rasterizer.cellMetrics();
    inputs.glyphReferenceAdvanceInPixels = 8.0f;
    inputs.glyphReferenceLineHeightInPixels = 16.0f;
    inputs.glyphSlotWidthInPixels = 8.0f;
    inputs.glyphSlotHeightInPixels = 16.0f;
    inputs.images = t->images.stored.data();
    inputs.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    inputs.placements = t->images.placements.data();
    inputs.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;

    SoftwareRenderer renderer(8, 16, rasterizer);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(inputs, rgba, width, height));
    REQUIRE(width == columns * 8 && height == 16);
    int fringed = 0;
    int inkCoverage = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel pixel = at(rgba, width, x, y);
            fringed += pixel.green == 255 ? 0 : 1;
            inkCoverage += pixel.red;
        }
    }
    CHECK_MSG(fringed == 0, "no pixel of the frame takes the cell background's colour over the picture");
    CHECK_MSG(inkCoverage == 9 * (60 + 220 + 90), "the stem inks exactly its coverage over the picture");
}

// Kitty Unicode placeholders: a virtual placement (U=1) draws nothing by
// itself, and each U+10EEEE cell shows the tile of the image its row and
// column marks name, with the image id in its foreground. The block printed
// twice draws the image twice, on the first row; the second row holds no
// placeholder and shows the ground. Nothing here is in the placement table,
// which a frame captured from a terminal with only virtual placements leaves
// empty.
TEST(testPlaceholderCellsDrawTheTilesTheyName)
{
    // Two rows: the fewest a resize gives (Core/GridBounds.h, minRows).
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 2, 8);
    REQUIRE(t->rows == 2 && t->columns == 8);
    const auto feed = [&](const char* bytes) { Ubiquitty::processBytes(t.get(), bytes); };
    // Image 7: a red texel and a green one, /wAAAP8A = FF 00 00 00 FF 00, cut
    // into a one-row grid of two cells.
    feed("\x1b_Ga=T,U=1,i=7,f=24,s=2,v=1,c=2,r=1,q=2;/wAAAP8A\x1b\\");
    REQUIRE(Ubiquitty::Image::placementCount(t.get()) == 0);
    // U+10EEEE with row 0 (U+0305) and column 0 (U+0305) or 1 (U+030D).
    const char* const block = "\xF4\x8E\xBB\xAE\xCC\x85\xCC\x85"
                              "\xF4\x8E\xBB\xAE\xCC\x85\xCC\x8D";
    feed("\x1b[H\x1b[38;5;7m");
    feed(block);
    feed("\x1b[1;5H");
    feed(block);
    feed("\x1b[0m");

    TermPalette palette;
    palette.seedDefaults();
    const std::vector<Ubiquitty::Cell> grid = displayRows(*t);
    BuildInputs in;
    in.cells = grid.data();
    in.rows = t->rows;
    in.columns = t->columns;
    in.palette = &palette;
    in.clusterResolver = resolveTerminalCluster;
    in.clusterResolverContext = t.get();
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));

    REQUIRE(width == 8 * renderCellWidth && height == 2 * renderCellHeight);

    const Pixel red { 255, 0, 0, 255 };
    const Pixel green { 0, 255, 0, 255 };
    const Pixel ground { 0x10, 0x10, 0x14, 255 };
    const Pixel expected[8] = { red, green, ground, ground, red, green, ground, ground };
    for (int column = 0; column < 8; ++column) {
        int wrong = 0;
        int wrongBelow = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = column * renderCellWidth; x < (column + 1) * renderCellWidth; ++x) {
                if (y < renderCellHeight)
                    wrong += isSamePixel(at(rgba, width, x, y), expected[column]) ? 0 : 1;
                else
                    wrongBelow += isSamePixel(at(rgba, width, x, y), ground) ? 0 : 1;
            }
        }
        const std::string message = "column " + std::to_string(column) + " shows the tile its cell names, or the ground";
        CHECK_MSG(wrong == 0, message.c_str());
        const std::string below = "column " + std::to_string(column) + " of the second row shows the ground";
        CHECK_MSG(wrongBelow == 0, below.c_str());
    }
}

// The placeholder quads share the frame's image-quad storage, which holds
// `Image::maxTerminalImagePlacementCount`: a block of more cells than that
// draws its first cells and stops there, rather than writing past it.
TEST(testPlaceholderQuadsStopAtTheSharedImageLimit)
{
    constexpr int columns = 100;
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 2, columns);
    // A solid red image 100 texels across and 2 down, one texel a cell.
    std::string transmit = "\x1b_Ga=T,U=1,i=7,f=24,s=100,v=2,c=100,r=2,q=2;";
    for (int texel = 0; texel < columns * 2; ++texel)
        transmit += "/wAA";
    transmit += "\x1b\\";
    Ubiquitty::processBytes(t.get(), transmit);
    // Each row names its first cell and leaves the rest to inherit the next
    // column from the cell to their left: 200 placeholder cells.
    std::string cells = "\x1b[H\x1b[38;5;7m\xF4\x8E\xBB\xAE\xCC\x85\xCC\x85";
    for (int column = 1; column < columns; ++column)
        cells += "\xF4\x8E\xBB\xAE";
    cells += "\x1b[2;1H\xF4\x8E\xBB\xAE\xCC\x8D\xCC\x85";
    for (int column = 1; column < columns; ++column)
        cells += "\xF4\x8E\xBB\xAE";
    cells += "\x1b[0m";
    Ubiquitty::processBytes(t.get(), cells);

    TermPalette palette;
    palette.seedDefaults();
    const std::vector<Ubiquitty::Cell> grid = displayRows(*t);
    BuildInputs in;
    in.cells = grid.data();
    in.rows = t->rows;
    in.columns = t->columns;
    in.palette = &palette;
    in.clusterResolver = resolveTerminalCluster;
    in.clusterResolverContext = t.get();
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));
    const Pixel red { 255, 0, 0, 255 };
    int drawn = 0;
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < columns; ++column)
            drawn += isSamePixel(at(rgba, width, column * renderCellWidth + 3, row * renderCellHeight + 3), red) ? 1 : 0;
    CHECK_MSG(isSamePixel(at(rgba, width, 3, 3), red), "the block's first cell shows its tile");
    CHECK_MSG(drawn > 0 && drawn <= Ubiquitty::Image::maxTerminalImagePlacementCount, "no more cells draw than the frame has quads for");
}

// Selection-overlay draw: a single 'A' (red foreground, default background) with a translucent
// selection span over its cell must TINT the background toward the selection
// color (the overlay composites over the cell, the glyph reads through) — not
// invert the cell. Asserts an empty (background) corner picks up the tint while
// staying distinguishable from the un-selected default background.
// The software surface follows the caller's drawable, and the grid sits at the
// origin the caller gave.
//
// Two defects, one shape: a surface of `columns * cellWidth` whatever the
// drawable, and a `composite` that places cells at `gridPosition * cellWidth`
// with no origin at all. A frontend that insets its grid — an OSC 133
// prompt-mark gutter — then gets a surface with no room outside the cells, and
// every frame-level pass draws into coordinates the buffer does not have.
// Nothing fails: the composites clip, so the brackets are absent. Sizing the
// surface alone is worse, because then the cells draw at zero while the
// brackets draw at the inset.
TEST(testViewportSurface)
{
    TermPalette pal;
    pal.seedDefaults(); // background 101014

    const int rows = 2;
    const int columns = 4;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = 1; // red, so a painted cell is not the cleared background
        c.pen.underlineIndex = colorDefault;
    }

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    const int gridWidth = columns * renderCellWidth;
    const int gridHeight = rows * renderCellHeight;

    BuildInputs base;
    base.cells = grid.data();
    base.rows = rows;
    base.columns = columns;
    base.palette = &pal;

    // Where the grid's red starts, or -1 if it never does.
    const auto firstRedColumn = [](const std::vector<std::uint8_t>& rgba, int width) {
        for (int x = 0; x < width; ++x)
            if (at(rgba, width, x, 1).red > 0x40)
                return x;
        return -1;
    };

    // No drawable: the surface is the grid and the grid starts at zero. This is
    // every caller that keeps no viewport, and it must not move.
    {
        std::vector<std::uint8_t> rgba;
        int width = 0, height = 0;
        CHECK(renderer.render(base, rgba, width, height));
        CHECK_EQ(width, gridWidth);
        CHECK_EQ(height, gridHeight);
        CHECK_EQ(firstRedColumn(rgba, width), 0);
    }

    // Inset in a wider drawable: the surface is the drawable and the grid moved.
    // Asserting the width alone passes on a renderer that sizes the surface
    // correctly and still draws every cell at zero, which is the failure the
    // width was standing in for.
    const int gutter = 12;
    {
        BuildInputs inset = base;
        inset.originInPixels = { static_cast<float>(gutter), 0.0f };
        inset.viewportSizeInPixels = { static_cast<float>(gridWidth + 2 * gutter), static_cast<float>(gridHeight) };
        std::vector<std::uint8_t> rgba;
        int width = 0, height = 0;
        CHECK(renderer.render(inset, rgba, width, height));
        CHECK_EQ(width, gridWidth + 2 * gutter);
        CHECK_EQ(static_cast<int>(rgba.size()), width * height * 4);
        CHECK_MSG(firstRedColumn(rgba, width) == gutter, "the grid is painted at the origin the caller gave");
        const Pixel inGutter = at(rgba, width, gutter - 2, 1);
        CHECK_MSG(inGutter.red <= 0x40, "the gutter is surface outside the grid, not the grid drawn at zero");
    }

    // A FRACTIONAL origin, which is the only input that separates the candidate
    // rules. At an integral inset truncation, rounding and flooring all agree,
    // so a case without one proves the origin is applied and nothing about how.
    // This pass blits atlas coverage one texel to one pixel and cannot resample,
    // so a fraction has to resolve to a whole pixel; it rounds, because the
    // Metal fill rule resolves a fractional edge to the nearest boundary and
    // truncating would disagree by a whole pixel rather than half of one.
    {
        BuildInputs half = base;
        half.originInPixels = { gutter + 0.5f, 0.0f };
        half.viewportSizeInPixels = { static_cast<float>(gridWidth + 2 * gutter), static_cast<float>(gridHeight) };
        std::vector<std::uint8_t> rgba;
        int width = 0, height = 0;
        CHECK(renderer.render(half, rgba, width, height));
        CHECK_MSG(firstRedColumn(rgba, width) == gutter + 1,
            "a fractional origin rounds to the nearest whole pixel, not toward zero");
    }
    {
        BuildInputs justUnder = base;
        justUnder.originInPixels = { gutter + 0.25f, 0.0f };
        justUnder.viewportSizeInPixels = { static_cast<float>(gridWidth + 2 * gutter), static_cast<float>(gridHeight) };
        std::vector<std::uint8_t> rgba;
        int width = 0, height = 0;
        CHECK(renderer.render(justUnder, rgba, width, height));
        CHECK_MSG(firstRedColumn(rgba, width) == gutter,
            "and rounds DOWN below the half — a rule that always rounded up would pass the case above");
    }
}

TEST(testBrailleSquareCoverageRetainsFractionalHorizontalAndVerticalPositions)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 5;
    constexpr int rows = 3;
    std::vector<Cell> grid(columns * rows, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, row, column, 0x28FF, 0, 15, 0);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.drawsBrailleGrid = true;
    input.brailleStyle = BrailleStyle::Square;
    input.metrics.cellWidth = 15.25f;
    input.metrics.cellHeight = 31.0f;
    input.originInPixels = { 2.125f, 2.0f };
    input.viewportSizeInPixels = { 84.0f, 100.0f };
    input.subpixelPhaseCount = 4;
    input.subpixelPhaseRounding = PhaseRounding::Floor;
    SoftwareRenderer renderer(16, 31);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(input, pixels, width, height));
    const BrailleLattice lattice { input.originInPixels.x, input.originInPixels.y,
        input.metrics.cellWidth, input.metrics.cellHeight, BrailleStyle::Square,
        input.subpixelPhaseCount, input.subpixelPhaseRounding };
    for (int dotRow = 0; dotRow < rows * 4; ++dotRow) {
        for (int dotColumn = 0; dotColumn < columns * 2; ++dotColumn) {
            const auto ink = brailleDotRect(lattice, dotColumn / 2, dotRow / 4, dotColumn % 2, dotRow % 4);
            for (int y = static_cast<int>(std::floor(ink.top)); y < std::ceil(ink.bottom); ++y) {
                for (int x = static_cast<int>(std::floor(ink.left)); x < std::ceil(ink.right); ++x) {
                    const float coverageX = std::min(x + 1.0f, ink.right) - std::max(static_cast<float>(x), ink.left);
                    const float coverageY = std::min(y + 1.0f, ink.bottom) - std::max(static_cast<float>(y), ink.top);
                    const int expected = static_cast<int>(coverageX * coverageY * 255.0f + 0.5f);
                    const Pixel pixel = at(pixels, width, x, y);
                    CHECK_EQ(pixel.red, expected);
                    CHECK_EQ(pixel.green, expected);
                    CHECK_EQ(pixel.blue, expected);
                }
            }
        }
    }
}

TEST(testBrailleRoundStylesCompositeFractionalDotsAndGuides)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 3;
    constexpr int rows = 2;
    std::vector<Cell> grid(columns * rows, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, row, column, 0x2801, 0, 15, 0);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.metrics.cellWidth = 15.25f;
    input.metrics.cellHeight = 31.0f;
    input.originInPixels = { 2.125f, 2.0f };
    input.viewportSizeInPixels = { 52.0f, 68.0f };
    input.subpixelPhaseCount = 4;
    input.subpixelPhaseRounding = PhaseRounding::Floor;
    SoftwareRenderer renderer(16, 31);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    for (const bool isBitmapFace : { false, true }) {
        input.isBitmapFace = isBitmapFace;
        for (const auto style : { BrailleStyle::Regular, BrailleStyle::Outline6Dot, BrailleStyle::Outline8Dot,
                 BrailleStyle::Pinpoint6Dot, BrailleStyle::Pinpoint8Dot }) {
            input.brailleStyle = style;
            CHECK(renderer.render(input, pixels, width, height));
            const BrailleLattice lattice { input.originInPixels.x, input.originInPixels.y,
                input.metrics.cellWidth, input.metrics.cellHeight, style,
                input.subpixelPhaseCount, input.subpixelPhaseRounding };
            const bool isOutline = style == BrailleStyle::Outline6Dot || style == BrailleStyle::Outline8Dot;
            const bool isPinpoint = style == BrailleStyle::Pinpoint6Dot || style == BrailleStyle::Pinpoint8Dot;
            const bool isSixDot = style == BrailleStyle::Outline6Dot || style == BrailleStyle::Pinpoint6Dot;
            int partialPixels = 0;
            for (int dotRow = 0; dotRow < rows * 4; ++dotRow) {
                for (int dotColumn = 0; dotColumn < columns * 2; ++dotColumn) {
                    const auto ink = brailleDotRect(lattice, dotColumn / 2, dotRow / 4, dotColumn % 2, dotRow % 4);
                    const float centerX = (ink.left + ink.right) * 0.5f;
                    const float centerY = (ink.top + ink.bottom) * 0.5f;
                    const bool isRaised = dotColumn % 2 == 0 && dotRow % 4 == 0;
                    const bool isVisible = isRaised || ((isOutline || isPinpoint) && (!isSixDot || dotRow % 4 < 3));
                    const float fullRadius = (ink.right - ink.left) * 0.5f;
                    const float radius = !isRaised && isPinpoint ? fullRadius / 3.0f : fullRadius;
                    const float innerRadius = !isRaised && isOutline ? radius - std::max(0.5f, radius * 0.35f) : 0.0f;
                    for (int y = static_cast<int>(std::floor(ink.top - 0.5f)); y < std::ceil(ink.bottom + 0.5f); ++y) {
                        for (int x = static_cast<int>(std::floor(ink.left - 0.5f)); x < std::ceil(ink.right + 0.5f); ++x) {
                            const float distance = std::hypot(x + 0.5f - centerX, y + 0.5f - centerY);
                            float coverage = isVisible ? std::clamp(radius + 0.5f - distance, 0.0f, 1.0f) : 0.0f;
                            if (innerRadius > 0.0f) {
                                coverage = std::min(coverage, std::clamp(distance - innerRadius + 0.5f, 0.0f, 1.0f));
                            }
                            if (isBitmapFace) {
                                coverage = std::round(coverage * 51.0f) / 51.0f;
                            }
                            const int expected = static_cast<int>(coverage * 255.0f + 0.5f);
                            const auto pixel = at(pixels, width, x, y);
                            CHECK_EQ(pixel.red, expected);
                            CHECK_EQ(pixel.green, expected);
                            CHECK_EQ(pixel.blue, expected);
                            CHECK_EQ(pixel.alpha, 255);
                            partialPixels += expected > 0 && expected < 255;
                        }
                    }
                }
            }
            CHECK(partialPixels > 0);
        }
    }
}

TEST(testRoundedUnderlineCursorCompositesEndsInBothFaceModes)
{
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(1, defaultBlankCell);
    setCell(grid, 1, 0, 0, ' ', 0, 15, 0);
    BuildInputs input;
    input.cells = grid.data();
    input.rows = 1;
    input.columns = 1;
    input.palette = &palette;
    input.metrics.cellWidth = 8.0f;
    input.metrics.cellHeight = 16.0f;
    input.cursor.row = 0;
    input.cursor.shape = Cursor::Shape::Underline;
    input.cursor.mixA = 1.0f;
    input.cursor.rgb = { 1, 1, 1, 1 };
    SoftwareRenderer renderer(8, 16);
    for (const bool isBitmapFace : { false, true }) {
        input.isBitmapFace = isBitmapFace;
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        CHECK(renderer.render(input, pixels, width, height));
        CHECK_EQ(at(pixels, width, 4, 13).red, 0);
        CHECK_EQ(at(pixels, width, 4, 15).red, 255);
        const int cap = at(pixels, width, 0, 14).red;
        CHECK(cap > 0 && cap < 255);
        CHECK_EQ(cap, at(pixels, width, 7, 14).red);
        CHECK_EQ(cap, at(pixels, width, 0, 15).red);
        if (isBitmapFace) {
            CHECK_EQ(cap % 5, 0);
        }
    }
}

// The toolkit caret blends as GSK's cairo renderer blends GtkText's faded
// caret: pixman's source-over under an 8-bit mask, each product rounded on its own.
// One square column at the cell's left edge, nothing beside it.
TEST(testAToolkitCaretMixesAsPixmanDoes)
{
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(2, defaultBlankCell);
    BuildInputs input;
    input.cells = grid.data();
    input.rows = 1;
    input.columns = 2;
    input.palette = &palette;
    input.metrics.cellWidth = 8.0f;
    input.metrics.cellHeight = 16.0f;
    input.cellHeightPoints = 16.0;
    input.cursor.row = 0;
    input.cursor.column = 1;
    input.cursor.shape = Cursor::Shape::Bar;
    input.cursor.focused = true;
    input.cursor.mixA = 0.3f;
    input.cursor.rgb = { 1, 1, 1, 1 };
    input.toolkitCaret = { .aspectRatio = 0.04, .logicalScale = 1 };
    SoftwareRenderer renderer(8, 16);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));
    const auto multiply = [](int value, int weight) {
        const int product = value * weight + 0x80;
        return ((product >> 8) + product) >> 8;
    };
    const Pixel ground = at(pixels, width, 4, 4);
    const int mask = Ubiquitty::Rendering::toolkitCaretMask(0.3);
    for (int y = 0; y < 16; ++y)
        CHECK_EQ(static_cast<int>(at(pixels, width, 8, y).red), multiply(255, mask) + multiply(ground.red, 255 - mask));
    CHECK_EQ(at(pixels, width, 7, 8).red, ground.red);
    CHECK_EQ(at(pixels, width, 9, 8).red, ground.red);
}

// Kitty's fractional scale at s=4 n=1:d=2 draws an H at twice its size in the
// top-left quarter of the block it claims. On the bitmap tier the magnified
// letter is the face's own pixels doubled, as the GPU glyph pass's nearest tap
// draws it: a blended tap would add greys a 1-bit face does not have.
TEST(testAFractionalBitmapGlyphMagnifiesByWholeTexels)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec { /*scale=*/4, /*widthCells=*/1 };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    const unsigned id = table.intern(spec);
    constexpr int rows = 4;
    constexpr int columns = 4;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int band = 0; band < rows; ++band) {
        for (int offset = 0; offset < columns; ++offset) {
            setCell(grid, columns, band, offset, offset == 0 ? 0x48u : 0u, 0, 15, 0);
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + offset];
            Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.isBitmapFace = true;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));

    const Pixel background = at(pixels, width, width - 1, height - 1);
    int inkInside = 0;
    int inkOutside = 0;
    int blended = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel p = at(pixels, width, x, y);
            if (p.red == background.red && p.green == background.green && p.blue == background.blue)
                continue;
            const bool isInk = p.red == 255 && p.green == 255 && p.blue == 255;
            blended += isInk ? 0 : 1;
            const bool isInsideTheArea = x < 2 * renderCellWidth && y < 2 * renderCellHeight;
            ++(isInsideTheArea ? inkInside : inkOutside);
        }
    }
    CHECK_MSG(inkInside > 0, "the letter is drawn in the area");
    CHECK_MSG(inkOutside == 0, "and nowhere else in the block: the fraction halves the four-cell letter");
    CHECK_MSG(blended == 0, "every pixel is the face's ink or the background");
}

TEST(testFractionalTextBoxesSoftFontsAndRulesMatchTheirDrawnSize)
{
    TermPalette palette;
    palette.seedDefaults();
    TextSizeTable table;
    TextSizeSpec spec { 2, 1 };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    spec.verticalAlign = spec.horizontalAlign = 2;
    const unsigned id = table.intern(spec);
    VT::SoftFont softFont {};
    softFont.valid = true;
    softFont.serial = 1;
    softFont.matrixWidth = softFont.matrixHeight = 8;
    softFont.glyphSet[0] = true;
    for (int row = 0; row < 8; ++row)
        softFont.glyph[0][row] = static_cast<std::uint8_t>(row < 4 ? 0xF0u : 0x3Cu);
    constexpr int rows = 2;
    constexpr int columns = 8;
    struct Picture {
        std::uint32_t codePoint;
        std::uint8_t underlineStyle = VT::underlineNone;
    };
    const Picture pictures[] = { { 0x54u }, { 0x2588u }, { 0x256Du }, { 0xE0B0u }, { 0x1FB00u }, { VT::drcsCodePointBase },
        { 0x20u, VT::underlineSingle }, { 0x20u, VT::underlineDotted }, { 0x20u, VT::underlineCurly } };
    for (bool isDoubled : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
        for (const Picture picture : pictures) {
            const std::uint32_t codePoint = picture.codePoint;
            // Ordinary doubled box glyphs take their native double sheet. The
            // procedural shapes here compare at their ordinary one-cell size.
            if (isDoubled && codePoint != 0x54u && codePoint != VT::drcsCodePointBase && codePoint != 0x20u)
                continue;
            if (isDoubled && picture.underlineStyle > VT::underlineSingle)
                continue;
            auto renderScene = [&](bool isFractional) {
                std::vector<Cell> grid(rows * columns, defaultBlankCell);
                for (Cell& cell : grid) {
                    cell.pen.foregroundIndex = 15;
                    cell.pen.backgroundIndex = 0;
                }
                const int bands = isFractional ? 2 : 1;
                for (int band = 0; band < bands; ++band) {
                    for (int offset = 0; offset < bands; ++offset) {
                        setCell(grid, columns, band, offset, offset == 0 ? codePoint : 0, 0, 15, 0);
                        Cell& cell = grid[static_cast<std::size_t>(band * columns + offset)];
                        if (codePoint == 0x20u) {
                            cell.pen.attributes.add(VT::Attribute::Underline);
                            cell.pen.attributes.add(VT::Attribute::Strike);
                            penSetUnderlineShape(&cell.pen, picture.underlineStyle);
                        }
                        if (isFractional) {
                            cell.pen.attributes.add(offset == 0 ? VT::Attribute::Wide : VT::Attribute::WideContinuation);
                            cell.pen.extendedAttributes.setTextSizeSpecID(id);
                            penSetTextBand(&cell.pen, static_cast<std::uint8_t>(band));
                        }
                    }
                }
                const std::uint8_t lineAttributes[] = { isDoubled ? lineAttributeDoubleWidth : lineAttributeNormal, lineAttributeNormal };
                BuildInputs input;
                input.cells = grid.data();
                input.rows = rows;
                input.columns = columns;
                input.palette = &palette;
                input.textSizeTable = &table;
                input.softFont = &softFont;
                input.lineAttributes = lineAttributes;
                input.isBitmapFace = true;
                input.metrics.underlineThickness = 1;
                input.metrics.underlineDrop = 2;
                input.metrics.strikeThickness = 1;
                input.metrics.xHeight = 8;
                SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
                std::vector<std::uint8_t> pixels;
                int width = 0, height = 0;
                CHECK(renderer.render(input, pixels, width, height));
                CHECK(width == columns * renderCellWidth && height == rows * renderCellHeight);
                return pixels;
            };
            const auto ordinary = renderScene(false);
            const auto fractional = renderScene(true);
            REQUIRE(ordinary.size() == fractional.size() && !ordinary.empty());
            const int pictureWidth = isDoubled ? 16 : 8;
            const int left = pictureWidth / 2;
            constexpr int top = 8;
            constexpr int width = columns * renderCellWidth;
            const Pixel background = at(ordinary, width, width - 1, 0);
            int ink = 0;
            int differences = 0;
            for (int y = 0; y < rows * renderCellHeight; ++y) {
                for (int x = 0; x < width; ++x) {
                    const bool isInside = x >= left && x < left + pictureWidth && y >= top;
                    const Pixel expected = isInside ? at(ordinary, width, x - left, y - top) : background;
                    const Pixel actual = at(fractional, width, x, y);
                    differences += actual.red != expected.red || actual.green != expected.green || actual.blue != expected.blue || actual.alpha != expected.alpha;
                    ink += actual.red != background.red || actual.green != background.green || actual.blue != background.blue;
                }
            }
            CHECK_MSG(ink > 0, "the fixture must draw visible ink");
            if (differences)
                std::fprintf(stderr, "fractional code point U+%04X doubled=%d differs in %d pixels\n", codePoint, isDoubled, differences);
            CHECK_MSG(differences == 0, "s=2 n=1/d=2 is the ordinary picture placed inside its larger block");
        }
    }
}

namespace {

// A vector rasterizer whose cell-sized glyph has no ink and whose scaled one is
// solid, so any ink on screen came from a raster drawn at the scaled size and
// none from a resample of the cell's own.
struct ScaledOnlyRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = 8.0f;
        metrics.cellHeight = 16.0f;
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, static_cast<std::uint8_t>(0));
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        requests.push_back({ master.scale, slotWidth, slotHeight });
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, static_cast<std::uint8_t>(255));
        return true;
    }
    struct Request {
        RasterScale scale;
        int slotWidth;
        int slotHeight;
    };
    std::vector<Request> requests;
};

} // namespace

// An OSC 66 fraction that is not a whole scale reaches the rasterizer as the
// fraction itself: s=3 at n=1:d=2 is a raster at 3/2 of the cell, into a master
// the cell slot's size times 3/2, and the block draws from it alone — the
// cell's own raster, resampled, would put no ink on screen here.
namespace {

// A face whose slots have no margin: every code point but `x` draws one row of
// ink along its slot's bottom edge, the way a descender or an underscore
// reaches it, and `x` draws a block in the middle of its slot.
struct BottomEdgeInkRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, std::uint8_t { 0 });
        if (codePoint == 'x') {
            for (int y = 5; y < 11; ++y)
                std::fill_n(coverageOut + static_cast<std::size_t>(y) * slotWidth + 2, 4, std::uint8_t { 255 });
        } else {
            std::fill_n(coverageOut + static_cast<std::size_t>(slotHeight - 1) * slotWidth, slotWidth, std::uint8_t { 255 });
        }
        return true;
    }
};

std::vector<std::uint8_t> renderSuperscriptX(SoftwareRenderer& renderer, const TermPalette& palette)
{
    constexpr int rows = 2;
    constexpr int columns = 2;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0u, 0, 15, 0);
    }
    Cell& scripted = grid[static_cast<std::size_t>(columns)];
    scripted.codePoint = 'x';
    penSetScript(&scripted.pen, VT::scriptSuper);
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(input, pixels, width, height));
    return pixels;
}

} // namespace

// A scripted glyph is the same picture whatever the sheet allocated around its
// slot. Its quad is shrunk, so its taps are linear, and a tap half a texel past
// its box would read the neighbouring slot: with no margin between slots, the
// bottom row of the glyph allocated above it drew a dash beside every sub- and
// superscript, one that moved with allocation order.
TEST(testAScriptedGlyphIsTheSamePictureWhateverSurroundsItsSlot)
{
    TermPalette palette;
    palette.seedDefaults();
    BottomEdgeInkRasterizer freshFace;
    SoftwareRenderer alone(renderCellWidth, renderCellHeight, freshFace);
    const std::vector<std::uint8_t> first = renderSuperscriptX(alone, palette);

    // The same frame after a frame that filled a row of slots with glyphs
    // whose ink reaches their slots' bottom edge, so the slot `x` takes has
    // ink-filled slots before it and above it.
    BottomEdgeInkRasterizer primedFace;
    SoftwareRenderer primed(renderCellWidth, renderCellHeight, primedFace);
    constexpr int primeColumns = 80;
    std::vector<Cell> primeGrid(primeColumns, defaultBlankCell);
    for (int column = 0; column < primeColumns; ++column)
        setCell(primeGrid, primeColumns, 0, column, 0x100u + static_cast<std::uint32_t>(column), 0, 15, 0);
    BuildInputs prime;
    prime.cells = primeGrid.data();
    prime.rows = 1;
    prime.columns = primeColumns;
    prime.palette = &palette;
    std::vector<std::uint8_t> primePixels;
    int primeWidth = 0;
    int primeHeight = 0;
    REQUIRE(primed.render(prime, primePixels, primeWidth, primeHeight));
    const std::vector<std::uint8_t> second = renderSuperscriptX(primed, palette);

    REQUIRE(first.size() == second.size() && !first.empty());
    CHECK_MSG(first == second, "the superscript draws the same bytes in either allocation order");
}

TEST(testAFractionalBlockIsRasterisedAtItsFractionalSize)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec { /*scale=*/3, /*widthCells=*/1 };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    spec.verticalAlign = 2;
    spec.horizontalAlign = 2;
    const unsigned id = table.intern(spec);
    constexpr int rows = 3;
    constexpr int columns = 6;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0u, 0, 15, 0);
    }
    for (int band = 0; band < 3; ++band) {
        for (int offset = 0; offset < 3; ++offset) {
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + offset];
            cell.codePoint = offset == 0 ? 0x41u : 0u;
            Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    ScaledOnlyRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));

    CHECK_MSG(!rasterizer.requests.empty(), "the rasterizer is asked for the scaled raster");
    for (const ScaledOnlyRasterizer::Request& request : rasterizer.requests) {
        CHECK_MSG(request.scale.numerator == 3 && request.scale.denominator == 2, "at 3/2, not at a whole scale");
        CHECK_MSG(request.slotWidth == (renderCellWidth * 3 + 1) / 2 && request.slotHeight == renderCellHeight * 3 / 2,
            "into a master the cell slot's size times 3/2");
    }

    // The 12 by 24 picture stands centred in the 24 by 48 block.
    const Pixel background = at(pixels, width, width - 1, height - 1);
    int inkInside = 0;
    int inkOutside = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel p = at(pixels, width, x, y);
            if (p.red == background.red && p.green == background.green && p.blue == background.blue)
                continue;
            ++(x >= 6 && x < 18 && y >= 12 && y < 36 ? inkInside : inkOutside);
        }
    }
    CHECK_MSG(inkInside == 12 * 24, "the scaled raster fills the area, every pixel of it");
    CHECK_MSG(inkOutside == 0, "and nothing else in the block");
}

TEST(testSelectionOverlay)
{
    TermPalette pal;
    pal.seedDefaults(); // default foreground C0C0C0, background 101014

    const int rows = 1;
    const int columns = 1;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    setCell(grid, columns, 0, 0, 0x41u, 0, /*foreground*/ 1, colorDefault); // 'A' red

    // No selection: the corner is the plain default background.
    BuildInputs plain;
    plain.cells = grid.data();
    plain.rows = rows;
    plain.columns = columns;
    plain.palette = &pal;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> plainRgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(plain, plainRgba, width, height));
    const Pixel plainCorner = at(plainRgba, width, 0, 0);
    CHECK(plainCorner.red == 0x10 && plainCorner.green == 0x10 && plainCorner.blue == 0x14);

    // With a selection span over the whole cell: the same corner is tinted toward
    // the (light) selection color — brighter than the dark default background — but still
    // a blend (not the opaque selection color).
    const SelectionRowSpan spans[] = { { 0, 0, 0 } };
    BuildInputs selection = plain;
    selection.selectionSpans = spans;
    selection.selectionSpanCount = 1;
    selection.selectionColor = { 0.62f, 0.74f, 0.93f, 0.40f }; // the Linux frontend's tint

    std::vector<std::uint8_t> selectionRGBA;
    CHECK(renderer.render(selection, selectionRGBA, width, height));
    const Pixel tinted = at(selectionRGBA, width, 0, 0);
    // Tinted brighter than the dark default background on every channel (the light tint).
    CHECK(tinted.red > plainCorner.red && tinted.green > plainCorner.green && tinted.blue > plainCorner.blue);
    // But a blend: still well below the opaque tint (0.62*255 ≈ 158).
    CHECK(tinted.red < 158 && tinted.alpha == 255);
    // The glyph still reads: somewhere in the cell the red 'A' ink survives the
    // translucent overlay (~60% of the red foreground shows through).
    bool foundRedInk = false;
    for (int y = 0; y < renderCellHeight && !foundRedInk; ++y)
        for (int x = 0; x < renderCellWidth; ++x) {
            const Pixel p = at(selectionRGBA, width, x, y);
            if (p.red > p.green + 30 && p.red > p.blue + 30) { // red-dominant: glyph ink shows through
                foundRedInk = true;
                break;
            }
        }
    CHECK(foundRedInk);
}

// image-nearest-magnify regression: a small checkerboard upscaled to a large
// cell span must stay CRISP — hard pixel edges, no blended intermediate color
// between the two checker colors. The CPU path samples nearest, so this pins the
// crisp behavior end-to-end; the Metal path gets the same via the dedicated
// _imageSampler (nearest magnification). A blurred (linear-magnify) sampler would
// smear a red<->green blend pixel along every block boundary.
TEST(testImageNearestMagnify)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 6, 8);

    // 2x2 RGB checkerboard, f=24: top row (red, green), bottom row (green, red).
    // Payload = base64 of the 12 raw bytes FF0000 00FF00 00FF00 FF0000. Displayed
    // at c=4,r=4 so the 2x2 source upscales into a 32x64px block (each source
    // texel -> a 16x32px device block). q=2 silences the kitty response.
    auto feed = [&](const char* s) { Ubiquitty::processBytes(t.get(), s); };
    feed("\x1b_Ga=T,f=24,s=2,v=2,c=4,r=4,q=2;/wAAAP8AAP8A/wAA\x1b\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);

    TermPalette pal;
    pal.seedDefaults();
    const int rows = t->rows;
    const int columns = t->columns;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (int row = 0; row < rows; ++row)
        Ubiquitty::Screen::getDisplayRow(*t, 0, row, &grid[static_cast<std::size_t>(row) * columns], columns);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.images = t->images.stored.data();
    in.imageCount = Ubiquitty::Image::maxTerminalImageCount;
    in.placements = t->images.placements.data();
    in.placementCount = Ubiquitty::Image::maxTerminalImagePlacementCount;
    in.viewOffset = 0;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(in, rgba, width, height));

    // The image (opaque f=24) fills device x in [0,32), y in [0,64) — 4 cells each
    // at 8x16px. Scan the whole block: every pixel must be strongly RED or strongly
    // GREEN, and never a mix of the two (a linear magnify would blend at edges).
    const int imageWidth = 4 * renderCellWidth; // 32
    const int imageHeight = 4 * renderCellHeight; // 64
    bool sawRed = false, sawGreen = false, sawBlend = false;
    for (int y = 0; y < imageHeight; ++y)
        for (int x = 0; x < imageWidth; ++x) {
            const Pixel p = at(rgba, width, x, y);
            if (p.red > 200 && p.green < 55 && p.blue < 55)
                sawRed = true;
            if (p.green > 200 && p.red < 55 && p.blue < 55)
                sawGreen = true;
            if (p.red > 55 && p.green > 55) // both channels mid/high = a red<->green blur
                sawBlend = true;
        }
    CHECK(sawRed); // the checkerboard's red texels didRender
    CHECK(sawGreen); // and its green texels
    CHECK(!sawBlend); // hard edges: no blended pixel anywhere — the crisp-upscale assertion

    // The source column boundary (texel 0|1) lands at device x=16 on the top band
    // (source row 0 = red|green): x=15 is still red, x=16 is green — a hard jump
    // with no gradient between, exactly what a linear magnify would smear.
    const Pixel leftOfEdge = at(rgba, width, 15, 8);
    const Pixel rightOfEdge = at(rgba, width, 16, 8);
    CHECK(leftOfEdge.red > 200 && leftOfEdge.green < 55);
    CHECK(rightOfEdge.green > 200 && rightOfEdge.red < 55);

    t.reset();
}

TEST(testScaledUnifontGeometry)
{
    UnifontRasterizer rasterizer(2, 4);
    const CellMetrics metrics = rasterizer.cellMetrics();
    CHECK(metrics.cellWidth == 16.0f);
    CHECK(metrics.cellHeight == 36.0f);
    CHECK(metrics.underlineThickness == 2.0f);
    CHECK(metrics.baselineFromTop == 29.0f);

    std::vector<std::uint8_t> coverage(16 * 36, 0);
    CHECK(rasterizer.rasterizeGlyph('A', FontFace::Regular, 0, 16, 36, coverage.data()));
    for (int x = 0; x < 16; ++x) {
        CHECK(coverage[x] == 0);
        CHECK(coverage[16 + x] == 0);
        CHECK(coverage[34 * 16 + x] == 0);
        CHECK(coverage[35 * 16 + x] == 0);
    }
    bool foundInk = false;
    for (int y = 2; y < 34; y += 2) {
        for (int x = 0; x < 16; x += 2) {
            CHECK(coverage[static_cast<std::size_t>(y) * 16 + x] == coverage[static_cast<std::size_t>(y) * 16 + x + 1]);
            CHECK(coverage[static_cast<std::size_t>(y) * 16 + x] == coverage[static_cast<std::size_t>(y + 1) * 16 + x]);
            foundInk = foundInk || coverage[static_cast<std::size_t>(y) * 16 + x] != 0;
        }
    }
    CHECK(foundInk);
}

// The bitmap tier's proportional face is FreeType's text, not a lookup of
// whole pixels: the strike keeps HarfBuzz's fractional advances and pair
// kerning, and a descender that Unifont's cell has no rows for is drawn into
// the slot's overhang rather than cut at the cell.
TEST(testUnifontProportionalStrikeKernsAndKeepsDescenders)
{
    // Scale two: the 24 px strike's g reaches a row past the 32-row cell,
    // where the 12 px strike's g still fits its three rows below the baseline.
    UnifontRasterizer rasterizer(2, 0);
    CHECK(rasterizer.hasProportionalFace());

    const std::array<std::uint32_t, 2> pair = { 'A', 'V' };
    const std::array<std::uint32_t, 1> alone = { 'A' };
    std::array<Ubiquitty::ProportionalShapedGlyph, 2> pairShaped {};
    std::array<Ubiquitty::ProportionalShapedGlyph, 1> aloneShaped {};
    CHECK(rasterizer.shapeProportionalRun(pair, FontFace::Regular, pairShaped) == 2);
    CHECK(rasterizer.shapeProportionalRun(alone, FontFace::Regular, aloneShaped) == 1);
    // A kerned pair tightens: the A before a V advances less than the A alone.
    CHECK(pairShaped[0].advanceInPixels < aloneShaped[0].advanceInPixels);
    // The advance keeps its fraction: at 24 px Noto Sans's A is not a whole
    // number of pixels wide, and a strike that rounded would answer one.
    CHECK(aloneShaped[0].advanceInPixels != std::floor(aloneShaped[0].advanceInPixels));

    // A 'g' at the cell's baseline: its descender reaches past the 32-row
    // cell, and the slot the strip is cut for holds it above and below.
    const int overhang = rasterizer.proportionalStripOverhangInPixels();
    CHECK(overhang >= 1);
    const int stripWidth = 32;
    const int stripHeight = 32 + 2 * overhang;
    const std::array<std::uint32_t, 1> descender = { 'g' };
    std::array<Ubiquitty::ProportionalShapedGlyph, 1> descenderShaped {};
    CHECK(rasterizer.shapeProportionalRun(descender, FontFace::Regular, descenderShaped) == 1);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(stripWidth) * static_cast<std::size_t>(stripHeight), 0);
    CHECK(rasterizer.rasteriseProportionalRun(descenderShaped, FontFace::Regular, stripWidth, stripHeight, coverage));
    const int baselineRow = overhang + static_cast<int>(rasterizer.cellMetrics().baselineFromTop);
    bool hasInkBelowBaseline = false;
    bool hasInkInBottomOverhang = false;
    for (int y = baselineRow; y < stripHeight; ++y) {
        for (int x = 0; x < stripWidth; ++x) {
            if (coverage[static_cast<std::size_t>(y) * stripWidth + x] == 0)
                continue;
            hasInkBelowBaseline = true;
            if (y >= overhang + 32)
                hasInkInBottomOverhang = true;
        }
    }
    CHECK(hasInkBelowBaseline);
    CHECK(hasInkInBottomOverhang);
}

TEST(testProportionalGlyphSheetOwnsItsBoundedCacheAndScale)
{
    UnifontRasterizer rasterizer(1, 0);
    ProportionalGlyphSheet oversized(INT_MAX, INT_MAX, INT_MAX, INT_MAX,
        &rasterizer, INT_MAX);
    CHECK(!oversized.isUsable());
    CHECK(oversized.coverage().empty());
    ProportionalGlyphSheet sheet(8, 16, 0, 0, &rasterizer,
        /*maxTextureDimension=*/64);
    CHECK(sheet.isUsable());
    ProportionalAtlasBinding binding = sheet.binding();
    CHECK(binding.cache != nullptr);
    CHECK(binding.slotCapacity > 0);
    CHECK(binding.slotCapacity < 128 * 64);
    CHECK(binding.phaseCount == subpixelPhaseCount);

    const AtlasRowBand initiallyDirty = sheet.dirtyRows();
    CHECK(initiallyDirty.firstRow == 0);
    CHECK(initiallyDirty.rowCount == sheet.heightInPixels());
    sheet.didUpload();
    CHECK(sheet.dirtyRows().rowCount == 0);

    std::vector<std::uint64_t> insertedKeys;
    for (int index = 0; index <= binding.slotCapacity; ++index) {
        const std::array<std::uint32_t, 1> text = {
            static_cast<std::uint32_t>('A' + index)
        };
        std::array<ProportionalShapedGlyph, 2> shaped {};
        CHECK(sheet.shapeRun(text, FontFace::Regular, shaped) == 1);
        const std::uint64_t key = proportionalGlyphKey(shaped[0].glyph, 0,
            FontFace::Regular, 0);
        sheet.beginFrame();
        bool isFresh = false;
        const int slot = binding.cache->slotForKey(key, &isFresh);
        CHECK(slot >= 0);
        CHECK(isFresh);
        CHECK(sheet.rasteriseGlyph(shaped[0].glyph, FontFace::Regular, 0, 0,
            slot));
        CHECK(binding.cache->slotHasOwnedInk(slot));
        const auto ink = binding.cache->ownedInkForSlot(slot, 0);
        CHECK(ink.leftInPixels >= 0.0f);
        CHECK(ink.rightInPixels <= binding.slotWidthInPixels);
        insertedKeys.push_back(key);
    }
    CHECK(sheet.generation()
        == static_cast<std::uint32_t>(binding.slotCapacity + 1));
    const AtlasRowBand changed = sheet.dirtyRows();
    CHECK(changed.rowCount > 0);
    CHECK(changed.rowCount <= sheet.heightInPixels());

    // One more frame proves the oldest identity was evicted within this
    // sheet's own budget. Its slot can be reused, but no wide or strip cache
    // participates in the choice because neither is reachable from binding.
    int evictedKeys = 0;
    for (const std::uint64_t key : insertedKeys) {
        sheet.beginFrame();
        bool isFreshAgain = false;
        const int recycled = binding.cache->slotForKey(key,
            &isFreshAgain);
        CHECK(recycled >= 0);
        if (isFreshAgain)
            ++evictedKeys;
    }
    CHECK(evictedKeys > 0);

    UnifontRasterizer scaledRasterizer(2, 0);
    ProportionalGlyphSheet scaled(16, 32, 0, 0, &scaledRasterizer,
        /*maxTextureDimension=*/128);
    const ProportionalAtlasBinding scaledBinding = scaled.binding();
    CHECK(scaledBinding.cache != binding.cache);
    CHECK(scaledBinding.slotWidthInPixels > binding.slotWidthInPixels);
    CHECK(scaledBinding.slotHeightInPixels > binding.slotHeightInPixels);
}
} // namespace

// The one row every pixel case below reads: 'A' in red, an underlined space, a
// wide CJK lead and its continuation, didRender once at the default palette.
//
// A helper rather than a fixture each case builds: the cases assert about
// DIFFERENT pixels of the same frame, and rebuilding it per case would let two
// of them disagree about what was drawn while both passed. It asserts nothing
// itself — CHECK increments a counter that only a case body declares.
struct RenderedRow {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool didRender = false;
};

static RenderedRow renderFixtureRow()
{
    TermPalette pal;
    pal.seedDefaults(); // default foreground C0C0C0, background 101014; ansi[1] = red 205,0,0

    const int rows = 1;
    const int columns = 4;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    setCell(grid, columns, 0, 0, 0x41u, 0, /*foreground*/ 1, colorDefault); // 'A' red
    setCell(grid, columns, 0, 1, 0x20u, static_cast<uint16_t>(Ubiquitty::VT::Attribute::Underline), colorDefault, colorDefault); // underlined space
    setCell(grid, columns, 0, 2, 0x4E00u, static_cast<uint16_t>(Ubiquitty::VT::Attribute::Wide), colorDefault, colorDefault); // wide CJK
    setCell(grid, columns, 0, 3, wideContinuationSentinelCodePoint, static_cast<uint16_t>(Ubiquitty::VT::Attribute::WideContinuation), colorDefault, colorDefault);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    RenderedRow out;
    out.didRender = renderer.render(in, out.rgba, out.width, out.height);
    return out;
}

TEST(testFourCellRowRendersAtCellGeometry)
{
    const RenderedRow row = renderFixtureRow();
    CHECK(row.didRender);
    CHECK(row.width == 4 * renderCellWidth); // 32
    CHECK(row.height == 1 * renderCellHeight); // 16
    CHECK(row.rgba.size() == static_cast<std::size_t>(row.width) * row.height * 4);
}

// Underlined space (column 1, x in [8,16)): background above the rule (row 8),
// and the single underline rule on row 15 (baseline 13.5 + drop 2) in the
// default foreground (C0C0C0) — the same decoration pass macOS draws. Pins that
// the rule renders + lands crisp.
TEST(testUnderlinedSpaceDrawsOneCrispRule)
{
    const RenderedRow row = renderFixtureRow();
    const Pixel above = at(row.rgba, row.width, 11, 8);
    CHECK(above.red == 0x10 && above.green == 0x10 && above.blue == 0x14 && above.alpha == 255);
    const Pixel rule = at(row.rgba, row.width, 11, 15);
    CHECK(rule.red == 0xC0 && rule.green == 0xC0 && rule.blue == 0xC0 && rule.alpha == 255);
    // Row 14 (just above the 1px rule on row 15) is still background.
    const Pixel justAbove = at(row.rgba, row.width, 11, 14);
    CHECK(justAbove.red == 0x10);
}

// Glyph cell (column 0): the top-left corner is empty (background), and at least
// one pixel carries the red foreground where 'A' has ink.
TEST(testGlyphCellPaintsForegroundInk)
{
    const RenderedRow row = renderFixtureRow();
    const Pixel corner = at(row.rgba, row.width, 0, 0);
    CHECK(corner.red == 0x10 && corner.green == 0x10 && corner.blue == 0x14);
    bool foundRed = false;
    for (int y = 0; y < renderCellHeight && !foundRed; ++y)
        for (int x = 0; x < renderCellWidth; ++x) {
            const Pixel p = at(row.rgba, row.width, x, y);
            if (p.red == 205 && p.green == 0 && p.blue == 0) {
                foundRed = true;
                break;
            }
        }
    CHECK(foundRed); // 'A' didRender in red
}

// Wide cell (lead at column 2) spans x in [16,32): its second half (x ~28) must
// be painted (opaque), proving the continuation column isn't an unpainted gap.
TEST(testWideCellSecondHalfIsPainted)
{
    const RenderedRow row = renderFixtureRow();
    const Pixel p = at(row.rgba, row.width, 28, 8);
    CHECK(p.alpha == 255);
}

TEST(stagedCoverageStaysInsideItsSlot)
{
    // The staging entry writes a Core-rasterised plane into one slot, row by row
    // at the atlas stride. A stride that walks corrupts the NEIGHBOUR while the
    // target slot compares byte-for-byte correct, so "did this slot change" is
    // the one question that cannot see it — @ubiquitty-frontends injected a
    // one-byte overrun in the Vulkan atlas and every target-slot assertion passed.
    Ubiquitty::Rendering::SoftwareRenderer renderer(8, 16);
    const Ubiquitty::Rendering::GlyphBackend backend = renderer.glyphBackend();
    CHECK_MSG(backend.stageMonoCoverage != &Ubiquitty::Rendering::declineMonoCoverage,
        "this renderer must declare the entry, or the case below stages nothing");

    const Ubiquitty::Rendering::MonoAtlasBinding mono = renderer.bindMonoAtlas();
    const int slotWidth = mono.geometry.slotWidthInPixels;
    const int slotHeight = mono.geometry.slotHeightInPixels;
    const std::vector<std::uint8_t> plane(static_cast<std::size_t>(slotWidth) * slotHeight, 0xFF);

    // Slot 1 rather than 0: an overrun off slot 0 runs into slot 1 forwards, and
    // a stride short by one walks BACKWARDS into slot 0. Staging in the middle
    // leaves a witness on both sides.
    CHECK(backend.stageMonoCoverage(backend.context, 1, std::span<const std::uint8_t>(plane)));

    const Ubiquitty::Rendering::SoftwareRenderer::AtlasView atlas = renderer.monoAtlas();
    CHECK_MSG(atlas.pixels != nullptr, "the atlas must exist, or every read below is vacuous");

    const auto slotIsClear = [&](int slot) {
        const int column = slot % mono.geometry.columns;
        const int row = slot / mono.geometry.columns;
        for (int y = 0; y < slotHeight; ++y) {
            const std::uint8_t* line = atlas.pixels
                + static_cast<std::size_t>(row * slotHeight + y) * atlas.width
                + static_cast<std::size_t>(column) * slotWidth;
            if (std::ranges::any_of(std::span(line, static_cast<std::size_t>(slotWidth)),
                    [](std::uint8_t byte) { return byte != 0; }))
                return false;
        }
        return true;
    };

    CHECK_MSG(!slotIsClear(1), "the staged slot carries the plane, or nothing was written at all");
    CHECK_MSG(slotIsClear(0), "staging slot 1 leaves slot 0 untouched");
    CHECK_MSG(slotIsClear(2), "staging slot 1 leaves slot 2 untouched");
    CHECK_MSG(slotIsClear(mono.geometry.columns + 1), "staging slot 1 leaves the slot below untouched");
}

// A doubled row's cells span twice their advance, and the compositor has to
// paint what the builder measured.
//
// `widthCells` is the builder's own quantity — (wide ? 2 : 1) x the row's
// xStep — so a DECDWL row makes a narrow cell 2 and a wide cell 4, and a
// straddling cell a fraction of either. A compositor reading it as a
// narrow-or-wide flag paints two cells for every one of those, which leaves
// the right half of a doubled wide cell holding whatever the surface had.
// Nothing else in this file renders a doubled row, and the GPU backends scale
// by the field rather than branching on it, so the CPU arm is the only place
// this can go wrong and the only place it was.
TEST(testDoubledRowCellsPaintTheirWholeSpan)
{
    TermPalette pal;
    pal.seedDefaults();

    const int rows = 1;
    const int columns = 8;
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    for (auto& c : grid) {
        c = Ubiquitty::Cell {};
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    // A wide CJK lead at column 0 of a DECDWL row: xStep 2 and a wide cell, so
    // the builder reports four cells of span starting at grid x 0.
    setCell(grid, columns, 0, 0, 0x4E00u, static_cast<uint16_t>(Ubiquitty::VT::Attribute::Wide), colorDefault, /*background*/ 1);
    setCell(grid, columns, 0, 1, wideContinuationSentinelCodePoint, static_cast<uint16_t>(Ubiquitty::VT::Attribute::WideContinuation), colorDefault, /*background*/ 1);

    const std::uint8_t doubleWidth = Ubiquitty::lineAttributeDoubleWidth;
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.lineAttributes = &doubleWidth;

    SoftwareRenderer renderer(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> rgba;
    int width = 0, height = 0;
    CHECK(renderer.render(in, rgba, width, height));

    // ansi[1] is red (205,0,0): the cell's background, so the span it painted
    // is visible as its own colour rather than as the default. Four cells of
    // span means x in [0, 4 * cellWidth); the fourth cell is the half a
    // boolean read of `widthCells` drops.
    for (int cell = 0; cell < 4; ++cell) {
        const int x = cell * renderCellWidth + renderCellWidth / 2;
        const Pixel p = at(rgba, width, x, renderCellHeight / 2);
        CHECK(p.red == 205 && p.green == 0 && p.blue == 0);
    }
    // And it stops there: column 4 onward is the default background, so a
    // compositor that painted the whole row would not pass this either.
    const Pixel past = at(rgba, width, 4 * renderCellWidth + renderCellWidth / 2, renderCellHeight / 2);
    CHECK(past.red == 0x10 && past.green == 0x10 && past.blue == 0x14);
}

UBIQUITTY_TEST_MAIN()

/// A renderer that has already drawn a frame draws the next one as a fresh one does.
///
/// `SoftwareRenderer` carries its glyph atlas between calls, so the second frame
/// over a given renderer takes a path the first cannot: it reads slots the first
/// frame filled. Every snapshot arm renders one frame per process — the portable
/// entry point constructs its renderer and its frame inputs as locals
/// (`TerminalSnapshot.cpp`), so no golden anywhere can observe a glyph the atlas
/// retained.
///
/// Rendering the same content twice would compare a cached answer with itself.
/// The warm renderer is asked for a frame half of which it has already drawn,
/// and held to what a renderer with no history produces for the whole of it. A
/// hit that resolves to the wrong slot, or a slot reused without being redrawn,
/// moves the warm image away from the cold one and nothing else here does.
TEST(testAWarmRendererDrawsWhatAColdOneDraws)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 1, 8);

    const auto rowInputs = [&terminal](const char* text, Ubiquitty::Cell* row) {
        Ubiquitty::processBytes(&terminal, "\x1b[2J\x1b[H");
        Ubiquitty::processBytes(&terminal, text);
        Ubiquitty::Screen::getDisplayRow(terminal, 0, 0, row, 8);
        BuildInputs inputs;
        inputs.cells = row;
        inputs.rows = 1;
        inputs.columns = 8;
        inputs.palette = &terminal.palette;
        return inputs;
    };

    Ubiquitty::Cell firstRow[8] {};
    Ubiquitty::Cell secondRow[8] {};
    const BuildInputs first = rowInputs("abcdefgh", firstRow);
    // Overlapping, not disjoint. A second frame sharing no glyph with the first
    // takes only misses, and the atlas it inherits then contributes nothing the
    // comparison could see. Half of these are slots the warm renderer filled on
    // the frame before and half are new, so the warm run mixes hits with misses
    // where the cold run has misses alone.
    const BuildInputs second = rowInputs("abcdijkl", secondRow);

    int width = 0;
    int height = 0;

    // The renderer that has a history: draw one frame, then the frame under test.
    SoftwareRenderer warm(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> firstPixels;
    CHECK(warm.render(first, firstPixels, width, height));
    std::vector<std::uint8_t> warmPixels;
    CHECK(warm.render(second, warmPixels, width, height));

    // The same frame with nothing behind it.
    SoftwareRenderer cold(renderCellWidth, renderCellHeight);
    std::vector<std::uint8_t> coldPixels;
    CHECK(cold.render(second, coldPixels, width, height));

    // Without this the comparison could hold because the two frames are the
    // same image, which would make a retained slot undetectable by construction.
    CHECK(firstPixels.size() == warmPixels.size());
    CHECK(firstPixels != warmPixels);

    CHECK(warmPixels.size() == coldPixels.size());
    CHECK(warmPixels == coldPixels);
}

// A bold cell reaches the rasterizer as bold.
//
// The slot keys carry the face — `monoGlyphKey` and `wideGlyphKey` both take
// one — so a bold cell is given a slot of its own. A fill that names
// `FontFace::Regular` outright leaves the key saying bold and the bytes in the
// slot regular: one cache, two answers about the same slot.
//
// Invisible to the tier this renderer is drawn for. `UnifontRasterizer`
// ignores the face, having one weight to draw, so nothing the portable arm
// renders can tell the difference — and the web arm, which injects a rasterizer
// backed by a face that does have a bold member, would draw bold text regular. The
// snapshot arms cannot see it either: the web arm runs with no DOM and takes
// the same Unifont fallback.
//
// So the oracle is a rasterizer that answers differently per face, which is
// what a real one does and what the injected seam exists to allow.
namespace {
class FaceRecordingRasterizer final : public Rendering::GlyphRasterizer {
public:
    [[nodiscard]] Rendering::CellMetrics cellMetrics() const override { return m_unifont.cellMetrics(); }
    [[nodiscard]] Rendering::GlyphOverhangInputs glyphOverhang() const override { return m_unifont.glyphOverhang(); }

    bool rasterizeGlyph(std::uint32_t codePoint, Rendering::FontFace face, std::uint32_t weight,
        int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        m_facesSeen.push_back(face);
        if (!m_unifont.rasterizeGlyph(codePoint, face, weight, slotWidth, slotHeight, coverageOut))
            return false;
        // A bold face draws heavier, the way a real one does: without this the
        // two slots hold identical bytes and the test cannot tell a face that
        // arrived from a face that was ignored.
        if (Rendering::isBold(face)) {
            for (int i = 0; i < slotWidth * slotHeight; ++i)
                coverageOut[i] = static_cast<std::uint8_t>(coverageOut[i] / 2 + 128);
        }
        return true;
    }

    [[nodiscard]] const std::vector<Rendering::FontFace>& facesSeen() const { return m_facesSeen; }

private:
    Rendering::UnifontRasterizer m_unifont;
    std::vector<Rendering::FontFace> m_facesSeen;
};
} // namespace

TEST(testABoldCellReachesTheRasterizerAsBold)
{
    FaceRecordingRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);

    const AtlasGlyph regular = renderer.monoGlyph('A', FontFace::Regular, 0, 0);
    const AtlasGlyph bold = renderer.monoGlyph('A', FontFace::Bold, 0, 0);
    CHECK(regular.present && bold.present);

    // Separate slots, which the keys already arranged: the same glyph at two
    // faces must not share one.
    CHECK(regular.minUV.x != bold.minUV.x || regular.minUV.y != bold.minUV.y);

    // And the faces the rasterizer was actually asked for. Checked here rather
    // than by comparing pixels alone, because the slot a key selects and the
    // bytes written into it are the two halves that disagreed.
    const auto& seen = rasterizer.facesSeen();
    CHECK(seen.size() == 2);
    CHECK(seen[0] == FontFace::Regular);
    CHECK(seen[1] == FontFace::Bold);
}

TEST(testTrackingMovesGlyphInkWithoutChangingItsCoverage)
{
    struct StemRasterizer final : GlyphRasterizer {
        CellMetrics cellMetrics() const override
        {
            CellMetrics metrics;
            metrics.cellWidth = 8.0f;
            metrics.cellHeight = 16.0f;
            metrics.baselineFromTop = 13.0f;
            return metrics;
        }
        bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverage) override
        {
            std::fill_n(coverage, static_cast<std::size_t>(slotWidth) * slotHeight, 0);
            for (int row = 3; row < 12; ++row) {
                coverage[row * slotWidth + 2] = 60;
                coverage[row * slotWidth + 3] = 220;
                coverage[row * slotWidth + 4] = 90;
            }
            return true;
        }
    } rasterizer;
    SoftwareRenderer renderer(8, 16, rasterizer);
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 16;
    std::vector<Cell> cells(columns, defaultBlankCell);
    for (auto& cell : cells) {
        cell.pen.foregroundIndex = 15;
        cell.pen.backgroundIndex = 0;
    }
    cells[1].codePoint = 'l';
    cells[4].codePoint = 'l';
    BuildInputs inputs;
    inputs.cells = cells.data();
    inputs.rows = 1;
    inputs.columns = columns;
    inputs.palette = &palette;
    inputs.subpixelGlyphs = true;
    inputs.metrics = rasterizer.cellMetrics();
    inputs.glyphReferenceAdvanceInPixels = 8.0f;
    inputs.glyphReferenceLineHeightInPixels = 16.0f;
    inputs.glyphSlotWidthInPixels = 8.0f;
    inputs.glyphSlotHeightInPixels = 16.0f;
    inputs.viewportSizeInPixels = { 96.0f, 16.0f };
    std::vector<std::uint8_t> reference;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(inputs, reference, width, height));
    REQUIRE(width == 96 && height == 16);
    int referenceCoverage = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            referenceCoverage += at(reference, width, x, y).red;
    CHECK(referenceCoverage == 2 * 9 * (60 + 220 + 90));

    for (float trackedPitch : { 7.0f, 10.0f, 11.0f }) {
        inputs.metrics.cellWidth = trackedPitch;
        std::vector<std::uint8_t> tracked;
        REQUIRE(renderer.render(inputs, tracked, width, height));
        int differing = 0;
        int trackedCoverage = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                Pixel expected { 0, 0, 0, 255 };
                for (int column : { 1, 4 }) {
                    const int left = static_cast<int>(column * trackedPitch);
                    if (x >= left && x < left + 8)
                        expected = at(reference, width, x - left + column * 8, y);
                }
                const Pixel actual = at(tracked, width, x, y);
                differing += actual.red != expected.red || actual.green != expected.green
                    || actual.blue != expected.blue || actual.alpha != expected.alpha;
                trackedCoverage += actual.red;
            }
        }
        CHECK_MSG(differing == 0, "tracking translates each original stem without resampling its coverage");
        CHECK(trackedCoverage == referenceCoverage);
    }
}

// A natural placement keeps its device-pixel footprint at every backing
// scale: a two-by-two image is four device pixels at 1x and at 2x alike,
// which is the footprint the parser's cursor advance was computed from.
TEST(aNaturalImageKeepsItsDevicePixelsAtEveryScale)
{
    for (int scale : { 1, 2 }) {
        auto terminal = std::make_unique<Terminal>();
        resize(*terminal, 4, 8);
        processBytes(terminal.get(), "\x1b_Ga=T,f=24,s=2,v=2,q=2;/wAA/wAA/wAA/wAA\x1b\\");
        TermPalette palette;
        palette.seedDefaults();
        std::vector<Cell> cells(32);
        BuildInputs inputs;
        inputs.cells = cells.data();
        inputs.rows = 4;
        inputs.columns = 8;
        inputs.palette = &palette;
        inputs.images = terminal->images.stored.data();
        inputs.imageCount = Image::maxTerminalImageCount;
        inputs.placements = terminal->images.placements.data();
        inputs.placementCount = Image::maxTerminalImagePlacementCount;
        UnifontRasterizer rasterizer(scale);
        SoftwareRenderer renderer(rasterizer);
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
        CHECK(renderer.render(inputs, rgba, width, height));
        int redPixels = 0;
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const auto pixel = at(rgba, width, column, row);
                if (pixel.red > 240 && pixel.green < 10 && pixel.blue < 10) {
                    ++redPixels;
                    CHECK(column < 2 && row < 2);
                }
            }
        }
        CHECK_EQ(redPixels, 4);
    }
}

namespace {
// A proportional primary face as a row laid out as letters sees it: the pitch
// is proportional, a run shapes to advances that are not whole pixels, and the
// glyphs themselves are Unifont's.
class ProportionalPitchRasterizer final : public Rendering::GlyphRasterizer {
public:
    [[nodiscard]] Rendering::CellMetrics cellMetrics() const override { return m_unifont.cellMetrics(); }
    [[nodiscard]] Rendering::GlyphOverhangInputs glyphOverhang() const override { return m_unifont.glyphOverhang(); }
    [[nodiscard]] Rendering::FontMetrics::FacePitch primaryFacePitch() const override { return Rendering::FontMetrics::FacePitch::Proportional; }

    bool rasterizeGlyph(std::uint32_t codePoint, Rendering::FontFace face, std::uint32_t weight,
        int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        return m_unifont.rasterizeGlyph(codePoint, face, weight, slotWidth, slotHeight, coverageOut);
    }

    int shapePrimaryRun(std::span<const std::uint32_t> codePoints, Rendering::FontFace, std::uint32_t,
        std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (codePoints.size() > glyphsOut.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = codePoints[index] == U'W' ? 6.3f : 5.4f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }

private:
    Rendering::UnifontRasterizer m_unifont;
};
} // namespace

// Two letters meeting on a fractional boundary share the pixel column between
// them. The cells of a row laid out as letters stand at pens that are not
// whole pixels, and a span rounded on its own from a rounded left edge leaves
// a column between two of them that nothing paints: W's 6.3 px round to 6, i's
// 5.4 px to 5, and the empty cell after them starts at the 12 their sum rounds
// to, so column 11 was the surface's. The span is cut at the rounded right
// edge instead, and every column from the first letter to the last empty cell
// is painted.
TEST(testLetterCellsShareTheirFractionalBoundary)
{
    const int columns = 4;
    const int rows = 1;
    TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows);
    for (auto& c : grid) {
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    setCell(grid, columns, 0, 0, U'W', 0, colorDefault, colorDefault);
    setCell(grid, columns, 0, 1, U'i', 0, colorDefault, colorDefault);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.subpixelGlyphs = true;

    ProportionalPitchRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));
    REQUIRE(width == columns * renderCellWidth);

    // The letters end at 11.7 px and the two empty cells keep the pitch after
    // them, so the row's cells reach 27.7 px: every column up to the 28 that
    // rounds to is painted by the cell standing on it.
    for (int x = 0; x < 28; ++x) {
        const Pixel p = at(rgba, width, x, 8);
        const std::string column = "column " + std::to_string(x) + " is painted by the cell standing on it";
        CHECK_MSG(p.alpha == 255, column.c_str());
    }
    // The seam between i and the empty cell after it carries the background,
    // not ink and not the surface.
    const Pixel seam = at(rgba, width, 11, 8);
    CHECK_MSG(seam.red == 0x10 && seam.green == 0x10 && seam.blue == 0x14, "the shared column holds the background");
}

// A pane whose grid face is proportional paints every pixel of every row. The
// letters' advances sum to less than the row's width when the face sets them
// narrower than the digit cell, and no cell stands past the last one; the
// builder paints the rest of such a row in the pane's background, so the
// surface shows through nowhere, as it shows through nowhere on a grid row.
TEST(testAProportionalPanePaintsEveryPixelOfEveryRow)
{
    const int columns = 6;
    const int rows = 3;
    TermPalette pal;
    pal.seedDefaults();
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(columns) * rows);
    for (auto& c : grid) {
        c.pen.foregroundIndex = colorDefault;
        c.pen.backgroundIndex = colorDefault;
        c.pen.underlineIndex = colorDefault;
    }
    // A row of narrow letters, a row of one letter and empties, a row left blank.
    for (int column = 0; column < columns; ++column)
        setCell(grid, columns, 0, column, U'i', 0, colorDefault, colorDefault);
    setCell(grid, columns, 1, 0, U'W', 0, colorDefault, colorDefault);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &pal;
    in.subpixelGlyphs = true;

    ProportionalPitchRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));
    REQUIRE(width == columns * renderCellWidth && height == rows * renderCellHeight);

    // Six i's reach 32.4 px of the row's 48, so a third of the first row is
    // past its cells; the W's row keeps five empties at the pitch after 6.3 px
    // and stops 1.7 px short; the blank row's cells reach the edge.
    int unpainted = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            unpainted += at(rgba, width, x, y).alpha == 255 ? 0 : 1;
    const std::string holes = std::to_string(unpainted) + " pixel(s) of the pane are unpainted, wanting none";
    CHECK_MSG(unpainted == 0, holes.c_str());
    // Past the letters the pane holds the default background, not ink.
    const Pixel rest = at(rgba, width, 40, 8);
    CHECK_MSG(rest.red == 0x10 && rest.green == 0x10 && rest.blue == 0x14, "the rest of a letter row is the pane's background");
}

// The portable arm's view of an explicit width: `s=3:w=3` claims nine columns
// a band and its glyph three. The face draws its 1x glyph blank and its sized
// master solid, so ink can only come from sharp windows cut from the master.
// Counted across the glyph, the three bands draw it into exactly its three
// columns; counted across the block, the nine-window band was refused and the
// blank 1x raster drew nothing.
TEST(testAnExplicitWidthBlockIsCutFromItsMasterOnThePortableArm)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/3, /*widthCells=*/3 });
    constexpr int rows = 3;
    constexpr int columns = 12;
    constexpr int claim = 9;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0u, 0, 15, 0);
    }
    for (int band = 0; band < rows; ++band) {
        for (int offset = 0; offset < claim; ++offset) {
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + offset];
            cell.codePoint = offset == 0 ? 0x58u : 0u;
            Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    ScaledOnlyRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));

    const Pixel background = at(pixels, width, width - 1, height - 1);
    int inkInGlyph = 0;
    int inkPastGlyph = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel p = at(pixels, width, x, y);
            if (p.red == background.red && p.green == background.green && p.blue == background.blue)
                continue;
            ++(x < 3 * renderCellWidth ? inkInGlyph : inkPastGlyph);
        }
    }
    const std::string inked = "the glyph's three columns of every band are cut from the master: "
        + std::to_string(inkInGlyph) + " of " + std::to_string(3 * renderCellWidth * rows * renderCellHeight) + " pixels";
    CHECK_MSG(inkInGlyph == 3 * renderCellWidth * rows * renderCellHeight, inked.c_str());
    CHECK_MSG(inkPastGlyph == 0, "the explicit width's spare columns draw nothing");
}

namespace {

// A face whose master carries its own column number as coverage, four a column,
// so the texel a screen pixel shows can be read back from the pixel.
struct ColumnCodedRasterizer final : GlyphRasterizer {
    static std::uint8_t coverageOfColumn(int column) { return static_cast<std::uint8_t>(4 * (1 + column % 60)); }

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, std::uint8_t { 0 });
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster&, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int y = 0; y < slotHeight; ++y) {
            for (int x = 0; x < slotWidth; ++x)
                coverageOut[static_cast<std::size_t>(y) * slotWidth + x] = coverageOfColumn(x);
        }
        return true;
    }
};

} // namespace

// A sized block on the cell-baked arm lands its windows on grid cells, a whole
// cell box apart, so each window is the box wide and is drawn texel for texel.
// A face whose advance is 7.65 on 8 pixel cells would, cut at the advance and
// drawn on the cell, repeat a column of the master at every join, which is the
// block stretched by 4 percent. The master carries its column number as
// coverage: along the block's row every pixel shows the column after the one
// before it.
TEST(testACellBakedBlockIsDrawnTexelForTexelOnItsGridCells)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    constexpr int scale = 3;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { scale, /*widthCells=*/0 });
    constexpr int columns = scale;
    std::vector<Cell> grid(static_cast<std::size_t>(scale) * columns, defaultBlankCell);
    for (int band = 0; band < scale; ++band) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, band, column, column == 0 ? 0x58u : 0u, 0, 15, 0);
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + column];
            Pen pen = cell.pen;
            pen.attributes.add(column == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = scale;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.glyphReferenceAdvanceInPixels = 7.65f;
    ColumnCodedRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));
    REQUIRE(width == columns * renderCellWidth);

    const int y = renderCellHeight + 4;
    const auto shown = [&](int x) { return static_cast<int>(at(pixels, width, x, y).green); };
    int repeated = 0;
    int skipped = 0;
    for (int x = 0; x + 1 < columns * renderCellWidth; ++x) {
        const int step = shown(x + 1) - shown(x);
        repeated += step <= 0 ? 1 : 0;
        skipped += step >= 6 ? 1 : 0;
    }
    const std::string seams = std::to_string(repeated) + " column(s) of the master drawn twice and " + std::to_string(skipped) + " skipped along the block";
    CHECK_MSG(repeated == 0 && skipped == 0, seams.c_str());
}

namespace {

// A face whose sized master is one straight stroke, drawn exactly where Core's
// placement says: the pen ``sizedMasterPen`` gives, moved down by the shift,
// with every pixel the area the stroke covers of it. Down the block the stroke
// leans `slope` pixels across a row, so where the block is one glyph each
// pixel row's ink is centred on one line, whichever window and band drew it.
struct DiagonalStrokeRasterizer final : GlyphRasterizer {
    static constexpr double advance = 7.6;
    static constexpr double slope = 0.3;

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, std::uint8_t { 0 });
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        const double down = master.scale.value();
        const SubpixelPhase pen = sizedMasterPen(slotWidth, master.horizontalMagnification().value() * advance, master);
        const double penX = pen.snappedPenX + static_cast<double>(pen.phase) / sizedMasterPenGrid;
        // The box's top, which the master's centre places: every master of a
        // block is the same size, so this is the same place in each.
        const double boxTop = (slotHeight - down * renderCellHeight) / 2.0 + master.shiftY;
        const double halfWidth = 0.6 * down;
        constexpr int samplesDown = 16;
        for (int y = 0; y < slotHeight; ++y) {
            for (int x = 0; x < slotWidth; ++x) {
                double covered = 0.0;
                for (int sample = 0; sample < samplesDown; ++sample) {
                    const double v = y + (sample + 0.5) / samplesDown - boxTop;
                    if (v < down || v > (renderCellHeight - 1) * down)
                        continue;
                    const double centre = penX + 1.5 * down + slope * v;
                    covered += std::max(0.0, std::min(centre + halfWidth, x + 1.0) - std::max(centre - halfWidth, static_cast<double>(x)));
                }
                coverageOut[static_cast<std::size_t>(y) * slotWidth + x] = static_cast<std::uint8_t>(std::lround(255.0 * covered / samplesDown));
            }
        }
        return true;
    }
};

} // namespace

// A sized block on the subpixel arm steps its windows by the face's fractional
// advance, and every window and band of it is cut from a master of the same
// glyph at the same place: drawn, the block is one stroke, whose ink centre
// moves by the stroke's slope from each pixel row to the next across every
// window join and every band join. A window begun at a fraction of a pixel
// would be drawn from the whole pixel below it, the texel a pixel shows taken
// that fraction from the one cut for it, and the stroke would step sideways at
// the join by the fraction.
TEST(testASizedStrokeRunsOnAcrossEveryWindowAndBandJoinOnThePortableArm)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    constexpr int scale = 3;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { scale, /*widthCells=*/0 });
    constexpr int columns = scale + 2;
    constexpr int blockColumn = 1;
    std::vector<Cell> grid(static_cast<std::size_t>(scale) * columns, defaultBlankCell);
    for (int band = 0; band < scale; ++band) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, band, column, 0u, 0, 15, 0);
        for (int offset = 0; offset < scale; ++offset) {
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + blockColumn + offset];
            cell.codePoint = offset == 0 ? 0x5Cu : 0u;
            Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = scale;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.subpixelGlyphs = true;
    input.glyphReferenceAdvanceInPixels = static_cast<float>(DiagonalStrokeRasterizer::advance);
    DiagonalStrokeRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));

    // Each pixel row's ink across the block, from the green channel's rise
    // over the background: how much of the stroke it holds, and where its centre is.
    const int background = at(pixels, width, width - 1, height - 1).green;
    const int ink = 255 - background;
    REQUIRE(ink > 0);
    constexpr double strokeWidth = 2 * 0.6 * scale;
    std::vector<int> rows;
    std::vector<double> centres;
    int partialRows = 0;
    for (int y = 0; y < height; ++y) {
        double weight = 0.0;
        double moment = 0.0;
        for (int x = blockColumn * renderCellWidth - 4; x < (blockColumn + scale) * renderCellWidth + 4; ++x) {
            const double coverage = static_cast<double>(at(pixels, width, x, y).green - background) / ink;
            weight += coverage;
            moment += coverage * (x + 0.5);
        }
        if (weight < 0.05)
            continue;
        partialRows += std::fabs(weight - strokeWidth) > 0.1 ? 1 : 0;
        rows.push_back(y);
        centres.push_back(moment / weight);
    }
    REQUIRE(rows.size() > static_cast<std::size_t>(2 * renderCellHeight));
    REQUIRE(rows.front() < renderCellHeight && rows.back() >= (scale - 1) * renderCellHeight);
    const std::string held = std::to_string(partialRows) + " pixel row(s) of the stroke hold more or less than its width";
    CHECK_MSG(partialRows == 0, held.c_str());
    double worst = 0.0;
    int worstRow = -1;
    for (std::size_t index = 1; index < rows.size(); ++index) {
        const double error = std::fabs(centres[index] - centres[index - 1] - DiagonalStrokeRasterizer::slope);
        if (error > worst) {
            worst = error;
            worstRow = rows[index];
        }
    }
    const std::string where = "the stroke steps " + std::to_string(worst) + " pixels off its slope at row " + std::to_string(worstRow);
    CHECK_MSG(worst <= 0.125, where.c_str());
}

namespace {

// A face whose every row of ink is told apart from every other: the coverage of
// row `r` of the glyph at its own size is a function of `r` alone, and the
// master a block is cut from holds the same function down all its rows. Where
// the block's picture shows a row twice, or skips one, the rows after it no
// longer match the glyph's own.
struct RowMarkedRasterizer final : GlyphRasterizer {
    static std::uint8_t coverageOfRow(int row) { return static_cast<std::uint8_t>(5 * (1 + (row * 7) % 50)); }

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    // The 1x glyph `A` carries the rows the first band of a master does, `B` the
    // second's: the unsized picture of the rows a block must show.
    bool rasterizeGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        const int firstRow = (static_cast<int>(codePoint) - 'A') * renderCellHeight;
        for (int y = 0; y < slotHeight; ++y)
            std::fill_n(coverageOut + static_cast<std::size_t>(y) * slotWidth, slotWidth, coverageOfRow(firstRow + y));
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster&, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int y = 0; y < slotHeight; ++y)
            std::fill_n(coverageOut + static_cast<std::size_t>(y) * slotWidth, slotWidth, coverageOfRow(y));
        return true;
    }
};

// The brightest channel of the pixel, which carries the mixed coverage.
int inkOf(const Pixel& pixel)
{
    return pixel.green;
}

} // namespace

// A band shows the rows of its own cell and no others. The atlas holds a slot's
// cell box as the line rounded up, so its box is a row taller than the pitch the
// glyph stream lays the frame out on; a band cut a box along, or drawn a box
// tall, shows the rows the band above it shows, and the two quads blend them a
// second time where they overlap: a line across the glyph at every join, past
// its ink. Scale three on a 17-row box over 16-row cells is drawn beside the
// same block on a box of exactly the pitch, where the bands cannot overlap:
// every row of the first is the row of the second.
TEST(testSizedBandsPartitionTheGlyphWhereTheCellBoxIsTallerThanTheRow)
{
    constexpr int scale = 3;
    constexpr int columns = scale;
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/scale, /*widthCells=*/0 });

    std::vector<Cell> grid(static_cast<std::size_t>(scale) * columns, defaultBlankCell);
    for (int band = 0; band < scale; ++band) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, band, column, column == 0 ? 0x58u : 0u, 0, 15, 0);
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + column];
            Pen pen = cell.pen;
            pen.attributes.add(column == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    const auto drawWithBox = [&](int boxHeight, std::vector<std::uint8_t>& pixels) {
        RowMarkedRasterizer rasterizer;
        SoftwareRenderer renderer(renderCellWidth, boxHeight, rasterizer);
        BuildInputs input;
        input.cells = grid.data();
        input.rows = scale;
        input.columns = columns;
        input.palette = &palette;
        input.textSizeTable = &table;
        input.subpixelGlyphs = true;
        input.metrics = rasterizer.cellMetrics();
        // The geometry of the atlas the renderer holds, as a caller that lays
        // its frame out on another pitch reports it.
        input.glyphSlotWidthInPixels = static_cast<float>(renderCellWidth);
        input.glyphSlotHeightInPixels = static_cast<float>(boxHeight);
        input.glyphReferenceLineHeightInPixels = static_cast<float>(renderCellHeight);
        input.viewportSizeInPixels = { static_cast<float>(columns * renderCellWidth), static_cast<float>(scale * renderCellHeight) };
        int width = 0;
        int height = 0;
        REQUIRE(renderer.render(input, pixels, width, height));
        REQUIRE(width == columns * renderCellWidth && height == scale * renderCellHeight);
    };
    std::vector<std::uint8_t> partition;
    std::vector<std::uint8_t> roundedUp;
    drawWithBox(renderCellHeight, partition);
    drawWithBox(renderCellHeight + 1, roundedUp);

    int mismatched = 0;
    int firstMismatch = -1;
    for (int y = 0; y < scale * renderCellHeight; ++y) {
        if (inkOf(at(partition, columns * renderCellWidth, 3, y)) == inkOf(at(roundedUp, columns * renderCellWidth, 3, y)))
            continue;
        ++mismatched;
        if (firstMismatch < 0)
            firstMismatch = y;
    }
    const std::string rows = std::to_string(mismatched) + " of " + std::to_string(scale * renderCellHeight)
        + " rows differ from the block on a box of the pitch, the first at row " + std::to_string(firstMismatch);
    CHECK_MSG(mismatched == 0, rows.c_str());
    // The rows are told apart, so agreeing is not both being blank.
    CHECK_MSG(inkOf(at(partition, columns * renderCellWidth, 3, 17)) != inkOf(at(partition, columns * renderCellWidth, 3, 18)), "the master's rows are distinct on screen");
}

namespace {

// A face with no sized entry, whose 1x glyph is ink over its whole slot: a
// sized block in it is drawn soft, each band a crop of that one raster.
struct SolidUnsizedRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, std::uint8_t { 255 });
        return true;
    }
};

} // namespace

// A soft sized block is as solid at its band joins as inside its bands. At
// scale three a 16-row glyph gives each band five and a third of its rows, so
// a join falls inside a row of the raster, and the band's quad, magnified,
// filters across it. A tap past the band's own rows must read the raster's
// edge row rather than nothing: blending with nothing draws a lighter line
// across a solid glyph at every join.
TEST(testASoftSizedBlockIsSolidAcrossItsBandJoins)
{
    constexpr int scale = 3;
    constexpr int columns = scale;
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/scale, /*widthCells=*/0 });

    std::vector<Cell> grid(static_cast<std::size_t>(scale) * columns, defaultBlankCell);
    for (int band = 0; band < scale; ++band) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, band, column, column == 0 ? 0x58u : 0u, 0, 15, 0);
            Cell& cell = grid[static_cast<std::size_t>(band) * columns + column];
            Pen pen = cell.pen;
            pen.attributes.add(column == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(band));
            cellSetPen(cell, pen);
        }
    }
    SolidUnsizedRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    BuildInputs input;
    input.cells = grid.data();
    input.rows = scale;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.subpixelGlyphs = true;
    input.metrics = rasterizer.cellMetrics();
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));
    REQUIRE(width == columns * renderCellWidth && height == scale * renderCellHeight);

    // The same frame with no block, for the ground the block must differ from.
    std::vector<Cell> blank(grid.size(), defaultBlankCell);
    for (int band = 0; band < scale; ++band) {
        for (int column = 0; column < columns; ++column)
            setCell(blank, columns, band, column, 0u, 0, 15, 0);
    }
    input.cells = blank.data();
    std::vector<std::uint8_t> ground;
    REQUIRE(renderer.render(input, ground, width, height));

    const int middle = columns * renderCellWidth / 2;
    const int solid = inkOf(at(pixels, width, middle, height / 2));
    CHECK_MSG(solid != inkOf(at(ground, width, middle, height / 2)), "the block is drawn");
    int lighterRows = 0;
    int firstLighterRow = -1;
    for (int y = 0; y < height; ++y) {
        if (inkOf(at(pixels, width, middle, y)) == solid)
            continue;
        ++lighterRows;
        if (firstLighterRow < 0)
            firstLighterRow = y;
    }
    const std::string rows = std::to_string(lighterRows) + " of " + std::to_string(height)
        + " rows down the block's middle differ from its solid ink, the first at row " + std::to_string(firstLighterRow);
    CHECK_MSG(lighterRows == 0, rows.c_str());
}

// A screen of the largest sized text: ninety-six letters at s=7, side by side
// across seven rows, every window of every band a distinct slot. That is more
// windows than a sheet cut for a smaller screen holds, so the sheet must be
// cut for this one, and every band of every letter is drawn from its sharp
// windows. The face draws its 1x glyph blank and its sized master solid, so a
// band that fell back to the magnified crop shows no ink at all.
TEST(testAScreenOfScaleSevenLettersIsDrawnSharp)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    constexpr int scale = 7;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/scale, /*widthCells=*/1 });
    constexpr int letters = 96;
    // One plain row under the letters, for the background to compare with.
    constexpr int rows = scale + 1;
    constexpr int columns = letters * scale;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0u, 0, 15, 0);
    }
    for (int letter = 0; letter < letters; ++letter) {
        for (int band = 0; band < scale; ++band) {
            for (int offset = 0; offset < scale; ++offset) {
                Cell& cell = grid[static_cast<std::size_t>(band) * columns + static_cast<std::size_t>(letter * scale + offset)];
                cell.codePoint = offset == 0 ? static_cast<std::uint32_t>(0x100 + letter) : 0u;
                Pen pen = cell.pen;
                pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                penSetTextBand(&pen, static_cast<std::uint8_t>(band));
                cellSetPen(cell, pen);
            }
        }
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    ScaledOnlyRasterizer rasterizer;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(input, pixels, width, height));

    const Pixel background = at(pixels, width, width - 1, height - 1);
    int softBands = 0;
    std::string firstSoft;
    for (int letter = 0; letter < letters; ++letter) {
        for (int band = 0; band < scale; ++band) {
            int inked = 0;
            for (int y = band * renderCellHeight; y < (band + 1) * renderCellHeight; ++y) {
                for (int x = letter * scale * renderCellWidth; x < (letter + 1) * scale * renderCellWidth; ++x) {
                    const Pixel p = at(pixels, width, x, y);
                    inked += p.red == background.red && p.green == background.green && p.blue == background.blue ? 0 : 1;
                }
            }
            if (inked != scale * renderCellWidth * renderCellHeight) {
                if (softBands++ == 0)
                    firstSoft = "letter " + std::to_string(letter) + " band " + std::to_string(band) + " inks " + std::to_string(inked) + " of "
                        + std::to_string(scale * renderCellWidth * renderCellHeight) + " pixels";
            }
        }
    }
    const std::string sharp = std::to_string(softBands) + " of " + std::to_string(letters * scale) + " bands are not drawn sharp" + (softBands > 0 ? ", the first " + firstSoft : std::string {});
    CHECK_MSG(softBands == 0, sharp.c_str());
}

namespace {

// A proportional face whose glyph is three pixels of ink a pixel of advance,
// every row of it told apart from every other as `RowMarkedRasterizer`'s
// are, at its own size and at every master's: the ink at `scale` is `scale`
// times as wide, and row `r` of a master carries row `r`'s coverage.
struct RowMarkedProportionalRasterizer final : GlyphRasterizer {
    static constexpr float advanceInPixels = 4.0f;
    std::vector<int> mastersAsked;
    // How wide a glyph's ink is at its own size; wider than the advance, the
    // letters' ink overlaps and the run is drawn as a strip.
    int inkColumns = 3;
    // The most glyphs one master held: one on the per-glyph route, the run's
    // on the strip route.
    std::size_t largestMasterRun = 0;

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(renderCellWidth);
        metrics.cellHeight = static_cast<float>(renderCellHeight);
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight, std::uint8_t { 0 });
        return true;
    }
    [[nodiscard]] bool hasProportionalFace() const override { return true; }
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (codePoints.size() > glyphsOut.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = advanceInPixels;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    bool rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        return draw(glyphs, 1, inkColumns, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }
    bool rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace, const SizedMaster& master, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        mastersAsked.push_back(master.scale.whole());
        largestMasterRun = std::max(largestMasterRun, glyphs.size());
        return draw(glyphs, master.scale.whole(), inkColumns, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }

private:
    static bool draw(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, int scale, int inkColumns, int width, int height, std::span<std::uint8_t> coverageOut)
    {
        std::ranges::fill(coverageOut, std::uint8_t { 0 });
        float penX = 0.0f;
        for (const Ubiquitty::ProportionalShapedGlyph& glyph : glyphs) {
            const int left = static_cast<int>(std::floor(penX + glyph.offsetXInPixels));
            for (int y = 0; y < height; ++y) {
                for (int x = std::max(0, left); x < std::min(width, left + inkColumns * scale); ++x)
                    coverageOut[static_cast<std::size_t>(y) * width + x] = RowMarkedRasterizer::coverageOfRow(y);
            }
            penX += glyph.advanceInPixels * static_cast<float>(scale);
        }
        return true;
    }
};

// The two letters every anchor of the run below names.
int resolveTwoLetterRun(const Ubiquitty::Cell* cell, std::span<std::uint32_t> codePoints, void*)
{
    if (!cell || codePoints.size() < 2 || cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        return 0;
    codePoints[0] = 'a';
    codePoints[1] = 'b';
    return 2;
}

} // namespace

namespace {

// The run "ab" at c=2:s=3 on a three-row grid, rendered by the portable arm
// through `rasterizer`; the pixels.
std::vector<std::uint8_t> renderTripledRun(RowMarkedProportionalRasterizer& rasterizer, int& width, int& height)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.scale = 3;
    spec.runWidthCells = 2;
    const unsigned id = table.intern(spec);
    constexpr int rows = 3;
    constexpr int columns = 8;
    constexpr int claim = 6;
    std::vector<Cell> grid(rows * columns, defaultBlankCell);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            setCell(grid, columns, row, column, 0u, 0, 15, 0);
        for (int offset = 0; offset < claim; ++offset) {
            Cell& cell = grid[static_cast<std::size_t>(row) * columns + offset];
            cell.codePoint = offset == 0 ? 'a' : wideContinuationSentinelCodePoint;
            Pen pen = cell.pen;
            pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(row));
            cellSetPen(cell, pen);
        }
        grid[static_cast<std::size_t>(row) * columns].setClusterID(1);
    }
    const std::uint8_t rowFlags[rows] = { static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed), static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed),
        static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed) };
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.rowFlags = rowFlags;
    input.clusterResolver = &resolveTwoLetterRun;
    input.subpixelGlyphs = true;
    SoftwareRenderer renderer(renderCellWidth, renderCellHeight, rasterizer);
    std::vector<std::uint8_t> pixels;
    width = 0;
    height = 0;
    CHECK(renderer.render(input, pixels, width, height));
    return pixels;
}

} // namespace

// A proportional run at OSC 66 scale 3 is drawn on the portable arm from
// masters three times its size, one picture over the block's three rows: down
// the first letter's ink every row of the screen shows the master row after
// the one above it, across both seams between the bands. Stretched from the
// glyph at its own size, each row of the glyph would stand three times.
TEST(testAScaledProportionalRunIsCutFromItsMasterAcrossItsBandsOnThePortableArm)
{
    RowMarkedProportionalRasterizer rasterizer;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> pixels = renderTripledRun(rasterizer, width, height);
    REQUIRE(height == 3 * renderCellHeight);

    CHECK_MSG(!rasterizer.mastersAsked.empty() && std::ranges::all_of(rasterizer.mastersAsked, [](int scale) { return scale == 3; }),
        "every window is cut from a master three times the glyph's size");
    // The first letter's ink is nine columns from the run's pen at 0.
    const int x = 4;
    int repeated = 0;
    for (int y = 0; y + 1 < height; ++y)
        repeated += inkOf(at(pixels, width, x, y)) == inkOf(at(pixels, width, x, y + 1)) ? 1 : 0;
    const std::string seams = std::to_string(repeated) + " row(s) of the master shown twice down the block";
    CHECK_MSG(repeated == 0, seams.c_str());
    // The second letter's ink begins at its tripled pen, twelve pixels along.
    CHECK_MSG(inkOf(at(pixels, width, 10, 8)) == inkOf(at(pixels, width, width - 1, 8)), "between the letters is background");
    CHECK_MSG(inkOf(at(pixels, width, 13, 8)) == inkOf(at(pixels, width, x, 8)), "the second letter's ink at its tripled pen");
}

namespace {

// The run "O" at proportional claim `cells` and OSC 66 scale `scale`, from the
// home cell, rendered by the portable arm on the bitmap tier at `unifontScale`;
// the pixels and the box its ink fills.
struct BitmapRunPicture {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int inkLeft = 0;
    int inkTop = 0;
    int inkRight = -1;
    int inkBottom = -1;
};

int resolveOneLetterRun(const Ubiquitty::Cell* cell, std::span<std::uint32_t> codePoints, void*)
{
    if (!cell || codePoints.empty() || cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        return 0;
    codePoints[0] = 'O';
    return 1;
}

BitmapRunPicture renderBitmapRun(int unifontScale, int scale, int cells)
{
    TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    Ubiquitty::TextSizeSpec spec {};
    spec.isProportional = 1;
    spec.scale = static_cast<std::uint8_t>(scale);
    spec.runWidthCells = static_cast<std::uint16_t>(cells);
    const unsigned id = table.intern(spec);
    const int rows = scale;
    const int columns = cells * scale;
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    std::vector<std::uint8_t> rowFlags(static_cast<std::size_t>(rows), static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            setCell(grid, columns, row, column, column == 0 ? 'O' : wideContinuationSentinelCodePoint, 0, 15, 0);
            Cell& cell = grid[static_cast<std::size_t>(row) * columns + column];
            Pen pen = cell.pen;
            pen.attributes.add(column == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
            pen.extendedAttributes.setTextSizeSpecID(id);
            penSetTextBand(&pen, static_cast<std::uint8_t>(row));
            cellSetPen(cell, pen);
        }
        grid[static_cast<std::size_t>(row) * columns].setClusterID(1);
    }
    BuildInputs input;
    input.cells = grid.data();
    input.rows = rows;
    input.columns = columns;
    input.palette = &palette;
    input.textSizeTable = &table;
    input.rowFlags = rowFlags.data();
    input.clusterResolver = &resolveOneLetterRun;
    input.subpixelGlyphs = true;
    UnifontRasterizer rasterizer(unifontScale);
    const CellMetrics metrics = rasterizer.cellMetrics();
    SoftwareRenderer renderer(static_cast<int>(metrics.cellWidth), static_cast<int>(metrics.cellHeight), rasterizer);
    BitmapRunPicture picture;
    const bool isRendered = renderer.render(input, picture.pixels, picture.width, picture.height);
    CHECK(isRendered);
    if (!isRendered)
        return picture;
    const Pixel background = at(picture.pixels, picture.width, picture.width - 1, picture.height - 1);
    picture.inkLeft = picture.width;
    picture.inkTop = picture.height;
    for (int y = 0; y < picture.height; ++y) {
        for (int x = 0; x < picture.width; ++x) {
            if (inkOf(at(picture.pixels, picture.width, x, y)) == inkOf(background))
                continue;
            picture.inkLeft = std::min(picture.inkLeft, x);
            picture.inkTop = std::min(picture.inkTop, y);
            picture.inkRight = std::max(picture.inkRight, x);
            picture.inkBottom = std::max(picture.inkBottom, y);
        }
    }
    return picture;
}

} // namespace

// The bitmap tier draws a run at scale 2 as it draws its text at twice its
// size: the 24 px strike, as a grid of scale two draws it, not the 12 px
// strike's pixels doubled. The letter of a scale-2 run on a scale-1 grid is
// pixel for pixel the letter of a plain run on a scale-2 grid. Stretched from
// its own size, every stroke would be two pixels of the smaller strike.
TEST(testABitmapRunAtScaleTwoIsTheBitmapTiersOwnTextAtTwiceItsSize)
{
    const BitmapRunPicture scaled = renderBitmapRun(1, 2, 2);
    const BitmapRunPicture reference = renderBitmapRun(2, 1, 2);
    REQUIRE(scaled.inkRight >= scaled.inkLeft && reference.inkRight >= reference.inkLeft);
    const int width = scaled.inkRight - scaled.inkLeft + 1;
    const int height = scaled.inkBottom - scaled.inkTop + 1;
    CHECK_EQ(width, reference.inkRight - reference.inkLeft + 1);
    CHECK_EQ(height, reference.inkBottom - reference.inkTop + 1);
    int differing = 0;
    int doubledBlocks = 0;
    int blocks = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int mine = inkOf(at(scaled.pixels, scaled.width, scaled.inkLeft + x, scaled.inkTop + y));
            const int theirs = inkOf(at(reference.pixels, reference.width, reference.inkLeft + x, reference.inkTop + y));
            differing += mine == theirs ? 0 : 1;
        }
    }
    // A picture doubled from the small strike repeats every pixel in a 2 by 2
    // block aligned on its own pen; the strike at the whole size does not.
    for (int y = 0; y + 1 < height; y += 2) {
        for (int x = 0; x + 1 < width; x += 2) {
            const auto ink = [&](int xOffset, int yOffset) { return inkOf(at(scaled.pixels, scaled.width, scaled.inkLeft + x + xOffset, scaled.inkTop + y + yOffset)); };
            ++blocks;
            doubledBlocks += ink(0, 0) == ink(1, 0) && ink(0, 0) == ink(0, 1) && ink(0, 0) == ink(1, 1) ? 1 : 0;
        }
    }
    const std::string differences = std::to_string(differing) + " pixel(s) of the letter differ from the scale-2 grid's";
    CHECK_MSG(differing == 0, differences.c_str());
    CHECK_MSG(doubledBlocks < blocks, "the letter is not the small strike's pixels doubled");
}

// A run whose letters' ink overlaps is drawn as a strip, and at a scale the
// strip is drawn as the per-glyph route draws its windows: from a master three
// times the size, every master row shown once down the block, across the seams
// between the bands. Stretched from a strip one row tall, each of its rows
// would stand three times.
TEST(testAScaledProportionalStripIsCutFromItsMasterAcrossItsBandsOnThePortableArm)
{
    RowMarkedProportionalRasterizer rasterizer;
    rasterizer.inkColumns = 6;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> pixels = renderTripledRun(rasterizer, width, height);
    REQUIRE(height == 3 * renderCellHeight);
    CHECK_MSG(!rasterizer.mastersAsked.empty() && std::ranges::all_of(rasterizer.mastersAsked, [](int scale) { return scale == 3; }),
        "the strip is cut from a master three times the run's size");
    CHECK_MSG(rasterizer.largestMasterRun == 2, "the master is the run's strip, both letters in one");
    int repeated = 0;
    for (int y = 0; y + 1 < height; ++y)
        repeated += inkOf(at(pixels, width, 4, y)) == inkOf(at(pixels, width, 4, y + 1)) ? 1 : 0;
    const std::string seams = std::to_string(repeated) + " row(s) of the master shown twice down the block";
    CHECK_MSG(repeated == 0, seams.c_str());
}
