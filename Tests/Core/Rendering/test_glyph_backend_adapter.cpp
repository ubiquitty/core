// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_glyph_backend_adapter — the fixture adapter installs what the fixture declares.
//
// `UbiquittyTest::glyphBackendFor` detects the two optional entries by calling
// them inside a `requires` clause. A probe naming a signature the fixture does
// not have takes the false branch: the entry is installed as null, Core
// substitutes, and every case still passes. Silent absence reading as a
// decision is the failure `StrictAtlas.h` is written against, and its own
// detection had it.
//
// `SoftwareRenderer` spells both entries as a pointer and a count where the
// probes passed a `std::span`, so neither matched. Every builder case driven by
// the real renderer ran with both entries null and exercised only Core's
// substitution — the base code point alone for a cluster, foreground coverage
// for an emoji. The hooks were reachable and never reached.
//
// A green suite cannot report that, which is why this file exists: the suite
// passes on the substitution and passes identically once the entry is
// installed. So the assertion has to be about the adapter rather than about any
// case that routes through it, and "a fixture declaring an entry gets it
// installed" is decidable here.

#include "Harness.h"

#include "Core/Rendering/BoxDrawingRasterizer.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/ProportionalGlyphSheet.h"
#include "Core/Rendering/SizedMaster.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/TermPalette.h"
#include "StrictAtlas.h"

#include <cstdint>
#include <span>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

struct NativePhaseRasterizer : GlyphRasterizer {
    std::vector<double> scalarPhases;
    std::vector<double> clusterPhases;
    std::vector<ClusterRasterBounds> clusterBounds;
    std::vector<int> clusterCanvasWidths;
    CellMetrics cellMetrics() const override { return { .cellWidth = 8, .cellHeight = 16 }; }
    int glyphPhaseCount() const override { return 4; }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int width, int height, std::uint8_t* out) override
    {
        std::fill_n(out, width * height, std::uint8_t { 10 });
        return true;
    }
    bool rasterizeGlyphAtPhase(std::uint32_t, FontFace, std::uint32_t, double phase, int width, int height, std::uint8_t* out) override
    {
        scalarPhases.push_back(phase);
        std::fill_n(out, width * height, static_cast<std::uint8_t>(10 + phase * 100));
        return true;
    }
    bool rasterizeClusterAtPhase(const std::uint32_t*, int, FontFace, std::uint32_t, double phase, int width, int height, std::uint8_t* out) override
    {
        clusterPhases.push_back(phase);
        std::fill_n(out, width * height, static_cast<std::uint8_t>(20 + phase * 100));
        return true;
    }
    bool rasterizeClusterInCell(const std::uint32_t* codePoints, int count, FontFace face, std::uint32_t weight, double phase, ClusterRasterBounds bounds, int width, int height, std::uint8_t* out) override
    {
        clusterBounds.push_back(bounds);
        clusterCanvasWidths.push_back(width);
        return GlyphRasterizer::rasterizeClusterInCell(codePoints, count, face, weight, phase, bounds, width, height, out);
    }
};

TEST(softwareClusterRastersReceiveCellWidthsInsteadOfAtlasPadding)
{
    struct PaddedRasterizer : NativePhaseRasterizer {
        GlyphOverhangInputs glyphOverhang() const override
        {
            return { .advanceWidth = 8, .ascent = 12, .descent = 4, .lineHeight = 16, .primaryBboxX = -4, .primaryBboxWidth = 20, .primaryBboxHeight = 16 };
        }
    } rasterizer;
    SoftwareRenderer renderer(rasterizer);
    const std::array<std::uint32_t, 2> cluster { 0x0915, 0x093F };
    CHECK(renderer.clusterGlyph(cluster.data(), 2, FontFace::Regular, 0, 1).present);
    CHECK(renderer.wideClusterGlyph(cluster, FontFace::Regular, 0).present);
    CHECK_EQ(rasterizer.clusterBounds.size(), 2u);
    CHECK(rasterizer.clusterBounds[0].left > 0.25);
    CHECK_EQ(rasterizer.clusterBounds[0].width, 8);
    CHECK(rasterizer.clusterCanvasWidths[0] > 8);
    CHECK_EQ(rasterizer.clusterBounds[1].left, 0);
    CHECK_EQ(rasterizer.clusterBounds[1].width, 16);
    CHECK_EQ(rasterizer.clusterCanvasWidths[1], 16);
}

TEST(softwareAtlasMasksUseTheirNativePhaseAndCacheEachRasterOnce)
{
    NativePhaseRasterizer rasterizer;
    SoftwareRenderer renderer(rasterizer);
    const std::array<std::uint32_t, 2> cluster { 'a', 0x301 };
    for (int repetition = 0; repetition < 2; ++repetition) {
        for (std::uint32_t phase = 0; phase < 4; ++phase) {
            const AtlasGlyph scalar = renderer.monoGlyph('I', FontFace::Regular, 0, phase);
            const AtlasGlyph marks = renderer.clusterGlyph(cluster.data(), 2, FontFace::Regular, 0, phase);
            const auto atlas = renderer.monoAtlas();
            const auto pixel = [&](const AtlasGlyph& glyph) {
                return atlas.pixels[static_cast<int>(glyph.minUV.y) * atlas.width + static_cast<int>(glyph.minUV.x)];
            };
            CHECK(scalar.present && marks.present);
            CHECK_EQ(pixel(scalar), static_cast<std::uint8_t>(10 + phase * 25));
            CHECK_EQ(pixel(marks), static_cast<std::uint8_t>(20 + phase * 25));
        }
    }
    CHECK(rasterizer.scalarPhases == std::vector<double>({ 0, 0.25, 0.5, 0.75 }));
    CHECK(rasterizer.clusterPhases == std::vector<double>({ 0, 0.25, 0.5, 0.75 }));
}

TEST(builderUsesTheProvidersRasterGridInsteadOfStaleCapturedPhaseMetadata)
{
    NativePhaseRasterizer rasterizer;
    SoftwareRenderer renderer(rasterizer);
    const GlyphBackend backend = renderer.glyphBackend();
    const MonoAtlasBinding binding = backend.bindMonoAtlas(backend.context);
    CHECK_EQ(binding.phaseCount, 4);
    Cell cell {};
    cell.codePoint = 'I';
    TermPalette palette {};
    BuildInputs inputs;
    inputs.cells = &cell;
    inputs.palette = &palette;
    inputs.columns = inputs.rows = 1;
    inputs.metrics = rasterizer.cellMetrics();
    inputs.originInPixels.x = 0.375f;
    inputs.subpixelGlyphs = true;
    inputs.subpixelPhaseCount = 8;
    inputs.glyphSlotWidthInPixels = binding.geometry.slotWidthInPixels;
    inputs.glyphSlotHeightInPixels = binding.geometry.slotHeightInPixels;
    inputs.glyphReferenceAdvanceInPixels = 8;
    inputs.glyphReferenceLineHeightInPixels = 16;
    std::array<CellInstance, 1> cells;
    std::array<GlyphInstance, 1> glyphs;
    InstanceBuffers out;
    out.cells = cells.data();
    out.cellCapacity = cells.size();
    out.glyphs = glyphs.data();
    out.glyphCapacity = glyphs.size();
    const auto counts = buildCellInstances(inputs, backend, out);
    CHECK_EQ(counts.glyphs, 1u);
    CHECK(rasterizer.scalarPhases == std::vector<double>({ 0.5 }));
}

TEST(softwareBuilderFaultPassesTheRasterVariantIntoTheNativeMask)
{
    NativePhaseRasterizer rasterizer;
    SoftwareRenderer renderer(rasterizer);
    const GlyphBackend backend = renderer.glyphBackend();
    const MonoAtlasBinding binding = backend.bindMonoAtlas(backend.context);
    bool isFresh = false;
    const int slot = binding.cache->slotForKey(0x1234u, &isFresh);
    CHECK(slot >= 0 && isFresh);
    CHECK(backend.rasteriseMonoGlyph(backend.context, 'W', FontFace::Regular, 3, slot));
    CHECK(rasterizer.scalarPhases == std::vector<double>({ 0.75 }));
    const GlyphUV uv = binding.geometry.atlasRectForSlot(slot, binding.slotCapacity);
    const auto atlas = renderer.monoAtlas();
    CHECK_EQ(atlas.pixels[static_cast<int>(uv.minV) * atlas.width + static_cast<int>(uv.minU)], std::uint8_t { 85 });
}

TEST(sizedWindowPensUseTheSameNativePhaseGridAsOrdinaryGlyphs)
{
    SizedTile tile { 2, 0, 0 };
    const SizedMaster master = selectSizedMaster(tile, {}, 0, 0);
    const SizedWindowCanvas canvas { 12, 20, 2, 2, 8, 16 };
    for (std::uint32_t phase = 0; phase < 4; ++phase) {
        const auto quarter = planSizedWindow(master, tile, phase, canvas, 4);
        const auto eighth = planSizedWindow(master, tile, phase * 2, canvas);
        CHECK_EQ(quarter.sourceX, eighth.sourceX);
        CHECK_NEAR(quarter.master.shiftX, eighth.master.shiftX, 1e-9);
        CHECK_NEAR(quarter.blockShiftX, eighth.blockShiftX, 1e-9);
    }
}

struct PhaseProportionalRasterizer final : NativePhaseRasterizer {
    std::vector<float> offsets;
    int monoPhases = 4;
    int proportionalPhases = 4;
    int glyphPhaseCount() const override { return monoPhases; }
    int proportionalGlyphPhaseCount() const override { return proportionalPhases; }
    bool hasProportionalFace() const override { return true; }
    bool rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace, int, int, std::span<std::uint8_t> coverage) override
    {
        offsets.push_back(glyphs.front().offsetXInPixels);
        std::ranges::fill(coverage, std::uint8_t { 255 });
        return true;
    }
    bool rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, const SizedMaster&, int width, int height, std::span<std::uint8_t> coverage) override
    {
        return rasteriseProportionalRun(glyphs, face, width, height, coverage);
    }
};

TEST(proportionalMasksAndSizedMasksDecodeThePublishedNativePhaseGrid)
{
    PhaseProportionalRasterizer rasterizer;
    ProportionalGlyphSheet sheet(8, 16, 1, 1, &rasterizer, 256);
    CHECK(sheet.isUsable());
    CHECK_EQ(sheet.binding().phaseCount, 4);
    std::array<std::uint8_t, 12 * 20> coverage {};
    CHECK(ProportionalGlyphSheet::rasteriseGlyphIntoSlot(rasterizer, 12, FontFace::Regular, 3, 2, 12, 20, coverage));
    std::vector<std::uint8_t> master;
    SizedTile tile { 2, 0, 0 };
    CHECK(ProportionalGlyphSheet::rasteriseSizedGlyphIntoSlot(rasterizer, 12, FontFace::Regular, 3, tile, 2, 12, 20, master, coverage));
    CHECK_EQ(rasterizer.offsets.size(), 2u);
    CHECK_NEAR(rasterizer.offsets[0], 2.75f, 1e-6);
    CHECK_NEAR(rasterizer.offsets[1], 4.75f, 1e-6);
}

TEST(aSeparateProportionalFaceDoesNotInheritTheGridFacesPhaseCount)
{
    PhaseProportionalRasterizer rasterizer;
    rasterizer.monoPhases = 1;
    rasterizer.proportionalPhases = 8;
    BoxDrawingRasterizer wrapped(rasterizer);
    CHECK_EQ(wrapped.glyphPhaseCount(), 1);
    CHECK_EQ(wrapped.proportionalGlyphPhaseCount(), 8);
    ProportionalGlyphSheet sheet(8, 16, 1, 1, &wrapped, 256);
    CHECK_EQ(sheet.binding().phaseCount, 8);
    std::array<std::uint8_t, 12 * 20> coverage {};
    CHECK(ProportionalGlyphSheet::rasteriseGlyphIntoSlot(wrapped, 12, FontFace::Regular, 7, 2, 12, 20, coverage));
    std::vector<std::uint8_t> master;
    CHECK(ProportionalGlyphSheet::rasteriseSizedGlyphIntoSlot(wrapped, 12, FontFace::Regular, 7,
        SizedTile { 2, 0, 0 }, 2, 12, 20, master, coverage));
    CHECK_EQ(rasterizer.offsets.size(), 2u);
    CHECK_NEAR(rasterizer.offsets[0], 2.875f, 1e-6);
    CHECK_NEAR(rasterizer.offsets[1], 4.875f, 1e-6);
}

TEST(theBitmapTiersProportionalStrikeRetainsItsFractionalMasks)
{
    for (int scale : { 1, 2, 3 }) {
        UnifontRasterizer bitmap(scale);
        BoxDrawingRasterizer wrapped(bitmap);
        CHECK_EQ(wrapped.glyphPhaseCount(), 1);
        CHECK_EQ(wrapped.proportionalGlyphPhaseCount(), 8);
        const auto metrics = bitmap.cellMetrics();
        ProportionalGlyphSheet sheet(static_cast<int>(metrics.cellWidth), static_cast<int>(metrics.cellHeight), 2, 0, &wrapped, 256);
        CHECK_EQ(sheet.binding().phaseCount, 8);
        const int width = sheet.slotWidthInPixels();
        const int height = sheet.slotHeightInPixels();
        std::vector<std::uint8_t> actual(static_cast<std::size_t>(width) * height);
        std::vector<std::uint8_t> reference(actual.size());
        std::vector<std::uint8_t> wholePixel(actual.size());
        CHECK(ProportionalGlyphSheet::rasteriseGlyphIntoSlot(wrapped, 'W', FontFace::Regular, 7, 2, width, height, actual));
        ProportionalShapedGlyph glyph {};
        glyph.glyph = 'W';
        glyph.offsetXInPixels = 2.875f;
        CHECK(bitmap.rasteriseProportionalRun(std::span(&glyph, 1), FontFace::Regular, width, height, reference));
        CHECK(actual == reference);
        glyph.offsetXInPixels = 2;
        CHECK(bitmap.rasteriseProportionalRun(std::span(&glyph, 1), FontFace::Regular, width, height, wholePixel));
        CHECK(actual != wholePixel);
    }
}

TEST(boxWrapperForwardsTextPhasesAndKeepsProceduralPiecesOnTheCell)
{
    NativePhaseRasterizer native;
    BoxDrawingRasterizer wrapped(native);
    std::array<std::uint8_t, 8 * 16> coverage {};
    CHECK_EQ(wrapped.glyphPhaseCount(), 4);
    CHECK(wrapped.rasterizeGlyphAtPhase('I', FontFace::Regular, 0, 0.5, 8, 16, coverage.data()));
    const auto calls = native.scalarPhases.size();
    CHECK(wrapped.rasterizeGlyphAtPhase(0x2588, FontFace::Regular, 0, 0.5, 8, 16, coverage.data()));
    CHECK_EQ(native.scalarPhases.size(), calls);
    CHECK(native.scalarPhases == std::vector<double>({ 0.5 }));
}

// The seven entries every fixture owes, so each arm below differs only in the
// two optional ones.
#define UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES                                                  \
    void beginFrame() { }                                                                      \
    MonoAtlasBinding bindMonoAtlas() { return {}; }                                            \
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }      \
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; } \
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }                               \
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

// Declares both the way a fixture does, so the span-shaped probe is the one
// that has to match.
struct SpanShapedAtlas : UbiquittyTest::StrictAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES

    AtlasGlyph clusterGlyph(std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph colorGlyph(std::span<const std::uint32_t>, std::uint16_t) { return {}; }
};

// Declares neither, so what it carries is `StrictAtlas`'s aborting pair. The
// adapter has to install those: an installed abort is what turns reaching an
// unmodelled path into a failure, where a null entry hands the question to
// Core's substitution and gets a plausible answer back.
struct InheritingAtlas : UbiquittyTest::StrictAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES
};

// Declares neither and inherits nothing, so both entries stay null.
struct PlainAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES
};

} // namespace

// The renderer the builder cases actually drive. It publishes its own table, so
// the adapter has to hand that back rather than build a second one: the
// renderer gates its colour entries on whether the rasterizer has a colour
// source, and a table reconstructed from its members cannot carry that decision.
TEST(theAdapterReturnsSoftwareRenderersOwnTable)
{
    SoftwareRenderer renderer { 8, 16 };
    const GlyphBackend adapted = UbiquittyTest::glyphBackendFor(renderer);
    const GlyphBackend published = renderer.glyphBackend();

    CHECK_MSG(adapted.clusterGlyph == published.clusterGlyph,
        "the cluster entry is the renderer's own, so a builder case shapes clusters "
        "the way production does instead of falling back to the base code point");
    CHECK_MSG(adapted.colorGlyph == published.colorGlyph,
        "the colour entry is the renderer's own, including its absence: this "
        "rasterizer has no colour source, and installing a hook that answers "
        "absent every time would report a complete backend over that loss");
    CHECK_MSG(adapted.wideClusterGlyph == published.wideClusterGlyph, "and the wide-cluster entry");

    // The property those three serve, stated once. A backend claiming no loss
    // while dropping marks is the failure the whole seam is arranged against.
    CHECK_MSG(Rendering::optionalDefaultsAreLossy(adapted)
            == Rendering::optionalDefaultsAreLossy(published),
        "the adapter reports the same loss the renderer does");
}

// The other spelling, so the assertion above is about detection and not about
// one renderer.
TEST(theAdapterInstallsWhatASpanShapedFixtureDeclares)
{
    SpanShapedAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph != nullptr, "a span-shaped clusterGlyph is installed");
    CHECK_MSG(backend.colorGlyph != nullptr, "a span-shaped colorGlyph is installed");
}

// The aborting pair is an entry like any other and has to arrive installed, or
// `StrictAtlas` refuses nothing.
TEST(aFixtureThatModelsNeitherGetsTheAbortingPair)
{
    InheritingAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph != nullptr,
        "StrictAtlas's aborting clusterGlyph is installed, so a cluster reaching this "
        "fixture fails instead of being answered by its base code point");
    CHECK_MSG(backend.colorGlyph != nullptr,
        "StrictAtlas's aborting colorGlyph is installed, so an emoji reaching this "
        "fixture fails instead of being answered by foreground coverage");
}

// And the arm that makes every assertion above falsifiable. An adapter
// installing an entry unconditionally satisfies all of them, and would tell a
// fixture modelling nothing that it models both.
TEST(theAdapterInstallsNothingAFixtureNeitherDeclaresNorInherits)
{
    PlainAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph == nullptr, "nothing to install, so the entry stays null");
    CHECK_MSG(backend.colorGlyph == nullptr, "nothing to install, so the entry stays null");
}

namespace {

/// Whether the stacking face below says where its slot puts the baseline.
enum class WindowDrawing {
    /// It draws cluster windows.
    Drawn,
    /// It keeps the default, and its clusters stay cut at their slot.
    Declined,
};

/// A face whose clusters stack a bar of marks 36 rows over the baseline, far
/// past the top of a cell 16 rows tall.
///
/// Its slot centres the cell, so the baseline in a slot of any height is
/// known, and a window is the same drawing moved up by its offset.
struct StackingRasterizer final : GlyphRasterizer {
    /// The baseline's row in the cell.
    static constexpr int baselineFromTop = 12;
    /// How far over the baseline the stack's top stands.
    static constexpr int stackTopAboveBaseline = 36;
    /// Whether the face draws cluster windows.
    WindowDrawing windows = WindowDrawing::Drawn;
    /// - Returns: An 8 by 16 cell.
    CellMetrics cellMetrics() const override
    {
        CellMetrics metrics;
        metrics.cellWidth = 8;
        metrics.cellHeight = 16;
        metrics.baselineFromTop = baselineFromTop;
        return metrics;
    }
    /// The baseline's row in a slot `height` rows tall.
    ///
    /// - Parameter height: The slot's height.
    /// - Returns: The row, the cell centred in the slot.
    static int baselineIn(int height) { return baselineFromTop + (height - 16) / 2; }
    /// Draws a letter body standing eight rows on the baseline, and the marks
    /// over it when `marks` is positive, clipped to the canvas.
    ///
    /// - Parameters:
    ///   - baseline: The baseline's row in the canvas.
    ///   - width: The canvas's width.
    ///   - height: The canvas's height.
    ///   - marks: How many marks ride the letter.
    ///   - out: The canvas, `width * height` bytes.
    static void draw(int baseline, int width, int height, int marks, std::uint8_t* out)
    {
        std::fill_n(out, static_cast<std::size_t>(width) * static_cast<std::size_t>(height), std::uint8_t { 0 });
        const auto fill = [&](int top, int bottom, int left, int right) {
            for (int row = std::max(top, 0); row < std::min(bottom, height); ++row)
                std::fill(out + static_cast<std::size_t>(row) * width + left, out + static_cast<std::size_t>(row) * width + right, std::uint8_t { 255 });
        };
        fill(baseline - 8, baseline, 1, 7);
        if (marks > 0)
            fill(baseline - stackTopAboveBaseline, baseline - 10, 3, 5);
    }
    /// - Returns: True, having drawn the letter body alone.
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int width, int height, std::uint8_t* out) override
    {
        draw(baselineIn(height), width, height, 0, out);
        return true;
    }
    /// - Returns: True, having drawn the letter and its marks, cut at the slot.
    bool rasterizeClusterInCell(const std::uint32_t*, int count, FontFace, std::uint32_t, double, ClusterRasterBounds, int width, int height, std::uint8_t* out) override
    {
        draw(baselineIn(height), width, height, count - 1, out);
        return true;
    }
    /// - Returns: Whether the face draws cluster windows.
    bool drawsClusterWindows() const override { return windows == WindowDrawing::Drawn; }
    /// - Returns: True, having drawn the cluster moved up by the offset.
    bool rasterizeClusterWindow(const std::uint32_t*, int count, FontFace, std::uint32_t, double, ClusterRasterBounds, int width, int height,
        int rowOffset, int windowHeight, std::uint8_t* out) override
    {
        draw(baselineIn(height) - rowOffset, width, windowHeight, count - 1, out);
        return true;
    }
};

} // namespace

// The CPU renderer draws a stack of marks whole: a cluster in the bottom row
// whose marks reach two rows up inks those rows, where a face that draws no
// windows leaves them bare, and the cluster's own cell is the same pixels
// either way.
TEST(theSoftwareRendererDrawsAStackPastItsCellThroughWindows)
{
    constexpr int rows = 4;
    constexpr int columns = 2;
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    Cell& stacked = grid[static_cast<std::size_t>(rows - 1) * columns];
    stacked.codePoint = 'Z';
    stacked.setClusterID(1);
    const auto render = [&](WindowDrawing windows, std::vector<std::uint8_t>& rgba) {
        StackingRasterizer rasterizer;
        rasterizer.windows = windows;
        SoftwareRenderer renderer(rasterizer);
        BuildInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.clusterResolver = [](const Cell*, std::span<std::uint32_t> output, void*) {
            constexpr std::array<std::uint32_t, 4> stack { 'Z', 0x301u, 0x302u, 0x303u };
            std::ranges::copy(stack, output.begin());
            return static_cast<int>(stack.size());
        };
        const GlyphBackend backend = renderer.glyphBackend();
        CHECK_EQ(backend.rasteriseClusterWindow != nullptr, windows == WindowDrawing::Drawn);
        int width = 0;
        int height = 0;
        REQUIRE(renderer.render(in, rgba, width, height));
        REQUIRE(width == columns * 8 && height == rows * 16);
    };
    std::vector<std::uint8_t> whole;
    std::vector<std::uint8_t> cut;
    render(WindowDrawing::Drawn, whole);
    render(WindowDrawing::Declined, cut);
    const auto pixel = [](const std::vector<std::uint8_t>& rgba, int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * columns * 8 + static_cast<std::size_t>(x)) * 4;
        return std::array<std::uint8_t, 3> { rgba[at], rgba[at + 1], rgba[at + 2] };
    };
    const int rowTop = (rows - 1) * 16;
    const int stackTop = rowTop + StackingRasterizer::baselineFromTop - StackingRasterizer::stackTopAboveBaseline;
    REQUIRE(stackTop < rowTop - 16);
    const auto background = pixel(cut, 7, 0);
    for (int y = stackTop; y < rowTop; ++y) {
        CHECK_MSG(pixel(whole, 3, y) != background, "the marks over the cell land on the rows above it");
        CHECK_MSG(pixel(cut, 3, y) == background, "and a face that draws no windows leaves them bare");
    }
    CHECK_MSG(pixel(whole, 3, stackTop - 1) == background, "nothing lands above the stack's own top");
    for (int y = rowTop; y < rows * 16; ++y) {
        for (int x = 0; x < 8; ++x)
            CHECK_MSG(pixel(whole, x, y) == pixel(cut, x, y), "the cluster's own cell is the same pixels either way");
    }
}

// The bitmap face draws its stacks whole on the CPU renderer too: Unifont
// stacks six marks over a Z in its own grid, and the five that stand past the
// cell's top land in the rows above it, through windows, as five separate
// marks, with no ink table bound for the bitmap face. An e with an acute, which
// sits clear of its letter, puts nothing above its cell.
TEST(theBitmapFaceDrawsItsStackedMarksPastTheCellThroughWindows)
{
    constexpr int rows = 4;
    constexpr int columns = 2;
    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, defaultBlankCell);
    Cell& stacked = grid[static_cast<std::size_t>(rows - 1) * columns];
    stacked.codePoint = 'Z';
    stacked.setClusterID(1);
    Cell& accented = grid[static_cast<std::size_t>(rows - 1) * columns + 1];
    accented.codePoint = 'e';
    accented.setClusterID(2);
    UnifontRasterizer unifont(1);
    SoftwareRenderer renderer(unifont);
    const GlyphBackend backend = renderer.glyphBackend();
    CHECK_MSG(backend.measureClusterInk != nullptr && backend.rasteriseClusterWindow != nullptr, "the bitmap face offers windows");
    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.isBitmapFace = true;
    in.clusterResolver = [](const Cell* cell, std::span<std::uint32_t> output, void*) {
        if (cell->codePoint == 'e') {
            output[0] = 'e';
            output[1] = 0x301u;
            return 2;
        }
        constexpr std::array<std::uint32_t, 7> stack { 'Z', 0x301u, 0x302u, 0x303u, 0x304u, 0x306u, 0x309u };
        std::ranges::copy(stack, output.begin());
        return static_cast<int>(stack.size());
    };
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(renderer.render(in, rgba, width, height));
    REQUIRE(width == columns * 8 && height == rows * 16);
    CHECK_MSG(renderer.bindMonoAtlas().inkBoxForSlot == nullptr, "the bitmap face still binds no ink table");
    const auto pixel = [&](int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * columns * 8 + static_cast<std::size_t>(x)) * 4;
        return std::array<std::uint8_t, 3> { rgba[at], rgba[at + 1], rgba[at + 2] };
    };
    const auto background = pixel(15, 0);
    // The runs of inked rows over a cell's columns, above the bottom row.
    const auto bandsAbove = [&](int left) {
        int bands = 0;
        int topmost = -1;
        bool wasInked = false;
        for (int y = 0; y < (rows - 1) * 16; ++y) {
            bool isInked = false;
            for (int x = left; x < left + 8; ++x)
                isInked = isInked || pixel(x, y) != background;
            if (isInked && !wasInked) {
                ++bands;
                topmost = topmost < 0 ? y : topmost;
            }
            wasInked = isInked;
        }
        return std::array<int, 2> { bands, topmost };
    };
    const std::array<int, 2> overZ = bandsAbove(0);
    CHECK_MSG(overZ[0] == 5, "five marks stand past the cell's top, each apart from the next");
    CHECK_MSG(overZ[1] == (rows - 1) * 16 - 18, "the hook on top stands eighteen rows over the cell, at its full height");
    CHECK_MSG(bandsAbove(8)[0] == 0, "an e with an acute keeps its marks in its cell");
}

UBIQUITTY_TEST_MAIN()
