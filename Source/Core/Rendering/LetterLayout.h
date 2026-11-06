// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// LetterLayout.h — where each column's letter stands on a row whose primary
// face is proportional (Docs/proportional-text.md, "A proportional face as
// the grid face").
//
// A proportional primary face lays a row out as proportional runs: the text
// of neighbouring cells drawn in one pen is shaped together in the primary
// face and every letter stands at the shaper's own advance from the one
// before it, packed as the typeface intends. A glyph that is not text in that
// face — box drawing, a block or shade, an emoji, a wide glyph from a
// fallback face, an OSC 66 claim, a downloaded bitmap — keeps the cells it
// claims at the grid's pitch, inside the row's flow; a cell nothing has
// written keeps the pitch too, so a column a program addressed past its text
// stands where the grid put it. The program's contract does not change:
// columns, rows, the winsize and the cursor column are the grid's. What
// changes is where a column is on screen, and every presentation that reads
// the screen — the cursor, the selection, the pointer, the rules under the
// text — reads the answer here rather than multiplying a column by a pitch.
// Column alignment across rows is not preserved, by design.
//
// Pure C++, no platform type: the shaper is the backend's and arrives as a
// function, so every arm lays its rows out by one rule.

#include "Base/SwiftInterop.h"

#include "Core/Rendering/GlyphKey.h"
#include "PAL/TextShaping.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// One column's letter on a row laid out as proportional runs: where its
/// glyph's pen stands and how far it advances, in device pixels from the
/// grid's left edge.
///
/// A claim of several cells gives its lead the whole claim and each
/// continuation the slice the claim's pitch puts there, so a pointer inside
/// the claim still resolves to the column it stands on.
///
/// Escapable: two floats, so a frontend reads one through the capture and
/// keeps it.
struct UBIQUITTY_ESCAPABLE LetterExtent {
    float leftInPixels = 0.0f;
    float advanceInPixels = 0.0f;
};

/// How a cell takes part in its row's layout.
enum class LetterCellKind : std::uint8_t {
    /// Text in the primary face, shaped with the cells beside it that share
    /// its pen, at the face's own advances.
    Letter,
    /// A glyph that keeps its cells at the grid's pitch: `widthCells` of them.
    CellBound,
    /// A cell nothing has written, which keeps one cell's pitch.
    Empty,
    /// A cell of the claim before it, which its lead's extent covers.
    Continuation,
};

/// One cell as the row layout reads it.
struct LetterCell {
    LetterCellKind kind = LetterCellKind::Empty;
    /// The face the cell draws in, which is the face the run is shaped in.
    FontFace face = FontFace::Regular;
    /// The SGR 1:n variable weight the pen carries; zero for the face's own.
    std::uint32_t weight = 0;
    /// Where a run may not cross: two neighbouring letters shape together
    /// only when this agrees. The builder derives it from the cell's pen, so
    /// a run is the cells of one pen.
    std::uint32_t runKey = 0;
    /// The cell's code points in the caller's pool: its base and the marks
    /// that follow it, `clusterCount` entries from `clusterStart`.
    std::uint32_t clusterStart = 0;
    std::uint16_t clusterCount = 0;
    /// What a cell-bound glyph advances by, in cells at the grid's pitch: the
    /// columns its claim covers, times the row's doubling.
    float widthCells = 1.0f;
};

/// Shapes a run in the primary face at its natural advances; the shape
/// ``GlyphBackend::shapePrimaryRun`` has. Writes up to `capacity` glyphs and
/// returns how many; zero refuses the run.
using LetterShaper = int (*)(void* context, std::span<const std::uint32_t> codePoints, FontFace, std::uint32_t weight,
    Ubiquitty::ProportionalShapedGlyph* out, int capacity);

/// Row scratch the layout fills, so a frame that lays out many rows reuses
/// one allocation rather than taking one per run.
struct LetterLayoutScratch {
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped;
    std::vector<std::uint16_t> cellOfCodePoint;
    std::vector<float> runLefts;
};

/// Lays one row out as proportional runs.
///
/// Walks the cells in order with one pen. A maximal run of letters sharing a
/// run key is shaped once; each of its cells then begins where the first
/// glyph of its cluster begins, so a kern between two letters moves the
/// second and a mark adds nothing to its base. A cell whose code points the
/// shaper absorbed into a neighbour's glyph stands at that neighbour's end
/// with no advance of its own. A run the shaper refuses keeps the grid's
/// pitch, which is what a backend with no shaper draws. Every cell-bound or
/// empty cell advances the pen by its cells at the pitch, and a continuation
/// takes its slice of the claim before it.
///
/// - Parameters:
///   - cells: The row, one entry per column.
///   - codePointPool: The code points the cells' clusters index.
///   - cellAdvanceInPixels: One cell's pitch on this row's frame, in device pixels.
///   - widthScale: The row's doubling: 2 on a DECDWL or DECDHL row, else 1.
///     A letter's advance and an empty cell's pitch are both taken that many
///     times; a cell-bound cell's `widthCells` already carries it.
///   - shaper: The backend's primary-face shaper; null keeps every run on the pitch.
///   - shaperContext: Handed to `shaper` as its first argument.
///   - scratch: Reusable storage.
///   - out: Receives one extent per cell; its size bounds the columns read.
/// - Returns: True when some run was laid out at the shaper's advances;
///   false when every cell kept the grid's pitch.
bool layoutRowLetters(std::span<const LetterCell> cells, std::span<const std::uint32_t> codePointPool,
    double cellAdvanceInPixels, double widthScale, LetterShaper shaper, void* shaperContext,
    LetterLayoutScratch& scratch, std::span<LetterExtent> out);

/// The column a pixel along a laid-out row falls on.
///
/// The column whose extent holds the pixel; a pixel before the first letter
/// names column 0, one past the last names the last column. A column with no
/// advance of its own — one a ligature absorbed — is passed over for the
/// column whose letter is under the pixel.
///
/// - Parameters:
///   - letters: The row's extents.
///   - xInPixels: The pixel, from the grid's left edge.
/// - Returns: The column, or 0 for an empty row.
[[nodiscard]] int letterColumnAt(std::span<const LetterExtent> letters, double xInPixels);

/// Where a pixel is along a laid-out row, in cells: the column under it plus
/// the fraction of that letter's advance the pixel has crossed.
///
/// The position the pointer ladder resolves a caret from: its nearest whole
/// value is the boundary between two letters, so a press on the right half
/// of a letter stands after it, as it does on a monospace row.
///
/// - Parameters:
///   - letters: The row's extents.
///   - xInPixels: The pixel, from the grid's left edge.
/// - Returns: The position, 0 through the column count.
[[nodiscard]] double letterXInCells(std::span<const LetterExtent> letters, double xInPixels);

} // namespace Ubiquitty::Rendering
