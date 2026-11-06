// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>

// The participation decision and its criterion, in a header low enough for
// both the seam types and the slot-geometry record to name: the enum rides
// GlyphSlotGeometry from the arm that probed the font into BuildInputs, and
// GlyphBackendTypes.h re-exports it to every backend.

namespace Ubiquitty::Rendering {

/// Which stretches of a row are worth shaping, decided by what the font does
/// to plain text.
///
/// ``OperatorRuns`` is the classic economy: letters and digits shape to
/// themselves in a FiraCode-class font, so only a stretch holding an operator
/// character can gain anything from a strip, and every other row skips the
/// shaper entirely. That economy is CORRECT only while its premise holds.
///
/// A texture-healing font (Monaspace-class) swaps ordinary letters for
/// contextual alternates, so under the operator gate the same words render
/// two ways — healed where an operator happens to share the stretch, default
/// forms where none does — and the choice reads as random to the user because
/// the deciding neighbour is invisible. ``AllEligibleRuns`` shapes every
/// eligible stretch of two or more cells, so one font produces one rendering
/// of one text wherever it appears.
///
/// Seam contract, not a backend tunable, for the reason the run cap and the
/// context length are: two backends gating differently would draw one row two
/// ways.
enum class LigatureParticipation : std::uint8_t {
    OperatorRuns,
    AllEligibleRuns,
};

/// Decides participation from the healing probe's two glyph mappings.
///
/// `shapedWithStripFont` is ``ligatureHealingProbe`` shaped as one run under
/// the font strips rasterise with, default features included; `isolated` is
/// the same characters mapped one at a time, which is what the per-cell mono
/// path draws. Any disagreement means a shaped stretch and its unshaped
/// neighbour would render the same letters differently — the inconsistency
/// the gate exists to prevent — so the answer is ``AllEligibleRuns``.
///
/// A COUNT mismatch is also a disagreement: a font that ligates inside the
/// letter probe has substituted, and substitution is what the probe asks
/// about. The empty probe answers ``OperatorRuns`` — a backend that cannot
/// shape reports nothing and keeps the economy.
///
/// One criterion in Core rather than a probe per arm, because two arms
/// classifying one font two ways would split rows differently in the exact
/// frames a comparison oracle holds them together.
///
/// - Parameters:
///   - shapedWithStripFont: Glyph ids from shaping the probe as one run.
///   - isolated: Glyph ids from mapping the probe's characters one at a time.
/// - Returns: The participation the row walk must use for this font.
inline LigatureParticipation decideLigatureParticipation(
    std::span<const std::uint32_t> shapedWithStripFont,
    std::span<const std::uint32_t> isolated)
{
    if (shapedWithStripFont.empty() || isolated.empty())
        return LigatureParticipation::OperatorRuns;
    if (shapedWithStripFont.size() != isolated.size())
        return LigatureParticipation::AllEligibleRuns;
    for (std::size_t i = 0; i < isolated.size(); ++i) {
        if (shapedWithStripFont[i] != isolated[i])
            return LigatureParticipation::AllEligibleRuns;
    }
    return LigatureParticipation::OperatorRuns;
}

/// The decision above, spelled for callers the span form cannot reach.
///
/// Swift sees no instantiation of the span parameters, so this is the
/// spelling the interop importer hands it. One criterion either way: this
/// overload is implemented on the span form, not beside it.
///
/// - Parameters:
///   - shapedWithStripFont: Glyph ids from shaping the probe as one run.
///   - shapedCount: How many entries `shapedWithStripFont` holds.
///   - isolated: Glyph ids from mapping the probe one character at a time.
///   - isolatedCount: How many entries `isolated` holds.
/// - Returns: The participation the row walk must use for this font.
inline LigatureParticipation decideLigatureParticipation(
    const std::uint32_t* shapedWithStripFont, int shapedCount,
    const std::uint32_t* isolated, int isolatedCount)
{
    return decideLigatureParticipation(
        std::span<const std::uint32_t>(shapedWithStripFont, static_cast<std::size_t>(shapedCount > 0 ? shapedCount : 0)),
        std::span<const std::uint32_t>(isolated, static_cast<std::size_t>(isolatedCount > 0 ? isolatedCount : 0)));
}

} // namespace Ubiquitty::Rendering
