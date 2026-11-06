// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// BoxStrokeProfile.h — the font's own box-stroke geometry, measured from the
// glyphs it ships, so the procedural fallback draws the characters it lacks
// exactly as the font designer drew the ones it has.
//
// A face rarely carries the whole box repertoire: singles and doubles are
// common, the single/double junctions and the terminal-graphics family
// (U+23B8..U+23CC) much less so. The characters the font lacks fall to the
// procedural path, and a procedural stroke derived from the em alone sits
// beside the font's strokes at its own thickness and its own centreline — a
// healed junction visibly patched into the table it completes. Measuring the
// references the font does ship and drawing the missing characters from those
// bands is what makes the two sources stitch.
//
// Core owns the measurement policy (which reference answers which class, how
// runs fold to a centre and a thickness) and the consumption (buildBoxGlyph);
// an arm contributes only rasterised reference bitmaps, which is the one
// thing the platform shaper alone can produce.

#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering {
struct GlyphRasterizer;
}

namespace Ubiquitty::Rendering::BoxDrawing {

/// One measured stroke band: where a reference glyph's rule sits and how
/// thick it is, in unit cell coordinates so a profile is scale-free.
///
/// Thickness is the coverage INTEGRAL and the centre its centroid, because
/// they describe the weight the eye sees: an antialiased 1.5px rule whose
/// two edge rows both clear a threshold would measure 2px by extent, and a
/// consumer painting two full rows beside the font's soft rule draws the
/// healed character heavier than the table it completes. A hard-edged
/// reference reports the same numbers under both definitions.
struct BoxStrokeBand {
    bool isValid = false;
    /// The band's ink centroid along the scanned axis, 0..1 across the cell.
    float centerInCell = 0.0f;
    /// The band's coverage integral across the cell, 0..1 of the cell extent.
    float thicknessInCell = 0.0f;
};

/// A measured double-rule band: two rails and the gap between them.
struct BoxDoubleBand {
    bool isValid = false;
    /// The midpoint between the two rails along the scanned axis, 0..1.
    float centerInCell = 0.0f;
    float railThicknessInCell = 0.0f;
    float gapInCell = 0.0f;
};

/// The font's measured box strokes, one band per reference glyph.
///
/// Horizontal bands are scanned from `─ ━ ═` (rows carry the ink), vertical
/// bands from `│ ┃ ║` (columns carry it). Thickness is kept per orientation
/// because a face may cut its rules differently on the two axes. A band a
/// face does not ship stays invalid, and the consumer falls back to the
/// em-derived weight for exactly that class — a profile is a refinement per
/// stroke class, never all-or-nothing.
struct BoxStrokeProfile {
    /// Spelled out for the Swift importer: without it Swift synthesizes a
    /// zero-initializing `init()` it deprecates, warning on every use; with
    /// it the members' own initializers run.
    BoxStrokeProfile() = default;

    BoxStrokeBand lightHorizontal;
    BoxStrokeBand lightVertical;
    BoxStrokeBand heavyHorizontal;
    BoxStrokeBand heavyVertical;
    BoxDoubleBand doubleHorizontal;
    BoxDoubleBand doubleVertical;
};

/// The reference code points a profile is measured from.
///
/// An arm renders each of these at cell size and hands the bitmaps to
/// ``foldBoxReferenceScan``; which band each one feeds is Core's mapping, so
/// the arm never learns it. `─ │ ━ ┃ ═ ║`, in that order.
///
/// - Returns: The six reference code points.
[[nodiscard]] std::span<const std::uint32_t> boxStrokeReferenceCodePoints();

/// Folds one rendered reference bitmap into the profile.
///
/// Projects the coverage onto the axis the reference's ink crosses — a row
/// (or column) is inked when any pixel of its middle third reaches half
/// coverage; the middle third because end caps and corner antialiasing say
/// nothing about the rule's body — and folds the contiguous inked runs into
/// the band the code point names. One run is a single stroke; two runs of
/// like thickness are a double rule's rails. Anything else (no ink, three or
/// more runs, wildly unequal rails) leaves the band invalid rather than
/// mis-measure — an invalid band falls back to the em rule, a wrong one
/// mis-draws every healed character.
///
/// - Parameters:
///   - profile: The profile the scan lands in.
///   - codePoint: Which reference `coverage` renders; an unlisted code point
///     folds nothing.
///   - coverage: The glyph's R8 bitmap, row-major, `width * height` bytes.
///   - width: The bitmap's width in pixels.
///   - height: The bitmap's height in pixels.
void foldBoxReferenceScan(BoxStrokeProfile* profile, std::uint32_t codePoint,
    std::span<const std::uint8_t> coverage, int width, int height);

/// The fold above, spelled for callers the span form cannot reach.
///
/// Swift sees no instantiation of the span parameter, so this is the
/// spelling the interop importer hands it. One fold either way: this
/// overload is implemented on the span form, not beside it.
///
/// - Parameters:
///   - profile: The profile the scan lands in.
///   - codePoint: Which reference `coverage` renders.
///   - coverage: The glyph's R8 bitmap, row-major.
///   - count: How many bytes `coverage` holds.
///   - width: The bitmap's width in pixels.
///   - height: The bitmap's height in pixels.
inline void foldBoxReferenceScan(BoxStrokeProfile* profile, std::uint32_t codePoint,
    const std::uint8_t* coverage, int count, int width, int height)
{
    foldBoxReferenceScan(profile, codePoint,
        std::span<const std::uint8_t>(coverage, static_cast<std::size_t>(count > 0 ? count : 0)), width, height);
}

/// Measures a face's whole profile through the rasterizer seam.
///
/// Drives `rasterizeGlyph` for each reference over a `width * height` cell
/// bitmap and folds each scan; a glyph the face cannot draw leaves its band
/// invalid. For an arm whose rasterisation does not pass through
/// ``GlyphRasterizer``, ``foldBoxReferenceScan`` is the seam instead: the
/// arm renders each reference itself and hands over the bytes.
///
/// - Parameters:
///   - rasterizer: The chain the box references would otherwise render from,
///     fallbacks included — the profile describes what reaches the screen,
///     not what one font file holds.
///   - width: The cell width the references are drawn at, in pixels.
///   - height: The cell height, in pixels.
/// - Returns: The measured profile; unmeasurable classes are invalid inside.
[[nodiscard]] BoxStrokeProfile measureBoxStrokeProfile(GlyphRasterizer& rasterizer, int width, int height);

} // namespace Ubiquitty::Rendering::BoxDrawing
