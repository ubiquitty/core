// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Attributes/LinkRun.h"
#include "Core/Diagnostics/PerfDiag.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/OverlayAssembly.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Drawing.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ScrollSearch.h"
#include <span>

#include "Core/Cell.h"
#include "Core/CellColor.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Rendering/LinkColor.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>

namespace Ubiquitty::Rendering {

[[nodiscard]] static bool hasStoredImages(const Terminal& terminal)
{
    return std::ranges::any_of(terminal.images.stored, [](const Image::TerminalImage& image) {
        return image.used;
    });
}

/// The colour the selection's text is drawn in, and whether anything named one.
struct ResolvedSelectionForeground {
    Vec4 color { 0.0f, 0.0f, 0.0f, 1.0f };
    bool isOverridden = false;
};

/// Resolves the selection foreground a frame draws with.
///
/// The host's `OSC 19` wins over the frontend's own value, matching the choice
/// the background beneath it makes between a host colour and the built-in tint:
/// a reader who sets both is configuring one feature, and a foreground obeying
/// the frontend while the background obeyed the host is a split nothing outside
/// can predict. Channels divide by 65535 because that is the precision
/// `xparsecolor` carries and the last point at which the low byte still exists.
/// The result is opaque: a background is a wash over text and keeps the tint's
/// alpha, while a foreground is the ink itself with nothing behind it to show
/// through.
///
/// Resolved here rather than at the call site because the choice is a branch,
/// and `captureSource` keeps the block structure its PGO profile describes. The
/// caller stays straight-line and the rule still lives in one place.
///
/// - Parameters:
///   - source: The frame's model facts, carrying the host's stored colour.
///   - options: The frontend's own request, which stands when the host named none.
/// - Returns: The colour to draw the selection's text in, and whether to.
[[nodiscard]] __attribute__((noinline)) static ResolvedSelectionForeground resolveSelectionForeground(
    const FrameSource& source, const TerminalSnapshotOptions& options)
{
    if (source.hasSelectionForeground) {
        return { Vec4 { source.selectionForegroundRed / 65535.0f, source.selectionForegroundGreen / 65535.0f,
                     source.selectionForegroundBlue / 65535.0f, 1.0f },
            true };
    }
    return { options.selectionForeground, options.shouldOverrideSelectionForeground };
}

/// Resolves the selection fill from the frame's style and the host's colour.
///
/// - Parameters:
///   - options: The frame's style, carrying the configured style and the
///     platform colour.
///   - hasHostColor: Whether the host named a selection background.
///   - red: The host's red channel, 16-bit.
///   - green: The host's green channel, 16-bit.
///   - blue: The host's blue channel, 16-bit.
/// - Returns: The fill the capture hands the builder.
[[nodiscard]] static SelectionFill selectionFillFor(const TerminalSnapshotOptions& options, bool hasHostColor,
    std::uint16_t red, std::uint16_t green, std::uint16_t blue)
{
    // Divided by 65535 rather than narrowed to eight bits, which is the
    // precision xparsecolor carries and the last point at which the low byte
    // still exists.
    const Vec4 hostColor { red / 65535.0f, green / 65535.0f, blue / 65535.0f, 1.0f };
    return resolveSelectionFill(options.style.selectionStyle, options.style.platformSelectionColor,
        hasHostColor ? &hostColor : nullptr);
}

bool isFrameEdgeToEdge(const Ubiquitty::Terminal& terminal)
{
    return terminal.modes.fullscreenContent
        && terminal.fullscreenContentDisplay.letterbox == Ubiquitty::FullscreenContentLetterbox::Color;
}

std::optional<PaletteRGB> cursorTextColor(const Ubiquitty::Terminal& terminal)
{
    if (!terminal.hasCursorTextColor)
        return std::nullopt;
    return PaletteRGB { terminal.cursorTextRed, terminal.cursorTextGreen, terminal.cursorTextBlue };
}

void setCursorTextColor(CursorRenderState& cursor, std::optional<PaletteRGB> color)
{
    cursor.hasTextColor = color.has_value();
    const PaletteRGB text = color.value_or(PaletteRGB { 0, 0, 0 });
    cursor.textRGB = {
        channel16To8(text.red) / 255.0f,
        channel16To8(text.green) / 255.0f,
        channel16To8(text.blue) / 255.0f,
        1.0f,
    };
}

bool TerminalFrameInputs::capture(const Terminal& terminal, const TerminalSnapshotOptions& options, const CellMetrics& baseMetrics,
    const GlyphSlotGeometry& glyphSlot, ChromeLayers chrome)
{
    // A default refresh reads every row — the partial one below is the same
    // walk with rows allowed to survive.
    return capture(terminal, options, baseMetrics, glyphSlot, CapturedRowRefresh {}, chrome);
}

bool TerminalFrameInputs::capture(const Terminal& terminal, const TerminalSnapshotOptions& options, const CellMetrics& baseMetrics,
    const GlyphSlotGeometry& glyphSlot, const CapturedRowRefresh& refresh, ChromeLayers chrome)
{
    m_inputs = BuildInputs {};
    m_cursorPresentationFrame = CursorPresentation::Frame {};
    m_imageQuads.clear();
    m_richTextQuads.clear();
    m_richTextDrawings.clear();
    m_richTextHighlights.clear();
    m_hasRichTextCaret = false;
#if UBIQUITTY_PERF_DIAG
    m_copyStatistics = FrameCopyStatistics {};
#endif
    m_glyphSlot = glyphSlot;
    if (terminal.columns <= 0 || terminal.rows <= 0 || !terminal.screens.active) {
        m_terminalRowStorage = TerminalRowStorage::None;
        return false;
    }

    // The display rows are read here and not in the capture below, because
    // reading them is the part that needs the Terminal. A caller holding its own
    // copy calls the other overload and never enters this function.
    const std::size_t cellCount = static_cast<std::size_t>(terminal.rows) * terminal.columns;
    const std::size_t rowCount = static_cast<std::size_t>(terminal.rows);
    const bool isChromeActive = hasDrawnChrome(chrome);
    const bool hasStatusRow = options.statusRowCells != nullptr
        && options.statusRowColumns == terminal.columns;
    const bool needsPristineSource = isChromeActive || hasStatusRow;

    std::vector<Cell>* capturedCells = &m_cells;
    std::vector<std::uint8_t>* capturedLineAttributes = &m_lineAttributes;
    std::vector<std::uint8_t>* capturedRowMarks = &m_rowMarks;
    const bool hadTerminalLineage = m_terminalRowStorage != TerminalRowStorage::None;
    if (needsPristineSource) {
        // The composite mutates m_cells. On the first composite after an
        // ordinary frame, seed the scratch with that frame's pristine rows so
        // the incoming damage mask can refresh it incrementally.
        if (m_terminalRowStorage == TerminalRowStorage::Cells) {
            m_scratchCells = m_cells;
            m_scratchLineAttributes = m_lineAttributes;
            m_scratchRowMarks = m_rowMarks;
#if UBIQUITTY_PERF_DIAG
            m_copyStatistics.cellBytesCopied += cellCount * sizeof(Cell);
            m_copyStatistics.rowsCopied += static_cast<std::uint32_t>(rowCount);
#endif
        }
        capturedCells = &m_scratchCells;
        capturedLineAttributes = &m_scratchLineAttributes;
        capturedRowMarks = &m_scratchRowMarks;
    } else if (m_terminalRowStorage == TerminalRowStorage::Scratch) {
        // Leaving a composite frame promotes its pristine scratch directly to
        // the capture. The old composited frame becomes disposable scratch.
        m_cells.swap(m_scratchCells);
        m_lineAttributes.swap(m_scratchLineAttributes);
        m_rowMarks.swap(m_scratchRowMarks);
    }

    // Whether the selected storage already holds a frame of this shape, read ahead of the
    // resize that would make the question unanswerable. A partial refresh keeps
    // rows the drain called clean, and the drain checks its own row count — but a
    // grid that kept its rows and changed its column count reallocates here with
    // the row count still matching, so nothing downstream could tell that the
    // surviving rows describe a different width.
    const bool isSameShape = hadTerminalLineage && capturedCells->size() == cellCount
        && capturedLineAttributes->size() == rowCount
        && capturedRowMarks->size() == rowCount;
    // resize, not assign: a full walk overwrites every entry, and a partial one
    // is only reached when the shape did not change.
    capturedCells->resize(cellCount);
    capturedLineAttributes->resize(rowCount);
    capturedRowMarks->resize(rowCount);

    const int history = Screen::scrollbackCount(&terminal);
    const int viewOffset = std::clamp(options.viewOffset, 0, history);
    const int firstAbsoluteRow = history - viewOffset;

    // The view is re-derived rather than taken from the refresh, and a refresh
    // that names a different one loses its right to keep any row: the drain that
    // produced those dirty marks walked another set of lines, so a row it called
    // clean describes a line this frame does not show.
    CapturedRowRefresh rows = refresh;
    if (!isSameShape || rows.viewOffset != viewOffset || rows.firstAbsoluteRow != firstAbsoluteRow
        || (rows.baseGeneration != 0 && rows.baseGeneration != m_rowRefreshGeneration)) {
        rows.canReuseRows = false;
    }
    rows.viewOffset = viewOffset;
    rows.firstAbsoluteRow = firstAbsoluteRow;
    // Hover changes presentation without Core cell damage. Both the row copy
    // and the build must include those rows, even when another capture consumed
    // their model damage before this capture refreshed its cached cells.
    if (rows.canReuseRows && rows.rowDirty.size() == rowCount) {
        const SelectionFill selection = selectionFillFor(options, terminal.selectionBackgroundSet,
            terminal.selectionBackgroundRed, terminal.selectionBackgroundGreen, terminal.selectionBackgroundBlue);
        rows.rowDirty = { damageIncludingPresentation(rows.rowDirty.data(), options, terminal.rows,
                              hasStatusRow ? StatusRowPresence::Present : StatusRowPresence::Absent, selection, rows.scrollDelta),
            rowCount };
    }
    // One row walk for both entry points. `canReuseRows` false re-reads every
    // row, which is the full capture; true skips `getDisplayRow` on the clean
    // ones and shifts the surviving band when a scroll is pending. Line
    // attributes and marks are re-read either way, because both change without
    // the row's cells changing.
    // The flags below are indexed by row beside the cells, and the cells are
    // about to move: a scrolled band is shifted rather than re-read.
    shiftLinkFlagsForScroll(rows, terminal.rows, terminal.columns);
    m_didKeepCapturedRows = rows.canReuseRows;
#if UBIQUITTY_PERF_DIAG
    const FrameCopyStatistics rowStatistics = refreshCapturedRowsWithStatistics(
        terminal, rows, *capturedCells, *capturedLineAttributes, *capturedRowMarks);
    m_copyStatistics.cellBytesCopied += rowStatistics.cellBytesCopied;
    m_copyStatistics.rowsInspected += rowStatistics.rowsInspected;
    m_copyStatistics.rowsCopied += rowStatistics.rowsCopied;
#else
    refreshCapturedRows(terminal, rows, *capturedCells, *capturedLineAttributes, *capturedRowMarks);
#endif

    // One flag set per visible row, beside the attributes: live rows from
    // their metadata, history rows from their cells. A capture that copies
    // cells is already touching them, and the flag is what lets the
    // renderer's walk and a pointer's mapping skip every row without a run.
    m_rowFlags.resize(rowCount);
    for (int row = 0; row < terminal.rows; ++row)
        m_rowFlags[static_cast<std::size_t>(row)] = Screen::rowFlags(&terminal, firstAbsoluteRow + row);

    FrameSource source;
    source.cells = capturedCells->data();
    source.lineAttributes = capturedLineAttributes->data();
    source.rowFlags = m_rowFlags.data();
    // Marks live on the primary, so the alternate screen has none to pass.
    source.rowMarks = terminal.screens.isUsingAlternateScreen ? nullptr : capturedRowMarks->data();
    source.alternateActive = terminal.screens.isUsingAlternateScreen;
    source.rows = terminal.rows;
    source.columns = terminal.columns;
    source.scrollbackCount = history;
    source.modelGeneration = terminal.modelEpoch;
    source.palette = &terminal.palette;
    source.paletteGeneration = terminal.paletteEpoch;
    source.hasPaletteGeneration = true;
    source.textSizeTable = &terminal.textSizeTable;
    source.ambiguousWidth = terminal.ambiguousWidth;
    source.cursorColor = terminal.palette.cursor;
    source.hasSelectionBackground = terminal.selectionBackgroundSet;
    source.selectionBackgroundRed = terminal.selectionBackgroundRed;
    source.selectionBackgroundGreen = terminal.selectionBackgroundGreen;
    source.selectionBackgroundBlue = terminal.selectionBackgroundBlue;
    source.hasSelectionForeground = terminal.selectionForegroundSet;
    source.selectionForegroundRed = terminal.selectionForegroundRed;
    source.selectionForegroundGreen = terminal.selectionForegroundGreen;
    source.selectionForegroundBlue = terminal.selectionForegroundBlue;
    source.hasCursorTextColor = terminal.hasCursorTextColor;
    source.cursorTextColor = { terminal.cursorTextRed, terminal.cursorTextGreen, terminal.cursorTextBlue };
    source.isScreenReverse = isReverseVideo(&terminal);
    source.isColumns132 = terminal.modes.columns132;
    source.isScreenBlanked = terminal.screenBlanked;
    // Read here, beside the cells, because the caller holds the model lock for
    // exactly this call. A frame path that reads the mode again when it draws
    // reads it after the drawable wait, and the mode can be reset in that
    // window: fresh geometry then pairs with cells captured under the mode.
    source.isEdgeToEdge = isFrameEdgeToEdge(terminal);
    source.isFullscreenContentActive = terminal.modes.fullscreenContent;
    source.letterboxColor = terminal.fullscreenContentDisplay.letterboxColor;
    source.isCursorVisible = terminal.modes.isCursorVisible;
    source.isPasswordPromptActive = terminal.passwordPromptActive;
    source.activeMatchEpoch = Screen::ScrollSearch::activeMatchEpoch(terminal);
    source.cursorRow = terminal.screens.active->cursorY;
    source.cursorColumn = terminal.screens.active->cursorX;
    // Cursor/RenderState.h is where the shape and blink are decided — its
    // contract is that a frontend paints what it returns and makes no
    // cursor-related decision of its own. Hard coding a block here discards
    // DECSCUSR for every backend downstream: a program asking for a bar gets a
    // block and has no way to tell.
    source.cursorShape = Ubiquitty::Cursor::resolveAppearance(&terminal, 0).shape;
    // The glide policy rides with the shape for the same reason: it is the
    // terminal's configured answer, and an animator that held its own copy
    // would be a second settings store nobody remembers to fill.
    source.cursorMotionMode = terminal.cursorMotionModePref;
    source.cursorMotionDurationMilliseconds = terminal.cursorMotionDurationMilliseconds;
    source.images = hasStoredImages(terminal) ? terminal.images.stored.data() : nullptr;
    source.placements = terminal.images.livePlacementCount != 0 ? terminal.images.placements.data() : nullptr;
    // Reading a cell's full grapheme cluster needs the Terminal's cluster pool,
    // which the terminal holds and the builder does not, so the reader travels
    // with the source. A proportional run's anchor is read through the same
    // resolver, from the run-text store, so a terminal holding run text and no
    // pooled cluster needs it too.
    if (!terminal.clusterPool.empty() || terminal.runTextStore.liveCodePoints() != 0) {
        source.clusterResolver = [](const Cell* cell, std::span<std::uint32_t> codePoints, void* context) {
            return Ubiquitty::cellClusterCodePoints(static_cast<const Terminal*>(context), cell, codePoints.data(), static_cast<int>(codePoints.size()));
        };
        source.clusterResolverContext = const_cast<Terminal*>(&terminal);
    }
    // Borrowed for the call only — the capture takes its own serial-gated copy,
    // so this pointer does not have to outlive the caller's lock.
    source.softFont = Ubiquitty::softFontOrNull(terminal);
    const SourceRowStorage sourceStorage = needsPristineSource ? SourceRowStorage::Copy : SourceRowStorage::CaptureOwned;
    TerminalSnapshotOptions captureOptions = options;
    // Per-kind link styling for a caller that hands in the Terminal and no
    // resolver of its own. The lookup rides the same borrow the cluster
    // resolver above does; a caller's own resolver wins, so the flag fills the
    // field only when it is empty.
    if (options.shouldResolveLinkKindsFromSource && !options.linkKindResolver) {
        captureOptions.linkKindResolverContext = const_cast<Terminal*>(&terminal);
        captureOptions.linkKindResolver = [](std::uint16_t identifier, void* context) -> std::uint8_t {
            return static_cast<std::uint8_t>(
                Attributes::LinkRun::kindForLinkID(static_cast<const Terminal*>(context), identifier));
        };
    }
    // Only when the frame may trust the set. `BuildInputs::dirtyRows` says a
    // clean row emits nothing and that only a caller still holding that row's
    // previous output may set it — and `canReuseRows` false is exactly the
    // caller that does not: the target it would have kept is about to be
    // cleared. Forwarded unconditionally, a frame that refused the target built
    // only the marked rows onto a fresh texture, so an empty set drew an empty
    // screen and a set holding one row drew that row alone. Twenty lines up,
    // `refreshCapturedRows` asks the same question before it skips a row; this
    // is the second half of the same guard.
    if (refresh.canReuseRows && !refresh.rowDirty.empty()) {
        captureOptions.dirtyRows = rows.rowDirty.size() == rowCount
            ? rows.rowDirty.data()
            : nullptr;
    }
    // The terminal behind this capture, for the link kind that is resolved
    // whether or not the caller asked for per-kind styling; cleared with the
    // capture, since a source-only capture has no terminal to read.
    m_linkKindTerminal = &terminal;
    const bool didCapture = captureSource(source, captureOptions, baseMetrics, glyphSlot, sourceStorage, chrome);
    m_linkKindTerminal = nullptr;
#if UBIQUITTY_PERF_DIAG
    if (didCapture)
        notePaintHeat();
#endif
    if (didCapture && source.images) {
        m_imageQuads.resize(Image::maxTerminalImagePlacementCount);
        const int count = collectImageQuadsOrdered(&terminal,
            { m_metrics.cellWidth, m_metrics.cellHeight },
            { m_inputs.originInPixels.x, m_inputs.originInPixels.y },
            m_metrics.deviceScale, options.viewOffset, m_imageQuads,
            options.viewportSizeInPixels, m_metrics.presentationScale);
        m_imageQuads.resize(static_cast<std::size_t>(count));
    }
    if (didCapture && !terminal.richTextBlocks.isEmpty())
        collectRichTextQuads(terminal, options);
    m_rowRefreshGeneration = didCapture ? refresh.generationAfterDrain : 0;
    m_terminalRowStorage = didCapture
        ? (needsPristineSource ? TerminalRowStorage::Scratch : TerminalRowStorage::Cells)
        : TerminalRowStorage::None;
    return didCapture;
}

void TerminalFrameInputs::collectRichTextQuads(const Terminal& terminal, const TerminalSnapshotOptions& options)
{
    const float cellWidth = m_metrics.cellWidth;
    const float cellHeight = m_metrics.cellHeight;
    if (cellWidth <= 0 || cellHeight <= 0)
        return;
    const CellPalette palette = CellPalette::fromTerminalPalette(terminal.palette);
    const std::span<const RichText::BlockSelection> selections = options.richTextSelections.first(std::min(options.richTextSelections.size(), static_cast<std::size_t>(std::max(options.richTextSelectionCount, 0))));
    // The offset the cells are read at: one past the history shows the live
    // screen, and a block drawn at the raw offset would sit below its cells.
    const int viewOffset = Screen::resolvedViewOffset(&terminal, options.viewOffset);
    for (const RichText::Block& block : terminal.richTextBlocks.blocks(RichText::activeScreen(terminal))) {
        const int visibleTop = block.row + viewOffset;
        if (visibleTop + static_cast<int>(block.height) <= 0 || visibleTop >= terminal.rows)
            continue;
        const RichText::Layout& layout = RichText::layoutOf(terminal, block);
        auto& cursor = m_cursorPresentationFrame.cursor;
        if (block.caret != 65535 && m_cursorPresentationFrame.isEligible
            && block.covers(cursor.row - viewOffset, cursor.column)) {
            for (std::size_t index = 0; index < layout.visibleLineCount(); ++index) {
                const RichText::LineLayout line = layout.line(index);
                if (block.caret < line.first || block.caret > line.end)
                    continue;
                const float x = static_cast<float>(layout.caretX(line, block.caret)) / static_cast<float>(layout.cellWidth());
                cursor.richTextOffset = static_cast<float>(block.column) + x - static_cast<float>(cursor.column);
                // A native bar retains focus/blink behavior without re-emitting
                // the grid cell underneath the proportional glyph as a block.
                cursor.shape = Cursor::Shape::Bar;
                if (m_cursorPresentationFrame.isProgramVisible) {
                    m_inputs.cursor = cursor;
                    m_hasRichTextCaret = true;
                }
                break;
            }
        }
        RichText::BlockQuad quad;
        quad.serial = block.serial;
        quad.left = m_inputs.originInPixels.x + static_cast<float>(block.column) * cellWidth;
        quad.top = m_inputs.originInPixels.y + static_cast<float>(visibleTop) * cellHeight;
        quad.width = static_cast<float>(block.width) * cellWidth;
        quad.height = static_cast<float>(block.height) * cellHeight;
        quad.pixelsPerUnitX = cellWidth / static_cast<float>(layout.cellWidth());
        quad.pixelsPerUnitY = cellHeight / static_cast<float>(layout.cellHeight());
        quad.deviceScale = m_metrics.deviceScale;
        quad.underlineThicknessScale = options.style.underlineThicknessScale;
        quad.underlineOffset = options.style.underlineOffsetInPixels;
        quad.isSkippingInk = options.style.shouldUnderlineSkipInk ? 1 : 0;
        quad.skipInkPadding = m_metrics.skipInkPadding;
        // The placing pen lives in covered cells. A clipped block's origin
        // may belong to a menu, so sample the first visible covered rectangle.
        std::array<std::uint8_t, 3> foreground {};
        std::array<std::uint8_t, 3> background {};
        int colorRow = std::max(visibleTop, 0);
        int colorColumn = block.column;
        for (const auto& rectangle : block.visibleRects) {
            const int top = visibleTop + rectangle.y;
            if (top + rectangle.height <= 0 || top >= terminal.rows)
                continue;
            colorRow = std::max(top, 0);
            colorColumn += rectangle.x;
            break;
        }
        if (const Cell* cell = Screen::visibleCell(&terminal, viewOffset, colorRow, colorColumn))
            palette.resolve(cellPen(*cell), ReverseMode::Normal, foreground.data(), background.data());
        quad.red = foreground[0];
        quad.green = foreground[1];
        quad.blue = foreground[2];
        // Behind the text the band is the colour a selected cell beside the
        // block takes, washed over the block's background by the builder's
        // own rule, so the block's highlight and the grid's meet in one tone.
        const Vec4 fill = m_inputs.selectionColor;
        if (m_inputs.isSelectionBehindText) {
            const auto wash = [&](std::uint8_t channel, float selected) {
                const float ground = static_cast<float>(channel) / 255.0f;
                return ground + (selected - ground) * fill.w;
            };
            quad.highlightRed = wash(background[0], fill.x);
            quad.highlightGreen = wash(background[1], fill.y);
            quad.highlightBlue = wash(background[2], fill.z);
            quad.highlightAlpha = 1.0f;
        } else {
            quad.highlightRed = fill.x;
            quad.highlightGreen = fill.y;
            quad.highlightBlue = fill.z;
            quad.highlightAlpha = fill.w;
        }
        std::uint64_t key = 0xcbf29ce484222325ull;
        auto mix = [&key](std::uint64_t value) {
            key ^= value;
            key *= 0x100000001b3ull;
        };
        mix(block.serial);
        mix(terminal.typography.epoch);
        mix((std::uint64_t { foreground[0] } << 16) | (std::uint64_t { foreground[1] } << 8) | foreground[2]);
        mix(std::bit_cast<std::uint32_t>(cellWidth));
        mix(std::bit_cast<std::uint32_t>(cellHeight));
        // The rule settings are drawn into the raster.
        mix(std::bit_cast<std::uint32_t>(quad.deviceScale));
        mix(std::bit_cast<std::uint32_t>(quad.underlineThicknessScale));
        mix(std::bit_cast<std::uint32_t>(quad.underlineOffset));
        mix(std::bit_cast<std::uint32_t>(quad.skipInkPadding));
        mix(quad.isSkippingInk);
        quad.contentKey = key;
        for (const RichText::BlockSelection& selection : selections) {
            if (selection.serial != block.serial)
                continue;
            if (block.visibleRects.empty()) {
                RichText::appendHighlights(layout, selection, quad, m_richTextHighlights);
                continue;
            }
            std::vector<RichText::HighlightQuad> highlights;
            RichText::appendHighlights(layout, selection, quad, highlights);
            for (const auto& highlight : highlights) {
                for (const auto& rectangle : block.visibleRects) {
                    const float left = std::max(highlight.left, quad.left + rectangle.x * cellWidth);
                    const float top = std::max(highlight.top, quad.top + rectangle.y * cellHeight);
                    const float right = std::min(highlight.left + highlight.width, quad.left + (rectangle.x + rectangle.width) * cellWidth);
                    const float bottom = std::min(highlight.top + highlight.height, quad.top + (rectangle.y + rectangle.height) * cellHeight);
                    if (right > left && bottom > top) {
                        RichText::HighlightQuad clipped = highlight;
                        clipped.left = left;
                        clipped.top = top;
                        clipped.width = right - left;
                        clipped.height = bottom - top;
                        m_richTextHighlights.push_back(clipped);
                    }
                }
            }
        }
        m_richTextDrawings.push_back(terminal.richTextLayouts.drawingOf(block, terminal.typography));
        m_richTextQuads.push_back(quad);
    }
}

bool TerminalFrameInputs::capture(const FrameSource& source, const TerminalSnapshotOptions& options, const CellMetrics& baseMetrics,
    const GlyphSlotGeometry& glyphSlot, ChromeLayers chrome)
{
    m_terminalRowStorage = TerminalRowStorage::None;
#if UBIQUITTY_PERF_DIAG
    m_copyStatistics = FrameCopyStatistics {};
#endif
    m_didKeepCapturedRows = true;
    const bool didCapture = captureSource(source, options, baseMetrics, glyphSlot, SourceRowStorage::Copy, chrome);
#if UBIQUITTY_PERF_DIAG
    if (didCapture) {
        m_copyStatistics.rowsInspected = static_cast<std::uint32_t>(source.rows);
        notePaintHeat();
    }
#endif
    return didCapture;
}

void TerminalFrameInputs::notePaintHeat()
{
    if (m_paintHeatShadow.isEnabled())
        m_paintHeatShadow.noteCapturedGrid(m_inputs.cells, m_inputs.rows, m_inputs.columns);
}

void TerminalFrameInputs::notePaintHeatOf(const TerminalFrameInputs& shown)
{
    if (m_paintHeatShadow.isEnabled())
        m_paintHeatShadow.noteCapturedGrid(shown.m_inputs.cells, shown.m_inputs.rows, shown.m_inputs.columns);
}

void TerminalFrameInputs::shiftLinkFlagsForScroll(const CapturedRowRefresh& refresh, int rows, int columns)
{
    if (!refresh.canReuseRows || refresh.scrollDelta == 0 || rows <= 0 || columns <= 0)
        return;
    if (m_linkCells.size() != static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns))
        return;
    if (refresh.scrollTop < 0 || refresh.scrollBottom < refresh.scrollTop || refresh.scrollBottom >= rows)
        return;
    const int shift = refresh.scrollDelta > 0 ? refresh.scrollDelta : -refresh.scrollDelta;
    if (shift > refresh.scrollBottom - refresh.scrollTop)
        return;
    const std::size_t rowBytes = static_cast<std::size_t>(columns);
    if (refresh.scrollDelta > 0) {
        for (int row = refresh.scrollTop; row <= refresh.scrollBottom - shift; ++row)
            std::memmove(m_linkCells.data() + static_cast<std::size_t>(row) * columns,
                m_linkCells.data() + static_cast<std::size_t>(row + shift) * columns, rowBytes);
    } else {
        for (int row = refresh.scrollBottom; row >= refresh.scrollTop + shift; --row)
            std::memmove(m_linkCells.data() + static_cast<std::size_t>(row) * columns,
                m_linkCells.data() + static_cast<std::size_t>(row - shift) * columns, rowBytes);
    }
}

void TerminalFrameInputs::markBakedSelectionRows(std::span<const SelectionRowSpan> selection, SelectionRecolor recolor,
    int scrollDelta, int rowCount)
{
    const auto mark = [&](int row) {
        if (row >= 0 && row < rowCount)
            m_effectiveDirtyRows[static_cast<std::size_t>(row)] = 1;
    };
    // A recoloured selection, or one under a retained target that shifts
    // before this frame draws, owes every row either frame holds it on: the
    // shifted pixels carry the old selection to rows the diff below would
    // call unchanged. The shifted positions are marked as well as the
    // unshifted ones, since the shift applies only inside the scroll region.
    if (recolor == SelectionRecolor::Changed || scrollDelta != 0) {
        for (const SelectionRowSpan& span : m_lastBakedSelectionSpans) {
            mark(span.visibleRow);
            mark(span.visibleRow - scrollDelta);
        }
        for (const SelectionRowSpan& span : selection)
            mark(span.visibleRow);
        return;
    }
    // Otherwise only the rows whose columns or cuts differ: a drag that grows
    // the selection by a row repaints that row and the one it left, not every
    // row it holds; one moving a letter inside a run's cell repaints that row.
    //
    // A row held in several pieces, one each side of a rich text block or one
    // for a sized block the selection touches, is judged on its pieces as a
    // set: the per-row columns below keep only one piece of a row.
    constexpr std::size_t fieldsPerRow = 6;
    m_bakedSelectionPieceCount.assign(static_cast<std::size_t>(rowCount), 0);
    const auto countPieces = [&](std::span<const SelectionRowSpan> spans) {
        for (const SelectionRowSpan& span : spans) {
            if (span.visibleRow < 0 || span.visibleRow >= rowCount)
                continue;
            std::uint8_t& pieces = m_bakedSelectionPieceCount[static_cast<std::size_t>(span.visibleRow)];
            if (pieces < 2)
                ++pieces;
        }
    };
    // Whether `piece` is one of `spans`, all its fields equal.
    const auto isPieceHeld = [](std::span<const SelectionRowSpan> spans, const SelectionRowSpan& piece) {
        return std::ranges::any_of(spans, [&piece](const SelectionRowSpan& held) { return held == piece; });
    };
    const std::span<const SelectionRowSpan> lastSpans(m_lastBakedSelectionSpans);
    countPieces(lastSpans);
    countPieces(selection);
    const auto isMultiPieceRow = [&](int row) { return m_bakedSelectionPieceCount[static_cast<std::size_t>(row)] > 1; };
    m_bakedSelectionColumnScratch.assign(static_cast<std::size_t>(rowCount) * fieldsPerRow, -1);
    for (const SelectionRowSpan& span : m_lastBakedSelectionSpans) {
        if (span.visibleRow < 0 || span.visibleRow >= rowCount || isMultiPieceRow(span.visibleRow))
            continue;
        const std::size_t index = static_cast<std::size_t>(span.visibleRow) * fieldsPerRow;
        m_bakedSelectionColumnScratch[index] = span.column0;
        m_bakedSelectionColumnScratch[index + 1] = span.column1;
        m_bakedSelectionColumnScratch[index + 2] = span.subColumn0;
        m_bakedSelectionColumnScratch[index + 3] = span.subColumn1;
        m_bakedSelectionColumnScratch[index + 4] = span.firstLetter;
        m_bakedSelectionColumnScratch[index + 5] = span.endLetter;
    }
    for (const SelectionRowSpan& span : selection) {
        if (span.visibleRow < 0 || span.visibleRow >= rowCount)
            continue;
        if (isMultiPieceRow(span.visibleRow)) {
            if (!isPieceHeld(lastSpans, span))
                mark(span.visibleRow);
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(span.visibleRow) * fieldsPerRow;
        if (m_bakedSelectionColumnScratch[index] != span.column0 || m_bakedSelectionColumnScratch[index + 1] != span.column1
            || m_bakedSelectionColumnScratch[index + 2] != span.subColumn0 || m_bakedSelectionColumnScratch[index + 3] != span.subColumn1
            || m_bakedSelectionColumnScratch[index + 4] != span.firstLetter || m_bakedSelectionColumnScratch[index + 5] != span.endLetter)
            mark(span.visibleRow);
        // Matched or marked, the row is settled; what is left afterwards is a
        // row the selection left.
        m_bakedSelectionColumnScratch[index] = -1;
        m_bakedSelectionColumnScratch[index + 1] = -2;
    }
    for (const SelectionRowSpan& span : lastSpans) {
        if (span.visibleRow >= 0 && span.visibleRow < rowCount && isMultiPieceRow(span.visibleRow) && !isPieceHeld(selection, span))
            mark(span.visibleRow);
    }
    for (int row = 0; row < rowCount; ++row) {
        if (m_bakedSelectionColumnScratch[static_cast<std::size_t>(row) * fieldsPerRow + 1] >= 0)
            mark(row);
    }
}

const std::uint8_t* TerminalFrameInputs::damageIncludingPresentation(const std::uint8_t* damage,
    const TerminalSnapshotOptions& options, int rowCount, StatusRowPresence statusRow,
    const SelectionFill& selection, int scrollDelta)
{
    const std::span<const SelectionRowSpan> bakedSelection = selection.isBehindText && options.selectionSpans
        ? std::span(options.selectionSpans, static_cast<std::size_t>(std::max(options.selectionSpanCount, 0)))
        : std::span<const SelectionRowSpan> {};
    // The cuts count: a drag moving a letter inside one cell of a run
    // changes what the row bakes and no column.
    const auto isSameSpan = [](const SelectionRowSpan& a, const SelectionRowSpan& b) { return a == b; };
    const bool didBakedSelectionColorChange = !bakedSelection.empty() && !m_lastBakedSelectionSpans.empty()
        && (selection.color.x != m_lastBakedSelectionColor.x || selection.color.y != m_lastBakedSelectionColor.y
            || selection.color.z != m_lastBakedSelectionColor.z);
    const bool didBakedSelectionChange = didBakedSelectionColorChange
        || (scrollDelta != 0 && !(bakedSelection.empty() && m_lastBakedSelectionSpans.empty()))
        || !std::ranges::equal(bakedSelection, m_lastBakedSelectionSpans, isSameSpan);

    const bool didHoverChange = options.isHoverLinkActive != m_lastCaptureHoverLinkActive
        || options.hoverLinkID != m_lastCaptureHoverLinkID
        || options.hoverLinkRowFirst != m_lastCaptureHoverLinkRowFirst
        || options.hoverLinkRowLast != m_lastCaptureHoverLinkRowLast
        || options.hoverLinkColumnFirst != m_lastCaptureHoverLinkColumnFirst
        || options.hoverLinkColumnLast != m_lastCaptureHoverLinkColumnLast
        || options.hoverLinkSpanFirst != m_lastCaptureHoverLinkSpanFirst
        || options.hoverLinkSpanLast != m_lastCaptureHoverLinkSpanLast;
    const bool didLinkPresentationChange = options.shouldUnderlineLinks != m_lastCaptureUnderlinedLinks;
    // A status row that vanished since the last capture is copied below, but no
    // Core damage names it: the mask has to, or a retained frame keeps the row
    // the status line covered.
    const bool didStatusRowVanish = m_lastCaptureHadStatusRow && statusRow == StatusRowPresence::Absent && rowCount > 0;
    // The published set is the caller's verdict against the frame it
    // presented, not this capture's own lineage: a capture without lineage
    // copies every cell and still hands the builder and the sync record the
    // rows that changed.
    const std::uint8_t* buildDamage = damage;
    if (damage
        && (didLinkPresentationChange || (options.shouldUnderlineLinks && didHoverChange) || didStatusRowVanish
            || didBakedSelectionChange)) {
        if (damage != m_effectiveDirtyRows.data()) {
            m_effectiveDirtyRows.assign(damage, damage + rowCount);
        }
        if (didLinkPresentationChange) {
            std::ranges::fill(m_effectiveDirtyRows, std::uint8_t { 1 });
        } else {
            auto markHoverRows = [&](bool active, int first, int last) {
                if (!active)
                    return;
                first = std::max(first, 0);
                last = std::min(last, rowCount - 1);
                for (int row = first; row <= last; ++row)
                    m_effectiveDirtyRows[static_cast<std::size_t>(row)] = 1;
            };
            markHoverRows(m_lastCaptureHoverLinkActive, m_lastCaptureHoverLinkRowFirst, m_lastCaptureHoverLinkRowLast);
            markHoverRows(options.isHoverLinkActive, options.hoverLinkRowFirst, options.hoverLinkRowLast);
        }
        if (didStatusRowVanish)
            m_effectiveDirtyRows[static_cast<std::size_t>(rowCount - 1)] = 1;
        if (didBakedSelectionChange)
            markBakedSelectionRows(bakedSelection,
                didBakedSelectionColorChange ? SelectionRecolor::Changed : SelectionRecolor::Unchanged, scrollDelta, rowCount);
        buildDamage = m_effectiveDirtyRows.data();
    }
    return buildDamage;
}

bool TerminalFrameInputs::captureSource(const FrameSource& source, const TerminalSnapshotOptions& options, const CellMetrics& baseMetrics,
    const GlyphSlotGeometry& glyphSlot, SourceRowStorage sourceStorage, ChromeLayers chrome)
{
    m_inputs = BuildInputs {};
    m_cursorPresentationFrame = CursorPresentation::Frame {};
    m_imageQuads.clear();
    m_richTextQuads.clear();
    m_richTextDrawings.clear();
    m_richTextHighlights.clear();
    m_hasRichTextCaret = false;
    m_glyphSlot = glyphSlot;
    if (source.columns <= 0 || source.rows <= 0 || !source.cells || !source.lineAttributes) {
        m_hasFullLineage = false;
        return false;
    }

    captureViewTiers(options);
    m_modelGeneration = source.modelGeneration;
    const bool hadPalette = m_hasPalette;
    const bool hadPaletteGeneration = m_hasPaletteGeneration;
    const std::uint64_t previousPaletteGeneration = m_paletteGeneration;
    m_hasPalette = source.palette != nullptr;
    m_hasPaletteGeneration = source.hasPaletteGeneration;
    m_paletteGeneration = source.paletteGeneration;
    const bool canReusePalette = m_hasPalette && hadPalette && source.hasPaletteGeneration
        && hadPaletteGeneration && source.paletteGeneration == previousPaletteGeneration;
    if (m_hasPalette && !canReusePalette) {
        m_palette = *source.palette;
        m_paletteLinkColor.reset();
    }
    m_hasTextSizeTable = source.textSizeTable != nullptr;
    if (m_hasTextSizeTable) {
        m_textSizeTable = *source.textSizeTable;
    }

    // The geometry describes the atlas, so every capture carries it. A capture
    // that dropped it would report no margin, and the glyph pass would fall
    // back to the cell-baked path with the overhang clipped.
    m_inputs.glyphMarginInPixels.width = static_cast<float>(m_glyphSlot.glyphMarginInPixels.width);
    m_inputs.glyphMarginInPixels.height = static_cast<float>(m_glyphSlot.glyphMarginInPixels.height);
    m_inputs.glyphSlotWidthInPixels = static_cast<float>(m_glyphSlot.slotWidthInPixels);
    m_inputs.glyphSlotHeightInPixels = static_cast<float>(m_glyphSlot.slotHeightInPixels);
    m_inputs.glyphReferenceAdvanceInPixels = m_glyphSlot.referenceAdvanceInPixels;
    m_inputs.glyphReferenceLineHeightInPixels = m_glyphSlot.referenceLineHeightInPixels;
    m_inputs.glyphWideSlotWidthInPixels = static_cast<float>(m_glyphSlot.wideSlotWidthInPixels);
    m_inputs.glyphWideSlotHeightInPixels = static_cast<float>(m_glyphSlot.wideSlotHeightInPixels);
    m_inputs.glyphDoubleSlotWidthInPixels = static_cast<float>(m_glyphSlot.doubleSlotWidthInPixels);
    m_inputs.glyphDoubleSlotHeightInPixels = static_cast<float>(m_glyphSlot.doubleSlotHeightInPixels);
    m_inputs.glyphBoxWidthInPixels = static_cast<float>(m_glyphSlot.glyphWidthInPixels);
    m_inputs.ligatureParticipation = m_glyphSlot.ligatureParticipation;
    m_inputs.boxStrokeProfile = m_glyphSlot.boxStrokeProfile;
    m_inputs.subpixelGlyphs = options.shouldUseSubpixelGlyphs
        && (m_glyphSlot.glyphMarginInPixels.width > 0 || m_glyphSlot.glyphMarginInPixels.height > 0);
    m_inputs.subpixelPhaseCount = std::clamp(options.subpixelPhaseCount, 1, subpixelPhaseCount);
    m_inputs.widePhaseCount = std::clamp(options.widePhaseCount, 0, subpixelPhaseCount);
    m_inputs.subpixelPhaseRounding = options.shouldFloorSubpixelPhase ? PhaseRounding::Floor : PhaseRounding::Nearest;
    m_inputs.hasRasterTiers = options.hasRasterTiers;

    // The selection travels with the frame, so every renderer built from this
    // capture highlights the same cells. The tint is filled in here rather than
    // asked of the caller: the builder's default is a fully transparent quad, so
    // a frontend that supplies spans and no colour emits the overlay on every
    // frame and paints nothing. Spans without a colour is never what a caller
    // means.
    m_inputs.selectionSpans = m_selectionSpans.empty() ? nullptr : m_selectionSpans.data();
    m_inputs.selectionSpanCount = static_cast<int>(m_selectionSpans.size());
    // Derived from the capture, not supplied beside it. DECSET 7402's Color
    // letterbox decides both, and the capture resolved that under the model
    // lock — so a caller that also carried them would be the second channel the
    // style comment warns about, filled from a different frame than the cells.
    // The alpha is opaque here rather than asked for: the letterbox is the
    // window the terminal is not using, and showing the desktop through it is
    // the artefact the mode exists to remove.
    m_inputs.isGridOccluded = source.isEdgeToEdge;
    m_inputs.occludingColor = Vec4 {
        static_cast<float>((source.letterboxColor >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((source.letterboxColor >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(source.letterboxColor & 0xFFu) / 255.0f,
        1.0f
    };
    // A host naming a selection foreground with OSC 19 is what activates the
    // override, so the stored colour is read rather than left to each frontend:
    // an override filled from options alone answers with the frontend's own
    // state and drops the host's colour, which is the shape that left OSC 19
    // reaching the screen through one platform's walk and inert everywhere the
    // capture draws. The rule itself sits in resolveSelectionForeground, which
    // is where its reasoning is written.
    const ResolvedSelectionForeground ink = resolveSelectionForeground(source, options);
    m_inputs.selectionForegroundOverride = ink.isOverridden;
    m_inputs.selectionForeground = ink.color;
    // The fill is resolved whether or not anything is selected, because the
    // damage below compares it against the last frame's: a selection that was
    // baked and is now cleared still owes the rows it covered.
    const SelectionFill selection = selectionFillFor(options, source.hasSelectionBackground,
        source.selectionBackgroundRed, source.selectionBackgroundGreen, source.selectionBackgroundBlue);
    if (!m_selectionSpans.empty() || options.richTextSelectionCount > 0) {
        m_inputs.selectionColor = selection.color;
        m_inputs.isSelectionBehindText = selection.isBehindText;
    }

    const std::size_t cellCount = static_cast<std::size_t>(source.rows) * source.columns;
    const int viewOffset = std::clamp(options.viewOffset, 0, std::max(source.scrollbackCount, 0));
    const bool chromeActive = hasDrawnChrome(chrome);
    // The status line replaces the last visible row; the full statement of why
    // sits at the overwrite below. Decided here because the copy needs it: a
    // status row that deactivated leaves that row holding chrome the source
    // never carried, so a partial copy must restore it whether or not the
    // caller's damage names it.
    const bool hasStatusRow = options.statusRowCells != nullptr
        && options.statusRowColumns == source.columns
        && source.rows > 0;
    const int statusRow = hasStatusRow ? source.rows - 1 : -1;
    // A clean row's cells are byte-identical to the source's while the lineage
    // holds — same shape, same view offset, every skipped row covered by the
    // previous capture, nothing composited over it since — so a caller that
    // states its damage pays the copy for the damaged rows alone. Everything
    // else, including a caller passing no damage, copies whole: a grid that
    // shrank must not keep stale cells past the new extent, and one that grew
    // must start from the source rather than whatever the previous capture
    // left.
    const std::uint8_t* const damage = options.dirtyRows;
    const bool canPartialCopy = m_hasFullLineage && damage != nullptr
        && source.rows == m_capturedRows && source.columns == m_capturedColumns
        && viewOffset == m_viewOffset;

    const std::uint8_t* buildDamage = damageIncludingPresentation(damage, options, source.rows,
        hasStatusRow ? StatusRowPresence::Present : StatusRowPresence::Absent, selection, 0);
    // The copy consults the verdict only under this capture's own lineage;
    // without it every row is copied. Read by the copy and nothing after it.
    const std::uint8_t* frameDamage = canPartialCopy ? buildDamage : nullptr;

    if (sourceStorage == SourceRowStorage::CaptureOwned) {
        assert(source.cells == m_cells.data());
        assert(source.lineAttributes == m_lineAttributes.data());
        assert(source.rowMarks == nullptr || source.rowMarks == m_rowMarks.data());
    } else if (canPartialCopy) {
        const std::size_t rowBytes = static_cast<std::size_t>(source.columns) * sizeof(Cell);
        for (int row = 0; row < source.rows; ++row) {
            const bool restoresStatusRow = m_lastCaptureHadStatusRow && !hasStatusRow && row == source.rows - 1;
            if (!frameDamage[row] && !restoresStatusRow)
                continue;
            std::memcpy(m_cells.data() + static_cast<std::size_t>(row) * source.columns,
                source.cells + static_cast<std::size_t>(row) * source.columns, rowBytes);
#if UBIQUITTY_PERF_DIAG
            m_copyStatistics.cellBytesCopied += rowBytes;
            ++m_copyStatistics.rowsCopied;
#endif
#if UBIQUITTY_ENABLE_TEST_HOOKS
            m_sourceCellCopyCountForTest += static_cast<std::size_t>(source.columns);
#endif
        }
    } else {
        m_cells.assign(source.cells, source.cells + cellCount);
#if UBIQUITTY_PERF_DIAG
        m_copyStatistics.cellBytesCopied += cellCount * sizeof(Cell);
        m_copyStatistics.rowsCopied += static_cast<std::uint32_t>(source.rows);
#endif
#if UBIQUITTY_ENABLE_TEST_HOOKS
        m_sourceCellCopyCountForTest += cellCount;
#endif
    }

    // The published set, republished as storage this capture owns:
    // `options.dirtyRows` belongs to the caller and the inputs outlive the call.
    if (buildDamage) {
        m_capturedDirtyRows.assign(buildDamage, buildDamage + source.rows);
        buildDamage = m_capturedDirtyRows.data();
    } else {
        m_capturedDirtyRows.clear();
    }
    if (sourceStorage == SourceRowStorage::Copy) {
        m_lineAttributes.assign(source.lineAttributes, source.lineAttributes + source.rows);
        if (source.rowFlags && source.rowFlags != m_rowFlags.data())
            m_rowFlags.assign(source.rowFlags, source.rowFlags + source.rows);
        else if (!source.rowFlags)
            m_rowFlags.clear();
        if (source.rowMarks)
            m_rowMarks.assign(source.rowMarks, source.rowMarks + source.rows);
    }

    // The soft font, copied only when it CHANGED. `serial` advances once per
    // committed DECDLD download (Core/VT/SoftFont.h), so a session that draws
    // DRCS every frame copies the matrix once and a session that never
    // downloaded one — serial 0 — allocates nothing at all. Copying per frame
    // would put ~4.7KB of memcpy on the render path to reproduce bytes that had
    // not moved since the last download.
    // The serial the capture is leaving behind is reported so the builder can
    // release that font's atlas slots — they name glyphs nothing can ask for
    // again, and a stream of downloads would otherwise spend the shared atlas on
    // them one dead set at a time.
    const std::uint32_t softFontSerial = source.softFont != nullptr ? source.softFont->serial : 0;
    const std::uint32_t previousSoftFontSerial = m_softFont ? m_softFont->serial : 0;
    m_retiredSoftFontSerial = previousSoftFontSerial != softFontSerial ? previousSoftFontSerial : 0;
    if (softFontSerial == 0) {
        m_softFont.reset();
    } else if (!m_softFont || m_softFont->serial != softFontSerial) {
        if (!m_softFont)
            m_softFont = std::make_unique<Ubiquitty::VT::SoftFont>();
        *m_softFont = *source.softFont;
    }

    m_viewOffset = viewOffset;
    m_firstAbsoluteRow = std::max(source.scrollbackCount, 0) - m_viewOffset;

    // Chrome over the captured grid, before anything reads it. After the row
    // walk so it wins over the cells it covers, and before the digests so a
    // covered row's hash includes it.
    // In list order, so the last layer draws on top: a palette over a hint,
    // because an open palette is what the user is looking at.
    for (const ChromePlacement& layer : chrome) {
        if (layer.canvas != nullptr && layer.canvas->rows > 0)
            composite(m_cells.data(), source.rows, source.columns, *layer.canvas, layer.originRow, layer.originColumn);
    }

    // The status line replaces the last visible row. After the chrome composite,
    // because chrome is drawn over the frame and a popover covering the bottom row
    // covers the status line with it; before every consumer below, so none of them
    // needs to know a status line exists.
    //
    // A width mismatch is refused rather than clamped: a status array shorter than
    // the row is a caller mid-resize, and painting a partial status line over live
    // grid content would show two frames' worth of the screen at once. Decided
    // above, ahead of the copy.
    // Published for the overlay assembly's cursor suppression: the membership
    // rule lives there, and a capture that derived the row without saying so
    // left the assembly's gate permanently open — the data cursor could draw
    // onto the status row on the arm that draws through the assembly.
    m_inputs.hasStatusLineOnBottomRow = hasStatusRow;
    if (hasStatusRow) {
        std::copy(options.statusRowCells, options.statusRowCells + source.columns,
            m_cells.begin() + static_cast<std::ptrdiff_t>(statusRow) * source.columns);
#if UBIQUITTY_PERF_DIAG
        m_copyStatistics.cellBytesCopied += static_cast<std::uint64_t>(source.columns) * sizeof(Cell);
        ++m_copyStatistics.rowsCopied;
#endif
        // The status line is single-width and single-height whatever the
        // application left on that row, so a DECDWL there must not scale it.
        m_lineAttributes[static_cast<std::size_t>(statusRow)] = lineAttributeNormal;
    }

    // Which cells carry the OSC 8 / regex link affordance. Read from the
    // composited cells, so a cell the chrome covers is flagged from what the
    // frame shows: a link under an opaque popover is not reachable, and
    // underlining it would draw an affordance for a target no click can hit.
    //
    // Empty when the setting is off: `BuildInputs::linkCells` reads null as "no
    // link styling", so the whole per-cell branch disappears on the frames of
    // every user who turned it off, and the buffer is never sized.
    // A hovered run is drawn only over the text it was resolved on. Output can
    // move or rewrite those cells between the resolve and this frame, and a run
    // drawn by its rows alone would then underline whatever text arrived there.
    // A run without a digest, or one partly outside the visible rows, is drawn
    // as its rows say, since this frame cannot read the text to check.
    bool isHoverVerified = options.isHoverLinkActive;
    if (isHoverVerified && options.hoverLinkTextDigest != 0 && options.hoverLinkRowFirst >= 0
        && options.hoverLinkRowLast < source.rows && options.hoverLinkRowFirst <= options.hoverLinkRowLast) {
        const std::uint64_t digest = linkRunTextDigest(options.hoverLinkRowFirst, options.hoverLinkRowLast,
            options.hoverLinkColumnFirst, options.hoverLinkColumnLast, options.hoverLinkSpanFirst,
            options.hoverLinkSpanLast, source.columns, [&](int row, int column) {
                return m_cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(source.columns) + static_cast<std::size_t>(column)].codePoint;
            });
        isHoverVerified = digest == options.hoverLinkTextDigest;
    }
    if (options.shouldUnderlineLinks) {
        // The scan follows the effective damage gate: clean rows retain their
        // prior flags, while a hover move contributes its old and new rows. A
        // run whose verification flipped rescans whole, so no row keeps the
        // emphasis of a run this frame refuses.
        // The status row is scanned whenever it is composited: its cells come
        // from the options, not the grid, so the damage mask cannot name them.
        // The scan reuses a row's flags only where this capture kept that row's
        // cells. `dirtyRows` answers a question about the retained target the
        // frontend holds, and a capture that re-reads every row still forwards
        // it, so a scan keyed on it alone leaves a row the flags of whatever
        // line the previous capture found there.
        const bool canPartialScan = canPartialCopy && m_didKeepCapturedRows && m_lastCaptureUnderlinedLinks
            && m_linkCells.size() == cellCount && isHoverVerified == m_lastCaptureHoverTextVerified;
        if (!canPartialScan)
            m_linkCells.assign(cellCount, 0);
        for (int row = 0; row < source.rows; ++row) {
            if (canPartialScan && !buildDamage[row] && row != statusRow)
                continue;
            const HoverColumnBand band = (isHoverVerified && row != statusRow)
                ? hoverColumnBand(row, options.hoverLinkRowFirst, options.hoverLinkRowLast,
                      options.hoverLinkColumnFirst, options.hoverLinkColumnLast,
                      options.hoverLinkSpanFirst, options.hoverLinkSpanLast, source.columns)
                : HoverColumnBand {};
            const std::size_t rowBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(source.columns);
            const Cell* const cellRow = m_cells.data() + rowBase;
            std::uint8_t* const flagRow = m_linkCells.data() + rowBase;
            for (int column = 0; column < source.columns; ++column) {
                const std::uint16_t identifier = cellRow[column].linkID();
                // Hover wins where both hold. A cell can carry an OSC 8 id and sit
                // under the pointer, and the hovered presentation is the one the
                // reader is being shown the run by.
                //
                // A cell with no id still hovers: a regex or path link has no OSC 8
                // id at all, so a rule keyed on the id would leave every detected
                // URL unhighlighted under the pointer — which is most of what the
                // hover affordance is for.
                const bool isHovered = linkHoverContainsCell(isHoverVerified, options.hoverLinkID, column,
                    band.first, band.last, identifier);
                // Hover outranks the kind: the hovered run reads solid in the accent
                // whatever scheme it points at, so resolving a kind for it would be
                // work whose answer is discarded.
                LinkState state = isHovered ? LinkState::Hovered
                    : identifier != 0       ? LinkState::Resting
                                            : LinkState::None;
                if (identifier != 0 && (options.linkKindResolver || m_linkKindTerminal)) {
                    // The caller's resolver is per-kind styling asked for; the
                    // terminal behind the capture answers otherwise, for the one
                    // kind that is not a style: an action is what the cell is,
                    // whatever the setting.
                    const bool isStyledPerKind = options.linkKindResolver != nullptr;
                    const auto kind = static_cast<Attributes::LinkRun::Kind>(isStyledPerKind
                            ? options.linkKindResolver(identifier, options.linkKindResolverContext)
                            : static_cast<std::uint8_t>(Attributes::LinkRun::kindForLinkID(m_linkKindTerminal, identifier)));
                    switch (kind) {
                    case Attributes::LinkRun::Kind::File:
                        if (state == LinkState::Resting && isStyledPerKind)
                            state = LinkState::RestingFile;
                        break;
                    case Attributes::LinkRun::Kind::Mailto:
                        if (state == LinkState::Resting && isStyledPerKind)
                            state = LinkState::RestingMailto;
                        break;
                    case Attributes::LinkRun::Kind::Action:
                        state = isHovered ? LinkState::HoveredAction : LinkState::RestingAction;
                        break;
                    case Attributes::LinkRun::Kind::URL:
                    case Attributes::LinkRun::Kind::Other:
                        break;
                    }
                }
                flagRow[column] = static_cast<std::uint8_t>(state);
            }
        }
    } else {
        m_linkCells.clear();
    }
    m_lastCaptureHoverTextVerified = isHoverVerified;

    captureClusters(source);
    captureImages(source);

    // The user's rule style. A backend that builds its own BuildInputs applies
    // the same function rather than a copy of it — the Vulkan grid path does.
    m_metrics = baseMetrics;
    applyRuleStyle(m_metrics, options.style);

    m_inputs.cells = m_cells.data();
    m_inputs.rows = source.rows;
    m_inputs.columns = source.columns;
    // The letter record follows the grid's shape; a shape change forgets every
    // row, since no row of the old shape stands where it did.
    const std::size_t letterCount = static_cast<std::size_t>(source.rows) * static_cast<std::size_t>(source.columns);
    if (m_letters.size() != letterCount || m_letterRows.size() != static_cast<std::size_t>(source.rows)) {
        m_letters.assign(letterCount, LetterExtent {});
        m_letterRows.assign(static_cast<std::size_t>(source.rows), 0u);
    }
    m_inputs.palette = m_hasPalette ? &m_palette : nullptr;
    m_inputs.textSizeTable = m_hasTextSizeTable ? &m_textSizeTable : nullptr;
    m_inputs.ambiguousWidth = source.ambiguousWidth;
    m_inputs.screenReverse = source.isScreenReverse;
    m_inputs.brightenBold = options.style.shouldBrightenBold;
    m_inputs.shouldEnhanceTextContrast = options.style.shouldEnhanceTextContrast;
    m_inputs.usesNativeTextVibrancy = options.style.usesNativeTextVibrancy;
    m_inputs.preservesBackgroundTints = options.style.preservesBackgroundTints;
    m_inputs.usesNativeBackgroundVibrancy = options.style.usesNativeBackgroundVibrancy;
    m_inputs.nativeSecondaryInk = options.style.nativeSecondaryInk;
    m_inputs.nativeTertiaryInk = options.style.nativeTertiaryInk;
    m_inputs.viewportSizeInPixels = { options.viewportSizeInPixels.width, options.viewportSizeInPixels.height };
    m_inputs.isColumns132 = source.isColumns132;
    m_inputs.isScreenBlanked = source.isScreenBlanked;
    m_inputs.metrics = m_metrics;
    m_inputs.lineAttributes = m_lineAttributes.data();
    m_inputs.rowFlags = m_rowFlags.size() == static_cast<std::size_t>(source.rows) ? m_rowFlags.data() : nullptr;
    m_inputs.dirtyRows = buildDamage;
    // The builder's gate is the absence of marks rather than a flag it would
    // have to be told to honour, so a source on the alternate screen passes none
    // and nothing downstream needs to know which screen it was.
    m_inputs.promptMarks.rowMarks = source.rowMarks ? m_rowMarks.data() : nullptr;
    m_inputs.linkCells = m_linkCells.empty() ? nullptr : m_linkCells.data();
    m_inputs.softFont = m_softFont.get();
    m_inputs.retiredSoftFontSerial = m_retiredSoftFontSerial;
    m_inputs.linkUnderlineStyle = options.linkUnderlineStyle;
    m_inputs.linkUnderlineStyleFile = options.linkUnderlineStyleFile;
    m_inputs.linkUnderlineStyleMailto = options.linkUnderlineStyleMailto;
    // The link colour is the palette's unless the frontend named one: the same
    // blue every arm resolves from the theme, so the portable renderer and the
    // Linux arm, which have no accent to hand in, draw the link a macOS window
    // draws rather than a black one. A hover colour left unset is the resting
    // colour, so a hovered run on such an arm still reads as a link.
    const Vec4 paletteLinkColor = [&] {
        static const bool shouldLogMetrics = std::getenv("UBIQUITTY_METRICS_LOG") != nullptr;
        if (options.linkColor.w > 0.0f && !shouldLogMetrics)
            return Vec4 {};
        const auto to8 = [](const PaletteRGB& c) { return ColorRGBA { channel16To8(c.red), channel16To8(c.green), channel16To8(c.blue), 255 }; };
        if (!m_paletteLinkColor) {
            m_paletteLinkColor = resolveLinkColor(to8(m_palette.indexed[4]), to8(m_palette.indexed[12]), to8(m_palette.defaultBackground), to8(m_palette.defaultForeground));
#if UBIQUITTY_ENABLE_TEST_HOOKS
            ++m_linkColorResolveCountForTest;
#endif
        }
        const ColorRGBA link = *m_paletteLinkColor;
        if (shouldLogMetrics) {
            const ColorRGBA b = to8(m_palette.indexed[4]), bb = to8(m_palette.indexed[12]), bg = to8(m_palette.defaultBackground), fg = to8(m_palette.defaultForeground);
            std::fprintf(stderr, "metrics-log link blue=%d,%d,%d bright=%d,%d,%d bg=%d,%d,%d fg=%d,%d,%d -> %d,%d,%d\n", b.red, b.green, b.blue, bb.red, bb.green, bb.blue, bg.red, bg.green, bg.blue, fg.red, fg.green, fg.blue, link.red, link.green, link.blue);
        }
        return Vec4 { link.red / 255.0f, link.green / 255.0f, link.blue / 255.0f, 1.0f };
    }();
    m_inputs.linkColor = options.linkColor.w > 0.0f ? options.linkColor : paletteLinkColor;
    m_inputs.linkHoverColor = options.linkHoverColor.w > 0.0f ? options.linkHoverColor : m_inputs.linkColor;
    m_inputs.promptMarks.fontSize = options.promptMarkFontSize;
    m_inputs.promptMarks.ink = options.style.promptMarkInk;
    m_inputs.promptMarks.fontAscent = options.promptMarkFontAscent;
    m_inputs.promptMarks.fontDescent = options.promptMarkFontDescent;
    m_inputs.promptMarks.lineGapTopInPixels = options.promptMarkLineGapTopInPixels;
    m_inputs.promptMarks.bracketInkTopInPixels = options.promptMarkBracketInkTopInPixels;
    m_inputs.promptMarks.bracketInkBottomInPixels = options.promptMarkBracketInkBottomInPixels;
    m_inputs.promptMarks.xHeight = options.promptMarkFontXHeight;
    m_inputs.promptMarks.capHeight = options.promptMarkFontCapHeight;
    m_inputs.promptMarks.atlasScale = options.promptMarkAtlasScale;
    // The bitmap tier's mark is sized by the bitmap face on every arm: the
    // face metrics the software renderer draws its brackets from, not the
    // point size and the CoreText ascent a Metal arm folded for its atlas.
    // Fed those, the same bracket came out shorter and heavier there, and the
    // tier's one golden could not hold.
    if (options.style.unifontScale > 0) {
        const FaceMetrics face = unifontFaceMetricsForScale(options.style.unifontScale,
            static_cast<int>(std::lround(options.lineSpacingPoints * baseMetrics.deviceScale)));
        m_inputs.promptMarks.fontSize = face.fontSize;
        m_inputs.promptMarks.fontAscent = face.ascent;
        m_inputs.promptMarks.fontDescent = face.descent;
        m_inputs.promptMarks.lineGapTopInPixels = face.lineGapTopInPixels;
        m_inputs.promptMarks.bracketInkTopInPixels = face.bracketInkTopInPixels;
        m_inputs.promptMarks.bracketInkBottomInPixels = face.bracketInkBottomInPixels;
        // The bitmap face reports no x-height or cap height, so its pills
        // stay on the cell's box on every arm, and the tier keeps agreeing
        // with itself.
        m_inputs.promptMarks.xHeight = 0.0f;
        m_inputs.promptMarks.capHeight = 0.0f;
    }
    // Two plain stores rather than one short-circuit. The option is the frontend
    // saying it has no view layer to draw a badge in and the source field is the
    // terminal saying there is a prompt to draw one for, and the assembly is
    // where membership is decided — so folding them here would move that
    // decision, and would add a branch to a function the profile fills with
    // seventeen million counters, which detaches the whole of it.
    m_inputs.shouldDrawPasswordLock = options.shouldDrawPasswordLock;
    m_inputs.isPasswordPromptActive = source.isPasswordPromptActive;
    m_inputs.images = m_hasImages ? m_images.data() : nullptr;
    m_inputs.imageCount = m_hasImages ? Image::maxTerminalImageCount : 0;
    m_inputs.placements = m_hasPlacements ? m_placements.data() : nullptr;
    m_inputs.placementCount = m_hasPlacements ? Image::maxTerminalImagePlacementCount : 0;
    m_inputs.alternateActive = source.alternateActive;
    m_inputs.viewOffset = m_viewOffset;
    m_inputs.backgroundOpacity = std::clamp(options.style.backgroundOpacity, 0.0f, 1.0f);
    // The per-index opacity is the captured palette's own, assembled with
    // every capture from the live palette rather than registered by a
    // frontend at theme apply: an OSC 4 write that made a slot opaque, or an
    // OSC 104 that gave the theme's dimming back, is what this frame draws.
    m_inputs.paletteForegroundAlpha = m_hasPalette ? m_palette.indexedAlpha.data() : nullptr;
    m_inputs.defaultForegroundAlpha = options.style.defaultForegroundAlpha;
    m_inputs.uniformBackgroundOpacity = options.style.uniformBackgroundOpacity;
    m_inputs.uniformDefaultBackground = options.style.uniformDefaultBackground;
    m_inputs.isEdgeToEdge = source.isEdgeToEdge;
    m_inputs.isFullscreenContentActive = source.isFullscreenContentActive;
    // The style's unifont scale is Core's routing verdict for the configured
    // family (`unifontScaleForConfiguredFamily`): positive for a face Core
    // rasterises itself, zero for a CoreText or FreeType face.
    m_inputs.isBitmapFace = options.style.unifontScale > 0;
    m_inputs.drawsBrailleGrid = options.style.shouldDrawBrailleGrid;
    m_inputs.brailleStyle = options.style.brailleStyle;
    m_inputs.shadeStyle = options.style.shadeStyle;
    m_inputs.resolveBlink = options.shouldResolveBlink;
    m_inputs.blinkTicks = options.blinkTicks;
    // The frontend reports where its padding puts the grid; whether the frame
    // keeps that padding is decided here, beside the letterbox, from the same
    // source. Under the colour letterbox the frame has none: the fill reaches
    // the drawable's edge on every arm, whichever origin the caller registered
    // ahead of the capture — the Swift arm's boundary route registers its
    // padded origin on the ingest thread, long before any frame reads the mode.
    m_inputs.originInPixels = { options.originInPixels.x, options.originInPixels.y };
    if (source.isEdgeToEdge)
        m_inputs.originInPixels = { 0.0f, 0.0f };

    // The cursor sits at a screen row; scrolling back moves it down the visible
    // window and past its bottom edge, so it is drawn only while it lands inside.
    //
    // Point-space line geometry, set for every frame rather than with the
    // cursor. The prompt-mark pass scales its brackets by cellHeightPoints too,
    // and a program that hides the cursor — every fullscreen TUI does — still
    // draws marks. Leaving these to the cursor branch sized those brackets off
    // a zero height, and only on the frames where the cursor was absent.
    m_inputs.cellHeightPoints = options.cellHeightPoints;
    m_inputs.lineSpacingPoints = options.lineSpacingPoints;
    m_inputs.toolkitCaret = options.toolkitCaret;

    // Highlight spans are frame state, not cursor state: a fullscreen TUI
    // hides the cursor on the very frames a user searches over, and spans
    // copied inside the cursor branch vanished exactly then — search, trigger
    // and hilite tiers dropping whenever the cursor did.
    m_inputs.searchMatchSpans = m_searchMatchSpans.empty() ? nullptr : m_searchMatchSpans.data();
    m_inputs.searchMatchSpanCount = static_cast<int>(m_searchMatchSpans.size());
    m_inputs.searchMatchHighlight = options.searchMatchHighlight;
    // The pill's bounce clock: the search state's epoch against the frame tick,
    // so a match's arrival is Core's reading and not a frontend's timer.
    m_inputs.focusedMatchAgeSeconds = m_focusedMatchBounce.ageSeconds(source.activeMatchEpoch, options.blinkTicks);
    m_inputs.otherSearchMatchSpans = m_otherSearchMatchSpans.empty() ? nullptr : m_otherSearchMatchSpans.data();
    m_inputs.otherSearchMatchSpanCount = static_cast<int>(m_otherSearchMatchSpans.size());
    m_inputs.triggerHighlightSpans = m_triggerHighlightSpans.empty() ? nullptr : m_triggerHighlightSpans.data();
    m_inputs.triggerHighlightSpanCount = static_cast<int>(m_triggerHighlightSpans.size());
    m_inputs.hiliteRegionSpans = m_hiliteRegionSpans.empty() ? nullptr : m_hiliteRegionSpans.data();
    m_inputs.hiliteRegionSpanCount = static_cast<int>(m_hiliteRegionSpans.size());
    m_inputs.bellFlashAlpha = options.bellFlashAlpha;
    m_inputs.backgroundImage = options.backgroundImage;

    // The blink phase is not sampled here. This capture has no clock and must be
    // deterministic — the same source must produce the same frame — so the
    // weight arrives as options.cursorMixAlpha from a caller that owns a frame
    // clock, defaulting to the solid phase.
    const int visibleCursorRow = source.cursorRow + m_viewOffset;
    // Motion and presentation consume the captured policy even when DECTCEM
    // hides the cursor, independently of the blink clock.
    m_cursorMotionMode = source.cursorMotionMode;
    m_cursorMotionDurationMilliseconds = source.cursorMotionDurationMilliseconds;
    // A cursor landing on the status row is not drawn. The row shows chrome the
    // terminal wrote, so the application's cursor is not addressing anything there
    // — and a block baked into a status cell would sit on text the application
    // cannot see or move away from.
    m_cursorPresentationFrame.geometry = {
        .rows = source.rows,
        .columns = source.columns,
        .viewOffset = m_viewOffset,
        .cellWidth = m_metrics.cellWidth,
        .cellHeight = m_metrics.cellHeight,
        .originX = m_inputs.originInPixels.x,
        .originY = m_inputs.originInPixels.y,
        .viewportWidth = options.viewportSizeInPixels.width,
        .viewportHeight = options.viewportSizeInPixels.height,
        .isAlternateActive = source.alternateActive,
        .hasStatusLineOnBottomRow = m_inputs.hasStatusLineOnBottomRow,
    };
    m_cursorPresentationFrame.isProgramVisible = source.isCursorVisible;
    if (options.drawCursor && visibleCursorRow >= 0 && visibleCursorRow < source.rows
        && source.cursorColumn >= 0 && source.cursorColumn < source.columns
        && visibleCursorRow != statusRow) {
        m_cursorPresentationFrame.isEligible = true;
        auto& presentationCursor = m_cursorPresentationFrame.cursor;
        presentationCursor.row = visibleCursorRow;
        presentationCursor.column = source.cursorColumn;
        presentationCursor.shape = source.cursorShape;
        presentationCursor.focused = options.isFocused;
        presentationCursor.mixA = options.cursorMixAlpha;
        presentationCursor.shouldInvertText = options.shouldCursorInvertText;
        presentationCursor.isSliding = options.isCursorSliding;
        presentationCursor.columnFractional = options.cursorColumnFractional;
        presentationCursor.vrowFractional = options.cursorVRowFractional;
        // Read from the same per-row table the frontends' encodes use, so a doubled row
        // cannot scale its glyphs and its cursor differently.
        presentationCursor.lineAttribute = m_lineAttributes[static_cast<std::size_t>(visibleCursorRow)];
        const PaletteRGB cursor = source.cursorColor;
        presentationCursor.rgb = {
            channel16To8(cursor.red) / 255.0f,
            channel16To8(cursor.green) / 255.0f,
            channel16To8(cursor.blue) / 255.0f,
            options.cursorAlpha,
        };
        setCursorTextColor(presentationCursor, source.hasCursorTextColor ? std::optional<PaletteRGB>(source.cursorTextColor) : std::nullopt);
        if (source.isCursorVisible) {
            m_inputs.cursor = presentationCursor;
        }
    }

    m_capturedRows = source.rows;
    m_capturedColumns = source.columns;
    m_lastCaptureHadStatusRow = hasStatusRow;
    m_lastCaptureUnderlinedLinks = options.shouldUnderlineLinks;
    m_lastCaptureHoverLinkActive = options.isHoverLinkActive;
    m_lastCaptureHoverLinkID = options.hoverLinkID;
    m_lastCaptureHoverLinkRowFirst = options.hoverLinkRowFirst;
    m_lastCaptureHoverLinkRowLast = options.hoverLinkRowLast;
    m_lastCaptureHoverLinkColumnFirst = options.hoverLinkColumnFirst;
    m_lastCaptureHoverLinkColumnLast = options.hoverLinkColumnLast;
    m_lastCaptureHoverLinkSpanFirst = options.hoverLinkSpanFirst;
    m_lastCaptureHoverLinkSpanLast = options.hoverLinkSpanLast;
    if (m_inputs.isSelectionBehindText) {
        m_lastBakedSelectionSpans = m_selectionSpans;
        m_lastBakedSelectionColor = m_inputs.selectionColor;
    } else {
        m_lastBakedSelectionSpans.clear();
    }
    // Chrome writes rows the damage mask does not name, so a chrome frame
    // leaves no lineage and the next capture copies whole.
    m_hasFullLineage = !chromeActive;
    return true;
}

void TerminalFrameInputs::captureViewTiers(const TerminalSnapshotOptions& options)
{
    const auto copySpans = [](const SelectionRowSpan* spans, int count, std::vector<SelectionRowSpan>& destination) {
        if (!spans || count <= 0) {
            destination.clear();
            return;
        }
        destination.assign(spans, spans + count);
    };

    copySpans(options.selectionSpans, options.selectionSpanCount, m_selectionSpans);
    copySpans(options.searchMatchSpans, options.searchMatchSpanCount, m_searchMatchSpans);
    copySpans(options.otherSearchMatchSpans, options.otherSearchMatchSpanCount, m_otherSearchMatchSpans);
    copySpans(options.triggerHighlightSpans, options.triggerHighlightSpanCount, m_triggerHighlightSpans);
    copySpans(options.hiliteRegionSpans, options.hiliteRegionSpanCount, m_hiliteRegionSpans);
}

void TerminalFrameInputs::captureClusters(const FrameSource& source)
{
    m_clusterOffsets.clear();
    m_clusterLengths.clear();
    m_clusterCodePoints.clear();

    if (!source.clusterResolver) {
        m_inputs.clusterResolver = nullptr;
        m_inputs.clusterResolverContext = nullptr;
        return;
    }

    const auto firstOverflow = std::ranges::find_if(m_cells, [](const Cell& cell) {
        return cell.clusterID() != 0;
    });
    if (firstOverflow == m_cells.end()) {
        m_inputs.clusterResolver = nullptr;
        m_inputs.clusterResolverContext = nullptr;
        return;
    }

    constexpr std::uint32_t noCluster = std::numeric_limits<std::uint32_t>::max();
    m_clusterOffsets.assign(m_cells.size(), noCluster);
    m_clusterLengths.assign(m_cells.size(), 0);

    // Room for a whole proportional run, which its anchor resolves to; an
    // ordinary cluster fills sixteen of it at most. Kept across frames, since
    // it is more than a stack frame should carry.
    thread_local std::vector<std::uint32_t> codePoints(static_cast<std::size_t>(runTextMaximumCodePoints));
    for (std::size_t index = static_cast<std::size_t>(firstOverflow - m_cells.begin()); index < m_cells.size(); ++index) {
        if (m_cells[index].clusterID() == 0) {
            continue;
        }
        int length = source.clusterResolver(&m_cells[index], codePoints, source.clusterResolverContext);
        length = std::clamp(length, 0, runTextMaximumCodePoints);
        if (length == 0) {
            continue;
        }
        assert(m_clusterCodePoints.size() <= std::numeric_limits<std::uint32_t>::max());
        m_clusterOffsets[index] = static_cast<std::uint32_t>(m_clusterCodePoints.size());
        m_clusterLengths[index] = static_cast<std::uint16_t>(length);
        m_clusterCodePoints.insert(m_clusterCodePoints.end(), codePoints.begin(), codePoints.begin() + length);
    }

    m_inputs.clusterResolver = resolveCapturedCluster;
    m_inputs.clusterResolverContext = this;
}

int TerminalFrameInputs::resolveCapturedCluster(const Cell* cell, std::span<std::uint32_t> codePoints, void* context)
{
    if (!cell || !context || codePoints.empty()) {
        return 0;
    }
    const auto* capture = static_cast<const TerminalFrameInputs*>(context);
    const Cell* const begin = capture->m_cells.data();
    const Cell* const end = begin + capture->m_cells.size();
    const std::less<const Cell*> isBefore;
    if (isBefore(cell, begin) || !isBefore(cell, end)) {
        return 0;
    }
    const std::size_t cellIndex = static_cast<std::size_t>(cell - begin);
    if (cellIndex >= capture->m_clusterOffsets.size() || cellIndex >= capture->m_clusterLengths.size()) {
        return 0;
    }
    constexpr std::uint32_t noCluster = std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t offset = capture->m_clusterOffsets[cellIndex];
    const std::size_t storedLength = capture->m_clusterLengths[cellIndex];
    if (offset == noCluster || offset > capture->m_clusterCodePoints.size()
        || storedLength > capture->m_clusterCodePoints.size() - offset) {
        return 0;
    }
    const std::size_t copiedLength = std::min(storedLength, codePoints.size());
    std::ranges::copy_n(capture->m_clusterCodePoints.begin() + offset, copiedLength, codePoints.begin());
    return static_cast<int>(copiedLength);
}

void TerminalFrameInputs::captureImages(const FrameSource& source)
{
    const bool hadImages = m_hasImages;
    m_hasImages = source.images != nullptr;
    if (m_hasImages) {
#if UBIQUITTY_ENABLE_TEST_HOOKS
        ++m_imageTableMutationCountForTest;
#endif
        for (std::size_t index = 0; index < m_images.size(); ++index) {
            const Image::TerminalImage& incoming = source.images[index];
            const Image::TerminalImage previous = m_images[index];
            std::vector<unsigned char>& pixels = m_imagePixels[index];
            Image::TerminalImage captured = incoming;
            captured.frames = {};

            const bool hasValidPixels = incoming.used && incoming.pixels && incoming.pixelLength > 0
                && incoming.pixelLength <= static_cast<std::uint32_t>(Image::maxDecodedImageBytes);
            if (!hasValidPixels) {
                captured.pixels = nullptr;
                captured.pixelLength = 0;
                m_images[index] = captured;
                continue;
            }

            const bool canReusePixels = previous.used
                && previous.contentGeneration == incoming.contentGeneration
                && previous.currentFrame == incoming.currentFrame
                && previous.pixelLength == incoming.pixelLength
                && pixels.size() == incoming.pixelLength;
            if (!canReusePixels) {
                pixels.assign(incoming.pixels, incoming.pixels + incoming.pixelLength);
#if UBIQUITTY_PERF_DIAG
                m_copyStatistics.imageBytesCopied += incoming.pixelLength;
#endif
            }
            captured.pixels = pixels.data();
            if (captured.currentFrame < captured.frames.size()) {
                Image::TerminalImageFrame current {};
                if (incoming.currentFrame < incoming.frames.size()) {
                    current = incoming.frames[incoming.currentFrame];
                }
                current.used = true;
                current.pixelLength = captured.pixelLength;
                current.pixels = captured.pixels;
                captured.frames[captured.currentFrame] = current;
            }
            m_images[index] = captured;
        }
    } else if (hadImages) {
        m_images = {};
#if UBIQUITTY_ENABLE_TEST_HOOKS
        ++m_imageTableMutationCountForTest;
#endif
    }

    const bool hadPlacements = m_hasPlacements;
    m_hasPlacements = source.placements != nullptr;
    if (m_hasPlacements) {
        std::copy_n(source.placements, m_placements.size(), m_placements.begin());
#if UBIQUITTY_ENABLE_TEST_HOOKS
        ++m_placementTableMutationCountForTest;
#endif
    } else if (hadPlacements) {
        m_placements = {};
#if UBIQUITTY_ENABLE_TEST_HOOKS
        ++m_placementTableMutationCountForTest;
#endif
    }
}

CursorMotionTarget resolveCursorMotionTarget(const Terminal& terminal, int viewOffset, bool drawCursor) // bool-ok: mirrors TerminalSnapshotOptions::drawCursor, the bool the options already carry
{
    CursorMotionTarget target;
    target.mode = terminal.cursorMotionModePref;
    target.durationMilliseconds = terminal.cursorMotionDurationMilliseconds;
    target.column = terminal.screens.active->cursorX;
    // The capture's own arithmetic, so the two cannot disagree about which
    // cell the caret is on: the offset clamped to the history that exists,
    // the screen row moved down by it, and a row outside the window drawing
    // nothing.
    const int history = std::max(Screen::scrollbackCount(&terminal), 0);
    const int visibleRow = terminal.screens.active->cursorY + std::clamp(viewOffset, 0, history);
    if (drawCursor && terminal.modes.isCursorVisible && visibleRow >= 0 && visibleRow < terminal.rows)
        target.visibleRow = visibleRow;
    return target;
}

} // namespace Ubiquitty::Rendering
