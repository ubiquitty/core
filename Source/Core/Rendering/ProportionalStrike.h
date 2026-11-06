// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Rendering::ProportionalStrike {

/// The bitmap tier's proportional face: Noto Sans, Regular and Bold,
/// pre-rendered at 12 px and 24 px on Unifont's cell of 16 rows per scale
/// with the baseline on Unifont's row, baked into Core by
/// Tools/gen-proportional-strike. What it records is what the Linux arm's
/// FreeType and HarfBuzz produce at that size: advances and pair kerning in
/// 1/64 px, and a light-hinted raster of each glyph at every subpixel phase
/// Core places at, so a run drawn from it is that arm's text with the same
/// bytes on every arm.
inline constexpr int boxRowsPerScale = 16;
/// The face's em at scale 1, in pixels: the 12 px strike's size.
inline constexpr int emPixelsPerScale = 12;
inline constexpr float baselineRowsPerScale = 13.5f;
/// The unit of the strike's advances and kerns: 1/64 px, as HarfBuzz answers.
inline constexpr int unitsPerPixel = 64;

/// One glyph of a strike at one phase: its coverage, row-major
/// `width * height` bytes, and where it sits — the pen offset to its first
/// column, the rows from the baseline up to its first row, and the advance
/// to the next pen in 1/64 px. `present` is false for a code point the face
/// has no glyph for, and the other fields are then zero.
struct Glyph {
    const std::uint8_t* coverage = nullptr;
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    int advanceInUnits = 0;
    bool present = false;
};

/// The face index a strike is keyed by: Regular or Bold; italic draws upright.
enum class Face : int {
    Regular = 0,
    Bold = 1,
};

/// The subpixel phases a strike holds a raster for.
///
/// - Parameter scale: The strike's scale, 1 or 2; any other value answers zero.
/// - Returns: The phase count, or zero for a scale the strike lacks.
[[nodiscard]] int phaseCount(int scale);

/// Looks a code point up in one strike at one phase.
///
/// - Parameters:
///   - face: Regular or Bold.
///   - scale: The strike's scale, 1 or 2; any other value answers absent.
///   - codePoint: The code point.
///   - phase: The pen's subpixel phase, in `[0, phaseCount(scale))`.
/// - Returns: The glyph, `present` false when the strike has none.
[[nodiscard]] Glyph lookup(Face face, int scale, std::uint32_t codePoint, int phase);

/// The kerning the strike applies between two code points, in 1/64 px at
/// the strike's scale, added to the first glyph's advance.
///
/// - Parameters:
///   - face: Regular or Bold.
///   - scale: The strike's scale, 1 or 2.
///   - first: The code point on the left.
///   - second: The code point on the right.
/// - Returns: The kern, zero for a pair the strike does not kern.
[[nodiscard]] int kern(Face face, int scale, std::uint32_t first, std::uint32_t second);

/// The rows the strike's ink reaches past Unifont's cell, above or below,
/// the larger of the two over both faces; a strip slot grows by it on both
/// sides so a descender is drawn rather than cut at the cell.
///
/// - Parameter scale: The strike's scale, 1 or 2.
/// - Returns: The rows, zero for a scale the strike lacks.
[[nodiscard]] int overhangInPixels(int scale);

} // namespace Ubiquitty::Rendering::ProportionalStrike
