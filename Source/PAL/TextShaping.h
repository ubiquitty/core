// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// TextShaping.h — what a platform's shaper answers for a proportional run
// (Docs/proportional-text.md), and nothing it does not.
//
// In the PAL contract rather than the renderer because two seams speak it: the
// renderer's glyph backend, which draws the glyphs, and the model's text
// measurer, which lays a run out at write time so the cells can hold the text
// where its ink is. Both hand the answer to Core (Rendering/ProportionalRun.h),
// and a frontend that can shape a run gains everything Core derives from it —
// alignment, justification, cells, hit-testing — by answering this once.

#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty {

/// One glyph a shaper chose, in its face's own numbering.
struct UBIQUITTY_ESCAPABLE ProportionalShapedGlyph {
    /// The face's glyph identifier; zero is `.notdef`.
    std::uint32_t glyph = 0;
    /// The pen's advance after this glyph, in the pixels the face is set in.
    float advanceInPixels = 0.0f;
    /// The glyph's offset from the pen, x right and y up.
    float offsetXInPixels = 0.0f;
    float offsetYInPixels = 0.0f;
    /// The index in the run's text of the code point this glyph begins at. A
    /// ligature's glyphs share one, which is how ink maps back to text after
    /// substitution has changed the count.
    std::uint16_t cluster = 0;
};

/// The face a run is set in, as the pen asks for it: SGR 1 bold and SGR 3
/// italic as two independent axes, and the SGR 1:n variable weight.
///
/// Carried to the frontend's shaper so the layout Core derives a run's
/// cells, letters and pointer positions from is the layout the frontend
/// draws: a bold face is wider than its regular, and a run measured in one
/// and drawn in the other puts every letter in a cell of its own choosing.
struct UBIQUITTY_ESCAPABLE ProportionalRunStyle {
    /// Two bits: 1 bold, 2 italic, in the order `Rendering::FontFace` packs
    /// them.
    std::uint8_t faceBits = 0;
    /// The variable-font weight step, 0 for the face's own and 1 through 7
    /// for the `wght` axis.
    std::uint32_t weight = 0;
};

} // namespace Ubiquitty
