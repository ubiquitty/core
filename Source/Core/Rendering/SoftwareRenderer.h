// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"
#include "Core/Image/ImagePlacement.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/CellInstanceScratch.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/LetterLayout.h"
#include "Core/Rendering/LigatureSheet.h"
#include "Core/Rendering/ProportionalGlyphSheet.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// The layer a frame's rich text blocks are drawn in: over the grid and its
/// images, under the cursor tier and the overlays, where the Metal arm
/// encodes its block layer (Docs/rich-text.md, "Rendering").
///
/// A function record rather than a type this renderer knows, so the raster
/// stays independent of the faces a caller sets blocks in. A null function
/// draws no blocks, which leaves the covered cells in their pen's background.
struct BlockLayer {
    /// The frame's pixels, RGBA8 with row stride `4 * surface.width`; named
    /// because Swift's interop has no spelling for a span's extent.
    using Pixels = std::span<std::uint8_t>;
    using Function = void (*)(void* context, Pixels rgba, IntSize surface);
    Function function = nullptr;
    void* context = nullptr;
};

// SoftwareRenderer — the portable, GPU-free renderer backend. It owns CPU R8
// glyph atlases (a narrow/mono and a wide one) backed by GlyphSlotCache,
// supplies GlyphBackend by rasterizing GNU Unifont into slots, and composites
// the SHARED buildCellInstances() output into an RGBA8 image with the same
// mix(background, foreground, coverage) the GPU fragment shader uses — so it
// renders the exact same instance stream the Vulkan/Metal backends draw. Pure
// C++, no platform / GPU types.
/// The portable, GPU-free renderer backend.
///
/// Owns CPU R8 glyph atlases behind ``GlyphSlotCache``, supplies
/// ``GlyphBackend`` from GNU Unifont, and composites the shared
/// ``buildCellInstances`` output with the same mix the GPU fragment shader uses
/// — so it renders the exact instance stream the Vulkan and Metal backends draw.
///
/// Swift borrows the immovable renderer from its C++ owner. That owner outlives
/// every backend callback and every borrowed atlas view.
class SoftwareRenderer {
public:
    // Defaults to the owned UnifontRasterizer (the byte-for-byte reference
    // raster), with the given fixed cell — the 8×16 Unifont path the agent
    // screenshot + the host-tests use.
    SoftwareRenderer(int cellWidthInPixels, int cellHeightInPixels);
    // Same, but faulting through an injected rasterizer at the given fixed
    // cell. The caller owns the rasterizer and must outlive the renderer.
    SoftwareRenderer(int cellWidthInPixels, int cellHeightInPixels, GlyphRasterizer&);
    // Font-derived cell: take the cell dims (and the builder CellMetrics) from
    // the injected rasterizer's cellMetrics(), so a real-font face (e.g. a
    // Linux FreeTypeRasterizer) drives the whole geometry — the atlas slot
    // size and the decoration metrics — instead of a hardcoded cell. The
    // caller owns the rasterizer and must outlive the renderer.
    explicit SoftwareRenderer(GlyphRasterizer&);

    // The cell geometry this renderer composites at (columns*cellWidth ×
    // rows*cellHeight). Font-derived when constructed from a rasterizer; the
    // caller (the window / resize math) reads these to size the grid.
    [[nodiscard]] int cellWidth() const { return m_cellWidth; }
    [[nodiscard]] int cellHeight() const { return m_cellHeight; }

    /// Builds and composites a frame into tightly packed RGBA8 pixels.
    ///
    /// - Parameters:
    ///   - in: The captured frame inputs, including its viewport and grid origin.
    ///   - rgbaOut: The destination pixels, with row stride `4 * widthOut`.
    ///   - widthOut: The rendered viewport width.
    ///   - heightOut: The rendered viewport height.
    ///   - paddingColor: The margin color. It is painted after the grid, image
    ///     and cursor layers, which clips them to the grid box, and before the
    ///     overlays, which alone may reach into the margin.
    ///   - blocks: The frame's rich text blocks, drawn after the grid and its
    ///     images and before the cursor tier, the margin and the overlays.
    /// - Returns: Whether the frame dimensions are valid and rendering succeeds.
    bool render(const BuildInputs& in, std::vector<std::uint8_t>& rgbaOut, int& widthOut, int& heightOut,
        std::array<std::uint8_t, 4> paddingColor = {}, BlockLayer blocks = {});

    /// Hands a build the letter rows this renderer keeps, for a frame drawn
    /// by another stream from the same inputs.
    ///
    /// ``render`` lends them to its own build; a frontend that builds the
    /// instances itself and draws them on a GPU calls this before
    /// ``buildCellInstances`` so ``columnAt`` and ``xInCellsAt`` answer for
    /// the frame it drew rather than for no frame at all.
    ///
    /// - Parameters:
    ///   - in: The inputs the build reads.
    ///   - buffers: The build's output buffers, whose letter fields are set.
    void lendLetters(const BuildInputs& in, InstanceBuffers& buffers);

    /// The column under a pixel along a row of the last rendered frame.
    ///
    /// The letter the pixel falls on where the primary face laid the row out
    /// as proportional runs (LetterLayout.h), else the column at the pitch:
    /// a frontend's pointer reads this rather than dividing by the cell.
    ///
    /// - Parameters:
    ///   - row: The visible row.
    ///   - xInPixels: The pixel, from the grid's left edge.
    /// - Returns: The column, clamped to the frame.
    [[nodiscard]] int columnAt(int row, double xInPixels) const;

    /// Where a pixel is along a row of the last rendered frame, in cells:
    /// the column under it plus the fraction of its letter crossed, which is
    /// what ``Input::PointerFact::xInCells`` carries.
    ///
    /// - Parameters:
    ///   - row: The visible row.
    ///   - xInPixels: The pixel, from the grid's left edge.
    /// - Returns: The position, 0 through the column count.
    [[nodiscard]] double xInCellsAt(int row, double xInPixels) const;

    /// A read-only view of one of the coverage atlases: R8 bytes, tightly
    /// packed, row stride `width`.
    ///
    /// The atlas is what a GPU backend uploads as a texture. It already exists
    /// here — faulted through the shared GlyphSlotCache, rasterized by whichever
    /// GlyphRasterizer was injected — so a backend that drew the instances on a
    /// GPU would otherwise have built a second atlas from the same rasterizer
    /// and had to keep the two slot policies agreeing.
    ///
    /// `pixels` is null while nothing has faulted yet. The view is invalidated
    /// by the next glyph fault, so a backend uploads within the frame.
    struct UBIQUITTY_NONESCAPABLE AtlasView {
        const std::uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
    };

    /// Bumped whenever a glyph is rasterized into a slot, so a GPU backend can
    /// skip re-uploading a texture whose pixels did not move. Once a screen
    /// settles, most frames fault nothing and the upload is pure waste.
    /// How many times the mono sheet's pixels have changed.
    ///
    /// Per sheet, because an uploader gating all three on one counter re-sends
    /// all three whenever any one of them faults -- and the colour sheet is
    /// four bytes per pixel, so a mono glyph faulting re-sends megabytes of
    /// emoji that did not change. Which sheet moved is a fact the rasteriser
    /// has and the uploader cannot recover.
    [[nodiscard]] std::uint32_t monoAtlasGeneration() const { return m_monoAtlasGeneration; }

    /// How many times the wide sheet's pixels have changed.
    [[nodiscard]] std::uint32_t wideAtlasGeneration() const { return m_wideAtlasGeneration; }

    /// How many times the dedicated proportional sheet's pixels have changed.
    [[nodiscard]] std::uint32_t proportionalAtlasGeneration() const { return m_proportional.generation(); }

    /// How many times the colour sheet's pixels have changed.
    [[nodiscard]] std::uint32_t colorAtlasGeneration() const { return m_colorAtlasGeneration; }

    /// The narrow/mono atlas — every single-width glyph.
    [[nodiscard]] AtlasView monoAtlas() const UBIQUITTY_LIFETIMEBOUND;

    /// The double-width atlas. Kept separate because a wide glyph's slot is a
    /// different shape, not because its coverage differs.
    [[nodiscard]] AtlasView wideAtlas() const UBIQUITTY_LIFETIMEBOUND;

    /// The dedicated proportional-glyph atlas.
    [[nodiscard]] AtlasView proportionalAtlas() const UBIQUITTY_LIFETIMEBOUND;

    /// The bounded proportional-run fallback atlas.
    [[nodiscard]] AtlasView proportionalStripAtlas() const UBIQUITTY_LIFETIMEBOUND;
    [[nodiscard]] bool proportionalStripAtlasIsDirty() const { return m_proportionalStrip.isDirty(); }
    /// Marks the current proportional-strip mirror as uploaded.
    void didUploadProportionalStripAtlas() { m_proportionalStrip.clearDirty(); }

    /// Returns the run-composited coverage used for ligatures and overlapping proportional glyphs.
    [[nodiscard]] AtlasView ligatureAtlas() const UBIQUITTY_LIFETIMEBOUND;
    [[nodiscard]] bool ligatureAtlasIsDirty() const { return m_ligature.isDirty(); }
    /// Marks the current strip mirror as uploaded.
    void didUploadLigatureAtlas() { m_ligature.clearDirty(); }

    /// The RGBA colour sheet — emoji, drawn in their own colours.
    ///
    /// Four bytes per pixel where the other two are one, so a consumer indexes it
    /// by `bytesPerPixel` rather than assuming the stride the others have.
    ///
    /// Empty when the injected rasterizer has no colour source, which is the same
    /// condition that leaves ``GlyphBackend::colorGlyph`` null.
    ///
    /// - Returns: The sheet's pixels and extent, or an empty view with no colour
    ///   source.
    [[nodiscard]] AtlasView colorAtlas() const UBIQUITTY_LIFETIMEBOUND;

    /// Returns the colour master for a cluster, faulting it in on a miss.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster.
    ///   - count: How many code points; 1 is a lone one.
    ///   - clusterID: Distinguishes two clusters that share a first code point.
    /// - Returns: The atlas rect, or an absent glyph with no colour source.
    AtlasGlyph colorGlyph(const std::uint32_t* codePoints, int count, std::uint16_t clusterID);

    /// Binds the colour sheet for one frame, so the walk can probe it directly.
    ///
    /// The builder prefers this to ``colorGlyph``: a resident emoji then costs a
    /// cache probe in Core rather than a call across the seam.
    ///
    /// - Returns: The cache and the slot grid, valid for the frame.
    [[nodiscard]] ColorAtlasBinding bindColorAtlas();

    /// Rasterises a cluster into a colour slot the cache has already assigned.
    ///
    /// Reached only on a probe miss, so this is per emoji rather than per cell.
    /// A slot the rasterizer declines is cleared rather than left, because the
    /// cache has already given the key away and the previous tenant's pixels
    /// would otherwise draw under this cluster's name.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster.
    ///   - count: How many code points; 1 is a lone one.
    ///   - slot: The slot the cache assigned.
    /// Rasterises a colour cluster into a slot, or reports the slot untouched.
    ///
    /// A rasteriser that declines the cluster is not a failure here: the slot is
    /// cleared, so its contents are this key's and every later frame is right to
    /// serve them. False is reserved for the argument guard, which returns before
    /// any write and leaves the previous tenant's pixels in place.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster, base code point first.
    ///   - count: How many code points the cluster carries.
    ///   - slot: The slot the cache assigned.
    /// - Returns: Whether this slot's contents are now this cluster's.
    bool rasteriseColorGlyph(const std::uint32_t* codePoints, int count, int slot);

    /// Returns one colour-square window of a cluster drawn at a sized block's size.
    ///
    /// Backs ``GlyphBackend::sizedColorGlyph``: the window is cut from Core's
    /// sized colour master (``rasteriseSizedColorWindow``) into an ordinary
    /// colour slot, keyed by ``sizedColorTileKey`` and verified, so it is the
    /// same bytes the Vulkan sheet holds. Absent when the sheet is full or the
    /// face declines, and the builder then magnifies the 1x master.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster, base first.
    ///   - tile: The block's scale and the window's band and column.
    /// - Returns: The window's rect in colour-atlas pixels.
    AtlasGlyph sizedColorGlyph(std::span<const std::uint32_t> codePoints, SizedTile tile);

    /// The seam this renderer supplies, as a table.
    ///
    /// Two hooks refuse by name whatever the rasterizer, because each is a
    /// decision rather than a gap. `boxGlyph` takes ``declineBoxGlyph``:
    /// Unifont's box characters do not tile, so the builder's decomposition
    /// draws them better than this font can. `shapeLigatureRuns` takes
    /// ``declineLigatureShaping``: there is no shaper here. Both drew the same
    /// picture as a null; naming them is what lets a reader of the table tell
    /// the choice from an entry nobody reached.
    ///
    /// The three colour hooks track the injected rasterizer. Without a colour
    /// source they stay null and an emoji draws its coverage in the foreground
    /// colour, which makes `optionalDefaultsAreLossy()` true — the accurate
    /// answer rather than a defect. Filling them regardless would report a
    /// complete backend over exactly that loss.
    ///
    /// - Returns: A table pointing at this renderer, valid for its lifetime.
    [[nodiscard]] GlyphBackend glyphBackend() UBIQUITTY_LIFETIMEBOUND;

    /// Starts a frame on the owned caches, so a slot touched inside it is not
    /// evicted within it.
    void beginFrame();

    /// Grows the mono and wide sheets to hold one screen of `cellsOnScreen`
    /// cells, by Core's sheet rule (``glyphSheetGridForScreen``).
    ///
    /// A sheet cut for a smaller screen runs out of slots before the frame
    /// does, and a frame of sized text then has blocks it cannot draw sharp.
    /// Never shrinks, so a grid that grows pays once and the steady state
    /// allocates nothing. A sheet that grows starts empty and bumps its
    /// generation, so an uploader re-sends it at its new extent. ``render``
    /// calls this; a caller that builds instances itself calls it before the
    /// build.
    ///
    /// - Parameter cellsOnScreen: Cells in the grid the next frame draws.
    void reserveSheetsForScreen(int cellsOnScreen);
    /// Returns one code point's atlas rect, faulting it into a slot on a miss.
    ///
    /// - Parameters:
    ///   - codePoint: The code point to draw.
    ///   - weight: Ignored; this rasterizer applies no variable weight.
    ///   - phase: Ignored; this rasterizer has no subpixel phases.
    /// - Returns: The glyph's atlas rect, or an absent glyph when no slot is free.
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t weight, std::uint32_t phase);
    /// Hands the builder this renderer's mono atlas for one frame.
    ///
    /// - Returns: The slot cache, its layout and capacity, and weight 0 — this
    ///   rasterizer applies no variable weight, so one key keeps key and raster
    ///   in agreement.
    MonoAtlasBinding bindMonoAtlas();

    /// Binds the dedicated proportional sheet for one frame.
    [[nodiscard]] ProportionalAtlasBinding bindProportionalAtlas();

    /// Shapes a proportional run through the injected rasterizer.
    int shapeProportionalRun(ClusterSpan, FontFace,
        std::span<Ubiquitty::ProportionalShapedGlyph>);

    /// Rasterises one proportional glyph into a Core-assigned slot.
    bool rasteriseProportionalGlyph(std::uint32_t glyphID, FontFace,
        std::uint32_t weight, std::uint32_t variant, int slot);
    /// Rasterizes a mono glyph into a slot the cache has just allocated.
    ///
    /// - Parameters:
    ///   - codePoint: The code point to draw.
    ///   - phase: Ignored; this rasterizer has no subpixel phases.
    ///   - slot: The slot to draw into.
    /// Rasterises one code point into a mono slot.
    ///
    /// Always writes: a code point the rasteriser declines leaves the slot
    /// blanked rather than stale, which is a defined answer this key owns.
    ///
    /// - Parameters:
    ///   - codePoint: The code point to draw.
    ///   - slot: The slot the cache assigned.
    /// - Returns: True, always — see the discussion.
    bool rasteriseMonoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t variant, int slot);

    /// Returns the monochrome master for a base and its combining marks.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster's code points, base first.
    ///   - count: How many of them.
    ///   - weight: Ignored; this rasterizer applies no variable weight.
    ///   - phase: Ignored; this rasterizer has no subpixel phases.
    /// - Returns: The run's atlas rect, or an absent glyph when no slot is free.
    AtlasGlyph clusterGlyph(const std::uint32_t* codePoints, int count, FontFace, std::uint32_t weight, std::uint32_t phase);
    /// Returns a two-cell glyph's rect from the wide atlas.
    ///
    /// - Parameter codePoint: The code point to draw.
    /// - Returns: The glyph's atlas rect, or an absent glyph when no slot is free.
    AtlasGlyph wideGlyph(std::uint32_t codePoint, FontFace);
    /// Returns a two-cell cluster's rect from the wide atlas.
    ///
    /// On the wide page for the reason ``wideGlyph`` is, and the note below
    /// about the 2x master is the same hazard a third time: ``composite``
    /// selects an atlas by instance width alone, so a wide cell holding a UV
    /// taken from the mono page names a slot nothing wrote.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster, base code point first.
    ///   - weight: The stroke weight, which this rasterizer does not vary.
    /// - Returns: The cluster's atlas rect, or an absent glyph when no slot is free.
    AtlasGlyph wideClusterGlyph(std::span<const std::uint32_t> codePoints, FontFace, std::uint32_t weight);
    // No 2x master on a sheet of its own: this rasterizer keeps two sheets,
    // and a double-master UV belongs to neither. A doubled row's cells take
    // ``sizedGlyph`` windows of their doubled master instead, and only a cell
    // those decline is the 1x glyph cropped.
    //
    // Double rows reach this backend: TerminalFrameInputs::capture fills
    // lineAttributes for every caller, the Web frontend captures through it,
    // and composite() reads the row's scale to tell a wide cell from a narrow
    // one on a doubled row. Anything here that assumes their absence paints a
    // doubled row's cells at the wrong span.
    /// Reports no 2x master, so the builder crops the 1x glyph instead.
    ///
    /// - Parameters:
    ///   - codePoint: Ignored; no code point has a master here.
    ///   - face: Ignored, for the same reason.
    /// - Returns: An absent glyph, always.
    AtlasGlyph doubleGlyph(std::uint32_t codePoint, FontFace face, DoubleRegion)
    {
        (void)codePoint;
        (void)face;
        return {};
    }

    /// Returns one cell-shaped window of a glyph rasterised at the block's scale.
    ///
    /// Whether this answers at all is the rasterizer's to say: an outline face
    /// gains real detail at three times its size, a bitmap face reproduces the
    /// magnified crop the builder already draws for free, and spending an atlas
    /// slot to do that is the trade ``rasterisesScaledGlyphsSharply`` refuses.
    /// The same function therefore serves a bitmap arm by declining and an
    /// outline arm by drawing — which is why it lives here rather than once per
    /// frontend: portable, Linux and the browser all render through this class.
    ///
    /// - Parameters:
    ///   - codePoint: The character to resolve.
    ///   - face: Which face to draw it in.
    ///   - weight: The pen's weight, for the key.
    ///   - phase: The subpixel phase the anchor was positioned at.
    ///   - tile: Which window of the scaled glyph.
    /// - Returns: The window's rect, or an absent glyph — absent keeps the cell
    ///   on the crop, which draws the same picture soft.
    AtlasGlyph sizedGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile);

    /// ``sizedGlyph`` for a two-cell glyph, cut from the wide page.
    ///
    /// The raster is drawn at the tile's scale times the wide slot and its
    /// windows step two cells, the span the builder's wide tiles take, so each
    /// window maps 1:1 onto one tile. Declined exactly where ``sizedGlyph`` is.
    ///
    /// - Parameters:
    ///   - codePoint: The character to resolve.
    ///   - face: Which face to draw it in.
    ///   - tile: Which window of the scaled glyph.
    /// - Returns: The window's rect on the wide page, or an absent glyph, which
    ///   keeps the cell on the crop.
    AtlasGlyph sizedWideGlyph(std::uint32_t codePoint, FontFace face, SizedTile tile);

private:
    // One CPU R8 coverage atlas: a GlyphSlotCache policy + the slot geometry
    // + the pixel storage it indexes. atlasWidth = columns*slotWidthInPixels,
    // atlasHeight = (capacity/columns)*slotHeightInPixels.
    struct Atlas {
        GlyphSlotCache cache;
        GlyphSlotGeometry geometry;
        std::vector<std::uint8_t> pixels; // atlasWidth*atlasHeight*bytesPerPixel
        int capacity;
        int atlasWidth;
        int atlasHeight;
        /// 1 for a coverage sheet, 4 for the RGBA colour one.
        ///
        /// The colour sheet is the same slot grid and the same cache over a wider
        /// pixel, so it is this field rather than a second type: everything above
        /// — the key, the eviction, the slot geometry, the UV — is identical, and
        /// a parallel struct would duplicate all of it to vary the stride.
        int bytesPerPixel;
        /// Builds one atlas: a slot grid, and the glyph box inside each slot.
        ///
        /// A `glyphWidthInPixels` of zero or less makes the glyph box fill the
        /// slot — cell-tight, no margin, which is what the wide and Unifont
        /// atlases want. A narrower box with a non-zero margin makes the slot a
        /// larger canvas with the cell box centred in it, which is what the
        /// subpixel overflow pass needs to draw ink outside the cell.
        ///
        /// - Parameters:
        ///   - capacity: How many slots.
        ///   - columns: Slots per row in the grid.
        ///   - slotWidthInPixels: One slot's width.
        ///   - slotHeightInPixels: One slot's height.
        ///   - glyphWidthInPixels: The glyph box width; <= 0 fills the slot.
        ///   - marginInPixels: Canvas margin on each side, per axis.
        ///   - bytesPerPixel: 1 for coverage, 4 for RGBA.
        Atlas(int capacity, int columns, int slotWidthInPixels, int slotHeightInPixels, int glyphWidthInPixels = -1, IntSize marginInPixels = {}, int bytesPerPixel = 1);

        /// Builds one atlas of `grid.columns * grid.rows` slots, `grid.columns`
        /// a row; the other parameters are the ones above.
        Atlas(GridSize grid, int slotWidthInPixels, int slotHeightInPixels, int glyphWidthInPixels = -1, IntSize marginInPixels = {}, int bytesPerPixel = 1)
            : Atlas(grid.columns * grid.rows, grid.columns, slotWidthInPixels, slotHeightInPixels, glyphWidthInPixels, marginInPixels, bytesPerPixel)
        {
        }
    };

    /// Recuts `atlas` to `grid`, empty, keeping its slot geometry.
    ///
    /// - Parameters:
    ///   - atlas: The mono or wide sheet.
    ///   - grid: The grid it holds from here on.
    static void recutAtlas(Atlas& atlas, GridSize grid);

    /// Which part of a cell-baked instance a cell pass paints.
    ///
    /// A cell-baked glyph is drawn inside its cell's own quad, so the cell pass
    /// lays the ground and the ink in one go. A picture placed under the text
    /// has to go between the two, and a frame holding one paints each cell
    /// twice: the ground, then the ink once the picture is down.
    enum class CellPaint : std::uint8_t {
        /// The ground and the ink together, mixed by the glyph's coverage.
        Whole,
        /// The ground alone, over the cell's whole span.
        Ground,
        /// The ink alone, mixed over whatever the surface holds where the glyph
        /// has coverage. Over the cell's own ground it gives the byte
        /// ``Whole`` gives.
        Ink,
    };
    /// What a glyph pass mixes its ink over.
    enum class InkGround : std::uint8_t {
        /// The cell background the instance carries, as the GPU glyph shaders
        /// mix it, so the CPU and GPU arms land on the same byte.
        CellBackground,
        /// The surface's own pixel, for a frame with a picture under its text:
        /// the cell background is no longer what lies beneath the ink there,
        /// and mixing over it would ring every antialiased edge with it.
        Surface,
    };
    /// Composites one emoji from the RGBA sheet over the destination.
    ///
    /// Separate from ``compositeGlyph`` because there is no pen to apply: the
    /// glyph carries its own colours, and tinting them with the foreground is
    /// what the mono path already does to an emoji.
    ///
    /// - Parameters:
    ///   - instance: The emoji's box and its slot's UV rect.
    ///   - rgba: The destination surface, blended in place.
    ///   - surface: The destination's extent.
    void compositeColorGlyph(const GlyphInstance& instance, std::vector<std::uint8_t>& rgba, IntSize surface) const;
    /// Blends one cell's slice of a shaped run over the surface.
    ///
    /// The slice, not the run: the builder has already reduced a run's strip to
    /// the part this cell shows, so both atlas corners and the destination box
    /// name one cell and this needs no notion of which cell of the run it is.
    ///
    /// Samples the strip sheet by integer tap, as the shader does. The CPU and
    /// GPU arms are compared byte for byte, and a filtered read here would differ
    /// from an unfiltered read there by a fraction of a channel on every edge
    /// pixel — a disagreement in the oracle rather than in the picture.
    ///
    /// - Parameters:
    ///   - instance: The slice's box and its rect in the strip sheet.
    ///   - rgba: The destination surface, blended in place.
    ///   - surface: The destination's extent.
    ///   - ground: What the ink mixes over.
    void compositeLigatureGlyph(const GlyphInstance& instance, const LigatureSheet& sheet, std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const;
    /// Blends one shaped proportional glyph from its dedicated sheet.
    ///
    /// - Parameters:
    ///   - instance: The glyph's box and its rect in the sheet.
    ///   - rgba: The destination surface, blended in place.
    ///   - surface: The destination's extent.
    ///   - ground: What the ink mixes over.
    void compositeProportionalGlyph(const GlyphInstance& instance,
        std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const;

    /// Resolves a key to a slot, rasterising into it when the slot is fresh.
    ///
    /// The face travels with the key rather than beside it: the keys separate
    /// the faces, so a slot filled from a different one holds a glyph its key
    /// does not describe.
    ///
    /// - Parameters:
    ///   - atlas: The sheet to fault into.
    ///   - key: The slot key, which already encodes the face.
    ///   - codePoint: The glyph to draw.
    ///   - face: The face to draw it at.
    /// - Returns: The slot's atlas rectangle, or an absent glyph when the sheet
    ///   is full for this frame.
    AtlasGlyph faultGlyph(Atlas& atlas, std::uint32_t key, std::uint32_t codePoint, FontFace face);
    /// Faults a cluster's composite into the sheet: the slot its key already
    /// holds, or a fresh one drawn through the rasterizer's cluster path.
    ///
    /// - Parameters:
    ///   - atlas: The sheet to fault into.
    ///   - key: The slot key for the cluster at its phase and face.
    ///   - codePoints: The cluster's code points, base first.
    ///   - face: The face to draw it at.
    ///   - weight: The weight the rasterizer applies, where it applies one.
    /// - Returns: The slot's atlas rectangle, or an absent glyph when the sheet
    ///   is full for this frame.
    AtlasGlyph faultCluster(Atlas& atlas, std::uint32_t key, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight);
    /// Draws one glyph into a slot through the injected rasterizer.
    ///
    /// A rasterizer with a single weight may ignore the face; one with bold
    /// members answers it. Either way the slot is filled at the face its key
    /// names.
    ///
    /// - Parameters:
    ///   - atlas: The sheet holding the slot.
    ///   - slot: The slot index to fill.
    ///   - codePoint: The glyph to draw.
    ///   - face: The face to draw it at.
    void rasterizeInto(Atlas& atlas, int slot, std::uint32_t codePoint, FontFace face);
    /// Copies a coverage plane the builder rasterised into a mono slot.
    ///
    /// Backs ``GlyphBackend::stageMonoCoverage``, whose whole obligation is this
    /// copy: the glyphs Core draws itself — a DECDLD soft font today — arrive as
    /// finished pixels rather than as something to look up.
    ///
    /// - Parameters:
    ///   - slot: The slot the cache assigned.
    ///   - coverage: R8 bytes, row-major, laid out for the slot box; the size
    ///     is checked against the slot rather than trusted.
    /// - Returns: Whether the slot was written.
    bool stageMonoCoverage(int slot, std::span<const std::uint8_t> coverage);
    /// Draws a cluster's composite into a slot through the injected rasterizer.
    ///
    /// - Parameters:
    ///   - atlas: The sheet holding the slot.
    ///   - slot: The slot index to fill.
    ///   - codePoints: The cluster's code points, base first.
    ///   - face: The face to draw it at.
    ///   - weight: The weight the rasterizer applies, where it applies one.
    void rasterizeClusterInto(Atlas& atlas, int slot, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight);
    /// Returns one window of a sized glyph on `atlas`, faulting it on a miss.
    ///
    /// - Parameters:
    ///   - atlas: The mono or the wide page; the window takes its slot geometry.
    ///   - codePoint: The character to resolve.
    ///   - face: Which face to draw it in.
    ///   - weight: The pen's weight, for the key.
    ///   - phase: The subpixel phase, for the key.
    ///   - tile: Which window of the scaled glyph.
    /// - Returns: The window's rect, or an absent glyph when declined or full.
    AtlasGlyph faultSizedWindow(Atlas& atlas, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile);
    /// Rasterizes `codePoint` at `tile.scale` and copies one window into `slot`.
    ///
    /// The scaled raster is drawn into a scratch buffer `scale` times the slot
    /// on both axes and the window is cut from it, rather than each window being
    /// drawn from its own raster: one raster serves every window of a block, and
    /// the cut lands on exactly the boundaries the quads abut on. Windows step
    /// by the frame's reference advance, which is one cell on the mono page and
    /// two on the wide one.
    ///
    /// - Parameters:
    ///   - atlas: The page the slot belongs to.
    ///   - slot: The slot to fill.
    ///   - codePoint: The character to draw.
    ///   - face: Which face to draw it in.
    ///   - weight: The pen's weight.
    ///   - phase: The anchor's raster variant; its phase bits are drawn into the window.
    ///   - tile: Which window of the scaled glyph.
    void rasteriseSizedWindowInto(Atlas& atlas, int slot, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase, SizedTile tile);
    /// Paints one cell-baked instance at the frame's grid origin.
    ///
    /// Every other composite pass reads an absolute position off its instance;
    /// this one is the exception, because ``CellInstance::gridPosition`` is
    /// (column, row) and the GPU backends add the origin as a shader uniform.
    ///
    /// - Parameters:
    ///   - instance: The cell to paint.
    ///   - gridOriginInPixels: The inset the caller passed as
    ///     ``BuildInputs::originInPixels``, already whole.
    ///   - rgba: The surface to paint into.
    ///   - surface: Its bounds.
    ///   - rowScale: The row's DECDWL width scale, 1 or 2. It divides out of
    ///     `widthCells` to leave the cell's own width, which is what says
    ///     whether the glyph came from the wide sheet.
    ///   - paint: Which part of the cell to paint.
    void composite(const CellInstance& instance, IntPoint gridOriginInPixels, std::vector<std::uint8_t>& rgba, IntSize surface, int rowScale, CellPaint paint) const;
    /// Draws one subpixel-overflow glyph over the composited backgrounds.
    ///
    /// The full slot quad at `penX - margin`, mixed in the integer domain over
    /// `ground`, with coverage 0 discarded — over the cell background, the
    /// mirror of the GPU's glyph shader. The buffer is empty in cell-baked mode.
    ///
    /// - Parameters:
    ///   - atlas: The sheet the glyph's rect names: the mono page, or the
    ///     wide page for a two-cell glyph.
    ///   - rgba: Destination pixels.
    ///   - surface: The destination's extent.
    ///   - ground: What the ink mixes over.
    void compositeGlyph(const GlyphInstance&, const Atlas& atlas, std::vector<std::uint8_t>& rgba, IntSize surface, InkGround ground) const;

    /// Draws one decoration rule (underline, strikethrough) over the frame.
    ///
    /// - Parameters:
    ///   - rgba: Destination pixels.
    ///   - surface: The destination's extent.
    void compositeDecoration(const DecorationInstance&, std::vector<std::uint8_t>& rgba, IntSize surface) const;
    /// Alpha-blends one overlay quad (the cursor) over the composited frame.
    ///
    /// A filled rect, or just the border when `strokeWidth > 0`. The blend is in
    /// the integer domain to match the overlay shader, which is what makes this
    /// the CPU side of the CPU/GPU oracle. No SDF, corner radius or shade here.
    ///
    /// - Parameters:
    ///   - rgba: Destination pixels.
    ///   - surface: The destination's extent.
    void compositeOverlay(const OverlayInstance&, std::vector<std::uint8_t>& rgba, IntSize surface) const;
    /// Draws the toolkit caret: the stem's whole pixels, mixed under its
    /// 8-bit mask as GSK's cairo renderer mixes GtkText's caret.
    ///
    /// - Parameters:
    ///   - overlay: The caret quad, `shapeKind` the toolkit caret's.
    ///   - rgba: The image, written in place.
    ///   - surface: That image's size.
    void compositeToolkitCaret(const OverlayInstance& overlay, std::vector<std::uint8_t>& rgba, IntSize surface) const;
    /// Draws the focused pill's ring: the shadow and the pill's ends outside
    /// the matched cells' box, and nothing inside it.
    ///
    /// - Parameters:
    ///   - overlay: The ring quad, `shapeKind` the ring's.
    ///   - rgba: The image, written in place.
    ///   - surface: That image's size.
    void compositeFocusedPillRing(const OverlayInstance& overlay, std::vector<std::uint8_t>& rgba, IntSize surface) const;

    /// Composites one box-drawing quad — the CPU mirror of the overlay
    /// fragment's analytic branch.
    ///
    /// The same stream the GPU arms draw between the cell pass and the glyph
    /// passes: plain strip fills, screen-space shades, the mosaic mask, and
    /// the Powerline SDF shapes, each antialiased from its signed distance so
    /// a diagonal or a curve matches the Metal arms' feathered edge instead
    /// of a stair-step.
    ///
    /// - Parameters:
    ///   - rgba: The destination surface, RGBA8.
    ///   - surface: Its extent in pixels.
    void compositeBoxFill(const OverlayInstance&, std::vector<std::uint8_t>& rgba, IntSize surface) const;
    // Bilinear-free nearest sample of the mono coverage atlas at a UV, for
    // the decoration skip-ink mask. Clamped to the atlas.
    /// One texel of the mono coverage sheet, addressed in atlas pixels.
    [[nodiscard]] float sampleMonoCoverage(float x, float y) const;
    /// One texel of the ligature strip sheet, addressed in its atlas pixels;
    /// the skip-ink tap for a cell a ligature run covers. Zero without a
    /// sheet.
    ///
    /// - Parameters:
    ///   - x: The texel column.
    ///   - y: The texel row.
    /// - Returns: The coverage, 0…1.
    [[nodiscard]] float sampleStripCoverage(float x, float y) const;
    /// One texel of the proportional fallback-strip sheet.
    ///
    /// - Parameters:
    ///   - x: The texel column.
    ///   - y: The texel row.
    /// - Returns: The coverage, 0…1.
    [[nodiscard]] float sampleProportionalStripCoverage(float x, float y) const;
    /// One texel of the dedicated proportional glyph sheet, addressed in
    /// atlas pixels: what a rule under a run drawn glyph by glyph taps.
    ///
    /// - Parameters:
    ///   - x: The texel column.
    ///   - y: The texel row.
    /// - Returns: The coverage, 0…1.
    [[nodiscard]] float sampleProportionalCoverage(float x, float y) const;
    /// Which kitty z-band to composite: Under = z<0, drawn over the cell
    /// backgrounds and before every pass that inks text, the cell-baked ink
    /// included, so the text reads on top; Over = z>=0, drawn after the text
    /// and its rules so it covers them. The under/over split the macOS GPU
    /// image pass uses.
    enum class ImageBand {
        Under,
        Over
    };
    /// Resolves the frame's image quads, ordered for drawing, into
    /// `m_imageQuads`, and where the text goes between them into
    /// `m_imageQuadSplit`.
    ///
    /// The quads are what ``Rendering::collectImageQuads`` assembles from a
    /// live terminal, read from the frame's copies instead: each grid placement
    /// through the shared `resolveImagePlacements`, the arithmetic the Vulkan
    /// backend runs, then a quad for each kitty Unicode-placeholder cell, both
    /// at this renderer's cell. Quads z-sort with the lower z first, and ties
    /// keep resolve order so a later placement at the same z draws on top. The
    /// count is bounded by the shared image-quad limit,
    /// `Image::maxTerminalImagePlacementCount`, as on the GPU arms.
    ///
    /// Also retires the mip chains of images the previous frame drew and this
    /// one does not, so it runs once a frame, before either band.
    ///
    /// - Parameter in: The frame's inputs, holding the image table, the
    ///   placements, and the cells the placeholders are read from.
    void resolveImageQuads(const BuildInputs& in);
    /// Draws the frame's resolved image quads of one z-band into the grid.
    ///
    /// Sampling is the quad's `filter`: nearest for a whole-number enlargement,
    /// else bilinear within the two mip levels that bracket the quad's level of
    /// detail and blended between them. The blend is straight-alpha RGBA, and
    /// each quad is clipped to its device rect.
    ///
    /// Frame 0 only; animation is deferred. A no-op when the frame resolved no
    /// quads in `band`.
    ///
    /// - Parameters:
    ///   - in: The frame's inputs, holding the image table the quads name.
    ///   - band: Which z-band to draw — `Under` is z<0, `Over` is z>=0.
    ///   - rgba: Destination pixels.
    ///   - surface: The destination's extent.
    void compositeImages(const BuildInputs& in, ImageBand band, std::vector<std::uint8_t>& rgba, IntSize surface) const;

    /// One image's mip chain, built the first time a filtered quad samples it
    /// and kept until its content stamp moves or it leaves the frame.
    struct ImageMipChain {
        std::uint32_t imageID = 0;
        std::uint64_t stamp = 0;
        std::vector<IntSize> sizes;
        std::vector<std::vector<std::uint8_t>> levels;
        bool isSeenThisFrame = false;
    };

    /// The chain for `image`, built or reused.
    ///
    /// - Parameter image: A used image with pixels.
    /// - Returns: The chain, valid until the next composite.
    [[nodiscard]] const ImageMipChain& imageMipChain(const Image::TerminalImage& image) const;

    /// Fills `chain` with `image`'s levels, each bled and halved from the one
    /// above by Core's shared rule.
    ///
    /// - Parameters:
    ///   - chain: The entry to fill; emptied by the caller.
    ///   - image: A used image with pixels.
    /// - Returns: `chain`.
    static ImageMipChain& buildImageMipChain(ImageMipChain& chain, const Image::TerminalImage& image);

    /// Composites one `Smooth` quad: bilinear in the two levels that bracket
    /// its level of detail, blended between them, then the same
    /// straight-alpha over-blend as the nearest path.
    ///
    /// - Parameters:
    ///   - quad: The quad, already clipped.
    ///   - image: Its image.
    ///   - rgba: Destination pixels.
    ///   - surface: The destination's extent.
    void compositeFilteredImage(const Image::ImageQuad& quad, const Image::TerminalImage& image, std::vector<std::uint8_t>& rgba, IntSize surface) const;

    std::uint32_t m_monoAtlasGeneration = 0;
    std::uint32_t m_wideAtlasGeneration = 0;
    std::uint32_t m_colorAtlasGeneration = 0;
    int m_cellWidth;
    int m_cellHeight;
    // Declared before the atlases: a mono slot reads this rasterizer's
    // glyphOverhang() (via the shared sizing policy) to size its overflow canvas,
    // and the cell-width/height ctor delegates with m_unifont as the rasterizer —
    // so it must be fully constructed before m_mono's initializer runs.
    UnifontRasterizer m_unifont; // the default raster; also the injected one's fallback
    Atlas m_mono;
    Atlas m_wide;
    // Asked once, at construction, and never again: a rasterizer's answer is a
    // property of its font stack rather than of a frame, and re-reading it per
    // call would let the sheet and the seam disagree within one frame.
    //
    // Declared above m_color because m_color's extent depends on it.
    bool m_hasColorSource;
    // Square at the cell height, so a two-cell emoji maps to a near-square device
    // box without distortion — the slot macOS picks, for the same reason.
    //
    // One pixel when there is no colour source: a sheet at the cell height is
    // hundreds of kilobytes, and a Unifont-only build would carry all of it to
    // answer absent every time. Every accessor reports absent from
    // `m_hasColorSource` rather than from this extent, so the placeholder is
    // never something a caller can reach.
    Atlas m_color;
    // Asked once for the same reason m_hasColorSource is, and answered by the
    // same instance the GPU backend faults through. That shared instance is what
    // keeps the two arms byte-identical: a rasterizer that shapes gives both of
    // them ligatures, and one that does not gives neither.
    bool m_hasLigatureShaping;
    bool m_hasStripSheet; // whether the proportional face builds its fallback strip sheet
    // Per-glyph proportional coverage, independent of the run-keyed strip and
    // two-cell sheets even when their canvases happen to have the same shape.
    ProportionalGlyphSheet m_proportional;
    // Bounded proportional-run fallbacks use their own slots and coverage;
    // ligature pressure cannot evict them and they never sample its texture.
    LigatureSheet m_proportionalStrip;
    // The strip sheet, over the same rasterizer. Constructed unconditionally with
    // a one-cell box when nothing can shape, so absence lives in the bool above
    // rather than in an extent each accessor would have to interpret — the shape
    // m_color uses, for the same reason.
    LigatureSheet m_ligature;
    GlyphRasterizer& m_rasterizer; // == m_unifont unless a real-font one is injected
    // Mutable because compositing is const: the chains are a cache of what
    // the frame's images already say, rebuilt from them whenever they move.
    mutable std::vector<ImageMipChain> m_imageMipChains;
    // The frame's image quads in drawing order, and the index the text draws
    // at: quads before it go under the text, quads from it on over it.
    // Empty, holding no storage, until a frame first carries an image.
    std::vector<Image::ImageQuad> m_imageQuads;
    int m_imageQuadSplit = 0;
    std::vector<std::uint8_t> m_slotScratch; // slotWidthInPixels*slotHeightInPixels R8, reused per glyph
    // The scaled master a sized block's windows are cut from, held rather than
    // allocated per window: a scale-7 block asks for it 49 times a frame.
    std::vector<std::uint8_t> m_sizedScratch;
    // The line height the frame being built maps a sized band by, which the cut
    // of a sized window steps by: the cutter and the builder read one number.
    float m_sizedBandLineHeightInPixels = 0.0f;
    // The advance the frame being built steps a sized block's windows by, read
    // by the cut as the line height is.
    float m_sizedWindowAdvanceInPixels = 0.0f;
    std::vector<std::uint8_t> m_colorScratch; // slotInPixels^2 RGBA; separate because m_slotScratch is R8-sized
    std::vector<GlyphInstance> m_colorGlyphs; // the emoji pass, empty without a colour source
    std::vector<GlyphInstance> m_proportionalGlyphs;
    std::vector<GlyphInstance> m_proportionalStrips;
    std::vector<GlyphInstance> m_ligatureGlyphs; // the strip pass, empty without a shaper
    std::vector<CellInstance> m_cellInstances;
    std::vector<GlyphInstance> m_glyphs; // subpixel overflow pass (empty cell-baked)
    std::vector<GlyphInstance> m_wideGlyphs; // two-cell glyphs drawn smaller than their sized block
    std::vector<DecorationInstance> m_decorations;
    std::vector<OverlayInstance> m_overlays;
    // Whether the frame being rendered is the bitmap tier, whose partial
    // coverage is held to 51 levels before it mixes (see quantizedCoverage).
    bool m_isBitmapFace = false;
    std::vector<OverlayInstance> m_boxFills; // the analytic box/Powerline quads
    CellInstanceScratch m_builderScratch; // the builder's per-glyph rule scratch, kept across frames
    // The last frame's letters by row and column, and which rows hold them
    // (LetterLayout.h), for `columnAt` and `xInCellsAt`.
    std::vector<LetterExtent> m_letters;
    std::vector<std::uint8_t> m_letterRows;
    int m_letterColumns = 0;
    [[nodiscard]] std::span<const LetterExtent> rowLetters(int row) const;
} UBIQUITTY_UNSAFE_REFERENCE;

} // namespace Ubiquitty::Rendering
