// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/ProportionalStrike.h"

#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering::ProportionalOutline {

/// The bitmap tier's proportional face drawn from its outlines, at any size.
///
/// The tier's strikes (``ProportionalStrike``) hold Noto Sans at 12 px and
/// 24 px, light-hinted as FreeType draws it on Linux. A size they do not hold —
/// a run at an OSC 66 scale, on a DECDHL pair, on a DECDWL row, or on a grid
/// upscaled past two — is drawn from the face's own outlines, baked into Core
/// by Tools/gen-proportional-outlines, by the coverage a vector rasterizer
/// gives each pixel: the area of it the glyph covers, unhinted, the way
/// FreeType's smooth rasterizer draws an unhinted outline. So the face is
/// rasterised at size rather than a strike's pixels magnified, and every arm
/// draws the same bytes.

/// Whether the face has an outline for a code point.
///
/// - Parameters:
///   - face: Regular or Bold.
///   - codePoint: The code point.
/// - Returns: True when ``rasterise`` can draw it.
[[nodiscard]] bool hasGlyph(ProportionalStrike::Face face, std::uint32_t codePoint);

/// Draws one glyph at a size into a coverage plane, keeping the greater of
/// its coverage and what the plane holds, so glyphs drawn at fractional pens
/// share their edge pixels rather than one notching the other.
///
/// - Parameters:
///   - face: Regular or Bold.
///   - codePoint: The code point.
///   - pixelsPerEm: The face's em in device pixels down.
///   - horizontalScale: The factor across past the em: 2 for a DECDWL row.
///   - penX: The glyph's origin, in pixels from the plane's left, fractional.
///   - baselineY: The baseline, in pixel rows from the plane's top.
///   - width: The plane's width.
///   - height: The plane's height.
///   - coverage: The plane, `width * height` bytes, row-major.
///   - slant: The shear across per unit of height above the baseline; zero
///     draws the face upright, a positive value leans it right as an oblique.
/// - Returns: Whether the face had the glyph; a glyph with no contours, a
///   space, draws nothing and answers true.
bool rasterise(ProportionalStrike::Face face, std::uint32_t codePoint, double pixelsPerEm, double horizontalScale,
    double penX, double baselineY, int width, int height, std::span<std::uint8_t> coverage, double slant = 0.0);

} // namespace Ubiquitty::Rendering::ProportionalOutline
