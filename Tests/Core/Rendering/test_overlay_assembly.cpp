// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_overlay_assembly — the overlay stream's order and membership.
//
// The order is the contract: two translucent tints whose blend order decides
// the pixel wherever their spans overlap, so two producers that order
// trigger and hilite differently both pass any test that does not hold the
// pair. Every
// span here overlaps the same cells for exactly that reason: a case whose
// tiers touch different cells passes under any order and holds nothing.
//
// Each tier is identified in the stream by what makes it itself — the tint the
// geometry entries bake in, the input selection colour, the flash alpha — so
// the assertions read the stream the way the blend does, not the way the
// source is arranged.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/FocusedMatchBounce.h"
#include "Core/Rendering/OverlayAssembly.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Screen/MarkValues.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "StrictAtlas.h"
#include "TerminalFixture.h"

#include <cmath>
#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

// One fully-populated frame: every tier present, every span over the same
// cells, capacity ample. Grid 10x6, cell 8x16, padding origin (12, 20) — the
// origin is non-zero because a zero origin cannot distinguish content-local
// from viewport space, which is how a shipping arm drew a selection displaced
// by exactly the padding.
extern const std::uint8_t executedRowMarks[6];

OverlayAssemblyInputs populatedInputs(const TermPalette& palette, std::span<const SelectionRowSpan> overlapping)
{
    OverlayAssemblyInputs in;
    in.grid = { 10, 6 };
    in.cellSizeInPixels = { 8.0f, 16.0f };
    in.originInPixels = { 12.0f, 20.0f };
    in.viewportSizeInPixels = { 300.0f, 136.0f };
    in.deviceScale = 2.0f;
    in.bellFlashAlpha = 0.125f;
    in.isColumns132 = true; // viewport leaves > one cell past the grid: edge draws
    in.defaultForeground = palette.defaultForeground;
    in.otherSearchMatchSpans = overlapping;
    in.hiliteRegionSpans = overlapping;
    in.triggerHighlightSpans = overlapping;
    in.selectionSpans = overlapping;
    in.selectionColor = { 0.25f, 0.5f, 0.75f, 0.5f };
    in.promptMarks.rowMarks = executedRowMarks;
    in.promptMarks.fontSize = 12;
    in.promptMarks.fontAscent = 10.0f;
    in.promptMarks.fontDescent = 3.0f;
    in.cursor.row = 2;
    in.cursor.column = 3;
    in.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    in.cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
    in.cursor.mixA = 1.0f;
    in.cursor.focused = true;
    in.cellHeightPoints = 16.0f;
    in.palette = &palette;
    return in;
}

// The fixture's face is 16pt at one atlas pixel per point with a 10px ascent
// and no gap: the baseline sits at 10 of 16, so a 2px x-height with no cap
// height puts the band's midpoint at 9, one below the cell's 8. The pill
// centres there with a pixel of breathing room, three pixels in from the
// row's top and one from its bottom (``searchPillInsets``).
constexpr float xHeightBelowTheCellsMidpoint = 2.0f;

// Row 2 ran a command, so the prompt-mark tier draws its brackets — a fixture
// without marks leaves that tier empty, and a swap control over an empty tier
// passes, which is how this file's first control read "held" for a pair
// nothing held.
const std::uint8_t executedRowMarks[6] = { 0, 0, Screen::osc133MarkCmdExecuted, 0, 0, 0 };

// Two floats the same to within a pixel's worth of rounding.
bool approx(float a, float b)
{
    return std::fabs(a - b) < 1e-3f;
}

bool sameColor(const OverlayInstance& instance, float red, float green, float blue)
{
    return std::fabs(instance.color.x - red) < 1e-4f && std::fabs(instance.color.y - green) < 1e-4f
        && std::fabs(instance.color.z - blue) < 1e-4f;
}

// The stream index of the first instance a predicate accepts, or -1.
template<typename Predicate>
int indexWhere(std::span<const OverlayInstance> stream, Predicate&& predicate)
{
    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (predicate(stream[i]))
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TEST(aPromptMarkIsSolidInkOverATranslucentGround)
{
    // A mark says which rows a command ran on, and a reader has to read it
    // against whatever lies behind a translucent window. Its bracket therefore
    // carries the window's alpha no more than a glyph does: the ink is opaque
    // where it covers, and the surface keeps its own alpha everywhere else.
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    UbiquittyTest::feed(terminal, "\033]133;A\007$ ran\033]133;C\007");

    constexpr int width = 64;
    constexpr int height = 32;
    constexpr std::uint8_t groundAlpha = 153; // 0.6 of 255, the translucent body
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 16;
        rgba[i + 1] = 16;
        rgba[i + 2] = 20;
        rgba[i + 3] = groundAlpha;
    }

    TermPalette palette;
    palette.seedDefaults();
    PromptMarkParameters parameters = promptMarkParametersForPadding(palette,
        GridSize { 4, terminal.rows }, FloatSize { 8.0f, 16.0f }, FloatPoint { 12.0f, 0.0f },
        static_cast<float>(width), 16.0, FaceMetrics { 12.0f, 10.0f, 3.0f, 0.0f, 0.0f, 0.0f });
    compositePromptMarksIntoPadding(parameters, terminal, 0, rgba, IntSize { width, height });

    int opaque = 0;
    int ground = 0;
    int between = 0;
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] == 255)
            ++opaque;
        else if (rgba[i] == groundAlpha)
            ++ground;
        else
            ++between;
    }
    CHECK_MSG(opaque > 0, "the bracket's own pixels are opaque, whatever the window's alpha");
    CHECK_MSG(ground > 0, "and the surface the ink does not reach keeps the body's alpha");
    CHECK_MSG(between == 0 || opaque > 0,
        "a partly covered pixel lands between the two rather than below the ground");
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        CHECK_MSG(rgba[i] >= groundAlpha, "no pixel of a mark lowers the alpha a compositor blends against");
    }
}

TEST(theOverlayStreamDrawsItsTiersInTheReasonChainOrder)
{
    TermPalette palette;
    palette.seedDefaults();
    // One span, rows 2, columns 2..4 — the cells every tinted tier and the
    // selection share, and the cursor's cell (2,3) sits inside it.
    const SelectionRowSpan overlapping[] = { { 2, 2, 4 } };
    const OverlayAssemblyInputs in = populatedInputs(palette, overlapping);

    std::vector<OverlayInstance> stream(64);
    const int total = assembleOverlays(in, stream);
    CHECK(total > 0 && total <= static_cast<int>(stream.size()));
    const std::span<const OverlayInstance> written { stream.data(), static_cast<std::size_t>(total) };

    // The flash is the full-viewport quad at the flash alpha.
    const int flash = indexWhere(written, [&](const OverlayInstance& o) {
        return std::fabs(o.color.w - in.bellFlashAlpha) < 1e-4f && o.topLeftInPixels.x == 0.0f
            && o.bottomRightInPixels.x == in.viewportSizeInPixels.width;
    });
    // The 132 edge is the only hairline at alpha 0.30 spanning the full height.
    const int edge = indexWhere(written, [&](const OverlayInstance& o) {
        return std::fabs(o.color.w - 0.30f) < 1e-4f && o.bottomRightInPixels.y == in.viewportSizeInPixels.height
            && o.topLeftInPixels.x > 0.0f;
    });
    // The three tints carry their geometry entries' own colours; resolve each
    // by asking the entry for its quad rather than restating the constant.
    OverlayQuad searchQuad, hiliteQuad, triggerQuad;
    CHECK(otherSearchMatchQuadAtVisibleRow(2, 2, 4, { 8.0f, 16.0f }, { 12.0f, 20.0f }, searchQuad));
    CHECK(hiliteRegionQuadAtVisibleRow(2, 2, 4, { 8.0f, 16.0f }, { 12.0f, 20.0f }, hiliteQuad));
    CHECK(triggerHighlightQuadAtVisibleRow(2, 2, 4, { 8.0f, 16.0f }, { 12.0f, 20.0f }, triggerQuad));
    const int search = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, searchQuad.red, searchQuad.green, searchQuad.blue);
    });
    const int hilite = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, hiliteQuad.red, hiliteQuad.green, hiliteQuad.blue);
    });
    const int trigger = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, triggerQuad.red, triggerQuad.green, triggerQuad.blue);
    });
    const int selection = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, in.selectionColor.x, in.selectionColor.y, in.selectionColor.z);
    });
    const int cursor = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, 1.0f, 0.0f, 0.0f);
    });
    // With no x-height the pills sit on the cell's box: the dim tier's quad
    // is the plain geometry entry, and the focused ring's rect starts at the
    // row's top with no inset on it. With one, both pills stand in from the
    // row by Core's insets at both ends, and the ring carries the insets and
    // the ground for the strips it vacates — three packed bytes in the alpha
    // the ring never blends by.
    {
        CHECK(search >= 0 && std::fabs(written[static_cast<std::size_t>(search)].topLeftInPixels.y - searchQuad.minY) < 1e-4f);
        OverlayAssemblyInputs focused = populatedInputs(palette, overlapping);
        focused.searchMatchSpans = overlapping;
        focused.searchMatchHighlight = { 1.0f, 0.82f, 0.18f, 1.0f };
        std::vector<OverlayInstance> plainStream(64);
        const int plainTotal = assembleOverlays(focused, plainStream);
        const std::span<const OverlayInstance> plain { plainStream.data(), static_cast<std::size_t>(plainTotal) };
        const int plainRing = indexWhere(plain, [](const OverlayInstance& o) { return o.shapeKind == focusedPillRingShapeKind; });
        CHECK(plainRing >= 0);
        const float rowTop = focused.originInPixels.y + 2.0f * focused.cellSizeInPixels.height;
        CHECK(plainRing >= 0 && std::fabs(plain[static_cast<std::size_t>(plainRing)].plA.y - rowTop) < 1e-4f);
        CHECK(plainRing >= 0 && plain[static_cast<std::size_t>(plainRing)].strokeWidth == 0.0f);
        CHECK(plainRing >= 0 && plain[static_cast<std::size_t>(plainRing)].shadeDensity == 0.0f);

        focused.promptMarks.xHeight = xHeightBelowTheCellsMidpoint;
        const Ubiquitty::Rendering::SearchPillInsets insets = Ubiquitty::Rendering::searchPillInsets(focused.cellSizeInPixels.height,
            focused.cellHeightPoints, focused.promptMarks.atlasScale > 0.0f ? static_cast<double>(focused.promptMarks.atlasScale) : 1.0,
            focused.promptMarks.fontAscent, focused.promptMarks.lineGapTopInPixels, focused.promptMarks.xHeight, focused.promptMarks.capHeight);
        // Both ends move, so both strips have to be repainted.
        CHECK(insets.top > 0.0f && insets.bottom > 0.0f);
        std::vector<OverlayInstance> loweredStream(64);
        const int loweredTotal = assembleOverlays(focused, loweredStream);
        const std::span<const OverlayInstance> lowered { loweredStream.data(), static_cast<std::size_t>(loweredTotal) };
        const int loweredSearch = indexWhere(lowered, [&](const OverlayInstance& o) {
            return sameColor(o, searchQuad.red, searchQuad.green, searchQuad.blue);
        });
        CHECK(loweredSearch >= 0 && std::fabs(lowered[static_cast<std::size_t>(loweredSearch)].topLeftInPixels.y - (searchQuad.minY + insets.top)) < 1e-4f);
        CHECK(loweredSearch >= 0 && std::fabs(lowered[static_cast<std::size_t>(loweredSearch)].bottomRightInPixels.y - (searchQuad.maxY - insets.bottom)) < 1e-4f);
        const int loweredRing = indexWhere(lowered, [](const OverlayInstance& o) { return o.shapeKind == focusedPillRingShapeKind; });
        CHECK(loweredRing >= 0);
        if (loweredRing >= 0) {
            const OverlayInstance& ring = lowered[static_cast<std::size_t>(loweredRing)];
            CHECK(std::fabs(ring.plA.y - (rowTop + insets.top)) < 1e-4f);
            CHECK(std::fabs(ring.plA.w - (rowTop + focused.cellSizeInPixels.height - insets.bottom)) < 1e-4f);
            CHECK(ring.strokeWidth == insets.top);
            CHECK(ring.shadeDensity == insets.bottom);
            const RGB8 ground = { channel16To8(palette.defaultBackground.red), channel16To8(palette.defaultBackground.green), channel16To8(palette.defaultBackground.blue) };
            CHECK(ring.color.w == static_cast<float>(ground.red * 65536 + ground.green * 256 + ground.blue));
        }
    }
    // The mark's colour is the ink mixed with the ground, not the palette entry
    // itself, so it is resolved by asking the geometry for a quad — the rule
    // this test already follows for the three tints above. Matching the raw
    // palette colour here found nothing once the mix landed, and reported it as
    // a missing TIER rather than as a changed colour.
    const RGB8 markInk = resolvePaletteColor(palette, promptMarkOkColorIndex, true);
    const PaletteRGB markGround = palette.defaultBackground;
    PromptMarkParameters markParameters;
    markParameters.grid = in.grid;
    markParameters.cellSizeInPixels = in.cellSizeInPixels;
    markParameters.origin = in.originInPixels;
    markParameters.viewportWidthInPixels = in.viewportSizeInPixels.width;
    markParameters.cellHeightPoints = in.cellHeightPoints;
    markParameters.fontSize = in.promptMarks.fontSize;
    markParameters.fontAscent = in.promptMarks.fontAscent;
    markParameters.fontDescent = in.promptMarks.fontDescent;
    markParameters.okRed = markInk.red / 255.0f;
    markParameters.okGreen = markInk.green / 255.0f;
    markParameters.okBlue = markInk.blue / 255.0f;
    markParameters.groundRed = channel16To8(markGround.red) / 255.0f;
    markParameters.groundGreen = channel16To8(markGround.green) / 255.0f;
    markParameters.groundBlue = channel16To8(markGround.blue) / 255.0f;
    OverlayQuad markQuads[maxPromptMarkQuadsPerRow];
    CHECK(buildPromptMarkBrackets(markParameters, resolvePromptMarkGeometry(markParameters), 0, false, markQuads,
              static_cast<int>(sizeof(markQuads) / sizeof(markQuads[0])))
        > 0);
    const int marks = indexWhere(written, [&](const OverlayInstance& o) {
        return sameColor(o, markQuads[0].red, markQuads[0].green, markQuads[0].blue);
    });

    CHECK(flash >= 0 && edge >= 0 && search >= 0 && hilite >= 0 && trigger >= 0 && selection >= 0 && marks >= 0 && cursor >= 0);

    // The chain, pairwise, so a failure names the two tiers that swapped
    // rather than reporting one permutation as another.
    CHECK_MSG(flash < edge, "the flash washes beneath the viewport furniture");
    CHECK_MSG(edge < search, "furniture sits beneath every content annotation");
    CHECK_MSG(search < hilite, "asked-for emphasis never competes with a proposal");
    CHECK_MSG(hilite < trigger,
        "the user's own rule outranks the host's proposal — the pair the two "
        "pre-assembly arms disagreed on, each green because nothing held it");
    CHECK_MSG(trigger < selection, "what the user is dragging wins over every standing rule");
    CHECK_MSG(selection < marks, "chrome annotates over the user's gesture");
    CHECK_MSG(marks < cursor, "the cursor is chrome's topmost — the pair the two "
                              "pre-assembly arms ordered oppositely, invisible because the "
                              "tiers never overlap");
}

// Membership is the other half of the contract: a tier that draws when its
// rule says it must not is invisible to the order case above.
TEST(theOverlayStreamAppliesItsMembershipRules)
{
    TermPalette palette;
    palette.seedDefaults();
    const SelectionRowSpan overlapping[] = { { 2, 2, 4 } };

    // A dead flash and an under-a-cell 132 strip draw nothing.
    {
        OverlayAssemblyInputs in = populatedInputs(palette, overlapping);
        in.bellFlashAlpha = 0.0f;
        in.viewportSizeInPixels.width = in.originInPixels.x + in.grid.columns * in.cellSizeInPixels.width + 4.0f;
        std::vector<OverlayInstance> stream(64);
        const int total = assembleOverlays(in, stream);
        const std::span<const OverlayInstance> written { stream.data(), static_cast<std::size_t>(total) };
        CHECK(indexWhere(written, [&](const OverlayInstance& o) {
            return o.topLeftInPixels.x == 0.0f && o.topLeftInPixels.y == 0.0f
                && o.bottomRightInPixels.x == in.viewportSizeInPixels.width;
        }) == -1);
        // The edge's full predicate, not alpha alone: a tint tier shares the
        // 0.30 alpha, and an assertion that under-identifies its subject
        // reports that tier's presence as the edge's.
        CHECK(indexWhere(written, [&](const OverlayInstance& o) {
            return std::fabs(o.color.w - 0.30f) < 1e-4f && o.topLeftInPixels.y == 0.0f
                && o.bottomRightInPixels.y == in.viewportSizeInPixels.height;
        }) == -1);
    }

    // The cursor never paints on a DECSSDT status row.
    {
        OverlayAssemblyInputs in = populatedInputs(palette, overlapping);
        in.cursor.row = in.grid.rows - 1;
        in.hasStatusLineOnBottomRow = true;
        std::vector<OverlayInstance> stream(64);
        const int total = assembleOverlays(in, stream);
        const std::span<const OverlayInstance> written { stream.data(), static_cast<std::size_t>(total) };
        CHECK_MSG(indexWhere(written, [](const OverlayInstance& o) { return sameColor(o, 1.0f, 0.0f, 0.0f); }) == -1,
            "a status line owns the bottom row; the data cursor must not paint there");
    }

    // A span off the viewport contributes nothing, and the count still names
    // what would have drawn: capacity starvation counts without writing.
    {
        OverlayAssemblyInputs in = populatedInputs(palette, overlapping);
        std::vector<OverlayInstance> stream(64);
        const int total = assembleOverlays(in, stream);
        std::vector<OverlayInstance> tiny(1);
        const int starved = assembleOverlays(in, tiny);
        CHECK_MSG(starved == total, "capacity bounds what is written, never what is counted");
    }
}

// The capacity formula against the assembly's own worst case, sized by the
// formula rather than by a constant the test picked: a buffer the formula
// calls sufficient must hold every instance the assembly emits, and the
// instance that pays for an undercount is the cursor, emitted last. The
// starvation case above cannot see this — its buffers are arbitrary sizes,
// so the formula is never the thing under test.
TEST(semanticTerminalMarksPreserveErrorInk)
{
    TermPalette palette;
    palette.seedDefaults();
    auto inputs = populatedInputs(palette, {});
    const std::uint8_t marks[6] = { Screen::osc133MarkCmdExecuted,
        Screen::osc133MarkCmdExecuted | Screen::osc133MarkCmdFailed, 0, 0, 0, 0 };
    inputs.promptMarks.rowMarks = marks;
    std::vector<OverlayInstance> original(maxOverlaysForRows(inputs.grid.rows));
    const int count = assembleOverlays(inputs, original);
    inputs.promptMarks.ink = { 0.25f, 0.25f, 0.25f, -1 };
    std::vector<OverlayInstance> semantic(original.size());
    CHECK(assembleOverlays(inputs, semantic) == count);
    int vibrant = 0;
    for (int index = 0; index < count; ++index) {
        const auto& actual = semantic[index];
        if (actual.color.w < 0) {
            ++vibrant;
            CHECK(actual.color.x == 0.25f && actual.color.y == 0.25f && actual.color.z == 0.25f);
            CHECK(actual.topLeftInPixels.y < inputs.originInPixels.y + inputs.cellSizeInPixels.height);
        } else {
            const auto& expected = original[index].color;
            CHECK(actual.color.x == expected.x && actual.color.y == expected.y && actual.color.z == expected.z && actual.color.w == expected.w);
        }
    }
    CHECK(vibrant > 0);
}

// A doubled row draws its logical column c across screen cells 2c and 2c+1,
// and the spans every tint tier carries are logical, so a quad on such a row
// stands at twice the column and twice the width — the cursor's rule. At
// one cell a column the selection tint sat on the glyphs left of the ones it
// named, and the dim search pills and the trigger wash beside it the same.
TEST(aSpanTintOnADoubledRowCoversTheDoubledCells)
{
    TermPalette palette;
    palette.seedDefaults();
    const SelectionRowSpan onDoubledRows[] = { { 2, 2, 4 }, { 3, 1, 1 }, { 4, 0, 9 } };
    static const std::uint8_t attributes[6] = { lineAttributeNormal, lineAttributeNormal, lineAttributeDoubleWidth,
        lineAttributeDoubleHeightTop, lineAttributeDoubleHeightBottom, lineAttributeNormal };
    OverlayAssemblyInputs in = populatedInputs(palette, {});
    in.selectionSpans = onDoubledRows;
    in.triggerHighlightSpans = onDoubledRows;
    in.lineAttributes = attributes;
    std::vector<OverlayInstance> stream(64);
    const int total = assembleOverlays(in, stream);
    REQUIRE(total > 0 && total <= static_cast<int>(stream.size()));
    const std::span<const OverlayInstance> written { stream.data(), static_cast<std::size_t>(total) };

    const auto selectionOnRow = [&](int row) -> const OverlayInstance* {
        for (const OverlayInstance& o : written) {
            if (sameColor(o, in.selectionColor.x, in.selectionColor.y, in.selectionColor.z)
                && std::fabs(o.topLeftInPixels.y - (20.0f + 16.0f * static_cast<float>(row))) < 1e-4f)
                return &o;
        }
        return nullptr;
    };
    const OverlayInstance* wide = selectionOnRow(2);
    REQUIRE_MSG(wide, "the DECDWL row's selection is drawn");
    CHECK_MSG(std::fabs(wide->topLeftInPixels.x - (12.0f + 2.0f * 2.0f * 8.0f)) < 1e-4f, "it starts at twice the column");
    CHECK_MSG(std::fabs(wide->bottomRightInPixels.x - (12.0f + 5.0f * 2.0f * 8.0f)) < 1e-4f, "and ends at twice the column after its last");
    CHECK_MSG(std::fabs(wide->bottomRightInPixels.y - wide->topLeftInPixels.y - 16.0f) < 1e-4f, "over the whole row");
    const OverlayInstance* top = selectionOnRow(3);
    REQUIRE_MSG(top, "the DECDHL top half's selection is drawn");
    CHECK_MSG(std::fabs(top->topLeftInPixels.x - (12.0f + 1.0f * 2.0f * 8.0f)) < 1e-4f
            && std::fabs(top->bottomRightInPixels.x - (12.0f + 2.0f * 2.0f * 8.0f)) < 1e-4f,
        "a half-height row doubles across the same way");
    const OverlayInstance* bottom = selectionOnRow(4);
    REQUIRE_MSG(bottom, "the DECDHL bottom half's selection is drawn");
    CHECK_MSG(std::fabs(bottom->bottomRightInPixels.x - (12.0f + 10.0f * 8.0f)) < 1e-4f,
        "a span past the half a doubled row shows is cut at the grid's edge");
    // The trigger wash on the same rows doubles with them.
    OverlayQuad triggerQuad;
    CHECK(triggerHighlightQuadAtVisibleRow(2, 2, 4, { 8.0f, 16.0f }, { 12.0f, 20.0f }, triggerQuad));
    const auto triggerOnRow = [&](int row) -> const OverlayInstance* {
        for (const OverlayInstance& o : written) {
            if (sameColor(o, triggerQuad.red, triggerQuad.green, triggerQuad.blue)
                && std::fabs(o.topLeftInPixels.y - (20.0f + 16.0f * static_cast<float>(row))) < 1e-4f)
                return &o;
        }
        return nullptr;
    };
    const OverlayInstance* trigger = triggerOnRow(2);
    REQUIRE_MSG(trigger, "the trigger wash on the DECDWL row is drawn");
    CHECK_MSG(std::fabs(trigger->topLeftInPixels.x - (12.0f + 2.0f * 2.0f * 8.0f)) < 1e-4f, "at twice the column too");
}

TEST(theCapacityFormulaHoldsTheAssemblysOwnWorstCase)
{
    TermPalette palette;
    palette.seedDefaults();
    // Every row selected and every row marked executed, plus all three fixed
    // tiers live: flash, 132 edge, cursor.
    const SelectionRowSpan everyRow[] = { { 0, 0, 9 }, { 1, 0, 9 }, { 2, 0, 9 }, { 3, 0, 9 }, { 4, 0, 9 }, { 5, 0, 9 } };
    OverlayAssemblyInputs in = populatedInputs(palette, {});
    in.selectionSpans = everyRow;
    static const std::uint8_t allExecuted[6] = { Screen::osc133MarkCmdExecuted, Screen::osc133MarkCmdExecuted,
        Screen::osc133MarkCmdExecuted, Screen::osc133MarkCmdExecuted, Screen::osc133MarkCmdExecuted,
        Screen::osc133MarkCmdExecuted };
    in.promptMarks.rowMarks = allExecuted;

    std::vector<OverlayInstance> sized(maxOverlaysForRows(in.grid.rows));
    const int total = assembleOverlays(in, sized);
    CHECK_MSG(total <= static_cast<int>(sized.size()),
        "the formula promises a span that cannot overflow; a count past it "
        "means an instance was silently dropped, and the last tier is the "
        "cursor");
    // The cursor must appear in the written stream, not merely in the count.
    const int cursor = indexWhere({ sized.data(), static_cast<std::size_t>(total) },
        [](const OverlayInstance& o) { return sameColor(o, 1.0f, 0.0f, 0.0f); });
    CHECK_MSG(cursor >= 0, "the topmost tier survives the formula-sized buffer");
}

// A reported toolkit caret makes the bar GTK's: a square stem tagged with the
// toolkit caret's kind, its opacity carried as cairo's 8-bit mask. Other
// shapes and an unreported toolkit keep the capsule's plain quad.
TEST(aToolkitCaretTagsTheBarWithItsMask)
{
    TermPalette palette;
    palette.seedDefaults();
    OverlayAssemblyInputs in = populatedInputs(palette, {});
    in.cursor.mixA = 0.3f;
    in.toolkitCaret = { .aspectRatio = 0.04, .logicalScale = 1 };
    std::vector<OverlayInstance> stream(maxOverlaysForRows(in.grid.rows));
    stream.resize(static_cast<std::size_t>(assembleOverlays(in, stream)));
    const int caret = indexWhere(stream, [](const OverlayInstance& instance) { return instance.shapeKind == toolkitCaretShapeKind; });
    CHECK_MSG(caret >= 0, "the bar is tagged as the toolkit caret");
    if (caret >= 0) {
        const OverlayInstance& stem = stream[static_cast<std::size_t>(caret)];
        // 0.3 is 19661 in cairo's sixteen bits and 76 in the byte pixman keeps,
        // where rounding the opacity to a byte would give 77.
        CHECK(toolkitCaretMask(0.3) == 76);
        CHECK_MSG(std::lround(stem.color.w * 255.0f) == 76, "the mask travels exactly");
        CHECK_MSG(stem.cornerRadius == 0.0f && approx(stem.topLeftInPixels.x, 36.0f) && approx(stem.bottomRightInPixels.x, 37.0f),
            "one square pixel at the cell's left edge, past the origin");
        CHECK(sameColor(stem, 1.0f, 0.0f, 0.0f));
    }
    in.cursor.focused = false;
    stream.assign(maxOverlaysForRows(in.grid.rows), {});
    stream.resize(static_cast<std::size_t>(assembleOverlays(in, stream)));
    CHECK_MSG(indexWhere(stream, [](const OverlayInstance& instance) { return instance.shapeKind == toolkitCaretShapeKind; }) < 0,
        "unfocused, the bar is the hollow outline, not the caret");
    CHECK_MSG(indexWhere(stream, [](const OverlayInstance& instance) { return instance.strokeWidth > 0.0f && sameColor(instance, 1.0f, 0.0f, 0.0f); }) >= 0,
        "and the outline is in the stream");
    in.cursor.shape = Ubiquitty::Cursor::Shape::Underline;
    stream.assign(maxOverlaysForRows(in.grid.rows), {});
    stream.resize(static_cast<std::size_t>(assembleOverlays(in, stream)));
    CHECK_MSG(indexWhere(stream, [](const OverlayInstance& instance) { return instance.strokeWidth > 0.0f && sameColor(instance, 1.0f, 0.0f, 0.0f); }) >= 0,
        "an unfocused underline is the same outline");
    in.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    in.cursor.focused = true;
    in.cursor.shape = Ubiquitty::Cursor::Shape::Underline;
    stream.assign(maxOverlaysForRows(in.grid.rows), {});
    stream.resize(static_cast<std::size_t>(assembleOverlays(in, stream)));
    CHECK_MSG(indexWhere(stream, [](const OverlayInstance& instance) { return instance.shapeKind == toolkitCaretShapeKind; }) < 0,
        "the underline is not a caret");
    in.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    in.toolkitCaret = {};
    stream.assign(maxOverlaysForRows(in.grid.rows), {});
    stream.resize(static_cast<std::size_t>(assembleOverlays(in, stream)));
    CHECK_MSG(indexWhere(stream, [](const OverlayInstance& instance) { return instance.shapeKind == toolkitCaretShapeKind; }) < 0,
        "without a toolkit the bar is the capsule");
}

TEST(passwordLockIsOptIn)
{
    TermPalette palette;
    palette.seedDefaults();
    OverlayAssemblyInputs in = populatedInputs(palette, {});

    std::vector<OverlayInstance> sized(maxOverlaysForRows(in.grid.rows));
    const int without = assembleOverlays(in, sized);

    in.shouldDrawPasswordLock = true;
    // Asking without a prompt adds nothing: the intent and the terminal's state
    // are separate facts and the badge needs both.
    CHECK(assembleOverlays(in, sized) == without);
    in.isPasswordPromptActive = true;
    const int with = assembleOverlays(in, sized);

    // The two extra instances are the badge's, and their absence in the first
    // call is the contract a frontend with its own view-layer badge relies on:
    // Core consults the terminal's password state only when asked to draw one.
    // Counted rather than colour-matched — the badge's ink is a mix of the
    // palette's foreground and background, and a search by value finds whatever
    // else happens to land on that value.
    CHECK_MSG(with == without + maxPasswordLockQuads,
        "asking for the badge adds exactly the badge; not asking adds nothing");

    // A caret with no cell after it has nowhere to stand the badge, so asking
    // for one changes nothing. Without this, the opt-in test above passes for a
    // build that draws the badge unconditionally at column zero.
    in.cursor.column = in.grid.columns - 1;
    CHECK(assembleOverlays(in, sized) == without);
}

// Where the focused pill's bounce ends. The curve rests at one, and gets there
// within a hundredth before the cut, so the settle does not jump. The clock
// starts the bounce only when the active match's epoch moves: the same focus
// seen on every later frame ages past it. At rest the ring is the matched
// cells' box reaching the corner radius past each side, and the box it keeps
// the destination in is exactly the cells the builder fills, so the fill and
// the ends meet with no gap and no overlap.
TEST(theFocusedPillRestsAroundTheCellsItsFillIsBakedInto)
{
    CHECK(focusedPillScale(-1.0f) == 1.0f);
    CHECK(focusedPillScale(0.0f) == 1.0f);
    CHECK(focusedPillScale(0.05f) > 1.1f);
    CHECK_NEAR(focusedPillScale(focusedPillBounceSeconds - 0.001f), 1.0, 0.01);
    CHECK(focusedPillScale(focusedPillBounceSeconds) == 1.0f);
    CHECK(focusedPillScale(60.0f) == 1.0f);

    FocusedMatchBounceClock clock;
    CHECK(clock.ageSeconds(0, 100) < 0.0f);
    CHECK(clock.ageSeconds(3, 120) == 0.0f);
    CHECK_NEAR(clock.ageSeconds(3, 180), 1.0, 1e-6);
    CHECK(focusedPillScale(clock.ageSeconds(3, 600)) == 1.0f);
    CHECK(clock.ageSeconds(4, 600) == 0.0f);

    TermPalette palette;
    palette.seedDefaults();
    const SelectionRowSpan match[] = { { 2, 2, 4 } };
    OverlayAssemblyInputs in = populatedInputs(palette, {});
    in.searchMatchSpans = match;
    in.searchMatchHighlight = { 1.0f, 0.82f, 0.18f, 1.0f };
    const float boxLeft = in.originInPixels.x + 2.0f * in.cellSizeInPixels.width;
    const float boxRight = in.originInPixels.x + 5.0f * in.cellSizeInPixels.width;
    const float rowTop = in.originInPixels.y + 2.0f * in.cellSizeInPixels.height;
    const float radius = in.cellSizeInPixels.height * focusedPillCornerFraction;
    for (const float age : { -1.0f, focusedPillBounceSeconds, 5.0f }) {
        in.focusedMatchAgeSeconds = age;
        std::vector<OverlayInstance> stream(64);
        const int total = assembleOverlays(in, stream);
        REQUIRE(total > 0 && total <= static_cast<int>(stream.size()));
        const std::span<const OverlayInstance> written { stream.data(), static_cast<std::size_t>(total) };
        const int ring = indexWhere(written, [](const OverlayInstance& o) { return o.shapeKind == focusedPillRingShapeKind; });
        REQUIRE(ring >= 0);
        const OverlayInstance& pill = written[static_cast<std::size_t>(ring)];
        CHECK_MSG(approx(pill.plB.z, boxLeft) && approx(pill.plB.w, boxRight), "the kept box is the filled cells");
        CHECK_MSG(approx(pill.plA.x, boxLeft - radius) && approx(pill.plA.z, boxRight + radius), "the ends reach a corner radius past them");
        CHECK_MSG(approx(pill.plA.y, rowTop) && approx(pill.plA.w, rowTop + in.cellSizeInPixels.height), "and the pill is the row's height, unscaled");
        CHECK(approx(pill.cornerRadius, radius));
        CHECK(sameColor(pill, 1.0f, 0.82f, 0.18f));
    }
}

// A fixture that answers the two glyph shapes the cursor cases below reach —
// a narrow letter and a wide ideograph — and refuses every other route, so a
// case cannot pass on a substituted glyph.
namespace {

struct CursorClaimAtlas : UbiquittyTest::StrictAtlas {
    GlyphSlotCache monoCache { 1024, recommendedHashBuckets(1024), 32 };
    int nextSlot = 0;

    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
            /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    void beginFrame() { }
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t) { return assign(codePoint); }
    AtlasGlyph wideGlyph(std::uint32_t codePoint, FontFace) { return assign(codePoint); }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }
    AtlasGlyph assign(std::uint32_t codePoint)
    {
        if (codePoint == 0x20u)
            return {}; // a space has no coverage, so it is not present
        const float u = static_cast<float>(nextSlot++) * 0.01f;
        return { { u, u }, { u + 0.02f, u + 0.02f }, true };
    }
};

// The cursor's grid: an `a` at column 0 and a two-column ideograph anchored at
// column 2, both on row 1.
constexpr int claimRows = 3;
constexpr int claimColumns = 6;
constexpr float claimCellWidth = 8.0f;
constexpr int claimCursorRow = 1;
constexpr int narrowColumn = 0;
constexpr int wideAnchorColumn = 2;

std::vector<Ubiquitty::Cell> gridWithAWidePair()
{
    std::vector<Ubiquitty::Cell> grid(claimRows * claimColumns, Ubiquitty::defaultBlankCell);
    const std::size_t rowBase = static_cast<std::size_t>(claimCursorRow) * claimColumns;
    grid[rowBase + narrowColumn].codePoint = U'a';
    Ubiquitty::Cell& anchor = grid[rowBase + wideAnchorColumn];
    anchor.codePoint = 0x4E16u; // 世, two columns
    anchor.pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    grid[rowBase + wideAnchorColumn + 1].pen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    return grid;
}

// One frame's streams, kept together because a case reads the cursor out of
// two of them: the overlay quad the hollow ring is, and the cell the focused
// block is.
struct CursorFrame {
    std::vector<CellInstance> cells;
    std::vector<OverlayInstance> overlays;
    InstanceCounts counts;
};

// Builds one frame over `grid`. `dirtyRows` is the caller's repaint mask, null
// for a frame that rebuilds every row.
CursorFrame buildCursorFrame(const std::vector<Ubiquitty::Cell>& grid, const TermPalette& palette,
    int cursorColumn, Ubiquitty::Rendering::CursorRenderState cursor, const std::uint8_t* dirtyRows)
{
    BuildInputs in;
    in.cells = grid.data();
    in.rows = claimRows;
    in.columns = claimColumns;
    in.palette = &palette;
    in.metrics.cellWidth = claimCellWidth;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.glyphReferenceLineHeightInPixels = 16.0f;
    in.cellHeightPoints = 16.0;
    in.dirtyRows = dirtyRows;
    in.cursor = cursor;
    in.cursor.row = claimCursorRow;
    in.cursor.column = cursorColumn;

    CursorFrame frame;
    frame.cells.resize(static_cast<std::size_t>(claimRows) * claimColumns + 8);
    frame.overlays.resize(maxOverlaysForRows(claimRows));
    std::vector<DecorationInstance> decorations(static_cast<std::size_t>(claimRows) * claimColumns * 8);
    InstanceBuffers out { frame.cells.data(), frame.cells.size(), decorations.data(), decorations.size() };
    out.overlays = frame.overlays.data();
    out.overlayCapacity = frame.overlays.size();
    CursorClaimAtlas atlas;
    frame.counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out);
    return frame;
}

// The cursor the unfocused pane draws: a block that declines its fill, which
// the stream carries as the one instance with a stroke.
Ubiquitty::Rendering::CursorRenderState hollowBlockCursor()
{
    Ubiquitty::Rendering::CursorRenderState cursor;
    cursor.shape = Ubiquitty::Cursor::Shape::Block;
    cursor.focused = false;
    cursor.mixA = 1.0f;
    cursor.rgb = { 1.0f, 0.0f, 0.0f, 1.0f };
    return cursor;
}

// The frame's hollow ring, or null where it drew none. A stroke is what makes
// it itself: every other quad in this stream is a fill.
const OverlayInstance* ringOf(const CursorFrame& frame)
{
    for (std::size_t i = 0; i < frame.counts.overlays && i < frame.overlays.size(); ++i) {
        if (frame.overlays[i].strokeWidth > 0.0f)
            return &frame.overlays[i];
    }
    return nullptr;
}

// The ring's width in cells, or 0 when the frame drew no ring.
float ringWidthInCells(const CursorFrame& frame)
{
    const OverlayInstance* ring = ringOf(frame);
    return ring ? (ring->bottomRightInPixels.x - ring->topLeftInPixels.x) / claimCellWidth : 0.0f;
}

} // namespace

// The unfocused hollow cursor covers the glyph it stands on, on a row the
// frame repaints nothing of.
//
// The claim is a fact about the grid, and this frame's mask is the reason it
// has to be read as one: no cursor is owed a repaint, since the retained
// pixels carry none, so an inactive window's frames leave the cursor's row clean.
// A claim discovered by walking the row is then a claim the inactive window
// never has, and its outline stands one column wide on a two-column glyph
// while the focused block over the same character covers both.
TEST(theHollowCursorSpansItsGlyphOnAnUnrepaintedRow)
{
    TermPalette palette;
    palette.seedDefaults();
    const std::vector<Ubiquitty::Cell> grid = gridWithAWidePair();
    // Every row but the cursor's, which is the mask an unfocused pane hands a
    // frame that repaints nothing: the outline is an overlay, and no cell of
    // its row changed.
    const std::uint8_t everyRowButTheCursors[claimRows] = { 1, 0, 1 };

    const CursorFrame onTheAnchor = buildCursorFrame(grid, palette, wideAnchorColumn, hollowBlockCursor(), everyRowButTheCursors);
    CHECK_MSG(approx(ringWidthInCells(onTheAnchor), 2.0f),
        "the hollow ring covers both columns of the ideograph it stands on");

    // The cursor addressed to the pair's second column is the same character:
    // a wide pair has no column of its own the cursor could mean.
    const CursorFrame onTheContinuation = buildCursorFrame(grid, palette, wideAnchorColumn + 1, hollowBlockCursor(), everyRowButTheCursors);
    CHECK_MSG(approx(ringWidthInCells(onTheContinuation), 2.0f),
        "and covers both columns from the pair's second column too");
    const OverlayInstance* fromTheContinuation = ringOf(onTheContinuation);
    const OverlayInstance* fromTheAnchor = ringOf(onTheAnchor);
    CHECK_MSG(fromTheContinuation && fromTheAnchor
            && approx(fromTheContinuation->topLeftInPixels.x, fromTheAnchor->topLeftInPixels.x),
        "anchored at the glyph's own column, not at the column addressed");

    // A narrow glyph keeps a one-cell ring, which is what says the width comes
    // from the character rather than from a constant.
    const CursorFrame onTheLetter = buildCursorFrame(grid, palette, narrowColumn, hollowBlockCursor(), everyRowButTheCursors);
    CHECK_MSG(approx(ringWidthInCells(onTheLetter), 1.0f),
        "the ring over an `a` is one cell, as it was");

    // The focused block is the cell it stands on, re-emitted with the cursor's
    // colours — at mixA 1 its background resolves to the cursor colour — and
    // its width is the claim the glyph makes.
    Ubiquitty::Rendering::CursorRenderState focused = hollowBlockCursor();
    focused.focused = true;
    const CursorFrame focusedFrame = buildCursorFrame(grid, palette, wideAnchorColumn, focused, nullptr);
    CHECK_MSG(ringWidthInCells(focusedFrame) == 0.0f, "a focused block draws no ring");
    float focusedBlockWidthInCells = 0.0f;
    float focusedBlockColumn = -1.0f;
    for (std::size_t i = 0; i < focusedFrame.counts.cells && i < focusedFrame.cells.size(); ++i) {
        const CellInstance& instance = focusedFrame.cells[i];
        if (approx(instance.background.x, 1.0f) && approx(instance.background.y, 0.0f) && approx(instance.background.z, 0.0f)) {
            focusedBlockWidthInCells = instance.widthCells;
            focusedBlockColumn = instance.gridPosition.x;
        }
    }
    CHECK_MSG(approx(focusedBlockWidthInCells, 2.0f) && approx(focusedBlockColumn, static_cast<float>(wideAnchorColumn)),
        "the focused block covers the same two columns");
}

UBIQUITTY_TEST_MAIN()
