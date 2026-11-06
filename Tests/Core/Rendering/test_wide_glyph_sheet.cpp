// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_wide_glyph_sheet — a two-cell glyph must be reachable from the texture
// the backend actually binds.
//
// A cell quad samples one texture. `SoftwareRenderer` keeps two atlas pages —
// mono for one-cell glyphs, wide for two-cell ones — with independent slot
// geometry, so their normalized coordinates are not comparable: the same UV
// names different pixels on each page.
//
// So a backend that binds the mono page and bakes a wide glyph's UV into a cell
// quad draws whatever happens to sit at that coordinate on the other page. It is
// not a blank or a fallback box, which is why it survives a smoke test: it is a
// real glyph, sharply drawn, and the wrong one. Only a reader who knows
// what the text should say can see it.
//
// The builder already has the correct route — a two-cell glyph leaves the cell
// for its own stream when the caller offers one. This file pins the OBLIGATION
// that creates: offering no stream while binding only the mono page is the
// combination that silently produces wrong glyphs, and a backend has no way to
// discover it from inside its own frame.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"

#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"
#include "StrictAtlas.h"

#include <array>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

constexpr int rows = 1;
constexpr int columns = 8;
// U+65E5 (日) — two cells wide, and present in the bundled Unifont, so the
// rasterizer produces real coverage rather than a miss that would make every
// comparison below trivially equal.
constexpr std::uint32_t wideCodePoint = 0x65E5;

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    std::vector<GlyphInstance> wideOut { static_cast<std::size_t>(rows) * columns };
    BuildInputs in;
    InstanceBuffers out;
    SoftwareRenderer renderer { 8, 16 };

    Scene()
    {
        palette.seedDefaults();
        Cell& cell = cells[0];
        cell.codePoint = wideCodePoint;
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::Wide);
        cellSetPen(cell, pen);

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
    }

    InstanceCounts build() { return buildCellInstances(in, UbiquittyTest::glyphBackendFor(renderer), out); }
};

bool sameUV(const CellInstance& instance, const AtlasGlyph& glyph)
{
    return instance.atlasTopLeft.x == glyph.minUV.x && instance.atlasTopLeft.y == glyph.minUV.y
        && instance.atlasBottomRight.x == glyph.maxUV.x && instance.atlasBottomRight.y == glyph.maxUV.y;
}

// U+FE0F. Presentation selectors are the shortest cluster there is: one mark on
// a base, carried inline, and on a base of two columns it is also the shortest
// way to make a cell both wide and clustered.
constexpr std::uint32_t variationSelector16 = 0xFE0Fu;

// Lit texels inside the region a UV pair names, on the page given.
//
// The page is the argument because it is the whole question. A slot is written
// by whichever fetch path rasterized the glyph, and sampled from whichever page
// the compositor picks; when those disagree the UV is well-formed, in range, and
// addresses texels nobody wrote. Coverage is zero and every count agrees.
int inkUnderUV(const SoftwareRenderer::AtlasView& page, Vec2 minUV, Vec2 maxUV)
{
    if (page.pixels == nullptr || page.width <= 0 || page.height <= 0)
        return 0;
    const int left = static_cast<int>(minUV.x * static_cast<float>(page.width));
    const int right = static_cast<int>(maxUV.x * static_cast<float>(page.width));
    const int top = static_cast<int>(minUV.y * static_cast<float>(page.height));
    const int bottom = static_cast<int>(maxUV.y * static_cast<float>(page.height));
    int lit = 0;
    for (int y = top; y < bottom && y < page.height; ++y) {
        for (int x = left; x < right && x < page.width; ++x) {
            if (page.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(page.width) + static_cast<std::size_t>(x)] != 0)
                ++lit;
        }
    }
    return lit;
}

} // namespace

// A backend that offers the wide stream gets the glyph out of the cell, which is
// the arrangement where a wide page can be bound separately and sampled with its
// own coordinates.
TEST(aWideGlyphLeavesTheCellWhenTheBackendOffersTheStream)
{
    Scene scene;
    scene.out.wideGlyphs = scene.wideOut.data();
    scene.out.wideGlyphCapacity = scene.wideOut.size();
    // The builder needs the wide slot geometry to place the glyph; without it the
    // route is off regardless of the stream, which is itself worth stating.
    scene.in.glyphWideSlotWidthInPixels = 16.0f;
    scene.in.glyphBoxWidthInPixels = 8.0f;

    const InstanceCounts counts = scene.build();
    CHECK_MSG(counts.wideGlyphs == 1, "the two-cell glyph went to the wide stream");

    // And its cell carries no baked glyph, so nothing samples the mono page for
    // it. An equal-UV cell would double-draw once the wide pass ran.
    const CellInstance& cell = scene.cellsOut[0];
    CHECK_MSG(cell.atlasTopLeft.x == cell.atlasBottomRight.x
            && cell.atlasTopLeft.y == cell.atlasBottomRight.y,
        "the cell quad is marked glyphless, so the wide pass is the only draw");
}

// The obligation. A backend offering no wide stream keeps the glyph on the cell,
// and the cell quad can only sample the page that backend bound. Today that UV
// comes from the wide page, so a backend binding mono draws the wrong glyph.
//
// This asserts the two pages really are incomparable — if they ever became one
// page with one geometry, the hazard would be gone and this case should be
// deleted rather than adjusted.
TEST(aCellBakedWideGlyphDoesNotAddressTheMonoPage)
{
    Scene scene;
    scene.out.wideGlyphs = nullptr; // the web GPU path, today

    const InstanceCounts counts = scene.build();
    CHECK_MSG(counts.wideGlyphs == 0, "no wide stream, so nothing left the cell");

    const CellInstance& cell = scene.cellsOut[0];
    CHECK_MSG(cell.atlasTopLeft.x != cell.atlasBottomRight.x, "the cell carries a baked glyph");

    const AtlasGlyph wide = scene.renderer.wideGlyph(wideCodePoint, FontFace::Regular);
    CHECK_MSG(wide.present, "the wide page rasterized it");
    CHECK_MSG(sameUV(cell, wide), "and the baked UV is the WIDE page's coordinate");

    // Both pages happen to be 256x512, so the hazard is not a size mismatch —
    // it is slot geometry. A wide slot is two cells across, so slot N covers
    // different pixels on each page and the same normalized coordinate names
    // different glyphs. A check that compared atlas dimensions would find them
    // equal and conclude the pages were interchangeable.
    const SoftwareRenderer::AtlasView mono = scene.renderer.monoAtlas();
    CHECK_MSG(mono.width > 0 && mono.height > 0, "there is a mono page to bind");
    const AtlasGlyph monoSame = scene.renderer.monoGlyph(wideCodePoint, FontFace::Regular, 0u, 0u);
    CHECK_MSG(!sameUV(cell, monoSame),
        "the wide UV is NOT where this glyph lives on the mono page — a backend binding mono draws the wrong pixels");
}

// A wide cell carrying a cluster takes its UV from the wide page for the same
// reason a wide cell without one does. The cluster fetch is tested before the
// wide fetch — a cluster is the more specific answer — so it is the arm that has
// to know which page a wide cell's coordinate is read against.
TEST(aWideClusterAddressesTheWidePageNotTheMonoOne)
{
    Scene scene;
    scene.out.wideGlyphs = nullptr; // the single-sheet arrangement, where the UV rides the cell

    Cell& cell = scene.cells[0];
    // A real combining mark rather than a variation selector: a selector adds no
    // coverage, so a raster taken from either page would look alike and the
    // comparison below would hold for the wrong reason.
    cell.combining[0] = 0x0301;
    cell.setCombiningLength(1);

    scene.build();
    const CellInstance& instance = scene.cellsOut[0];
    CHECK_MSG(instance.atlasTopLeft.x != instance.atlasBottomRight.x, "the cell carries a baked glyph");

    const std::array<std::uint32_t, 2> cluster { wideCodePoint, 0x0301u };
    const AtlasGlyph wide = scene.renderer.wideClusterGlyph(cluster, FontFace::Regular, 0u);
    CHECK_MSG(wide.present, "the wide page rasterized the cluster");
    CHECK_MSG(sameUV(instance, wide), "and the baked UV is the WIDE page's coordinate");

    const AtlasGlyph mono = scene.renderer.clusterGlyph(cluster.data(), static_cast<int>(cluster.size()), FontFace::Regular, 0u, 0u);
    CHECK_MSG(mono.present, "the mono page can hold the same cluster");
    CHECK_MSG(!sameUV(instance, mono),
        "the wide UV is NOT where this cluster lives on the mono page — a consumer keyed on cell width samples the wrong slot");
}

UBIQUITTY_TEST_MAIN()

// The conjunction, as a matrix, because only a conjunction case can distinguish
// a conjunction defect.
//
// Width and clustering are independent inputs to two independent decisions: the
// builder chooses a fetch path, and the compositor chooses a page. Each arm
// alone routes consistently, so a suite that varies one factor at a time
// confirms both decisions and never puts them in the same cell. Three of the
// four entries below pass against a compositor that picks its page from the
// cell's width alone; the fourth is where the two disagree, and it is the only
// arrangement a reader ever sees as a missing glyph.
//
// The assertion is texels under the UV rather than a count or a presence flag.
// A slot allocated by one path and sampled through another yields a UV that is
// well-formed and in range, an AtlasGlyph that reports present, and an instance
// the streams count exactly once — every question except "did anyone write
// these texels" answers correctly.
TEST(aWideClusteredCellSamplesThePageItsSlotWasWrittenInto)
{
    struct Arm {
        const char* name;
        bool isWide;
        bool isClustered;
    };
    // Both single-factor arms are here to be passed. They carry no defect and
    // are not padding: if one of them ever fails, the conjunction arm below is
    // reporting something other than the disagreement it exists for.
    constexpr Arm arms[] = {
        { "narrow, no cluster", false, false },
        { "narrow, clustered", false, true },
        { "wide, no cluster", true, false },
        { "wide, clustered", true, true },
    };

    for (const Arm& arm : arms) {
        Scene scene;
        Cell& cell = scene.cells[0];
        cell.codePoint = arm.isWide ? wideCodePoint : 0x0041u; // 'A' is one column
        if (arm.isClustered) {
            cell.combining[0] = variationSelector16;
            cell.setCombiningLength(1);
        }
        Pen pen = cellPen(cell);
        if (arm.isWide)
            pen.attributes.add(VT::Attribute::Wide);
        else
            pen.attributes.remove(VT::Attribute::Wide);
        cellSetPen(cell, pen);

        // No wide stream, so the glyph stays baked on the cell quad and the
        // compositor's page choice is the only thing that decides what it
        // samples. That is the arrangement this file exists for.
        scene.out.wideGlyphs = nullptr;
        scene.build();

        const CellInstance& instance = scene.cellsOut[0];
        const std::string baked = std::string(arm.name) + ": the cell carries a baked glyph";
        CHECK_MSG(instance.atlasTopLeft.x != instance.atlasBottomRight.x, baked.c_str());

        // The page the compositor binds for this cell, by the rule it uses.
        const SoftwareRenderer::AtlasView page
            = arm.isWide ? scene.renderer.wideAtlas() : scene.renderer.monoAtlas();
        const int lit = inkUnderUV(page, instance.atlasTopLeft, instance.atlasBottomRight);
        // Naming the arm is what makes a failure here readable: three of the
        // four are expected to pass, so a bare message leaves the reader unable
        // to tell the conjunction from a broken single factor.
        const std::string sampled = std::string(arm.name)
            + ": the page this cell samples has texels under its UV";
        CHECK_MSG(lit > 0, sampled.c_str());

        // And the slot is as wide as the cell it draws. This is the same defect
        // stated before any page is chosen, which is what makes it the portable
        // half: a backend carrying the sheet on the stream never consults width
        // at draw time, so it samples the mono page happily and draws a one-cell
        // raster across two cells. Texels are present and the glyph is narrow —
        // a degradation the coverage check above cannot distinguish from a
        // correct draw. Absent on one backend, narrow on another, one cause.
        // The rect is atlas pixels, so its span is the width, with no
        // normalized span to scale by the page width.
        const float uvWidthInPixels = instance.atlasBottomRight.x - instance.atlasTopLeft.x;
        const float expectedWidthInPixels = arm.isWide ? 16.0f : 8.0f;
        const std::string slotWidth = std::string(arm.name)
            + ": the slot is as wide as the cell it draws";
        CHECK_MSG(uvWidthInPixels == expectedWidthInPixels, slotWidth.c_str());
    }
}
