// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphRasterizer.h"

namespace Ubiquitty::Rendering {

/// The rows of GNU Unifont's `[` that hold ink, first and one past last, in
/// the face's sixteen-row box. The bracket is what the prompt marks are
/// drawn from, and its ink bounds place them. Two constants rather than a
/// read of the bitmap, so a unit that never draws — the daemon's frame
/// capture — sizes the mark without the nine megabytes the glyphs occupy;
/// test_unifont_sizing rasterizes the glyph and holds the pair to it, so a
/// bitmap whose bracket moves fails there rather than drifting here.
inline constexpr int unifontBracketFirstInkRow = 3;
inline constexpr int unifontBracketInkRowEnd = 15;

/// The bitmap face's metrics at an integer scale: the sixteen-row box at the
/// scale with the baseline 13.5 rows down it, the gap above from the line
/// spacing, and the bracket's ink distances from that baseline. Everything
/// the bitmap tier sizes its prompt marks from, on every arm.
///
/// - Parameters:
///   - scale: Device pixels per Unifont pixel, one or more.
///   - lineSpacingInPixels: The extra rows the cell adds, split above and
///     below with the same integer halving the cell metrics use.
/// - Returns: The face metrics.
[[nodiscard]] FaceMetrics unifontFaceMetricsForScale(int scale, int lineSpacingInPixels);

} // namespace Ubiquitty::Rendering
