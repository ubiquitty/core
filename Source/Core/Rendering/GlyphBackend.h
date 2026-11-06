// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/Rendering/FontMetrics.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include <algorithm>
#include <cstdint>
#include <span>

// The seam a frontend implements to supply glyphs — what PAL is for the OS,
// this is for drawing. It lives at the rendering layer rather than in PAL
// because everything crossing it is a rendering type; PAL is L0 and names
// nothing above it, so putting this there would invert the layers.
//
// Function pointers and a context, not an abstract class, for one reason that
// is not style: Swift cannot inherit a C++ pure-virtual interface, so an
// abstract class needs an adapter per frontend whose whole job is to forward
// the virtuals. A table any language can fill needs no adapter.
//
// Easy and fast are not in tension here, and the reason is worth stating
// because it is what licences the design. Nothing per-cell crosses this seam:
//
//     bindMonoAtlas      once per frame — returns the cache and geometry, and
//                        the per-cell walk reads them directly afterwards
//     beginFrame         once per frame
//     shapeLigatureRuns  once per DIRTY ROW
//     rasteriseMonoGlyph once per glyph MISS
//     wide/cluster/colour/box   once per cell of that kind, which are the rare
//                        kinds; the common path never reaches them
//
// So an indirect call here costs nothing measurable, while the same indirection
// per cell would not survive. A backend that finds itself wanting a hot hook
// should assume it has misread the seam rather than that the seam is missing
// one.
//
// A NULL entry is Core's default, which is what makes a partial backend
// legitimate rather than broken: fill the required ones, leave the rest null,
// and the frontend draws. `optionalDefaultsAreLossy` below is the exception a
// caller has to answer for, and `isDrawable` is the one the builder enforces.
//
// Every hook that fills a slot returns whether it filled it, and the reason is
// the cache rather than tidiness. `slotForKey` reports a slot fresh exactly
// once, so a backend that takes a slot and fails to fill it is not merely blank
// for a frame: every later frame is told the key is cached, the raster is never
// retried, and the slot may by then hold another glyph's ink — with every count
// in the frame correct. A false is Core's cue to forget the key, so the next
// frame rasterises again instead of blitting what nobody wrote.
//
// What that buys is narrower than it reads: forgetting the key makes a transient
// failure transient. The frame that failed still draws background, and only the
// next one recovers — so a backend returning true is claiming it wrote the slot,
// not that the cell inks. Nothing here can check the claim; the seam can only
// make it a claim somebody typed, which a `void` return could not.
//
// The question is whether this slot's contents are the caller's, rather than
// whether the font had a glyph, and the two come apart in the case that looks
// most like a failure. A backend whose rasteriser declines a code point and then
// blanks the slot has written it: the contents are defined, every later frame is
// right to serve them, and re-rasterising would ask the same font the same
// question for the same answer. `SoftwareRenderer::rasteriseColorGlyph` clears
// on a decline for that reason — the slot otherwise holds the evicted tenant's
// pixels under this key's name.
//
// So `false` is for a slot left UNTOUCHED — an argument guard, a missing sheet,
// an early return before any write. Answering false for a declined glyph that was
// blanked costs a re-rasterisation per frame forever and buys nothing.
//
// One entry defaults to a function instead of to null, and the difference is
// worth the inconsistency. A null entry obliges its reader to test before
// calling, and an entry whose safe answer is "do nothing" turns that test into a
// branch the builder can forget and a hole it can dereference. Where the safe
// answer is expressible as a RETURN VALUE, the default is a named Core function
// returning it — the omission then arrives on the same path a real backend's
// failure does, and there is no second path for it to arrive on. Naming that
// function is also what makes leaving it alone a decision rather than an
// oversight.

namespace Ubiquitty::Rendering {

// The named aliases Swift receives spans through: the importer hands a NAMED
// span alias the element accessors and the buffer-pointer initialiser, where
// the direct spelling imports only as an opaque generic. Every span parameter
// in the table below is spelled through one of these, so the Swift side of the
// seam speaks the type Core passes rather than converting at the thunk.
using CoveragePlaneSpan = std::span<const std::uint8_t>;
using ClusterSpan = std::span<const std::uint32_t>;
using LigatureRowSpan = std::span<const LigatureCell>;
using LigatureRunStream = std::span<LigatureRun>;

/// Measures the ink box of a rasterised coverage plane.
///
/// The box is the smallest rectangle holding every nonzero texel, optionally
/// grown by one guard texel on each side and clamped to the plane: a linear tap at a quad
/// edge reads half a texel outward, so a box flush against the ink would
/// feather the outermost coverage against whatever lies beyond it. The guard
/// keeps every tap the fragment shader can make inside texels the rasteriser
/// wrote.
///
/// - Parameters:
///   - coverage: The R8 plane, row-major, `widthInPixels` by `heightInPixels`.
///   - widthInPixels: The plane's width.
///   - heightInPixels: The plane's height.
///   - guardInPixels: The extra sampling border; zero measures exact ink.
/// - Returns: The guarded box; a zero-size box at the origin for a plane with
///   no ink, which the builder reads as "emit no quad"; the unmeasured marker
///   for a plane smaller than its stated dimensions, which no correct caller
///   passes and a wrong one must not have trusted.
[[nodiscard]] inline GlyphInkBox measureInkBox(CoveragePlaneSpan coverage, int widthInPixels, int heightInPixels, int guardInPixels = 1)
{
    GlyphInkBox box;
    if (widthInPixels <= 0 || heightInPixels <= 0
        || coverage.size() < static_cast<std::size_t>(widthInPixels) * static_cast<std::size_t>(heightInPixels))
        return box;
    int minX = widthInPixels;
    int minY = heightInPixels;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < heightInPixels; ++y) {
        const std::uint8_t* row = coverage.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(widthInPixels);
        for (int x = 0; x < widthInPixels; ++x) {
            if (row[x] != 0) {
                if (x < minX)
                    minX = x;
                if (y < minY)
                    minY = y;
                if (x > maxX)
                    maxX = x;
                maxY = y;
            }
        }
    }
    if (maxX < 0) {
        box.minInPixels = { 0.0f, 0.0f };
        box.maxInPixels = { 0.0f, 0.0f };
        return box;
    }
    const int guard = std::clamp(guardInPixels, 0, 1);
    box.minInPixels = { static_cast<float>(std::max(0, minX - guard)), static_cast<float>(std::max(0, minY - guard)) };
    box.maxInPixels = { static_cast<float>(std::min(widthInPixels, maxX + 1 + guard)),
        static_cast<float>(std::min(heightInPixels, maxY + 1 + guard)) };
    return box;
}

/// Records a proportional glyph's exact ink beside its cached raster.
///
/// Core uses this extent to keep overlapping glyphs in one strip, where their
/// coverage is combined before foreground compositing. An opaque-background
/// glyph pass would otherwise erase an earlier glyph's antialiased edge.
inline void recordProportionalGlyphInk(GlyphSlotCache& cache, int slot,
    CoveragePlaneSpan coverage, int widthInPixels, int heightInPixels)
{
    const GlyphInkBox ink = measureInkBox(coverage, widthInPixels, heightInPixels, 0);
    if (ink.maxInPixels.x < ink.minInPixels.x) {
        cache.clearOwnedInkRecord(slot);
        return;
    }
    cache.beginOwnedInkRecord(slot);
    cache.recordShapedGlyphInk(slot, 0, 0, 1,
        static_cast<int>(ink.minInPixels.x), static_cast<int>(ink.maxInPixels.x), 0);
}

/// The default ``GlyphBackend::stageMonoCoverage``: stages nothing, and says so.
///
/// Reported rather than asserted, because a backend with no way to stage a
/// Core-rasterised plane is not broken — it is one that cannot draw the few
/// glyphs Core rasterises, and every one of them has a defined blank.
///
/// - Parameters:
///   - context: The backend's own object, unread.
///   - slot: The slot the cache assigned, unread.
///   - coverage: The plane to stage, unread.
///   - count: The plane's byte count, unread.
/// - Returns: `false`, always — the slot was not written.
inline bool declineMonoCoverage(void* /*context*/, int /*slot*/, CoveragePlaneSpan /*coverage*/) { return false; }

/// A ``GlyphBackend::boxGlyph`` that answers absent, so Core draws every box.
///
/// The entry and this function produce the same picture; what differs is what a
/// reader can conclude. A null says nothing — the author may have judged the
/// font's boxes worse than the decomposition, or may never have met the entry.
/// This says the backend met it and chose, which is the distinction
/// ``optionalDefaultsAreLossy`` reports on and the one a checker cannot recover
/// from prose.
///
/// The choice is a claim about a FONT rather than about the entry: Core's own
/// renderer takes this route because Unifont's box characters do not tile, and a
/// backend whose font draws them well should fill the entry instead.
///
/// - Parameters:
///   - context: The backend's own object, unread.
///   - codePoint: The box character, unread.
/// - Returns: An absent glyph, always — Core then draws the box itself.
inline AtlasGlyph declineBoxGlyph(void* /*context*/, std::uint32_t /*codePoint*/, FontFace) { return {}; }

/// A ``GlyphBackend::shapeLigatureRuns`` that shapes nothing, and says so.
///
/// Every run stays unligatured, which is correct text rather than degraded text
/// — so unlike the entries ``optionalDefaultsAreLossy`` names, this costs the
/// reader nothing. It exists for the same reason ``declineBoxGlyph`` does: a
/// backend with no shaper has made a decision, and a null cannot be told from an
/// author who never reached the entry.
///
/// - Parameters:
///   - context: The backend's own object, unread.
///   - row: The row's cells, unread.
///   - runs: The output runs, every length reset — declining is answering
///     "no runs", and the span may arrive holding a previous row's.
inline void declineLigatureShaping(void* /*context*/, LigatureRowSpan /*row*/, float /*rowOriginXInPixels*/, float /*cellWidthInPixels*/, GlyphPass, LigatureRunStream runs)
{
    for (LigatureRun& run : runs)
        run.length = 0;
}

/// Everything a frontend supplies so the cell builder can draw.
///
/// Every member takes `context` first, which is the frontend's own object.
/// A member left null falls to Core's behaviour for it; the four marked
/// REQUIRED have no meaningful default and a backend without them cannot draw.
/// Borrowed for the frame: `context` points at whatever filled the table, and
/// every entry is a function pointer into it. Tagged so a Swift implementer
/// cannot outlive its own context.
struct UBIQUITTY_NONESCAPABLE GlyphBackend {
    /// The frontend object every entry point receives. Core never dereferences it.
    void* context = nullptr;

    // ---- REQUIRED ----

    /// Starts a frame; slots touched after this are not evicted within it.
    void (*beginFrame)(void* context) = nullptr;

    /// Returns the atlas rect for one code point, rasterising it on a miss.
    ///
    /// `variant` is the raster variant (GlyphKey.h makeRasterVariant): the
    /// subpixel phase in its low bits, and for a backend whose rasteriser
    /// weights a glyph by its fill colour, the smoothing tier above. A backend
    /// that keys its own cache probes it with
    /// `glyphSlotKey(monoGlyphKey(...), variant)`; one that positions
    /// continuously and smooths nothing receives tier 0 and may read
    /// `variantPhase(variant)` alone. Same for every `variant` below.
    AtlasGlyph (*monoGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t weight, std::uint32_t variant) = nullptr;

    /// Returns the mono atlas's slot cache and layout for this frame.
    ///
    /// Read once per frame; the per-cell walk then uses the returned POD
    /// directly, which is why no per-cell call crosses this seam.
    MonoAtlasBinding (*bindMonoAtlas)(void* context) = nullptr;

    /// Rasterises a code point into a slot the cache has already assigned.
    ///
    /// Required rather than optional, because the builder runs the mono hit path
    /// itself: on a probe MISS it has already taken the slot, and the cache will
    /// report that slot resident and not fresh to every later cell asking for the
    /// same glyph. A backend that cannot fill it leaves whatever the atlas last
    /// cleared the slot to, and the cell draws blank — which is what a space
    /// looks like, in a frame where every count is right.
    bool (*rasteriseMonoGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t variant, int slot) = nullptr;

    // ---- OPTIONAL: null falls back to Core ----

    /// Returns the atlas rect for a double-width glyph.
    ///
    /// The only route to a wide cell's glyph for a backend that binds no wide
    /// sheet, and the wide arm is an `if`/`else if` with no third branch: a
    /// backend offering neither leaves the cell background-only. That is the
    /// whole character gone rather than a mark or a colour, so it is the largest
    /// loss any entry here can cause and `optionalDefaultsAreLossy` names it.
    AtlasGlyph (*wideGlyph)(void* context, std::uint32_t codePoint, FontFace) = nullptr;

    /// Returns the 2x master for a DECDWL/DECDHL row.
    ///
    /// The backend applies `DoubleRegion` itself and returns the finished rect.
    ///
    /// Null reports absent and the builder halves the 1x glyph's V range
    /// instead, so double rows never blank — softer than a real master, which
    /// is why a backend that has one should say so. The variant carries the
    /// foreground's raster tier and no phase.
    AtlasGlyph (*doubleGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t variant, DoubleRegion) = nullptr;

    /// Returns the atlas rect for a whole grapheme cluster.
    ///
    /// Null renders the base code point ALONE — `e` where the text said `é`.
    /// See `optionalDefaultsAreLossy`.
    AtlasGlyph (*clusterGlyph)(void* context, ClusterSpan codePoints, FontFace, std::uint32_t weight, std::uint32_t variant) = nullptr;

    /// Binds the two-cell sheet for one frame, so the walk can probe it directly.
    ///
    /// Null, or a null cache, leaves wide glyphs to ``wideGlyph``.
    WideAtlasBinding (*bindWideAtlas)(void* context) = nullptr;

    /// Rasterises a two-cell glyph into a slot the cache has already assigned.
    ///
    /// Reached only on a probe MISS, so this is per glyph per style. The
    /// variant carries the foreground's raster tier, which the slot's key
    /// carries too; a two-cell glyph has no subpixel phase.
    bool (*rasteriseWideGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t variant, int slot) = nullptr;

    /// Returns the pen grid one two-cell glyph is drawn on, on a phased frame.
    ///
    /// A backend that grows two-cell glyphs draws each at a device size of its
    /// own — the cap's growth, less for a glyph whose ink already fills the
    /// slot, none for one that would overflow it — and a rasteriser that
    /// quantises the pen does so in buckets chosen from that size. The builder
    /// keys and places the glyph in the buckets it names, so the raster asked
    /// for at a pen is the one the rasteriser draws there. Asked per phased
    /// two-cell cell; a backend answers from a cache of its own.
    ///
    /// Null, or zero, keys the glyph at the frame's wide count
    /// (`BuildInputs::widePhaseCount`), which is right for every glyph that
    /// grows by the full cap from the primary face's size.
    int (*wideGlyphPhaseCount)(void* context, std::uint32_t codePoint, FontFace) = nullptr;

    /// Returns the atlas rect for a whole cluster on a two-cell base.
    ///
    /// Separate from ``clusterGlyph`` because the sheet is part of the answer
    /// rather than an implementation detail of it: a cell-baked wide glyph's
    /// rect is a wide-page coordinate, and a wide slot is two cells across, so
    /// slot N covers different texels on each page. A cluster taken from the
    /// mono page and handed to a wide cell names a slot nothing wrote.
    ///
    /// Null drops the marks — the wide base code point alone — which keeps the
    /// cell inked and its coordinate on the page its width names. Reported by
    /// ``optionalDefaultsAreLossy``. The variant carries the foreground's
    /// raster tier, as ``rasteriseWideGlyph``'s does.
    AtlasGlyph (*wideClusterGlyph)(void* context, ClusterSpan codePoints, FontFace, std::uint32_t weight, std::uint32_t variant) = nullptr;

    /// Binds the colour sheet for one frame, so the walk can probe it directly.
    ///
    /// Null, or a null cache, leaves colour glyphs to ``colorGlyph``.
    ColorAtlasBinding (*bindColorAtlas)(void* context) = nullptr;

    /// Rasterises a colour glyph into a slot the cache has already assigned.
    ///
    /// Reached only on a probe MISS, so this is per emoji rather than per cell.
    /// `count == 1` is a lone code point and more is a cluster; a backend may
    /// raster the two by different routes, and the count is how it tells.
    bool (*rasteriseColorGlyph)(void* context, ClusterSpan codePoints, int slot) = nullptr;

    /// Returns the colour-emoji master for a cluster.
    ///
    /// Null reports absent and the cluster falls to the mono path, which draws
    /// the emoji as an outline rather than in colour.
    AtlasGlyph (*colorGlyph)(void* context, ClusterSpan codePoints, std::uint16_t clusterID) = nullptr;

    /// Returns a box-drawing glyph taken from the font rather than drawn.
    ///
    /// The entry is two things, because ``BoxDrawing::prefersProcedural`` splits
    /// its range. An area fill, diagonal, or analytic Powerline separator is
    /// Core's to draw whatever the backend offers, and the entry is never
    /// consulted for one. A box-drawing LINE is font-first: the builder asks
    /// here and falls back only on a refusal, so a null overrides that policy
    /// and drops the font's stroke weight and dashes.
    ///
    /// So a null is free for half the range and lossy for the other half, and
    /// ``optionalDefaultsAreLossy`` counts it — unless the backend takes
    /// ``declineBoxGlyph``, which draws the same picture and records that the
    /// trade was weighed rather than missed.
    AtlasGlyph (*boxGlyph)(void* context, std::uint32_t codePoint, FontFace) = nullptr;

    /// Rasterises a combining cluster into a slot the cache has already assigned.
    ///
    /// The cluster hit path is portable and runs in Core, for the same reason the
    /// mono one does: ``coverageClusterKey`` is a Core derivation, the slots live
    /// in the cache ``bindMonoAtlas`` already handed over, and the UV comes from
    /// its geometry. Nothing in that sequence needs the backend, so nothing in it
    /// crosses — a screen whose clusters are resident crosses here zero times.
    ///
    /// Reached only on a probe MISS, so this is per cluster per style rather than
    /// per cell. Null means Core cannot fill a slot it took, so it declines the
    /// slot instead and the cluster falls to ``clusterGlyph``.
    bool (*rasteriseClusterGlyph)(void* context, ClusterSpan codePoints, FontFace, std::uint32_t variant, int slot) = nullptr;

    /// Stages a coverage plane Core rasterised into a slot the cache assigned.
    ///
    /// For the glyphs Core draws itself rather than asks the font for. A DECDLD
    /// soft-font cell is today's only one: its matrix arrives off the wire, so
    /// there is no code point to shape and nothing for the backend to look up —
    /// only pixels to move. ``rasteriseMonoGlyph`` is the mirror of this and
    /// cannot serve it, because it names a glyph the backend must find.
    ///
    /// Not named for soft fonts, though they are its only caller today: what
    /// crosses here is a finished coverage plane, and a second Core-rasterised
    /// glyph would need this and not a second copy of it. Keeping the DRCS
    /// vocabulary out of the signature is also what keeps ``VT::SoftFont`` on
    /// Core's side of the seam — see ``GlyphBackendTypes.h``, which owes its
    /// callers a contract naming no model type.
    ///
    /// `coverage` is R8, row-major with a top-left origin, laid out for the slot
    /// box ``bindMonoAtlas`` reported. The backend needs no dimensions to copy
    /// it: they are its own. `count` is `slotWidthInPixels * slotHeightInPixels`
    /// so a backend can REFUSE a plane that is not the shape it expects instead
    /// of reading past one.
    ///
    /// Reached once per glyph MISS, so this is per downloaded glyph rather than
    /// per cell.
    ///
    /// Returning false leaves the cell background-only, which is where an
    /// undefined DRCS position already lands. That is why the default declines
    /// rather than being null: a backend that never implements this draws the
    /// blank DEC defines, by the path a real staging failure takes, and the
    /// builder tests a bool it was going to test anyway.
    bool (*stageMonoCoverage)(void* context, int slot, CoveragePlaneSpan coverage) = &declineMonoCoverage;

    /// Returns one slot-shaped window of a glyph rasterised at an OSC 66 scale.
    ///
    /// Without them a sized cell stretches a cell-sized master: the band crop slices
    /// its 1/scale V slice and the quad magnifies it back, so a scale-3 glyph
    /// is a small bitmap drawn large. The crop site reserves an own-sheet
    /// master as "a sharpness win that must reproduce these same bounds", and
    /// these three entries are that reservation called in. A backend that
    /// implements one rasterises the glyph at `tile.scale` times its size and
    /// returns the rect for the slot-shaped window `(tile.band, tile.tile)` of
    /// it — on the sheet the stream already samples, in slots of the ordinary
    /// size, so no instance stream, pass or texture changes hands. The streams
    /// then draw one ordinary quad per window in place of the one stretched
    /// quad, and the destination geometry is identical to the crop's by
    /// construction.
    ///
    /// Null, or an absent answer, falls back to the crop: the same picture,
    /// soft. That is a fair trade, not a loss — every cell is still inked with
    /// the right glyph — so `optionalDefaultsAreLossy` does not count these.
    /// `SoftwareRenderer` and `VulkanGlyphAtlas` answer all three from Core's
    /// sized masters (SizedMaster.h) — the mono entries also for a DECDWL or
    /// DECDHL cell on the cell-baked path, as a tile doubled across or two rows
    /// tall — and decline the mono entries under a rasterizer whose
    /// `rasterisesScaledGlyphsSharply` is false: a bitmap face such as Unifont
    /// rasterised at 3x is the magnified crop by another route, so the crop draws
    /// it without spending a slot per window.
    ///
    /// Like ``doubleGlyph`` — the same idea at scale 2 — these are direct
    /// calls, per sized cell per frame, with the backend keying its own cache
    /// (`sizedTileKey` / `sizedColorTileKey`, memcmp-verified). Sized cells
    /// are a per-run embellishment, not the steady state, which is the same
    /// argument that puts DECDHL rows on a per-cell call.
    ///
    /// Windows step by the destination the slot kind fills: a cell for this
    /// entry, two cells for the wide one, the colour square for the colour
    /// one.
    ///
    /// The pass picks the RECT the answer maps, not the raster: one slot
    /// serves both, but a ``GlyphPass``' `Subpixel` quad is the full slot and
    /// wants the margin-contracted rect it re-expands, while a `CellBaked`
    /// quad is the cell and wants the glyph box — the same two answers the
    /// builder's `monoSlotUV` gives for an unsized glyph, for the same reason.
    AtlasGlyph (*sizedGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t weight, std::uint32_t phase, GlyphPass, SizedTile) = nullptr;

    /// ``sizedGlyph`` for a two-cell glyph, windows two cells wide. The
    /// variant carries the foreground's raster tier and no phase.
    AtlasGlyph (*sizedWideGlyph)(void* context, std::uint32_t codePoint, FontFace, std::uint32_t variant, SizedTile) = nullptr;

    /// ``sizedGlyph`` for a colour cluster, windows the colour square wide.
    ///
    /// The window grid is the SOURCE square's, not the cell row's: the colour
    /// quad is a centred square, so a window can straddle a row boundary when
    /// the square and the cell height differ, and the builder clips each
    /// window against the band's row exactly as it clips the soft square.
    AtlasGlyph (*sizedColorGlyph)(void* context, ClusterSpan codePoints, SizedTile) = nullptr;

    /// Rasterises the glyph the shaper chose into a mono slot the cache has
    /// already assigned.
    ///
    /// The entry a bounded atlas needs: every other rasterise entry here is
    /// keyed by CODE POINT and re-derives the glyph through the shaper, so a
    /// healed letterform can only be stored under the text that produced it —
    /// and text fragments do not repeat, where the glyphs a face draws
    /// saturate near its repertoire. Core keys the slot with
    /// ``shapedGlyphKey`` and runs the hit path itself, as it does for a code
    /// point, so the slot's entry in ``MonoAtlasBinding::inkBoxForSlot`` is the
    /// ink the builder reads for the glyph. A backend that measures ink owes
    /// that entry for this raster the way it does for ``rasteriseMonoGlyph``.
    ///
    /// A glyph id is meaningless outside the face that produced it, so the
    /// producer of ``LigatureRun::shaped`` must report no shaped glyph rather
    /// than an id from a fallback family: drawn here it would be whatever that
    /// id names in the primary face, which is a different letter and not a
    /// missing one.
    ///
    /// Reached only for a substitution — a cell whose answer covers one cell —
    /// and only on a probe miss. False declines the slot, and the caller keeps
    /// the strip it already has, so a backend fills this when it can and loses
    /// nothing when it cannot. Null is the same answer for a backend that has
    /// no by-identity route at all.
    bool (*rasteriseShapedGlyph)(void* context, std::uint32_t glyphID, FontFace, std::uint32_t variant, int slot) = nullptr;

    /// The strip a proportional run draws (Docs/proportional-text.md): the
    /// run's text shaped in the backend's proportional face at its natural
    /// advances, aligned by `horizontalAlign` inside `widthCells` cells and
    /// clipped to them.
    ///
    /// Core's strip layout selects `tileIndex` from the complete shaped run.
    /// Each tile rasterises with guards and returns its disjoint visible
    /// interior through ``ProportionalStripPlacement``. The pen's whole pixel
    /// and `penResidualInPixels` stay consistent across tiles, preserving
    /// crossing glyphs and fractional placement. Absent when the sheet
    /// refuses; the run then draws its background alone, which reads as a gap
    /// where a stretched first glyph would read as an error. Null leaves the
    /// run to the grid-face fallback the builder draws itself.
    ///
    /// `size` is the run's size as the per-glyph route's sized windows take
    /// it: `size.scale` rows for a DECDHL pair (2) or an OSC 66 scale, and
    /// `size.widthScale` 2 for a run on a DECDWL row. A backend that can draws
    /// the run at that size — `scale` times its size, `widthScale` times again
    /// across, over as many times the cells, on a canvas `scale` slots tall
    /// whose baseline is the slot's taken `scale` times — and answers the
    /// slot height `size.band` of it, saying so in
    /// ``ProportionalStripPlacement/drawnWidthScale`` and
    /// ``ProportionalStripPlacement/drawnScale``, with `topInPixels` measured
    /// from the block's top row: `scale` times the strip's own top, and
    /// `band` slot heights down. One that cannot draws the run at its own
    /// size, and the builder stretches the quad.
    ProportionalStripPlacement (*proportionalStrip)(void* context, ClusterSpan codePoints, int widthCells, std::uint8_t horizontalAlign, float penResidualInPixels, FontFace, std::uint32_t weight, std::uint32_t variant, int tileIndex, SizedTile size) = nullptr;

    /// Shapes a proportional run in the backend's proportional face
    /// (Docs/proportional-text.md), one ``ProportionalShapedGlyph`` per glyph.
    ///
    /// The half of the per-glyph route that only the backend can answer: which
    /// glyphs its face draws for this text and how far each advances. Writes
    /// up to `capacity` glyphs into `out` and returns how many; zero refuses
    /// the run, which then draws as a strip where the backend offers one, or
    /// in the grid face. Null for a backend with no proportional face.
    ///
    /// Core places what comes back — alignment, justification, the phase each
    /// glyph is drawn at, the cell each glyph's ink begins in — in
    /// Rendering/ProportionalRun.h, so the two answers together are the whole
    /// of what a frontend supplies, and the model's measurement and the
    /// renderer's quads are one arithmetic.
    int (*shapeProportionalRun)(void* context, ClusterSpan codePoints, FontFace, std::uint32_t weight, ProportionalShapedGlyph* out, int capacity) = nullptr;

    /// Binds the dedicated proportional sheet's portable cache and geometry.
    ///
    /// Core derives keys, performs eviction, and maps assigned slots to UVs;
    /// the frontend only supplies storage. Null keeps the strip or grid-face
    /// fallback, even when shaping is available.
    ProportionalAtlasBinding (*bindProportionalAtlas)(void* context) = nullptr;

    /// Rasterises one proportional-face glyph into a slot Core assigned.
    ///
    /// Called only for a fresh cache slot. False makes Core forget the key, so
    /// a failed platform raster never leaves an unwritten slot resident.
    /// A successful raster also records its ink through
    /// ``recordProportionalGlyphInk`` in the binding's one-column ledger.
    bool (*rasteriseProportionalGlyph)(void* context, std::uint32_t glyphID, FontFace, std::uint32_t weight, std::uint32_t variant, int slot) = nullptr;

    /// Rasterises one window of a proportional glyph drawn larger than
    /// ``rasteriseProportionalGlyph`` draws it into a slot Core assigned: for a
    /// run on a DECDWL row, on a DECDHL pair, or at an OSC 66 scale.
    ///
    /// The raster is the glyph at `tile.scale` times its size both ways and
    /// `tile.widthScale` times again across, on a canvas `tile.scale` slots
    /// tall whose baseline is the slot's taken `tile.scale` times, the way a
    /// sized block's master is drawn (``selectSizedMaster``). Its pen stands
    /// `tile.scale * tile.widthScale` margins in, at the variant's phase. The
    /// window is the slot-wide cut `tile.tile` slots along, from the slot
    /// height `tile.band` slots down. A DECDWL row asks for scale 1 and width
    /// scale 2, which a backend may draw from the face widened on its `wdth`
    /// axis, scaled across by what the axis left; a DECDHL pair asks for scale 2 and
    /// its half as the band; a scaled run asks for its scale and its row of
    /// the block. Called only for a fresh slot; false makes Core forget the key
    /// and draw the run's ordinary glyphs stretched instead. Null does the same
    /// for every sized run.
    bool (*rasteriseSizedProportionalGlyph)(void* context, std::uint32_t glyphID, FontFace, std::uint32_t weight, std::uint32_t variant, SizedTile tile, int slot) = nullptr;

    /// Shapes a row's ligature runs.
    ///
    /// The strip's pixels are on the GRID: every cluster anchors at its own
    /// cell — column K at K times `cellWidthInPixels`, the shaper's offsets
    /// doing only the relative work inside a cluster — and the builder slices
    /// each covered cell 1:1 against that pitch. A backend that leaves its
    /// shaper's pen in charge puts a neighbour's edge into cell K wherever
    /// the face's advance and the cell disagree: fractionally under a rounded
    /// advance, by the whole tracking under `font.character_spacing`. A
    /// future proportional run relaxes exactly this clause — natural
    /// advances, with the cells it covered reported back — which is why the
    /// anchoring is this contract's to name and not a property of strips.
    ///
    /// Called once per row that can hold a run, before the column walk, so a
    /// backend shapes a row in one pass rather than answering per cell. See
    /// ``LigatureCell`` for why the row arrives distilled rather than as cells or
    /// as code points.
    ///
    /// `runs` holds one entry per column and the backend owns the whole span,
    /// not only the columns its runs cover: the caller reuses one span across
    /// rows and frames, so an uncovered column may arrive holding a previous
    /// row's run and must have its `length` reset. Carried as spans rather
    /// than as pointers beside a count: the output had no bound in the signature
    /// at all, and a backend writing one entry past the row would have corrupted
    /// whatever the caller put after it with nothing to say it had.
    ///
    /// One entry per column means every column the run covers, not only the
    /// column it starts at. The consumer never scans for an enclosing run: ``CellInstanceBuilder``
    /// asks `runs[column].length > 0` for each column in turn and takes that
    /// cell's slice as `column - startColumn`. Publishing only at the start
    /// column therefore does not render the run shorter — it renders one cell of
    /// it, and leaves the rest of the columns drawing their own unshaped glyphs
    /// beside it. A Vulkan backend landed exactly that and an eleven-cell run
    /// covered one cell.
    ///
    /// Null means no ligature shaping: text renders unligatured rather than
    /// wrongly. ``SoftwareRenderer`` installs ``declineLigatureShaping``, which
    /// is a decision and not an unfinished edge: it is the reference arm the GPU
    /// backends are compared against, so a GPU that ligates against a CPU that
    /// cannot is a real disagreement and the comparison oracles report it as
    /// one. Giving a backend shaping means answering for that arm too, not only
    /// for the backend.
    /// Collects immutable shaping requests without rasterizing or changing the row.
    /// Paint variants are unset; preparation may depend only on text, faces, and pitch.
    void (*prepareLigatureRow)(void* context, LigatureRowSpan row, float cellWidthInPixels) = nullptr;
    /// Completes all collected requests before any row consumes their results.
    void (*finishLigaturePreparation)(void* context) = nullptr;

    void (*shapeLigatureRuns)(void* context, LigatureRowSpan row, float rowOriginXInPixels, float cellWidthInPixels, GlyphPass, LigatureRunStream runs) = nullptr;

    /// The primary face's pitch: ``FontMetrics::facePitch`` over the advances
    /// the backend sampled when it cut its cell. A fact about the face rather
    /// than a hook, and Core's verdict on it: `Proportional` makes the builder
    /// lay every row out as proportional runs (Rendering/LetterLayout.h) when
    /// ``shapePrimaryRun`` is offered, and leaves the grid alone otherwise.
    FontMetrics::FacePitch primaryFacePitch = FontMetrics::FacePitch::Fixed;

    /// Shapes a run of same-pen cells in the primary face at its natural
    /// advances, one ``ProportionalShapedGlyph`` per glyph.
    ///
    /// The letter layout's one question to the backend: how far each letter of
    /// this text advances in the face the grid draws, kerned as the face kerns
    /// it, with the shaper's fraction kept. Substitutions are the face's
    /// default ones without discretionary ligatures: the builder draws each
    /// letter from the mono sheet at the pen this answer puts it on, so a
    /// glyph the shaper fused would still draw as its letters, and the
    /// clusters map ink back to cells. Writes up to `capacity` glyphs and
    /// returns how many; zero refuses the run, which then keeps the grid's
    /// pitch. Null for a backend with no shaper, whose proportional face stays
    /// on the grid.
    int (*shapePrimaryRun)(void* context, ClusterSpan codePoints, FontFace, std::uint32_t weight, ProportionalShapedGlyph* out, int capacity) = nullptr;
};

/// Whether this backend can put a glyph in a two-cell cell at all.
///
/// Two ways reach one outcome and a backend needs either: bind the wide sheet
/// and rasterise into it, or answer ``GlyphBackend::wideGlyph``. The builder
/// tries the cache first, so a backend that binds one never consults the entry
/// — which is why "is `wideGlyph` null" is the wrong question and this is the
/// right one. A backend with neither draws every wide cell as bare background.
///
/// How far this goes, because it is short of where a reader would take it: the
/// cache route is two POINTERS, and the cache itself arrives when the frame
/// calls ``GlyphBackend::bindWideAtlas``. A backend holding both pointers can
/// hand back an empty binding, and then this answers `true` while every wide
/// cell still draws bare background. `aBoundButEmptyWideBindingPassesTheRouteTestAndStillDrawsNothing`
/// holds that gap open, and
/// `Scripts/check-glyph-backend-claims.py` records the same one for colour.
///
/// - Parameter backend: The backend to inspect.
/// - Returns: `true` when the table offers some route to a two-cell glyph.
[[nodiscard]] inline bool hasWideGlyphRoute(const GlyphBackend& backend)
{
    return backend.wideGlyph != nullptr
        || (backend.bindWideAtlas != nullptr && backend.rasteriseWideGlyph != nullptr);
}

/// Whether this backend's null entries change what the user sees.
///
/// Some defaults are a fair trade — an unligatured row is still correct text.
/// These are not. Without `clusterGlyph` a combining mark is dropped, without
/// `wideClusterGlyph` the same happens on a two-cell base, and without
/// `colorGlyph` an emoji loses its colour. Each leaves the cell inked and the
/// count right, so every check downstream passes and only the picture is wrong.
///
/// The wide loss is the largest and the least like the others: a wide cell with
/// no route to a glyph draws BLANK, so the whole character goes rather than a
/// mark or a hue. It is also the one that cannot be read off a single entry.
/// The builder tries the wide CACHE first and falls to `wideGlyph` only when
/// that route is absent, so a backend that binds a cache never consults the
/// entry and its null costs nothing — which is the arrangement the Swift arm
/// ships. Asking about `wideGlyph` alone reports a loss that backend does not
/// have.
///
/// `boxGlyph` is the one that is lossy for only part of its range — free for
/// area fills, which Core draws whatever a backend offers, and lossy for
/// box-drawing lines, where Core's policy is font-first and a null overrides it.
/// It is counted here because a cell in that range does lose what it carried,
/// and a backend that has weighed the trade says so by taking
/// ``declineBoxGlyph`` rather than by leaving a null nobody can read.
///
/// That difference — a null nobody decided against a named refusal somebody
/// chose — is the whole of what this reports beyond the entry list. A frontend
/// calls it and decides; Core cannot decide for it, and silently degrading is
/// what makes the omission cost a bug report instead of a build error.
///
/// - Parameter backend: The backend to inspect.
/// - Returns: `true` when an unconsidered null drops information the cell carried.
inline bool optionalDefaultsAreLossy(const GlyphBackend& backend)
{
    return backend.clusterGlyph == nullptr || backend.wideClusterGlyph == nullptr
        || backend.colorGlyph == nullptr || backend.boxGlyph == nullptr
        || !hasWideGlyphRoute(backend);
}

/// Whether this backend can draw at all.
///
/// ``buildCellInstances`` calls this once per frame and writes nothing when it
/// is false — no cell, decoration, overlay or glyph instance, and every count
/// zero. A predicate nobody is obliged to call is a note describing a condition
/// rather than a check for it, and a table cannot make a missing entry a
/// compile error: the omission is a hole to dereference per glyph. A blank
/// frame is the loudest symptom available.
///
/// - Parameter backend: The backend to inspect.
/// - Returns: `true` when every REQUIRED entry is filled.
inline bool isDrawable(const GlyphBackend& backend)
{
    return backend.beginFrame && backend.monoGlyph && backend.bindMonoAtlas && backend.rasteriseMonoGlyph;
}

} // namespace Ubiquitty::Rendering
