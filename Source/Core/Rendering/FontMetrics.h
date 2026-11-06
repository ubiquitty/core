// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>

// FontMetrics.h — portable vertical cell layout for a monospace face.
//
// A glyph is baseline-placed; the cell adds non-ink slack (the font's line gap,
// the user's line spacing, and the grid-snap remainder). The font `leading` must
// be split EVENLY above and below the glyph (half-leading), not left inside the
// ink box with the baseline pinned at `descent`: pinning shoves every glyph down
// by the full leading, so a face with a built-in line gap (e.g. Monaspace Neon,
// 1.4pt at 14pt) rides low while a leading-0 face looks fine.
//
// No platform types: pure C++ in namespace Ubiquitty, host-testable. All lengths
// share one unit (the caller's atlas px); the function neither knows nor cares
// which.

namespace Ubiquitty::Rendering::FontMetrics {

/// Unhinted face measurements in em units; zero means unavailable.
struct FallbackMetrics {
    double xHeight { 0 };
    double capHeight { 0 };
    double lineHeight { 0 };
};

/// A range override, or the catalogue default when inherited.
enum class FallbackSizing : std::uint8_t {
    Inherit,
    MatchPrimary,
    Nominal
};

/// Matches a fallback's visual size to the primary face, preferring x-height,
/// then cap height, then line height. Invalid metrics fall through to the next
/// pair. The adjustment is bounded to half through twice the nominal size so
/// malformed font metrics cannot request an unbounded raster allocation.
///
/// - Parameters:
///   - primary: The primary face metrics in em units.
///   - fallback: The fallback face metrics in em units.
/// - Returns: The multiplier for the primary point size.
[[nodiscard]] double fallbackScale(const FallbackMetrics& primary, const FallbackMetrics& fallback);

struct CellVerticalMetrics {
    double lineHeight; // cell height: ink box + leading + spacing, snapped
    double gapTotal; // non-ink slack = lineHeight - (ascent + descent)
    double gapTop; // slack above the glyph (whole units), ~gapTotal/2
    double baselineFromBottom; // pen baseline above the cell bottom = descent + gapBottom
};

// Lay out a cell's vertical metrics. `lineSpacing` is the user's extra spacing
// (same unit; may be negative). `snapUnit` > 0 rounds the cell height up to a
// whole multiple of snapUnit (one device px / whole point) so rows land on the
// pixel grid; 0 keeps the exact line. The total slack (leading + spacing + snap
// remainder) is split evenly, gapTop rounded to a whole unit so the pen origin
// stays pixel-aligned. A very negative lineSpacing is floored so the cell never
// collapses below half the ink box (glyphs would overprint their neighbours).
[[nodiscard]] CellVerticalMetrics layoutCell(double ascent, double descent, double leading, double lineSpacing, double snapUnit);

// The horizontal counterpart: which advance defines the cell.
//
// One representative glyph ('M') is the obvious probe and is correct for any face
// whose advances really are uniform — but a face that declares monospace while
// carrying one wider glyph then draws that glyph into its neighbour's column.
// GlyphSlotPolicy lets ink overhang the cell rather than clip it, so the failure
// mode is silent overlap rather than a visible cut.
//
// Take the widest of a representative sample instead. For a genuinely uniform
// face every advance is equal, so this returns exactly what probing 'M' returned:
// the cost is zero wherever the font is correct, and is paid only by a font that
// would otherwise overlap — where a wider cell is the only one that draws every
// glyph inside its own column.
//
// Printable ASCII is the right SET to sample, because that is the range
// "monospace" is a claim about. Fallback faces (symbols, CJK) routinely exceed the
// cell and are not covered: the slot margin already folds the fallback bounding
// box, and widening every column to fit a Nerd Font glyph would spend the whole
// grid on it.

/// The first code point a cell-advance sample covers (space).
inline constexpr char32_t firstProbeCharacter = 0x20;
/// The last code point a cell-advance sample covers (tilde).
inline constexpr char32_t lastProbeCharacter = 0x7E;
/// How many advances a full sample carries, so a caller can size its buffer
/// from the seam rather than from its own copy of the range.
inline constexpr int probeCharacterCount = static_cast<int>(lastProbeCharacter - firstProbeCharacter) + 1;

/// One face's sampled advances. Named because Swift imports a C++ parameter by
/// its type's name and a bare specialization has none — the same reason
/// ``CoveragePlaneSpan`` and ``ClusterSpan`` are named.
using AdvanceSpan = std::span<const double>;

/// Whether a face's columns hold glyphs of one advance or of many.
enum class FacePitch : std::uint8_t {
    /// Every sampled glyph advances the cell: the face's own placement fills it.
    Fixed,
    /// The sampled advances differ: the face's rows are laid out as
    /// proportional runs (Rendering/LetterLayout.h), and its cell is the
    /// digit's advance.
    Proportional,
};

/// The pitch of a face, from the same printable-ASCII sample as ``cellAdvance``.
///
/// Missing and broken entries are skipped rather than counted against the
/// face: a symbol face drawn as the primary carries a handful of ASCII glyphs
/// at one width, and its absent letters are no evidence of unequal ones.
/// Only two usable advances more than `0.001` apart make a face proportional.
///
/// - Parameter advances: The sampled advances, in any order.
/// - Returns: The pitch; `Fixed` when fewer than two usable advances were sampled.
[[nodiscard]] FacePitch facePitch(AdvanceSpan advances);

/// The cell width for a face, from a printable-ASCII advance sample and the
/// pitch Core read from the face.
///
/// A fixed-pitch face's cell is its widest usable advance, which for a
/// genuinely uniform face is every advance. A proportional face's cell is the
/// advance of `0`: its rows are not a grid of its letters but proportional
/// runs (Rendering/LetterLayout.h), and the cell serves what still counts
/// columns — the window's size, the winsize, a cell nothing has written, a
/// glyph that keeps its cells — so it is cut to the figure, which text faces
/// carry at one width so that numbers align, rather than to `W`. A face that
/// lacks `0` takes the mean of the lowercase advances it does carry.
///
/// The pitch is the caller's rather than read from `advances` here: the
/// sample a cell is cut from may be hinted — FreeType rounds each advance to
/// whole pixels on its own, so a monospace face can sample two widths a
/// pixel apart — while ``facePitch`` is read from the face's unhinted
/// advances. A fixed face told so keeps its widest hinted advance whatever
/// the rounding did to the rest.
///
/// A face missing a glyph advances it 0 and a broken face can report a
/// negative; neither is a cell width, so both are skipped. Returns 0 when
/// nothing usable was sampled, which leaves the caller's own fallback in
/// charge rather than collapsing the grid to a zero-width cell.
///
/// `advances` is read in code-point order from ``firstProbeCharacter``; a
/// caller that can only fill part of the range may pass what it has, and a
/// sample too short to reach `0` takes the widest advance it holds.
///
/// - Parameters:
///   - pitch: The face's pitch, from ``facePitch`` over its unhinted advances.
///   - advances: The sampled advances, in code-point order.
/// - Returns: The cell width, or 0 when nothing usable was sampled.
[[nodiscard]] double cellAdvance(FacePitch pitch, AdvanceSpan advances);

/// Whether a face's advances are those of a monospace face.
///
/// That is the terminal's definition of monospace: a column has one advance,
/// so a face that draws every `U+0020...U+007E` at one width is fixed-pitch
/// even when its `kCTFontTraitMonoSpace` bit is clear. Every probe glyph must
/// be present: a missing glyph advances `0` and a broken face can report a
/// negative, and a face with either does not draw printable ASCII, which
/// rules it out as a primary face however uniform its remaining advances
/// are. A braille or emoji face with one or two ASCII glyphs is the case
/// this refuses.
///
/// - Parameter advances: The sampled advances, in code-point order from
///   ``firstProbeCharacter``.
/// - Returns: True when the sample is non-empty, every advance is positive,
///   and every advance equals the first within `0.001`.
[[nodiscard]] bool isMonospace(AdvanceSpan advances);

/// The cell advance a character-spacing setting asks for.
///
/// Tracking moves the column pitch, not the ink: the glyph keeps the size and
/// the subpixel phase the face drew it at, and the cell it sits in grows or
/// shrinks around it. That is what makes it the horizontal twin of line
/// spacing rather than a scale.
///
/// Negative tracking is allowed because tightening a wide face is the reason
/// the setting exists, but a cell has to stay drawable: an advance at or below
/// zero is a grid with no columns. The floor is what the caller gets instead,
/// so a reader who drags the control to its end sees a tight grid rather than
/// an empty window.
///
/// Both arguments are in one unit and the result is in that unit — the floor
/// is one of it, which is a whole pixel for a caller working in atlas pixels
/// and a whole point for one working in the layout metric.
///
/// - Parameters:
///   - advance: The face's own cell advance.
///   - tracking: The tracking to fold in, in the same unit.
/// - Returns: The advance to lay the grid out on.
[[nodiscard]] double trackedCellAdvance(double advance, double tracking);

} // namespace Ubiquitty::Rendering::FontMetrics
