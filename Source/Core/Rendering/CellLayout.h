// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Rendering/CellLayout.h — where the baseline sits in a cell once
// line spacing is added.
//
// The spacing is split above and below the type. Added below, the glyph rides
// high in its cell and the distance from a descender to its underline SHRINKS
// as the line height is raised — the setting taking away the room it exists to
// add.
//
// A backend supplies the ascent and descent; how they and the spacing become a
// cell is here, so two faces in one frontend cannot round the split differently.
//

#include "Base/SwiftInterop.h"

namespace Ubiquitty::Rendering {

/// A cell's vertical layout, in device pixels.
struct UBIQUITTY_ESCAPABLE CellLayout {
    /// Ascent + descent + the whole spacing.
    float cellHeight = 0.0f;
    /// Cell top to the baseline: the ascent plus the spacing above it.
    float baselineFromTop = 0.0f;
    /// The spacing placed above the type. The overflow pass needs it apart from
    /// the baseline to know how much of the cell is leading rather than glyph.
    float lineGapTop = 0.0f;
};

/// Places the baseline for a face and a line spacing.
///
/// Halving is integer, so an odd spacing puts the spare pixel below. Every face
/// in a frontend must round it the same way or a fallback glyph sits a pixel off
/// the baseline its neighbours share.
///
/// - Parameters:
///   - ascentInPixels: Baseline to the top of the type.
///   - descentInPixels: Baseline to the bottom, positive.
///   - lineSpacingInPixels: Extra rows of space; zero or less adds nothing.
/// - Returns: The layout.
[[nodiscard]] inline CellLayout cellLayout(float ascentInPixels, float descentInPixels, int lineSpacingInPixels)
{
    const int spacing = lineSpacingInPixels > 0 ? lineSpacingInPixels : 0;
    const float leadingAbove = static_cast<float>(spacing / 2);

    CellLayout layout;
    layout.cellHeight = ascentInPixels + descentInPixels + static_cast<float>(spacing);
    layout.baselineFromTop = ascentInPixels + leadingAbove;
    layout.lineGapTop = leadingAbove;
    return layout;
}

/// Resolves the x-height a strike rides.
///
/// The strike is drawn at `baseline - xHeight / 2`, so a face that reports
/// nothing must be given a stand-in rather than zero, which would put the rule
/// on the baseline.
///
/// Half the ascent is the stand-in. It is not a measurement and is not meant to
/// be one — it places the rule through the middle of the type, which is where a
/// strike belongs when the face declines to say where its lowercase ends.
///
/// - Parameters:
///   - measuredInPixels: What the face reported, or zero or less when it
///     reported nothing usable.
///   - ascentInPixels: Baseline to the top of the type.
/// - Returns: The x-height to place the strike by.
[[nodiscard]] inline float xHeightForStrike(float measuredInPixels, float ascentInPixels)
{
    return measuredInPixels > 0.0f ? measuredInPixels : ascentInPixels * 0.5f;
}

} // namespace Ubiquitty::Rendering
