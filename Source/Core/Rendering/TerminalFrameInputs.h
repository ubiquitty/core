// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include <span>

#include "Core/Rendering/BeamLayout.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/CursorMotion.h"
#include "Core/Rendering/CursorPresentation.h"
#include "Core/Rendering/FocusedMatchBounce.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Rendering/OverlayAssembly.h"
#include "Core/Rendering/TerminalSnapshot.h"

#include "Core/Color.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Diagnostics/PaintHeatmap.h"
#include "Core/Image/TerminalImage.h"
#include "Core/TermPalette.h"
#include "Core/TextSizeTable.h"
#include "Core/VT/SoftFont.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Ubiquitty {

struct Terminal;

namespace Rendering {

/// The cell a cursor-motion tick aims at, resolved ahead of the capture the
/// way ``TerminalFrameInputs::capture`` will resolve it.
///
/// A frontend that plans its retained frame before it captures needs the
/// current frame's slide verdict at plan time, and the verdict needs the frame's
/// target. Ticked after the capture, the verdict the plan read was a frame
/// stale: on the frame a slide settled the plan still said in flight, and
/// the builder drew a settled block for a frame that was still gliding.
/// Resolving the target here, from the same terminal under the same lock,
/// lets the tick run first without a second spelling of the cursor rule.
struct CursorMotionTarget {
    /// The cursor's visible-grid row, or -1 when the frame draws none: the
    /// caller drawing no cursor, DECTCEM hidden, or scrolled off the window
    /// by the view offset. A status row is the capture's to hide, so a cursor
    /// parked there resolves to its row here and to none in the capture; the
    /// builder ignores every slide field of a frame that draws no cursor, so
    /// the tick aims one frame at a cell nothing draws and then pins.
    int visibleRow = -1;
    /// The cursor's column, meaningful while `visibleRow` is not -1.
    int column = 0;
    /// The terminal's `[cursor]` motion policy, as the capture will carry it.
    CursorMotionMode mode = CursorMotionMode::Off;
    /// Its slide duration in milliseconds, zero for Core's default.
    std::uint32_t durationMilliseconds = 0;
};

/// Resolves the cell this frame's cursor motion aims at.
///
/// - Parameters:
///   - terminal: The terminal about to be captured, under the model lock.
///   - viewOffset: The rows of history the frame presents above the tail,
///     clamped here as the capture clamps it.
///   - drawCursor: Whether the frame draws a cursor at all, as
///     `TerminalSnapshotOptions::drawCursor` will say.
/// - Returns: The target, with `visibleRow` -1 when the frame draws no cursor.
[[nodiscard]] CursorMotionTarget resolveCursorMotionTarget(const Terminal& terminal, int viewOffset, bool drawCursor);

/// A frame's worth of terminal state, already read out of whatever holds it.
///
/// A capture reads a live `Terminal` in four places: the display rows, a handful
/// of scalars, the images, and the cursor. A frontend that renders off the model
/// thread cannot do any of that on its render path — it holds a copy taken under
/// its model lock, and reaching back into the `Terminal` to capture is a read
/// racing the parser. This is that copy's shape, so the same capture serves both
/// and neither grows its own.
///
/// Every field is resolved rather than derived: `cells` are display rows at the
/// view offset, `rowMarks` is null on the alternate screen rather than carrying
/// marks a caller must know to ignore, and the cursor is where it sits on the
/// SCREEN, before the view offset moves it. A source that had to be interpreted
/// would put the interpretation back in each frontend.
struct UBIQUITTY_NONESCAPABLE FrameSource {
    /// Display rows at the view offset, row-major, `rows * columns` of them.
    const Cell* cells = nullptr;
    /// One line attribute per visible row.
    const std::uint8_t* lineAttributes = nullptr;
    /// One ``RowFlag`` set per visible row, raw, or null when the capture
    /// did not read them; the builder then asks each cell.
    const std::uint8_t* rowFlags = nullptr;
    /// One OSC 133 mark per visible row, or null on the alternate screen, where
    /// marks do not live and the builder's gate is that absence.
    const std::uint8_t* rowMarks = nullptr;
    int rows = 0;
    int columns = 0;
    /// Scrollback available, for gridFloor the requested view offset.
    int scrollbackCount = 0;
    /// The model input epoch represented by this source.
    std::uint64_t modelGeneration = 0;
    /// The palette to copy into the immutable capture.
    const TermPalette* palette = nullptr;
    /// The palette generation represented by `palette`.
    std::uint64_t paletteGeneration = 0;
    /// Whether `paletteGeneration` is authoritative for palette changes.
    ///
    /// Generation zero is a valid initial generation, so absence cannot be
    /// encoded in the number. A source without a counter leaves this false and
    /// the capture conservatively copies its palette on every frame.
    bool hasPaletteGeneration = false;
    /// The OSC 66 spec store the visible cells' size ids index into.
    ///
    /// Borrowed only while capture copies it. A source that leaves it null
    /// renders every sized block unsized, which is a legible frame rather than
    /// a wrong one — and is therefore invisible, so a source gaining sized-text
    /// support is checked against a rendered block.
    const TextSizeTable* textSizeTable = nullptr;
    /// The terminal's ambiguous-width choice, which the sized-block claims in
    /// `textSizeTable` are measured under.
    Unicode::Width::AmbiguousWidth ambiguousWidth = Unicode::Width::AmbiguousWidth::Narrow;
    /// The cursor's colour, as the palette carries it.
    PaletteRGB cursorColor {};
    /// DECSCNM: the default foreground and background trade places.
    bool isScreenReverse = false;
    /// DECCOLM: read alongside the column count, never taken on trust — the mode
    /// and the count are the same fact, and a source supplying one without the
    /// other draws the edge rule down the middle of a grid.
    bool isColumns132 = false;
    bool isScreenBlanked = false;
    bool isCursorVisible = false;
    /// Whether the terminal is taking a password (mode 7401, or the
    /// `password_detect` heuristic settling on a prompt row).
    bool isPasswordPromptActive = false;
    /// `ScrollSearch::activeMatchEpoch` for the terminal, zero without search
    /// state; the capture's bounce clock reads a match's arrival off it.
    std::uint32_t activeMatchEpoch = 0;
    /// Screen-relative, before the view offset moves it down the window.
    int cursorRow = 0;
    int cursorColumn = 0;
    /// Decided by `Cursor::resolveAppearance`, including when DECTCEM is hidden.
    Ubiquitty::Cursor::Shape cursorShape = Ubiquitty::Cursor::Shape::Block;
    /// The terminal's `[cursor]` motion preferences, seeded beside its blink
    /// by `Preferences::Effective::applyCursorPreferences`. Carried so the
    /// animator that ticks `CursorMotion` against this capture reads the
    /// policy the terminal was configured with, and holds no settings of its
    /// own. Off never glides; a zero duration takes Core's default.
    CursorMotionMode cursorMotionMode = CursorMotionMode::Off;
    std::uint32_t cursorMotionDurationMilliseconds = 0;
    /// Image metadata and current decoded payloads to copy into the capture, or
    /// null when no stored image exists.
    const Ubiquitty::Image::TerminalImage* images = nullptr;
    /// Image placements to copy into the capture, or null when none are live.
    const Ubiquitty::Image::TerminalImagePlacement* placements = nullptr;
    /// The screen active at capture time, so a renderer resolving from this
    /// capture draws only the placements belonging to it.
    bool alternateActive = false;
    /// Reads a cell's full grapheme cluster. A source that leaves this null is
    /// not refused: the builder falls back to the cell's base code point, and a
    /// ZWJ family, a flag or a skin-toned hand key the atlas as a man, a
    /// regional-indicator letter and a bare hand — each a real glyph, so the
    /// frame renders and only the picture is wrong.
    BuildInputs::ClusterResolver clusterResolver = nullptr;
    void* clusterResolverContext = nullptr;
    /// Whether the host named a selection background with `OSC 17`.
    bool hasSelectionBackground = false;
    /// That colour, 16-bit per channel as `xparsecolor` carries it, read only
    /// under `hasSelectionBackground`.
    ///
    /// What the host asked for, not a resolved tint: the capture owns the rule
    /// that `OSC 17` names a colour and not an opacity, so a source that
    /// resolved its own would be free to take the alpha with it and hide the
    /// glyphs under the selection on that frontend alone.
    std::uint16_t selectionBackgroundRed = 0;
    std::uint16_t selectionBackgroundGreen = 0;
    std::uint16_t selectionBackgroundBlue = 0;
    /// Whether the host named a selection foreground with `OSC 19`.
    bool hasSelectionForeground = false;
    /// That colour, 16-bit per channel as `xparsecolor` carries it, read only
    /// under `hasSelectionForeground`.
    ///
    /// Carried beside the background rather than resolved by the source for the
    /// same reason: the capture owns the rule that a host naming a selection
    /// foreground is what activates the override, so a frontend that resolved
    /// its own would decide that question per platform.
    std::uint16_t selectionForegroundRed = 0;
    std::uint16_t selectionForegroundGreen = 0;
    std::uint16_t selectionForegroundBlue = 0;
    /// Whether the program named the color of the glyph under a focused block
    /// cursor with kitty's `cursor_text`.
    bool hasCursorTextColor = false;
    /// That color, read only under `hasCursorTextColor`.
    PaletteRGB cursorTextColor {};
    /// The session's DECDLD soft font, or null when it never downloaded one.
    ///
    /// Borrowed only for the call: the capture keeps its own copy, gated on
    /// `serial`, so the ~4.7KB matrix is copied once per download rather than
    /// once per frame. A source that reads a live terminal can therefore point
    /// straight at it and let go of its lock, on the same terms as `cells`.
    const Ubiquitty::VT::SoftFont* softFont = nullptr;
    /// DECSET 7402 with a colour letterbox: the frame drops its padding and
    /// glyphless cells paint `letterboxColor` opaquely.
    ///
    /// Resolved from the mode and the letterbox selection together, because
    /// they are one fact: `Passthrough` leaves the terminal content visible
    /// around a fitted image, so the grid renders padded under the same mode
    /// bit that makes `Color` go edge to edge. A source carrying the mode
    /// alone would decide the frame's geometry from half of what decides it.
    bool isEdgeToEdge = false;
    /// Whether pixel reports use the drawable origin for this captured picture.
    bool isFullscreenContentActive = false;
    /// The letterbox fill, 0x00RRGGBB, read only under `isEdgeToEdge`.
    ///
    /// Travels beside the mode rather than being fetched when the frame is
    /// drawn: read later, it is the letterbox of whatever session the terminal
    /// holds by then, while the cells are the ones captured here.
    std::uint32_t letterboxColor = 0;
};

/// Whether the frame paints edge to edge: DECSET 7402 with a `Color` letterbox.
///
/// The one spelling of the two facts that decide it, for every reader: the
/// capture, the lock-free snapshot a pointer maps through between drains, and
/// a frame loop choosing its origin under the lock the capture reads under.
/// `Passthrough` leaves the terminal content visible around a fitted image, so
/// the grid stays padded under the same mode bit that sends `Color` to the
/// drawable's edge; a reader of the mode alone decides the geometry from half
/// of what decides it.
///
/// - Parameter terminal: The terminal, read under its model lock.
/// - Returns: Whether the frame drops its padding.
[[nodiscard]] bool isFrameEdgeToEdge(const Ubiquitty::Terminal& terminal);

/// The color kitty's `cursor_text` names for the glyph under a focused block.
///
/// - Parameter terminal: The terminal whose `cursor_text` it reads.
/// - Returns: The color, or none while the program has not named one.
std::optional<PaletteRGB> cursorTextColor(const Ubiquitty::Terminal& terminal);

/// Gives a cursor the glyph color kitty's `cursor_text` names.
///
/// Every renderer of a focused block takes the glyph's color through this, so
/// none of them draws the program's `cursor_text` differently or drops it.
///
/// - Parameters:
///   - cursor: The cursor whose `hasTextColor` and `textRGB` it sets.
///   - color: The `cursor_text` color, or none to leave the glyph to the
///     renderer's own contrast with the cursor.
void setCursorTextColor(CursorRenderState& cursor, std::optional<PaletteRGB> color);

/// Everything `buildCellInstances` needs about a terminal, captured once.
///
/// Turning a `Terminal` into `BuildInputs` is not a lookup: it snapshots the
/// display rows at a view offset, derives each row's line attribute, folds the
/// style into the rasterizer's `CellMetrics`, and resolves the cursor against
/// the palette and the visible range. Every backend that draws a terminal needs
/// exactly that, and each one with its own copy drifts on its own — the
/// software snapshot, the GPU backends, and any frontend after them.
///
/// The captured `BuildInputs` borrows the row and attribute storage this object
/// owns, so it stays valid only while the capture does. Capturing again reuses
/// that storage rather than reallocating, which is what makes this usable on a
/// per-frame path rather than only for a one-shot snapshot.
/// Non-escapable, because it holds a `BuildInputs` by value and that is
/// non-escapable: it carries borrowed spans into the rows, images and marks
/// this object owns. An untagged class is implicitly escapable, and an
/// escapable record may not contain a non-escapable field — so leaving the tag
/// off does not make the type more available, it makes it unimportable, and a
/// Swift frontend then reimplements the capture rather than calling it.
class UBIQUITTY_NONESCAPABLE TerminalFrameInputs {
public:
    /// Captures `terminal` at `options.viewOffset`, deriving cells, line
    /// attributes, metrics and cursor. Returns false for a degenerate terminal
    /// (no active screen, or no rows or columns), leaving the capture empty.
    ///
    /// `baseMetrics` comes from the rasterizer that will fault the glyphs, so
    /// the geometry the decorations are drawn at is the geometry the glyphs were
    /// rasterized at.
    ///
    /// `chrome`, when non-null, is composited over the captured cells before
    /// anything reads them — so the damage tracker digests the popover with the
    /// grid, covered rows rebuild and uncovered ones stay clean, and no backend
    /// needs a second pass or a dirty-row field. Null is the ordinary frame and
    /// costs one branch per capture rather than per cell.
    ///
    /// It composites into the CAPTURE, never into the terminal: the grid belongs
    /// to the application on the other end of the PTY, and chrome written there
    /// would be indistinguishable from its output and would corrupt an Update's
    /// baseline that then described cells nobody sent.
    [[nodiscard]] bool capture(const Terminal&, const TerminalSnapshotOptions&, const CellMetrics& baseMetrics,
        const GlyphSlotGeometry& glyphSlot, ChromeLayers chrome = {});

    /// Captures `terminal`, re-reading only the rows `refresh` marks dirty.
    ///
    /// The partial-frame entry point for a caller that owns the capture rather
    /// than its cells. The two shipping frontends keep their own cell buffers and
    /// call ``refreshCapturedRows`` on them; a caller holding this object cannot,
    /// because the cells are private to it — so it hands the refresh in and the
    /// same function runs against the storage already here.
    ///
    /// Every row's line attribute and mark is re-read regardless, because both
    /// change without the row's cells changing. What the refresh skips is
    /// `getDisplayRow`, which is the per-cell cost.
    ///
    /// The refresh's `viewOffset` and `firstAbsoluteRow` are re-derived here from
    /// `options` and the terminal rather than taken from the caller, and a
    /// refresh that disagrees is demoted to a full re-read. A drain that ran
    /// against a different view marked rows describing other lines, and a stale
    /// row kept on that basis is a wrong frame rather than a slow one.
    ///
    /// - Parameters:
    ///   - terminal: The terminal to read.
    ///   - options: The frame's view offset, style and cursor decisions.
    ///   - baseMetrics: From the rasterizer that will fault the glyphs.
    ///   - glyphSlot: The atlas slot layout.
    ///   - refresh: The drain's verdict and dirty rows. A default-constructed
    ///     one re-reads every row, which is what the overload above passes.
    ///   - chrome: In-grid chrome to composite over the frame, or null.
    /// - Returns: Whether the capture holds a frame.
    [[nodiscard]] bool capture(const Terminal&, const TerminalSnapshotOptions&, const CellMetrics& baseMetrics,
        const GlyphSlotGeometry& glyphSlot, const CapturedRowRefresh& refresh, ChromeLayers chrome = {});

    /// Captures from a `FrameSource` a caller already holds, for a frontend whose
    /// render path must not touch the live model.
    ///
    /// This is the whole of the capture; the `Terminal` overload reads the four
    /// places a terminal keeps this state and hands the result here. Two entry
    /// points, one implementation — a second copy of the cursor visibility rule
    /// or the view-offset clamp is a divergence nobody would see until the two
    /// frontends disagreed about a frame.
    ///
    /// The source's `cells`, `lineAttributes` and `rowMarks` are COPIED, so the
    /// caller may drop its own storage or take its lock again as soon as this
    /// returns. Borrowing them would move the lifetime problem rather than solve
    /// it, and the copy is the one this object already made for the other
    /// overload.
    ///
    /// - Parameters:
    ///   - source: The frame's terminal state. Degenerate sources — no rows, no
    ///     columns, no cells — are refused, leaving the capture empty.
    ///   - options: The frame's view offset, style and cursor decisions.
    ///   - baseMetrics: From the rasterizer that will fault the glyphs.
    ///   - chrome: In-grid chrome to composite over the frame, or null. Taken
    ///     here rather than only on the other overload because this is the whole
    ///     of the capture: a split-model frontend, which holds its own source and
    ///     never has a `Terminal`, is exactly the caller that needs a palette
    ///     drawn over a frame it did not produce.
    /// - Returns: Whether the capture holds a frame.
    [[nodiscard]] bool capture(const FrameSource& source, const TerminalSnapshotOptions& options, const CellMetrics& baseMetrics,
        const GlyphSlotGeometry& glyphSlot, ChromeLayers chrome = {});

    /// The captured inputs. Valid until the next capture; empty before the first
    /// successful one.
    [[nodiscard]] const BuildInputs& inputs() const { return m_inputs; }

    /// Where each column's letter stands on the rows the last build laid out
    /// as proportional runs (LetterLayout.h): `rows * columns` extents, and a
    /// byte per row that is nonzero where the row was laid out so.
    ///
    /// The build writes them through ``letterStorage``; a row the build left
    /// clean keeps what the previous build wrote, which is why the frame owns
    /// them rather than the build. Read by the capture's hit tests, so a
    /// pointer lands on the letter the frame drew rather than on a column at
    /// the pitch.
    [[nodiscard]] std::span<const LetterExtent> letters() const { return m_letters; }
    [[nodiscard]] std::span<const std::uint8_t> letterRows() const { return m_letterRows; }

    /// The letter storage a build writes into, sized to the captured grid.
    ///
    /// Mutable through a const frame because the build is one: the capture
    /// is read-only to the builder, and these are the builder's record of
    /// what it drew rather than part of the capture.
    ///
    /// - Parameter out: The build's output spans, whose letter fields are filled.
    void letterStorage(InstanceBuffers& out) const
    {
        out.letters = m_letters.data();
        out.letterCapacity = m_letters.size();
        out.letterRows = m_letterRows.data();
        out.letterRowCapacity = m_letterRows.size();
    }

    /// Returns image geometry resolved under the same lock as the captured pixels.
    [[nodiscard]] std::span<const Image::ImageQuad> imageQuads() const { return m_imageQuads; }

    /// The rich text blocks the frame shows, in placement order.
    [[nodiscard]] std::span<const RichText::BlockQuad> richTextQuads() const { return m_richTextQuads; }

    /// The drawing of the block at a position in ``richTextQuads()``, shared
    /// with the terminal's cache; it lives as long as this capture.
    [[nodiscard]] const RichText::BlockDrawing& richTextDrawing(std::size_t index) const { return *m_richTextDrawings[index]; }

    /// Whether the cursor stands inside a rich text block as its caret.
    ///
    /// Such a caret belongs to the block's own text: it sits at the block's
    /// insertion offset rather than at a cell edge, and the frontend that
    /// composites the block draws it. Every other cursor is the grid's.
    [[nodiscard]] bool hasRichTextCaret() const { return m_hasRichTextCaret; }

    /// The selection's highlights inside the blocks, under their glyphs.
    [[nodiscard]] std::span<const RichText::HighlightQuad> richTextHighlights() const { return m_richTextHighlights; }

    /// The style-adjusted metrics `inputs()` points at, for a caller that draws
    /// its own decorations over the same geometry.
    [[nodiscard]] const CellMetrics& metrics() const { return m_metrics; }

    /// The absolute row index of the top visible row — the basis prompt marks
    /// and any other scrollback-relative overlay resolve against.
    [[nodiscard]] int firstAbsoluteRow() const { return m_firstAbsoluteRow; }

    /// `options.viewOffset` clamped to the scrollback actually available.
    [[nodiscard]] int viewOffset() const { return m_viewOffset; }

    /// The palette generation captured with the frame.
    [[nodiscard]] std::uint64_t paletteGeneration() const { return m_paletteGeneration; }

    /// The model input generation captured with the frame.
    [[nodiscard]] std::uint64_t modelGeneration() const { return m_modelGeneration; }

    /// The damage rows copied into capture-owned storage.
    ///
    /// Empty means a full frame. A non-empty span contains one byte per visible
    /// row and remains valid until the next capture.
    [[nodiscard]] std::span<const std::uint8_t> capturedDirtyRows() const { return m_capturedDirtyRows; }

    /// The ownership traffic used by the most recent successful capture.
    [[nodiscard]] const FrameCopyStatistics& copyStatistics() const { return m_copyStatistics; }

    /// The `[cursor] motion` policy the captured terminal was configured with.
    ///
    /// Read by the motion tick that runs against this capture, so the glide
    /// follows the terminal's own preference rather than a store the frontend
    /// would have to remember to fill — the store neither shell ever filled.
    [[nodiscard]] CursorMotionMode cursorMotionMode() const { return m_cursorMotionMode; }

    /// The `[cursor] motion_duration` the captured terminal was configured
    /// with, in milliseconds; zero means Core's default slide.
    [[nodiscard]] std::uint32_t cursorMotionDurationMilliseconds() const { return m_cursorMotionDurationMilliseconds; }

#if UBIQUITTY_ENABLE_TEST_HOOKS
    /// Returns the cells copied from a `FrameSource` into capture-owned storage.
    ///
    /// A live-terminal capture reads display rows directly into owned storage,
    /// so this remains zero for that entry point. The source entry point must
    /// copy because its caller may release the source as soon as capture returns.
    ///
    /// - Returns: The cumulative number of copied cells.
    [[nodiscard]] std::size_t sourceCellCopyCountForTest() const { return m_sourceCellCopyCountForTest; }

    /// Returns image-table copies and clears performed by this capture.
    [[nodiscard]] std::size_t imageTableMutationCountForTest() const { return m_imageTableMutationCountForTest; }

    /// Returns placement-table copies and clears performed by this capture.
    [[nodiscard]] std::size_t placementTableMutationCountForTest() const { return m_placementTableMutationCountForTest; }

    /// Returns palette-derived link colours resolved by this capture.
    [[nodiscard]] std::size_t linkColorResolveCountForTest() const { return m_linkColorResolveCountForTest; }
#endif

    /// Starts noting, after each capture, which cells' content changed since
    /// the last one, for the paint heatmap.
    ///
    /// A pass over the captured cells rather than a step inside any renderer:
    /// it reads the frame and writes no pixels, and every arm captures through
    /// here, so no arm can show a map the other would not. Enabling resyncs the
    /// shadow and clears the counters; the next capture is the baseline.
    /// Inert in a build without UBIQUITTY_PERF_DIAG, where the pass is not
    /// compiled.
    void enablePaintHeatmap() { m_paintHeatShadow.enable(); }

    /// Stops the heatmap pass and releases its shadow.
    void disablePaintHeatmap() { m_paintHeatShadow.disable(); }

    /// Whether captures feed the paint heatmap.
    [[nodiscard]] bool isPaintHeatmapEnabled() const { return m_paintHeatShadow.isEnabled(); }

    /// Feeds a frame another capture took to this one's paint heatmap.
    ///
    /// For a renderer presenting a frame it did not capture itself — a
    /// synchronized-output boundary frame the ingest thread froze — so the map
    /// sees every frame the reader saw, not only the live reads.
    ///
    /// - Parameter shown: The capture whose frame was presented.
    void notePaintHeatOf(const TerminalFrameInputs& shown);

    /// Restates the captured cursor as mid-slide, from a motion tick that ran
    /// after the capture.
    ///
    /// The motion tick's target is the captured cursor cell, so it can only run
    /// once the capture holds one — after the options carrying the slide were
    /// consumed. This writes back the one decision that is made in that window
    /// and nothing else; a capture that draws no cursor keeps `row == -1` and
    /// the builder ignores every slide field.
    ///
    /// - Parameters:
    ///   - columnFractional: The interpolated column.
    ///   - vrowFractional: The interpolated visible row.
    void restateCursorSliding(float columnFractional, float vrowFractional)
    {
        m_inputs.cursor.isSliding = true;
        m_inputs.cursor.columnFractional = columnFractional;
        m_inputs.cursor.vrowFractional = vrowFractional;
    }

    /// Applies the presentation clock's blink phase without recapturing grid content.
    ///
    /// - Parameter alpha: The resolved blink opacity, from zero to one.
    void restateCursorMixAlpha(float alpha)
    {
        m_inputs.cursor.mixA = alpha;
        m_cursorPresentationFrame.cursor.mixA = alpha;
    }

    /// Applies presentation timing to the original captured cursor facts.
    ///
    /// Repeated application reads the captured target, never the position a
    /// previous presentation held. Neither the terminal nor its wire state changes.
    ///
    /// - Parameters:
    ///   - presentation: The surface's cursor presentation state.
    ///   - nowSeconds: The monotonic presentation timestamp.
    ///   - intervalSeconds: The display interval, or zero to disable holds.
    ///   - bypass: Whether nearby input-driven movement should be immediate.
    void applyCursorPresentation(CursorPresentation& presentation, double nowSeconds, double intervalSeconds, bool bypass) // bool-ok: forwards the caller's per-frame recent-input predicate.
    {
        m_inputs.cursor = presentation.update(m_cursorPresentationFrame, nowSeconds, intervalSeconds, bypass);
        if (m_inputs.cursor.row >= 0) {
            m_inputs.cursor.lineAttribute = m_lineAttributes[static_cast<std::size_t>(m_inputs.cursor.row)];
        }
    }

    /// Places the presentation cursor over the captured cell and its row geometry.
    ///
    /// - Parameters:
    ///   - column: The visible column to draw over.
    ///   - row: The visible row to draw over.
    void restateCursorPosition(int column, int row)
    {
        restateCursorSettled();
        if (row < 0 || row >= m_inputs.rows || column < 0 || column >= m_inputs.columns
            || (m_inputs.hasStatusLineOnBottomRow && row == m_inputs.rows - 1)) {
            m_inputs.cursor.row = -1;
            return;
        }
        m_inputs.cursor.row = row;
        m_inputs.cursor.column = column;
        m_inputs.cursor.widthCells = 1;
        m_inputs.cursor.lineAttribute = m_lineAttributes[static_cast<std::size_t>(row)];
    }

    /// Restates the captured cursor as settled, the counterpart of
    /// ``restateCursorSliding``.
    ///
    /// Clears the fractional position rather than leaving it: the builder reads
    /// it only while sliding, and a stale position pins the cursor to wherever
    /// it last moved from the first time a caller forgets the order.
    void restateCursorSettled()
    {
        m_inputs.cursor.isSliding = false;
        m_inputs.cursor.columnFractional = 0.0f;
        m_inputs.cursor.vrowFractional = 0.0f;
    }

private:
    /// Selects whether `captureSource` must take ownership of its row inputs.
    enum class SourceRowStorage : std::uint8_t {
        Copy,
        CaptureOwned,
    };

    /// Names the member storage containing the last pristine terminal rows.
    enum class TerminalRowStorage : std::uint8_t {
        None,
        Cells,
        Scratch,
    };

    /// Completes a capture from rows whose ownership is stated explicitly.
    ///
    /// - Parameters:
    ///   - source: The frame state to capture.
    ///   - options: The frame's view and presentation decisions.
    ///   - baseMetrics: The rasterizer metrics to style.
    ///   - glyphSlot: The atlas slot layout.
    ///   - sourceStorage: Whether the rows require a copy.
    ///   - chrome: In-grid chrome to composite, or null.
    /// - Returns: Whether the source produced a frame.
    [[nodiscard]] bool captureSource(const FrameSource& source, const TerminalSnapshotOptions& options,
        const CellMetrics& baseMetrics, const GlyphSlotGeometry& glyphSlot, SourceRowStorage sourceStorage,
        ChromeLayers chrome);

    /// Describes whether a status row covers terminal content in the capture.
    enum class StatusRowPresence {
        Absent,
        Present
    };

    /// Extends model damage with the rows whose frontend presentation changed.
    ///
    /// The returned storage belongs to the capture and is shared by the row
    /// copy and build so neither can omit a row the other redraws.
    ///
    /// - Parameters:
    ///   - damage: The model's dirty rows, or null for a full capture.
    ///   - options: The current view presentation.
    ///   - rowCount: The number of rows in the capture.
    ///   - statusRow: Whether the capture contains a status row.
    ///   - selection: The fill this frame's selection resolved to.
    ///   - scrollDelta: The rows the retained target shifts by before this
    ///     frame draws, zero when it does not.
    /// - Returns: The combined damage, or null for a full capture.
    [[nodiscard]] const std::uint8_t* damageIncludingPresentation(const std::uint8_t* damage,
        const TerminalSnapshotOptions& options, int rowCount, StatusRowPresence statusRow,
        const SelectionFill& selection, int scrollDelta);

    /// Describes whether a baked selection's colour differs from the last one.
    enum class SelectionRecolor {
        Unchanged,
        Changed
    };

    /// Marks the rows a baked selection reached or left since the last capture.
    ///
    /// - Parameters:
    ///   - selection: This frame's baked selection, empty for none.
    ///   - recolor: Whether the baked colour differs from the last one.
    ///   - scrollDelta: The rows the retained target shifts by first.
    ///   - rowCount: The number of rows in the capture.
    void markBakedSelectionRows(std::span<const SelectionRowSpan> selection, SelectionRecolor recolor, int scrollDelta,
        int rowCount);

    /// Copies every borrowed view-tier span into frame-owned storage.
    ///
    /// - Parameter options: The options whose view spans survive the call.
    void captureViewTiers(const TerminalSnapshotOptions& options);

    /// Copies overflow grapheme clusters referenced by the captured cells.
    ///
    /// - Parameter source: The resolver and context valid for this capture.
    void captureClusters(const FrameSource& source);

    /// Copies image metadata, placements, and current decoded payloads.
    ///
    /// Stable payloads are reused when their content generation and displayed
    /// animation frame match the previous capture in this slot. An absent table
    /// clears a populated capture once and leaves an already-empty capture
    /// untouched.
    ///
    /// - Parameter source: The image arrays valid for this capture.
    void captureImages(const FrameSource& source);

    /// Resolves one overflow cluster from frame-owned packed storage.
    ///
    /// - Parameters:
    ///   - cell: A cell in `m_cells`.
    ///   - codePoints: The destination cluster buffer.
    ///   - context: This capture.
    /// - Returns: The copied cluster length, or zero for an invalid cell.
    static int resolveCapturedCluster(const Cell* cell, std::span<std::uint32_t> codePoints, void* context);

    /// Feeds a successful capture to the paint heatmap's shadow.
    void notePaintHeat();

    // The paint heatmap's per-cell content keys; empty while no heatmap watches.
    Diagnostics::PaintHeatShadow m_paintHeatShadow;
    // Where the Terminal overload reads the display rows before handing them
    // over when chrome or a status row needs a pristine baseline beside the
    // composited frame. Ordinary captures read directly into m_cells.
    std::vector<Cell> m_scratchCells;
    std::vector<std::uint8_t> m_scratchLineAttributes;
    std::vector<std::uint8_t> m_scratchRowMarks;
    std::vector<Cell> m_cells;
    // The last build's letters, by row and column, and which rows hold them
    // (``letters``). Written by the build, which runs on a const frame.
    mutable std::vector<LetterExtent> m_letters;
    mutable std::vector<std::uint8_t> m_letterRows;
    std::uint64_t m_rowRefreshGeneration = 0;
    std::vector<std::uint8_t> m_lineAttributes;
    std::vector<std::uint8_t> m_rowFlags;
    std::vector<std::uint8_t> m_rowMarks;
    /// Moves the retained link flags the way the retained cells move.
    ///
    /// The flags are one byte a cell and the scan that fills them follows the
    /// drain's damage, so a row the drain calls clean keeps the flags it has.
    /// A scroll moves a clean row's cells without damaging it, so flags that
    /// stay put describe another line and the row draws the underlines of a
    /// link it does not hold. The same band moved by the same rows keeps every
    /// row's flags over its own cells.
    ///
    /// - Parameters:
    ///   - refresh: The drain's verdict and its pending scroll.
    ///   - rows: The visible row count.
    ///   - columns: The row width.
    void shiftLinkFlagsForScroll(const CapturedRowRefresh& refresh, int rows, int columns);

    std::vector<std::uint8_t> m_linkCells;
    // A pointer move changes presentation without changing Terminal cells. When
    // that happens, this extends the caller's Core damage with the old and new
    // hover rows so the retained target replaces both presentations.
    std::vector<std::uint8_t> m_effectiveDirtyRows;
    // The damage the frame record owns. Empty is the full-frame spelling.
    std::vector<std::uint8_t> m_capturedDirtyRows;
    std::vector<SelectionRowSpan> m_selectionSpans;
    // The selection the last capture baked into its cells, and in what colour;
    // empty when it drew none behind the text. A baked selection changes
    // pixels without changing a cell, so the next capture marks the rows it
    // leaves and reaches from this.
    std::vector<SelectionRowSpan> m_lastBakedSelectionSpans;
    Vec4 m_lastBakedSelectionColor {};
    // Per visible row, the last baked span's columns, for the diff.
    std::vector<int> m_bakedSelectionColumnScratch;
    // Per visible row, how many baked selection pieces the last and the next
    // frame hold on it, capped at two: a row of two is compared by its pieces.
    std::vector<std::uint8_t> m_bakedSelectionPieceCount;
    std::vector<SelectionRowSpan> m_searchMatchSpans;
    // The focused pill's bounce clock, advanced once per capture.
    FocusedMatchBounceClock m_focusedMatchBounce;
    // The captured terminal's motion policy, for the tick that runs after
    // the capture; filled whether or not the frame draws a cursor, since a
    // hidden cursor still pins the motion state to its cell.
    CursorMotionMode m_cursorMotionMode = CursorMotionMode::Off;
    std::uint32_t m_cursorMotionDurationMilliseconds = 0;
    CursorPresentation::Frame m_cursorPresentationFrame;
    std::vector<SelectionRowSpan> m_otherSearchMatchSpans;
    std::vector<SelectionRowSpan> m_triggerHighlightSpans;
    std::vector<SelectionRowSpan> m_hiliteRegionSpans;
    TermPalette m_palette {};
    // Derived from the owned palette, invalidated whenever that palette is copied.
    std::optional<ColorRGBA> m_paletteLinkColor;
    TextSizeTable m_textSizeTable;
    bool m_hasPalette = false;
    bool m_hasPaletteGeneration = false;
    bool m_hasTextSizeTable = false;
    std::array<Ubiquitty::Image::TerminalImage, Ubiquitty::Image::maxTerminalImageCount> m_images {};
    std::array<std::vector<unsigned char>, Ubiquitty::Image::maxTerminalImageCount> m_imagePixels;
    std::array<Ubiquitty::Image::TerminalImagePlacement, Ubiquitty::Image::maxTerminalImagePlacementCount> m_placements {};
    bool m_hasImages = false;
    bool m_hasPlacements = false;
    // One packed offset and length per cell with an overflow cluster. Inline
    // clusters remain wholly in Cell and need no side storage.
    std::vector<std::uint32_t> m_clusterOffsets;
    std::vector<std::uint16_t> m_clusterLengths;
    std::vector<std::uint32_t> m_clusterCodePoints;
    // The soft font this capture hands the builder, held by value so it outlives
    // the source's lock. Allocated on the first DECDLD a session commits and
    // refreshed only when the serial changes, so a session that never downloads
    // one carries nothing and one that does pays the copy per download.
    std::unique_ptr<Ubiquitty::VT::SoftFont> m_softFont;
    // The serial the last capture displaced, for the builder to release. Held
    // rather than derived: by the time the builder runs, m_softFont is the NEW
    // font and the one it replaced is gone.
    /// Resolves the blocks the frame shows, their drawings and the
    /// selection's highlights in them; the drawings are the terminal's,
    /// built once per block and table, so a static block costs a copy of a
    /// pointer per frame.
    ///
    /// - Parameters:
    ///   - terminal: The model, read under its lock.
    ///   - options: The capture's options.
    void collectRichTextQuads(const Terminal& terminal, const TerminalSnapshotOptions& options);
    std::uint32_t m_retiredSoftFontSerial = 0;
    // Value-initialized: GlyphSlotGeometry declares no member defaults, and a
    // zero margin is what selects the cell-baked path for a caller that never
    // sets it.
    GlyphSlotGeometry m_glyphSlot {};
    CellMetrics m_metrics;
    std::vector<Image::ImageQuad> m_imageQuads;
    std::vector<RichText::BlockQuad> m_richTextQuads;
    std::vector<std::shared_ptr<const RichText::BlockDrawing>> m_richTextDrawings;
    std::vector<RichText::HighlightQuad> m_richTextHighlights;
    bool m_hasRichTextCaret = false;
    BuildInputs m_inputs;
    int m_firstAbsoluteRow = 0;
    int m_viewOffset = 0;
    std::uint64_t m_modelGeneration = 0;
    std::uint64_t m_paletteGeneration = 0;
    FrameCopyStatistics m_copyStatistics;
    TerminalRowStorage m_terminalRowStorage = TerminalRowStorage::None;
    // Declared in every build and counted only in the hooked one. A member
    // behind a build flag gives the class two layouts: a test binary that
    // links library code built without the flag allocates the smaller one and
    // constructs the larger into it, overrunning the stack of whoever holds it.
    [[maybe_unused]] std::size_t m_sourceCellCopyCountForTest = 0;
    [[maybe_unused]] std::size_t m_imageTableMutationCountForTest = 0;
    [[maybe_unused]] std::size_t m_placementTableMutationCountForTest = 0;
    [[maybe_unused]] std::size_t m_linkColorResolveCountForTest = 0;
    // Lineage for the damage-gated copy: a partial capture may keep a clean
    // row's cells only while they are byte-identical to the source's, which
    // holds when the shape and view offset match the previous capture, that
    // capture copied every row it now skips, and nothing was composited over
    // them since. Chrome breaks the last condition, so a chrome frame leaves
    // the lineage invalid and the next capture copies whole.
    int m_capturedRows = 0;
    int m_capturedColumns = 0;
    bool m_hasFullLineage = false;
    bool m_lastCaptureHadStatusRow = false;
    /// Whether this capture kept the cells of the rows its damage calls clean.
    ///
    /// Distinct from the damage itself, which describes the frontend's retained
    /// target: a capture whose refresh was demoted re-reads every row and still
    /// forwards that damage, so per-cell state derived from the cells has to
    /// follow this instead.
    bool m_didKeepCapturedRows = true;
    bool m_lastCaptureUnderlinedLinks = false;
    bool m_lastCaptureHoverLinkActive = false;
    std::uint16_t m_lastCaptureHoverLinkID = 0;
    int m_lastCaptureHoverLinkRowFirst = 0;
    int m_lastCaptureHoverLinkRowLast = -1;
    int m_lastCaptureHoverLinkColumnFirst = 0;
    int m_lastCaptureHoverLinkColumnLast = -1;
    int m_lastCaptureHoverLinkSpanFirst = 0;
    int m_lastCaptureHoverLinkSpanLast = -1;
    bool m_lastCaptureHoverTextVerified = false;
    // The terminal a Terminal-taking capture is reading, for the duration of
    // that capture only; null in a capture from a source alone.
    const Terminal* m_linkKindTerminal = nullptr;
};

} // namespace Rendering
} // namespace Ubiquitty
