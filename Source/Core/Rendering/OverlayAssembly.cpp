// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OverlayAssembly.h"

#include "Core/Rendering/ColorResolve.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/MarkValues.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

namespace {

// The one mapping from a tier's quad to a stream instance, for every pass and
// every frontend: a field of `OverlayInstance` is defaulted exactly once, or the
// tiers disagree about what an unset field means.
void emit(const OverlayQuad& quad, std::size_t& total, std::span<OverlayInstance> out, float strokeWidth = 0.0f)
{
    const std::size_t index = total++;
    if (index >= out.size())
        return;
    OverlayInstance& overlay = out[index];
    overlay = {};
    overlay.topLeftInPixels = { quad.minX, quad.minY };
    overlay.bottomRightInPixels = { quad.maxX, quad.maxY };
    overlay.color = { quad.red, quad.green, quad.blue, quad.alpha };
    overlay.cornerRadius = quad.cornerRadius;
    overlay.strokeWidth = strokeWidth;
    overlay.shadeDensity = 0.0f;
    overlay.shapeKind = 0.0f;
    overlay.plA = { 0.0f, 0.0f, 0.0f, 0.0f };
    overlay.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
}

// How many cells a logical column of a visible row spans: 2 on a DECDWL or
// DECDHL row, where the span tiers' logical columns stand at twice the column
// and twice the width, as the cursor does.
[[nodiscard]] int rowWidthStep(const OverlayAssemblyInputs& in, int visibleRow)
{
    if (!in.lineAttributes || visibleRow < 0 || visibleRow >= in.grid.rows)
        return 1;
    return textScaleForLineAttribute(in.lineAttributes[visibleRow]).widthScale;
}

// A span's columns on its row's pitch: the logical columns clamped to the
// ones the row draws, which on a doubled row end where the grid does.
struct RowSpanColumns {
    int column0 = 0;
    int column1 = -1;
    int step = 1;
};

[[nodiscard]] RowSpanColumns rowSpanColumns(const OverlayAssemblyInputs& in, const SelectionRowSpan& span)
{
    RowSpanColumns columns;
    columns.step = rowWidthStep(in, span.visibleRow);
    const int lastDrawn = in.grid.columns / columns.step - 1;
    columns.column0 = span.column0 < 0 ? 0 : span.column0;
    columns.column1 = span.column1 > lastDrawn ? lastDrawn : span.column1;
    return columns;
}

// The letters of a visible row the primary face laid out as proportional
// runs (LetterLayout.h), or an empty span for a row on the grid.
[[nodiscard]] std::span<const LetterExtent> rowLetters(const OverlayAssemblyInputs& in, int visibleRow)
{
    if (!in.letters || !in.letterRows || visibleRow < 0 || visibleRow >= in.grid.rows || in.grid.columns <= 0
        || in.letterRows[static_cast<std::size_t>(visibleRow)] == 0u)
        return {};
    return { in.letters + static_cast<std::size_t>(visibleRow) * static_cast<std::size_t>(in.grid.columns), static_cast<std::size_t>(in.grid.columns) };
}

// Where logical columns `column0...column1` of a row laid out as letters
// stand, from the grid's left edge: the first letter's left to the last
// letter's right, so a tier over them covers the letters and not the pitch.
struct LetterSpanInPixels {
    float left = 0.0f;
    float right = 0.0f;
};

[[nodiscard]] LetterSpanInPixels letterSpanInPixels(std::span<const LetterExtent> letters, int column0, int column1)
{
    const int last = static_cast<int>(letters.size()) - 1;
    const int first = std::clamp(column0, 0, last);
    const int end = std::clamp(column1, first, last);
    return { letters[static_cast<std::size_t>(first)].leftInPixels,
        letters[static_cast<std::size_t>(end)].leftInPixels + letters[static_cast<std::size_t>(end)].advanceInPixels };
}

// A span tier: clamp to the viewport, resolve the tier's quad, emit. The three
// tinted tiers differ only in their geometry entry, which is where each keeps
// its colour and inset. The entry lays columns out at the cell pitch, so a
// doubled row hands it the pitch doubled, and a row laid out as letters hands
// it the letters' span as one cell standing at the first letter.
template<typename QuadAtVisibleRow>
void emitSpanTier(std::span<const SelectionRowSpan> spans, const OverlayAssemblyInputs& in,
    QuadAtVisibleRow&& quadAtVisibleRow, std::size_t& total, std::span<OverlayInstance> out)
{
    for (const SelectionRowSpan& span : spans) {
        if (span.visibleRow < 0 || span.visibleRow >= in.grid.rows)
            continue;
        OverlayQuad quad;
        const std::span<const LetterExtent> letters = rowLetters(in, span.visibleRow);
        if (!letters.empty()) {
            if (span.column1 < span.column0)
                continue;
            const LetterSpanInPixels pixels = letterSpanInPixels(letters, span.column0, span.column1);
            if (pixels.right <= pixels.left)
                continue;
            if (!quadAtVisibleRow(span.visibleRow, 0, 0, FloatSize { pixels.right - pixels.left, in.cellSizeInPixels.height },
                    FloatPoint { in.originInPixels.x + pixels.left, in.originInPixels.y }, quad))
                continue;
            emit(quad, total, out);
            continue;
        }
        const RowSpanColumns columns = rowSpanColumns(in, span);
        if (columns.column1 < columns.column0)
            continue;
        if (!quadAtVisibleRow(span.visibleRow, columns.column0, columns.column1,
                FloatSize { in.cellSizeInPixels.width * static_cast<float>(columns.step), in.cellSizeInPixels.height },
                FloatPoint { in.originInPixels.x, in.originInPixels.y }, quad))
            continue;
        emit(quad, total, out);
    }
}

} // namespace

namespace {

/// Blends one pixel of a mark bracket into a padded image.
///
/// The ink is opaque where it covers: a mark says which rows a command ran on,
/// and a reader has to read it against whatever lies behind a translucent
/// window, so its bracket carries the window's alpha no more than its glyphs
/// do. The surface keeps its own alpha everywhere the ink does not reach, and a
/// partly covered pixel raises it only as far as that coverage — which is the
/// source-over of an opaque source, and never lowers the alpha a compositor
/// blends against.
void blendPromptMarkPixel(std::vector<std::uint8_t>& rgba, IntSize surface, int x, int y, RGB8 color, float alpha)
{
    if (x < 0 || y < 0 || x >= surface.width || y >= surface.height)
        return;
    const int weight = std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255);
    if (weight == 0)
        return;
    const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width) + static_cast<std::size_t>(x)) * 4;
    if (offset + 3 >= rgba.size())
        return;
    const int inverse = 255 - weight;
    rgba[offset] = static_cast<std::uint8_t>((rgba[offset] * inverse + color.red * weight + 127) / 255);
    rgba[offset + 1] = static_cast<std::uint8_t>((rgba[offset + 1] * inverse + color.green * weight + 127) / 255);
    rgba[offset + 2] = static_cast<std::uint8_t>((rgba[offset + 2] * inverse + color.blue * weight + 127) / 255);
    rgba[offset + 3] = static_cast<std::uint8_t>(weight + (rgba[offset + 3] * inverse + 127) / 255);
}

} // namespace

PromptMarkParameters promptMarkParametersForPadding(const TermPalette& palette, GridSize grid, FloatSize cellSizeInPixels,
    FloatPoint origin, float viewportWidthInPixels, double cellHeightPoints, const FaceMetrics& face)
{
    const RGB8 okInk = resolvePaletteColor(palette, promptMarkOkColorIndex, true);
    const RGB8 failInk = resolvePaletteColor(palette, promptMarkFailColorIndex, true);
    const PaletteRGB ground = palette.defaultBackground;
    PromptMarkParameters parameters;
    parameters.grid = grid;
    parameters.cellSizeInPixels = cellSizeInPixels;
    parameters.origin = origin;
    parameters.viewportWidthInPixels = viewportWidthInPixels;
    parameters.cellHeightPoints = cellHeightPoints;
    // The FACE's metrics, not the cell's. The cell folds the line spacing into
    // its baseline and the face does not, so substituting one for the other is
    // right only while that spacing is zero — and the mark then climbs away from
    // the text as the user opens the lines up.
    parameters.fontSize = face.fontSize;
    parameters.fontAscent = face.ascent;
    parameters.fontDescent = face.descent;
    parameters.lineGapTopInPixels = face.lineGapTopInPixels;
    // Both zero when the face was not measured, which the geometry reads as
    // unmeasured and answers with the ascent/descent midpoint.
    parameters.bracketInkTopInPixels = face.bracketInkTopInPixels;
    parameters.bracketInkBottomInPixels = face.bracketInkBottomInPixels;
    parameters.okRed = okInk.red / 255.0f;
    parameters.okGreen = okInk.green / 255.0f;
    parameters.okBlue = okInk.blue / 255.0f;
    parameters.failRed = failInk.red / 255.0f;
    parameters.failGreen = failInk.green / 255.0f;
    parameters.failBlue = failInk.blue / 255.0f;
    parameters.groundRed = channel16To8(ground.red) / 255.0f;
    parameters.groundGreen = channel16To8(ground.green) / 255.0f;
    parameters.groundBlue = channel16To8(ground.blue) / 255.0f;
    return parameters;
}

void compositePromptMarksIntoPadding(const PromptMarkParameters& parameters, const Ubiquitty::Terminal& terminal, int firstAbsoluteRow,
    std::vector<std::uint8_t>& rgba, IntSize surface)
{
    if (terminal.screens.isUsingAlternateScreen || parameters.fontSize <= 0)
        return;
    const PromptMarkGeometry geometry = resolvePromptMarkGeometry(parameters);
    if (geometry.perRow <= 0)
        return;
    OverlayQuad quads[maxPromptMarkQuadsPerRow];
    for (int row = 0; row < parameters.grid.rows; ++row) {
        const std::uint8_t mark = Screen::rowMark(&terminal, firstAbsoluteRow + row);
        if (!(mark & Screen::osc133MarkCmdExecuted))
            continue;
        const int count = buildPromptMarkBrackets(parameters, geometry, row, (mark & Screen::osc133MarkCmdFailed) != 0,
            quads, maxPromptMarkQuadsPerRow);
        for (int index = 0; index < count; ++index) {
            const OverlayQuad& quad = quads[index];
            const RGB8 color { static_cast<std::uint8_t>(std::lround(quad.red * 255.0f)),
                static_cast<std::uint8_t>(std::lround(quad.green * 255.0f)),
                static_cast<std::uint8_t>(std::lround(quad.blue * 255.0f)) };
            for (int y = static_cast<int>(std::floor(quad.minY)); y < static_cast<int>(std::ceil(quad.maxY)); ++y)
                for (int x = static_cast<int>(std::floor(quad.minX)); x < static_cast<int>(std::ceil(quad.maxX)); ++x)
                    blendPromptMarkPixel(rgba, surface, x, y, color, quad.alpha);
        }
    }
}

namespace {

/// Emits the password badge's quads, or none.
///
/// Out of line, and never inlined, so the tier's own branch lives here and
/// ``assembleOverlays`` keeps the block structure its PGO profile describes.
///
/// - Parameters:
///   - in: The frame the assembly is drawing.
///   - total: The running instance count, advanced by what this writes.
///   - out: The caller's overlay span.
[[gnu::noinline]] void emitPasswordLockTier(const OverlayAssemblyInputs& in, std::size_t& total, std::span<OverlayInstance> out)
{
    if (!in.shouldDrawPasswordLock || !in.isPasswordPromptActive || !in.palette || in.grid.rows <= 0 || in.grid.columns <= 0)
        return;

    const PaletteRGB ink = in.palette->defaultForeground;
    const PaletteRGB ground = in.palette->defaultBackground;
    PasswordLockParameters lockParameters;
    lockParameters.grid = { in.grid.columns, in.grid.rows };
    lockParameters.cellSizeInPixels = { in.cellSizeInPixels.width, in.cellSizeInPixels.height };
    lockParameters.origin.x = in.originInPixels.x;
    lockParameters.origin.y = in.originInPixels.y;
    lockParameters.cursorRow = in.cursor.row;
    lockParameters.cursorColumn = in.cursor.column;
    lockParameters.inkRed = channel16To8(ink.red) / 255.0f;
    lockParameters.inkGreen = channel16To8(ink.green) / 255.0f;
    lockParameters.inkBlue = channel16To8(ink.blue) / 255.0f;
    lockParameters.groundRed = channel16To8(ground.red) / 255.0f;
    lockParameters.groundGreen = channel16To8(ground.green) / 255.0f;
    lockParameters.groundBlue = channel16To8(ground.blue) / 255.0f;

    const PasswordLockGeometry lockGeometry = resolvePasswordLockGeometry(lockParameters);
    OverlayQuad lockQuads[maxPasswordLockQuads];
    const int written = buildPasswordLockQuads(lockParameters, lockGeometry, lockQuads, maxPasswordLockQuads);
    for (int quadIndex = 0; quadIndex < written; ++quadIndex)
        emit(lockQuads[quadIndex], total, out, lockQuads[quadIndex].strokeWidth);
}

} // namespace

int assembleOverlays(const OverlayAssemblyInputs& in, OverlayStream out)
{
    std::size_t total = 0;

    // Transient wash: the bell flash, a full-viewport quad in the default
    // foreground at the frontend's envelope alpha. Beneath everything, so the
    // annotations stay legible while the viewport pulses.
    if (in.bellFlashAlpha > 0.0f) {
        const std::size_t index = total++;
        if (index < out.size()) {
            OverlayInstance& overlay = out[index];
            overlay = {};
            overlay.topLeftInPixels = { 0.0f, 0.0f };
            overlay.bottomRightInPixels = { in.viewportSizeInPixels.width, in.viewportSizeInPixels.height };
            overlay.color = { channel16To8(in.defaultForeground.red) / 255.0f,
                channel16To8(in.defaultForeground.green) / 255.0f,
                channel16To8(in.defaultForeground.blue) / 255.0f, in.bellFlashAlpha };
            overlay.cornerRadius = 0.0f;
            overlay.strokeWidth = 0.0f;
            overlay.shadeDensity = 0.0f;
            overlay.shapeKind = 0.0f;
            overlay.plA = { 0.0f, 0.0f, 0.0f, 0.0f };
            overlay.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
        }
    }

    // Viewport furniture: the DECCOLM-132 content edge. At 132 columns the grid
    // is a fixed width inside whatever width the window has, so the leftover
    // strip can be wider than a cell — wide enough to read as empty terminal
    // rather than as outside it. A translucent rule marks where the terminal
    // ends. Skipped when the leftover is under a cell wide: that is the
    // ordinary asymmetric padding every resize leaves, and a rule there would
    // draw a line down the side of a window nobody put in 132-column mode.
    if (in.isColumns132 && in.viewportSizeInPixels.width > 0.0f) {
        const float contentRight = in.originInPixels.x + in.grid.columns * in.cellSizeInPixels.width;
        if (in.viewportSizeInPixels.width - contentRight > in.cellSizeInPixels.width) {
            const std::size_t index = total++;
            if (index < out.size()) {
                const float lineWidth = std::max(1.0f, std::round(in.deviceScale));
                OverlayInstance& overlay = out[index];
                overlay = {};
                overlay.topLeftInPixels = { contentRight, 0.0f };
                overlay.bottomRightInPixels = { contentRight + lineWidth, in.viewportSizeInPixels.height };
                overlay.color = { channel16To8(in.defaultForeground.red) / 255.0f,
                    channel16To8(in.defaultForeground.green) / 255.0f,
                    channel16To8(in.defaultForeground.blue) / 255.0f, 0.30f };
                overlay.cornerRadius = 0.0f;
                overlay.strokeWidth = 0.0f;
                overlay.shadeDensity = 0.0f;
                overlay.shapeKind = 0.0f;
                overlay.plA = { 0.0f, 0.0f, 0.0f, 0.0f };
                overlay.plB = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
        }
    }

    // Asked-for emphasis: the non-focused search matches. One neutral rounded
    // quad per span, so the hits the user is not standing on locate without
    // competing with the one they are — the focused match is absent by
    // construction, baked into its own cells.
    // Both search pills stand in from their row by the same insets, so each
    // centres on the words rather than on the cell and neither reaches the
    // row below or meets a pill on a neighbouring row.
    const SearchPillInsets pillInsets = searchPillInsets(in.cellSizeInPixels.height, in.cellHeightPoints,
        in.promptMarks.atlasScale > 0.0f ? static_cast<double>(in.promptMarks.atlasScale) : 1.0,
        in.promptMarks.fontAscent, in.promptMarks.lineGapTopInPixels, in.promptMarks.xHeight, in.promptMarks.capHeight);
    emitSpanTier(in.otherSearchMatchSpans, in, [pillInsets](int visibleRow, int column0, int column1, FloatSize cell, FloatPoint origin, OverlayQuad& quad) { return otherSearchMatchQuadAtVisibleRow(visibleRow, column0, column1,
                                                                                                                                                                  { cell.width, cell.height }, { origin.x, origin.y }, pillInsets, quad); }, total, out);

    // The focused match's pill: the ring around the cells that hold its flat
    // fill. One quad per span, the pill's rect reaching past the cells by the
    // corner radius each side and scaled by the bounce, inflated by the
    // shadow's reach; the fragment keeps the destination inside the cells'
    // box, where the cell pass painted the pill under the glyphs, and draws
    // the shadow and the pill's ends everywhere else — above the neighbours'
    // ink, as Safari draws its find highlight. Emitted after the other
    // matches so the focused one sits above them where they touch.
    //
    // The pill stands in from the cells' box by `pillInsets`, and the cell
    // pass painted the whole box: the fragment paints the strips the insets
    // vacated in the frame's ground. The top inset rides in `strokeWidth` and
    // the bottom in `shadeDensity`, which the ring has no outline or dither to
    // spend them on, and the ground rides in the colour's alpha as three packed
    // bytes, which the ring never blends by — the software compositor has no
    // frame background at hand, so the instance carries what both consumers
    // need.
    if (in.searchMatchHighlight.w > 0.0f) {
        const float scale = focusedPillScale(in.focusedMatchAgeSeconds);
        const PaletteRGB groundColor = in.palette ? in.palette->defaultBackground : PaletteRGB {};
        const float packedGround = static_cast<float>(
            channel16To8(groundColor.red) * 65536 + channel16To8(groundColor.green) * 256 + channel16To8(groundColor.blue));
        for (const SelectionRowSpan& span : in.searchMatchSpans) {
            if (span.visibleRow < 0 || span.visibleRow >= in.grid.rows || span.column1 < span.column0)
                continue;
            const RowSpanColumns columns = rowSpanColumns(in, span);
            if (columns.column1 < columns.column0)
                continue;
            const float pitch = in.cellSizeInPixels.width * static_cast<float>(columns.step);
            const float height = in.cellSizeInPixels.height - pillInsets.top - pillInsets.bottom;
            const float radius = height * focusedPillCornerFraction;
            const float shadow = height * focusedPillShadowFraction;
            const float top = in.originInPixels.y + static_cast<float>(span.visibleRow) * in.cellSizeInPixels.height + pillInsets.top;
            const float bottom = top + height;
            float boxLeft = in.originInPixels.x + static_cast<float>(columns.column0) * pitch;
            float boxRight = in.originInPixels.x + static_cast<float>(columns.column1 + 1) * pitch;
            if (const std::span<const LetterExtent> letters = rowLetters(in, span.visibleRow); !letters.empty()) {
                const LetterSpanInPixels pixels = letterSpanInPixels(letters, span.column0, span.column1);
                boxLeft = in.originInPixels.x + pixels.left;
                boxRight = in.originInPixels.x + pixels.right;
            }
            const float centreX = (boxLeft + boxRight) * 0.5f;
            const float centreY = (top + bottom) * 0.5f;
            const float halfWidth = ((boxRight - boxLeft) * 0.5f + radius) * scale;
            const float halfHeight = height * 0.5f * scale;
            const std::size_t index = total++;
            if (index >= out.size())
                continue;
            OverlayInstance& overlay = out[index];
            overlay = {};
            overlay.topLeftInPixels = { centreX - halfWidth - shadow, centreY - halfHeight - shadow };
            overlay.bottomRightInPixels = { centreX + halfWidth + shadow, centreY + halfHeight + shadow };
            overlay.color = { in.searchMatchHighlight.x, in.searchMatchHighlight.y, in.searchMatchHighlight.z, packedGround };
            overlay.cornerRadius = radius * scale;
            overlay.cornerStyle = 0.0f;
            overlay.strokeWidth = pillInsets.top;
            overlay.shadeDensity = pillInsets.bottom;
            overlay.shapeKind = focusedPillRingShapeKind;
            overlay.plA = { centreX - halfWidth, centreY - halfHeight, centreX + halfWidth, centreY + halfHeight };
            overlay.plB = { shadow, focusedPillShadowAlpha, boxLeft, boxRight };
        }
    }

    // Host-proposed: the hilite band (DEC mode 1001). The host asked to track a
    // region and gets an amber tint over it — beneath every user tier, because
    // nothing the host proposes outranks what the user configured or dragged.
    emitSpanTier(in.hiliteRegionSpans, in, [](int visibleRow, int column0, int column1, FloatSize cell, FloatPoint origin, OverlayQuad& quad) { return hiliteRegionQuadAtVisibleRow(visibleRow, column0, column1,
                                                                                                                                                    { cell.width, cell.height }, { origin.x, origin.y }, quad); }, total, out);

    // User-configured: trigger highlights. One violet tint per span a
    // `highlight` trigger recorded — the user's own rule firing, above the
    // host's proposal and beneath the user's live gesture.
    emitSpanTier(in.triggerHighlightSpans, in, [](int visibleRow, int column0, int column1, FloatSize cell, FloatPoint origin, OverlayQuad& quad) { return triggerHighlightQuadAtVisibleRow(visibleRow, column0, column1,
                                                                                                                                                        { cell.width, cell.height }, { origin.x, origin.y }, quad); }, total, out);

    // User-dragged: selection. One translucent filled quad per selected
    // on-screen row span, over every standing annotation so what the user is
    // doing right now reads on top.
    for (const SelectionRowSpan& span : in.selectionSpans) {
        if (span.column1 < span.column0 || span.visibleRow < 0 || span.visibleRow >= in.grid.rows)
            continue;
        // On a row laid out as letters the highlight covers the selected
        // letters' extents, as one cell standing at the first of them.
        if (const std::span<const LetterExtent> letters = rowLetters(in, span.visibleRow); !letters.empty()) {
            const LetterSpanInPixels pixels = letterSpanInPixels(letters, span.column0, span.column1);
            if (pixels.right <= pixels.left)
                continue;
            const OverlayQuad quad = cellSpanQuad(0, 0, span.visibleRow,
                { pixels.right - pixels.left, in.cellSizeInPixels.height },
                { in.originInPixels.x + pixels.left, in.originInPixels.y },
                { in.selectionColor.x, in.selectionColor.y, in.selectionColor.z, in.selectionColor.w });
            emit(quad, total, out);
            continue;
        }
        const RowSpanColumns columns = rowSpanColumns(in, span);
        if (columns.column1 < columns.column0)
            continue;
        const OverlayQuad quad = cellSpanQuad(columns.column0, columns.column1, span.visibleRow,
            { in.cellSizeInPixels.width * static_cast<float>(columns.step), in.cellSizeInPixels.height },
            { in.originInPixels.x, in.originInPixels.y },
            { in.selectionColor.x, in.selectionColor.y, in.selectionColor.z, in.selectionColor.w });
        emit(quad, total, out);
    }

    // Chrome: OSC 133 prompt marks, a bracket in each padding gutter beside
    // every visible row whose command actually ran (the bit is promoted at the
    // A→C transition, so a bare prompt draws nothing). Absent marks is the
    // whole gate a caller controls; the alternate screen reaches this as "no
    // marks passed" rather than as a flag. The gutter test is
    // resolvePromptMarkGeometry's: perRow is zero when neither side has room.
    if (in.promptMarks.rowMarks && in.promptMarks.fontSize > 0 && in.grid.rows > 0 && in.grid.columns > 0 && in.palette) {
        const RGB8 okInk = resolvePaletteColor(*in.palette, promptMarkOkColorIndex, true);
        const RGB8 failInk = resolvePaletteColor(*in.palette, promptMarkFailColorIndex, true);
        const PaletteRGB ground = in.palette->defaultBackground;
        PromptMarkParameters markParameters;
        markParameters.grid = { in.grid.columns, in.grid.rows };
        markParameters.cellSizeInPixels = { in.cellSizeInPixels.width, in.cellSizeInPixels.height };
        markParameters.origin.x = in.originInPixels.x;
        markParameters.origin.y = in.originInPixels.y;
        markParameters.viewportWidthInPixels = in.viewportSizeInPixels.width;
        markParameters.cellHeightPoints = in.cellHeightPoints;
        markParameters.atlasScale = in.promptMarks.atlasScale > 0.0f
            ? static_cast<double>(in.promptMarks.atlasScale)
            : 1.0;
        // The bitmap tier sizes the mark in device pixels, as the software
        // renderer does: its face metrics are already pixels, and a point
        // convention on top of them drew the bracket twice as heavy and half
        // again as tall on a scaled display as the reference draws it.
        if (in.isBitmapFace) {
            markParameters.cellHeightPoints = in.cellSizeInPixels.height;
            markParameters.atlasScale = 1.0;
        }
        markParameters.fontSize = in.promptMarks.fontSize;
        markParameters.fontAscent = in.promptMarks.fontAscent;
        markParameters.fontDescent = in.promptMarks.fontDescent;
        markParameters.lineGapTopInPixels = in.promptMarks.lineGapTopInPixels;
        markParameters.bracketInkTopInPixels = in.promptMarks.bracketInkTopInPixels;
        markParameters.bracketInkBottomInPixels = in.promptMarks.bracketInkBottomInPixels;
        markParameters.okRed = okInk.red / 255.0f;
        markParameters.okGreen = okInk.green / 255.0f;
        markParameters.okBlue = okInk.blue / 255.0f;
        markParameters.failRed = failInk.red / 255.0f;
        markParameters.failGreen = failInk.green / 255.0f;
        markParameters.failBlue = failInk.blue / 255.0f;
        markParameters.groundRed = channel16To8(ground.red) / 255.0f;
        markParameters.groundGreen = channel16To8(ground.green) / 255.0f;
        markParameters.groundBlue = channel16To8(ground.blue) / 255.0f;

        const PromptMarkGeometry markGeometry = resolvePromptMarkGeometry(markParameters);
        if (markGeometry.perRow > 0) {
            OverlayQuad markQuads[maxPromptMarkQuadsPerRow];
            for (int row = 0; row < in.grid.rows; ++row) {
                const std::uint8_t mark = in.promptMarks.rowMarks[static_cast<std::size_t>(row)];
                if (!(mark & Screen::osc133MarkCmdExecuted))
                    continue;
                const bool failed = (mark & Screen::osc133MarkCmdFailed) != 0;
                const int written = buildPromptMarkBrackets(markParameters, markGeometry, row, failed,
                    markQuads, static_cast<int>(sizeof(markQuads) / sizeof(markQuads[0])));
                for (int quadIndex = 0; quadIndex < written; ++quadIndex) {
                    if (!failed && in.promptMarks.ink.w != 0) {
                        auto& quad = markQuads[quadIndex];
                        quad.red = in.promptMarks.ink.x;
                        quad.green = in.promptMarks.ink.y;
                        quad.blue = in.promptMarks.ink.z;
                        quad.alpha = in.promptMarks.ink.w;
                    }
                    emit(markQuads[quadIndex], total, out, markQuads[quadIndex].strokeWidth);
                }
            }
        }
    }

    // Chrome, topmost: the cursor. The settled focused block is the walk's
    // cursor tier — the cell re-emitted with the cursor's colours at each
    // stream's tail, the mixA blink in its fill; every other case — bar,
    // underline, unfocused hollow, a block in flight — is one quad, and
    // `buildCursorQuad` declines the settled focused block and the
    // faded/off-screen cases on its own. A DECSSDT status line owns the
    // bottom grid row, so the data cursor never paints there — membership, not
    // a frontend gate.
    const bool isCursorOnStatusRow = in.hasStatusLineOnBottomRow && in.cursor.row == in.grid.rows - 1;
    if (in.cursor.row >= 0 && !isCursorOnStatusRow) {
        CursorQuadParameters cursorParameters;
        cursorParameters.column = in.cursor.column;
        cursorParameters.vrow = in.cursor.row;
        cursorParameters.rows = in.grid.rows;
        cursorParameters.columns = in.grid.columns;
        cursorParameters.shape = in.cursor.shape;
        cursorParameters.blinkAlpha = in.cursor.mixA;
        cursorParameters.focused = in.cursor.focused;
        cursorParameters.cellWidthInPixels = in.cellSizeInPixels.width;
        cursorParameters.cellHeightInPixels = in.cellSizeInPixels.height;
        cursorParameters.origin.x = in.originInPixels.x + in.cursor.richTextOffset * in.cellSizeInPixels.width;
        cursorParameters.origin.y = in.originInPixels.y;
        cursorParameters.cursorRed = in.cursor.rgb.x;
        cursorParameters.cursorGreen = in.cursor.rgb.y;
        cursorParameters.cursorBlue = in.cursor.rgb.z;
        cursorParameters.cursorAlpha = in.cursor.rgb.w;
        cursorParameters.cellHeightPoints = in.cellHeightPoints;
        cursorParameters.lineSpacingPoints = in.lineSpacingPoints;
        cursorParameters.toolkitCaret = in.toolkitCaret;
        cursorParameters.lineAttribute = in.cursor.lineAttribute;
        cursorParameters.widthCells = in.cursor.widthCells;
        cursorParameters.sliding = in.cursor.isSliding;
        cursorParameters.columnFractional = in.cursor.columnFractional;
        cursorParameters.vrowFractional = in.cursor.vrowFractional;
        // On a row laid out as letters the cursor stands at its letter's pen
        // and takes the letter's advance, every shape alike: the quad is built
        // as column zero of a one-column row whose origin is the letter and
        // whose cell is its advance. The letter already carries the row's
        // doubling and a claim's width, so neither is applied again. A sliding
        // cursor interpolates between the letters it slides from and to.
        if (const std::span<const LetterExtent> letters = rowLetters(in, in.cursor.row); !letters.empty()) {
            const int last = static_cast<int>(letters.size()) - 1;
            const int column = std::clamp(in.cursor.column, 0, last);
            float left = letters[static_cast<std::size_t>(column)].leftInPixels;
            float advance = letters[static_cast<std::size_t>(column)].advanceInPixels;
            if (in.cursor.isSliding) {
                const float whole = std::floor(in.cursor.columnFractional);
                const int from = std::clamp(static_cast<int>(whole), 0, last);
                const float fraction = in.cursor.columnFractional - whole;
                const LetterExtent& at = letters[static_cast<std::size_t>(from)];
                left = at.leftInPixels + fraction * at.advanceInPixels;
                advance = at.advanceInPixels;
            }
            if (advance <= 0.0f)
                advance = in.cellSizeInPixels.width;
            cursorParameters.column = 0;
            cursorParameters.columns = 1;
            cursorParameters.widthCells = 1;
            cursorParameters.lineAttribute = lineAttributeNormal;
            cursorParameters.columnFractional = 0.0f;
            cursorParameters.cellWidthInPixels = advance;
            cursorParameters.origin.x = in.originInPixels.x + left;
        }
        OverlayQuad quad;
        if (buildCursorQuad(cursorParameters, quad)) {
            const std::size_t index = total;
            emit(quad, total, out, quad.strokeWidth);
            // The focused toolkit caret keeps the toolkit's blend, so its
            // alpha travels as the toolkit's own 8-bit mask; unfocused, the
            // bar is the hollow outline every shape is.
            if (cursorParameters.toolkitCaret.isPresent() && cursorParameters.shape == Ubiquitty::Cursor::Shape::Bar && cursorParameters.focused
                && index < out.size()) {
                out[index].shapeKind = toolkitCaretShapeKind;
                out[index].color.w = static_cast<float>(toolkitCaretMask(quad.alpha)) / 255.0f;
            }
        }
    }

    // Chrome, above the cursor: the password badge, in the cell after the
    // caret. Above rather than below because the two never overlap — the badge
    // takes the next cell — so the order costs nothing and a future caret shape
    // that overhangs its cell cannot swallow the one indicator that says the
    // keystrokes are not echoing.
    emitPasswordLockTier(in, total, out);

    return static_cast<int>(total);
}

} // namespace Ubiquitty::Rendering
