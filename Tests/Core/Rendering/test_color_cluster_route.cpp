// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_color_cluster_route — a colour cell hands its whole cluster to the sheet.
//
// A colour glyph is shaped from a sequence, not from a code point: a flag is two
// regional indicators, a family is people joined by ZWJ, a skin tone is a base
// plus a modifier, and an emoji presentation is a base plus U+FE0F. Passing the
// base alone yields a real glyph at a plausible UV — the wrong one, drawn
// sharply, which is the failure mode that survives every count and every
// presence flag.
//
// `Tests/StrictAtlas.h` exists because Core substitutes when a fixture declares
// no `colorGlyph`, and its substitution is the base code point alone. So a
// fixture that does not model colour cannot contradict a builder that stopped
// passing the cluster: it would be handed the base, answer about the base, and
// agree.
//
// Nothing in the tree modelled a colour sheet that inks, so this obligation had
// no oracle. The bundled font reports `present` for U+1F600 and rasterises
// blank, which means a coverage-based check over the real renderer compares two
// empty rasters and passes whatever the builder does.
//
// The fixture here records the sequence it is handed rather than drawing it,
// because the assertion is about what the builder passed and not about what a
// rasterizer made of it.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TermPalette.h"
#include "StrictAtlas.h"

#include <cstdint>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

constexpr int rows = 1;
constexpr int columns = 4;

// U+1F600 GRINNING FACE, and the presentation selector that makes a cluster of
// it. The pair is the shortest colour cluster that exists; a ZWJ family would
// assert the same property at more length.
constexpr std::uint32_t grinningFace = 0x1F600u;
constexpr std::uint32_t variationSelector16 = 0xFE0Fu;

// Models the colour sheet, and records the sequence rather than drawing it.
//
// The UV it returns is a real region so the cell reads as carrying a glyph;
// nothing here asserts about pixels, because a rasterizer that dropped the
// selector would be a second defect and this case is about the first.
struct RecordingColorAtlas : UbiquittyTest::StrictAtlas {
    std::vector<std::uint32_t> lastSequence;
    int colorCalls = 0;

    void beginFrame() { }
    MonoAtlasBinding bindMonoAtlas() { return {}; }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

    AtlasGlyph colorGlyph(std::span<const std::uint32_t> codePoints, std::uint16_t)
    {
        ++colorCalls;
        lastSequence.assign(codePoints.begin(), codePoints.end());
        AtlasGlyph glyph;
        glyph.present = true;
        glyph.minUV = { 0.0f, 0.0f };
        glyph.maxUV = { 0.25f, 0.25f };
        return glyph;
    }
};

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    std::vector<GlyphInstance> colorOut { static_cast<std::size_t>(rows) * columns };
    BuildInputs in;
    InstanceBuffers out;
    RecordingColorAtlas atlas;

    Scene()
    {
        palette.seedDefaults();
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        in.metrics.baselineFromTop = 13.0f;
        in.metrics.underlineThickness = 1.0f;

        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
        out.decorations = decorationsOut.data();
        out.decorationCapacity = decorationsOut.size();
        // A colour sheet needs both its raster provider and an output stream.
        out.colorGlyphs = colorOut.data();
        out.colorGlyphCapacity = colorOut.size();
    }

    // - Parameter marks: Combining code points to carry inline, in order.
    void placeColorCell(std::span<const std::uint32_t> marks)
    {
        Cell& cell = cells[0];
        cell.codePoint = grinningFace;
        for (std::size_t index = 0; index < marks.size(); ++index)
            cell.combining[index] = marks[index];
        cell.setCombiningLength(static_cast<std::uint8_t>(marks.size()));
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::Emoji);
        cellSetPen(cell, pen);
    }

    InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out); }
};

} // namespace

// The obligation: everything the cell carries reaches the sheet, in source
// order.
TEST(aColourCellHandsItsWholeClusterToTheSheet)
{
    Scene scene;
    const std::uint32_t marks[] = { variationSelector16 };
    scene.placeColorCell(marks);
    scene.build();

    CHECK_EQ(scene.atlas.colorCalls, 1);
    CHECK_EQ(static_cast<int>(scene.atlas.lastSequence.size()), 2);
    CHECK_EQ(static_cast<long long>(scene.atlas.lastSequence[0]), static_cast<long long>(grinningFace));
    CHECK_EQ(static_cast<long long>(scene.atlas.lastSequence[1]), static_cast<long long>(variationSelector16));
}

// The control, and the case that makes the one above falsifiable.
//
// A builder that dropped the cluster entirely would satisfy this one and fail
// the one above; a builder that passed a cluster where none exists would do the
// reverse. Asserting only the clustered case leaves "hands the sheet exactly one
// code point, always" indistinguishable from correct.
TEST(aColourCellWithNoMarksHandsTheSheetItsCodePointAlone)
{
    Scene scene;
    scene.placeColorCell({});
    scene.build();

    CHECK_EQ(scene.atlas.colorCalls, 1);
    CHECK_EQ(static_cast<int>(scene.atlas.lastSequence.size()), 1);
    CHECK_EQ(static_cast<long long>(scene.atlas.lastSequence[0]), static_cast<long long>(grinningFace));
}

TEST(aMissingColourSheetFallsBackToCoverage)
{
    Scene scene;
    const std::uint32_t marks[] = { variationSelector16 };
    scene.placeColorCell(marks);
    auto backend = UbiquittyTest::glyphBackendFor(scene.atlas);
    backend.colorGlyph = nullptr;
    backend.bindColorAtlas = [](void*) { return ColorAtlasBinding {}; };
    backend.clusterGlyph = [](void*, std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) {
        return AtlasGlyph { { 1, 1 }, { 9, 17 }, true };
    };
    const auto counts = buildCellInstances(scene.in, backend, scene.out);
    CHECK_EQ(counts.colorGlyphs, 0u);
    CHECK_EQ(scene.atlas.colorCalls, 0);
    CHECK(scene.cellsOut[0].atlasBottomRight.x > scene.cellsOut[0].atlasTopLeft.x);
}

TEST(aWideEmojiWithoutColourUsesTheWideCoverageSheet)
{
    Scene scene;
    scene.placeColorCell({});
    auto pen = cellPen(scene.cells[0]);
    pen.attributes.add(VT::Attribute::Wide);
    cellSetPen(scene.cells[0], pen);
    std::vector<GlyphInstance> wideOut(columns);
    scene.out.wideGlyphs = wideOut.data();
    scene.out.wideGlyphCapacity = wideOut.size();
    scene.in.glyphWideSlotWidthInPixels = 16;
    scene.in.glyphBoxWidthInPixels = 8;
    auto backend = UbiquittyTest::glyphBackendFor(scene.atlas);
    backend.colorGlyph = nullptr;
    backend.bindColorAtlas = [](void*) { return ColorAtlasBinding {}; };
    backend.wideGlyph = [](void*, std::uint32_t, FontFace) {
        return AtlasGlyph { { 1, 1 }, { 17, 17 }, true };
    };
    const auto counts = buildCellInstances(scene.in, backend, scene.out);
    CHECK_EQ(counts.colorGlyphs, 0u);
    CHECK_EQ(counts.wideGlyphs, 1u);
}

UBIQUITTY_TEST_MAIN()
