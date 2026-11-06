// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphSlotCache.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty::Rendering {

/// One glyph of a run drawn glyph by glyph, for the rules beneath it.
///
/// Where the glyph's ink lies on screen and where its slot sits in the
/// proportional sheet, so a cell's skip-ink tap can be mapped onto the glyph
/// that inks it.
struct ProportionalGlyphInk {
    float inkLeftInPixels = 0.0f;
    float inkRightInPixels = 0.0f;
    /// The quad's left edge; the slot's first column lands here.
    float quadLeftInPixels = 0.0f;
    /// The row's top, in rows from the slot's top: the quad reaches above the
    /// cell by the sheet's margin, and the band fractions are the cell's.
    float cellTopInSlot = 0.0f;
    GlyphUV slot;
};

/// Row scratch the instance builder fills while a skip-ink rule runs under a
/// proportional run drawn glyph by glyph.
///
/// Caller-owned, so a renderer that keeps one across frames reuses its
/// storage instead of allocating per frame; ``InstanceBuffers/scratch`` hands
/// it to the build. The builder touches it only on a row that holds such a
/// claim, and a build given none keeps one of its own for the call. Nothing in
/// it outlives a row: the builder resets both members before it reads them.
struct CellInstanceScratch {
    /// The row's glyph inks, appended run by run.
    std::vector<ProportionalGlyphInk> proportionalGlyphInks;
    /// Per column, whether the claim covering it was drawn glyph by glyph.
    std::vector<std::uint8_t> proportionalPerGlyphClaims;
};

} // namespace Ubiquitty::Rendering
