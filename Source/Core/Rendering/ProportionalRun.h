// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ProportionalRun.h — where a proportional run's glyphs go
// (Docs/proportional-text.md), from what a shaper answered.
//
// A backend shapes the run in its proportional face and answers one
// ProportionalShapedGlyph per glyph; everything placed from that answer is
// here, once, for every arm. The renderer asks where each glyph's quad goes
// and at what phase; the model asks which cell each code point's ink begins
// in, so the text can ride the cells that show it; a frontend asks which
// code point a pixel falls on. Three questions, one layout: a frontend that
// can shape gains all three by answering the first, and no two arms can
// disagree about where a letter is.

#include "Core/Rendering/GlyphBackendTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// One glyph of a laid-out run: which glyph, where its origin is from the
/// run's pen, and which code point it begins at.
struct ProportionalGlyphPlacement {
    /// The face's glyph identifier.
    std::uint32_t glyph = 0;
    /// The glyph origin's x, in pixels from the run's aligned pen: the pen
    /// after the glyphs before it, plus this glyph's own offset and any
    /// justification opened before it.
    float xInPixels = 0.0f;
    /// The glyph origin's y from the baseline, up.
    float yInPixels = 0.0f;
    /// The index in the run's text of the code point the glyph begins at.
    std::uint16_t cluster = 0;
};

/// A laid-out run: its glyphs from the aligned pen, and the pen itself.
struct ProportionalRunLayout {
    /// How many entries of the caller's placement span were written.
    int glyphCount = 0;
    /// The text's width after justification, in pixels.
    double widthInPixels = 0.0;
    /// The aligned pen's offset from the run's first cell, in pixels: the
    /// alignment's slack for a right- or centre-aligned run, zero otherwise.
    double penOffsetInPixels = 0.0;
};

using ProportionalSourceOffsets = std::vector<double>;
using ProportionalSourceSpan = std::span<const std::uint32_t>;

/// Fills justified shifts before each source code point and the end boundary.
///
/// Only interior spaces open, once per source space regardless of how many
/// glyphs share its cluster. Frontends map their encoding indices to these
/// source offsets without choosing which gaps open.
///
/// - Parameters:
///   - codePoints: The source run.
///   - gapExtraInPixels: The extra advance assigned to each interior space.
///   - offsets: Reusable storage, resized to the source length plus one.
void proportionalJustificationOffsets(ProportionalSourceSpan codePoints,
    double gapExtraInPixels, ProportionalSourceOffsets& offsets);

/// Lays out a shaped run inside the cells it claims.
///
/// Justification (`h=3`) opens each interior space by the same amount until
/// the text spans its cells; right and centre alignment move the pen by the
/// slack. Every glyph's origin is then a distance from the run's first cell,
/// which is what every consumer wants: the renderer adds the run's pen, the
/// model divides by the cell advance, a hit test compares with a pixel.
/// Source-ordered glyphs use constant workspace; reordered justified glyphs
/// use one locally owned offset table bounded by the source span's length.
///
/// - Parameters:
///   - shaped: The shaper's answer, `shapedCount` entries.
///   - codePoints: The run's text, which the justification reads for spaces.
///   - widthCells: The cells the run claims.
///   - horizontalAlign: 0 left, 1 right, 2 centred, 3 justified.
///   - cellAdvanceInPixels: One cell's advance.
///   - out: Receives one placement per glyph, up to its size.
/// - Returns: The layout; `glyphCount` is zero for an empty answer.
[[nodiscard]] ProportionalRunLayout layoutProportionalRun(std::span<const ProportionalShapedGlyph> shaped,
    std::span<const std::uint32_t> codePoints, int widthCells, std::uint8_t horizontalAlign,
    double cellAdvanceInPixels, std::span<ProportionalGlyphPlacement> out);

/// The cell each code point's ink begins in.
///
/// A code point belongs to the cell its glyph's origin falls in, clamped to
/// the run's claim; a code point a ligature absorbed belongs to the
/// ligature's cell. The first code point belongs to the anchor whatever the
/// slack before the text: the anchor's content is the run's first code
/// point, which is what every reader of the grid takes the run to begin
/// with. The model stores each cell's text from this, so a copy of
/// the cells under a word yields that word and a cursor addressed to a cell
/// stands before the letters that begin there.
///
/// - Parameters:
///   - placements: The layout's glyphs, `glyphCount` entries.
///   - layout: The layout they came from.
///   - codePointCount: How many code points the run has.
///   - widthCells: The cells the run claims.
///   - cellAdvanceInPixels: One cell's advance.
///   - cellOfCodePoint: Receives one cell index per code point.
void proportionalRunCellOwners(std::span<const ProportionalGlyphPlacement> placements, const ProportionalRunLayout& layout,
    int codePointCount, int widthCells, double cellAdvanceInPixels, std::span<std::uint16_t> cellOfCodePoint);

/// The cells of a run's claim its text reaches: from the claim's first cell
/// through the one the last glyph's advance ends in.
///
/// A letter's ink runs on past the cell it begins in — the owner map names
/// that cell — and the cells it runs into are the text's, not the claim's
/// empty tail: a selection or a pointer on them is on the letter.
///
/// - Parameters:
///   - layout: The run's layout.
///   - widthCells: The cells the run claims.
///   - cellAdvanceInPixels: One cell's advance.
/// - Returns: The count, at least one and at most `widthCells`.
[[nodiscard]] int proportionalRunTextCells(const ProportionalRunLayout& layout, int widthCells, double cellAdvanceInPixels);

/// The code point a pixel inside the run falls on.
///
/// The glyph whose extent — origin to the next glyph's origin, or the text's
/// end — holds the pixel names it; a pixel past the last glyph names the
/// count, the position after the text, so a caret dropped there lands at the
/// end rather than on the last letter.
///
/// - Parameters:
///   - placements: The layout's glyphs, `glyphCount` entries.
///   - layout: The layout they came from.
///   - codePointCount: How many code points the run has.
///   - xInPixels: The pixel, from the run's first cell.
/// - Returns: The code point index, 0…`codePointCount`.
[[nodiscard]] int proportionalRunCodePointAt(std::span<const ProportionalGlyphPlacement> placements,
    const ProportionalRunLayout& layout, int codePointCount, double xInPixels);

/// A caret inside a run: the code point it stands before, and the direction
/// the pixel travelled to reach it.
struct ProportionalRunCaret {
    /// The code point the caret stands before, 0 through the run's length.
    int codePointIndex = 0;
    /// Whether the pixel rounded up to this caret rather than down to it.
    /// A caret that lands inside a grapheme is moved to the cluster's edge on
    /// the side it came from, so the direction outlives the rounding.
    bool didRoundForward = false;
};

/// The caret a pixel inside the run is nearest.
///
/// The difference from ``proportionalRunCodePointAt`` is the rounding, and it
/// is the whole reason this exists: that answers which letter a pixel is on,
/// this answers which gap between letters it is nearest. Glyphs arrive in text
/// order and each begins its cluster, so a glyph's origin is the caret before
/// its cluster and the next origin the caret after it; a pixel past the text
/// names the position after the last code point.
///
/// - Parameters:
///   - placements: The layout's glyphs, `glyphCount` entries.
///   - layout: The layout they came from.
///   - codePointCount: How many code points the run has.
///   - xInPixels: The pixel, from the run's first cell.
/// - Returns: The caret, whose index is 0 through `codePointCount`.
[[nodiscard]] ProportionalRunCaret proportionalRunCaretAt(std::span<const ProportionalGlyphPlacement> placements,
    const ProportionalRunLayout& layout, int codePointCount, double xInPixels);

} // namespace Ubiquitty::Rendering
