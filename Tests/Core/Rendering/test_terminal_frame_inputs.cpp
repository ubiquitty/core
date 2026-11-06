// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_terminal_frame_inputs.cpp — the Terminal-to-BuildInputs capture.
//
// Turning a terminal into the builder's inputs is the step every backend
// performs before it can draw anything, and the step each one otherwise
// performs for itself. These cases pin what the capture must derive, so a
// backend reusing it inherits the behaviour rather than a second reading of it.

#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "StrictAtlas.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>

#include "Core/Attributes/LinkRun.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Rendering/BeamLayout.h"
#include "Core/Rendering/CellCanvas.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

std::unique_ptr<Terminal> makeTerminal(int rows = 6, int columns = 20)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

void feed(Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

CellMetrics unifontMetrics()
{
    UnifontRasterizer rasterizer(1, 0);
    return rasterizer.cellMetrics();
}

TEST(testCaptureFillsTheGridAndGeometry)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));

    const BuildInputs& in = frame.inputs();
    CHECK(in.rows == 6);
    CHECK(in.columns == 20);
    CHECK_MSG(in.cells != nullptr, "the capture owns the rows it points at");
    CHECK_MSG(in.palette != &t->palette, "the capture owns the palette it points at");
    CHECK(in.palette->defaultForeground.red == t->palette.defaultForeground.red);
    CHECK_MSG(in.lineAttributes != nullptr, "line attributes are derived per row");
    CHECK_MSG(in.cells[0].codePoint == 'h', "row 0 holds what was written");
    CHECK(in.cells[4].codePoint == 'o');

    t.reset();
}

TEST(compactRowsReachFullAndRetainedCapturesWithoutBorrowingLiveStorage)
{
    constexpr int rows = 3;
    constexpr int columns = 12;
    constexpr std::array<std::uint32_t, 5> family { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 };
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        auto terminal = makeTerminal(rows, columns);
        if (screen == ScreenMode::Alternate)
            feed(*terminal, "\033[?1049h");
        // A whole-width recycle admits compact storage after resize materialized the grid.
        feed(*terminal, "\033[HAAAAAAAAAAAABBBBBBBBBBBBCCCCCCCCCCCC");
        feed(*terminal, "\033[?2027h\033[3;1H\r\nretained\r\ncompact\033[1;1H"
                        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7");
        REQUIRE(terminal->grid->compactASCIIRow(Row(1)));
        REQUIRE(terminal->grid->compactASCIIRow(Row(2)));

        TerminalSnapshotOptions options;
        TerminalFrameInputs held;
        REQUIRE(held.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
        REQUIRE(held.inputs().clusterResolver);
        std::array<Cell, rows * columns> retainedCells;
        std::copy_n(held.inputs().cells, retainedCells.size(), retainedCells.begin());
        CHECK_EQ(retainedCells[columns].codePoint, std::uint32_t { 'r' });
        CHECK_EQ(retainedCells[2 * columns].codePoint, std::uint32_t { 'c' });

        RetainedFrameBaseline baseline;
        RetainedFrameRequest request;
        request.rows = rows;
        request.columns = columns;
        request.isTargetMatching = true;
        request.isGridCacheEnabled = true;
        request.cellHeightInPixels = 16;
        request.textureHeightInPixels = rows * 16;
        std::array<std::uint8_t, rows> damage {};
        TerminalFrameInputs incremental;
        const auto initial = planRetainedFrame(*terminal, baseline, request, damage);
        REQUIRE(incremental.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, initial.refresh));

        feed(*terminal, "\033[3;1H\r\nfresh");
        REQUIRE(terminal->grid->compactASCIIRow(Row(2)));
        const auto next = planRetainedFrame(*terminal, baseline, request, damage);
        CHECK(next.refresh.canReuseRows);
        CHECK_EQ(next.damage.scrollDelta, 1);
        CHECK(damage[2]);
        REQUIRE(incremental.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, next.refresh));
        TerminalFrameInputs full;
        REQUIRE(full.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
        CHECK(std::memcmp(incremental.inputs().cells, full.inputs().cells, sizeof(retainedCells)) == 0);
        CHECK(std::memcmp(incremental.inputs().lineAttributes, full.inputs().lineAttributes, rows) == 0);

        feed(*terminal, "\033[1;1H\xf0\x9f\x8f\xb3\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x8c\x88");
        Ubiquitty::resize(*terminal, 4, 9);
        terminal.reset();
        CHECK_EQ(held.inputs().rows, rows);
        CHECK_EQ(held.inputs().columns, columns);
        CHECK(std::memcmp(held.inputs().cells, retainedCells.data(), sizeof(retainedCells)) == 0);
        std::array<std::uint32_t, clusterPoolMaximumCodePoints> resolved {};
        const auto& captured = held.inputs();
        const int count = captured.clusterResolver(captured.cells, resolved, captured.clusterResolverContext);
        REQUIRE(count == static_cast<int>(family.size()));
        CHECK(std::ranges::equal(std::span(resolved).first(family.size()), family));
    }
}

TEST(pixelReportBasisBelongsToTheCapturedPicture)
{
    auto terminal = makeTerminal();
    auto* capture = FrameCapture::make();
    REQUIRE(capture);
    TerminalSnapshotOptions options;
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!capture->isFullscreenContentActive());
    feed(*terminal, "\x1b[?7402h");
    CHECK(!capture->isFullscreenContentActive());
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(capture->isFullscreenContentActive());
    feed(*terminal, "\x1b[?7402l");
    CHECK(capture->isFullscreenContentActive());
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!capture->isFullscreenContentActive());
    releaseFrameCapture(capture);
}

TEST(uniformFitRebuildsCapturedImageEdgesWithoutChangingPixelsOrTheGrid)
{
    auto terminal = makeTerminal();
    feed(*terminal, "\033[2;3H\033_Ga=T,i=71,f=24,s=1,v=1,C=1;/wAA\033\\");
    Ubiquitty::drainAsyncDecodes(*terminal);
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const auto metrics = unifontMetrics();
    REQUIRE(frame.capture(*terminal, options, metrics, GlyphSlotGeometry {}));
    REQUIRE(frame.imageQuads().size() == 1);
    const auto natural = frame.imageQuads()[0];
    REQUIRE(natural.maxXInPixels - natural.minXInPixels == 1);
    const auto pixelGeneration = terminal->images.stored[0].contentGeneration;

    options.originInPixels = { 5, 9 };
    REQUIRE(frame.capture(*terminal, options, scaledCellMetrics(metrics, 0.5f), GlyphSlotGeometry {}));
    REQUIRE(frame.imageQuads().size() == 1);
    const auto fitted = frame.imageQuads()[0];
    CHECK_EQ(fitted.minXInPixels, 5 + natural.minXInPixels * 0.5f);
    CHECK_EQ(fitted.maxXInPixels, 5 + natural.maxXInPixels * 0.5f);
    CHECK_EQ(fitted.minYInPixels, 9 + natural.minYInPixels * 0.5f);
    CHECK_EQ(fitted.maxYInPixels, 9 + natural.maxYInPixels * 0.5f);
    CHECK_EQ(fitted.u0, natural.u0);
    CHECK_EQ(fitted.u1, natural.u1);
    CHECK_EQ(fitted.v0, natural.v0);
    CHECK_EQ(fitted.v1, natural.v1);
    CHECK_EQ(frame.inputs().columns, terminal->columns);
    CHECK_EQ(frame.inputs().rows, terminal->rows);
    CHECK_EQ(terminal->images.stored[0].contentGeneration, pixelGeneration);

    options.originInPixels = {};
    REQUIRE(frame.capture(*terminal, options, metrics, GlyphSlotGeometry {}));
    REQUIRE(frame.imageQuads().size() == 1);
    CHECK_EQ(frame.imageQuads()[0].minXInPixels, natural.minXInPixels);
    CHECK_EQ(frame.imageQuads()[0].maxYInPixels, natural.maxYInPixels);
}

// A mirrored screen carries its prompt marks into the render inputs.
//
// OverlayAssembly draws marks only when promptMarks.rowMarks is non-null, and
// the one place that field is ever set reads source.rowMarks from the terminal
// being captured. A synced session is rendered from a terminal the wire filled,
// so the marks have to survive checkpoint -> apply -> capture to be drawn at
// all, and an arm draws nothing unless this path carries them.
//
// Both halves are asserted. The mark has to arrive, and it has to arrive on the
// row it was set on — a capture that filled the array with the first row's value
// would satisfy a null check and put the bracket somewhere nobody typed.
TEST(testAMirroredScreenCarriesItsPromptMarksIntoTheRenderInputs)
{
    auto host = makeTerminal();
    // Two rows apart, so a mark landing on the wrong one is a wrong answer
    // rather than the right one by coincidence.
    feed(*host, "\x1b]133;A\x07prompt one\r\n");
    feed(*host, "output\r\n");
    feed(*host, "\x1b]133;A\x07prompt two\r\n");

    auto mirror = makeTerminal();
    CHECK_MSG(UbiquittyTest::copyThroughCheckpoint(*host, *mirror) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied,
        "premise: the mirror takes the host's checkpoint");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*mirror, options, unifontMetrics(), GlyphSlotGeometry {}));
    const BuildInputs& in = frame.inputs();
    REQUIRE(in.promptMarks.rowMarks != nullptr);

    const auto marked = [&in](int row) {
        return (in.promptMarks.rowMarks[static_cast<std::size_t>(row)] & Screen::osc133MarkPromptStart) != 0;
    };
    CHECK_MSG(marked(0), "the first prompt's row reaches the render inputs marked");
    CHECK_MSG(marked(2), "the second prompt's row reaches them marked too");
    CHECK_MSG(!marked(1), "the row between two prompts is not marked");

    host.reset();
    mirror.reset();
}

TEST(testDegenerateTerminalIsRefused)
{
    // A grid with no extent has no rows to snapshot, and a backend that drew
    // from a half-filled capture would read whatever the previous frame left.
    // A constructed Terminal is already 24x80, so the degenerate state has to be
    // imposed — which is exactly the shape a caller mid-teardown hands over.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));

    t->rows = 0;
    CHECK_MSG(!frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}), "no rows is refused");
    CHECK_MSG(frame.inputs().cells == nullptr, "a refused capture points at nothing");
    CHECK(frame.inputs().rows == 0);

    t->rows = 6;
    t->columns = 0;
    CHECK_MSG(!frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}), "no columns is refused");
    CHECK_MSG(frame.inputs().cells == nullptr, "still points at nothing");

    t->columns = 20;
    t.reset();
}

// The cursor is a screen position, so scrolling back moves it down the visible
// window and past its bottom edge. A capture that kept reporting it would draw a
// cursor onto scrollback the user is only reading.
TEST(testCursorIsResolvedAgainstTheVisibleWindow)
{
    auto t = makeTerminal(4, 20);
    for (int i = 0; i < 12; ++i)
        feed(*t, "line\r\n");

    TerminalSnapshotOptions options;
    TerminalFrameInputs frame;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row >= 0, "the live tail shows the cursor");

    // Scroll far enough back that the cursor's screen row leaves the window.
    options.viewOffset = 8;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row < 0, "a cursor scrolled out of view is not drawn");

    t.reset();
}

TEST(testHiddenCursorAndDrawCursorAreHonoured)
{
    auto t = makeTerminal();
    TerminalFrameInputs frame;

    TerminalSnapshotOptions options;
    options.drawCursor = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row < 0, "drawCursor false suppresses it");

    options.drawCursor = true;
    feed(*t, "\033[?25l"); // DECTCEM off
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row < 0, "a hidden cursor is not drawn");

    feed(*t, "\033[?25h");
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row >= 0, "showing it again draws it");

    t.reset();
}

// kitty's cursor_text names the colour of the glyph under a focused block. The
// capture carries it to the cursor the builder bakes, and a reset hands the
// glyph back to the renderer's own contrast.
TEST(aBlockCursorDrawsItsGlyphInTheCursorTextColor)
{
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().cursor.hasTextColor);

    feed(*t, "\033]21;cursor_text=#336699\033\\");
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const CursorRenderState& cursor = frame.inputs().cursor;
    CHECK(cursor.hasTextColor);
    CHECK(std::fabs(cursor.textRGB.x - 0x33 / 255.0f) < 0.001f);
    CHECK(std::fabs(cursor.textRGB.y - 0x66 / 255.0f) < 0.001f);
    CHECK(std::fabs(cursor.textRGB.z - 0x99 / 255.0f) < 0.001f);
    const CursorBake baked = bakeBlockCursor({ 1, 1, 1, 1 }, { 0, 0, 0, 1 }, cursor.rgb, 1.0f, { CursorTextInk::Source::Color, cursor.textRGB });
    CHECK(std::fabs(baked.foreground.x - cursor.textRGB.x) < 0.001f);
    CHECK(std::fabs(baked.foreground.y - cursor.textRGB.y) < 0.001f);
    CHECK(std::fabs(baked.foreground.z - cursor.textRGB.z) < 0.001f);

    feed(*t, "\033]21;cursor_text\033\\");
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().cursor.hasTextColor);

    t.reset();
}

// The cursor's opacity arrives beside its colour rather than on it: the model's
// palette is three channels, so a reader who asked for a translucent cursor can
// only say so through the options. The capture has to carry that into the fourth
// channel the builder reads, and default it to solid for a caller who says
// nothing — an unconfigured terminal draws an opaque cursor.
TEST(testCursorAlphaReachesTheAssembledColour)
{
    auto t = makeTerminal();
    TerminalFrameInputs frame;

    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(std::fabs(frame.inputs().cursor.rgb.w - 1.0f) < 0.001f,
        "a caller that sets nothing gets a solid cursor");
    const float solidRed = frame.inputs().cursor.rgb.x;

    options.cursorAlpha = 0.5f;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(std::fabs(frame.inputs().cursor.rgb.w - 0.5f) < 0.001f,
        "the asked-for opacity is what the builder reads");

    // The three colour channels are the model's and the opacity is the reader's,
    // so asking for a translucent cursor must not tint it.
    CHECK_MSG(std::fabs(frame.inputs().cursor.rgb.x - solidRed) < 0.001f,
        "the colour channels are untouched by the opacity");

    t.reset();
}

TEST(testViewOffsetIsClampedToAvailableScrollback)
{
    auto t = makeTerminal(4, 20);
    feed(*t, "one\r\ntwo\r\n");

    TerminalSnapshotOptions options;
    options.viewOffset = 10000; // far past anything scrolled off
    TerminalFrameInputs frame;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.viewOffset() <= 2, "the offset is clamped to real history");
    CHECK_MSG(frame.viewOffset() == frame.inputs().viewOffset, "the clamped value is what the builder sees");
    CHECK_MSG(frame.firstAbsoluteRow() >= 0, "the absolute row stays in range");

    options.viewOffset = -5;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.viewOffset() == 0, "a negative offset clamps to the live tail");

    t.reset();
}

// Style is not passed through — it is folded into the metrics the decorations
// are drawn from, and the builder reads only the folded result.
TEST(testStyleFoldsIntoMetrics)
{
    auto t = makeTerminal();
    const CellMetrics base = unifontMetrics();

    TerminalSnapshotOptions options;
    options.style.underlineThicknessScale = 3.0f;
    options.style.underlineOffsetInPixels = 2.0f;
    options.style.shouldUnderlineSkipInk = true;
    options.style.underlineSkipInkPaddingInPixels = 4.0f;

    TerminalFrameInputs frame;
    CHECK(frame.capture(*t, options, base, GlyphSlotGeometry {}));

    CHECK(frame.metrics().underlineThickness == base.underlineThickness * 3.0f);
    CHECK(frame.metrics().underlineCurlyThickness == base.underlineCurlyThickness * 3.0f);
    CHECK(frame.metrics().underlineDrop == base.underlineDrop + 2.0f);
    CHECK(frame.metrics().skipInk);
    CHECK(frame.metrics().skipInkPadding == 4.0f);
    // Asserted by value, not by pointer identity: the field the builder
    // actually reads must carry the folded numbers, and a zero cell box would
    // place every glyph at the grid corner while the cell count stayed right.
    CHECK(frame.inputs().metrics.cellWidth > 0);
    CHECK(frame.inputs().metrics.cellHeight > 0);
    CHECK(frame.inputs().metrics.underlineThickness == frame.metrics().underlineThickness);
    CHECK(frame.inputs().metrics.underlineDrop == frame.metrics().underlineDrop);
    CHECK(frame.inputs().metrics.skipInk == frame.metrics().skipInk);

    t.reset();
}

TEST(testTheBitmapTierIsCoresVerdictFromTheUnifontScale)
{
    // The style's unifont scale is the routing verdict Core hands a frontend
    // (`unifontScaleForConfiguredFamily`): positive for a face Core rasterises
    // itself, zero for a CoreText or FreeType face. The frame folds it into one
    // whole-frame fact the uniforms carry, so a procedural pass on a GPU arm
    // learns the tier from Core and never from a frontend's own reading of
    // the font name.
    auto t = makeTerminal();
    const CellMetrics base = unifontMetrics();

    TerminalSnapshotOptions options;
    options.style.unifontScale = 2;
    TerminalFrameInputs bitmap;
    CHECK(bitmap.capture(*t, options, base, GlyphSlotGeometry {}));
    CHECK(bitmap.inputs().isBitmapFace);
    CHECK(deriveFrameUniforms(bitmap.inputs()).bitmapTier == 1);

    options.style.unifontScale = 0;
    TerminalFrameInputs native;
    CHECK(native.capture(*t, options, base, GlyphSlotGeometry {}));
    CHECK(!native.inputs().isBitmapFace);
    CHECK(deriveFrameUniforms(native.inputs()).bitmapTier == 0);

    // A style nobody wrote the scale into claims no tier. The GPU arms set it
    // from their atlas; a frontend that forgot must land on the native path,
    // never on the bitmap one, or a real font would composite like Unifont.
    TerminalSnapshotOptions untouched;
    TerminalFrameInputs forgotten;
    CHECK(forgotten.capture(*t, untouched, base, GlyphSlotGeometry {}));
    CHECK(!forgotten.inputs().isBitmapFace);

    t.reset();
}

TEST(testNegativeStyleScalesAreClampedNotPropagated)
{
    auto t = makeTerminal();
    TerminalSnapshotOptions options;
    options.style.underlineThicknessScale = -2.0f;
    options.style.underlineSkipInkPaddingInPixels = -1.0f;
    options.style.backgroundOpacity = 4.0f;

    TerminalFrameInputs frame;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.metrics().underlineThickness == 0.0f, "a negative scale clamps to zero, not a mirrored rule");
    CHECK(frame.metrics().skipInkPadding == 0.0f);
    CHECK_MSG(frame.inputs().backgroundOpacity == 1.0f, "opacity above one clamps to opaque");

    options.style.backgroundOpacity = -1.0f;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().backgroundOpacity == 0.0f);

    t.reset();
}

TEST(testReverseVideoReachesTheBuilder)
{
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().screenReverse);

    feed(*t, "\033[?5h"); // DECSCNM
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().screenReverse, "DECSCNM is folded in, not left to the backend");

    t.reset();
}

// Recapturing reuses the storage, which is what makes this usable per frame
// rather than once. A grid that shrank must not leave stale cells visible past
// the new extent.
TEST(testRecaptureAfterResizeDropsStaleCells)
{
    auto t = makeTerminal(6, 20);
    feed(*t, "aaaaaaaaaaaaaaaaaaaa");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().rows == 6 && frame.inputs().columns == 20);

    Ubiquitty::resize(*t, 3, 8);
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().rows == 3);
    CHECK(frame.inputs().columns == 8);
    // Every cell the builder will read must be inside the new grid's worth of
    // storage; reading beyond it is what a resize() rather than assign() allows.
    for (int i = 0; i < 3 * 8; ++i)
        CHECK(frame.inputs().cells[i].codePoint != 0xFFFFFFFFu);

    t.reset();
}

// A slot with margins, the configuration that selects the subpixel glyph pass.
GlyphSlotGeometry marginedSlot()
{
    GlyphSlotGeometry geometry {};
    geometry.columns = 16;
    geometry.glyphWidthInPixels = 10;
    geometry.glyphMarginInPixels.width = 3;
    geometry.glyphMarginInPixels.height = 2;
    geometry.slotWidthInPixels = 10 + 2 * 3 + 1; // the odd pixel is phase headroom
    geometry.slotHeightInPixels = 20 + 2 * 2;
    return geometry;
}

TEST(testCaptureFillsTheCellBoxTheBuilderReads)
{
    // A zero-sized cell collapses every origin and advance to the corner while
    // the cell count stays right, so no count downstream contradicts it. The
    // capture must fill the one field buildCellInstances reads.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const CellMetrics base = unifontMetrics();
    CHECK(base.cellWidth > 0.0f && base.cellHeight > 0.0f);
    CHECK(frame.capture(*t, options, base, GlyphSlotGeometry {}));

    const BuildInputs& in = frame.inputs();
    CHECK_MSG(in.metrics.cellWidth == base.cellWidth, "the builder's own field carries the cell box");
    CHECK(in.metrics.cellHeight == base.cellHeight);
    // SoftwareRenderer keeps caller-supplied metrics rather than substituting
    // its rasterizer's, so both consumers read the same box by construction.
    CHECK(in.metrics.underlineThickness == frame.metrics().underlineThickness);

    t.reset();
}

TEST(testCaptureCarriesWindowFocusToTheCursor)
{
    // A focused block cursor is the tier over its cell; an unfocused one is a hollow
    // outline over unmodified text. The builder reads BuildInputs::cursor.focused
    // and that member defaults to true, so a capture that never assigns it paints
    // every background window's cursor solid — and solid is what an active window
    // looks like, so nothing about the frame says it is wrong.
    auto t = makeTerminal();
    feed(*t, "x");
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    options.isFocused = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row >= 0, "the cursor is still drawn when unfocused");
    CHECK_MSG(!frame.inputs().cursor.focused, "an unfocused surface reports an unfocused cursor");

    options.isFocused = true;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().cursor.focused);

    t.reset();
}

TEST(testCaptureCarriesTheTerminalsGlidePolicy)
{
    // The animator that ticks Core's cursor motion reads its mode and duration
    // off the capture, so the glide follows the terminal's `[cursor]` seed and
    // no frontend keeps a settings store for it — the store neither shell ever
    // wrote, which is why no arm glided. Carried whether or not the frame draws
    // a cursor: a hidden cursor pins the motion state rather than skipping it.
    auto t = makeTerminal();
    feed(*t, "x");
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.cursorMotionMode() == CursorMotionMode::Off, "an unseeded terminal captures a caret that snaps");
    CHECK(frame.cursorMotionDurationMilliseconds() == 0u);

    t->cursorMotionModePref = CursorMotionMode::Long;
    t->cursorMotionDurationMilliseconds = 180;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.cursorMotionMode() == CursorMotionMode::Long, "the seeded policy rides the capture");
    CHECK_MSG(frame.cursorMotionDurationMilliseconds() == 180u, "with the seeded duration");

    feed(*t, "\x1b[?25l"); // DECTCEM hidden: no cursor is drawn, the policy still travels
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().cursor.row < 0, "the fixture hides the cursor");
    CHECK_MSG(frame.cursorMotionMode() == CursorMotionMode::Long, "a hidden cursor keeps its policy on the capture");

    t.reset();
}

TEST(testTheMotionTargetAgreesWithTheCaptureItPrecedes)
{
    // A frontend that plans before it captures ticks the caret's motion at
    // plan time, against this resolver; the capture then resolves the same
    // cursor. A resolver that disagreed would slide the caret toward a cell
    // nothing draws, so every answer here is checked against the capture's.
    auto t = makeTerminal(4, 20);
    t->cursorMotionModePref = CursorMotionMode::Always;
    t->cursorMotionDurationMilliseconds = 90;
    feed(*t, "\x1b[3;6H");
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CursorMotionTarget target = resolveCursorMotionTarget(*t, options.viewOffset, options.drawCursor);
    CHECK_MSG(target.visibleRow == frame.inputs().cursor.row, "the target's row is the capture's");
    CHECK_MSG(target.column == frame.inputs().cursor.column, "and so is its column");
    CHECK(target.visibleRow == 2 && target.column == 5);
    CHECK_MSG(target.mode == CursorMotionMode::Always, "the policy rides the target");
    CHECK(target.durationMilliseconds == 90u);

    // Scrolled into history the cursor leaves the window, and both say so.
    for (int line = 0; line < 8; ++line)
        feed(*t, "\r\n");
    options.viewOffset = 4;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    target = resolveCursorMotionTarget(*t, options.viewOffset, options.drawCursor);
    CHECK_MSG(frame.inputs().cursor.row < 0, "the fixture scrolls the cursor off the window");
    CHECK_MSG(target.visibleRow == -1, "and the target draws none");

    // Hidden by DECTCEM, or a caller that draws no cursor: none either way,
    // and the policy still travels for the tick that pins the motion.
    options.viewOffset = 0;
    feed(*t, "\x1b[?25l");
    target = resolveCursorMotionTarget(*t, options.viewOffset, options.drawCursor);
    CHECK(target.visibleRow == -1);
    feed(*t, "\x1b[?25h");
    target = resolveCursorMotionTarget(*t, options.viewOffset, /*drawCursor=*/false);
    CHECK(target.visibleRow == -1);
    CHECK_MSG(target.mode == CursorMotionMode::Always, "a frame that draws no cursor still carries the policy");

    t.reset();
}

TEST(testCaptureCarriesTheGridOrigin)
{
    // Glyph instances carry absolute device positions, so a grid inset from the
    // drawable edge needs the inset. Cell instances carry grid coordinates and
    // the shader applies the same origin, so a dropped origin misplaces every
    // glyph while every cell stays right — a frame that looks like a font bug.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().originInPixels.x == 0.0f, "a grid-sized surface needs no inset");
    CHECK(frame.inputs().originInPixels.y == 0.0f);

    options.originInPixels.x = 8.0f;
    options.originInPixels.y = 3.5f;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().originInPixels.x == 8.0f);
    CHECK_MSG(frame.inputs().originInPixels.y == 3.5f, "a fractional inset survives the seam");

    t.reset();
}

TEST(testWideSlotGeometryGatesTheWideStream)
{
    // A backend keeping two-cell glyphs on a sheet of their own reports a wide
    // slot; one with a single sheet reports none and its wide glyphs stay on
    // their cells. Zero is the default, so the second case is what a caller
    // that never sets it gets.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().glyphWideSlotWidthInPixels == 0.0f, "no wide sheet by default");

    GlyphSlotGeometry geometry = marginedSlot();
    geometry.wideSlotWidthInPixels = 2 * geometry.slotWidthInPixels;
    geometry.wideSlotHeightInPixels = geometry.slotHeightInPixels;
    CHECK(frame.capture(*t, options, unifontMetrics(), geometry));
    CHECK(frame.inputs().glyphWideSlotWidthInPixels == 34.0f);
    CHECK(frame.inputs().glyphWideSlotHeightInPixels == 24.0f);
    // The cell box, not the advance: a two-cell slot is laid out in whole boxes.
    CHECK_MSG(frame.inputs().glyphBoxWidthInPixels == 10.0f, "the wide scale references the cell box");

    t.reset();
}

TEST(testGlyphSlotGeometryReachesTheBuildInputs)
{
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), marginedSlot()));

    const BuildInputs& in = frame.inputs();
    CHECK_MSG(in.subpixelGlyphs, "a margin selects the subpixel glyph pass");
    CHECK(in.glyphMarginInPixels.width == 3.0f);
    CHECK(in.glyphMarginInPixels.height == 2.0f);
    // The physical slot, phase headroom included: the quad is the slot and the
    // UV is the full slot, so a quad sized to the cell box would resample.
    CHECK(in.glyphSlotWidthInPixels == 17.0f);
    CHECK(in.glyphSlotHeightInPixels == 24.0f);

    t.reset();
}

TEST(testEachCaptureCarriesTheGeometryItWasGiven)
{
    // Alternating rather than repeating: a capture that kept the previous one's
    // geometry passes a repeated margined capture and fails here on the second
    // frame. The third capture is margined again because a capture that cleared
    // the geometry instead of adopting the argument also passes the first two.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const GlyphSlotGeometry empty {};

    CHECK(frame.capture(*t, options, unifontMetrics(), marginedSlot()));
    CHECK_MSG(frame.inputs().subpixelGlyphs, "a margin selects the subpixel glyph pass");
    CHECK(frame.inputs().glyphSlotWidthInPixels == 17.0f);

    CHECK(frame.capture(*t, options, unifontMetrics(), empty));
    CHECK_MSG(!frame.inputs().subpixelGlyphs, "an empty slot does not inherit the last one's margin");
    CHECK(frame.inputs().glyphSlotWidthInPixels == 0.0f);

    CHECK(frame.capture(*t, options, unifontMetrics(), marginedSlot()));
    CHECK(frame.inputs().glyphSlotWidthInPixels == 17.0f);

    t.reset();
}

TEST(testZeroMarginSlotKeepsTheCellBakedPath)
{
    // A rasterizer with no overhang (Unifont) sizes a slot with no margin. The
    // margin is the gate, not the slot dimensions.
    auto t = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    GlyphSlotGeometry geometry {};
    geometry.columns = 16;
    geometry.glyphWidthInPixels = 8;
    geometry.slotWidthInPixels = 8;
    geometry.slotHeightInPixels = 16;
    CHECK(frame.capture(*t, options, unifontMetrics(), geometry));

    CHECK_MSG(!frame.inputs().subpixelGlyphs, "zero margin means nothing overflows");
    CHECK_MSG(frame.inputs().glyphSlotWidthInPixels == 8.0f, "the dimensions still cross the seam");

    t.reset();
}

// --- The FrameSource overload ------------------------------------------------
//
// The split exists so a frontend rendering off the model thread can capture from
// the copy it took under its lock instead of reaching back into a live Terminal.
// That is only safe if the two entry points agree, so the first case captures
// the same terminal both ways and compares the results field by field rather
// than spot-checking a few. A divergence here is a frontend drawing a different
// frame from every other one, and nothing downstream would report it.

// Reads a Terminal into the shape a frontend's own copy would have. Not a
// helper on the class: the point is that a caller assembles this from
// whatever it holds, and a convenience that took a Terminal would be the thing
// the split exists to avoid.
[[nodiscard]] Rendering::FrameSource sourceFromTerminal(const Terminal& t, std::vector<Cell>& cells,
    std::vector<std::uint8_t>& lineAttributes, std::vector<std::uint8_t>& rowMarks, int viewOffset)
{
    const int history = Screen::scrollbackCount(&t);
    const int clamped = viewOffset < 0 ? 0 : (viewOffset > history ? history : viewOffset);
    const int firstAbsoluteRow = history - clamped;
    cells.assign(static_cast<std::size_t>(t.rows) * t.columns, defaultBlankCell);
    lineAttributes.assign(static_cast<std::size_t>(t.rows), lineAttributeNormal);
    rowMarks.assign(static_cast<std::size_t>(t.rows), 0u);
    for (int row = 0; row < t.rows; ++row) {
        Screen::getDisplayRow(t, clamped, row, &cells[static_cast<std::size_t>(row) * t.columns], t.columns);
        lineAttributes[static_cast<std::size_t>(row)] = Screen::rowLineAttribute(&t, firstAbsoluteRow + row);
        rowMarks[static_cast<std::size_t>(row)] = Screen::rowMark(&t, firstAbsoluteRow + row);
    }

    Rendering::FrameSource source;
    source.cells = cells.data();
    source.lineAttributes = lineAttributes.data();
    source.rowMarks = t.screens.isUsingAlternateScreen ? nullptr : rowMarks.data();
    source.rows = t.rows;
    source.columns = t.columns;
    source.scrollbackCount = history;
    source.modelGeneration = t.modelEpoch;
    source.palette = &t.palette;
    source.paletteGeneration = t.paletteEpoch;
    source.hasPaletteGeneration = true;
    source.textSizeTable = &t.textSizeTable;
    source.cursorColor = t.palette.cursor;
    source.hasSelectionBackground = t.selectionBackgroundSet;
    source.selectionBackgroundRed = t.selectionBackgroundRed;
    source.selectionBackgroundGreen = t.selectionBackgroundGreen;
    source.selectionBackgroundBlue = t.selectionBackgroundBlue;
    source.isScreenReverse = isReverseVideo(&t);
    source.isColumns132 = t.modes.columns132;
    source.isCursorVisible = t.modes.isCursorVisible;
    source.cursorRow = t.screens.active->cursorY;
    source.cursorColumn = t.screens.active->cursorX;
    source.cursorShape = Ubiquitty::Cursor::resolveAppearance(&t, 0).shape;
    const bool hasStoredImages = std::ranges::any_of(t.images.stored, [](const Image::TerminalImage& image) {
        return image.used;
    });
    source.images = hasStoredImages ? t.images.stored.data() : nullptr;
    source.placements = t.images.livePlacementCount != 0 ? t.images.placements.data() : nullptr;
    source.clusterResolver = [](const Cell* cell, std::span<std::uint32_t> codePoints, void* context) {
        return Ubiquitty::cellClusterCodePoints(static_cast<const Terminal*>(context), cell, codePoints.data(), static_cast<int>(codePoints.size()));
    };
    source.clusterResolverContext = const_cast<Terminal*>(&t);
    source.softFont = Ubiquitty::softFontOrNull(t);
    source.isEdgeToEdge = t.modes.fullscreenContent
        && t.fullscreenContentDisplay.letterbox == Ubiquitty::FullscreenContentLetterbox::Color;
    source.letterboxColor = t.fullscreenContentDisplay.letterboxColor;
    return source;
}

TEST(testCursorPresentationPreservesModelStateThroughLiveAndSourceCaptures)
{
    enum class CaptureKind {
        Terminal,
        Source
    };
    for (const auto kind : std::array { CaptureKind::Terminal, CaptureKind::Source }) {
        auto terminal = makeTerminal();
        feed(*terminal, "\x1b[4;3H\x1b[5 q");
        terminal->palette.cursor = { 0x2222, 0x4444, 0x6666 };
        TerminalFrameInputs frame;
        TerminalSnapshotOptions options;
        options.isFocused = true;
        options.cursorMixAlpha = 0.35f;
        options.cursorAlpha = 0.6f;
        std::vector<Cell> cells;
        std::vector<std::uint8_t> lineAttributes;
        std::vector<std::uint8_t> rowMarks;
        const auto capture = [&] {
            if (kind == CaptureKind::Terminal) {
                return frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {});
            }
            return frame.capture(sourceFromTerminal(*terminal, cells, lineAttributes, rowMarks, options.viewOffset),
                options, unifontMetrics(), GlyphSlotGeometry {});
        };
        CursorPresentation presentation;
        REQUIRE(capture());
        frame.applyCursorPresentation(presentation, 0, 0.016, false);
        const auto prompt = frame.inputs().cursor;
        REQUIRE(prompt.row == 3 && prompt.column == 2);
        REQUIRE(prompt.shape == Cursor::Shape::Bar);

        feed(*terminal, "\x1b[?25l\x1b[1;10H\x1b[2 q");
        terminal->palette.cursor = { 0xffff, 0xffff, 0xffff };
        options.isFocused = false;
        options.cursorAlpha = 0.25f;
        options.cursorMixAlpha = 0.2f;
        REQUIRE(capture());
        CHECK_EQ(frame.inputs().cursor.row, -1);
        frame.applyCursorPresentation(presentation, 1, 0.016, false);
        CHECK_EQ(frame.inputs().cursor.row, 3);
        CHECK_EQ(frame.inputs().cursor.column, 2);
        CHECK(frame.inputs().cursor.shape == Cursor::Shape::Bar);
        CHECK_EQ(frame.inputs().cursor.rgb.x, prompt.rgb.x);
        CHECK_EQ(frame.inputs().cursor.rgb.w, 0.25f);
        CHECK_EQ(frame.inputs().cursor.mixA, 0.2f);
        CHECK(!frame.inputs().cursor.focused);
        CHECK(!terminal->modes.isCursorVisible);
        CHECK_EQ(terminal->screens.active->cursorX, 9);
        CHECK_EQ(terminal->screens.active->cursorY, 0);
        terminal->outputLength = 0;
        feed(*terminal, "\x1b[?25$p\x1b[6n");
        CHECK(std::string_view(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength))
            == "\x1b[?25;2$y\x1b[1;10R");

        auto mirror = makeTerminal();
        REQUIRE(UbiquittyTest::copyThroughCheckpoint(*terminal, *mirror) == Protocol::Sync::ApplyOutcome::Applied);
        CHECK(!mirror->modes.isCursorVisible);
        CHECK_EQ(mirror->screens.active->cursorX, 9);
        frame.restateCursorMixAlpha(0);
        frame.applyCursorPresentation(presentation, 1.049, 0.016, false);
        CHECK_EQ(frame.inputs().cursor.row, 3);
        CHECK_EQ(frame.inputs().cursor.mixA, 0);
        frame.applyCursorPresentation(presentation, 1.051, 0.016, false);
        CHECK_EQ(frame.inputs().cursor.row, -1);
        CHECK_EQ(presentation.deadlineSeconds(), 0);
    }
}

TEST(testRepeatedFrameCapturePresentationReturnsToItsOriginalTarget)
{
    auto terminal = makeTerminal();
    feed(*terminal, "\x1b[4;3H");
    auto* capture = FrameCapture::make();
    REQUIRE(capture != nullptr);
    CursorPresentation presentation;
    TerminalSnapshotOptions options;
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    capture->applyCursorPresentation(presentation, 0, 0.016, false);
    feed(*terminal, "\x1b[2;10H");
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    capture->applyCursorPresentation(presentation, 1, 0.016, false);
    CHECK_EQ(capture->cursorRow(), 3);
    CHECK_EQ(capture->cursorColumn(), 2);
    capture->applyCursorPresentation(presentation, 1.017, 0.016, false);
    CHECK_EQ(capture->cursorRow(), 1);
    CHECK_EQ(capture->cursorColumn(), 9);

    feed(*terminal, "\x1b[?25l");
    REQUIRE(capture->capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    capture->applyCursorPresentation(presentation, 2, 0.016, false);
    capture->setCursorMixAlpha(0.125f);
    capture->applyCursorPresentation(presentation, 2.049, 0.016, false);
    CHECK_EQ(capture->cursorMixAlpha(), 0.125f);
    capture->applyCursorPresentation(presentation, 2.051, 0.016, false);
    CHECK_EQ(capture->cursorRow(), -1);
    releaseFrameCapture(capture);
}

TEST(testCaptureHardSuppressionCancelsCursorPresentation)
{
    enum class Suppression {
        Disabled,
        Scrollback,
        StatusRow,
        AlternateScreen,
        Viewport
    };
    for (const auto suppression : std::array { Suppression::Disabled, Suppression::Scrollback,
             Suppression::StatusRow, Suppression::AlternateScreen, Suppression::Viewport }) {
        auto terminal = makeTerminal(4, 20);
        for (int row = 0; row < 8; ++row) {
            feed(*terminal, "line\r\n");
        }
        TerminalFrameInputs frame;
        TerminalSnapshotOptions options;
        std::array<Cell, 20> statusCells;
        statusCells.fill(defaultBlankCell);
        CursorPresentation presentation;
        REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
        frame.applyCursorPresentation(presentation, 0, 0.016, false);
        REQUIRE(frame.inputs().cursor.row >= 0);
        feed(*terminal, "\x1b[?25l");
        REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
        frame.applyCursorPresentation(presentation, 1, 0.016, false);
        REQUIRE(frame.inputs().cursor.row >= 0);
        switch (suppression) {
        case Suppression::Disabled:
            options.drawCursor = false;
            break;
        case Suppression::Scrollback:
            options.viewOffset = 4;
            break;
        case Suppression::StatusRow:
            options.statusRowCells = statusCells.data();
            options.statusRowColumns = static_cast<int>(statusCells.size());
            break;
        case Suppression::AlternateScreen:
            feed(*terminal, "\x1b[?1049h");
            break;
        case Suppression::Viewport:
            options.viewportSizeInPixels.width = 400;
            break;
        }
        REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
        frame.applyCursorPresentation(presentation, 1.001, 0.016, false);
        CHECK_EQ(frame.inputs().cursor.row, -1);
        CHECK_EQ(presentation.deadlineSeconds(), 0);
    }
}

TEST(testCursorAppearanceContinuesBlinkWithoutChangingHiddenResolve)
{
    auto terminal = makeTerminal();
    feed(*terminal, "\x1b[4;3H\x1b[5 q");
    terminal->cursorBlinkPref = true;
    terminal->windowFocused = true;
    bool sawFadingAppearance = false;
    for (std::uint32_t tick = 0; tick < 120; ++tick) {
        terminal->modes.isCursorVisible = true;
        const auto visible = Cursor::resolve(terminal.get(), tick);
        terminal->modes.isCursorVisible = false;
        const auto appearance = Cursor::resolveAppearance(terminal.get(), tick);
        const auto hidden = Cursor::resolve(terminal.get(), tick);
        CHECK(appearance.shape == Cursor::Shape::Bar);
        CHECK_EQ(appearance.blinkAlpha, visible.blinkAlpha);
        CHECK_EQ(appearance.row, 3);
        CHECK_EQ(appearance.column, 2);
        CHECK(!hidden.visible);
        CHECK_EQ(hidden.blinkAlpha, 0);
        CHECK_EQ(hidden.row, 0);
        sawFadingAppearance |= appearance.blinkAlpha > 0 && appearance.blinkAlpha < 1;
    }
    CHECK(sawFadingAppearance);
}

TEST(testBothEntryPointsCaptureTheSameFrame)
{
    auto t = makeTerminal();
    // A frame with something in every derivable field: text, a doubled row, a
    // prompt mark, a moved cursor, scrollback to clamp against. A blank terminal
    // agrees on both paths trivially, and would agree just as well if the split
    // had dropped every one of them.
    feed(*t, "prompt\r\n");
    feed(*t, "\x1b#6doubled\r\n");
    feed(*t, "plain text\r\n");
    // Enough to put rows in scrollback so the view offset has something to clamp
    // against, and few enough that the marked and doubled rows stay inside the
    // window at that offset. The two coverage checks below fail if this drifts,
    // which is how the count was chosen rather than guessed.
    for (int i = 0; i < 5; ++i)
        feed(*t, "scroll\r\n");
    feed(*t, "\x1b[3;5H");
    // Set on the screen's own mark table rather than fed as OSC 133. What the
    // handler does with the sequence is another test's subject; this case needs
    // the mark present in the window, and going through the parser would make it
    // depend on where the scrolling left the marked row.
    t->screens.active->mark[1] = Ubiquitty::Screen::osc133MarkCmdExecuted;

    TerminalSnapshotOptions options;
    options.viewOffset = 2;
    options.drawCursor = true;
    options.isFocused = true;
    options.style.underlineThicknessScale = 1.5f;
    options.promptMarkFontSize = 13.0f;

    TerminalFrameInputs viaTerminal;
    CHECK(viaTerminal.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));

    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    TerminalFrameInputs viaSource;
    CHECK(viaSource.capture(sourceFromTerminal(*t, cells, lineAttributes, rowMarks, options.viewOffset),
        options, unifontMetrics(), GlyphSlotGeometry {}));

    const BuildInputs& a = viaTerminal.inputs();
    const BuildInputs& b = viaSource.inputs();

    CHECK_MSG(viaTerminal.viewOffset() == viaSource.viewOffset(), "the view offset clamps the same way");
    CHECK_MSG(viaTerminal.firstAbsoluteRow() == viaSource.firstAbsoluteRow(), "and resolves the same top row");
    CHECK(a.rows == b.rows && a.columns == b.columns);
    CHECK_MSG(a.palette != b.palette, "each capture owns its palette copy");
    CHECK(a.palette->defaultForeground.red == b.palette->defaultForeground.red);
    CHECK(viaTerminal.paletteGeneration() == viaSource.paletteGeneration());
    CHECK(a.screenReverse == b.screenReverse);
    CHECK(a.isColumns132 == b.isColumns132);
    CHECK(a.viewOffset == b.viewOffset);
    CHECK(a.images == nullptr && b.images == nullptr);
    CHECK(a.placements == nullptr && b.placements == nullptr);

    // The rows themselves, cell for cell. A count comparison passes for two
    // grids of the same size holding different text.
    CHECK_MSG(a.cells != b.cells, "each capture owns its own copy");
    bool isEveryCellEqual = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(a.rows) * a.columns; ++i)
        if (std::memcmp(&a.cells[i], &b.cells[i], sizeof(Cell)) != 0)
            isEveryCellEqual = false;
    CHECK_MSG(isEveryCellEqual, "every cell agrees");
    CHECK_MSG(a.cells[0].codePoint != ' ', "and the grid is not blank, so agreeing means something");

    bool isEveryAttributeEqual = true;
    bool hasDoubledRow = false;
    for (int row = 0; row < a.rows; ++row) {
        if (a.lineAttributes[row] != b.lineAttributes[row])
            isEveryAttributeEqual = false;
        if (a.lineAttributes[row] != lineAttributeNormal)
            hasDoubledRow = true;
    }
    CHECK_MSG(isEveryAttributeEqual, "line attributes agree per row");
    CHECK_MSG(hasDoubledRow, "a doubled row is in the window, so the comparison covers one");

    CHECK_MSG((a.promptMarks.rowMarks == nullptr) == (b.promptMarks.rowMarks == nullptr),
        "both decide the same way about marks");
    if (a.promptMarks.rowMarks && b.promptMarks.rowMarks) {
        bool isEveryMarkEqual = true;
        bool hasMark = false;
        for (int row = 0; row < a.rows; ++row) {
            if (a.promptMarks.rowMarks[row] != b.promptMarks.rowMarks[row])
                isEveryMarkEqual = false;
            if (a.promptMarks.rowMarks[row] != 0u)
                hasMark = true;
        }
        CHECK_MSG(isEveryMarkEqual, "and agree per row");
        CHECK_MSG(hasMark, "with at least one real mark, so the loop compares something");
    }

    CHECK_MSG(a.cursor.row == b.cursor.row && a.cursor.column == b.cursor.column, "the cursor lands identically");
    CHECK(a.cursor.shape == b.cursor.shape);
    CHECK(a.cursor.lineAttribute == b.cursor.lineAttribute);
    CHECK_MSG(a.cursor.rgb.x == b.cursor.rgb.x && a.cursor.rgb.y == b.cursor.rgb.y
            && a.cursor.rgb.z == b.cursor.rgb.z,
        "including its colour");

    // The style fold is the capture's, not the reader's, so it must survive the
    // delegation rather than being applied twice or not at all.
    CHECK(a.metrics.underlineThickness == b.metrics.underlineThickness);
    CHECK_MSG(a.metrics.underlineThickness != unifontMetrics().underlineThickness,
        "the 1.5x scale was applied, so comparing it proves the fold ran");

    t.reset();
}

TEST(testImageFreeCaptureDoesNoImageTableWorkAcrossTransitions)
{
    auto terminal = makeTerminal();
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().images == nullptr);
    CHECK(frame.inputs().placements == nullptr);
    CHECK_EQ(frame.imageTableMutationCountForTest(), std::size_t { 0 });
    CHECK_EQ(frame.placementTableMutationCountForTest(), std::size_t { 0 });

    terminal->images.stored[0].used = true;
    terminal->images.stored[0].imageID = 41;
    terminal->images.stored[0].contentGeneration = 1;
    terminal->images.placements[0].used = true;
    terminal->images.placements[0].imageID = 41;
    terminal->images.livePlacementCount = 1;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    REQUIRE(frame.inputs().images != nullptr);
    REQUIRE(frame.inputs().placements != nullptr);
    CHECK(frame.inputs().images[0].imageID == 41);
    CHECK(frame.inputs().placements[0].imageID == 41);
    CHECK_EQ(frame.imageTableMutationCountForTest(), std::size_t { 1 });
    CHECK_EQ(frame.placementTableMutationCountForTest(), std::size_t { 1 });

    terminal->images.stored[0] = {};
    terminal->images.placements[0] = {};
    terminal->images.livePlacementCount = 0;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().images == nullptr);
    CHECK(frame.inputs().placements == nullptr);
    CHECK_EQ(frame.imageTableMutationCountForTest(), std::size_t { 2 });
    CHECK_EQ(frame.placementTableMutationCountForTest(), std::size_t { 2 });

    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.imageTableMutationCountForTest(), std::size_t { 2 });
    CHECK_EQ(frame.placementTableMutationCountForTest(), std::size_t { 2 });

    terminal->images.stored[0].used = true;
    terminal->images.stored[0].imageID = 99;
    terminal->images.stored[0].contentGeneration = 2;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    REQUIRE(frame.inputs().images != nullptr);
    CHECK(frame.inputs().images[0].imageID == 99);
    CHECK(frame.inputs().placements == nullptr);
    CHECK_EQ(frame.imageTableMutationCountForTest(), std::size_t { 3 });
    CHECK_EQ(frame.placementTableMutationCountForTest(), std::size_t { 2 });
}

TEST(testACaptureAfterASelectorDeleteDoesNoPlacementWork)
{
    // A delete by selector that takes every placement leaves nothing to draw, so
    // the capture after it hands the builder no placement table and the capture
    // after that copies none. A count left behind by the delete would keep both
    // paying for a table of empty slots on every frame.
    auto terminal = makeTerminal();
    terminal->cellPixelWidth = 8;
    terminal->cellPixelHeight = 16;
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;

    // Two 1x1 opaque images on layer 5: base64 of four 0xFF bytes.
    feed(*terminal, "\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1,z=5,i=1,q=2;/////w==\x1b\\");
    feed(*terminal, "\x1b[2;3H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1,z=5,i=2,q=2;/////w==\x1b\\");
    Ubiquitty::drainAsyncDecodes(*terminal);
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    REQUIRE(frame.inputs().placements != nullptr);

    feed(*terminal, "\x1b_Ga=d,d=z,z=5,q=2\x1b\\");
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().placements == nullptr, "a frame with no placement carries no placement table");
    const std::size_t mutations = frame.placementTableMutationCountForTest();
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.placementTableMutationCountForTest(), mutations);
}

TEST(testAuthoritativePaletteGenerationGatesOwnedCopies)
{
    auto terminal = makeTerminal();
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    FrameSource source = sourceFromTerminal(*terminal, cells, lineAttributes, rowMarks, 0);
    source.paletteGeneration = 7;
    source.hasPaletteGeneration = true;

    terminal->palette.indexed[3].red = 0x1111;
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().palette->indexed[3].red == 0x1111);

    terminal->palette.indexed[3].red = 0x2222;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().palette->indexed[3].red == 0x1111,
        "an authoritative unchanged generation preserves the owned palette");

    source.paletteGeneration = 8;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().palette->indexed[3].red == 0x2222);

    source.hasPaletteGeneration = false;
    terminal->palette.indexed[3].red = 0x3333;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().palette->indexed[3].red == 0x3333,
        "a source without a counter remains conservatively fresh");
}

TEST(testCaptureLinkColorFollowsTheOwnedPalette)
{
    auto terminal = makeTerminal();
    terminal->palette.defaultBackground = { 0, 0, 0 };
    terminal->palette.defaultForeground = { 0xffff, 0xffff, 0xffff };
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0, 0xffff, 0 };
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    FrameSource source = sourceFromTerminal(*terminal, cells, lineAttributes, rowMarks, 0);
    source.paletteGeneration = 0;
    source.hasPaletteGeneration = true;
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const auto captureAndCheck = [&](Vec4 expected, std::size_t resolves) {
        REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
        const Vec4 actual = frame.inputs().linkColor;
        CHECK(actual.x == expected.x && actual.y == expected.y && actual.z == expected.z && actual.w == expected.w);
        CHECK_EQ(frame.linkColorResolveCountForTest(), resolves);
    };
    const Vec4 green { 0.0f, 1.0f, 0.0f, 1.0f };
    const Vec4 red { 1.0f, 0.0f, 0.0f, 1.0f };
    captureAndCheck(green, 1);
    captureAndCheck(green, 1);

    // A zero epoch is authoritative too. The borrowed palette can change
    // without changing the owned palette or the colour derived from it.
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0xffff, 0, 0 };
    captureAndCheck(green, 1);
    ++source.paletteGeneration;
    captureAndCheck(red, 2);

    // No palette means the capture retains its previous owned palette; a
    // returning source must refresh even when its epoch matches the old one.
    source.palette = nullptr;
    captureAndCheck(red, 2);
    source.palette = &terminal->palette;
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0, 0xffff, 0 };
    captureAndCheck(green, 3);

    source.hasPaletteGeneration = false;
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0xffff, 0, 0 };
    captureAndCheck(red, 4);
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0, 0xffff, 0 };
    captureAndCheck(green, 5);
}

TEST(testExplicitLinkColorLeavesPaletteFallbackFresh)
{
    auto terminal = makeTerminal();
    terminal->palette.defaultBackground = { 0, 0, 0 };
    terminal->palette.defaultForeground = { 0xffff, 0xffff, 0xffff };
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0, 0xffff, 0 };
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.linkColor = { 0.25f, 0.5f, 0.75f, 0.5f };
    options.linkHoverColor = {};
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.inputs().linkColor.x, 0.25f);
    CHECK_EQ(frame.inputs().linkColor.y, 0.5f);
    CHECK_EQ(frame.inputs().linkColor.z, 0.75f);
    CHECK_EQ(frame.inputs().linkColor.w, 0.5f);
    CHECK_EQ(frame.inputs().linkHoverColor.x, 0.25f);
    CHECK_EQ(frame.inputs().linkHoverColor.w, 0.5f);
    // Metrics logging asks for the palette's answer even when it is overridden.
    const bool isLoggingMetrics = std::getenv("UBIQUITTY_METRICS_LOG") != nullptr;
    CHECK_EQ(frame.linkColorResolveCountForTest(), isLoggingMetrics ? 1u : 0u);

    options.linkColor = {};
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.inputs().linkColor.y, 1.0f);
    CHECK_EQ(frame.linkColorResolveCountForTest(), 1u);

    options.linkColor = { 0.25f, 0.5f, 0.75f, 0.5f };
    terminal->palette.indexed[4] = terminal->palette.indexed[12] = { 0xffff, 0, 0 };
    ++terminal->paletteEpoch;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.inputs().linkColor.x, 0.25f);
    CHECK_EQ(frame.linkColorResolveCountForTest(), isLoggingMetrics ? 2u : 1u);

    options.linkColor = {};
    options.linkHoverColor = { 0.75f, 0.5f, 0.25f, 0.25f };
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.inputs().linkColor.x, 1.0f);
    CHECK_EQ(frame.inputs().linkColor.y, 0.0f);
    CHECK_EQ(frame.inputs().linkColor.z, 0.0f);
    CHECK_EQ(frame.inputs().linkColor.w, 1.0f);
    CHECK_EQ(frame.inputs().linkHoverColor.x, 0.75f);
    CHECK_EQ(frame.inputs().linkHoverColor.w, 0.25f);
    CHECK_EQ(frame.linkColorResolveCountForTest(), 2u);
}

TEST(testLiveTerminalRowsLandDirectlyInCaptureStorage)
{
    auto terminal = makeTerminal();
    feed(*terminal, "first\r\nsecond");

    TerminalFrameInputs liveCapture;
    TerminalSnapshotOptions options;
    REQUIRE(liveCapture.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(liveCapture.sourceCellCopyCountForTest(), std::size_t { 0 });

    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(terminal->rows), 0);
    rowDirty[1] = 1;
    CapturedRowRefresh refresh;
    refresh.canReuseRows = true;
    refresh.rowDirty = rowDirty;
    feed(*terminal, "\033[2;1Hchanged");
    REQUIRE(liveCapture.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK_EQ(liveCapture.sourceCellCopyCountForTest(), std::size_t { 0 });
    CHECK_EQ(liveCapture.inputs().cells[static_cast<std::size_t>(terminal->columns)].codePoint,
        static_cast<std::uint32_t>('c'));

    // A detached source is the control: its storage may disappear on return,
    // so this entry point still copies every cell into the capture.
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    const FrameSource source = sourceFromTerminal(*terminal, cells, lineAttributes, rowMarks, 0);
    TerminalFrameInputs sourceCapture;
    REQUIRE(sourceCapture.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(sourceCapture.sourceCellCopyCountForTest(),
        static_cast<std::size_t>(terminal->rows) * terminal->columns);
}

TEST(testFrameSourceEdges)
{
    auto t = makeTerminal();
    feed(*t, "text");
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    TerminalSnapshotOptions options;

    // --- Degenerate sources are refused rather than half-captured. -----------
    {
        TerminalFrameInputs frame;
        Rendering::FrameSource empty;
        CHECK_MSG(!frame.capture(empty, options, unifontMetrics(), GlyphSlotGeometry {}), "no rows, no columns, no cells");
        CHECK_MSG(frame.inputs().cells == nullptr, "and the capture is left empty");

        Rendering::FrameSource noCells = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        noCells.cells = nullptr;
        CHECK_MSG(!frame.capture(noCells, options, unifontMetrics(), GlyphSlotGeometry {}), "a source with rows but no cells is refused");

        Rendering::FrameSource noAttributes = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        noAttributes.lineAttributes = nullptr;
        CHECK_MSG(!frame.capture(noAttributes, options, unifontMetrics(), GlyphSlotGeometry {}),
            "and so is one with no line attributes, which the cursor reads");
    }

    // --- Null rowMarks is the alternate screen, and reaches the builder as -----
    // --- an absence rather than as zeroes it would have to interpret. ---------
    {
        TerminalFrameInputs frame;
        Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        source.rowMarks = nullptr;
        CHECK(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
        CHECK_MSG(frame.inputs().promptMarks.rowMarks == nullptr, "no marks means no mark pass");
    }

    // --- A source may omit the cluster reader; the builder degrades. ----------
    //
    // Asserted because it is a real configuration rather than an oversight: a
    // frontend without a cluster pool copy has nothing to supply, and the
    // capture must carry the absence through instead of refusing the frame.
    {
        TerminalFrameInputs frame;
        Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        source.clusterResolver = nullptr;
        source.clusterResolverContext = nullptr;
        CHECK(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
        CHECK_MSG(frame.inputs().clusterResolver == nullptr, "the absence crosses the seam intact");
    }

    // --- The copy is the caller's release point. -----------------------------
    //
    // A frontend takes its lock again the moment capture returns, so the storage
    // it passed may be reused or freed. Overwriting it here and reading the
    // capture afterwards is what distinguishes a copy from a borrow; a borrow
    // would report the overwritten bytes and pass every other case in this file.
    {
        TerminalFrameInputs frame;
        Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        const std::uint32_t expected = cells[0].codePoint;
        CHECK(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
        for (auto& cell : cells)
            cell.codePoint = 0xFFFFu;
        CHECK_MSG(frame.inputs().cells[0].codePoint == expected,
            "the capture kept its own cells, so clobbering the source changed nothing");
        CHECK_MSG(expected != 0xFFFFu, "and the sentinel differs from the real code point");
    }

    // --- Chrome crosses both entry points identically. -----------------------
    //
    // The two overloads exist so a split-model frontend can capture without a
    // Terminal, and the claim is "two entry points, one implementation". Chrome
    // is where that claim is cheapest to break: the composite lives in one body,
    // and a parameter added to only the other one still COMPILES as long as the
    // bodies happen to sit in the same function. So this asserts the frames are
    // equal cell for cell, not that each drew something.
    //
    // A split that leaves chrome behind on the source overload is invisible to
    // every frontend test, because those all hold a Terminal.
    {
        CellCanvas popover;
        popover.resizeAndBlank(2, 6);
        Pen pen {};
        pen.foregroundIndex = colorRGB;
        pen.backgroundIndex = colorRGB;
        pen.backgroundRed = 0x40;
        fill(popover, GridRect { 0, 0, 6, 2 }, pen);
        drawText(popover, 0, 0, "beam", pen);
        const ChromePlacement layers[] = { ChromePlacement { &popover, 1, 2 } };

        TerminalFrameInputs viaTerminal;
        CHECK(viaTerminal.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, layers));
        const std::vector<Cell> fromTerminal(viaTerminal.inputs().cells,
            viaTerminal.inputs().cells + (static_cast<std::size_t>(t->rows) * t->columns));

        TerminalFrameInputs viaSource;
        Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
        CHECK(viaSource.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}, layers));

        bool identical = true;
        for (std::size_t i = 0; i < fromTerminal.size(); ++i)
            if (viaSource.inputs().cells[i].codePoint != fromTerminal[i].codePoint
                || viaSource.inputs().cells[i].pen.backgroundRed != fromTerminal[i].pen.backgroundRed)
                identical = false;
        CHECK_MSG(identical, "both entry points composited the same chrome the same way");

        // And it is really there — an equality that holds because neither drew
        // would pass the check above, which is the trap this file has hit before.
        TerminalFrameInputs plain;
        CHECK(plain.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
        bool differsFromPlain = false;
        for (std::size_t i = 0; i < fromTerminal.size(); ++i)
            if (plain.inputs().cells[i].codePoint != fromTerminal[i].codePoint)
                differsFromPlain = true;
        CHECK_MSG(differsFromPlain, "and chrome changed the frame at all");
    }

    t.reset();
}

TEST(testCaptureOwnsEveryBorrowedFrameFact)
{
    auto terminal = makeTerminal(2, 4);
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;
    FrameSource source = sourceFromTerminal(*terminal, cells, lineAttributes, rowMarks, 0);

    cells[0].codePoint = 'A';
    cells[0].setClusterID(7);
    struct ClusterSource {
        std::array<std::uint32_t, 3> codePoints { 'A', 0x200Du, 0x1F680u };
    } clusterSource;
    source.clusterResolver = [](const Cell*, std::span<std::uint32_t> output, void* context) {
        const auto& stored = static_cast<const ClusterSource*>(context)->codePoints;
        const std::size_t count = std::min(output.size(), stored.size());
        std::copy_n(stored.begin(), count, output.begin());
        return static_cast<int>(count);
    };
    source.clusterResolverContext = &clusterSource;

    std::array<unsigned char, 4> pixels { 0x11, 0x22, 0x33, 0x44 };
    std::array<Image::TerminalImage, Image::maxTerminalImageCount> images {};
    images[0].used = true;
    images[0].displayed = true;
    images[0].imageID = 41;
    images[0].width = 1;
    images[0].height = 1;
    images[0].format = 32;
    images[0].pixelLength = static_cast<std::uint32_t>(pixels.size());
    images[0].pixels = pixels.data();
    images[0].frameCount = 1;
    images[0].currentFrame = 0;
    images[0].contentGeneration = 9;
    images[0].frames[0] = { true, false, static_cast<std::uint32_t>(pixels.size()), pixels.data(), 40 };
    std::array<Image::TerminalImagePlacement, Image::maxTerminalImagePlacementCount> placements {};
    placements[0].used = true;
    placements[0].imageID = images[0].imageID;
    placements[0].cursorX = 2;
    source.images = images.data();
    source.placements = placements.data();

    terminal->palette.indexed[7].red = 0x1234;
    ++terminal->paletteEpoch;
    source.paletteGeneration = terminal->paletteEpoch;
    TextSizeSpec size;
    size.scale = 3;
    const unsigned sizeID = terminal->textSizeTable.intern(size);
    REQUIRE(sizeID != 0);

    std::array<SelectionRowSpan, 1> selection { SelectionRowSpan { 0, 0, 1 } };
    std::array<SelectionRowSpan, 1> search { SelectionRowSpan { 0, 1, 2 } };
    std::array<SelectionRowSpan, 1> otherSearch { SelectionRowSpan { 1, 0, 0 } };
    std::array<SelectionRowSpan, 1> trigger { SelectionRowSpan { 1, 1, 1 } };
    std::array<SelectionRowSpan, 1> hilite { SelectionRowSpan { 1, 2, 3 } };
    terminal->palette.indexedAlpha.fill(0xA5);
    std::array<std::uint8_t, 2> dirtyRows { 1, 0 };
    TerminalSnapshotOptions options;
    options.selectionSpans = selection.data();
    options.selectionSpanCount = static_cast<int>(selection.size());
    options.searchMatchSpans = search.data();
    options.searchMatchSpanCount = static_cast<int>(search.size());
    options.otherSearchMatchSpans = otherSearch.data();
    options.otherSearchMatchSpanCount = static_cast<int>(otherSearch.size());
    options.triggerHighlightSpans = trigger.data();
    options.triggerHighlightSpanCount = static_cast<int>(trigger.size());
    options.hiliteRegionSpans = hilite.data();
    options.hiliteRegionSpanCount = static_cast<int>(hilite.size());
    options.dirtyRows = dirtyRows.data();

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    ++images[0].contentGeneration;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    const BuildInputs& captured = frame.inputs();
    REQUIRE(captured.palette != nullptr);
    REQUIRE(captured.textSizeTable != nullptr);
    REQUIRE(captured.images != nullptr);
    REQUIRE(captured.placements != nullptr);
    REQUIRE(captured.clusterResolver != nullptr);

    cells[0].codePoint = 'Z';
    lineAttributes[0] = lineAttributeDoubleWidth;
    terminal->palette.indexed[7].red = 0xFFFF;
    terminal->textSizeTable.retainOnly(0);
    selection[0] = { 1, 3, 3 };
    search[0] = { 1, 3, 3 };
    otherSearch[0] = { 0, 3, 3 };
    trigger[0] = { 0, 3, 3 };
    hilite[0] = { 0, 3, 3 };
    terminal->palette.indexedAlpha.fill(0);
    dirtyRows.fill(0);
    pixels.fill(0);
    images[0].imageID = 99;
    placements[0].cursorX = 0;
    clusterSource.codePoints.fill('Z');

    CHECK(captured.cells[0].codePoint == static_cast<std::uint32_t>('A'));
    CHECK(captured.lineAttributes[0] != lineAttributeDoubleWidth);
    CHECK(captured.palette != &terminal->palette);
    CHECK(captured.palette->indexed[7].red == 0x1234);
    CHECK(frame.paletteGeneration() == source.paletteGeneration);
    CHECK(captured.textSizeTable != &terminal->textSizeTable);
    REQUIRE(captured.textSizeTable->lookup(sizeID) != nullptr);
    CHECK(captured.textSizeTable->lookup(sizeID)->scale == 3);
    CHECK(captured.selectionSpans[0].visibleRow == 0);
    CHECK(captured.searchMatchSpans[0].visibleRow == 0);
    CHECK(captured.otherSearchMatchSpans[0].visibleRow == 1);
    CHECK(captured.triggerHighlightSpans[0].visibleRow == 1);
    CHECK(captured.hiliteRegionSpans[0].visibleRow == 1);
    REQUIRE(captured.paletteForegroundAlpha != nullptr);
    CHECK(captured.paletteForegroundAlpha[7] == 0xA5);
    CHECK(captured.paletteForegroundAlpha == captured.palette->indexedAlpha.data());
    CHECK(captured.dirtyRows[0] == 1);
    CHECK(frame.capturedDirtyRows()[0] == 1);
    CHECK(captured.images != images.data());
    CHECK(captured.images[0].imageID == 41);
    CHECK(captured.images[0].pixels != pixels.data());
    CHECK(captured.images[0].pixels[0] == 0x11);
    CHECK(captured.placements != placements.data());
    CHECK(captured.placements[0].cursorX == 2);

    std::array<std::uint32_t, clusterPoolMaximumCodePoints> resolved {};
    const int resolvedCount = captured.clusterResolver(&captured.cells[0], resolved, captured.clusterResolverContext);
    CHECK(resolvedCount == 3);
    CHECK(resolved[0] == static_cast<std::uint32_t>('A'));
    CHECK(resolved[1] == 0x200D);
    CHECK(resolved[2] == 0x1F680);
    CHECK(frame.copyStatistics().cellBytesCopied == static_cast<std::size_t>(source.columns) * sizeof(Cell));
    CHECK(frame.copyStatistics().imageBytesCopied == pixels.size());
}

TEST(testUniformsDeriveTheSameOnBothSidesOfTheCapture)
{
    auto t = makeTerminal();
    feed(*t, "both sides of the copy");
    // In the mode, so edgeToEdge is the one field where the two sides are
    // meant to differ rather than one where they agree by both being zero.
    feed(*t, "\033]7402;fill=204060\033\\\033[?7402h");

    // Every fact the uniform fields read, set away from its default so a
    // field the capture forgot to copy cannot match by both sides being zero.
    TerminalSnapshotOptions options;
    options.viewportSizeInPixels = { 811.0f, 407.0f };
    options.originInPixels = { 37.0f, 23.0f };
    options.style.uniformBackgroundOpacity = 0.85f;
    options.style.uniformDefaultBackground = { 0.11f, 0.22f, 0.33f, 0.44f };
    // A rule-style override, so the capture's applyRuleStyle runs over metrics
    // this derivation was handed unstyled — the claim under test is that the
    // styling moves no field the uniforms read.
    options.style.underlineThicknessScale = 2.0f;
    options.style.underlineOffsetInPixels = 3.0f;

    const CellMetrics metrics = unifontMetrics();
    const FrameUniforms before = deriveFrameUniforms(options, metrics);

    TerminalFrameInputs frame;
    CHECK(frame.capture(*t, options, metrics, GlyphSlotGeometry {}));
    const FrameUniforms after = deriveFrameUniforms(frame.inputs());

    // edgeToEdge is the documented exception: it is a fact about the terminal,
    // and only the capture side reads a terminal. The options side leaves it
    // zero rather than guessing, so the assertion names the difference instead
    // of a memcmp reporting it as an unexplained byte. The origin follows it:
    // under the colour letterbox the frame has no padding, so the capture
    // zeroes whatever origin the options carried, and the options side keeps
    // the padded one it was handed.
    CHECK(after.edgeToEdge == 1);
    CHECK(before.edgeToEdge == 0);
    CHECK(after.originInPixels.x == 0.0f);
    CHECK(after.originInPixels.y == 0.0f);
    CHECK(before.originInPixels.x == 37.0f);
    CHECK(before.originInPixels.y == 23.0f);

    // Byte equality over the rest, not field-wise closeness: a caller digests
    // these bytes as a cache key, so agreement to a tolerance is still a
    // reclassified frame. Copied and normalised rather than compared field by
    // field, so a field added to FrameUniforms is covered without being listed.
    FrameUniforms beforeWithTerminalFacts = before;
    beforeWithTerminalFacts.edgeToEdge = after.edgeToEdge;
    beforeWithTerminalFacts.originInPixels = after.originInPixels;
    CHECK(std::memcmp(&beforeWithTerminalFacts, &after, sizeof(FrameUniforms)) == 0);
}

/// The mirror holds under DECSCNM too, which is where it is easiest to break.
///
/// Reverse video swaps the DEFAULT foreground and background, and the two
/// derivations reach that fact by different routes: the capture side carries the
/// palette and `screenReverse`, the options side carries neither. So a swap
/// applied where the palette happens to be in reach — the capture side — makes
/// the two disagree on exactly the reverse-video frames, and the disagreement
/// lands in a cache key, because the options overload exists for a caller that
/// digests these bytes before its capture runs.
///
/// The case above cannot see that: it never enters reverse video, so a one-sided
/// swap never fires and its byte comparison passes. This one enters the mode and
/// asks the same question.
TEST(testUniformsDeriveTheSameOnBothSidesUnderReverseVideo)
{
    auto t = makeTerminal();
    feed(*t, "reverse video, both sides");
    feed(*t, "\033[?5h"); // DECSCNM
    CHECK(t->modes.reverseVideo);

    // A default background distinguishable from the default foreground, so a
    // swap on one side moves bytes rather than exchanging equal ones.
    TerminalSnapshotOptions options;
    options.viewportSizeInPixels = { 640.0f, 384.0f };
    options.originInPixels = { 4.0f, 6.0f };
    options.style.uniformBackgroundOpacity = 0.9f;
    options.style.uniformDefaultBackground = { 0.05f, 0.10f, 0.15f, 0.95f };

    const CellMetrics metrics = unifontMetrics();
    const FrameUniforms before = deriveFrameUniforms(options, metrics);

    TerminalFrameInputs frame;
    CHECK(frame.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(frame.inputs().screenReverse); // the capture side really has the fact
    const FrameUniforms after = deriveFrameUniforms(frame.inputs());

    // edgeToEdge is the documented exception here as above, and this terminal is
    // not in that mode, so both sides read zero and the normalisation is a no-op
    // rather than a mask.
    CHECK(before.edgeToEdge == 0);
    CHECK(after.edgeToEdge == 0);
    CHECK(std::memcmp(&before, &after, sizeof(FrameUniforms)) == 0);
}

/// The letterbox COLOUR reaches the builder, not just the mode.
///
/// It travelled into FrameSource and stopped there — written by the capture,
/// read by nothing, while the only channel the builder had was one the caller
/// filled from the live terminal at draw time. A mode that arrives without its
/// colour paints the letterbox black on any frontend that has no second way to
/// ask.
TEST(testTheLetterboxColourReachesTheBuilder)
{
    auto t = makeTerminal();
    const CellMetrics metrics = unifontMetrics();
    TerminalSnapshotOptions options;

    TerminalFrameInputs plain;
    CHECK(plain.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(!plain.inputs().isGridOccluded); // the fixture, before the mode

    feed(*t, "\033]7402;fill=204060\033\\\033[?7402h");
    TerminalFrameInputs colored;
    CHECK(colored.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(colored.inputs().isGridOccluded);
    // 0x20/0x40/0x60 over 255, opaque — the alpha is the capture's, not the
    // sequence's, because a translucent letterbox shows the desktop the mode
    // exists to cover.
    CHECK(std::fabs(colored.inputs().occludingColor.x - (32.0f / 255.0f)) < 0.001f);
    CHECK(std::fabs(colored.inputs().occludingColor.y - (64.0f / 255.0f)) < 0.001f);
    CHECK(std::fabs(colored.inputs().occludingColor.z - (96.0f / 255.0f)) < 0.001f);
    CHECK(std::fabs(colored.inputs().occludingColor.w - (1.0f)) < 0.001f);

    // Passthrough keeps the grid padded, so nothing occludes it whatever the
    // fill was — the two facts stay one decision.
    feed(*t, "\033]7402;fill=pass\033\\");
    TerminalFrameInputs passthrough;
    CHECK(passthrough.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(!passthrough.inputs().isGridOccluded);

    t.reset();
}

TEST(testCaptureResolvesFullscreenContentFromTheTerminal)
{
    // The mode alone does not decide the frame's geometry: Passthrough leaves
    // the terminal content visible around a fitted image, so the grid stays
    // padded under the same mode bit that sends Color edge to edge. A capture
    // carrying the mode without the letterbox selection would take half of
    // what decides it.
    auto t = makeTerminal();
    const CellMetrics metrics = unifontMetrics();
    TerminalSnapshotOptions options;

    TerminalFrameInputs plain;
    CHECK(plain.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(!plain.inputs().isEdgeToEdge); // the fixture, before the mode

    feed(*t, "\033]7402;fill=204060\033\\\033[?7402h");
    TerminalFrameInputs colored;
    CHECK(colored.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(colored.inputs().isEdgeToEdge);

    feed(*t, "\033]7402;fill=pass\033\\");
    TerminalFrameInputs passthrough;
    CHECK(passthrough.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(!passthrough.inputs().isEdgeToEdge); // same mode bit, padded grid

    // Resetting the mode with the letterbox still Color: the mode is the other
    // half, so neither one alone can hold the frame edge to edge.
    feed(*t, "\033]7402;fill=204060\033\\\033[?7402l");
    TerminalFrameInputs modeOff;
    CHECK(modeOff.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(!modeOff.inputs().isEdgeToEdge);

    t.reset();
}

TEST(testCaptureDropsTheOriginUnderTheColourLetterbox)
{
    // The frontend reports where its padding puts the grid; whether the frame
    // keeps that padding is Core's to decide, beside the letterbox, so an arm
    // that registers its origin ahead of the capture cannot draw the fill to
    // the drawable's edge and the cells one padding in.
    auto t = makeTerminal();
    const CellMetrics metrics = unifontMetrics();
    TerminalSnapshotOptions options;
    options.originInPixels = { 16.0f, 24.0f };

    TerminalFrameInputs padded;
    CHECK(padded.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(padded.inputs().originInPixels.x == 16.0f);
    CHECK(padded.inputs().originInPixels.y == 24.0f);

    feed(*t, "\033]7402;fill=204060\033\\\033[?7402h");
    CHECK(isFrameEdgeToEdge(*t));
    TerminalFrameInputs edgeToEdge;
    CHECK(edgeToEdge.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(edgeToEdge.inputs().originInPixels.x == 0.0f);
    CHECK(edgeToEdge.inputs().originInPixels.y == 0.0f);
    // The uniforms every pass reads derive from the same inputs, so the glyph
    // quads and the letterbox agree about where the grid starts.
    CHECK(deriveFrameUniforms(edgeToEdge.inputs()).originInPixels.x == 0.0f);

    // Passthrough keeps the padding under the same mode bit: the terminal
    // content stays visible around the fitted image, where the user put it.
    feed(*t, "\033]7402;fill=pass\033\\");
    CHECK(!isFrameEdgeToEdge(*t));
    TerminalFrameInputs passthrough;
    CHECK(passthrough.capture(*t, options, metrics, GlyphSlotGeometry {}));
    CHECK(passthrough.inputs().originInPixels.x == 16.0f);
    CHECK(passthrough.inputs().originInPixels.y == 24.0f);

    t.reset();
}

TEST(testMarkGeometrySurvivesAHiddenCursor)
{
    // cellHeightPoints is the point-space line height, and two passes read it:
    // the cursor, which needs it to size a bar or an underline, and the OSC 133
    // mark pass, which scales its brackets by it. Deriving it alongside the
    // cursor made the brackets depend on whether a cursor was drawn.
    //
    // Every fullscreen program hides the cursor while it paints, so this is the
    // ordinary case rather than an edge: a shell with prompt marks running vim
    // draws its marks on frames with no cursor.
    auto t = makeTerminal();
    feed(*t, "prompt\r\n");
    t->screens.active->mark[0] = Ubiquitty::Screen::osc133MarkCmdExecuted;

    TerminalSnapshotOptions shown;
    shown.drawCursor = true;
    shown.cellHeightPoints = 17.0;
    shown.lineSpacingPoints = 2.0;
    shown.promptMarkFontSize = 12;
    shown.promptMarkFontAscent = 10.0f;
    shown.promptMarkFontDescent = 3.0f;
    shown.promptMarkAtlasScale = 2.0f;

    TerminalFrameInputs withCursor;
    CHECK(withCursor.capture(*t, shown, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(withCursor.inputs().cursor.row >= 0, "the cursor is drawn in this arm");
    const double heightWithCursor = withCursor.inputs().cellHeightPoints;

    // DECTCEM off. The mark pass must not notice.
    TerminalSnapshotOptions hidden = shown;
    hidden.drawCursor = false;
    TerminalFrameInputs withoutCursor;
    CHECK(withoutCursor.capture(*t, hidden, unifontMetrics(), GlyphSlotGeometry {}));

    CHECK_MSG(heightWithCursor == shown.cellHeightPoints, "the arm with a cursor carries the height");
    CHECK_MSG(withoutCursor.inputs().cellHeightPoints == heightWithCursor,
        "and hiding the cursor does not take the line height with it");
    CHECK_MSG(withoutCursor.inputs().lineSpacingPoints == withCursor.inputs().lineSpacingPoints,
        "nor the line spacing");
    // Guards the comparison: a zero on both sides would satisfy the equality
    // above while proving the geometry never arrived at all.
    CHECK_MSG(withoutCursor.inputs().cellHeightPoints > 0.0,
        "and the height is real rather than zero on both sides");

    t.reset();
}

// --- The damage-gated copy ---------------------------------------------------
//
// options.dirtyRows already tells the builder which rows to rebuild; the copy
// honours the same mask, keeping a clean row's cells from the previous
// capture. Three legs, a case each, because each fails alone: a damaged row is
// fresh, a lineage break copies whole, and a row this object composited over
// is restored.

TEST(testDamagedRowsAreFreshAndCleanRowsAreSkipped)
{
    auto t = makeTerminal();
    feed(*t, "aaaa\r\nbbbb\r\ncccc");
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    TerminalSnapshotOptions options;
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 0);
    options.dirtyRows = damage.data();

    // The mask is present but nothing has been captured, so the first copy is
    // whole.
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[0].codePoint), static_cast<long long>('a'));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[2 * t->columns].codePoint), static_cast<long long>('c'));

    // Row 0 damaged and changed: the capture shows the change. Row 2 changed
    // but clean: the capture keeps the previous frame's cells. The stale read
    // is this case's proof that the gate gated — the builder never reads a
    // clean row, so no caller may rely on what this asserts.
    cells[0].codePoint = 'x';
    cells[2 * static_cast<std::size_t>(t->columns)].codePoint = 'y';
    damage[0] = 1;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[0].codePoint), static_cast<long long>('x'));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[2 * t->columns].codePoint), static_cast<long long>('c'));

    // A null mask on the next frame copies whole and picks the change up.
    options.dirtyRows = nullptr;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[2 * t->columns].codePoint), static_cast<long long>('y'));
}

TEST(testHoverMoveDamagesBothRetainedLinkRows)
{
    // Hover is presentation state owned by the frontend, not a Terminal write.
    // A clean Core damage mask must still rebuild the old and new link rows, or
    // the retained grid keeps the first underline and never paints the next.
    auto t = makeTerminal(3, 8);
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 1);
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    options.dirtyRows = damage.data();

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    // The first hover appears with no Terminal damage.
    std::ranges::fill(damage, std::uint8_t { 0 });
    options.isHoverLinkActive = true;
    options.hoverLinkRowFirst = 0;
    options.hoverLinkRowLast = 0;
    options.hoverLinkColumnFirst = 1;
    options.hoverLinkColumnLast = 2;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().dirtyRows[0]);
    CHECK(!frame.inputs().dirtyRows[1]);
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[1]), static_cast<int>(LinkState::Hovered));

    // Moving to another row must clear the old presentation and emit the new
    // one, despite the Core mask staying entirely clean.
    options.hoverLinkRowFirst = 1;
    options.hoverLinkRowLast = 1;
    options.hoverLinkColumnFirst = 4;
    options.hoverLinkColumnLast = 5;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().dirtyRows[0]);
    CHECK(frame.inputs().dirtyRows[1]);
    CHECK(!frame.inputs().dirtyRows[2]);
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[1]), static_cast<int>(LinkState::None));
    const std::size_t secondRow = static_cast<std::size_t>(t->columns);
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[secondRow + 4]), static_cast<int>(LinkState::Hovered));
}

TEST(testAHoverIsDrawnOnlyOverTheTextItWasResolvedOn)
{
    // The frontend resolves a run against the model, and output can rewrite
    // the cells under it before a frame draws. The digest taken at the resolve
    // is checked against this frame's cells, so a stale run draws nothing.
    auto t = makeTerminal(3, 12);
    feed(*t, "a link here");
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 1);
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    options.dirtyRows = damage.data();
    options.isHoverLinkActive = true;
    options.hoverLinkRowFirst = 0;
    options.hoverLinkRowLast = 0;
    options.hoverLinkColumnFirst = 2;
    options.hoverLinkColumnLast = 5;
    const auto codePointAt = [&](int row, int column) {
        return cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(t->columns) + static_cast<std::size_t>(column)].codePoint;
    };
    options.hoverLinkTextDigest = linkRunTextDigest(0, 0, 2, 5, 0, -1, t->columns, codePointAt);

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[2]), static_cast<int>(LinkState::Hovered));

    // The text under the run changes; the run's digest does not.
    feed(*t, "\033[1;3Hlost");
    source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[2]), static_cast<int>(LinkState::None));

    // A run resolved without a digest is drawn by its rows alone.
    options.hoverLinkTextDigest = 0;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[2]), static_cast<int>(LinkState::Hovered));
}

TEST(testAHoverMarkedRowCarriesThisFramesCells)
{
    // The case above pins that the hover MARKS its rows. This pins the other
    // half: a row it marks has to be refreshed too.
    //
    // The two damage sets are different questions. Core's says which rows the
    // terminal wrote; the hover adds rows the terminal did not, which is the
    // whole point of adding them. So a row the hover contributes is by
    // definition one the copy would skip if the copy consulted Core's set
    // alone — and the capture would then rebuild it from whatever the previous
    // frame left in that slot.
    //
    // On a still grid those are the same bytes and nothing shows. On a
    // SCROLLING one they are a different line: against a build appending
    // output, the hovered link's row draws the line above it and the row below
    // draws twice, healing only on a scroll — which drops the lineage and
    // copies whole.
    auto t = makeTerminal(4, 12);
    feed(*t, "one\r\ntwo\r\nthree\r\nfour");

    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 1);
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    options.dirtyRows = damage.data();

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    // One more line, so every visible row now shows the line below the one it
    // showed. A scroll reports only the freshly exposed row as damaged — the
    // rest move by a blit the renderer performs, which is exactly why the
    // capture is allowed to leave their cells alone.
    feed(*t, "\r\nfive");
    source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::ranges::fill(damage, std::uint8_t { 0 });
    damage[static_cast<std::size_t>(t->rows) - 1] = 1;

    // The pointer lands on a row the scroll did not damage. It held "two" a
    // frame ago and holds "three" now.
    const int hoveredRow = 1;
    options.isHoverLinkActive = true;
    options.hoverLinkRowFirst = hoveredRow;
    options.hoverLinkRowLast = hoveredRow;
    options.hoverLinkColumnFirst = 0;
    options.hoverLinkColumnLast = 4;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    REQUIRE(frame.inputs().dirtyRows != nullptr);
    CHECK_MSG(frame.inputs().dirtyRows[hoveredRow] != 0,
        "the hover must still mark its row — without that the rest of this case is vacuous");
    const std::size_t rowStart = static_cast<std::size_t>(hoveredRow) * t->columns;
    for (int column = 0; column < t->columns; ++column) {
        const std::size_t at = rowStart + static_cast<std::size_t>(column);
        char message[160];
        std::snprintf(message, sizeof(message),
            "row %d column %d was rebuilt from the previous frame's cells: wanted U+%04X, got U+%04X",
            hoveredRow, column, source.cells[at].codePoint, frame.inputs().cells[at].codePoint);
        CHECK_MSG(frame.inputs().cells[at].codePoint == source.cells[at].codePoint, message);
    }
}

TEST(testTerminalHoverRefreshCopiesRowsAddedByPresentationDamage)
{
    auto terminal = makeTerminal(4, 12);
    feed(*terminal, "\033[?1049hone\r\ntwo\r\nthree\r\nfour");
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));

    feed(*terminal, "\033[Hthree\r\nfour\r\nfive\r\nsix");
    std::array<std::uint8_t, 4> damage { 0, 0, 0, 1 };
    CapturedRowRefresh refresh;
    refresh.canReuseRows = true;
    refresh.rowDirty = damage;
    options.isHoverLinkActive = true;
    options.hoverLinkRowFirst = 1;
    options.hoverLinkRowLast = 1;
    options.hoverLinkColumnFirst = 0;
    options.hoverLinkColumnLast = 3;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    REQUIRE(frame.inputs().dirtyRows != nullptr);
    CHECK(frame.inputs().dirtyRows[1] != 0);
    CHECK_MSG(frame.inputs().cells[12].codePoint == 'f', "a hover-added row copies the current terminal row before building");
    CHECK_MSG(frame.inputs().cells[0].codePoint == 'o', "unaffected clean rows retain their cached cells");

    feed(*terminal, "\033[2;1Hnew!");
    options.isHoverLinkActive = false;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK_MSG(frame.inputs().cells[12].codePoint == 'n', "ending hover refreshes the previously highlighted row");
}

TEST(testShapeAndViewOffsetChangesCopyWhole)
{
    auto t = makeTerminal();
    feed(*t, "top\r\n");
    for (int i = 0; i < 8; ++i)
        feed(*t, "scroll\r\n");
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    TerminalSnapshotOptions options;
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 0);
    options.dirtyRows = damage.data();

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    // The view offset moved with the same mask: every row must be re-read,
    // because a clean row now shows different absolute content. The re-read
    // row 0 is scrollback the offset scrolled into view.
    std::vector<Cell> offsetCells;
    Rendering::FrameSource offsetSource = sourceFromTerminal(*t, offsetCells, lineAttributes, rowMarks, 2);
    options.viewOffset = 2;
    REQUIRE(frame.capture(offsetSource, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[0].codePoint),
        static_cast<long long>(offsetCells[0].codePoint));

    // A resize with the same mask: the shape check refuses the partial copy.
    Ubiquitty::resize(*t, t->rows, t->columns + 4);
    std::vector<Cell> widerCells;
    Rendering::FrameSource wider = sourceFromTerminal(*t, widerCells, lineAttributes, rowMarks, 0);
    options.viewOffset = 0;
    damage.assign(static_cast<std::size_t>(t->rows), 0);
    REQUIRE(frame.capture(wider, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().columns), static_cast<long long>(t->columns));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[0].codePoint),
        static_cast<long long>(widerCells[0].codePoint));
}

TEST(testStatusRowDeactivationRestoresTheGridRow)
{
    auto t = makeTerminal();
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    const std::size_t lastRowBase = static_cast<std::size_t>(t->rows - 1) * t->columns;
    cells[lastRowBase].codePoint = 'g';

    std::vector<Cell> statusCells(static_cast<std::size_t>(t->columns), defaultBlankCell);
    statusCells[0].codePoint = 's';
    TerminalSnapshotOptions options;
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 0);
    options.dirtyRows = damage.data();
    options.statusRowCells = statusCells.data();
    options.statusRowColumns = t->columns;

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[lastRowBase].codePoint), static_cast<long long>('s'));

    // The status line deactivates with the row still clean: the capture must
    // restore the grid content the composite covered, or the dead status line
    // stays on screen until something else damages the row.
    options.statusRowCells = nullptr;
    options.statusRowColumns = 0;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[lastRowBase].codePoint), static_cast<long long>('g'));
}

TEST(testAVanishedStatusRowIsNamedInTheCapturedDamage)
{
    auto t = makeTerminal();
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    const std::size_t lastRowBase = static_cast<std::size_t>(t->rows - 1) * t->columns;
    cells[lastRowBase].codePoint = 'g';

    std::vector<Cell> statusCells(static_cast<std::size_t>(t->columns), defaultBlankCell);
    statusCells[0].codePoint = 's';
    TerminalSnapshotOptions options;
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 0);
    options.dirtyRows = damage.data();
    options.statusRowCells = statusCells.data();
    options.statusRowColumns = t->columns;

    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[lastRowBase].codePoint), static_cast<long long>('s'));

    // The status line deactivates with every Core row clean. The capture copies
    // the grid row the composite covered back, so the mask it publishes has to
    // name that row: a retained frame redraws only the rows the mask names, and
    // a row copied but unnamed leaves the dead status line on the glass.
    options.statusRowCells = nullptr;
    options.statusRowColumns = 0;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<long long>(frame.inputs().cells[lastRowBase].codePoint), static_cast<long long>('g'));
    // An empty mask would mean the capture claimed no partial lineage, which
    // would make the row assertion above vacuous rather than passing.
    REQUIRE(frame.capturedDirtyRows().size() == static_cast<std::size_t>(t->rows));
    CHECK(frame.capturedDirtyRows()[static_cast<std::size_t>(t->rows - 1)] == 1);
}

// OSC 17 sets the selection-background colour and Core stores it
// (Terminal::selectionBackground*), and the capture fills the overlay tint from
// that state: a tint taken from the hardcoded selectionHighlightColor() leaves
// the host colour inert on every frontend. The rule divides the 16-bit channels
// by 65535 and keeps the built-in alpha — a wholesale replace that also drove
// alpha to 1.0 would hide the glyphs under the selection.
TEST(testSelectionTintCarriesTheHostOSC17ColourKeepingTheBuiltInAlpha)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const SelectionRowSpan span { 0, 0, 4 };
    options.style.selectionStyle = SelectionStyle::Tint;
    options.selectionSpans = &span;
    options.selectionSpanCount = 1;

    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-3f; };
    const OverlayColor builtIn = selectionHighlightColor();

    // With no host colour the built-in slate tint stands — this also proves the
    // span reaches the tint branch, so the control below tests a real path.
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 fallback = frame.inputs().selectionColor;
    CHECK_MSG(near(fallback.x, builtIn.red) && near(fallback.w, builtIn.alpha),
        "no host colour leaves the built-in tint");

    // OSC 17 ; rgb:ffff/0000/0000 — a pure red the slate default cannot mimic.
    t->selectionBackgroundRed = 0xffff;
    t->selectionBackgroundGreen = 0x0000;
    t->selectionBackgroundBlue = 0x0000;
    t->selectionBackgroundSet = true;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 tint = frame.inputs().selectionColor;
    CHECK_MSG(near(tint.x, 1.0f), "the host's red reaches the tint (65535 / 65535)");
    CHECK_MSG(near(tint.y, 0.0f), "the host's green reaches the tint");
    CHECK_MSG(near(tint.z, 0.0f), "the host's blue reaches the tint");
    CHECK_MSG(near(tint.w, builtIn.alpha),
        "the built-in alpha survives — OSC 17 names a colour, not an opacity");
}

// The native style draws the platform's colour under the text, and a host's
// OSC 17 colour takes that colour's place without changing where it is drawn.
// A platform that reports no colour has nothing native to draw, so the style
// falls back to the tint rather than to an opaque fill of a colour nobody chose.
TEST(testNativeSelectionFillsBehindTheTextInThePlatformOrHostColour)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.style.selectionStyle = SelectionStyle::Native;
    const SelectionRowSpan span { 0, 0, 4 };
    options.selectionSpans = &span;
    options.selectionSpanCount = 1;
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-3f; };

    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(!frame.inputs().isSelectionBehindText, "no platform colour falls back to the tint");
    CHECK(near(frame.inputs().selectionColor.w, selectionHighlightColor().alpha));

    options.style.platformSelectionColor = { 0.7f, 0.84f, 1.0f, 1.0f };
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().isSelectionBehindText);
    CHECK(near(frame.inputs().selectionColor.x, 0.7f) && near(frame.inputs().selectionColor.z, 1.0f));
    CHECK_MSG(near(frame.inputs().selectionColor.w, nativeSelectionOpacity), "a fill behind the text is the native wash over the cell's own background");

    t->selectionBackgroundRed = 0xffff;
    t->selectionBackgroundGreen = 0x0000;
    t->selectionBackgroundBlue = 0x0000;
    t->selectionBackgroundSet = true;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().isSelectionBehindText);
    CHECK_MSG(near(frame.inputs().selectionColor.x, 1.0f) && near(frame.inputs().selectionColor.z, 0.0f),
        "the host colour wins over the platform's");

    // The tint style ignores the platform colour whatever it reports.
    options.style.selectionStyle = SelectionStyle::Tint;
    t->selectionBackgroundSet = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().isSelectionBehindText);
    CHECK(near(frame.inputs().selectionColor.x, selectionHighlightColor().red));
}

// A selection behind the text is in the cells, so moving it changes pixels
// without changing a cell byte. Under a retained target and a clean model mask,
// the rows it leaves and the rows it reaches must still be rebuilt, and the rows
// it covers the same way on both frames must not: a drag that grows by a row
// repaints the rows that changed, not the whole selection.
TEST(testABakedSelectionDamagesTheRowsItReachesAndLeaves)
{
    auto t = makeTerminal(4, 8);
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 1);
    TerminalSnapshotOptions options;
    options.style.selectionStyle = SelectionStyle::Native;
    options.style.platformSelectionColor = { 0.7f, 0.84f, 1.0f, 1.0f };
    options.dirtyRows = damage.data();

    const std::array<SelectionRowSpan, 2> first { SelectionRowSpan { 0, 2, 7 }, SelectionRowSpan { 1, 0, 3 } };
    options.selectionSpans = first.data();
    options.selectionSpanCount = 2;
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    // Unchanged: nothing is owed.
    std::ranges::fill(damage, std::uint8_t { 0 });
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    for (int row = 0; row < t->rows; ++row)
        CHECK(!frame.inputs().dirtyRows[row]);

    // The drag grows through row 1 onto row 2; row 0 is covered the same.
    const std::array<SelectionRowSpan, 3> grown { SelectionRowSpan { 0, 2, 7 }, SelectionRowSpan { 1, 0, 7 },
        SelectionRowSpan { 2, 0, 1 } };
    options.selectionSpans = grown.data();
    options.selectionSpanCount = 3;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().dirtyRows[0]);
    CHECK(frame.inputs().dirtyRows[1]);
    CHECK(frame.inputs().dirtyRows[2]);
    CHECK(!frame.inputs().dirtyRows[3]);

    // A drag moving a letter inside one cell of a proportional run changes
    // the span's cut and no column: the row is owed, since the plate it
    // bakes moved.
    const std::array<SelectionRowSpan, 3> cut { SelectionRowSpan { 0, 2, 7 }, SelectionRowSpan { 1, 0, 7 },
        SelectionRowSpan { 2, 0, 1, 0, 2 } };
    options.selectionSpans = cut.data();
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(!frame.inputs().dirtyRows[0] && !frame.inputs().dirtyRows[1]);
    CHECK_MSG(frame.inputs().dirtyRows[2], "a cut that moved inside a cell owes its row");
    CHECK(!frame.inputs().dirtyRows[3]);
    options.selectionSpans = grown.data();
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().dirtyRows[2]);

    // A recolour, as a window losing focus does, owes every selected row.
    options.style.platformSelectionColor = { 0.86f, 0.86f, 0.86f, 1.0f };
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().dirtyRows[0] && frame.inputs().dirtyRows[1] && frame.inputs().dirtyRows[2]);
    CHECK(!frame.inputs().dirtyRows[3]);

    // Cleared: every row it covered is owed its own background back.
    options.selectionSpans = nullptr;
    options.selectionSpanCount = 0;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().dirtyRows[0] && frame.inputs().dirtyRows[1] && frame.inputs().dirtyRows[2]);
    CHECK(!frame.inputs().dirtyRows[3]);

    // A tinted selection is an overlay rebuilt every frame and owes nothing.
    options.style.selectionStyle = SelectionStyle::Tint;
    options.selectionSpans = grown.data();
    options.selectionSpanCount = 3;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    for (int row = 0; row < t->rows; ++row)
        CHECK(!frame.inputs().dirtyRows[row]);
}

// A row held in two pieces keeps both in the diff. Judged by one piece of the
// row, a change to the other leaves the row looking unchanged, and its baked
// cells stay as the last frame drew them.
TEST(testABakedSelectionRowInTwoPiecesIsDamagedByEitherPiece)
{
    auto t = makeTerminal(4, 12);
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes, rowMarks;
    Rendering::FrameSource source = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    std::vector<std::uint8_t> damage(static_cast<std::size_t>(t->rows), 1);
    TerminalSnapshotOptions options;
    options.style.selectionStyle = SelectionStyle::Native;
    options.style.platformSelectionColor = { 0.7f, 0.84f, 1.0f, 1.0f };
    options.dirtyRows = damage.data();

    const std::array<SelectionRowSpan, 3> first { SelectionRowSpan { 0, 4, 11 }, SelectionRowSpan { 1, 0, 11 },
        SelectionRowSpan { 0, 0, 1 } };
    options.selectionSpans = first.data();
    options.selectionSpanCount = 3;
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));

    std::ranges::fill(damage, std::uint8_t { 0 });
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    for (int row = 0; row < t->rows; ++row)
        CHECK_MSG(!frame.inputs().dirtyRows[row], "the same pieces owe nothing");

    // The piece listed first on the row moves; the one listed last does not.
    const std::array<SelectionRowSpan, 3> secondPieceMoved { SelectionRowSpan { 0, 4, 11 }, SelectionRowSpan { 1, 0, 11 },
        SelectionRowSpan { 0, 0, 2 } };
    options.selectionSpans = secondPieceMoved.data();
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().dirtyRows[0], "a change to the later piece owes the row");
    CHECK(!frame.inputs().dirtyRows[1]);

    const std::array<SelectionRowSpan, 3> firstPieceMoved { SelectionRowSpan { 0, 5, 11 }, SelectionRowSpan { 1, 0, 11 },
        SelectionRowSpan { 0, 0, 2 } };
    options.selectionSpans = firstPieceMoved.data();
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().dirtyRows[0], "a change to the earlier piece owes the row");
    CHECK(!frame.inputs().dirtyRows[1]);

    // The same pieces in another order are the same selection.
    const std::array<SelectionRowSpan, 3> reordered { SelectionRowSpan { 0, 0, 2 }, SelectionRowSpan { 0, 5, 11 },
        SelectionRowSpan { 1, 0, 11 } };
    options.selectionSpans = reordered.data();
    REQUIRE(frame.capture(source, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(!frame.inputs().dirtyRows[0] && !frame.inputs().dirtyRows[1], "the pieces are compared as a set");
}

// OSC 19 is the foreground half of the same feature, and it reached the capture
// later: filling the override from `options` alone answers with the frontend's
// own state, so a host colour stored on the terminal was drawn by the macOS walk
// and by nothing the capture fed. The rule here is the sibling's — the host's
// channels win over the frontend's, carried at /65535 — with the alpha reasoned
// the other way: a selection background is a wash over text and keeps the
// built-in's alpha, while a foreground is the ink and has nothing behind it to
// show through, so it is opaque.
//
// The low-byte pair is the assertion that matters. /65535 is exactly where an
// 8-bit narrowing hides: 0x7fff and 0x7f00 are one value once the low byte is
// dropped, so a whole-channel comparison passes on a narrowed path and only a
// pair that differs below the high byte can see it. Same shape as the
// narrowing family the bounds gates cover.
TEST(testSelectionForegroundCarriesTheHostOSC19ColourAtFullPrecision)
{
    auto t = makeTerminal();
    feed(*t, "hello");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const SelectionRowSpan span { 0, 0, 4 };
    options.selectionSpans = &span;
    options.selectionSpanCount = 1;

    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-3f; };

    // With no host colour the frontend's own value stands, which also proves the
    // assertions below turn on the host state rather than on a constant.
    options.shouldOverrideSelectionForeground = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(!frame.inputs().selectionForegroundOverride,
        "no host colour and no frontend override leaves the override off");

    // A pure-red host OSC 19 activates the override and reaches the tint.
    t->selectionForegroundRed = 0xffff;
    t->selectionForegroundGreen = 0x0000;
    t->selectionForegroundBlue = 0x0000;
    t->selectionForegroundSet = true;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_MSG(frame.inputs().selectionForegroundOverride,
        "a host OSC 19 activates the selection-foreground override");
    const Vec4 fg = frame.inputs().selectionForeground;
    CHECK_MSG(near(fg.x, 1.0f) && near(fg.y, 0.0f) && near(fg.z, 0.0f),
        "the host's foreground colour reaches the tint (65535 / 65535)");
    CHECK_MSG(near(fg.w, 1.0f), "the ink is opaque — a foreground names no opacity");

    // The low byte must survive: an 8-bit narrowing collapses these two.
    t->selectionForegroundRed = 0x7fff;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const float high = frame.inputs().selectionForeground.x;
    t->selectionForegroundRed = 0x7f00;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const float low = frame.inputs().selectionForeground.x;
    CHECK_MSG(high > low,
        "0x7fff and 0x7f00 differ — the channel is carried at 16 bits, not narrowed to 8");
}

// The control above drives the Terminal overload; every frontend drives this
// one, off its own copy of the grid, and the two reach the tint through
// different arguments. A source reports what the host asked for and the shared
// capture decides what the highlight becomes, so the three cases here are the
// three ways a frontend can get that wrong: report nothing, report a colour, or
// hold the state on its terminal and forget to carry it across.
TEST(testSelectionTintReadsTheHostColourFromTheFrameSourceOverload)
{
    auto t = makeTerminal();
    feed(*t, "hello");
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    std::vector<std::uint8_t> rowMarks;

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const SelectionRowSpan span { 0, 0, 4 };
    options.style.selectionStyle = SelectionStyle::Tint;
    options.selectionSpans = &span;
    options.selectionSpanCount = 1;

    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-3f; };
    const OverlayColor builtIn = selectionHighlightColor();

    Rendering::FrameSource silent = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    CHECK(frame.capture(silent, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 fallback = frame.inputs().selectionColor;
    CHECK_MSG(near(fallback.x, builtIn.red) && near(fallback.w, builtIn.alpha),
        "a source naming no colour leaves the built-in tint");

    // OSC 17 ; rgb:0000/ffff/0000 — green here where the Terminal-overload
    // control uses red, so neither case can pass on the other's value.
    Rendering::FrameSource named = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    named.hasSelectionBackground = true;
    named.selectionBackgroundRed = 0x0000;
    named.selectionBackgroundGreen = 0xffff;
    named.selectionBackgroundBlue = 0x0000;
    CHECK(frame.capture(named, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 tint = frame.inputs().selectionColor;
    CHECK_MSG(near(tint.x, 0.0f) && near(tint.y, 1.0f) && near(tint.z, 0.0f),
        "the colour the source reports reaches the tint");
    CHECK_MSG(near(tint.w, builtIn.alpha), "the built-in alpha survives this overload too");

    // Half the low byte is the trap the walk's rule was written against: at
    // 8-bit precision 0x7fff and 0x7f00 land on the same channel, so a source
    // that narrowed on the way through would pass every whole-channel case above
    // and lose exactly this one.
    Rendering::FrameSource halfLit = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    halfLit.hasSelectionBackground = true;
    halfLit.selectionBackgroundRed = 0x7fff;
    halfLit.selectionBackgroundGreen = 0x7f00;
    halfLit.selectionBackgroundBlue = 0x0000;
    CHECK(frame.capture(halfLit, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 sixteenBit = frame.inputs().selectionColor;
    CHECK_MSG(sixteenBit.x > sixteenBit.y,
        "the low byte survives — 0x7fff and 0x7f00 are different reds");

    // A frontend building a source from a terminal has to carry the state over,
    // and the helper above stands in for one that does.
    t->selectionBackgroundRed = 0x0000;
    t->selectionBackgroundGreen = 0x0000;
    t->selectionBackgroundBlue = 0xffff;
    t->selectionBackgroundSet = true;
    Rendering::FrameSource carried = sourceFromTerminal(*t, cells, lineAttributes, rowMarks, 0);
    CHECK(frame.capture(carried, options, unifontMetrics(), GlyphSlotGeometry {}));
    const Vec4 fromTerminal = frame.inputs().selectionColor;
    CHECK_MSG(near(fromTerminal.z, 1.0f) && near(fromTerminal.x, 0.0f),
        "a source built from a terminal carries the host colour across");
}

TEST(testAPartialRefreshKeepsTheRowsItWasToldAreClean)
{
    auto t = makeTerminal();
    feed(*t, "first\r\nsecond\r\n");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK_MSG(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}),
        "the full capture the partial one is measured against");
    CHECK(frame.inputs().cells[0].codePoint == 'f');
    CHECK(frame.inputs().cells[static_cast<std::size_t>(t->columns)].codePoint == 's');

    // Both rows are rewritten in the terminal, and only row 1 is named dirty.
    // Row 0 must come back holding the OLD text — that staleness is the whole
    // mechanism, not a defect: the caller keeps a row because the drain proved
    // its pixels are already on the target.
    feed(*t, "\033[H");
    feed(*t, "ZZZZZ\r\nYYYYYY");
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    rowDirty[1] = 1;
    CapturedRowRefresh refresh;
    refresh.canReuseRows = true;
    refresh.rowDirty = std::span<const std::uint8_t> { rowDirty };
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK_MSG(frame.inputs().cells[0].codePoint == 'f',
        "row 0 was named clean, so its cells are the ones the last capture read");
    CHECK_MSG(frame.inputs().cells[static_cast<std::size_t>(t->columns)].codePoint == 'Y',
        "row 1 was named dirty, so it was re-read");

    // The same terminal and the same dirty set, with the reuse verdict withdrawn:
    // every row is re-read, which is the full capture the other overload passes.
    CapturedRowRefresh full = refresh;
    full.canReuseRows = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, full));
    CHECK_MSG(frame.inputs().cells[0].codePoint == 'Z',
        "with no reuse verdict the clean row is re-read too");
}

TEST(testLinkFlagsFollowTheRowsAScrollMoves)
{
    // The link flags are one byte a cell beside the cells, and the scan that
    // fills them follows the damage the drain published. A scroll moves a
    // clean row's cells, so the flags have to move with them; a refresh the
    // capture demotes re-reads every row, so its damage set may not be trusted
    // at all. Left alone either way, a row underlines a link that scrolled off
    // it.
    auto t = makeTerminal(4, 12);
    feed(*t, "\033]8;;https://example.com\033\\link\033]8;;\033\\");
    feed(*t, "\r\nplain\r\nplain");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const std::size_t width = static_cast<std::size_t>(t->columns);
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[0]), static_cast<int>(LinkState::Resting));

    // The screen scrolls up by one, which is the case that reaches a reader:
    // the link leaves row 0 and only the row the scroll exposed is dirty. The
    // line scrolled into history moves the view's first absolute row, so this
    // refresh is demoted and every row is re-read.
    const auto captureAfterScroll = [&](int firstAbsoluteRow) {
        std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
        rowDirty[static_cast<std::size_t>(t->rows) - 1] = 1;
        CapturedRowRefresh refresh;
        refresh.canReuseRows = true;
        refresh.rowDirty = std::span<const std::uint8_t> { rowDirty };
        refresh.scrollTop = 0;
        refresh.scrollBottom = t->rows - 1;
        refresh.scrollDelta = 1;
        refresh.firstAbsoluteRow = firstAbsoluteRow;
        REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    };
    feed(*t, "\033[4;1H\r\ntail");
    captureAfterScroll(0);
    CHECK_MSG(frame.inputs().cells[0].codePoint == 'p', "row 0 holds the text the scroll moved into it");
    for (std::size_t column = 0; column < width; ++column) {
        CHECK_MSG(static_cast<int>(frame.inputs().linkCells[column]) == static_cast<int>(LinkState::None),
            "and carries that text's flags, not the ones the link left behind");
    }

    // The same scroll with a refresh the capture keeps: the cells are shifted
    // rather than re-read, and the flags are shifted with them.
    feed(*t, "\033]8;;https://example.com\033\\link\033]8;;\033\\");
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const int history = Ubiquitty::Screen::scrollbackCount(t.get());
    feed(*t, "\033[4;1H\r\nlast");
    captureAfterScroll(history + 1);
    for (std::size_t column = 0; column < width; ++column) {
        CHECK_MSG(static_cast<int>(frame.inputs().linkCells[column])
                == static_cast<int>(frame.inputs().cells[column].linkID() != 0 ? LinkState::Resting : LinkState::None),
            "every row's flags describe the cells the row holds after the shift");
    }
}

TEST(testAPartialRefreshForAnotherViewKeepsNothing)
{
    auto t = makeTerminal();
    feed(*t, "first\r\nsecond\r\n");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK(frame.inputs().cells[0].codePoint == 'f');

    feed(*t, "\033[H");
    feed(*t, "ZZZZZ");
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t->rows), 0);
    CapturedRowRefresh stale;
    stale.canReuseRows = true;
    stale.rowDirty = std::span<const std::uint8_t> { rowDirty };
    // A drain that ran at a different view offset. Its dirty marks describe
    // other lines, so a row it called clean is not this frame's row 0 — the
    // capture re-derives the view and demotes rather than keeping it.
    stale.viewOffset = 3;
    stale.firstAbsoluteRow = 7;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, stale));
    CHECK_MSG(frame.inputs().cells[0].codePoint == 'Z',
        "a refresh naming another view keeps no row, however clean it claims they are");
}

TEST(testTerminalCaptureKeepsPristineRowsAcrossChrome)
{
    auto terminal = makeTerminal(3, 8);
    feed(*terminal, "terminal");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(frame.inputs().cells[0].codePoint, static_cast<std::uint32_t>('t'));

    CellCanvas popover;
    popover.resizeAndBlank(1, 4);
    Pen pen {};
    pen.foregroundIndex = colorRGB;
    pen.backgroundIndex = colorRGB;
    drawText(popover, 0, 0, "beam", pen);
    const ChromePlacement layers[] = { ChromePlacement { &popover, 0, 0 } };
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, layers));
    CHECK_EQ(frame.inputs().cells[0].codePoint, static_cast<std::uint32_t>('b'));

    // Core reports no row damage because the terminal never changed. Removing
    // frontend chrome must still reveal the pristine row rather than promoting
    // the composited capture as the next terminal baseline.
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(terminal->rows), 0);
    CapturedRowRefresh refresh;
    refresh.canReuseRows = true;
    refresh.rowDirty = rowDirty;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK_EQ(frame.inputs().cells[0].codePoint, static_cast<std::uint32_t>('t'));
}

TEST(testTheCaptureResolvesLinkKindsFromTheTerminalWhenAsked)
{
    // The flag asks the Terminal overload to install Core's own kind lookup,
    // so a caller that can name no resolver still gets file:// dotted and
    // mailto: dashed — the mapping stays Core's, not a frontend copy.
    auto t = makeTerminal(6, 24);
    feed(*t, "\x1b]8;;https://example.com\x1b\\url\x1b]8;;\x1b\\\r\n"
             "\x1b]8;;file:///etc/hosts\x1b\\path\x1b]8;;\x1b\\\r\n"
             "\x1b]8;;mailto:a@b.com\x1b\\mail\x1b]8;;\x1b\\");
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    options.shouldResolveLinkKindsFromSource = true;
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const auto state = [&](int row) {
        return static_cast<int>(frame.inputs().linkCells[static_cast<std::size_t>(row) * static_cast<std::size_t>(t->columns)]);
    };
    CHECK_EQ(state(0), static_cast<int>(LinkState::Resting));
    CHECK_EQ(state(1), static_cast<int>(LinkState::RestingFile));
    CHECK_EQ(state(2), static_cast<int>(LinkState::RestingMailto));
}

// An action link is what the cell is rather than a style, so the capture
// resolves it from the terminal whether or not per-kind styling is on, and a
// hovered one is the hovered action rather than a hovered link. A file link
// without the setting stays on the base style, as before.
TEST(testAnActionLinkResolvesWithoutPerKindStyling)
{
    auto t = makeTerminal(6, 24);
    feed(*t, "\x1b]8;;ubiquitty:action/connect\x1b\\button\x1b]8;;\x1b\\\r\n"
             "\x1b]8;;file:///etc/hosts\x1b\\path\x1b]8;;\x1b\\\r\n"
             "\x1b]8;;ubiquitty:action/settings\x1b\\other\x1b]8;;\x1b\\");
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const auto state = [&](int row) {
        return static_cast<int>(frame.inputs().linkCells[static_cast<std::size_t>(row) * static_cast<std::size_t>(t->columns)]);
    };
    CHECK_EQ(state(0), static_cast<int>(LinkState::RestingAction));
    CHECK_EQ(state(1), static_cast<int>(LinkState::Resting));
    CHECK_EQ(state(2), static_cast<int>(LinkState::RestingAction));

    // Hover on the first button: its cells are the hovered action, the other
    // button still rests.
    options.hoverLinkRowFirst = 0;
    options.hoverLinkRowLast = 0;
    options.hoverLinkColumnFirst = 0;
    options.hoverLinkColumnLast = 5;
    options.hoverLinkSpanFirst = 0;
    options.hoverLinkSpanLast = 5;
    options.hoverLinkID = t->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->linkID();
    options.isHoverLinkActive = true;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(state(0), static_cast<int>(LinkState::HoveredAction));
    CHECK_EQ(state(2), static_cast<int>(LinkState::RestingAction));
}

TEST(testACallersOwnResolverOutranksTheSourceFlag)
{
    // The flag fills the field only when it is empty: a caller that supplies
    // its own lookup keeps it, or two resolvers would disagree about one cell.
    auto t = makeTerminal(3, 24);
    feed(*t, "\x1b]8;;file:///etc/hosts\x1b\\path\x1b]8;;\x1b\\");
    TerminalSnapshotOptions options;
    options.shouldUnderlineLinks = true;
    options.shouldResolveLinkKindsFromSource = true;
    options.linkKindResolver = [](std::uint16_t, void*) -> std::uint8_t {
        return static_cast<std::uint8_t>(Attributes::LinkRun::Kind::Mailto);
    };
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    CHECK_EQ(static_cast<int>(frame.inputs().linkCells[0]), static_cast<int>(LinkState::RestingMailto));
}

} // namespace

// Chrome is a LIST of layers, and the list has an order. An arm draws a hint
// under an open palette, so whichever is last must win where they overlap —
// two surfaces that both claim a cell and no stated order is how one of them
// disappears depending on which arm assembled the array.
TEST(chromeLayersCompositeInOrderSoTheLastOneWins)
{
    auto t = makeTerminal(6, 20);
    TerminalSnapshotOptions options;
    options.viewOffset = 0;

    CellCanvas under;
    under.resizeAndBlank(1, 4);
    Pen underPen {};
    underPen.foregroundIndex = colorRGB;
    underPen.backgroundIndex = colorRGB;
    drawText(under, 0, 0, "aaaa", underPen);

    CellCanvas over;
    over.resizeAndBlank(1, 4);
    Pen overPen {};
    overPen.foregroundIndex = colorRGB;
    overPen.backgroundIndex = colorRGB;
    drawText(over, 0, 0, "bbbb", overPen);

    // Both at the same origin: every cell is contested, so the winner is the
    // order and nothing else.
    const ChromePlacement layers[] = {
        ChromePlacement { &under, 2, 3 },
        ChromePlacement { &over, 2, 3 },
    };
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, layers));
    const Cell* contested = frame.inputs().cells + (static_cast<std::size_t>(2) * t->columns) + 3;
    CHECK_MSG(contested->codePoint == U'b', "the later layer draws over the earlier one");

    // And the reverse order reverses the answer, so the case is about the order
    // rather than about which canvas happened to be built second.
    const ChromePlacement reversed[] = {
        ChromePlacement { &over, 2, 3 },
        ChromePlacement { &under, 2, 3 },
    };
    TerminalFrameInputs flipped;
    REQUIRE(flipped.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, reversed));
    const Cell* other = flipped.inputs().cells + (static_cast<std::size_t>(2) * t->columns) + 3;
    CHECK_MSG(other->codePoint == U'a', "reversing the list reverses which surface is on top");
}

TEST(anEmptyLayerInTheListDrawsNothingAndDoesNotDisturbTheOthers)
{
    auto t = makeTerminal(6, 20);
    TerminalSnapshotOptions options;
    options.viewOffset = 0;

    // The shape every arm passes when one of its two surfaces is closed: a
    // placement whose canvas has no rows. It must be a no-op rather than a
    // reason to skip the list.
    CellCanvas drawn;
    drawn.resizeAndBlank(1, 3);
    Pen pen {};
    pen.foregroundIndex = colorRGB;
    pen.backgroundIndex = colorRGB;
    drawText(drawn, 0, 0, "hey", pen);
    CellCanvas closed; // never resized: rows == 0

    const ChromePlacement layers[] = {
        ChromePlacement { &closed, 1, 1 },
        ChromePlacement { &drawn, 3, 5 },
    };
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}, layers));
    const Cell* at = frame.inputs().cells + (static_cast<std::size_t>(3) * t->columns) + 5;
    CHECK_MSG(at->codePoint == U'h', "the closed layer was skipped and the drawn one still landed");
    CHECK_MSG(hasDrawnChrome(layers), "and the list still reports that something draws");

    const ChromePlacement none[] = { ChromePlacement { &closed, 1, 1 } };
    CHECK_MSG(!hasDrawnChrome(none), "a list of only closed layers draws nothing");
    CHECK_MSG(!hasDrawnChrome({}), "and neither does an empty list");
}

// This binary is built with the test hooks and links library code built
// without them, so every class the two share must have one layout either way.
// A member behind the hook flag made `TerminalFrameInputs` larger here than in
// the library: the library's snapshot render allocated the smaller object on
// its stack and this binary's constructor wrote the larger one into it, which
// aborted on the stack check or, with the check off, overwrote whatever lay
// beside it.
TEST(testTheLibrarysSnapshotRenderRunsInAHookedBinary)
{
    auto t = makeTerminal();
    feed(*t, "hello");
    TerminalSnapshotOptions options;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> rgba = renderTerminalSnapshotRGBA(*t, options, width, height);
    CHECK(width > 0 && height > 0);
    CHECK(rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
}

// A proportional run on a DECDWL row is drawn twice as wide as the same run on
// a plain row, through the portable renderer end to end. The builder doubles
// the run's quads, and the compositor has to map each doubled quad onto its
// atlas span: read one texel a pixel from the quad's origin, it drew the run
// at its plain width at the doubled position, with the rest of the claim empty.
TEST(testThePortableRendererDrawsADoubledProportionalRunTwiceAsWide)
{
    const auto inkedPixels = [](const char* text) {
        auto t = makeTerminal(1, 40);
        t->drawsSizedText = true;
        t->drawsProportionalText = true;
        feed(*t, text);
        TerminalSnapshotOptions options;
        int width = 0;
        int height = 0;
        const std::vector<std::uint8_t> rgba = renderTerminalSnapshotRGBA(*t, options, width, height);
        const std::uint8_t* corner = rgba.data();
        int inked = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::uint8_t* pixel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                if (std::abs(pixel[0] - corner[0]) + std::abs(pixel[1] - corner[1]) + std::abs(pixel[2] - corner[2]) > 96)
                    ++inked;
            }
        }
        return inked;
    };
    // The ink's area rather than its extent: drawn at their plain width from
    // doubled positions, the glyphs still reach as far, and only their area
    // tells a doubled glyph from a plain one standing where a doubled one would.
    const int plain = inkedPixels("\033]66;p=1:c=12;proportional\007");
    const int doubled = inkedPixels("\033#6\033]66;p=1:c=12;proportional\007");
    REQUIRE_MSG(plain > 0, "the plain run draws");
    CHECK_MSG(doubled > plain * 17 / 10 && doubled < plain * 23 / 10, "the doubled run inks twice the plain run's area");
}

UBIQUITTY_TEST_MAIN()

// A backend that remembers which code point each column asked for, so the
// grid-face fallback of a proportional run can be read off the fetches.
struct RecordingAtlas : UbiquittyTest::StrictAtlas {
    GlyphSlotCache monoCache { 1024, recommendedHashBuckets(1024), 32 };
    std::vector<std::uint32_t> fetched;
    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { 32, 8, 16, 6, { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.weight = 0u;
        return binding;
    }
    // The builder faults glyphs through the bound slot cache and asks this to
    // fill each fresh slot, once per distinct glyph in first-seen order.
    bool rasteriseMonoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int)
    {
        fetched.push_back(codePoint);
        return true;
    }
    void beginFrame() { }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }
};

// A proportional run without a strip-shaping backend draws its text in the
// grid's face, one code point per cell, clipped to the run
// (Docs/proportional-text.md): six cells of "Cancel" rasterise C, a, n, c,
// e, l in order after the plain "hi", each cell claims one column, and a run
// wider than its text draws blank cells past it. Asserted through the
// rasterisations because the stretched alternative — the anchor's glyph
// mapped across the whole run, which is what a sized block's width claim
// would produce — rasterises one glyph and paints it six cells wide.
TEST(testAProportionalRunDrawsItsTextAcrossItsCellsInTheGridFace)
{
    auto t = makeTerminal(3, 20);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    feed(*t, "hi \033]66;p=1:c=6;Cancel\007 \033]66;p=1:c=4;OK\007");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.shouldUseSubpixelGlyphs = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const BuildInputs& in = frame.inputs();
    CHECK_MSG(in.textSizeTable != nullptr, "the capture carries the spec table");

    RecordingAtlas atlas;
    std::vector<CellInstance> cells(static_cast<std::size_t>(in.rows) * in.columns);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    (void)counts;

    const std::vector<std::uint32_t> expected { 'h', 'i', 'C', 'a', 'n', 'c', 'e', 'l', 'O', 'K' };
    CHECK_MSG(atlas.fetched == expected, "one fetch per cell of the run, in the text's order, and nothing for the blank tail");
    if (atlas.fetched != expected) {
        std::printf("  fetched:");
        for (const std::uint32_t codePoint : atlas.fetched)
            std::printf(" %u", codePoint);
        std::printf("\n");
    }
    // Every cell of both runs claims one column: the anchor does not paint the
    // run's background as one span, so a continuation's own colours draw.
    bool isEveryRunCellOneWide = true;
    for (const CellInstance& inst : std::span(cells.data(), std::min(counts.cells, cells.size()))) {
        if (inst.gridPosition.y == 0.0f && inst.gridPosition.x < 11.0f && inst.widthCells != 1.0f)
            isEveryRunCellOneWide = false;
    }
    CHECK(isEveryRunCellOneWide);
    t.reset();
}

// The recording atlas with a proportional strip: what it was asked to shape,
// the pen residual it was handed, and a fixed placement it answers with — the
// quad three pixels right of the pen's whole pixel, forty wide.
struct StripAtlas : RecordingAtlas {
    std::vector<std::uint32_t> shaped;
    int shapedWidthCells = 0;
    std::uint8_t shapedAlign = 0;
    float shapedResidual = -1.0f;
    ProportionalStripPlacement proportionalStrip(std::span<const std::uint32_t> codePoints, int widthCells, std::uint8_t horizontalAlign, float penResidualInPixels, FontFace, std::uint32_t, std::uint32_t)
    {
        shaped.assign(codePoints.begin(), codePoints.end());
        shapedWidthCells = widthCells;
        shapedAlign = horizontalAlign;
        shapedResidual = penResidualInPixels;
        ProportionalStripPlacement placement;
        placement.strip = { { 0.5f, 0.5f }, { 0.75f, 0.6f }, true };
        placement.leftInPixels = 3.0f;
        placement.widthInPixels = 40.0f;
        return placement;
    }
};

// On a backend with the strip the anchor hands the whole run to it — the text
// once, the width, the alignment — and draws one quad over the run's cells on
// the strip stream; it fetches no glyph of its own, and the continuations
// draw nothing. The plain text beside the run still rasterises.
TEST(testAProportionalRunDrawsOneStripQuadOnABackendThatShapesIt)
{
    auto t = makeTerminal(3, 20);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    feed(*t, "hi \033[4m\033]66;p=1:c=6:h=2;Cancel\007\033[24m");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    options.style.shouldUnderlineSkipInk = true;
    options.shouldUseSubpixelGlyphs = false;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));
    const BuildInputs& in = frame.inputs();

    StripAtlas atlas;
    std::vector<CellInstance> cells(static_cast<std::size_t>(in.rows) * in.columns);
    std::vector<DecorationInstance> decorations(cells.size() * 8);
    std::vector<GlyphInstance> strips(16);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    buffers.proportionalStrips = strips.data();
    buffers.proportionalStripCapacity = strips.size();
    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);

    const std::vector<std::uint32_t> text { 'C', 'a', 'n', 'c', 'e', 'l' };
    CHECK_MSG(atlas.shaped == text, "the whole run reaches the strip once");
    CHECK_EQ(atlas.shapedWidthCells, 6);
    CHECK_EQ(static_cast<int>(atlas.shapedAlign), 2);
    const std::vector<std::uint32_t> plain { 'h', 'i' };
    CHECK_MSG(atlas.fetched == plain, "the run's letters fetch no glyph of their own");
    CHECK_EQ(static_cast<int>(counts.proportionalStrips), 1);
    CHECK_EQ(static_cast<int>(counts.ligatureGlyphs), 0);
    // Six cells wide plus the margin each side, at the run's column.
    const GlyphInstance& quad = strips[0];
    // The quad sits where the backend placed it from the pen's whole pixel;
    // a cell-baked pass hands the backend no residual.
    CHECK(std::fabs(quad.sizeInPixels.x - 40.0f) < 0.01f);
    CHECK(std::fabs(quad.originInPixels.x - (std::floor(in.originInPixels.x + 3.0f * in.metrics.cellWidth) + 3.0f)) < 0.01f);
    CHECK(atlas.shapedResidual == 0.0f);
    CHECK(quad.atlasTopLeft.x == 0.5f && quad.atlasBottomRight.x == 0.75f);
    bool hasProportionalStripSkipInk = false;
    for (const DecorationInstance& decoration : std::span(decorations.data(), std::min(counts.decorations, decorations.size()))) {
        if (decoration.skipInk > 0.5f && static_cast<int>(decoration.pad1.y + 0.5f) % 8 == 1) {
            hasProportionalStripSkipInk = true;
            break;
        }
    }
    CHECK_MSG(hasProportionalStripSkipInk, "an underlined proportional strip samples its own coverage sheet");
    // The anchor paints the run's six columns of background, one glyph-less
    // cell instance per column, the way a sized anchor paints its claim.
    int runBackgroundCells = 0;
    for (const CellInstance& inst : std::span(cells.data(), std::min(counts.cells, cells.size()))) {
        if (inst.gridPosition.y == 0.0f && inst.gridPosition.x >= 3.0f && inst.gridPosition.x < 9.0f) {
            ++runBackgroundCells;
            CHECK(inst.atlasTopLeft.x == inst.atlasBottomRight.x && inst.atlasTopLeft.y == inst.atlasBottomRight.y);
        }
    }
    CHECK_EQ(runBackgroundCells, 6);
    t.reset();
}

// A producer hook without an output stream is not a rendering route. Core
// expands the stored run into grid-face cells so no character disappears while
// a minimal frontend omits the optional strip resource.
TEST(testAProportionalRunFallsBackToGridGlyphsWithoutAStripOutputLane)
{
    auto t = makeTerminal(3, 20);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    feed(*t, "hi \033]66;p=1:c=6:h=2;Cancel\007");

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CHECK(frame.capture(*t, options, unifontMetrics(), GlyphSlotGeometry {}));

    StripAtlas atlas;
    std::vector<CellInstance> cells(static_cast<std::size_t>(frame.inputs().rows) * frame.inputs().columns);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    const InstanceCounts counts = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);

    const std::vector<std::uint32_t> expected { 'h', 'i', 'C', 'a', 'n', 'c', 'e', 'l' };
    CHECK_MSG(atlas.shaped.empty(), "the unavailable strip route is not called");
    CHECK_MSG(atlas.fetched == expected, "every stored run character takes the grid-face fallback");
    CHECK_EQ(static_cast<int>(counts.proportionalStrips), 0);
    t.reset();
}

TEST(testCursorBlinkAdvancesWithoutRecapturingContent)
{
    auto terminal = makeTerminal(3, 20);
    feed(*terminal, "A");
    TerminalSnapshotOptions options;
    options.isFocused = true;
    options.cursorMixAlpha = 0.0f;
    TerminalFrameInputs frame;
    REQUIRE(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    RecordingAtlas atlas;
    std::vector<CellInstance> cells(128);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    const auto dark = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(dark.cells > 1 && dark.cells <= cells.size());
    const auto gridCell = cells[0];
    const auto darkCursor = cells[dark.cells - 1];
    frame.restateCursorMixAlpha(1.0f);
    const auto lit = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(lit.cells > 1 && lit.cells <= cells.size());
    const auto litCursor = cells[lit.cells - 1];
    CHECK(std::memcmp(&cells[0], &gridCell, sizeof(CellInstance)) == 0);
    CHECK(litCursor.background.x != darkCursor.background.x
        || litCursor.background.y != darkCursor.background.y
        || litCursor.background.z != darkCursor.background.z);
}

TEST(testHeldCursorKeepsTheCharacterInverted)
{
    auto terminal = makeTerminal(3, 20);
    feed(*terminal, "\033[1;1HA\033[2;10H*");
    TerminalSnapshotOptions options;
    options.isFocused = true;
    options.shouldCursorInvertText = true;
    options.shouldUseSubpixelGlyphs = false;
    TerminalFrameInputs frame;
    CHECK(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    frame.restateCursorPosition(0, 0);
    CHECK(!frame.inputs().cursor.isSliding);
    RecordingAtlas atlas;
    std::vector<CellInstance> cells(128);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    // A held cursor and a settled one draw the same tier over the `A`: the
    // cell re-emitted at the tail with the cursor's colours, the grid's own
    // `A` untouched by either.
    const auto held = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(held.cursorTier.cells == 1 && held.cells > 1 && held.cells <= cells.size());
    const auto heldGridCell = cells[0];
    const auto heldTier = cells[held.cells - 1];
    feed(*terminal, "\033[1;1H");
    CHECK(frame.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}));
    const auto settled = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);
    REQUIRE(settled.cursorTier.cells == 1 && settled.cells == held.cells);
    const auto settledTier = cells[settled.cells - 1];
    CHECK(heldTier.foreground.x == settledTier.foreground.x);
    CHECK(heldTier.foreground.y == settledTier.foreground.y);
    CHECK(heldTier.foreground.z == settledTier.foreground.z);
    CHECK(heldTier.background.x == settledTier.background.x);
    CHECK(heldTier.background.y == settledTier.background.y);
    CHECK(heldTier.background.z == settledTier.background.z);
    CHECK_MSG(heldTier.background.x != heldGridCell.background.x || heldTier.foreground.x != heldGridCell.foreground.x,
        "the tier carries the cursor's colours and the grid's cell does not");
    CHECK(heldGridCell.background.x == cells[0].background.x && heldGridCell.foreground.x == cells[0].foreground.x);
    // A sliding cursor is the overlay's: no tier, and the grid's `A` the same.
    frame.restateCursorSliding(0, 0);
    const auto sliding = buildCellInstances(frame.inputs(), UbiquittyTest::glyphBackendFor(atlas), buffers);
    CHECK(sliding.cursorTier.cells == 0);
    CHECK(heldGridCell.foreground.x == cells[0].foreground.x && heldGridCell.background.x == cells[0].background.x);
}

TEST(testRotatingCapturesRefreshRowsOutsideTheCurrentDamage)
{
    auto terminal = makeTerminal(3, 20);
    TerminalSnapshotOptions options;
    TerminalFrameInputs first;
    TerminalFrameInputs second;
    std::array<std::uint8_t, 3> damage { 1, 1, 1 };
    CapturedRowRefresh refresh;
    refresh.rowDirty = damage;
    refresh.generationAfterDrain = 1;
    feed(*terminal, "\033[1;1HA");
    CHECK(first.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK(second.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    feed(*terminal, "\033[1;1HB");
    damage = { 1, 0, 0 };
    refresh.canReuseRows = true;
    refresh.baseGeneration = 1;
    refresh.generationAfterDrain = 2;
    CHECK(second.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    feed(*terminal, "\033[2;1HC");
    damage = { 0, 1, 0 };
    refresh.baseGeneration = 2;
    refresh.generationAfterDrain = 3;
    CHECK(first.capture(*terminal, options, unifontMetrics(), GlyphSlotGeometry {}, refresh));
    CHECK_MSG(first.inputs().cells[0].codePoint == 'B', "a recycled capture cannot resurrect its older clean row");
    CHECK(first.inputs().cells[20].codePoint == 'C');
    CHECK_MSG(first.inputs().dirtyRows && !first.inputs().dirtyRows[0], "refreshing owned rows preserves the target's incremental damage");
}
