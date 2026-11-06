// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// What a null `GlyphBackend::boxGlyph` costs, measured at the instance rather
// than in pixels, so the answer is the same on every backend.
//
// The entry is not one thing. `BoxDrawing::prefersProcedural` splits its range:
// an area fill is drawn from Core's decomposition whatever the backend offers,
// and a box-drawing line is font-first — `CellInstanceBuilder` asks the backend
// and falls back only on a refusal. So a null is free for one half and
// overrides Core's own policy for the other, and only a case per half can say
// so.

#include "Core/CellPen.h"
#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"

#include <cstdint>
#include <vector>

namespace {

using namespace Ubiquitty::Rendering;
using Ubiquitty::Cell;
using Ubiquitty::cellPen;
using Ubiquitty::cellSetPen;
using Ubiquitty::defaultBlankCell;
using Ubiquitty::Pen;
using Ubiquitty::TermPalette;

constexpr int rows = 1;
constexpr int columns = 4;
// U+2551 double vertical: a box-drawing LINE, so `prefersProcedural` is false
// and the builder asks the backend first.
constexpr std::uint32_t lineCodePoint = 0x2551;
// U+2588 full block: an area FILL, so `prefersProcedural` is true and the
// backend is never asked.
constexpr std::uint32_t fillCodePoint = 0x2588;
// U+E0B0 right separator: an analytic full-cell shape, so it shares the
// procedural lane's snapped geometry rather than the font lane's bitmap.
constexpr std::uint32_t powerlineSeparatorCodePoint = 0xE0B0;

// A UV no atlas would hand back, so a cell carrying it can only have come from
// the entry under test. Distinct from both the absent glyph (min == max) and
// from any real slot rect.
constexpr float sentinelMinU = 0.125f;
constexpr float sentinelMaxU = 0.875f;

// Which table a build runs against: the renderer's own, or that table with the
// sentinel standing in for `boxGlyph`.
enum class BoxEntry : std::uint8_t {
    AsPublished = 0,
    Sentinel = 1,
};

AtlasGlyph sentinelBoxGlyph(void*, std::uint32_t, FontFace)
{
    return { { sentinelMinU, sentinelMinU }, { sentinelMaxU, sentinelMaxU }, true };
}

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    // `isBoxDrawing` is gated on the CALLER offering this stream
    // (`hasBoxSheetInWalk` in `buildCellInstances`), not on the backend filling
    // `boxGlyph`. A scene without it skips the box branch whole and both builds
    // agree for the reason that has nothing to do with the entry.
    std::vector<OverlayInstance> boxFillsOut { static_cast<std::size_t>(rows) * columns * 8 };
    BuildInputs in;
    InstanceBuffers out;
    InstanceCounts counts;
    SoftwareRenderer renderer { 8, 16 };

    explicit Scene(std::uint32_t codePoint)
    {
        palette.seedDefaults();
        cells[0].codePoint = codePoint;

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
        out.boxFills = boxFillsOut.data();
        out.boxFillCapacity = boxFillsOut.size();
    }

    // The renderer's own published table, with `boxGlyph` either overridden by
    // the sentinel or left as the renderer publishes it. Taking the published
    // table rather than building one keeps every other entry exactly as
    // production has it, so the difference between two builds is this entry and
    // nothing else.
    const CellInstance& build(BoxEntry entry)
    {
        GlyphBackend backend = renderer.glyphBackend();
        if (entry == BoxEntry::Sentinel)
            backend.boxGlyph = sentinelBoxGlyph;
        counts = buildCellInstances(in, backend, out);
        return cellsOut[0];
    }
};

bool carriesSentinel(const CellInstance& instance)
{
    return instance.atlasTopLeft.x == sentinelMinU && instance.atlasBottomRight.x == sentinelMaxU;
}

} // namespace

// The premise every case below rests on: Core's policy really does split this
// range. Asserted rather than assumed, because both cases are about which side
// of that split their code point falls, and a policy change that moved either
// would leave them passing while testing something else.
TEST(coreAsksTheBackendForALineAndNeverForAFill)
{
    CHECK_MSG(!BoxDrawing::prefersProcedural(lineCodePoint), "a box-drawing line is font-first");
    CHECK_MSG(BoxDrawing::prefersProcedural(fillCodePoint), "an area fill is Core's to draw");
}

// A null takes the cell off the backend's glyph and onto Core's decomposition,
// so the font's stroke weight and dashes are gone. That is information the cell
// carried, which is `optionalDefaultsAreLossy`'s own test.
TEST(aNullBoxGlyphChangesWhatALineCellDraws)
{
    Scene filled(lineCodePoint);
    const CellInstance withEntry = filled.build(BoxEntry::Sentinel);
    CHECK_MSG(carriesSentinel(withEntry), "the builder consulted boxGlyph for a line");

    Scene bare(lineCodePoint);
    const CellInstance withoutEntry = bare.build(BoxEntry::AsPublished);
    CHECK_MSG(!carriesSentinel(withoutEntry), "and the null took a different route");
}

// The other half, and the reason the entry cannot be classified flat: the
// builder never asks for a fill, so filling the entry changes nothing and a
// null costs nothing. A classification that called the whole entry lossy would
// be wrong here in the direction that reads as a defect.
TEST(aNullBoxGlyphChangesNothingForAFillCell)
{
    Scene filled(fillCodePoint);
    const CellInstance withEntry = filled.build(BoxEntry::Sentinel);
    CHECK_MSG(!carriesSentinel(withEntry), "the builder did not consult boxGlyph for a fill");

    Scene bare(fillCodePoint);
    const CellInstance withoutEntry = bare.build(BoxEntry::AsPublished);
    CHECK_MSG(withEntry.atlasTopLeft.x == withoutEntry.atlasTopLeft.x
            && withEntry.atlasBottomRight.x == withoutEntry.atlasBottomRight.x,
        "so the two builds agree for a fill");
}

// A Powerline separator has an exact cell-relative silhouette. Letting a font
// answer first bakes that silhouette at phase zero and then resamples it on a
// fractionally positioned cell quad, which makes the separator's apparent edge
// depend on the column. The geometry lane instead snaps the destination once
// and draws the whole silhouette in one SDF quad.
TEST(aPowerlineSeparatorBypassesTheFontForOneSnappedShapeQuad)
{
    Scene scene(powerlineSeparatorCodePoint);
    scene.in.originInPixels.x = 0.37f;
    const CellInstance& instance = scene.build(BoxEntry::Sentinel);

    CHECK_MSG(BoxDrawing::prefersProcedural(powerlineSeparatorCodePoint), "an analytic Powerline separator is Core geometry");
    CHECK_MSG(!carriesSentinel(instance), "the builder did not consult the font glyph entry");
    CHECK_MSG(scene.counts.boxFills == 1, "one separator cell emits one shape quad");
    CHECK_MSG(scene.boxFillsOut[0].shapeKind == static_cast<float>(BoxDrawing::PowerlineShapeKind::SolidTriangle),
        "the emitted quad carries the analytic triangle");
    CHECK_MSG(scene.boxFillsOut[0].topLeftInPixels.x == 0.0f
            && scene.boxFillsOut[0].bottomRightInPixels.x == 8.0f,
        "the separator quad is snapped to the cell edges independently of the fractional origin");
}

// `wideGlyph`'s documentation promises a fallback: "Null reports absent, which
// the builder answers by stretching the 1x glyph — wide cells render, slightly
// wrong, rather than blank."
//
// The wide arm is an `if`/`else if` with no `else`, so a backend offering
// neither a wide cache nor the entry leaves the glyph at its absent default and
// the cell draws its background alone. A wide cell renders BLANK, which is the
// outcome the sentence rules out — and blank is more loss than any entry
// `optionalDefaultsAreLossy` names, all of which leave some ink.
//
// Pinned because the promise is the kind a reader acts on: a backend author who
// believes it will leave the entry null and ship invisible CJK.
TEST(aWideCellWithNoWideRouteDrawsNothingRatherThanAStretchedGlyph)
{
    Scene scene(0x4E00); // CJK U+4E00, two cells wide by the width tables
    Pen pen = cellPen(scene.cells[0]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    cellSetPen(scene.cells[0], pen);

    GlyphBackend backend = scene.renderer.glyphBackend();
    CHECK_MSG(backend.wideGlyph != nullptr, "the renderer publishes the entry, so removing it is the mutation");
    backend.wideGlyph = nullptr;
    backend.bindWideAtlas = nullptr;
    backend.rasteriseWideGlyph = nullptr;

    buildCellInstances(scene.in, backend, scene.out);
    const CellInstance& instance = scene.cellsOut[0];
    CHECK_MSG(instance.atlasTopLeft.x == instance.atlasBottomRight.x
            && instance.atlasTopLeft.y == instance.atlasBottomRight.y,
        "the cell carries no glyph at all — background only, not a stretched 1x raster");
}

// The control: with the published table intact the same cell does draw, so the
// case above measures the entry's absence and not a scene that never inks.
TEST(theSameWideCellDrawsWhenTheRouteIsPresent)
{
    Scene scene(0x4E00);
    Pen pen = cellPen(scene.cells[0]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    cellSetPen(scene.cells[0], pen);

    buildCellInstances(scene.in, scene.renderer.glyphBackend(), scene.out);
    const CellInstance& instance = scene.cellsOut[0];
    CHECK_MSG(instance.atlasBottomRight.x > instance.atlasTopLeft.x, "the published table draws it");
}

// The distinction the named refusal exists for, and the only case that can show
// it: a declined entry and a null one draw the identical cell, and Core reports
// them differently. Nothing about the picture separates a decision from an
// oversight, which is why the report is the only place the difference can live.
//
// Every other entry the predicate reads is held filled, so the reported
// difference is this entry and nothing else. Taking the published table alone
// would not do: this renderer's rasterizer carries no colour source, so
// `colorGlyph` is null and the predicate answers true whatever `boxGlyph` says.
TEST(aDeclinedBoxGlyphDrawsWhatANullDrawsAndReportsDifferently)
{
    // The refusal is stated on the table, not taken from this renderer: a
    // backend whose face draws box lines fills the entry instead, and which
    // one this renderer is says nothing about what a refusal must mean. The
    // claim is about the two ways an entry can be empty.
    Scene declined(lineCodePoint);

    auto holdingEverythingElseFilled = [](GlyphBackend table) {
        table.clusterGlyph = [](void*, std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return AtlasGlyph {}; };
        table.wideClusterGlyph = [](void*, std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return AtlasGlyph {}; };
        table.colorGlyph = [](void*, std::span<const std::uint32_t>, std::uint16_t) { return AtlasGlyph {}; };
        table.wideGlyph = [](void*, std::uint32_t, FontFace) { return AtlasGlyph {}; };
        return table;
    };

    GlyphBackend refuses = holdingEverythingElseFilled(declined.renderer.glyphBackend());
    refuses.boxGlyph = declineBoxGlyph;
    buildCellInstances(declined.in, refuses, declined.out);
    const CellInstance drawnWhenDeclined = declined.cellsOut[0];

    Scene nulled(lineCodePoint);
    GlyphBackend omits = holdingEverythingElseFilled(nulled.renderer.glyphBackend());
    omits.boxGlyph = nullptr;
    buildCellInstances(nulled.in, omits, nulled.out);
    const CellInstance drawnWhenNull = nulled.cellsOut[0];

    CHECK_MSG(drawnWhenDeclined.atlasTopLeft.x == drawnWhenNull.atlasTopLeft.x
            && drawnWhenDeclined.atlasBottomRight.x == drawnWhenNull.atlasBottomRight.x,
        "the two tables draw the identical cell");
    CHECK_MSG(!optionalDefaultsAreLossy(refuses), "a weighed refusal is not an unconsidered loss");
    CHECK_MSG(optionalDefaultsAreLossy(omits), "and a null is");
}

// The complement, and the reason the wide loss is a property of the ROUTE rather
// than of one entry: the builder tries the wide cache first and only falls to
// `wideGlyph` when that route is absent, so a backend that binds a cache never
// consults the entry and a null there costs it nothing. The Swift arm is
// exactly that shape (the Metal glyph source's `bindWideAtlas` binds, and
// leaves the entry null), and a predicate keyed on the entry alone reports a
// loss it does not have.
//
// This asserts the classification, not the drawing — the case above already
// pins what a cell with neither route draws, and building a second real wide
// cache here would test this fixture's plumbing rather than Core's rule.
TEST(aBoundWideCacheIsNotAWideLoss)
{
    Scene scene(0x4E00);
    GlyphBackend table = scene.renderer.glyphBackend();
    table.clusterGlyph = [](void*, std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return AtlasGlyph {}; };
    table.wideClusterGlyph = [](void*, std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return AtlasGlyph {}; };
    table.colorGlyph = [](void*, std::span<const std::uint32_t>, std::uint16_t) { return AtlasGlyph {}; };

    table.wideGlyph = nullptr;
    table.bindWideAtlas = nullptr;
    table.rasteriseWideGlyph = nullptr;
    CHECK_MSG(optionalDefaultsAreLossy(table), "no route to a wide glyph at all is a loss");

    table.bindWideAtlas = [](void*) { return WideAtlasBinding {}; };
    table.rasteriseWideGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, int) { return true; };
    CHECK_MSG(!optionalDefaultsAreLossy(table), "and a bound cache is the route, so the null entry is not");
}

// The limit of every question this seam can answer about itself, demonstrated
// rather than asserted: these are tests of POINTERS, and the resource behind a
// binding is resolved when the frame calls it. A backend can hold both cache
// pointers and hand back an empty binding, and then `hasWideGlyphRoute` says
// there is a route while the cell draws bare background — the same false
// negative `check-glyph-backend-claims.py` records for the colour route.
//
// Kept as a case rather than a sentence so the limit is checkable: a reader who
// takes the predicate for a guarantee that a wide cell inks is reading it
// further than it goes, and a case fails if the gap ever closes while the
// prose describing it stays.
TEST(aBoundButEmptyWideBindingPassesTheRouteTestAndStillDrawsNothing)
{
    Scene scene(0x4E00);
    Pen pen = cellPen(scene.cells[0]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    cellSetPen(scene.cells[0], pen);

    GlyphBackend table = scene.renderer.glyphBackend();
    table.wideGlyph = nullptr;
    // Both pointers set, and the binding carries no cache — which is what the
    // builder actually tests: `wide.cache` in `buildCellInstances`.
    table.bindWideAtlas = [](void*) { return WideAtlasBinding {}; };
    table.rasteriseWideGlyph = [](void*, std::uint32_t, FontFace, std::uint32_t, int) { return true; };

    CHECK_MSG(hasWideGlyphRoute(table), "the pointers are set, so the route test passes");

    buildCellInstances(scene.in, table, scene.out);
    const CellInstance& instance = scene.cellsOut[0];
    CHECK_MSG(instance.atlasTopLeft.x == instance.atlasBottomRight.x,
        "and the cell still draws nothing, because the cache the binding carries is null");
}

UBIQUITTY_TEST_MAIN()
