// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sized_band_pixels — the pixels a sized block or a DECDHL pair puts on
// screen, on every arm that draws a band from the cell box, when the box is the
// font's line rounded up.
//
// An atlas slot's cell box is the line height rounded up to whole pixels, and
// the grid's rows are the line itself. A band that draws its whole box reaches
// past its row into the next band's, and the two blend the rows they share
// twice: a hairline across the glyph at every join, past its ink. The builder
// is driven here with a backend that cuts real windows from a master whose row
// `r` holds a coverage told apart from every other row's, and the quads it
// emits are sampled at pixel centres and blended in stream order, which is what
// a GPU pass or the software renderer does at one atlas pixel to a device pixel.
// A row shown twice, skipped or blended twice reads back as the wrong value.
//
// The line is 30 rows and the cell 30 pixels, so a texel is a pixel and the
// expected row is exact. The box is 30 rows where nothing rounds and 31 where
// it does; the two must draw the same picture.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/SizedMaster.h"

#include "Core/RowMeta.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int cellWidth = 16;
constexpr int cellHeight = 30;
constexpr int marginX = 2;
constexpr int marginY = 2;
constexpr int atlasWidth = 64;
// Windows live far from the mono slots the builder's own cache hands out.
constexpr int windowsBase = 100000;

// The coverage of master row `row`: distinct for neighbouring rows, never zero
// or full, so a row blended twice reads as neither of the two it blended.
std::uint8_t coverageOfRow(int row)
{
    return static_cast<std::uint8_t>(20 + (row * 37) % 200);
}

struct RowRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(cellWidth);
        metrics.cellHeight = static_cast<float>(cellHeight);
        metrics.baselineFromTop = 24.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        fillRows(slotWidth, slotHeight, 1.0, coverageOut);
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        fillRows(slotWidth, slotHeight, master.scale.value(), coverageOut);
        return true;
    }

    // The line's rows carry ink and the margins around it, where a face's
    // overhang would live, carry none: a glyph at `scale` times the size has
    // `scale` lines of ink, `scale` margins down.
    static void fillRows(int width, int height, double scale, std::uint8_t* out)
    {
        const int first = static_cast<int>(std::lround(scale * marginY));
        const int last = static_cast<int>(std::lround(scale * (marginY + cellHeight)));
        for (int y = 0; y < height; ++y)
            std::fill_n(out + static_cast<std::size_t>(y) * static_cast<std::size_t>(width), width, y >= first && y < last ? coverageOfRow(y) : std::uint8_t { 0 });
    }
};

// Which sheet a window is cut for: a wide window is two cells across.
enum class WindowSheet {
    Mono,
    Wide,
};

// Whether a scenario also names the master row each pixel row must show.
enum class RowCheck {
    EqualToExactBox,
    ExactRows,
};

// The atlas the backend cuts into and the quads are sampled from.
struct WindowAtlas {
    int boxHeight = cellHeight;
    RowRasterizer rasterizer;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> scratch;
    std::map<std::tuple<int, int, int, int, int, int, int, int>, AtlasGlyph> windows;
    GlyphSlotCache monoCache { 16, recommendedHashBuckets(16), 4 };
    int usedRows = 0;

    int slotWidth() const { return cellWidth + 2 * marginX; }
    int slotHeight() const { return boxHeight + 2 * marginY; }
    int wideSlotWidth() const { return 2 * cellWidth + 2 * marginX; }
    int doubleSlotWidth() const { return 2 * slotWidth(); }
    int doubleSlotHeight() const { return 2 * slotHeight(); }

    // Reserves `height` rows and returns the first, in atlas pixels.
    int reserve(int height)
    {
        const int top = windowsBase + usedRows;
        usedRows += height;
        pixels.resize(static_cast<std::size_t>(usedRows) * atlasWidth, 0);
        return top;
    }

    std::uint8_t at(float u, float v) const
    {
        const int x = static_cast<int>(std::floor(u));
        const int y = static_cast<int>(std::floor(v)) - windowsBase;
        if (x < 0 || x >= atlasWidth || y < 0 || y >= usedRows)
            return 0;
        return pixels[static_cast<std::size_t>(y) * atlasWidth + static_cast<std::size_t>(x)];
    }

    // One window cut by the shared cutter, as the software renderer and the
    // Vulkan atlas cut theirs. Wide windows are cut into the wide slot and
    // return all of it; a narrow window returns its cell box.
    AtlasGlyph sizedWindow(std::uint32_t codePoint, FontFace face, SizedTile tile, WindowSheet sheet)
    {
        const bool isWide = sheet == WindowSheet::Wide;
        const auto key = std::make_tuple(isWide ? 1 : 0, tile.scale, tile.widthScale, tile.fractionNumerator, tile.fractionDenominator, tile.band, tile.tile, 0);
        if (const auto found = windows.find(key); found != windows.end())
            return found->second;
        const int width = isWide ? wideSlotWidth() : slotWidth();
        const int height = slotHeight();
        const SizedWindowCanvas canvas { width, height, marginX, marginY, static_cast<float>(cellHeight) };
        std::vector<std::uint8_t> window(static_cast<std::size_t>(width) * height);
        if (!rasteriseSizedWindow(rasterizer, codePoint, face, 0u, tile, 0u, canvas, scratch, window))
            return {};
        const int top = reserve(height);
        for (int y = 0; y < height; ++y)
            std::copy_n(window.begin() + static_cast<std::ptrdiff_t>(y) * width, width,
                pixels.begin() + static_cast<std::ptrdiff_t>(top - windowsBase + y) * atlasWidth);
        AtlasGlyph rect;
        rect.present = true;
        if (isWide) {
            rect.minUV = { 0.0f, static_cast<float>(top) };
            rect.maxUV = { static_cast<float>(width), static_cast<float>(top + height) };
        } else {
            rect.minUV = { static_cast<float>(marginX), static_cast<float>(top + marginY) };
            rect.maxUV = { static_cast<float>(marginX + cellWidth), static_cast<float>(top + marginY + boxHeight) };
        }
        windows[key] = rect;
        return rect;
    }

    // The 2x master of the DECDHL pair and DECDWL, one slot of rows told apart
    // and a region of it.
    AtlasGlyph doubleRegion(DoubleRegion region)
    {
        const int width = doubleSlotWidth();
        const int height = doubleSlotHeight();
        const auto key = std::make_tuple(2, 0, 0, 0, 0, 0, 0, 0);
        auto found = windows.find(key);
        if (found == windows.end()) {
            const int top = reserve(height);
            for (int y = 0; y < height; ++y)
                std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(top - windowsBase + y) * atlasWidth, width,
                    y >= 2 * marginY && y < 2 * (marginY + cellHeight) ? coverageOfRow(y) : std::uint8_t { 0 });
            AtlasGlyph whole;
            whole.present = true;
            whole.minUV = { 0.0f, static_cast<float>(top) };
            whole.maxUV = { static_cast<float>(width), static_cast<float>(top + height) };
            found = windows.emplace(key, whole).first;
        }
        AtlasGlyph rect = found->second;
        const float middle = 0.5f * (rect.minUV.y + rect.maxUV.y);
        if (region == DoubleRegion::Top)
            rect.maxUV.y = middle;
        else if (region == DoubleRegion::Bottom)
            rect.minUV.y = middle;
        return rect;
    }

    GlyphBackend backend()
    {
        GlyphBackend b;
        b.context = this;
        b.beginFrame = [](void* context) { static_cast<WindowAtlas*>(context)->monoCache.beginFrame(); };
        b.bindMonoAtlas = [](void* context) {
            WindowAtlas& self = *static_cast<WindowAtlas*>(context);
            MonoAtlasBinding binding;
            binding.cache = &self.monoCache;
            binding.geometry = { /*columns=*/4, self.slotWidth(), self.slotHeight(), cellWidth, { marginX, marginY } };
            binding.slotCapacity = 16;
            binding.weight = 0u;
            return binding;
        };
        b.rasteriseMonoGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, int) { return true; };
        // The cell's own glyph is blank: only the sized windows carry ink.
        b.monoGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, std::uint32_t) {
            return AtlasGlyph { { 0.1f, 0.1f }, { 0.2f, 0.2f }, true };
        };
        b.wideGlyph = [](void*, std::uint32_t, FontFace) { return AtlasGlyph { { 0.3f, 0.3f }, { 0.4f, 0.4f }, true }; };
        b.sizedGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t, std::uint32_t, GlyphPass, SizedTile tile) {
            return static_cast<WindowAtlas*>(context)->sizedWindow(codePoint, face, tile, WindowSheet::Mono);
        };
        b.sizedWideGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t, SizedTile tile) {
            return static_cast<WindowAtlas*>(context)->sizedWindow(codePoint, face, tile, WindowSheet::Wide);
        };
        b.doubleGlyph = [](void* context, std::uint32_t, FontFace, std::uint32_t, DoubleRegion region) {
            return static_cast<WindowAtlas*>(context)->doubleRegion(region);
        };
        return b;
    }
};

// One quad of any stream, in device pixels with the atlas rows it shows.
struct Quad {
    float x;
    float y;
    float width;
    float height;
    Vec2 topLeft;
    Vec2 bottomRight;
};

// What a build drew: its quads in draw order, and how many each stream held.
struct Picture {
    std::vector<Quad> quads;
    InstanceCounts counts;
    const WindowAtlas* atlas = nullptr;

    // The ink at a pixel, blended in draw order, and how many quads covered it.
    int ink(int px, int py, int* quadsCovering = nullptr) const
    {
        float result = 0.0f;
        int covering = 0;
        const float cx = static_cast<float>(px) + 0.5f;
        const float cy = static_cast<float>(py) + 0.5f;
        for (const Quad& quad : quads) {
            if (cx < quad.x || cx >= quad.x + quad.width || cy < quad.y || cy >= quad.y + quad.height)
                continue;
            ++covering;
            const float u = quad.topLeft.x + (cx - quad.x) / quad.width * (quad.bottomRight.x - quad.topLeft.x);
            const float v = quad.topLeft.y + (cy - quad.y) / quad.height * (quad.bottomRight.y - quad.topLeft.y);
            const float coverage = static_cast<float>(atlas->at(u, v)) / 255.0f;
            result += (255.0f - result) * coverage;
        }
        if (quadsCovering)
            *quadsCovering = covering;
        return static_cast<int>(std::lround(result));
    }
};

enum class Stream {
    Subpixel,
    CellBaked,
};

// The block a scenario draws.
struct Scenario {
    const char* name;
    TextSizeSpec spec;
    std::uint32_t codePoint = 'X';
    Stream stream = Stream::Subpixel;
    // How the rows are laid out: a DECDHL pair has no spec.
    std::uint8_t lineAttribute = lineAttributeNormal;
    bool isDoubledPair = false;
    bool hasWideSheet = false;
    bool hasDoubleSheet = false;
    // The column to read back, and the rows the block covers.
    int sampleX = cellWidth + 8;
    int rows = 3;
    // Which stream must have carried the ink, so a passing test is not a block
    // drawn on an arm the scenario meant to leave.
    enum class Carrier {
        Glyphs,
        WideGlyphs,
        DoubleGlyphs,
        Cells
    } carrier = Carrier::Glyphs;
};

Picture build(const Scenario& scenario, WindowAtlas& atlas)
{
    TermPalette palette;
    palette.seedDefaults();
    TextSizeTable table;
    const bool isSized = !scenario.isDoubledPair;
    const unsigned id = isSized ? table.intern(scenario.spec) : 0u;
    const bool isWideGlyph = scenario.codePoint > 0x3000;
    const int claim = isSized ? scenario.spec.scale * (isWideGlyph ? 2 : std::max<int>(scenario.spec.widthCells, 1)) : (scenario.lineAttribute == lineAttributeNormal ? 1 : 2);
    const int columns = 2 * claim + 4;
    const int rows = scenario.rows;
    Cell blank {};
    blank.pen.foregroundIndex = colorDefault;
    blank.pen.backgroundIndex = colorDefault;
    blank.pen.underlineIndex = colorDefault;
    std::vector<Cell> grid(static_cast<std::size_t>(columns) * rows, blank);
    std::vector<std::uint8_t> lineAttributes(static_cast<std::size_t>(rows), scenario.lineAttribute);
    for (int row = 0; row < rows; ++row) {
        for (int offset = 0; offset < (isSized ? claim : 1); ++offset) {
            Cell& cell = grid[static_cast<std::size_t>(row) * columns + 1 + offset];
            Pen pen = cell.pen;
            if (isSized) {
                pen.attributes.add(offset == 0 ? VT::Attribute::Wide : VT::Attribute::WideContinuation);
                pen.extendedAttributes.setTextSizeSpecID(id);
                penSetTextBand(&pen, static_cast<std::uint8_t>(row));
            }
            cellSetPen(cell, pen);
            if (offset == 0)
                cell.codePoint = scenario.codePoint;
        }
    }
    if (scenario.isDoubledPair) {
        lineAttributes[0] = lineAttributeDoubleHeightTop;
        lineAttributes[1] = lineAttributeDoubleHeightBottom;
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.textSizeTable = &table;
    in.lineAttributes = lineAttributes.data();
    in.subpixelGlyphs = scenario.stream == Stream::Subpixel;
    in.metrics.cellWidth = static_cast<float>(cellWidth);
    in.metrics.cellHeight = static_cast<float>(cellHeight);
    in.metrics.baselineFromTop = 24.0f;
    in.glyphReferenceAdvanceInPixels = static_cast<float>(cellWidth);
    in.glyphReferenceLineHeightInPixels = static_cast<float>(cellHeight);
    in.glyphBoxWidthInPixels = static_cast<float>(cellWidth);
    in.glyphMarginInPixels = { static_cast<float>(marginX), static_cast<float>(marginY) };
    in.glyphSlotWidthInPixels = static_cast<float>(atlas.slotWidth());
    in.glyphSlotHeightInPixels = static_cast<float>(atlas.slotHeight());
    if (scenario.hasWideSheet) {
        in.glyphWideSlotWidthInPixels = static_cast<float>(atlas.wideSlotWidth());
        in.glyphWideSlotHeightInPixels = static_cast<float>(atlas.slotHeight());
    }
    if (scenario.hasDoubleSheet) {
        in.glyphDoubleSlotWidthInPixels = static_cast<float>(atlas.doubleSlotWidth());
        in.glyphDoubleSlotHeightInPixels = static_cast<float>(atlas.doubleSlotHeight());
    }
    in.viewportSizeInPixels = { static_cast<float>(columns * cellWidth), static_cast<float>(rows * cellHeight) };

    const std::size_t capacity = static_cast<std::size_t>(columns) * rows * 4 + 64;
    std::vector<CellInstance> cells(capacity);
    std::vector<DecorationInstance> decorations(capacity * 4);
    std::vector<GlyphInstance> glyphs(capacity);
    std::vector<GlyphInstance> wideGlyphs(capacity);
    std::vector<GlyphInstance> doubleGlyphs(capacity);
    InstanceBuffers buffers { cells.data(), cells.size(), decorations.data(), decorations.size() };
    buffers.glyphs = glyphs.data();
    buffers.glyphCapacity = glyphs.size();
    if (scenario.hasWideSheet) {
        buffers.wideGlyphs = wideGlyphs.data();
        buffers.wideGlyphCapacity = wideGlyphs.size();
    }
    if (scenario.hasDoubleSheet) {
        buffers.doubleGlyphs = doubleGlyphs.data();
        buffers.doubleGlyphCapacity = doubleGlyphs.size();
    }
    Picture picture;
    picture.atlas = &atlas;
    const GlyphBackend backend = atlas.backend();
    picture.counts = buildCellInstances(in, backend, buffers);

    for (std::size_t i = 0; i < picture.counts.cells && i < cells.size(); ++i) {
        const CellInstance& cell = cells[i];
        if (!(cell.atlasBottomRight.x > cell.atlasTopLeft.x))
            continue;
        picture.quads.push_back({ cell.gridPosition.x * cellWidth, cell.gridPosition.y * cellHeight,
            cell.widthCells * cellWidth, static_cast<float>(cellHeight), cell.atlasTopLeft, cell.atlasBottomRight });
    }
    const auto addGlyphs = [&](const std::vector<GlyphInstance>& stream, std::size_t count) {
        for (std::size_t i = 0; i < count && i < stream.size(); ++i) {
            const GlyphInstance& g = stream[i];
            picture.quads.push_back({ g.originInPixels.x, g.originInPixels.y, g.sizeInPixels.x, g.sizeInPixels.y, g.atlasTopLeft, g.atlasBottomRight });
        }
    };
    addGlyphs(glyphs, picture.counts.glyphs);
    addGlyphs(wideGlyphs, picture.counts.wideGlyphs);
    addGlyphs(doubleGlyphs, picture.counts.doubleGlyphs);
    return picture;
}

// Whether the stream the scenario names drew anything.
bool carriedByItsStream(const Scenario& scenario, const Picture& picture)
{
    switch (scenario.carrier) {
    case Scenario::Carrier::Glyphs:
        return picture.counts.glyphs > 0;
    case Scenario::Carrier::WideGlyphs:
        return picture.counts.wideGlyphs > 0;
    case Scenario::Carrier::DoubleGlyphs:
        return picture.counts.doubleGlyphs > 0;
    case Scenario::Carrier::Cells:
        return std::ranges::any_of(picture.quads, [](const Quad& quad) { return quad.width > 0.0f; }) && picture.counts.glyphs == 0;
    }
    return false;
}

// The block drawn on a box of the line and on a box a row taller.
struct Pair {
    Picture exact;
    Picture rounded;
    WindowAtlas exactAtlas;
    WindowAtlas roundedAtlas;
};

void checkScenario(const Scenario& scenario, RowCheck rowCheck)
{
    auto pair = std::make_unique<Pair>();
    pair->exactAtlas.boxHeight = cellHeight;
    pair->roundedAtlas.boxHeight = cellHeight + 1;
    pair->exact = build(scenario, pair->exactAtlas);
    pair->rounded = build(scenario, pair->roundedAtlas);
    const std::string arm = std::string(scenario.name) + ": ";

    CHECK_MSG(carriedByItsStream(scenario, pair->exact), (arm + "the scenario's stream draws the block on an exact box").c_str());
    CHECK_MSG(carriedByItsStream(scenario, pair->rounded), (arm + "the scenario's stream draws the block on a rounded box").c_str());

    const int columns = static_cast<int>(pair->exact.quads.empty() ? 0 : 64);
    int mismatched = 0;
    int firstRow = -1;
    int firstWanted = 0;
    int firstGot = 0;
    int inked = 0;
    for (int y = 0; y < scenario.rows * cellHeight; ++y) {
        for (int x = 0; x < columns; ++x) {
            const int wanted = pair->exact.ink(x, y);
            const int got = pair->rounded.ink(x, y);
            inked += wanted != 0;
            if (wanted == got)
                continue;
            ++mismatched;
            if (firstRow < 0) {
                firstRow = y;
                firstWanted = wanted;
                firstGot = got;
            }
        }
    }
    CHECK_MSG(inked > 0, (arm + "the block draws ink").c_str());
    const std::string differs = arm + std::to_string(mismatched) + " pixel(s) differ between a box of the line and a box a row taller, the first at row "
        + std::to_string(firstRow) + " (" + std::to_string(firstWanted) + " against " + std::to_string(firstGot) + ")";
    CHECK_MSG(mismatched == 0, differs.c_str());

    if (rowCheck == RowCheck::EqualToExactBox)
        return;
    // The block is `scale` times the glyph, whose cell box begins a margin in:
    // pixel row y of the block shows the master's row y, `scale` margins down.
    const int scale = scenario.isDoubledPair ? 2 : scenario.spec.scale;
    int wrongRows = 0;
    int firstCovering = 0;
    int firstWrong = -1;
    for (int y = 0; y < scenario.rows * cellHeight; ++y) {
        int covering = 0;
        const int got = pair->rounded.ink(scenario.sampleX, y, &covering);
        if (got == coverageOfRow(scale * marginY + y) && covering == 1)
            continue;
        ++wrongRows;
        if (firstWrong < 0) {
            firstWrong = y;
            firstGot = got;
            firstWanted = coverageOfRow(scale * marginY + y);
            firstCovering = covering;
        }
    }
    const std::string rows = arm + std::to_string(wrongRows) + " of " + std::to_string(scenario.rows * cellHeight)
        + " rows are not the master's row once, the first at row " + std::to_string(firstWrong) + " (" + std::to_string(firstGot)
        + " from " + std::to_string(firstCovering) + " quad(s), wanting " + std::to_string(firstWanted) + ")";
    CHECK_MSG(wrongRows == 0, rows.c_str());
}

} // namespace

// Sharp windows on the subpixel stream: one quad a window, drawn at the block's
// scale from a slot whose box is a row taller than the line.
TEST(testSizedBandsOfTheSubpixelStreamShowEachMasterRowOnce)
{
    checkScenario({ .name = "subpixel s=3", .spec = TextSizeSpec { /*scale=*/3, /*widthCells=*/1 } }, RowCheck::ExactRows);
}

// The same windows on the cell-baked path: one cell instance a band.
TEST(testSizedBandsOfTheCellPathShowEachMasterRowOnce)
{
    checkScenario({ .name = "cell-baked s=3", .spec = TextSizeSpec { /*scale=*/3, /*widthCells=*/1 }, .stream = Stream::CellBaked, .carrier = Scenario::Carrier::Cells }, RowCheck::ExactRows);
}

// A two-cell glyph drawn from the wide sheet, whose slot is two cells across.
TEST(testSizedBandsOfTheWideSheetShowEachMasterRowOnce)
{
    checkScenario({ .name = "wide sheet s=3", .spec = TextSizeSpec { /*scale=*/3, /*widthCells=*/0 }, .codePoint = 0x6771, .hasWideSheet = true, .carrier = Scenario::Carrier::WideGlyphs }, RowCheck::ExactRows);
}

// A sized block on a DECDWL row is drawn from the doubled sheet's windows.
TEST(testSizedBandsOnADoubledRowShowEachMasterRowOnce)
{
    checkScenario({ .name = "DECDWL s=3", .spec = TextSizeSpec { /*scale=*/3, /*widthCells=*/1 }, .lineAttribute = lineAttributeDoubleWidth, .hasDoubleSheet = true, .sampleX = 3 * cellWidth + 8 }, RowCheck::ExactRows);
}

// A DECDHL pair is the 2x master cut at the line: the top row ends where the
// bottom row begins, and the master's rows below the line are the bottom row's
// overhang. Cut at the box's midline, the top half drew a row the bottom half
// drew again.
TEST(testADoubleHeightPairShowsEachMasterRowOnce)
{
    checkScenario({ .name = "DECDHL pair", .spec = TextSizeSpec {}, .lineAttribute = lineAttributeDoubleHeightTop, .isDoubledPair = true, .hasDoubleSheet = true, .sampleX = 2 * cellWidth + 16, .rows = 2, .carrier = Scenario::Carrier::DoubleGlyphs },
        RowCheck::ExactRows);
}

// A fraction draws its smaller area from windows at the fraction's own size.
TEST(testFractionalBandsShowEachMasterRowOnce)
{
    TextSizeSpec spec { /*scale=*/3, /*widthCells=*/1 };
    spec.fractionNumerator = 1;
    spec.fractionDenominator = 2;
    checkScenario({ .name = "fraction subpixel s=3 n=1 d=2", .spec = spec }, RowCheck::EqualToExactBox);
    checkScenario({ .name = "fraction cell-baked s=3 n=1 d=2", .spec = spec, .stream = Stream::CellBaked }, RowCheck::EqualToExactBox);
    spec.widthCells = 0;
    checkScenario({ .name = "fraction wide s=3 n=1 d=2", .spec = spec, .codePoint = 0x6771, .hasWideSheet = true, .carrier = Scenario::Carrier::WideGlyphs }, RowCheck::EqualToExactBox);
}

UBIQUITTY_TEST_MAIN()
