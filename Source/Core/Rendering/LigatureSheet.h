// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/GlyphBackend.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/ProportionalStripRaster.h"
#include "Core/Rendering/ShapedCellCache.h"

#include "PAL/TextShaping.h"
#include <cstdint>

#include <span>
#include <unordered_map>
#include <vector>

// The strip sheet shaped ligature runs live on.
//
// A strip holds one whole shaped run — `=>` drawn as the image the font makes of
// it — and every cell the run covers draws the slice of that strip its own
// column occupies. That is what lets a colour or style boundary inside a run
// recolour without moving a glyph: the ink is one picture, and the cells are
// windows onto it.
//
// The slot is cut to Core's published geometry and the run walk is Core's, for
// the reason both say so themselves: two arms deriving either would split one
// row's runs differently, and a comparison between them would disagree exactly
// where it is least able to say which is right. What is this file's is the
// shaping, the rasterising and the cache — both font halves through the Core
// GlyphRasterizer seam,
// and a slot per distinct run.
//
// Verifiable without a GPU. The host mirror is the source of truth and the
// upload copies it, so where a strip landed and whether writing one disturbed
// its neighbours are questions a smoke can ask on a machine with no device —
// the same property VulkanGlyphAtlas::monoCoverage exists for.

namespace Ubiquitty::Rendering {

/// The R8 strip sheet and the slot cache over it.
///
/// Not thread-safe, and does not need to be: it is reached from the frame build,
/// which is one thread here. The scratch buffers are reused strip to strip,
/// which is what keeps a steady-state frame free of allocation.
/// Probes a face for texture healing and answers the participation mode.
///
/// Runs Core's ``ligatureHealingProbe`` through the rasterizer twice — once as
/// one run, once a character at a time — and hands both glyph mappings to
/// ``decideLigatureParticipation``. The probe and the criterion live in Core so
/// every arm classifies one font one way; an arm with its own shaper (the Swift
/// sheet) shapes the same probe and calls the same decision.
///
/// - Parameter rasterizer: The face strips would shape and draw with.
/// - Returns: The mode the row walk must use for this face.
[[nodiscard]] LigatureParticipation probeLigatureParticipation(GlyphRasterizer& rasterizer);

class LigatureSheet {
public:
    /// Builds a sheet sized for the cell the frontend draws.
    ///
    /// - Parameters:
    ///   - cellWidthInPixels: The glyph box width inside the margins.
    ///   - cellHeightInPixels: The glyph box height.
    ///   - marginXInPixels: The horizontal overflow margin per side.
    ///   - marginYInPixels: The vertical overflow margin per side.
    ///   - rasterizer: The face to shape and draw with. Borrowed; a null one
    ///     leaves the sheet unusable rather than half-built.
    LigatureSheet(int cellWidthInPixels, int cellHeightInPixels, int marginXInPixels,
        int marginYInPixels, GlyphRasterizer* rasterizer);

    /// Whether this sheet can hand back a strip at all.
    ///
    /// False when there is no face, no shaper on it, or no room for even one
    /// slot. A caller that gets false leaves rows unligatured, which is the
    /// documented fallback rather than a failure.
    [[nodiscard]] bool isUsable() const { return m_isUsable; }

    /// Which stretches this sheet's font makes worth shaping.
    ///
    /// Probed once at construction from the face itself — see
    /// ``probeLigatureParticipation`` — so the row walk and the frontend's
    /// prescan read one verdict from one probe. The frontend copies this into
    /// ``BuildInputs::ligatureParticipation``.
    [[nodiscard]] LigatureParticipation participation() const { return m_participation; }

    /// Starts a frame on the slot cache, so a strip touched inside it is not
    /// evicted within it.
    void beginFrame()
    {
        m_cache.beginFrame();
        m_answers.endFrame();
    }

    /// The shaped answers held across frames: what a run's cells draw by
    /// identity, keyed by the run's text and face, never by its paint.
    [[nodiscard]] const ShapedCellCache& answers() const { return m_answers; }

    /// Fills one row's runs, shaping and rasterising each on a cache miss.
    ///
    /// Backs ``GlyphBackend::shapeLigatureRuns``. Every entry in
    /// `runs` is written, not only the covered ones: the caller reuses one span
    /// across rows, so a column this row does not cover would otherwise arrive
    /// holding the previous row's run.
    ///
    /// - Parameters:
    ///   - row: The distilled row, from Core's ``distilLigatureRow``.
    ///   - cellAdvanceInPixels: The advance one cell occupies, which the strip's
    ///     slices are measured against.
    ///   - runs: One entry per column, all reset before any is filled.
    void shapeRow(LigatureRowSpan row, float cellAdvanceInPixels, LigatureRunStream runs);

    /// The R8 host mirror the image is uploaded from, row-major.
    ///
    /// What a slot holds here is what the sampler will read, so a strip's
    /// placement is checkable with no device present.
    [[nodiscard]] std::span<const std::uint8_t> coverage() const { return m_coverage; }

    /// The mirror's row pitch in pixels.
    [[nodiscard]] int widthInPixels() const { return m_sheetWidthInPixels; }
    /// The mirror's height in pixels.
    [[nodiscard]] int heightInPixels() const { return m_sheetHeightInPixels; }
    /// One strip slot's width, Core's derivation.
    [[nodiscard]] int slotWidthInPixels() const { return m_slotWidthInPixels; }
    /// One strip slot's height, the mono slot's.
    [[nodiscard]] int slotHeightInPixels() const { return m_slotHeightInPixels; }
    /// How many strips the sheet holds.
    [[nodiscard]] int slotCapacity() const { return m_capacity; }
    /// Whether a slot changed since the last upload.
    [[nodiscard]] bool isDirty() const { return m_isDirty; }
    /// Marks the mirror uploaded.
    void clearDirty() { m_isDirty = false; }

    /// The rect a slot occupies, in ATLAS PIXELS.
    ///
    /// The unit is the contract: the builder adds a slice's atlas-pixel
    /// offsets to this rect's x, so a normalised fraction here sends every
    /// slot's slices to the sheet's origin and each run draws slot 0's ink.
    /// Public beside the mirror accessors for the same reason they are —
    /// where a strip sits is checkable with no device present.
    ///
    /// - Parameter slot: The strip slot.
    /// - Returns: The slot's full rect, `present` false out of range.
    [[nodiscard]] AtlasGlyph rectForSlot(int slot) const;

    /// Whether ``proportionalStrip`` can answer: the sheet is usable and the
    /// rasterizer has a proportional face.
    ///
    /// - Returns: True when proportional runs draw as strips here.
    [[nodiscard]] bool hasProportionalFace() const;

    /// The strip a proportional run draws and where its quad goes
    /// (Docs/proportional-text.md): ``GlyphBackend::proportionalStrip`` over
    /// this sheet.
    ///
    /// The text is shaped in the proportional face, laid out by
    /// ``proportionalStripLayout`` and rasterised at its fractional pen.
    /// The complete shape is reused across bounded horizontal tiles, clipped
    /// to the run's cells with the margin's overhang when the text fits.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - widthCells: The cells the run occupies.
    ///   - horizontalAlign: 0 left, 1 right, 2 centred, 3 justified.
    ///   - penResidualInPixels: The run's pen past its whole pixel.
    ///   - cellAdvanceInPixels: One cell's advance, the frame's.
    ///   - face: Regular or bold.
    ///   - variant: The raster variant the run's foreground selects.
    ///   - tileIndex: The zero-based horizontal tile of the complete run.
    ///   - size: The run's scale, the band asked for and its doubling across,
    ///     as ``GlyphBackend::proportionalStrip`` names them; the default is
    ///     the run at its own size.
    /// - Returns: The placement; an absent strip when the sheet refuses.
    [[nodiscard]] ProportionalStripPlacement proportionalStrip(std::span<const std::uint32_t> codePoints, int widthCells,
        std::uint8_t horizontalAlign, float penResidualInPixels, float cellAdvanceInPixels, FontFace face, std::uint32_t variant, int tileIndex = 0,
        SizedTile size = SizedTile { 1, 0, 0 });

    /// The cells a proportional run needs in the proportional face: what a
    /// `q=1` query is answered with.
    ///
    /// Shapes through the rasterizer alone and touches no sheet scratch, so
    /// the reader thread may ask while the render thread draws.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - cellAdvanceInPixels: One cell's advance.
    /// - Returns: `ceil(advance / cell)`, at least one; zero without a face.
    [[nodiscard]] int proportionalRunCells(std::span<const std::uint32_t> codePoints, float cellAdvanceInPixels) const;

    /// Shapes a run in the rasteriser's proportional face and answers its
    /// glyphs in the shape Core lays out (Docs/proportional-text.md).
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - face: The face the run is set in.
    ///   - glyphsOut: Receives one shaped glyph per glyph, up to its size.
    /// - Returns: How many were written, or zero without a proportional face.
    [[nodiscard]] int shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) const;

private:
    // Shapes and draws one run into `slot`, returning false when the face
    // declined it — a strip that could not be drawn is better left uncovered
    // than left holding the previous tenant's ink. `drawnCells` bounds the
    // columns whose ink ownership is recorded: context clusters shape and
    // draw but belong to a neighbouring chunk, so they own nothing here.
    /// What shaping a candidate produced.
    enum class RunOutcome : std::uint8_t {
        /// The face declined the run; the slot holds nothing to draw.
        Refused,
        /// The face shaped it glyph for glyph, changing nothing — the strip
        /// would draw the same characters the mono path draws, from a
        /// different raster. Not drawn, and the caller leaves the cells on
        /// the mono path.
        Unligated,
        /// The face substituted, so the strip carries ink the per-cell path
        /// cannot produce.
        Drawn,
    };

    /// The glyph this face maps `codePoint` to on its own — what the
    /// per-cell mono path draws. Memoized per face.
    ///
    /// - Parameter codePoint: The character to map.
    /// - Returns: The isolated glyph id, 0 when the face cannot answer.
    [[nodiscard]] unsigned int isolatedGlyphFor(std::uint32_t codePoint);

    [[nodiscard]] RunOutcome rasteriseRunInto(int slot, std::span<const std::uint32_t> codePoints, std::uint32_t weight, int leadingContext, int drawnCells, float cellAdvanceInPixels);

    /// Derives one answer per drawn cell from the run the sheet last
    /// rasterised, into ``m_answerScratch``.
    ///
    /// A run answers by identity only when the face mapped each character to
    /// its own glyph in place — one glyph per character, every cluster where
    /// its character was — and every glyph but a space drew ink. A cluster the
    /// face joined, a glyph that draws nothing (`///` is three glyphs, two of
    /// them blank under one carrier) or a `.notdef` leaves the run on its
    /// strip alone: a partial answer would draw some cells by identity beside
    /// neighbours drawn from a strip the identity cells no longer cover.
    ///
    /// - Parameters:
    ///   - leadingContext: How many shaped code points precede the drawn cells.
    ///   - drawnCells: How many cells the run covers.
    /// - Returns: Whether every drawn cell answered.
    [[nodiscard]] bool deriveShapedAnswers(int leadingContext, int drawnCells);

    int m_marginXInPixels;
    int m_stripTopInPixels = 0;
    int m_slotWidthInPixels = 0;
    int m_slotHeightInPixels = 0;
    int m_columns = 0;
    int m_capacity = 0;
    int m_sheetWidthInPixels = 0;
    int m_sheetHeightInPixels = 0;
    bool m_isUsable = false;
    bool m_isDirty = true;
    // The Core interface, not the FreeType class: the sheet needs a rasterizer
    // that shapes, and which one it is belongs to whoever built it. Holding the
    // concrete type would also make this sheet unusable to the CPU arm, which is
    // the arm the byte-identical oracle compares it against.
    GlyphRasterizer* m_rasterizer;
    LigatureParticipation m_participation = LigatureParticipation::OperatorRuns;

    /// The probe's verdict and the plan it sized, computed before construction
    /// so the constructor-only cache can be built to the plan's capacity.
    struct SheetDecision {
        LigatureParticipation participation = LigatureParticipation::OperatorRuns;
        LigatureStripPlan plan;
        /// The strip's top from the row's top: the margin and the face's
        /// proportional overhang above the cell, negated.
        int stripTopInPixels = 0;
    };
    /// Probes the face and plans the sheet it earns.
    ///
    /// - Parameters:
    ///   - cellWidthInPixels: The glyph box width inside the margins.
    ///   - cellHeightInPixels: The glyph box height.
    ///   - marginXInPixels: The horizontal overflow margin per side.
    ///   - marginYInPixels: The vertical overflow margin per side.
    ///   - rasterizer: The face to probe; null keeps the economy and no plan.
    /// - Returns: The verdict and the plan sized by it.
    static SheetDecision decideSheet(int cellWidthInPixels, int cellHeightInPixels,
        int marginXInPixels, int marginYInPixels, GlyphRasterizer* rasterizer);

    /// Builds the sheet a decision described.
    ///
    /// - Parameters:
    ///   - marginXInPixels: The horizontal overflow margin per side.
    ///   - rasterizer: The face to shape and draw with, borrowed.
    ///   - decision: The probe's verdict and the plan sized by it.
    LigatureSheet(int marginXInPixels, GlyphRasterizer* rasterizer, SheetDecision decision);
    GlyphSlotCache m_cache;
    std::vector<std::uint8_t> m_coverage; // R8 host mirror, sheetWidth * sheetHeight
    std::vector<std::uint8_t> m_slotScratch; // one slot, reused per strip
    std::vector<std::uint32_t> m_runCodePoints; // context + drawn + context, reused per run
    std::vector<GlyphRasterizer::ShapedGlyph> m_shaped; // reused per run
    std::vector<Ubiquitty::ProportionalShapedGlyph> m_proportionalShaped; // reused per proportional run
    std::vector<std::uint32_t> m_proportionalSource;
    ProportionalStripRequest m_proportionalRequest;
    ProportionalStripShape m_proportionalShape;
    // Per slot: whether the run in it substituted. A gated-walk font ligates
    // almost nothing a terminal shows — a path, a flag, a prompt — so the
    // verdict is what keeps those rows on the mono path, and it is recorded
    // per slot rather than recomputed because the row is re-walked every
    // frame while the answer is a property of the run's characters.
    std::vector<std::uint8_t> m_slotDidSubstitute;
    // The face's own one-character mapping, memoized: the run gate compares
    // against it, and it never changes for a given face.
    std::unordered_map<std::uint32_t, unsigned int> m_isolatedGlyph;

    std::vector<GlyphRasterizer::GlyphInkExtent> m_inkScratch; // reused per strip

    // The identity answers beside the strips. A strip is keyed by the run's
    // text and the text fragments of a screen do not repeat, so a sheet keyed
    // by fragment is unbounded; the glyphs a face actually draws saturate near
    // its repertoire, so a backend drawing by identity keys a bounded atlas.
    // The answers are derived from the strip's own measured raster on a fresh
    // slot, kept across frames, and asked for before the strip each frame.
    ShapedCellCache m_answers;
    std::vector<std::uint32_t> m_answerKey; // the run's words, then its context, length and weight
    std::vector<ShapedCell> m_answerScratch; // one per drawn cell, reused per run
};

} // namespace Ubiquitty::Rendering
