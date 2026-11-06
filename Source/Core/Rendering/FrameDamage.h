// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"

#include "Base/SwiftInterop.h"
#include "Core/GridBounds.h"
#include "Core/Rendering/GridCapturePlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// FrameDamage — the damage-first drain, as one Core call.
//
// GridCapturePlan.h holds the scalar policy: whether a retained buffer may
// update incrementally. This file holds the drain that applies it — settle the reuse
// verdict, consume the grid's paint session, mark the dirty rows, and hand back
// the pending scroll and the baseline for the next capture.
//
// One implementation, because the drain carries ordering rules a re-writer
// would rediscover as pixel bugs: the scroll-exposed band must be marked dirty
// at drain time, before the incremental runs are read, or scroll coalescing
// drops the exposed rows and the top of the screen keeps last frame's lines.
// A frontend typing its own copy of this loop has to find that order again.

namespace Ubiquitty {
class CellGrid;
struct Cell;
struct Terminal;
}

namespace Ubiquitty::Rendering {

/// One frame's drained damage: the reuse verdict, the pending scroll, and the
/// baseline the buffer stores for its next capture.
///
/// `scrollBottom` is inclusive; -1 means no pending scroll survived the bounds
/// check. `canReuse` already folds in `CaptureEligibility::trustDamage`, so a
/// caller gates its incremental path on this field alone.
struct UBIQUITTY_ESCAPABLE FrameDamage {
    bool canReuse = false;
    int scrollTop = 0;
    int scrollBottom = -1;
    int scrollDelta = 0; // >0 scrolls up
    /// From a bare drain, the grid's sticky blink hint. ``planRetainedCapture``
    /// refines it to the screen the capture reads: the live screen's own cells
    /// at the tail, the unretracted hint when scrolled back.
    bool hasBlink = false;
    /// The grid's damage generation after this drain — the value the buffer
    /// records so its NEXT capture can prove no peer drained in between.
    std::uint64_t generationAfterDrain = 0;
};

/// Whether any of `cells` carries SGR blink.
///
/// ``CellGrid::hasBlinkHint`` is monotone — any blink-attributed write arms it
/// and nothing clears it — so a surface that once showed a blinking cell keeps
/// its animation clock running for the life of the grid. This is the whole-
/// screen reading a viewer needs to retract that for itself, and it is a scan
/// of its own rather than a flag the frame builder accumulates: the retraction
/// is wanted occasionally, the builder runs every frame, and the builder is
/// where the profile's counters live.
///
/// A reader that retracts owes a second check. The reading describes the moment
/// it was taken, so it may be acted on only when it is not older than
/// ``CellGrid::blinkHintGeneration`` — otherwise a cell that began blinking
/// after the scan disarms a clock with blink on screen.
///
/// - Parameters:
///   - cells: The screen, row-major, `rows` × `columns`.
///   - rows: The row count.
///   - columns: The column count, and the row stride.
/// - Returns: Whether any cell carries `Attribute::Blink`; false for an empty
///   or absent screen.
[[nodiscard]] bool anyCellBlinks(const Cell* cells, int rows, int columns);

/// Whether any cell of `terminal`'s live screen carries SGR blink.
///
/// The same reading as the span overload, over the rows the terminal addresses
/// one at a time. It covers the live screen only: a viewer scrolled back
/// displays scrollback rows this does not reach, so a false here retracts the
/// hint for a viewer at the tail and says nothing about one that has scrolled
/// away.
///
/// - Parameter terminal: The terminal whose live screen is read.
/// - Returns: Whether any cell carries `Attribute::Blink`.
[[nodiscard]] bool anyCellBlinks(const Terminal& terminal);

/// Marks every visible row carrying an SGR-blink cell dirty, and says whether any did.
///
/// A blinking cell changes phase without any write, so nothing puts its row in
/// the drain's dirty set and a partial frame would hold it frozen. The whole-
/// frame answer to that is to refuse the retained target, which rebuilds every
/// row of a screen where one cell animates; this is the same scan reporting
/// which rows, so the hazard is paid a row at a time.
///
/// Reads the live screen at the tail. A caller displaying scrollback is looking
/// at rows this does not scan and must stay conservatively armed instead.
///
/// - Parameters:
///   - terminal: The terminal whose visible rows are read.
///   - rowDirty: The drain's dirty span; entries past its end are not marked.
/// - Returns: Whether any row was marked.
bool markBlinkRowsDirty(const Terminal& terminal, std::span<std::uint8_t> rowDirty);

/// Drains one frame of damage from `grid` into `rowDirty`.
///
/// Consumes the grid's paint session: acknowledges the pending scroll and every
/// dirty run, sets `rowDirty[row] = 1` for each dirtied visible row, and marks
/// the scroll-exposed band dirty regardless of what the runs said. Rows the
/// session names outside `[0, rows)` are acknowledged and dropped.
///
/// The caller zeroes nothing: `rowDirty` is cleared here, so a buffer reusing
/// its vector cannot carry last frame's marks into this one.
///
/// - Parameters:
///   - grid: The live grid, under the model lock.
///   - eligibility: The buffer's baseline against this capture.
///   - rowDirty: One byte per visible row, written 0 or 1. Its length is the
///     row count.
/// - Returns: The verdict, the scroll, and the next baseline. Defaults when
///   `rowDirty` is empty.
[[nodiscard]] FrameDamage drainFrameDamage(CellGrid& grid, const CaptureEligibility& eligibility,
    std::span<std::uint8_t> rowDirty);

/// The hazards a retained-target frontend folds into its reuse verdict, and
/// the two facts only the frontend knows.
///
/// Everything here except `isTargetMatching` and `hasBackdrop` is a snapshot
/// fact every backend carries; the policy of which hazards force a full frame lives in
/// ``canReuseRetainedTarget`` so two backends cannot rank them differently.
struct RetainedTargetEligibility {
    bool isTargetMatching = false; // the retained texture matches the drawable
    bool hasBlink = false; // blink cells change phase outside the dirty set
    bool hasStatusLine = false; // a DECSSDT row changes outside the dirty set
    /// The window's picture is composited under the grid on every frame and
    /// over the whole target, margins included: drawn again over Loaded
    /// pixels it builds up wherever the window is translucent, on every row
    /// the frame did not repaint.
    bool hasBackdrop = false;
    std::size_t rowDirtyCount = 0; // the drain's span, which must cover the grid
    int rows = 0;
};

/// Whether this frame may keep the retained target's pixels.
///
/// The drain's verdict says the retained STATE is coherent; this adds the
/// render-side hazards — a blink phase or a status row changes without ever
/// entering the drain's dirty set, so a partial frame would hold them frozen.
/// A frontend contributes two facts: whether its target survived at this
/// size, and whether it composites a backdrop under the grid.
///
/// - Parameters:
///   - damage: The drain's verdict for this frame.
///   - eligibility: The hazards, and the frontend's facts.
/// - Returns: Whether to Load the retained pixels and build only dirty rows.
[[nodiscard]] inline bool canReuseRetainedTarget(const FrameDamage& damage, const RetainedTargetEligibility& eligibility)
{
    return damage.canReuse && eligibility.isTargetMatching && !eligibility.hasBlink
        && !eligibility.hasStatusLine && !eligibility.hasBackdrop && eligibility.rows > 0
        && eligibility.rowDirtyCount == static_cast<std::size_t>(eligibility.rows);
}

/// Read-only bytes, one per row or row boundary.
///
/// Named because a span's extent is a template argument with no Swift spelling,
/// so an unnamed one drops the declaration from the generated interface.
using RowBytes = std::span<const std::uint8_t>;

/// Bytes, one per row or row boundary, that a call writes.
using RowBytesToWrite = std::span<std::uint8_t>;

/// Settles whether a frame that Loads the retained target and rebuilds only
/// its dirty rows draws what a full frame draws, as far as ink crossing
/// between rows goes, and moves the target's record of its crossings with
/// the frame.
///
/// A rebuilt row repaints its own band — the fills, then its ink — and nothing
/// outside it. Ink a neighbouring row drew into that band is painted over and
/// not drawn again, and ink the row's old content reached into a neighbour's
/// band stays there after the content is gone: a stacked accent erased from a
/// row leaves its top on the row above for as long as that row stays clean.
/// So a rebuilt row may border no crossing, in the target or in the build.
///
/// A scroll moves the target's rows by whole bands. Ink crossing between two
/// rows that both survive the blit moves with them and stays whole, so its
/// crossing moves too. Ink crossing an edge the blit cuts — the region's top
/// and bottom, and the edge between the rows that leave and the rows that stay
/// — would be split, and refuses.
///
/// The crossings are ``InstanceBuffers::rowInkCrossings``: the target's as the
/// last frame left them, the build's from this frame's.
///
/// - Parameters:
///   - dirtyRows: The rows this frame's build repainted, one byte per row.
///   - targetCrossings: Where the target's ink crosses, `rows + 1` entries;
///     moved with the scroll when the frame stands, untouched when it does not.
///   - builtCrossings: Where this build's ink crosses, `rows + 1` entries.
///   - scrollTop: The first row of the scroll the blit performs.
///   - scrollBottom: Its last row, inclusive.
///   - scrollDelta: The rows it moves by, positive up; zero for no scroll.
/// - Returns: Whether the partial frame may stand; false rebuilds every row,
///   after which the caller records the build's crossings as the target's.
[[nodiscard]] inline bool settleRowInkCrossings(RowBytes dirtyRows, RowBytesToWrite targetCrossings, RowBytes builtCrossings,
    int scrollTop, int scrollBottom, int scrollDelta)
{
    const int rows = static_cast<int>(dirtyRows.size());
    if (rows == 0 || targetCrossings.size() != dirtyRows.size() + 1 || builtCrossings.size() != dirtyRows.size() + 1)
        return false;
    std::array<std::uint8_t, Ubiquitty::maxRows + 1> moved {};
    if (rows > Ubiquitty::maxRows)
        return false;
    std::copy(targetCrossings.begin(), targetCrossings.end(), moved.begin());
    if (scrollDelta != 0) {
        if (scrollTop < 0 || scrollBottom < scrollTop || scrollBottom >= rows)
            return false;
        const int shift = scrollDelta > 0 ? scrollDelta : -scrollDelta;
        // Boundary b is the top edge of row b. The blit keeps rows
        // [top + shift, bottom] (up) or [top, bottom - shift] (down); the
        // edges it cuts are the region's two and the one between the rows it
        // drops and the rows it keeps, and every edge among the dropped rows.
        const int firstKept = scrollDelta > 0 ? scrollTop + shift : scrollTop;
        const int lastKept = scrollDelta > 0 ? scrollBottom : scrollBottom - shift;
        for (int boundary = scrollTop; boundary <= scrollBottom + 1; ++boundary) {
            const bool isInsideKeptBand = boundary > firstKept && boundary <= lastKept;
            if (!isInsideKeptBand && targetCrossings[static_cast<std::size_t>(boundary)])
                return false;
        }
        for (int boundary = scrollTop; boundary <= scrollBottom + 1; ++boundary)
            moved[static_cast<std::size_t>(boundary)] = 0;
        for (int boundary = firstKept + 1; boundary <= lastKept; ++boundary)
            moved[static_cast<std::size_t>(boundary - scrollDelta)] = targetCrossings[static_cast<std::size_t>(boundary)];
    }
    for (int row = 0; row < rows; ++row) {
        if (!dirtyRows[static_cast<std::size_t>(row)])
            continue;
        const std::size_t above = static_cast<std::size_t>(row);
        const std::size_t below = above + 1;
        if (moved[above] || moved[below] || builtCrossings[above] || builtCrossings[below])
            return false;
    }
    std::copy_n(moved.begin(), targetCrossings.size(), targetCrossings.begin());
    return true;
}

/// One band a scroll blit copies between the retained textures, in pixels.
struct UBIQUITTY_ESCAPABLE ScrollBlitBand {
    int sourceYInPixels = 0;
    int destinationYInPixels = 0;
    int heightInPixels = 0;
};

/// The bands of a laid-out scroll blit.
///
/// Named because a span's extent is a template argument with no Swift spelling,
/// so an unnamed one drops the declaration from the generated interface rather
/// than failing to compile.
using ScrollBlitBandSpan = std::span<const ScrollBlitBand>;

/// The bands a pending scroll copies from the front texture to the back.
///
/// At most three: the rows above the scroll region carry over unshifted, the
/// region's surviving rows move by the shift, and the rows below carry over
/// unshifted. A zero count means the scroll fits no valid plan and the caller
/// rebuilds the frame in full.
struct UBIQUITTY_ESCAPABLE ScrollBlitPlan {
    int bandCount = 0;
    ScrollBlitBand bands[3] = {};

    /// The bands this plan actually laid out.
    ///
    /// The count and the storage travel together, so a caller iterates what was
    /// planned rather than pairing the array with a separate bound it could
    /// carry past the end. Swift reaches a C array only through an unsafe cast
    /// and reads the count itself, which is the pairing this returns instead.
    [[nodiscard]] ScrollBlitBandSpan activeBands() const
    {
        return ScrollBlitBandSpan(static_cast<const ScrollBlitBand*>(bands), static_cast<std::size_t>(bandCount));
    }
};

/// Lays out the scroll blit for a retained grid texture.
///
/// One implementation, because every backend with a retained texture performs
/// the same three copies and the band edges are exactly the arithmetic a
/// re-writer gets almost right: a band clamped against the wrong edge copies a
/// partial row, and the tear sits at the bottom of the scroll region where a
/// full-height test scene never looks.
///
/// The exposed band is not in the plan: the drain marked it dirty at drain
/// time, and the content pass repaints it over whatever the copy left.
///
/// - Parameters:
///   - damage: The drain's scroll fields for this frame.
///   - rows: The visible row count.
///   - cellHeightInPixels: One row's height.
///   - originYInPixels: Where row zero starts in the texture.
///   - textureHeightInPixels: The retained texture's height, which every band
///     is clamped against.
/// - Returns: Up to three bands, or none when no valid plan exists.
[[nodiscard]] ScrollBlitPlan scrollBlitPlan(const FrameDamage& damage, int rows, int cellHeightInPixels,
    int originYInPixels, int textureHeightInPixels);

/// What a retained-row refresh needs to know about the frame it updates.
///
/// `rowDirty` is consulted only when `canReuseRows` is true; empty reads as
/// every row dirty, which degrades to the full re-read rather than to stale
/// cells.
struct CapturedRowRefresh {
    bool canReuseRows = false; // the drain's verdict, and only if the buffer kept its size
    int scrollTop = 0;
    int scrollBottom = -1; // inclusive; -1 = no pending scroll
    int scrollDelta = 0; // >0 scrolls up
    int viewOffset = 0;
    int firstAbsoluteRow = 0; // scrollbackCount - viewOffset
    /// The model generation after the damage producing this refresh was
    /// drained. Zero means the caller supplied no generation.
    std::uint64_t generationAfterDrain = 0;
    /// The retained frame this damage updates; zero when no lineage is supplied.
    std::uint64_t baseGeneration = 0;
    std::span<const std::uint8_t> rowDirty; // one byte per row, from the drain
};

/// The ownership traffic needed to assemble one immutable frame.
///
/// Cell bytes include source-row reads and in-capture row moves. Image bytes
/// are counted separately because their payloads can dwarf the grid and are
/// reused by content generation.
struct UBIQUITTY_ESCAPABLE FrameCopyStatistics {
    /// Bytes of `Cell` storage copied while producing the frame.
    std::uint64_t cellBytesCopied = 0;
    /// Decoded image bytes copied into frame-owned storage.
    std::uint64_t imageBytesCopied = 0;
    /// Visible rows whose attributes and damage were inspected.
    std::uint32_t rowsInspected = 0;
    /// Row-sized cell copies, including scroll-preservation moves.
    std::uint32_t rowsCopied = 0;
};

/// The baseline a retained frame proves itself against, and advances.
///
/// One per retained buffer, owned by the frontend and passed back to every
/// capture. Every field is written by the plan; the one act a frontend takes
/// on it is invalidation — clearing `isInitialized` on the frames it declines
/// to capture — because a frontend that wrote the others would disagree with
/// the drain about what the last frame was.
struct RetainedFrameBaseline {
    bool isInitialized = false;
    /// The grid this baseline last drained. A damage generation counts one
    /// grid's drains, so two grids can stand at the same count: a renderer
    /// drawing two panes of one size in turns would read the other pane's
    /// generation as its own and keep that pane's rows on this one's target.
    /// Compared by identity, so only the grid that drew the target vouches
    /// for it.
    const CellGrid* grid = nullptr;
    std::uint64_t generation = 0;
    int viewOffset = 0;
    int rows = 0;
    int columns = 0;
    bool isReverseVideo = false;
    std::uint64_t paletteEpoch = 0;
    /// The focused search match's run as the last frame drew it, in
    /// visible-grid space; ``matchRow0 > matchRow1`` means there was none.
    /// Read off the terminal's search by the plan itself: the match is the
    /// terminal's, and a frontend asked to report it is a frontend that can
    /// forget to.
    ///
    /// Kept because the match's highlight is baked into the cell — the builder
    /// overrides the background of every cell in the run, where an overlay
    /// quad would be rebuilt each frame — so a run that moves owes the rows it
    /// reached and the rows it left, and one that merely persists owes
    /// nothing. The cursor needs no such memory: the grid never holds one.
    int matchRow0 = 0;
    int matchRow1 = -1;
    int matchColumn0 = 0;
    int matchColumn1 = -1;
    /// Whether the last frame painted an image into the retained pixels, so
    /// the frame after a picture leaves still repaints the rows it covered.
    bool hadImagesOnViewport = false;
};

/// The facts one frame's plan needs from its frontend.
///
/// Everything else the plan derives from the terminal and the drain; these are
/// the quantities only the frontend holds — its view, its target, and its
/// cursor policy.
struct RetainedFrameRequest {
    int rows = 0;
    int columns = 0;
    int viewOffset = 0;
    /// Whether the retained target survived at this frame's size — a
    /// render-side fact a frontend contributes.
    bool isTargetMatching = false;
    /// Whether the frame composites a window backdrop under the grid. See
    /// ``RetainedTargetEligibility``.
    bool hasBackdrop = false;
    int cellHeightInPixels = 0;
    int originYInPixels = 0;
    int textureHeightInPixels = 0;
    /// Whether the retained grid survives between frames at all; this
    /// process's ``isRetainedGridCacheEnabled`` unless a test states another.
    bool isGridCacheEnabled = isRetainedGridCacheEnabled();
};

/// One frame's complete reuse plan: the verdict, the copies, and the refresh.
struct RetainedFramePlan {
    FrameDamage damage;
    /// Whether the frame may Load the retained target and build only the
    /// dirty rows. False rebuilds the frame in full — including when a
    /// pending scroll fits no valid blit plan.
    bool shouldLoadRetainedTarget = false;
    /// The scroll copies, when ``shouldLoadRetainedTarget`` and a scroll is
    /// pending; zero bands otherwise.
    ScrollBlitPlan blitPlan;
    /// Ready for ``refreshCapturedRows``. Its `rowDirty` span borrows the
    /// buffer the caller passed to the plan, so it lives exactly as long.
    CapturedRowRefresh refresh;
};

/// A frame request holding every field's stated default.
///
/// Swift's synthesised `init()` for an imported C++ struct may zero-fill it
/// without running the member initialisers, and two of this request's
/// defaults are not zero: the grid cache switch, which zero would turn off,
/// and nothing a frontend would notice until frames stopped reusing. A Swift
/// caller builds its request here.
///
/// - Returns: The request, defaults applied.
[[nodiscard]] inline RetainedFrameRequest makeRetainedFrameRequest() { return RetainedFrameRequest {}; }

/// The plan for a full rebuild, every field at its stated default.
///
/// For the same reason as ``makeRetainedFrameRequest``: a zero-filled plan
/// carries a scroll region of row zero rather than none.
///
/// - Returns: The default plan, which rebuilds every row.
[[nodiscard]] inline RetainedFramePlan makeFullRebuildPlan() { return RetainedFramePlan {}; }

/// The capture-time half's request: the frontend facts that exist under the
/// model lock, before any render target is known.
struct RetainedCaptureRequest {
    int rows = 0;
    int columns = 0;
    int viewOffset = 0;
    /// False for a capture whose damage stream is not authoritative — an
    /// off-main pending capture reads state a later live capture drains
    /// again, and trusting it would reuse rows the live drain never saw.
    bool shouldTrustDamage = true;
    /// Whether the retained grid survives between frames at all; this
    /// process's ``isRetainedGridCacheEnabled`` unless a test states another.
    /// Off, no capture trusts its damage, so every one is Full.
    bool isGridCacheEnabled = isRetainedGridCacheEnabled();
    /// Whether the frame bakes the focused search match's fill into its cells.
    /// False for a boundary capture, which is built without the view tiers:
    /// its pixels hold the match's cells unfilled, so the baseline it leaves
    /// records no match, and the next frame that bakes one owes its rows.
    bool isFocusedMatchBaked = true;
};

/// What capture time hands encode time: the drain's results plus the snapshot
/// facts the render verdict needs, all read under the model lock so the
/// encode half can run off it.
struct RetainedCapture {
    FrameDamage damage;
    /// Ready for ``refreshCapturedRows``. Its `rowDirty` span borrows the
    /// buffer the caller passed to the capture, so it lives exactly as long.
    CapturedRowRefresh refresh;
    bool hasStatusLine = false;
    std::size_t rowDirtyCount = 0;
};

/// Drains and captures a retained frame's state: the reuse verdict, the
/// dirty rows, the focused match's rows, the refresh descriptor, and the
/// baseline advance.
///
/// The first half of ``planRetainedFrame``, split out because a frontend may
/// capture under the model lock long before it knows anything about a render
/// target — the second half, ``planRetainedEncode``, is a pure function of
/// this capture and runs wherever the target lives.
///
/// - Parameters:
///   - grid: The live grid, under the model lock.
///   - terminal: The live terminal the hazards are read from.
///   - baseline: The buffer's baseline; read for eligibility and advanced.
///   - request: The frontend's capture-time facts.
///   - rowDirty: One byte per visible row, written 0 or 1. Its length must
///     equal `request.rows` or the capture refuses whole.
/// - Returns: The capture. Defaults — a full rebuild — when refused.
[[nodiscard]] RetainedCapture planRetainedCapture(CellGrid& grid, const Terminal& terminal,
    RetainedFrameBaseline& baseline, const RetainedCaptureRequest& request, std::span<std::uint8_t> rowDirty);

/// The encode-time half's request: the render target's facts.
struct RetainedEncodeRequest {
    /// Whether the retained target survived at this frame's size — the one
    /// render-side fact a frontend contributes.
    bool isTargetMatching = false;
    /// Whether the frame composites a window backdrop under the grid. See
    /// ``RetainedTargetEligibility``.
    bool hasBackdrop = false;
    int rows = 0;
    int cellHeightInPixels = 0;
    int originYInPixels = 0;
    int textureHeightInPixels = 0;
};

/// The encode-time half's verdict and copies.
struct RetainedEncodePlan {
    /// Whether the frame may Load the retained target and build only the
    /// dirty rows. False rebuilds the frame in full — including when a
    /// pending scroll fits no valid blit plan.
    bool shouldLoadRetainedTarget = false;
    /// The scroll copies, when ``shouldLoadRetainedTarget`` and a scroll is
    /// pending; zero bands otherwise.
    ScrollBlitPlan blitPlan;
};

/// Settles the render verdict and lays out the scroll copies for a capture.
///
/// A pure function of the capture and the target facts — no terminal, no
/// lock — so a frontend whose encode runs far from its capture calls it
/// exactly where the target is known.
///
/// - Parameters:
///   - capture: The capture-time half's result.
///   - request: The render target's facts.
/// - Returns: Whether to Load, and the copies a pending scroll performs.
[[nodiscard]] RetainedEncodePlan planRetainedEncode(const RetainedCapture& capture,
    const RetainedEncodeRequest& request);

/// Plans one retained frame: drains the damage, settles both verdicts, lays
/// out the scroll copies, and advances the baseline.
///
/// ``planRetainedCapture`` then ``planRetainedEncode``, for the frontend
/// whose capture and encode share one call site.
///
/// The one implementation of the ordering rules the two shipping frontends
/// each assembled by hand: the exposed band marked at drain time, the baked
/// cursor's rows marked after — the ghost's row SHIFTED by the pending
/// scroll, because the blit moves its pixels with the band — the render
/// verdict settled before any builder runs, and a scroll that fits no valid
/// band plan demoted to a full rebuild.
///
/// - Parameters:
///   - grid: The live grid, under the model lock.
///   - terminal: The live terminal the hazards are read from.
///   - baseline: The buffer's baseline; read for eligibility and advanced.
///   - request: The frontend's facts for this frame.
///   - rowDirty: One byte per visible row, written 0 or 1. Its length must
///     equal `request.rows` or the plan refuses whole.
/// - Returns: The plan. Defaults — a full rebuild — when refused.
[[nodiscard]] RetainedFramePlan planRetainedFrame(CellGrid& grid, const Terminal& terminal,
    RetainedFrameBaseline& baseline, const RetainedFrameRequest& request, std::span<std::uint8_t> rowDirty);

/// The buffer a plan writes one dirty byte per visible row into.
///
/// Named because a caller that cannot spell a bare `std::span` cannot call a
/// function taking one — the same reason `AlphaSpan` exists beside the session's
/// palette setter.
using RowDirtySpan = std::span<std::uint8_t>;

/// Plans one retained frame for a caller holding a `Terminal` and not its grid.
///
/// The grid is a `std::unique_ptr` member, so a caller that reaches the terminal
/// through an imported seam cannot dereference it to call the overload above.
/// This is that overload with the one dereference moved to this side, which is
/// the same split `TerminalFrameInputs::capture` makes for the same reason.
///
/// - Parameters:
///   - terminal: The terminal to plan against, holding the grid. A terminal with
///     no grid returns the default plan, which is a full rebuild.
///   - baseline: The buffer's baseline; read for eligibility and advanced.
///   - request: The frontend's facts for this frame.
///   - rowDirty: One byte per visible row, written 0 or 1.
/// - Returns: The plan. Defaults — a full rebuild — when refused.
[[nodiscard]] RetainedFramePlan planRetainedFrame(Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedFrameRequest& request, RowDirtySpan rowDirty);

/// Whether a build lets DECSET 7402's opaque letterbox stand in for the glyphs
/// under it, or keeps the full glyph path for a pixel-parity A/B
/// (UBIQUITTY_NO_OCCLUSION_SKIP on both macOS arms).
enum class OcclusionSkip : std::uint8_t {
    Enabled,
    Disabled,
};

/// Whether this frame may skip the damage drain and the grid copy outright.
///
/// The scalar rule is ``captureCanSkipGrid``; this reads its two terminal
/// facts — DECSET 7402 and the Color letterbox — off the terminal under the
/// model lock, so a frame loop that holds the lock spells the eligibility
/// once. The knob is the caller's: a build that keeps the full glyph path for
/// a parity A/B still reaches the cells, so it must keep capturing them.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - occlusionSkip: Whether the letterbox occludes the glyphs on this build.
/// - Returns: Whether no pixel of this frame reads a cell.
[[nodiscard]] bool canSkipGridCapture(const Terminal& terminal, OcclusionSkip occlusionSkip);

/// The plan for a frame whose grid nobody reads.
///
/// DECSET 7402 with an opaque letterbox paints every cell the letterbox
/// colour and no glyph, so the frame owes no row and reads no cell: the damage
/// stays undrained, the dirty set is cleared, and the refresh keeps every
/// captured row as it is — the cells the capture holds are the ones from
/// before the skip, and nothing reads them. The target is not loaded, because
/// the letterbox is rebuilt from the mode rather than from pixels.
///
/// The baseline is left uninitialised on purpose: the first plan after the
/// mode clears then re-reads every row, so a write made under the letterbox
/// reaches the screen even where the damage stream could not name it.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - baseline: The buffer's baseline, declared gone.
///   - request: The frontend's capture-time facts; only the shape is read.
///   - rowDirty: One byte per visible row, written 0.
/// - Returns: The plan. Defaults — a full rebuild — when the span is not the
///   row count.
[[nodiscard]] RetainedFramePlan planSkippedGridFrame(const Terminal& terminal, RetainedFrameBaseline& baseline,
    const RetainedFrameRequest& request, RowDirtySpan rowDirty);

/// Declares this frame's painted pixels gone, whatever the cells hold.
///
/// Two states decide what a frame paints, and a caller that resets one without
/// the other gets the combination that draws nothing: the retained target is
/// dropped, so the pass clears, and the drain — the cells being unchanged —
/// names no row to paint over the clear. The blank then survives, because
/// every later frame reuses the target it just blanked.
///
/// What calls for this is any replacement of what the pixels were drawn WITH
/// rather than of what they say: rebuilt faces, resized sheets, a new atlas, a
/// retained cell buffer swapped for an empty one. None of those changes a
/// cell, which is exactly why no damage reaches them.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - baseline: The baseline the next capture is diffed against.
void invalidateRetainedFrame(Terminal& terminal, RetainedFrameBaseline& baseline);

/// Refreshes a retained row buffer from the live terminal and measures its
/// ownership traffic.
///
/// Shifts the scrolled band in place first — the retained cells move the way
/// the screen did, so a clean row's cells are already where the frame expects
/// them — then re-reads only the dirty rows. Line attributes and marks refresh
/// for every row regardless: they are per-row bytes whose re-read costs less
/// than tracking their damage separately.
///
/// The invariant a caller may hold this to: the buffer afterwards equals a full
/// re-read, byte for byte. Reuse changes the work, never the result.
///
/// The spans carry the geometry: `lineAttributes` is one byte per row, and
/// `cells` must divide evenly by the row count — a buffer whose shape
/// disagrees with itself is refused whole rather than read past.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - refresh: The drain's verdict and scroll, and the view this frame shows.
///   - cells: The retained copy to update, rows x columns.
///   - lineAttributes: One byte per row.
///   - rowMarks: One byte per row.
/// - Returns: The refreshed buffer's row and byte traffic.
[[nodiscard]] FrameCopyStatistics refreshCapturedRowsWithStatistics(const Terminal& terminal,
    const CapturedRowRefresh& refresh,
    std::span<Cell> cells, std::span<std::uint8_t> lineAttributes, std::span<std::uint8_t> rowMarks);

/// Refreshes a retained row buffer when the caller needs no traffic counters.
///
/// The update semantics are those of ``refreshCapturedRowsWithStatistics``;
/// only the returned accounting is discarded on this explicitly void surface.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - refresh: The drain's verdict and scroll, and the view this frame shows.
///   - cells: The retained copy to update, rows x columns.
///   - lineAttributes: One byte per row.
///   - rowMarks: One byte per row.
void refreshCapturedRows(const Terminal& terminal, const CapturedRowRefresh& refresh,
    std::span<Cell> cells, std::span<std::uint8_t> lineAttributes, std::span<std::uint8_t> rowMarks);

/// Where an incrementally refreshed buffer first differs from a full re-read.
///
/// `row` and `column` are -1 when the buffer holds the invariant.
struct CapturedRowMismatch {
    int row = -1;
    int column = -1;
    int cellCount = 0; // how many cells differ in total
};

/// Checks a retained buffer against the full re-read it claims to equal.
///
/// ``refreshCapturedRows`` promises a buffer byte-identical to a full re-read;
/// reuse is meant to change the work and never the result. A buffer that
/// breaks that promise holds cells the screen no longer contains, and the
/// frontend draws them until something else marks their row — which reaches a
/// user as a character that is wrong, or missing, until it comes back. The
/// promise is checkable, so this checks it: the same refresh runs into the
/// caller's scratch with reuse denied, and the two buffers are compared.
///
/// What it cannot see is a renderer that draws nothing for a cell this buffer
/// holds correctly. The comparison is buffer against model, so a fault
/// downstream of the model satisfies it — a green here narrows the search to
/// the render path rather than clearing the frame.
///
/// Diagnostic, not a frame path — it costs a full re-read of the grid.
///
/// - Parameters:
///   - terminal: The live terminal, under the model lock.
///   - refresh: The refresh the buffer under test was built with.
///   - cells: The incrementally refreshed buffer to check.
///   - scratch: A buffer the same shape as `cells`, overwritten here.
///   - scratchLineAttributes: One byte per row, overwritten here.
///   - scratchRowMarks: One byte per row, overwritten here.
/// - Returns: The first differing cell and how many differ, or a row of -1
///   when the buffer matches.
[[nodiscard]] CapturedRowMismatch verifyCapturedRows(const Terminal& terminal,
    const CapturedRowRefresh& refresh, std::span<const Cell> cells, std::span<Cell> scratch,
    std::span<std::uint8_t> scratchLineAttributes, std::span<std::uint8_t> scratchRowMarks);

/// A scissor rectangle in target pixels, clamped to the target.
struct UBIQUITTY_ESCAPABLE RetainedContentRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    /// Whether the rectangle encloses any pixels.
    ///
    /// A degenerate rectangle is not an error — a target smaller than the grid
    /// origin produces one — but it must not be handed to a scissor, which
    /// rejects an empty rect.
    [[nodiscard]] bool isEmpty() const { return width <= 0 || height <= 0; }
};

/// The rectangle grid rendering is confined to when the target is retained.
///
/// A retained target keeps last frame's pixels with a Load, and only the dirty
/// rows are rebuilt — so nothing ever repaints the margins around the grid.
/// Glyph ink overhangs the grid (a combining stack above the first row, a
/// descender below the last, a left side bearing on column zero — every
/// texture-healed `w` or `m` at a row start in Monaspace), and ink left in a
/// margin no later frame repaints would stay for the life of the texture.
/// The rectangle therefore reaches `overhang` past the grid on every side,
/// which is as far as a glyph slot's margin lets ink go, and every rebuilt row
/// repaints its share of that strip before its glyphs land
/// (CellInstanceBuilder emits the strips into the box-fill stream when it
/// builds from a dirty-row mask). Confining the grid passes to this rectangle
/// is what keeps the rest of the margins equal to the clear that made them: the
/// load action runs before the scissor, so a full frame still clears the whole
/// target.
///
/// Every backend that retains a target owes the same confinement, and a
/// backend that derived the rectangle itself would differ in the last pixel —
/// which is a column of ink present in one arm and absent in the other, on the
/// one edge no other frame repaints.
///
/// - Parameters:
///   - origin: The grid's left and top edges in the target, in pixels.
///   - cell: One cell's extent, in pixels.
///   - grid: The grid's extent, in cells.
///   - target: The retained target's extent, in pixels.
///   - overhang: How far past the grid a glyph's ink may reach on each side,
///     in pixels — the glyph slot's margins.
/// - Returns: The rectangle, clamped to the target; empty when it does not fit.
///
/// Four pairs rather than eight scalars, and the grid's is the one that earns
/// its type twice over: written loose the parameters read `rows, columns` while
/// ``GridSize`` is `columns, rows`, so the two orders disagree and a caller that
/// gets it right does so by reading the names. Passing the type puts the order
/// in one place.
[[nodiscard]] inline RetainedContentRect retainedContentRect(FloatPoint origin, FloatSize cell,
    GridSize grid, IntSize target, IntSize overhang = {})
{
    RetainedContentRect rect;
    if (grid.rows <= 0 || grid.columns <= 0 || target.width <= 0 || target.height <= 0)
        return rect;
    const int reachX = overhang.width > 0 ? overhang.width : 0;
    const int reachY = overhang.height > 0 ? overhang.height : 0;
    const float left = origin.x - static_cast<float>(reachX);
    const float top = origin.y - static_cast<float>(reachY);
    rect.x = left > 0.0f ? static_cast<int>(left) : 0;
    rect.y = top > 0.0f ? static_cast<int>(top) : 0;
    if (rect.x > target.width)
        rect.x = target.width;
    if (rect.y > target.height)
        rect.y = target.height;
    // Ceil, not round: a fractional advance leaves the last column's right
    // edge inside the next pixel, and a rectangle that stopped short would
    // clip the very ink the grid owns. Measured from the grid's own edge, so a
    // clamp on the left takes nothing off the right.
    const float gridLeft = origin.x > 0.0f ? origin.x : 0.0f;
    const float gridTop = origin.y > 0.0f ? origin.y : 0.0f;
    rect.width = static_cast<int>(std::ceil(gridLeft + static_cast<double>(grid.columns) * cell.width)) + reachX - rect.x;
    rect.height = static_cast<int>(std::ceil(gridTop + static_cast<double>(grid.rows) * cell.height)) + reachY - rect.y;
    if (rect.x + rect.width > target.width)
        rect.width = target.width - rect.x;
    if (rect.y + rect.height > target.height)
        rect.height = target.height - rect.y;
    if (rect.width < 0)
        rect.width = 0;
    if (rect.height < 0)
        rect.height = 0;
    return rect;
}

} // namespace Ubiquitty::Rendering
