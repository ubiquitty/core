// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// What Core does when a backend's wide raster refuses.
//
// `GlyphBackend::rasteriseWideGlyph` is one of five raster hooks and the only
// one that reports failure; the other four return void, so a backend that could
// not fill the slot has no way to say so and Core has nothing to act on. This
// is the hook where the question is answerable, and a fixture that binds it to
// `nullptr` or to a lambda returning `true` never reaches the refusal branch in
// `buildCellInstances`, which hands `wideKey` to `forgetKey`.
//
// The failure it guards is the one the counting checks cannot see, and
// `GlyphSlotCache::forgetKey`'s own header says so: a caller whose raster fails
// has taken a slot it never wrote, and every later frame is told that key is
// resident and handed the slot back. The cell then draws whatever the evicted
// glyph left in those texels, under its own colours, with every instance count
// in the frame correct. A parity run compares two arms that agree, and a
// snapshot compares a frame to a golden recorded from the same defect.
//
// So the assertion that carries this file is not that a refused cell draws
// nothing on the frame that refused. It is that the second frame asks again.

#include "Core/CellPen.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphSlotCache.h"
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
// A two-cell CJK ideograph, so `isWide` takes the branch under test.
constexpr std::uint32_t wideCodePoint = 0x4E00;

// The cache is a portable Core type and the key derivation is Core's, so what
// this file constructs is the two entries `SoftwareRenderer` leaves null —
// `bindWideAtlas` and `rasteriseWideGlyph`. Everything else is the renderer's
// published table. A binding is not a stand-in for a sheet here: the slot
// arithmetic under test is `WideAtlasBinding::atlasRectForSlot`, which is the same
// code every backend runs.
constexpr int slotCapacity = 4;
constexpr int slotBuckets = 16;
constexpr int slotColumns = 2;

// Whether the hook fills the slot it was handed. The point of the file is that
// these two produce different cache state, so they are a policy the case sets
// rather than two separate fixtures that could drift apart.
enum class RasterPolicy : std::uint8_t {
    Fills = 0,
    Refuses = 1,
};

struct HookLog {
    RasterPolicy policy = RasterPolicy::Fills;
    int calls = 0;
    int lastSlot = -1;
    std::uint32_t lastVariant = 0;
};

// File scope because `GlyphBackend::context` belongs to the renderer — taking
// it for this log would swap the context every other published entry reads, and
// the table would stop being the one production hands out.
HookLog hookLog;
GlyphSlotCache* boundCache = nullptr;

WideAtlasBinding bindWideAtlas(void*)
{
    WideAtlasBinding binding;
    binding.cache = boundCache;
    binding.columns = slotColumns;
    binding.rows = slotCapacity / slotColumns;
    binding.slotWidthInPixels = 16;
    binding.slotHeightInPixels = 16;
    binding.slotCapacity = slotCapacity;
    return binding;
}

bool rasteriseWideGlyph(void*, std::uint32_t, FontFace, std::uint32_t variant, int slot)
{
    ++hookLog.calls;
    hookLog.lastSlot = slot;
    hookLog.lastVariant = variant;
    return hookLog.policy == RasterPolicy::Fills;
}

struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    std::vector<DecorationInstance> decorationsOut { static_cast<std::size_t>(rows) * columns * 8 };
    std::vector<OverlayInstance> boxFillsOut { static_cast<std::size_t>(rows) * columns * 8 };
    BuildInputs in;
    InstanceBuffers out;
    SoftwareRenderer renderer { 8, 16 };
    GlyphSlotCache cache { slotCapacity, slotBuckets, slotColumns };

    Scene()
    {
        palette.seedDefaults();
        cells[0].codePoint = wideCodePoint;
        Pen pen = cellPen(cells[0]);
        pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cellSetPen(cells[0], pen);

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

        boundCache = &cache;
        hookLog = {};
    }

    ~Scene() { boundCache = nullptr; }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // `wideGlyph` is cleared so the cache is the only route to a wide glyph.
    // Left in place it answers every cell whether or not the cache path ran,
    // and both arms below would draw the same thing for a reason that has
    // nothing to do with the refusal.
    const CellInstance& build(int (*penGrid)(void*, std::uint32_t, FontFace) = nullptr)
    {
        GlyphBackend backend = renderer.glyphBackend();
        backend.wideGlyph = nullptr;
        backend.bindWideAtlas = ::bindWideAtlas;
        backend.rasteriseWideGlyph = ::rasteriseWideGlyph;
        backend.wideGlyphPhaseCount = penGrid;
        buildCellInstances(in, backend, out);
        return cellsOut[0];
    }
};

// A cell that got a slot carries a rect with area; one that got nothing carries
// a degenerate one. Both arms read this, so neither can pass by drawing the
// other's picture.
bool carriesAGlyph(const CellInstance& instance)
{
    return instance.atlasTopLeft.x != instance.atlasBottomRight.x
        && instance.atlasTopLeft.y != instance.atlasBottomRight.y;
}

} // namespace

// The control arm, and the one that gives the refusal arm its meaning: a hook
// that fills is asked once. Without this, "the refusal arm called the hook
// twice" says nothing — a builder that asked every frame regardless would
// produce the same count.
TEST(aFilledWideSlotIsAskedForOnceAndThenServedFromTheCache)
{
    Scene scene;
    hookLog.policy = RasterPolicy::Fills;

    CHECK_MSG(carriesAGlyph(scene.build()), "the fill succeeded, so the cell carries the slot's rect");
    CHECK_EQ(hookLog.calls, 1);
    const int filledSlot = hookLog.lastSlot;
    CHECK_MSG(filledSlot >= 0, "and the cache assigned a slot to fill");

    CHECK_MSG(carriesAGlyph(scene.build()), "the second frame draws the same glyph");
    CHECK_MSG(hookLog.calls == 1, "and does not ask again — the key is resident, which is the whole point of the cache");
}

// The branch this file exists for. A refusal must reach `forgetKey`, so the
// next frame misses and asks again.
//
// Mutating that branch — dropping its `forgetKey(wideKey)` call and keeping the
// `if` — leaves the first two assertions passing and fails the last two, which
// is the shape a mutation gate needs.
TEST(aRefusedWideRasterIsAskedForAgainRatherThanServedFromASlotNobodyWrote)
{
    Scene scene;
    hookLog.policy = RasterPolicy::Refuses;

    CHECK_MSG(!carriesAGlyph(scene.build()), "nothing was written, so the cell draws no glyph");
    CHECK_EQ(hookLog.calls, 1);
    const int refusedSlot = hookLog.lastSlot;

    CHECK_MSG(!carriesAGlyph(scene.build()), "and the next frame still draws no glyph");
    CHECK_MSG(hookLog.calls == 2, "because the key was forgotten and the frame asked again");
    CHECK_MSG(hookLog.lastSlot == refusedSlot,
        "reusing the slot it could not fill — forgetting sends it to the LRU tail, so a "
        "failing glyph costs one slot rather than one per frame");
}

// A refusal is not permanent. The same key rasterises on a later frame once the
// backend can fill it, which is the half `forgetKey` buys and a check that only
// asserted the retry would not notice going missing.
TEST(aBackendThatRecoversDrawsTheGlyphOnTheNextFrame)
{
    Scene scene;
    hookLog.policy = RasterPolicy::Refuses;
    CHECK_MSG(!carriesAGlyph(scene.build()), "the first frame is refused");

    hookLog.policy = RasterPolicy::Fills;
    CHECK_MSG(carriesAGlyph(scene.build()), "and the recovered backend fills the slot it is asked for again");
    CHECK_EQ(hookLog.calls, 2);

    CHECK_MSG(carriesAGlyph(scene.build()), "which is then resident like any other glyph");
    CHECK_EQ(hookLog.calls, 2);
}

// A wide glyph is keyed and rasterised at its foreground's tier, as a mono one
// is. The raster dilates by the ink's luminance, so a two-cell glyph faulted
// in white and then drawn in black would carry white text's heavier strokes:
// the wide page held one slot per glyph and drew every CJK character at the
// weight of the lightest ink, which is what put dark CJK text visibly bolder
// than CoreText's.
TEST(aWideGlyphIsRasterisedAtItsForegroundsTier)
{
    Scene scene;
    scene.in.hasRasterTiers = true;
    const auto paint = [&](std::uint16_t foregroundIndex) {
        Pen pen = cellPen(scene.cells[0]);
        pen.foregroundIndex = foregroundIndex;
        cellSetPen(scene.cells[0], pen);
    };

    paint(0);
    CHECK(carriesAGlyph(scene.build()));
    CHECK_EQ(hookLog.calls, 1);
    const std::uint32_t darkTier = variantTier(hookLog.lastVariant);
    const int darkSlot = hookLog.lastSlot;

    paint(15);
    CHECK(carriesAGlyph(scene.build()));
    CHECK_MSG(hookLog.calls == 2, "a light ink is a different raster, so it is asked for rather than served the dark one");
    CHECK_MSG(variantTier(hookLog.lastVariant) > darkTier, "at a heavier tier than the dark ink's");
    CHECK_MSG(hookLog.lastSlot != darkSlot, "into a slot of its own");

    paint(0);
    CHECK(carriesAGlyph(scene.build()));
    CHECK_MSG(hookLog.calls == 2, "and the dark ink's slot is still resident");
}

// On a phased frame a two-cell glyph is placed the way a mono one is: faulted
// at the phase its pen falls in and blitted texel for pixel from the snapped
// pen. A 7.8-pixel advance puts the second column's pen at 7.8, so the glyph
// there is the phase-3 raster of four drawn from pixel 7. The wide page used
// to fault every glyph at phase zero and stretch the slot onto the
// fractional cell, which is the phase-zero glyph resampled: CoreText draws the
// pen where it falls, and a resampled glyph is a blurred copy of the wrong one.
TEST(aWideGlyphIsPlacedAtItsPensPhase)
{
    Scene scene;
    constexpr float advance = 7.8f;
    constexpr float margin = 2.0f;
    constexpr float wideSlotWidth = 2.0f * 8.0f + 2.0f * margin;
    scene.in.metrics.cellWidth = advance;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 4;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = 8.0f;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = wideSlotWidth;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    // The glyph moves to the second column, whose pen is fractional.
    scene.cells[1] = scene.cells[0];
    scene.cells[0] = defaultBlankCell;

    scene.build();
    CHECK_EQ(hookLog.calls, 1);
    CHECK_MSG(variantPhase(hookLog.lastVariant) == 3u, "a pen 0.8 into its pixel is the third phase of four");
    CHECK_MSG(wide[0].originInPixels.x == 7.0f - margin, "the quad starts at the snapped pixel, backed off by the margin");
    CHECK_MSG(wide[0].sizeInPixels.x == wideSlotWidth, "and is the slot's own width: one texel to one pixel");

    // At a whole-pixel pen the same glyph is another raster.
    scene.cells[0] = scene.cells[1];
    scene.cells[1] = defaultBlankCell;
    scene.build();
    CHECK_MSG(hookLog.calls == 2, "phase zero is a different slot, so it is asked for");
    CHECK_EQ(static_cast<int>(variantPhase(hookLog.lastVariant)), 0);
    CHECK(wide[0].originInPixels.x == -margin);
}

// A rasteriser that grows two-cell glyphs draws them at a larger device size
// than the mono text beside them, and CoreText's pen grid is a function of the
// device size: 16-pixel text lands on thirds and the same glyph grown a tenth
// on halves. The wide page is keyed at the count the backend names for it, so
// the raster asked for is the one the rasteriser can draw. Keyed at the mono
// count, a third of the pens asked for a bucket the grown glyph has no mask
// at, and it was drawn in the neighbouring one.
TEST(aWideGlyphIsKeyedAtTheWidePagesOwnPhaseCount)
{
    Scene scene;
    constexpr float advance = 7.8f;
    constexpr float margin = 2.0f;
    scene.in.metrics.cellWidth = advance;
    scene.in.metrics.skipInk = true;
    scene.in.metrics.skipInkPadding = 1.0f;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 3;
    scene.in.widePhaseCount = 2;
    scene.in.subpixelPhaseRounding = Ubiquitty::Rendering::PhaseRounding::Floor;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = 8.0f;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = 2.0f * 8.0f + 2.0f * margin;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    // The glyph stands at the second column, whose pen is 7.8, and is
    // underlined, so the rule's tap asks for the same raster.
    scene.cells[1] = scene.cells[0];
    scene.cells[0] = defaultBlankCell;
    Pen pen = cellPen(scene.cells[1]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
    cellSetPen(scene.cells[1], pen);
    Pen continuation = cellPen(scene.cells[2]);
    continuation.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    cellSetPen(scene.cells[2], continuation);

    scene.build();
    CHECK_MSG(variantPhase(hookLog.lastVariant) == 1u, "a pen 0.8 into its pixel is the second half, not the third third");
    CHECK_MSG(hookLog.calls == 1, "and the rule under it taps that raster rather than asking for another");
    CHECK_MSG(wide[0].originInPixels.x == 7.0f - margin, "blitted from the snapped pixel all the same");

    // A backend that names no count for the wide page keys it at the frame's.
    scene.in.widePhaseCount = 0;
    scene.build();
    CHECK_MSG(variantPhase(hookLog.lastVariant) == 2u, "the third third, at the frame's own count");
}

// The pen grid is the glyph's own: one that grows less than the page's cap, or
// not at all, is drawn at a smaller device size, on a finer grid than the
// page's. Keyed at the page's count, its pen floors into a bucket CoreText does
// not draw it in. A backend that names a glyph's count has it keyed there, and
// the rule under it taps that same raster.
TEST(aWideGlyphIsKeyedAtItsOwnPenGrid)
{
    Scene scene;
    constexpr float advance = 7.8f;
    constexpr float margin = 2.0f;
    scene.in.metrics.cellWidth = advance;
    scene.in.metrics.skipInk = true;
    scene.in.metrics.skipInkPadding = 1.0f;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 3;
    scene.in.widePhaseCount = 2;
    scene.in.subpixelPhaseRounding = Ubiquitty::Rendering::PhaseRounding::Floor;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = 8.0f;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = 2.0f * 8.0f + 2.0f * margin;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    scene.cells[1] = scene.cells[0];
    scene.cells[0] = defaultBlankCell;
    Pen pen = cellPen(scene.cells[1]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
    cellSetPen(scene.cells[1], pen);
    Pen continuation = cellPen(scene.cells[2]);
    continuation.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    cellSetPen(scene.cells[2], continuation);

    scene.build([](void*, std::uint32_t codePoint, FontFace) { return codePoint == wideCodePoint ? 3 : 0; });
    CHECK_MSG(variantPhase(hookLog.lastVariant) == 2u, "a pen 0.8 into its pixel is the glyph's third third, not the page's second half");
    CHECK_MSG(hookLog.calls == 1, "and the rule under it taps that raster rather than asking for another");
    CHECK_MSG(wide[0].originInPixels.x == 7.0f - margin, "blitted from the snapped pixel all the same");

    // A count of zero leaves the glyph on the page's.
    scene.cells[1].codePoint = wideCodePoint + 1;
    scene.build([](void*, std::uint32_t codePoint, FontFace) { return codePoint == wideCodePoint ? 3 : 0; });
    CHECK_MSG(variantPhase(hookLog.lastVariant) == 1u, "a glyph the backend names no count for keeps the page's");
}

// A rule under a two-cell glyph is cut by that glyph's strokes, as a rule under
// Latin is by descenders. The wide sheet holds the ink, so on a phased frame
// the rule is emitted a column at a time and each column taps its own half of
// the glyph's slot: the shader maps a neighbour onto a tap one cell wide, and
// one rule tapping the whole two-cell slot would map every neighbour onto the
// wrong texels. A rule that tapped nothing ran straight through the ink.
TEST(aRuleUnderAWideGlyphTapsTheWideSheetColumnByColumn)
{
    Scene scene;
    constexpr float advance = 8.0f;
    constexpr float margin = 2.0f;
    scene.in.metrics.cellWidth = advance;
    scene.in.metrics.skipInk = true;
    scene.in.metrics.skipInkPadding = 1.0f;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 4;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = advance;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = 2.0f * advance + 2.0f * margin;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    Pen pen = cellPen(scene.cells[0]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
    cellSetPen(scene.cells[0], pen);
    Pen continuation = cellPen(scene.cells[1]);
    continuation.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    cellSetPen(scene.cells[1], continuation);

    scene.build();
    std::vector<const DecorationInstance*> masked;
    for (const DecorationInstance& rule : scene.decorationsOut) {
        if (rule.skipInk > 0.5f)
            masked.push_back(&rule);
    }
    REQUIRE_MSG(masked.size() >= 2, "the wide glyph's rule is cut by its ink, one column at a time");
    const auto ownSheet = [](const DecorationInstance& rule) { return static_cast<int>(rule.pad1.y + 0.5f) % 8; };
    CHECK_MSG(ownSheet(*masked[0]) == 4 && ownSheet(*masked[1]) == 4, "both columns tap the two-cell sheet");
    CHECK_MSG(masked[1]->inkTopLeft.x - masked[0]->inkTopLeft.x == advance,
        "the second column's tap is the slot one cell along from the first's");
    CHECK_MSG(masked[0]->bottomRightInPixels.x - masked[0]->topLeftInPixels.x == advance, "each rule is one cell wide");
}

// The ideographic under-rules (SGR 60 and 61) sit on the SGR 4 rule's line and
// are cut by the same ink the same way: a column at a time, each column tapping
// its own half of the glyph. Handed the first column's tap over the whole
// two-cell rule, the shader squeezed half a glyph's ink across both cells and
// cut the rule where no stroke crossed it.
TEST(anIdeographicUnderRuleTapsAWideGlyphColumnByColumn)
{
    Scene scene;
    constexpr float advance = 8.0f;
    constexpr float margin = 2.0f;
    scene.in.metrics.cellWidth = advance;
    scene.in.metrics.skipInk = true;
    scene.in.metrics.skipInkPadding = 1.0f;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 4;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = advance;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = 2.0f * advance + 2.0f * margin;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    Pen pen = cellPen(scene.cells[0]);
    penSetIdeogram(&pen, Ubiquitty::VT::ideogramUnderline);
    cellSetPen(scene.cells[0], pen);
    Pen continuation = cellPen(scene.cells[1]);
    continuation.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    cellSetPen(scene.cells[1], continuation);

    scene.build();
    std::vector<const DecorationInstance*> masked;
    for (const DecorationInstance& rule : scene.decorationsOut) {
        if (rule.skipInk > 0.5f)
            masked.push_back(&rule);
    }
    REQUIRE_MSG(masked.size() == 2, "the ideographic rule is cut one column at a time");
    CHECK_MSG(masked[0]->bottomRightInPixels.x - masked[0]->topLeftInPixels.x == advance, "each rule is one cell wide");
    CHECK_MSG(masked[1]->inkTopLeft.x - masked[0]->inkTopLeft.x == advance,
        "and taps the slot one cell along from the column before it");
}

// Character spacing moves the pen without stretching the phased raster: the
// quad keeps the atlas scale from the snapped pen, and the extra pitch stays
// empty after the glyph. A column's tap is therefore its screen span converted
// at that atlas scale, one pitch wide and one pitch apart, which is the ink the
// column stands over. Converted at the stretch the quad no longer has, the
// taps fell short of the columns they cut and missed the right half's strokes.
TEST(aWideGlyphsColumnTapIsMeasuredInSlotTexelsUnderCharacterSpacing)
{
    Scene scene;
    constexpr float advance = 8.0f;
    constexpr float pitch = 10.0f;
    constexpr float margin = 2.0f;
    scene.in.metrics.cellWidth = pitch;
    scene.in.metrics.skipInk = true;
    scene.in.metrics.skipInkPadding = 1.0f;
    scene.in.subpixelGlyphs = true;
    scene.in.subpixelPhaseCount = 4;
    scene.in.glyphReferenceAdvanceInPixels = advance;
    scene.in.glyphBoxWidthInPixels = advance;
    scene.in.glyphMarginInPixels = { margin, margin };
    scene.in.glyphWideSlotWidthInPixels = 2.0f * advance + 2.0f * margin;
    scene.in.glyphWideSlotHeightInPixels = 16.0f + 2.0f * margin;
    std::vector<GlyphInstance> wide(4);
    scene.out.wideGlyphs = wide.data();
    scene.out.wideGlyphCapacity = wide.size();
    Pen pen = cellPen(scene.cells[0]);
    pen.attributes.add(Ubiquitty::VT::Attribute::Underline);
    cellSetPen(scene.cells[0], pen);
    Pen continuation = cellPen(scene.cells[1]);
    continuation.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    cellSetPen(scene.cells[1], continuation);

    scene.build();
    std::vector<const DecorationInstance*> masked;
    for (const DecorationInstance& rule : scene.decorationsOut) {
        if (rule.skipInk > 0.5f)
            masked.push_back(&rule);
    }
    REQUIRE_MSG(masked.size() >= 2, "the rule is cut one column at a time");
    CHECK_MSG(masked[0]->bottomRightInPixels.x - masked[0]->topLeftInPixels.x == pitch, "each rule is one cell of the pitch wide on screen");
    CHECK_MSG(masked[0]->inkBottomRight.x - masked[0]->inkTopLeft.x == pitch, "and taps the same span of the unstretched slot");
    CHECK_MSG(masked[1]->inkTopLeft.x - masked[0]->inkTopLeft.x == pitch, "the second column's tap one pitch along from the first's");
    CHECK_MSG(masked[0]->inkTopLeft.x - masked[0]->glyphSlotTopLeftUV.x == margin, "the first starting a margin into the slot, where the pen is");
}

UBIQUITTY_TEST_MAIN()
