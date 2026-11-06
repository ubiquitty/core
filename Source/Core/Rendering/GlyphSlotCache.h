// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/LigatureParticipation.h"

#include "Core/Rendering/BoxStrokeProfile.h"

#include "Base/Geometry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "Base/SwiftInterop.h"

// GlyphSlotCache — portable policy behind a terminal glyph atlas: maps a glyph
// KEY (a code point folded with its style/phase, or a cluster hash) to a SLOT
// index in a fixed-capacity grid, faulting in new glyphs and evicting the
// least-recently-used slot when full. It owns no pixels, no font, no GPU object
// — only the bookkeeping (open-addressing key->slot hash + intrusive LRU list +
// slot geometry).
//
// The platform backend owns the storage and rasterisation and drives this cache:
// on slotForKey() returning fresh==true it rasterises the glyph into its storage
// at the slot. Same-config backends produce identical masks, so a process-wide
// shared cache lets every view reuse one rasterisation and one storage buffer.
// Pure C++23, no exceptions / RTTI, host-testable.

namespace Ubiquitty::Rendering {

// A rectangle in an atlas texture, in atlas pixels.
//
// Pixels because a rect names texels: three of the four consumers fetch by
// integer texel, so the value is the index they need and the CPU and GPU agree
// on which texel a rect names by construction. Metal's cell pass is the one
// that samples normalized, and it divides in its fragment stage where the
// atlas extent is uniform for the draw.
struct GlyphUV {
    float minU, minV, maxU, maxV;
};

// Smallest power of two >= 2*capacity — the open-addressing hash size to pass
// the GlyphSlotCache constructor so the load factor stays <= 0.5 and the bucket
// mask is a cheap and. capacity need not itself be a power of two (the grid may
// be clamped to a GPU texture limit).
inline int recommendedHashBuckets(int capacity)
{
    int hashBuckets = 1;
    while (hashBuckets < capacity * 2)
        hashBuckets <<= 1;
    return hashBuckets;
}

/// The most slots one ``GlyphSlotCache`` indexes.
///
/// Slot indices are 16 bits wide and all-ones is the LRU's "no neighbour", so a
/// sheet cut to more slots than this would hand out indices that alias.
inline constexpr int maxGlyphSheetSlots = 0xFFFF;

// The result of fitting a glyph-atlas grid to a GPU texture limit: the clamped
// grid + whether any grid is valid.
struct AtlasGridFit {
    int columns; // clamped slots per row
    int rows; // clamped rows
    bool valid; // false = a single slot exceeds the limit / degenerate metrics: refuse the atlas
};

/// Returns the bounded working-set grid for per-glyph proportional text.
///
/// The slot is a two-cell coverage canvas, so a 128 by 64 grid is roughly an
/// 8K-glyph repertoire across face, phase, and smoothing variants without
/// inheriting the much larger CJK-oriented wide-sheet budget. Frontends clamp
/// this grid to their texture limit but do not choose its eviction policy.
///
/// - Returns: The nominal grid before a device-limit clamp.
[[nodiscard]] constexpr GridSize proportionalGlyphAtlasNominalGrid()
{
    return { 128, 64 };
}

/// Fits a glyph-atlas grid so its texture stays within the GPU's texture limit.
///
/// The backing texture is `grid.columns * slot.width` by `grid.rows *
/// slot.height`, so a bounded slot count is not enough on its own: at a large
/// font the per-slot pixels alone push the texture past the limit, and creating
/// it aborts descriptor validation. Reshapes the grid before reducing its capacity.
///
/// The grid and the slot are separate types because they are four integers at
/// every call site and two of them count cells while two count pixels. Passed
/// loose they are interchangeable to the compiler, and transposing them yields a
/// texture the wrong shape rather than a diagnostic.
///
/// - Parameters:
///   - grid: The requested slots per row and rows, in slots.
///   - slot: One slot's extent, in backing pixels.
///   - maxTextureDimension: The GPU's maximum 2D texture dimension, in pixels.
/// - Returns: The clamped grid, and whether any grid is valid at all — false
///   when a single slot exceeds the limit or either extent is degenerate, in
///   which case the caller refuses the atlas.
[[nodiscard]] inline AtlasGridFit clampAtlasGridToTextureLimit(GridSize grid, IntSize slot, int maxTextureDimension)
{
    if (grid.columns <= 0 || grid.rows <= 0 || slot.width <= 0 || slot.height <= 0) {
        return { grid.columns, grid.rows, false };
    }
    if (slot.width > maxTextureDimension || slot.height > maxTextureDimension) {
        return { grid.columns, grid.rows, false };
    }
    const int maxColumns = maxTextureDimension / slot.width;
    const int maxRows = maxTextureDimension / slot.height;
    const std::int64_t requestedSlots = static_cast<std::int64_t>(grid.columns) * grid.rows;
    grid.columns = std::min(grid.columns, maxColumns);
    grid.rows = static_cast<int>(std::min<std::int64_t>(maxRows,
        (requestedSlots + grid.columns - 1) / grid.columns));
    grid.columns = static_cast<int>(std::min<std::int64_t>(maxColumns,
        (requestedSlots + grid.rows - 1) / grid.rows));
    return { grid.columns, grid.rows, true };
}

/// The pixel rectangle one slot occupies in a sheet.
///
/// Escapable: four ints with no borrowed lifetime, produced and consumed on both
/// sides of the seam.
struct UBIQUITTY_ESCAPABLE AtlasSlotRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// Where a slot sits in its sheet, in backing pixels.
///
/// Every backend needs this to upload a rasterized glyph, and every backend that
/// derives it privately derives the same two expressions. A backend that got the
/// row and column the wrong way round would place each glyph in a transposed
/// slot and sample the one Core's UV points at, so the sheet fills with the
/// right glyphs in the wrong places and every count stays correct.
///
/// Takes the grid rather than reading ``GlyphSlotGeometry`` because one geometry
/// describes several sheets: the mono slot, the two-cell wide slot and the 2x
/// double slot have different dimensions and the same arithmetic.
///
/// - Parameters:
///   - slot: The slot index the cache assigned.
///   - columns: Slots per sheet row.
///   - slotWidthInPixels: One slot's width.
///   - slotHeightInPixels: One slot's height.
/// - Returns: The slot's rectangle, or a zero rectangle for a degenerate grid.
[[nodiscard]] inline AtlasSlotRegion atlasSlotRegion(int slot, int columns, int slotWidthInPixels, int slotHeightInPixels)
{
    if (slot < 0 || columns <= 0 || slotWidthInPixels <= 0 || slotHeightInPixels <= 0)
        return {};
    return {
        (slot % columns) * slotWidthInPixels,
        (slot / columns) * slotHeightInPixels,
        slotWidthInPixels,
        slotHeightInPixels,
    };
}

// Physical slot geometry of a glyph atlas grid, in backing pixels. The slot is
// the glyph box plus a margin (overhang headroom for italics / powerline /
// oversized box glyphs) plus, on the subpixel path, 1px shift headroom folded
// into slotWidthInPixels. glyphWidthInPixels/glyphHeight are the cell box the non-subpixel UV
// maps; the full slot includes the headroom.
struct GlyphSlotGeometry {
    int columns; // slots per texture row
    int slotWidthInPixels; // physical slot (incl. subpixel headroom)
    int slotHeightInPixels;
    int glyphWidthInPixels; // cell box width inside the margins
    IntSize glyphMarginInPixels; // margin (atlas px) around the glyph box, per axis
    /// The font's true advance and line height, un-rounded, in atlas pixels.
    ///
    /// The slot box is a ceil of these, so a consumer that scales against the
    /// slot shrinks a glyph already at its natural size. Zero leaves a consumer
    /// to treat its cell as the atlas cell and apply no scale.
    float referenceAdvanceInPixels = 0.0f;
    float referenceLineHeightInPixels = 0.0f;

    /// The wide (two-cell) slot, in backing pixels. Zero when the backend keeps
    /// no separate wide sheet, which is what tells a consumer to leave a wide
    /// glyph on its cell rather than route it to a wide pass.
    ///
    /// A wide glyph scales against ``glyphWidthInPixels`` — the ceil'd cell box
    /// — and not against ``referenceAdvanceInPixels``, because the two-cell slot
    /// is laid out in whole cell boxes.
    int wideSlotWidthInPixels = 0;
    int wideSlotHeightInPixels = 0;

    /// The double (DECDWL/DECDHL) master slot, in backing pixels. Zero when the
    /// backend keeps no separate 2x sheet, which leaves a doubled glyph on its
    /// cell rather than routing it to the double pass.
    ///
    /// A 2x master is rasterised at twice the cell box in both axes, so its
    /// margins are doubled too — a quad backing off by one margin sits a margin
    /// low. The quad takes the full width and HALF the height: a DHL pair's two
    /// rows sample the two halves of one master.
    int doubleSlotWidthInPixels = 0;
    int doubleSlotHeightInPixels = 0;

    /// The ligature strip slot, in backing pixels. Zero when the backend keeps
    /// no strip sheet, which leaves rows unligatured rather than wrongly drawn.
    ///
    /// A strip holds one whole shaped run — the contract's cell cap of cell
    /// boxes, both margins, and the subpixel headroom pixel — so the width is
    /// derived by `ligatureStripSlotWidthInPixels` (CellDecisions.h) rather
    /// than restated per frontend. Height is the mono slot's: a strip is one
    /// row of ordinary glyphs drawn as a single image.
    int ligatureSlotWidthInPixels = 0;
    int ligatureSlotHeightInPixels = 0;
    /// The strip font's probe verdict, carried with the geometry so the
    /// builder's prescan and the sheet's row walk read one answer. The arm
    /// that built the font answers Core's ``ligatureHealingProbe`` and sets
    /// this from ``decideLigatureParticipation``; the default is the
    /// operator economy, which is the safe direction for a font nobody
    /// probed.
    LigatureParticipation ligatureParticipation = LigatureParticipation::OperatorRuns;
    /// The font's measured box strokes, from the arm's reference probe.
    ///
    /// Rides the geometry for the reason the participation verdict does: the
    /// arm owns the font, and the builder's procedural box fills must cut
    /// the strokes the font's own rules measure into — unmeasured bands keep
    /// the em rule, so an arm that probes nothing changes nothing.
    BoxDrawing::BoxStrokeProfile boxStrokeProfile {};

    [[nodiscard]] int rows(int capacity) const { return columns > 0 ? capacity / columns : 0; }

    // Slot -> UV of the GLYPH BOX inside the margins (legacy stretched cell
    // path: the quad is the cell, the box maps onto it).
    [[nodiscard]] GlyphUV atlasRectForSlot(int slot, int capacity) const;

    // Slot -> UV of the FULL physical slot incl. headroom (subpixel 1:1 blit).
    //
    // `cellsWide` spans that many adjacent slots horizontally, for a glyph the
    // grid stores across more than one — a wide CJK cell, or a ligature run.
    // A span takes no horizontal margin: the box is the slots it occupies.
    [[nodiscard]] GlyphUV atlasRectForSlotFull(int slot, int capacity, int cellsWide = 1) const;

    /// Slot -> the UV an atlas hands ``buildCellInstances``.
    ///
    /// The builder expands what it receives by the margin, so this is the full
    /// slot contracted by the margin. Not the glyph box: the slot is wider by
    /// `2*margin + 1`, the odd pixel being subpixel phase headroom.
    [[nodiscard]] GlyphUV atlasRectForBuilder(int slot, int capacity) const;
};

} // namespace Ubiquitty::Rendering

namespace Ubiquitty::Rendering {
class GlyphSlotCache;
}
/// Claims a reference to `cache` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else; a
/// namespace-scoped pair is reported as missing.
///
/// - Parameter cache: The cache to retain. A null pointer is ignored.
void retainGlyphSlotCache(Ubiquitty::Rendering::GlyphSlotCache*);

/// Drops a reference to `cache`, destroying it at zero.
///
/// - Parameter cache: The cache to release. A null pointer is ignored.
void releaseGlyphSlotCache(Ubiquitty::Rendering::GlyphSlotCache*);

namespace Ubiquitty::Rendering {

// Noncopyable: a heavy object (three hashBuckets-sized + three capacity-sized
// vectors) with a single owner. Noncopyable makes the Swift importer treat it as
// ~Copyable so a stray Swift copy can't deep-copy the storage; the trailing
// shared-reference annotation makes Swift hold it as a managed reference instead
// of by value, so a Swift owner needs no manual allocate/deallocate pair and the
// address it hands Core stays put for the frame. Both are no-ops on C++.
class UBIQUITTY_NONCOPYABLE GlyphSlotCache {
public:
    // A slot key: the 32-bit derivation from GlyphKey.h in the low word and the
    // raster variant's tier above it (glyphSlotKey). 64 bits
    // because the text derivation already spends bits 0-28 on code point and
    // style and the top three on keyspace tags, leaving no room for the tier
    // that CoreText's smoothing keys a glyph's pixels on.
    using Key = std::uint64_t;

    // Reserved key meaning "empty bucket / unused slot". A backend must never
    // hand a real glyph this key (codePoint<<bits | style never reaches it; the
    // tier field tops out at 4, so no variant sets the high word to all-ones).
    static constexpr Key emptyKey = ~Key { 0 };

    // capacity   — number of slots (the storage grid holds this many glyphs).
    // hashBuckets — open-addressing table size; pass >= 2*capacity, power of two
    //               recommended for a cheap mask, so the load factor stays <=0.5.
    // columns    — slots per storage row, so the backend can map slot ->
    //               (column,row) = (slot % columns, slot / columns).
    GlyphSlotCache(int capacity, int hashBuckets, int columns);

    /// Marks the start of a render pass.
    ///
    /// Slots touched after this call are live this frame and will not be
    /// LRU-evicted within the pass (see ``slotForKey``); the touch and
    /// refusal counters restart with it. Call once per pass before the
    /// per-cell lookups.
    void beginFrame()
    {
        ++m_frame;
        m_stampsThisFrame = 0;
        m_refusalsThisFrame = 0;
        m_saturationsThisFrame = 0;
        m_freshThisFrame.clear();
    }

    /// How many slots this frame has assigned to a new key so far: a mark
    /// ``forgetSlotsAssignedSince`` rolls back to.
    ///
    /// - Returns: The count of fresh assignments since ``beginFrame``.
    [[nodiscard]] int slotsAssignedThisFrame() const { return static_cast<int>(m_freshThisFrame.size()); }

    /// Forgets every key this frame assigned a slot to after `mark`, as
    /// ``forgetKeyAtSlot`` forgets one.
    ///
    /// For a caller that faults a group of glyphs it draws only whole: when one
    /// is refused, the slots the others took go back, so the frame's remaining
    /// glyphs find them rather than the group holding slots it will not draw.
    /// Only slots this frame assigned are touched, so no glyph another caller
    /// resolved this frame, or any earlier one, loses its slot.
    ///
    /// - Parameter mark: A count ``slotsAssignedThisFrame`` returned this frame.
    void forgetSlotsAssignedSince(int mark);

    /// Resolves `key` to a slot, allocating one on a miss.
    ///
    /// On a hit the slot is promoted in the LRU and reported not fresh. On a
    /// miss a slot is allocated — the LRU tail evicted once the cache is full
    /// — and reported fresh, which obliges the backend to rasterise into it.
    /// The cache refuses only when every slot was touched this frame, because
    /// evicting one would corrupt a glyph the current pass already placed; a
    /// frame referencing more distinct glyphs than `capacity` therefore drops
    /// the overflow rather than drawing the wrong pixels.
    ///
    /// - Parameters:
    ///   - key: The slot key (GlyphKey.h glyphSlotKey); a 32-bit derivation
    ///     widens to the tier-0 key.
    ///   - fresh: Set to whether the slot was allocated by this call.
    /// - Returns: The slot, or -1 when the cache refuses.
    int slotForKey(Key key, bool* fresh);

    /// Resolves a CONTENT-HASHED key, whose low word is a lossy fold of `contents`.
    ///
    /// Two distinct runs can collide to one key and both be live in one frame:
    /// a ligature strip window split two ways carries the same code points, so
    /// the fold cannot separate them and only the trailing split words in
    /// `contents` can. One key therefore owns as many slots as it has distinct
    /// verified contents. A hit is a bucket whose key and contents both match;
    /// a key hit whose contents mismatch keeps probing rather than re-owning
    /// the slot, because re-owning would leave the other collider's recorded
    /// instances sampling this collider's pixels for the rest of the frame. No
    /// match anywhere in the chain allocates a fresh slot, and a collider that
    /// stops recurring ages out through the LRU like any other glyph.
    ///
    /// - Parameters:
    ///   - key: The slot key whose low word folds `contents`.
    ///   - contents: The run the key was folded from.
    ///   - count: How many words `contents` holds.
    ///   - fresh: Set to whether the slot was allocated by this call.
    /// - Returns: The slot, or -1 when the cache refuses.
    int slotForKeyVerified(Key key, const std::uint32_t* contents, int count, bool* fresh);

    /// Drops a key whose fill failed, so the next lookup misses and tries again.
    ///
    /// A slot is reported fresh exactly once — on the frame it is assigned. A
    /// caller whose raster then fails has taken a slot it never wrote, and every
    /// later frame is told that key is resident and is handed the slot back. The
    /// cell draws the evicted glyph's pixels under its own colours, with every
    /// count in the frame correct, which is the one failure a parity or count
    /// check cannot see. Forgetting the key costs a retried raster per frame
    /// instead, and a fill that keeps failing keeps drawing the defined blank.
    ///
    /// The slot goes to the LRU tail and loses its frame stamp, so it is the
    /// next one reused rather than ageing to the tail behind every live glyph.
    /// A forgotten slot holds pixels nobody can name; leaving it in place spends
    /// atlas capacity on them until the whole cache turns over.
    ///
    /// - Parameter key: The key a fresh ``slotForKey`` handed back.
    void forgetKey(Key key);

    /// Forgets a failed raster's exact slot without removing a hash collider.
    ///
    /// A verified key may name several live payloads, so rollback must name
    /// the slot its allocation returned. A mismatched key or slot is ignored.
    ///
    /// - Parameters:
    ///   - key: The failed raster's cache key.
    ///   - slot: The fresh slot returned for that raster.
    void forgetKeyAtSlot(Key key, int slot);

    // Flush every slot (e.g. a font/size change rebuilt the storage). After
    // this, every slotForKey is a fresh miss again. Retained sets empty with
    // the slots they pinned; leases stay valid and re-fill on their views'
    // next frames.
    void reset();

    /// Opens a retained-reference lease for one view of this shared cache.
    ///
    /// The sheets behind this cache are shared across every view showing one
    /// font configuration, and a frame's instances sample them by UV on the
    /// GPU while the CPU is already building later frames. Eviction that
    /// ignores in-flight frames rewrites a slot's texels under a command
    /// buffer that still samples them, and the view shows another window's
    /// ink for the frames the overlap lasts.
    ///
    /// That is the whole population the pins protect. A retained row needs no
    /// pin: it lives on as PIXELS in the view's retained texture, which no
    /// slot rewrite can reach — only rows a frame actually rebuilds sample
    /// the sheets, and only until that frame's command buffer completes. So a
    /// lease holds a RING of the last ``retainEpochDepth`` frames' touches,
    /// each ``retainTouchedSlots`` call rotating the oldest epoch out. A slot
    /// untouched for that many frames has no in-flight consumer left and
    /// returns to the evictable pool of its own accord — which is what keeps
    /// one window's history from pinning the sheet solid against another
    /// window's redraws.
    ///
    /// When pinned slots do fill the cache, new runs are refused rather than
    /// served a pinned slot — the caller draws its fallback, softer but never
    /// wrong.
    ///
    /// - Returns: The lease, to hand back to the other two calls.
    [[nodiscard]] int acquireRetainLease();

    /// Drops a lease and every pin it holds.
    ///
    /// - Parameter lease: A lease from ``acquireRetainLease``; unknown values
    ///   are ignored.
    void releaseRetainLease(int lease);

    /// Rotates `lease`'s epoch ring and pins this frame's touched slots.
    ///
    /// The oldest epoch's pins are dropped first: by the time a frame is
    /// ``retainEpochDepth`` frames old its command buffer has completed, so
    /// nothing samples the slots it touched and holding them would only
    /// starve other views of the shared sheet. The set this call collects
    /// covers the rows the frame just built — a retained-grid view's dirty rows
    /// — which together with the ring is exactly the population an in-flight
    /// frame can still sample.
    ///
    /// A frame that stamped nothing still rotates: an idle view's footprint
    /// drains over ``retainEpochDepth`` quiet frames rather than standing
    /// until a correction. Refusals under pressure remain the degraded-never-
    /// wrong fallback, and ``refusalsThisFrame`` still schedules the full
    /// rebuild that re-baselines.
    ///
    /// Called once per frame after the build, on the thread the build ran
    /// on. Frame 0 (no ``beginFrame`` discipline) retains nothing, so a
    /// single-shot caller sees a plain LRU cache.
    ///
    /// - Parameter lease: The building view's lease.
    void retainTouchedSlots(int lease);

    /// Empties `lease`'s retained set so the NEXT ``retainTouchedSlots`` is
    /// exact.
    ///
    /// Called at the start of a frame that rebuilds every row — the view's
    /// touches then reference precisely its live content, and accumulation's
    /// slack from scrolled-away rows is dropped in one step.
    ///
    /// - Parameter lease: The lease to re-baseline.
    void rebaselineRetainedSlots(int lease);

    /// How many lookups this pass refused for want of an evictable slot.
    ///
    /// A refusal draws the caller's fallback — softer, never wrong — so a
    /// frame with refusals is correct but degraded, and a view that sees a
    /// nonzero count schedules a full rebuild to re-baseline its pins.
    [[nodiscard]] std::uint32_t refusalsThisFrame() const { return m_refusalsThisFrame; }

    /// How many lookups this pass found every candidate slot already taken by
    /// this same pass.
    ///
    /// The other refusal: a frame asking for more distinct keys than the cache
    /// holds blocks itself, pins or no pins, and re-baselining a retained set
    /// cannot unblock it. Counted apart from ``refusalsThisFrame`` because the
    /// remedy differs — this one says the key space is too large for the
    /// capacity, that one says a neighbouring view is holding too much.
    [[nodiscard]] std::uint32_t saturationsThisFrame() const { return m_saturationsThisFrame; }

    /// How many slots are pinned by some lease and cannot be evicted.
    ///
    /// A refusal is what a caller sees once the pins crowd out a frame, and
    /// by then the frame has already drawn its fallback. This is the same
    /// pressure one step earlier, so a view can re-baseline before a cell
    /// goes blank rather than after.
    [[nodiscard]] int pinnedSlots() const { return m_pinnedSlots; }

    /// How many slots a frame can still allocate without a refusal.
    [[nodiscard]] int evictableSlots() const { return m_capacity - m_pinnedSlots; }

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int capacity() const { return m_capacity; }
    [[nodiscard]] int used() const { return m_used; } // slots filled so far (debug/diag)

    /// One covered column's share of a strip slot's ink, in slot pixels.
    ///
    /// Two spans, because a strip is cut by one and decorated by the other.
    /// `leftInPixels`/`rightInPixels` are the strip pixels the column draws:
    /// its glyphs' ink grown by the fringe their producer reported, then
    /// settled so no two columns claim one pixel. `inkLeftInPixels`/
    /// `inkRightInPixels` are the ink alone — no fringe, no settling — which
    /// is where a rule that follows the letter must end.
    ///
    /// `rightInPixels` at or below `leftInPixels` is a column that owns no
    /// ink — the tail cells of a multi-cell ligature, whose whole image the
    /// lead column carries.
    struct OwnedInkExtent {
        float leftInPixels = 0.0f;
        float rightInPixels = 0.0f;
        float inkLeftInPixels = 0.0f;
        float inkRightInPixels = 0.0f;
    };

    /// Opens the owned-ink ledger for a strip cache.
    ///
    /// The ledger records, per slot, which covered column each shaped
    /// glyph's ink belongs to, so a consumer can cut a strip by ownership
    /// instead of by cell geometry. It rides the cache, beside the slots, for
    /// two reasons that hold on every arm: a cache hit needs the extents of a
    /// strip rasterised frames ago, and both sheets already share this class
    /// through the seam — a ledger per sheet would be the same policy
    /// spelled twice. Opt-in, so the mono and colour caches this class also
    /// backs pay nothing for it.
    ///
    /// - Parameters:
    ///   - columnsPerSlot: The most covered columns one slot's run spans.
    ///   - slotWidthInPixels: The slot's width, which clamps every record.
    void enableOwnedInkRecords(int columnsPerSlot, int slotWidthInPixels);

    /// Starts `slot`'s record over as measured and empty.
    ///
    /// Called when a rasterisation that can measure begins filling the slot;
    /// the per-glyph records follow through ``recordShapedGlyphInk``.
    ///
    /// - Parameter slot: The slot being rasterised.
    void beginOwnedInkRecord(int slot);

    /// Folds one shaped glyph's ink into its owning column's record.
    ///
    /// The fold is the policy half of ownership, kept in one place: the
    /// glyph's cluster names its column relative to the drawn run, context
    /// clusters fall outside `[0, drawnCells)` and are dropped — their ink
    /// belongs to a neighbouring chunk — and a column hit twice widens to
    /// the union, because a cluster may shape to several glyphs. Extents are
    /// clamped to the slot, so no record can send a sampler outside it.
    ///
    /// The fringe is the producer's statement of how far its extent may miss
    /// the coverage: zero for one that measured the raster, nonzero for one
    /// that derived the extent from an outline box and cannot vouch for the
    /// antialiased edge past it. The fringe widens the pixels the column
    /// draws and never the ink, so a rule that follows the ink ends where the
    /// letter does on every producer.
    ///
    /// - Parameters:
    ///   - slot: The slot being rasterised.
    ///   - cluster: The glyph's cluster — its source index in the shaped
    ///     text, context included.
    ///   - leadingContext: How many context entries precede the drawn run.
    ///   - drawnCells: How many columns the drawn run covers.
    ///   - inkLeftInPixels: The glyph's leftmost inked x, slot-relative.
    ///   - inkRightInPixels: One past the glyph's rightmost inked x.
    ///   - fringeInPixels: The pixels each side of the ink the column draws
    ///     as well, to cover coverage the extent may not hold.
    void recordShapedGlyphInk(int slot, int cluster, int leadingContext, int drawnCells,
        int inkLeftInPixels, int inkRightInPixels, int fringeInPixels);

    /// Settles `slot`'s record so no two columns claim one pixel.
    ///
    /// Called once when a rasterisation finishes recording. Neighbouring
    /// drawn spans can overlap — an overhanging glyph, or a fringe reaching
    /// past the ink — and a consumer drawing both quads over one shared
    /// strip would composite the overlap twice and the seam
    /// would read bolder than the stroke. The contested span is split at
    /// its midpoint — each side keeps the half nearest its own ink — so
    /// every strip pixel belongs to exactly one column. The ink spans are
    /// left as recorded: a letter's ink is its own whoever draws the pixels.
    ///
    /// - Parameter slot: The slot whose record is settled.
    void finalizeOwnedInkRecord(int slot);

    /// Marks `slot`'s record unmeasured, so consumers keep the geometric cut.
    ///
    /// The unmeasured state is distinct from measured-and-empty: a producer
    /// that could not measure must not present as one whose glyphs drew
    /// nothing.
    ///
    /// - Parameter slot: The slot whose record is dropped.
    void clearOwnedInkRecord(int slot);

    /// Whether `slot` carries a measured owned-ink record.
    ///
    /// - Parameter slot: The slot to ask about.
    /// - Returns: True when ``beginOwnedInkRecord`` opened the slot's record
    ///   and nothing has cleared it since.
    [[nodiscard]] bool slotHasOwnedInk(int slot) const;

    /// One covered column's recorded extent.
    ///
    /// - Parameters:
    ///   - slot: The slot the strip lives in.
    ///   - coveredColumn: The column's index within the run, from zero.
    /// - Returns: The column's extent; empty when it owns no ink, or when
    ///   the slot is unmeasured or out of range.
    [[nodiscard]] OwnedInkExtent ownedInkForSlot(int slot, int coveredColumn) const;

    /// The rows of a strip slot its raster put ink in, in slot pixels: the
    /// first inked row and one past the last.
    ///
    /// `bottomInPixels <= topInPixels` is a strip with no ink; `isMeasured`
    /// false is a slot no raster has reported.
    struct InkRows {
        bool isMeasured = false;
        float topInPixels = 0.0f;
        float bottomInPixels = 0.0f;
    };

    /// One slot's R8 coverage plane, named for the Swift seam.
    using InkPlaneSpan = std::span<const std::uint8_t>;

    /// Records which rows of `slot` hold ink, measured from the raster the
    /// producer just drew into it.
    ///
    /// Cleared with the owned-ink record (``clearOwnedInkRecord``), so a
    /// producer records the rows after settling that record.
    ///
    /// Sized with the owned-ink ledger, by ``enableOwnedInkRecords``; a cache
    /// without the ledger ignores the call. Independent of the ledger's own
    /// measured state, because every producer has the strip's coverage in
    /// hand whether or not its rasteriser reports per-glyph extents.
    ///
    /// - Parameters:
    ///   - slot: The slot just rasterised.
    ///   - coverage: The slot's R8 plane, `widthInPixels` by `heightInPixels`.
    ///   - widthInPixels: The plane's width.
    ///   - heightInPixels: The plane's height.
    void recordInkRows(int slot, InkPlaneSpan coverage, int widthInPixels, int heightInPixels);

    /// The ink rows ``recordInkRows`` measured for `slot`.
    ///
    /// - Parameter slot: The slot to ask about.
    /// - Returns: The rows; unmeasured when no raster reported the slot.
    [[nodiscard]] InkRows inkRowsForSlot(int slot) const;

private:
    /// Finds the bucket holding `key`.
    ///
    /// - Parameter key: The key to find.
    /// - Returns: The bucket index, or -1 when the key is absent.
    [[nodiscard]] int hashFind(Key key) const;
    /// Inserts `key` mapping to `slot` at the first free bucket in its chain.
    ///
    /// - Parameters:
    ///   - key: The key to insert.
    ///   - slot: The slot it maps to.
    void hashInsert(Key key, std::uint16_t slot);
    /// Removes the bucket mapping `key` to `slot` specifically.
    ///
    /// Verified keys are not unique in the table — one fold can own a slot per
    /// live collider — so an eviction that removed by key alone could unhash a
    /// LIVE sibling and leave the evicted slot reachable, serving its new
    /// owner's pixels under the old key.
    ///
    /// - Parameters:
    ///   - key: The key of the bucket to remove.
    ///   - slot: The slot that bucket must map to; buckets carrying the same
    ///     key but a different slot are left in place.
    void hashRemoveEntry(Key key, std::uint16_t slot);
    /// Empties one bucket and repairs the probe chain behind it.
    ///
    /// Backward-shift deletion: entries past the gap whose ideal bucket lies at
    /// or before it are pulled back in, so a lookup for a displaced key still
    /// terminates at an empty bucket rather than walking past one.
    ///
    /// - Parameter gap: The bucket index to empty.
    void hashRemoveAt(std::uint32_t gap);
    /// The miss path shared by both lookups: takes a free or reclaimed slot,
    /// else evicts the LRU tail. Never consults the hash for `key`, so a
    /// verified caller that already scanned the chain cannot be handed a
    /// same-key slot whose contents it just rejected.
    ///
    /// - Parameters:
    ///   - key: The key the taken slot is hashed under.
    ///   - fresh: Set true when a slot was taken (the caller rasterises into
    ///     it), false when the cache refused.
    /// - Returns: The slot, or -1 when every slot is live this frame.
    [[nodiscard]] int allocateSlot(Key key, bool* fresh);
    /// Unlinks `slot` from the recency list, fixing both ends.
    ///
    /// The half both moves share. Written once because a splice duplicated is a
    /// splice where one copy forgets an end: ``lruTouch`` can skip the head fix
    /// only because it returns early when the slot is already the head, and that
    /// is a property of its caller rather than of the unlink.
    ///
    /// - Parameter slot: The slot to unlink. Its own links are left stale, for
    ///   the caller to overwrite as it relinks.
    void lruDetach(std::uint16_t slot);
    void lruTouch(std::uint16_t slot); // move slot to MRU head
    /// Moves `slot` to the LRU tail, so it is the next one reused.
    ///
    /// The mirror of ``lruTouch``. Reached when a slot's key is forgotten: it
    /// then holds pixels nothing can name, and the tail is where the allocator
    /// looks first.
    ///
    /// - Parameter slot: The slot to demote.
    void lruEvictToTail(std::uint16_t slot);

    [[nodiscard]] std::uint32_t hashOf(Key key) const
    { // Knuth multiplicative over the low word, the high word (the tier, a
      // handful of values) folded in first so the five tiers of one glyph do
      // not chain in one bucket run. The product's high half is folded onto
      // the low bits the mask keeps: alone, those depend only on the key's
      // low bits, which for a mono key are its style and phase lanes and the
      // code point's bottom three bits, so a frame of printable ASCII probed
      // 6.4 buckets per lookup instead of 1.03.
        const std::uint32_t folded = static_cast<std::uint32_t>(key) ^ (static_cast<std::uint32_t>(key >> 32) * 0x9E3779B1u);
        const std::uint32_t product = folded * 2654435761u;
        return (product ^ (product >> 16)) & m_hashMask;
    }

    int m_capacity;
    int m_columns;
    std::uint32_t m_hashMask; // hashBuckets - 1 (hashBuckets is power of two)

    std::vector<Key> m_hashKey; // hashBuckets; emptyKey == free
    std::vector<std::uint16_t> m_hashSlot; // hashBuckets
    std::vector<Key> m_keyForSlot; // capacity; emptyKey when unfilled

    std::vector<std::uint16_t> m_lruPrev; // capacity; 0xFFFF == none
    std::vector<std::uint16_t> m_lruNext; // capacity
    std::uint16_t m_lruHead;
    std::uint16_t m_lruTail;
    // How many slots an eviction samples before falling back to the LRU tail.
    // Enough that a frame's own placements rarely fill every draw, few enough
    // that the search stays constant.
    static constexpr int evictionSampleCount = 8;
    // Seeded, so a run reproduces its own eviction sequence and its own layout.
    mutable std::uint32_t m_evictionSeed = 0x9E3779B9u;
    int m_used; // slots filled so far (<= m_capacity)

    // Frame epoch: m_slotFrame[slot] is the frame counter at the slot's last
    // touch; a slot stamped with the current m_frame is "live this pass" and is
    // not evicted (eviction would recycle a glyph the pass already placed).
    std::uint32_t m_frame;
    std::vector<std::uint32_t> m_slotFrame; // capacity
    // The slots this frame assigned to a new key, in order; reserved to the
    // capacity so a frame appends without allocating.
    std::vector<std::uint16_t> m_freshThisFrame;

    // Retained-reference pins: m_pinCount[slot] counts the leases whose last
    // frame touched the slot; eviction skips any slot above zero. Written only
    // at frame boundaries (retainTouchedSlots) and lease teardown, so the
    // per-cell paths never touch it.
    std::vector<std::uint16_t> m_pinCount; // capacity
    int m_pinnedSlots = 0; // slots with m_pinCount > 0, maintained on the 0<->1 edges

    // How many frames of touches a lease pins at once. The bound is the
    // pipeline's: a triple-buffered swapchain holds at most three frames'
    // command buffers in flight, and the fourth epoch is the frame being
    // built. A slot older than the ring has no consumer left on the GPU.
    static constexpr int retainEpochDepth = 4;
    // A lease is stale once the frames since its last retain span more than
    // this many ring-depths of DISPLAY time. The frame counter advances once
    // per drawing view per display frame, so the display frames elapsed are
    // the ticks divided by the active lease count — a flat tick threshold
    // would mean a different wall time for every window count, and the
    // starved window pays the difference: an idle pane's pins protect slots
    // no command buffer can sample after retainEpochDepth display frames,
    // yet a flat bound held them for seconds while an active pane beside it
    // drew its text unhealed. Factor two over the ring keeps the reclaim
    // clear of anything genuinely in flight; reclaim runs only where the
    // alternative is refusing the lookup outright.
    static constexpr std::uint32_t staleLeaseRingSlack = 2;
    /// One lease's pins, bucketed by the frame that took them.
    struct RetainRing {
        std::array<std::vector<std::uint16_t>, retainEpochDepth> epochs;
        int head = 0; // the epoch holding the newest frame's touches
        std::uint32_t lastRetainFrame = 0;
    };
    std::vector<RetainRing> m_retainRings;
    std::vector<int> m_freeLeases;

    /// Unpins one epoch's slots and empties it.
    ///
    /// - Parameter epoch: The epoch bucket whose pins are dropped.
    void dropEpochPins(std::vector<std::uint16_t>& epoch);

    /// Drops every pin belonging to leases that stopped retaining long ago.
    ///
    /// Runs only where the alternative is a refusal: a lease whose owner has
    /// not drawn for ``staleLeaseRingSlack`` ring-depths of display frames has
    /// no command buffer in flight, so its pins protect nothing, and holding
    /// them would let one idle window starve every busy one sharing the sheet.
    ///
    /// - Returns: Whether any pins were dropped, so the caller retries once.
    [[nodiscard]] bool reclaimStaleLeasePins();
    // First-touches and refusals this pass. An int each on paths that already
    // branch, so the hit path pays nothing for them; retention early-outs on a
    // frame that stamped nothing, which is every steady frame of an idle
    // view.
    std::uint32_t m_stampsThisFrame = 0;
    std::uint32_t m_refusalsThisFrame = 0;
    std::uint32_t m_saturationsThisFrame = 0;
    // The owned-ink ledger, empty unless enableOwnedInkRecords sized it.
    // uint16 pairs: a strip slot is tens of cells wide, nowhere near the
    // range's edge, and the ledger is capacity × columnsPerSlot entries.
    std::vector<std::uint16_t> m_ownedInkLeft; // capacity * columnsPerSlot
    std::vector<std::uint16_t> m_ownedInkRight;
    std::vector<std::uint16_t> m_inkLeft; // the unfringed, unsettled ink
    std::vector<std::uint16_t> m_inkRight;
    std::vector<std::uint8_t> m_ownedInkMeasured; // capacity
    // Per slot, the guarded rows the strip's ink spans; top > bottom is a
    // slot never measured.
    std::vector<std::int16_t> m_inkRowTop; // capacity
    std::vector<std::int16_t> m_inkRowBottom;
    int m_ownedInkColumnsPerSlot = 0;
    int m_ownedInkSlotWidthInPixels = 0;
    /// Stamps a slot as touched this pass.
    ///
    /// - Parameter slot: The slot the caller just resolved or filled.
    void touchThisFrame(std::uint16_t slot)
    {
        m_slotFrame[slot] = m_frame;
        ++m_stampsThisFrame;
    }

    // Verified contents per slot (slotForKeyVerified): the exact run the slot's
    // pixels were rasterised from, memcmp'd on every hash hit so a colliding
    // fold can never serve the wrong cluster. Empty for unique-key slots.
    std::vector<std::vector<std::uint32_t>> m_contentsForSlot; // capacity

public:
    /// References held by a language that manages this object by reference.
    ///
    /// Only a heap instance from ``makeGlyphSlotCache`` participates. A cache
    /// embedded by value in a C++ owner — the software renderer and both Vulkan
    /// atlases hold one that way — never has its count touched, because nothing
    /// hands such an instance to a language that would.
    std::atomic<int> swiftReferences { 1 };
} UBIQUITTY_SHARED_REFERENCE(retainGlyphSlotCache, releaseGlyphSlotCache);

} // namespace Ubiquitty::Rendering

// Retain and release for the Swift importer, at global scope because the
// importer requires these to be global functions — a namespace-scoped pair
// reports as "cannot find retain function", which reads like a missing
// declaration rather than a scope rule.
//
// The hazard this carries: a foreign reference type tells Swift every instance
// it sees is heap-allocated and refcounted. Three C++ owners embed a cache as a
// member rather than a pointer, and a pointer to one of those reaching Swift
// would be released as though it were heap memory. Nothing does that today —
// Swift only ever holds a cache it made through `makeGlyphSlotCache`, and it
// produces the atlas bindings
// rather than consuming ones a C++ backend filled. A Swift consumer of a
// C++-filled MonoAtlasBinding would be the change that makes this live.
/// Claims a reference to `cache` on Swift's behalf.
///
/// - Parameter cache: The cache to retain. A null pointer is ignored.
inline void retainGlyphSlotCache(Ubiquitty::Rendering::GlyphSlotCache* cache)
{
    if (cache)
        cache->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops a reference to `cache`, destroying it at zero.
///
/// - Parameter cache: The cache to release. A null pointer is ignored.
inline void releaseGlyphSlotCache(Ubiquitty::Rendering::GlyphSlotCache* cache)
{
    if (cache && cache->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete cache;
}

namespace Ubiquitty::Rendering {

/// Allocates a slot cache a reference-managing language can own.
///
/// The counterpart to the constructor for callers that hold this by reference:
/// C++ owners embed one by value and need no factory, while Swift imports the
/// class as a reference type and has no way to place one itself.
///
/// - Parameters:
///   - capacity: Slots the grid holds.
///   - hashBuckets: Open-addressing table size; see ``recommendedHashBuckets``.
///   - columns: Slots per storage row.
/// - Returns: A cache with one reference held.
[[nodiscard]] inline GlyphSlotCache* makeGlyphSlotCache(int capacity, int hashBuckets, int columns)
    UBIQUITTY_RETURNS_RETAINED
{
    return new GlyphSlotCache(capacity, hashBuckets, columns);
}

} // namespace Ubiquitty::Rendering
