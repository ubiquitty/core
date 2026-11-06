// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/TerminalFrameInputs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace Ubiquitty::Rendering {

/// How much of a captured frame the builder rebuilds.
enum class UBIQUITTY_CLOSED_ENUM BuildScope : std::uint8_t {
    /// Only the rows the record's mask names; a null mask builds every row.
    Damaged,
    /// Every row, whatever the mask says: the consumer will not Load the
    /// retained grid, so a clean row would exist nowhere.
    Whole,
};

/// Whether a frame record replaces its baseline or updates selected rows.
enum class UBIQUITTY_CLOSED_ENUM FrameRecordKind : std::uint8_t {
    Full,
    Delta,
};

/// The immutable identity and lineage carried beside one captured frame.
struct UBIQUITTY_ESCAPABLE FrameRecordHeader {
    /// The publication sequence; zero is reserved for an unpublished capture.
    std::uint64_t sequence = 0;
    /// The sequence a delta updates; zero for a full frame.
    std::uint64_t baseSequence = 0;
    /// The terminal model generation represented by the capture.
    std::uint64_t modelGeneration = 0;
    /// The palette generation represented by the capture.
    std::uint64_t paletteGeneration = 0;
    /// The registered view geometry generation represented by the capture.
    std::uint64_t viewGeneration = 0;
    /// The blink clock tick associated with the capture.
    std::uint32_t captureTicks = 0;
    /// Whether the record replaces or updates its baseline.
    FrameRecordKind kind = FrameRecordKind::Full;
};

class FrameCapture;
}

/// Claims a reference to `capture` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else; a
/// namespace-scoped pair is reported as missing rather than as misplaced.
///
/// - Parameter capture: The capture to retain. A null pointer is ignored.
void retainFrameCapture(Ubiquitty::Rendering::FrameCapture*);

/// Drops a reference to `capture`, destroying it at zero.
///
/// - Parameter capture: The capture to release. A null pointer is ignored.
void releaseFrameCapture(Ubiquitty::Rendering::FrameCapture*);

namespace Ubiquitty::Rendering {

/// A frame's worth of terminal state, held across frames so its storage is
/// reused, and turned into instances without its caller naming the inputs.
///
/// `TerminalFrameInputs` is the capture; this owns one and hands out neither it
/// nor the `BuildInputs` inside it. That is what makes the whole path reachable
/// from a language that cannot hold a non-escapable value: a caller captures,
/// then builds, and the borrowed spans never cross a seam.
///
/// A C++ frontend has no reason to prefer this over holding a
/// `TerminalFrameInputs` directly — three do, and they stay as they are. This
/// exists for a caller that must own the capture without being able to name it.
class UBIQUITTY_ESCAPABLE FrameCapture {
public:
    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    /// Releases the captured frame and its owned storage.
    ~FrameCapture() { delete m_frame; }

    /// Captures `terminal` at `options.viewOffset`.
    ///
    /// Reuses the storage the previous capture allocated, which is the whole
    /// reason a renderer holds one of these rather than building a capture per
    /// frame.
    ///
    /// - Parameters:
    ///   - terminal: The terminal to read.
    ///   - options: The frame's view offset, style and cursor decisions.
    ///   - baseMetrics: From the rasterizer that will fault the glyphs, so
    ///     decorations are drawn at the geometry the glyphs were rasterized at.
    ///   - glyphSlot: The atlas slot layout.
    ///   - chrome: In-grid chrome to composite over the frame, or null.
    /// - Returns: Whether the capture holds a frame. A degenerate terminal
    ///   captures nothing and reports false.
    [[nodiscard]] bool capture(const Terminal& terminal, const TerminalSnapshotOptions& options,
        const CellMetrics& baseMetrics, const GlyphSlotGeometry& glyphSlot, ChromeLayers chrome = {})
    {
        if (!m_frame->capture(terminal, options, baseMetrics, glyphSlot, chrome)) {
            return false;
        }
        FrameDamage damage;
        commitAutomaticRecord(options, CapturedRowRefresh {}, damage);
        return true;
    }

    /// Captures `terminal`, re-reading only the rows `refresh` marks dirty.
    ///
    /// The partial-frame entry for exactly the caller this class exists for: one
    /// that owns the capture without being able to name it, and therefore cannot
    /// hold the cell buffer the two C++ frontends refresh themselves. The rows
    /// this skips are the per-cell cost; the verdict and the dirty set come from
    /// ``planRetainedFrame``, which is the same plan the encode half reads.
    ///
    /// A refresh whose view disagrees with `options`, or whose grid changed
    /// shape, is demoted to a full re-read inside — so a caller cannot keep a
    /// row by passing a stale plan.
    ///
    /// - Parameters:
    ///   - terminal: The terminal to read.
    ///   - options: The frame's view offset, style and cursor decisions.
    ///   - baseMetrics: From the rasterizer that will fault the glyphs.
    ///   - glyphSlot: The atlas slot layout.
    ///   - refresh: The drain's verdict and dirty rows.
    ///   - chrome: In-grid chrome to composite over the frame, or null.
    /// - Returns: Whether the capture holds a frame.
    [[nodiscard]] bool capture(const Terminal& terminal, const TerminalSnapshotOptions& options,
        const CellMetrics& baseMetrics, const GlyphSlotGeometry& glyphSlot, const CapturedRowRefresh& refresh,
        ChromeLayers chrome = {})
    {
        if (!m_frame->capture(terminal, options, baseMetrics, glyphSlot, refresh, chrome)) {
            return false;
        }
        FrameDamage damage;
        damage.canReuse = refresh.canReuseRows;
        damage.scrollTop = refresh.scrollTop;
        damage.scrollBottom = refresh.scrollBottom;
        damage.scrollDelta = refresh.scrollDelta;
        damage.generationAfterDrain = refresh.generationAfterDrain;
        commitAutomaticRecord(options, refresh, damage);
        return true;
    }

    /// Captures a record with publisher-assigned identity and lineage.
    ///
    /// A requested delta whose damage span cannot name every visible row is
    /// promoted to a full frame. This makes malformed incremental metadata
    /// conservative at the ownership boundary instead of publishing stale
    /// rows a consumer cannot detect.
    ///
    /// - Parameters:
    ///   - terminal: The terminal to read.
    ///   - options: The frame's view offset, style, and cursor decisions.
    ///   - baseMetrics: The rasterizer metrics to style.
    ///   - glyphSlot: The atlas slot layout.
    ///   - header: The publisher-assigned record identity.
    ///   - damage: The damage verdict represented by the record.
    ///   - refresh: The rows to retain or copy into the capture.
    ///   - chrome: In-grid chrome to composite over the frame, or null.
    /// - Returns: Whether the capture holds a frame record.
    [[nodiscard]] bool captureRecord(const Terminal& terminal, const TerminalSnapshotOptions& options,
        const CellMetrics& baseMetrics, const GlyphSlotGeometry& glyphSlot, const FrameRecordHeader& header,
        const FrameDamage& damage, const CapturedRowRefresh& refresh, ChromeLayers chrome = {})
    {
        if (!m_frame->capture(terminal, options, baseMetrics, glyphSlot, refresh, chrome)) {
            return false;
        }
        commitRecord(header, damage);
        return true;
    }

    /// Builds the captured frame into `out`.
    ///
    /// The counts describe what the frame needs, not what was written: a stream
    /// whose buffer was too small reports the count it would have filled, so a
    /// caller grows and builds again rather than discovering a short frame by
    /// looking at it.
    ///
    /// - Parameters:
    ///   - backend: The glyph backend the builder faults misses through.
    ///   - out: The instance buffers to fill.
    /// - Returns: How many instances each stream needs.
    [[nodiscard]] InstanceCounts build(const GlyphBackend& backend, const InstanceBuffers& out, BuildScope scope = BuildScope::Damaged) const
    {
        BuildInputs inputs = m_frame->inputs();
        if (scope == BuildScope::Whole)
            inputs.dirtyRows = nullptr;
        InstanceBuffers withStorage = out;
        lendLetterStorage(withStorage);
        return buildCellInstances(inputs, backend, withStorage);
    }

    /// The view tiers a build composites over a capture taken without them.
    ///
    /// A boundary capture cannot carry these: they are per-view state whose
    /// spans are owned by the frame the VIEW is building, not by the terminal
    /// the boundary froze — a selection tracks the live drag over the last
    /// coherent grid, and freezing it at the boundary would draw the artefact,
    /// not prevent one. The grid content stays the capture's; only the tiers
    /// named here are overridden.
    struct UBIQUITTY_NONESCAPABLE ViewTierOverrides {
        /// Selected on-screen row spans, in visible rows.
        const SelectionRowSpan* selectionSpans = nullptr;
        int selectionSpanCount = 0;
        /// The selection tint, RGBA 0..1.
        Vec4 selectionColor { 0.0f, 0.0f, 0.0f, 0.0f };
        /// Whether `selectionColor` fills the selected cells under their glyphs.
        bool isSelectionBehindText = false;
        /// Whether selected glyphs draw in `selectionForeground`.
        bool hasSelectionForegroundOverride = false;
        /// The forced selected-glyph ink, read under the override.
        Vec4 selectionForeground { 0.0f, 0.0f, 0.0f, 1.0f };
        /// The focused search match, in visible rows.
        const SelectionRowSpan* searchMatchSpans = nullptr;
        int searchMatchSpanCount = 0;
        /// What the focused highlight is drawn in.
        Vec4 searchMatchHighlight { 0.0f, 0.0f, 0.0f, 0.0f };
        /// Every match except the focused one, in visible rows.
        const SelectionRowSpan* otherSearchMatchSpans = nullptr;
        int otherSearchMatchSpanCount = 0;
        /// Trigger-action highlights, in visible rows.
        const SelectionRowSpan* triggerHighlightSpans = nullptr;
        int triggerHighlightSpanCount = 0;
        /// The mode-1001 hilite band, in visible rows.
        const SelectionRowSpan* hiliteRegionSpans = nullptr;
        int hiliteRegionSpanCount = 0;
    };

    /// Builds the captured frame with the caller's present-time view tiers.
    ///
    /// For a frame captured at a DECSET-2026 boundary, where the view tiers
    /// were necessarily absent. Damage keeps the capture's own semantics: a
    /// boundary capture carries no dirty-row mask, and per
    /// `TerminalSnapshotOptions::dirtyRows`' governing contract a null mask
    /// builds every row — full-frame, never nothing-dirty. A consumer reading
    /// the absent mask as a clean frame would draw a stale grid under a green
    /// gate.
    ///
    /// - Parameters:
    ///   - backend: The glyph backend the builder faults misses through.
    ///   - out: The instance buffers to fill.
    ///   - view: The tiers composited over the captured grid.
    /// - Returns: How many instances each stream needs.
    [[nodiscard]] InstanceCounts build(const GlyphBackend& backend, const InstanceBuffers& out,
        const ViewTierOverrides& view, BuildScope scope = BuildScope::Damaged) const
    {
        BuildInputs inputs = m_frame->inputs();
        if (scope == BuildScope::Whole)
            inputs.dirtyRows = nullptr;
        inputs.selectionSpans = view.selectionSpans;
        inputs.selectionSpanCount = view.selectionSpanCount;
        inputs.selectionColor = view.selectionColor;
        inputs.isSelectionBehindText = view.isSelectionBehindText;
        inputs.selectionForegroundOverride = view.hasSelectionForegroundOverride;
        inputs.selectionForeground = view.selectionForeground;
        inputs.searchMatchSpans = view.searchMatchSpans;
        inputs.searchMatchSpanCount = view.searchMatchSpanCount;
        inputs.searchMatchHighlight = view.searchMatchHighlight;
        inputs.otherSearchMatchSpans = view.otherSearchMatchSpans;
        inputs.otherSearchMatchSpanCount = view.otherSearchMatchSpanCount;
        inputs.triggerHighlightSpans = view.triggerHighlightSpans;
        inputs.triggerHighlightSpanCount = view.triggerHighlightSpanCount;
        inputs.hiliteRegionSpans = view.hiliteRegionSpans;
        inputs.hiliteRegionSpanCount = view.hiliteRegionSpanCount;
        InstanceBuffers withStorage = out;
        lendLetterStorage(withStorage);
        return buildCellInstances(inputs, backend, withStorage);
    }

    /// Where a column's letter stands on a captured row, from the grid's left
    /// edge in device pixels, as the last build laid it out.
    ///
    /// The frame's answer for every presentation that follows the letters
    /// (Docs/proportional-text.md): a row the primary face laid out as
    /// proportional runs answers from its layout; any other row answers the
    /// column at the pitch, so a caller reads one function for both.
    ///
    /// - Parameters:
    ///   - row: The visible row.
    ///   - column: The column.
    /// - Returns: The letter's left edge and advance; the pitch off the grid.
    [[nodiscard]] LetterExtent letterExtent(int row, int column) const
    {
        const float pitch = metrics().cellWidth;
        const std::span<const LetterExtent> letters = rowLetters(row);
        if (letters.empty() || column < 0 || column >= static_cast<int>(letters.size()))
            return { static_cast<float>(std::max(column, 0)) * pitch, pitch };
        return letters[static_cast<std::size_t>(column)];
    }

    /// The column under a pixel along a captured row.
    ///
    /// The letter the pixel falls on where the row was laid out as runs, else
    /// the column at the pitch. A pixel left of the grid names column 0 and
    /// one right of it the last column, as a report clamps.
    ///
    /// - Parameters:
    ///   - row: The visible row.
    ///   - xInPixels: The pixel, from the grid's left edge.
    /// - Returns: The column, 0 through `columns() - 1`.
    [[nodiscard]] int columnAt(int row, double xInPixels) const
    {
        const std::span<const LetterExtent> letters = rowLetters(row);
        if (!letters.empty())
            return letterColumnAt(letters, xInPixels);
        const float pitch = metrics().cellWidth;
        const int last = std::max(columns() - 1, 0);
        if (!(pitch > 0.0f))
            return 0;
        return std::clamp(static_cast<int>(std::floor(xInPixels / static_cast<double>(pitch))), 0, last);
    }

    /// Where a pixel is along a captured row, in cells: the column under it
    /// plus the fraction of that letter's advance the pixel has crossed.
    ///
    /// What ``Input::PointerFact::xInCells`` carries. On a row laid out as
    /// runs its nearest whole value is the boundary between two letters,
    /// which is what the ladder resolves a caret from; on a grid row it is
    /// the pixel over the pitch.
    ///
    /// - Parameters:
    ///   - row: The visible row.
    ///   - xInPixels: The pixel, from the grid's left edge.
    /// - Returns: The position, 0 through `columns()`.
    [[nodiscard]] double xInCellsAt(int row, double xInPixels) const
    {
        const std::span<const LetterExtent> letters = rowLetters(row);
        if (!letters.empty())
            return letterXInCells(letters, xInPixels);
        const float pitch = metrics().cellWidth;
        if (!(pitch > 0.0f))
            return 0.0;
        return std::clamp(xInPixels / static_cast<double>(pitch), 0.0, static_cast<double>(std::max(columns(), 0)));
    }

    /// Whether the captured row was laid out as proportional runs by the last build.
    ///
    /// - Parameter row: The visible row.
    /// - Returns: True when ``letterExtent`` answers from a layout there.
    [[nodiscard]] bool isRowLaidOutAsLetters(int row) const { return !rowLetters(row).empty(); }

    /// Settles whether this capture's build, Loaded over the retained target,
    /// draws what a full frame draws as far as ink crossing between rows goes,
    /// and moves the target's record with it.
    ///
    /// ``settleRowInkCrossings`` over the rows the build repainted and the
    /// scroll the blit performs, both this capture's own. A capture with no
    /// mask built every row, and a target is never Loaded under one.
    ///
    /// - Parameters:
    ///   - targetCrossings: Where the target's ink crosses, `rows + 1` entries;
    ///     moved with the scroll when the frame stands.
    ///   - builtCrossings: Where this build's ink crosses, `rows + 1` entries.
    /// - Returns: Whether the partial frame may stand.
    [[nodiscard]] bool settleRowInkCrossings(RowBytesToWrite targetCrossings, RowBytes builtCrossings) const
    {
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();
        if (dirtyRows.size() != static_cast<std::size_t>(rows()))
            return false;
        return Rendering::settleRowInkCrossings(dirtyRows, targetCrossings, builtCrossings,
            m_damage.scrollTop, m_damage.scrollBottom, m_damage.scrollDelta);
    }

    /// The whole-frame constants derived from this capture, for the passes.
    ///
    /// Core's one derivation over the capture's own facts: a caller that
    /// assembled uniforms beside the capture would hold a second spelling of
    /// the same frame, and the two disagree exactly when a style knob reaches
    /// one of them first.
    [[nodiscard]] FrameUniforms uniforms() const { return deriveFrameUniforms(m_frame->inputs()); }

    using FontPreparationBits = std::span<std::uint64_t>;
    using FontPreparationKeys = std::vector<std::uint64_t>;
    static constexpr std::size_t fontPreparationWordCount = 4 * 0x110000 / 64;

    /// Returns the storage size for the character/style preparation bitset.
    [[nodiscard]] static std::size_t fontPreparationStorageWords() { return fontPreparationWordCount; }

    /// Marks and returns a bounded set of unprepared visible character/style pairs.
    [[nodiscard]] FontPreparationKeys takeFontPreparationKeys(FontPreparationBits prepared, std::size_t limit) const
    {
        assert(prepared.size() == fontPreparationWordCount);
        FontPreparationKeys keys;
        const auto& inputs = m_frame->inputs();
        if (!inputs.cells || inputs.rows <= 0 || inputs.columns <= 0 || !limit) {
            return keys;
        }
        const auto count = static_cast<std::size_t>(inputs.rows) * static_cast<std::size_t>(inputs.columns);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& cell = inputs.cells[index];
            if (cell.codePoint <= 127 || cell.codePoint >= 0x110000) {
                continue;
            }
            if (cell.pen.attributes.contains(VT::Attribute::Hidden)
                || cell.codePoint == Kitty::placeholderCodePoint
                || BoxDrawing::covers(cell.codePoint)
                || (inputs.drawsBrailleGrid && isBraille(cell.codePoint))
                || cell.codePoint - VT::drcsCodePointBase < VT::drcsMaxCells) {
                continue;
            }
            const unsigned style = cell.pen.attributes.contains(VT::Attribute::Bold)
                | (static_cast<unsigned>(cell.pen.attributes.contains(VT::Attribute::Italic)) << 1);
            const std::size_t bit = static_cast<std::size_t>(style) * 0x110000 + cell.codePoint;
            const auto mask = std::uint64_t { 1 } << (bit % 64);
            if (prepared[bit / 64] & mask) {
                continue;
            }
            prepared[bit / 64] |= mask;
            keys.push_back(cell.codePoint | (static_cast<std::uint64_t>(style) << 32));
            if (keys.size() == limit) {
                break;
            }
        }
        return keys;
    }

    using CapturedImages = std::span<const Image::TerminalImage>;
    using CapturedImageQuads = std::span<const Image::ImageQuad>;

    /// Borrows image records and pixels owned by this capture.
    [[nodiscard]] CapturedImages images() const UBIQUITTY_LIFETIMEBOUND
    {
        const auto& inputs = m_frame->inputs();
        return { inputs.images, static_cast<std::size_t>(inputs.imageCount) };
    }

    /// Borrows resolved image geometry in protocol drawing order.
    [[nodiscard]] CapturedImageQuads imageQuads() const UBIQUITTY_LIFETIMEBOUND { return m_frame->imageQuads(); }

    using CapturedBlockQuads = std::span<const RichText::BlockQuad>;
    using CapturedHighlights = std::span<const RichText::HighlightQuad>;
    using CapturedStyles = std::span<const RichText::DrawnStyle>;
    using CapturedClusters = std::span<const RichText::DrawnCluster>;
    using CapturedShapedLines = std::span<const RichText::ShapedLine>;
    using CapturedRules = std::span<const RichText::DrawnRule>;
    using CapturedCodePoints = std::span<const std::uint32_t>;
    using CapturedVisibleRects = std::span<const RichText::VisibleRect>;

    /// The colour the selection is highlighted in, which a block's highlights
    /// take too.
    [[nodiscard]] Vec4 selectionColor() const { return m_frame->inputs().selectionColor; }
    /// Whether native selection belongs beneath glyphs, above text backgrounds.
    [[nodiscard]] bool isSelectionBehindText() const { return m_frame->inputs().isSelectionBehindText; }

    /// Borrows the rich text blocks the frame shows (Docs/rich-text.md).
    [[nodiscard]] CapturedBlockQuads richTextQuads() const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextQuads(); }

    /// Borrows the selection's highlights inside the blocks, drawn under
    /// their glyphs.
    [[nodiscard]] CapturedHighlights richTextHighlights() const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextHighlights(); }

    /// Borrows the styles a block's graphemes are drawn in.
    ///
    /// - Parameter block: A position in ``richTextQuads()``.
    [[nodiscard]] CapturedStyles richTextStyles(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).styles(); }

    /// Borrows a block's graphemes, placed in layout units.
    ///
    /// - Parameter block: A position in ``richTextQuads()``.
    [[nodiscard]] CapturedClusters richTextClusters(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).clusters(); }
    [[nodiscard]] CapturedShapedLines richTextShapedLines(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).shapedLines(); }

    /// Borrows a block's decorated stretches, one rule per decoration each.
    ///
    /// - Parameter block: A position in ``richTextQuads()``.
    [[nodiscard]] CapturedRules richTextRules(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).rules(); }

    /// Borrows a block's text, which its graphemes index.
    ///
    /// - Parameter block: A position in ``richTextQuads()``.
    [[nodiscard]] CapturedCodePoints richTextCodePoints(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).text(); }

    /// Borrows clipping rectangles in cells relative to the block's placement.
    [[nodiscard]] CapturedVisibleRects richTextVisibleRects(std::size_t block) const UBIQUITTY_LIFETIMEBOUND { return m_frame->richTextDrawing(block).visibleRects(); }

    /// Starts feeding each capture to the paint heatmap; see
    /// ``TerminalFrameInputs::enablePaintHeatmap()``.
    void enablePaintHeatmap() { m_frame->enablePaintHeatmap(); }

    /// Stops feeding captures to the paint heatmap.
    void disablePaintHeatmap() { m_frame->disablePaintHeatmap(); }

    /// Whether captures feed the paint heatmap.
    [[nodiscard]] bool isPaintHeatmapEnabled() const { return m_frame->isPaintHeatmapEnabled(); }

    /// Feeds a frame another capture holds to this one's paint heatmap; see
    /// ``TerminalFrameInputs::notePaintHeatOf``.
    ///
    /// - Parameter shown: The capture whose frame was presented.
    void notePaintHeatOf(const FrameCapture& shown) { m_frame->notePaintHeatOf(*shown.m_frame); }

    /// How many rows the captured frame holds.
    ///
    /// Exposed because a caller sizes its grid-derived buffers before building,
    /// and it cannot read them off the inputs it is never handed.
    [[nodiscard]] int rows() const { return m_frame->inputs().rows; }

    /// How many columns the captured frame holds.
    [[nodiscard]] int columns() const { return m_frame->inputs().columns; }

    /// Whether this picture reports pixels from the drawable origin.
    ///
    /// - Returns: The captured fullscreen-content mode.
    [[nodiscard]] bool isFullscreenContentActive() const { return m_frame->inputs().isFullscreenContentActive; }

    /// The style-adjusted metrics the captured frame was built against.
    [[nodiscard]] const CellMetrics& metrics() const { return m_frame->metrics(); }

    /// One cell's width in the captured frame's device pixels: the pitch the
    /// letter positions above are measured against, for a caller that reads
    /// them across the language seam in cells.
    [[nodiscard]] float cellWidthInPixels() const { return m_frame->metrics().cellWidth; }

    /// The absolute row index of the top visible row.
    [[nodiscard]] int firstAbsoluteRow() const { return m_frame->firstAbsoluteRow(); }

    /// `options.viewOffset` clamped to the scrollback actually available.
    [[nodiscard]] int viewOffset() const { return m_frame->viewOffset(); }

    /// The captured cursor's visible-grid row, or -1 when this frame draws none.
    ///
    /// -1 folds every reason a frame has no cursor — hidden, scrolled off the
    /// window, parked on the status row — into the one answer a motion tick
    /// needs, which is that there is nothing to slide toward.
    [[nodiscard]] int cursorRow() const { return m_frame->inputs().cursor.row; }

    /// Whether the focused search match's pill is mid-bounce in the captured frame,
    /// so the frame clock keeps its smooth cadence until it rests.
    [[nodiscard]] bool isFocusedMatchBouncing() const
    {
        const float age = m_frame->inputs().focusedMatchAgeSeconds;
        return age >= 0.0f && age < focusedPillBounceSeconds;
    }

    /// The captured cursor's column, meaningful while ``cursorRow()`` is not -1.
    [[nodiscard]] int cursorColumn() const { return m_frame->inputs().cursor.column; }

    /// The cursor motion policy belonging to this captured frame.
    [[nodiscard]] CursorMotionMode cursorMotionMode() const { return m_frame->cursorMotionMode(); }

    /// The captured cursor motion duration in milliseconds; zero selects the default.
    [[nodiscard]] std::uint32_t cursorMotionDurationMilliseconds() const { return m_frame->cursorMotionDurationMilliseconds(); }

    /// The captured cursor's shape, as ``Cursor::Shape``'s raw value.
    [[nodiscard]] std::uint8_t cursorShape() const { return static_cast<std::uint8_t>(m_frame->inputs().cursor.shape); }

    /// Whether the captured cursor was resolved focused.
    [[nodiscard]] bool isCursorFocused() const { return m_frame->inputs().cursor.focused; }

    /// The tick the captured frame's blink curves were resolved against.
    ///
    /// Exposed for the boundary route's smoke: with the harness's pin set,
    /// the tick a capture really carried is the one deterministic clock fact
    /// a consumer far from the fill can assert.
    [[nodiscard]] std::uint32_t blinkTicks() const { return m_frame->inputs().blinkTicks; }

    /// The captured frame's link foreground, straight RGBA.
    ///
    /// Exposed for the same reason ``uniforms()`` is: a consumer far from the
    /// options that steered this capture — the boundary route's smoke — can
    /// assert the styling the frame really carried rather than the styling
    /// somebody meant to register.
    [[nodiscard]] Vec4 linkColor() const { return m_frame->inputs().linkColor; }

    /// The record identity and lineage captured with the frame.
    [[nodiscard]] const FrameRecordHeader& recordHeader() const { return m_header; }

    /// The publication sequence, or zero before the first successful capture.
    [[nodiscard]] std::uint64_t sequence() const { return m_header.sequence; }

    /// The sequence this delta updates, or zero for a full frame.
    [[nodiscard]] std::uint64_t baseSequence() const { return m_header.baseSequence; }

    /// The terminal model generation represented by this frame.
    [[nodiscard]] std::uint64_t modelGeneration() const { return m_header.modelGeneration; }

    /// The palette generation represented by this frame.
    [[nodiscard]] std::uint64_t paletteGeneration() const { return m_header.paletteGeneration; }

    /// The registered view geometry generation represented by this frame.
    [[nodiscard]] std::uint64_t viewGeneration() const { return m_header.viewGeneration; }

    /// Whether this record replaces or updates its baseline.
    [[nodiscard]] FrameRecordKind recordKind() const { return m_header.kind; }

    /// Whether this record replaces its entire baseline.
    [[nodiscard]] bool isFullFrame() const { return m_header.kind == FrameRecordKind::Full; }

    /// Whether any visible row changed against the baseline: every row for a
    /// full record, any set entry of the owned mask for a delta. A short mask
    /// reads dirty, as isRowDirty does.
    [[nodiscard]] bool hasCapturedDirtyRows() const
    {
        if (m_header.kind == FrameRecordKind::Full)
            return true;
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();
        if (dirtyRows.size() != static_cast<std::size_t>(rows()))
            return true;
        for (const std::uint8_t row : dirtyRows) {
            if (row != 0)
                return true;
        }
        return false;
    }

    /// Whether the effective mask marks any row, read off the mask alone: a
    /// capture built without one (every row) reads dirty. Unlike
    /// hasCapturedDirtyRows this ignores the record kind, so a live capture —
    /// which commits no record and keeps the default Full header — answers
    /// from what it built.
    [[nodiscard]] bool hasDirtyRowsInMask() const
    {
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();
        if (dirtyRows.size() != static_cast<std::size_t>(rows()))
            return true;
        for (const std::uint8_t row : dirtyRows) {
            if (row != 0)
                return true;
        }
        return false;
    }

    /// The rows a delta owns against its baseline; every row for a full
    /// record or a short mask, as hasCapturedDirtyRows reads them.
    [[nodiscard]] int capturedDirtyRowCount() const
    {
        if (m_header.kind == FrameRecordKind::Full)
            return rows();
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();
        if (dirtyRows.size() != static_cast<std::size_t>(rows()))
            return rows();
        int count = 0;
        for (const std::uint8_t row : dirtyRows)
            count += row != 0;
        return count;
    }

    /// The damage verdict owned by this record.
    [[nodiscard]] const FrameDamage& frameDamage() const { return m_damage; }

    /// The record's pending scroll, as scalars a Swift caller can read.
    [[nodiscard]] int scrollTop() const { return m_damage.scrollTop; }
    [[nodiscard]] int scrollBottom() const { return m_damage.scrollBottom; }
    [[nodiscard]] int scrollDelta() const { return m_damage.scrollDelta; }

    /// How many visible rows the record's owned damage mask names.
    [[nodiscard]] int dirtyRowCount() const
    {
        return rows();
    }

    /// Whether `row` must be rebuilt for this record.
    ///
    /// Invalid rows read dirty, preserving the full-frame failure mode.
    ///
    /// - Parameter row: The visible row to inspect.
    /// - Returns: Whether the row must be rebuilt.
    [[nodiscard]] bool isRowDirty(int row) const
    {
        if (row < 0 || row >= rows() || m_header.kind == FrameRecordKind::Full) {
            return true;
        }
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();
        return dirtyRows.size() != static_cast<std::size_t>(rows())
            || dirtyRows[static_cast<std::size_t>(row)] != 0;
    }

    /// The frame's measured ownership traffic.
    [[nodiscard]] const FrameCopyStatistics& copyStatistics() const { return m_frame->copyStatistics(); }

    /// The captured cursor's cross-fade weight, zero when blink has it dark.
    [[nodiscard]] float cursorMixAlpha() const { return m_frame->inputs().cursor.mixA; }

    /// Applies the overlay's presentation-time blink phase to captured content.
    ///
    /// - Parameter alpha: The resolved blink opacity, from zero to one.
    void setCursorMixAlpha(float alpha) { m_frame->restateCursorMixAlpha(alpha); }

    /// Resolves the captured cursor through the surface's presentation state.
    ///
    /// - Parameters:
    ///   - presentation: The state retained by this surface between frames.
    ///   - nowSeconds: The monotonic presentation timestamp.
    ///   - intervalSeconds: The display interval, or zero to disable holds.
    ///   - bypass: Whether nearby input-driven movement should be immediate.
    void applyCursorPresentation(CursorPresentation& presentation, double nowSeconds, double intervalSeconds, bool bypass) // bool-ok: forwards the caller's per-frame recent-input predicate.
    {
        m_frame->applyCursorPresentation(presentation, nowSeconds, intervalSeconds, bypass);
    }

    /// Restates the captured cursor as mid-slide, from a motion tick that ran
    /// after the capture.
    ///
    /// The tick's target is the captured cursor cell, so it can only run once
    /// the capture holds one — after the options carrying the slide were
    /// consumed. The builder reads the slide during ``build()``, which is why
    /// the write lands here rather than in the next frame's options: a slide
    /// restated one frame late draws the moving block against last frame's
    /// grid.
    ///
    /// - Parameters:
    ///   - columnFractional: The interpolated column.
    ///   - vrowFractional: The interpolated visible row.
    void setCursorSliding(float columnFractional, float vrowFractional)
    {
        m_frame->restateCursorSliding(columnFractional, vrowFractional);
    }

    /// Restates the captured cursor as settled, the counterpart of
    /// ``setCursorSliding``.
    void setCursorSettled() { m_frame->restateCursorSettled(); }

    /// Places a stationary presentation cursor without changing the terminal model.
    ///
    /// - Parameters:
    ///   - column: The visible column to draw over.
    ///   - row: The visible row to draw over.
    void setCursorPosition(int column, int row) { m_frame->restateCursorPosition(column, row); }

    /// Allocates a capture, owned by whoever holds the returned pointer.
    ///
    /// The only way to make one. A stack instance would be released as though
    /// it were heap memory the first time Swift saw a pointer to it, which is
    /// the hazard a shared reference type carries and the reason the
    /// constructor is not public.
    ///
    /// - Returns: A capture with one reference, or null if allocation failed.
    [[nodiscard]] static FrameCapture* make() UBIQUITTY_RETURNS_RETAINED
    {
        return new (std::nothrow) FrameCapture();
    }

    /// The reference count Swift manipulates through the global retain and
    /// release. Public because those are free functions.
    std::atomic<int> swiftReferences { 1 };

private:
    FrameCapture()
        : m_frame(new TerminalFrameInputs())
    {
    }

    /// Assigns local lineage for a capture without an external publisher.
    void commitAutomaticRecord(const TerminalSnapshotOptions& options, const CapturedRowRefresh& refresh,
        const FrameDamage& damage)
    {
        FrameRecordHeader header;
        header.sequence = m_header.sequence == std::numeric_limits<std::uint64_t>::max()
            ? 1
            : m_header.sequence + 1;
        header.baseSequence = m_header.sequence;
        header.modelGeneration = m_frame->modelGeneration();
        header.paletteGeneration = m_frame->paletteGeneration();
        header.viewGeneration = refresh.canReuseRows ? m_header.viewGeneration : m_header.viewGeneration + 1;
        header.captureTicks = options.blinkTicks;
        header.kind = refresh.canReuseRows ? FrameRecordKind::Delta : FrameRecordKind::Full;
        commitRecord(header, damage);
    }

    /// Commits metadata only after the capture's owned state is complete.
    ///
    /// - Parameters:
    ///   - header: The publisher-assigned identity and lineage.
    ///   - damage: The damage verdict represented by the record.
    void commitRecord(const FrameRecordHeader& header, const FrameDamage& damage)
    {
        m_header = header;
        m_damage = damage;
        const std::span<const std::uint8_t> dirtyRows = m_frame->capturedDirtyRows();

        const bool isValidDelta = m_header.kind == FrameRecordKind::Delta
            && m_header.sequence != 0 && m_header.baseSequence != 0
            && m_damage.canReuse && dirtyRows.size() == static_cast<std::size_t>(rows());
        if (!isValidDelta) {
            m_header.kind = FrameRecordKind::Full;
            m_header.baseSequence = 0;
            m_damage.canReuse = false;
        }

        if (m_header.paletteGeneration == 0) {
            m_header.paletteGeneration = m_frame->paletteGeneration();
        }
        if (m_header.modelGeneration == 0) {
            m_header.modelGeneration = m_damage.generationAfterDrain;
        }
    }

    // Held behind a pointer rather than by value, and the indirection is what
    // makes the type importable at all: TerminalFrameInputs is non-escapable, an
    // escapable record may not contain a non-escapable field, and a shared
    // reference type is escapable. By value this class would not import.
    /// Lends the frame's own letter storage to a build whose caller brought
    /// none, so a clean row's letters survive a damage-first build and the hit
    /// tests above read what was drawn.
    ///
    /// - Parameter out: The build's output spans, filled where they name no letters.
    void lendLetterStorage(InstanceBuffers& out) const
    {
        if (out.letters == nullptr)
            m_frame->letterStorage(out);
    }

    /// The letters of a visible row the last build laid out as runs, else empty.
    [[nodiscard]] std::span<const LetterExtent> rowLetters(int row) const
    {
        const std::span<const std::uint8_t> rowsLaidOut = m_frame->letterRows();
        const int columnCount = columns();
        if (row < 0 || row >= static_cast<int>(rowsLaidOut.size()) || rowsLaidOut[static_cast<std::size_t>(row)] == 0u || columnCount <= 0)
            return {};
        const std::span<const LetterExtent> letters = m_frame->letters();
        const std::size_t first = static_cast<std::size_t>(row) * static_cast<std::size_t>(columnCount);
        if (first + static_cast<std::size_t>(columnCount) > letters.size())
            return {};
        return letters.subspan(first, static_cast<std::size_t>(columnCount));
    }

    TerminalFrameInputs* m_frame; // Owned; released by the capture's destructor.
    FrameRecordHeader m_header;
    FrameDamage m_damage;
} UBIQUITTY_SHARED_REFERENCE(retainFrameCapture, releaseFrameCapture);

} // namespace Ubiquitty::Rendering

/// Claims a reference to `capture` on Swift's behalf.
///
/// - Parameter capture: The capture to retain. A null pointer is ignored.
inline void retainFrameCapture(Ubiquitty::Rendering::FrameCapture* capture)
{
    if (capture)
        capture->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops a reference to `capture`, destroying it at zero.
///
/// - Parameter capture: The capture to release. A null pointer is ignored.
inline void releaseFrameCapture(Ubiquitty::Rendering::FrameCapture* capture)
{
    if (capture && capture->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete capture;
}
