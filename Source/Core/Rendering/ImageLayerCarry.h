// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/UnicodePlaceholders.h"

// What a frame does with the image layer it resolved — or failed to.
//
// A frontend resolves the model's placements into quads once per frame and
// keeps the last good set, because two situations produce an empty layer and
// they mean opposite things.
//
// A frame that could not resolve has no answer yet: the cell geometry is not
// primed (the first frame after a resize), or there is no store or terminal to
// resolve against. Blanking there flickers a picture that has not gone
// anywhere, so the previous layer is carried across the gap.
//
// A frame whose model placed nothing on the viewport has an answer, and the
// answer is that nothing is drawn: every placement clipped outside it, which is
// what an image scrolling off the top looks like. A frame whose model placed
// something the frontend cannot draw yet is a gap again — the texture upload is
// behind, and the picture has not moved. Carrying there pins the
// departed picture to the pixels it last occupied — it stays on screen while
// the text moves out from under it, and the next image lands beside it, so a
// session that shows several accumulates one stuck copy per image.
//
// The rule is one decision, and it is the same on every backend, so it lives
// here rather than being spelled once per frontend.

namespace Ubiquitty::Rendering {

/// What a frontend does with the image layer for the frame it just built.
enum class ImageLayerAction {
    /// Draw the layer this frame resolved.
    UseResolved,
    /// Draw nothing: the resolve ran and found nothing on screen.
    DrawNothing,
    /// Draw the previous layer: this frame could not resolve at all.
    CarryPrevious
};

/// What this frame's resolve produced.
struct ImageLayerFrame {
    /// Whether the frame could run a resolve (geometry primed, store present).
    bool canResolve = false;
    /// How many placements the model resolved onto the viewport, before the
    /// frontend bound textures to them. Zero means every placement clipped
    /// outside the viewport.
    int resolvedPlacements = 0;
    /// How many of those the frontend could actually draw. Fewer than
    /// `resolvedPlacements` means a texture is not ready yet, which is a gap
    /// rather than an answer: the picture is on screen by the model's reckoning
    /// and only the upload is behind.
    int drawableQuads = 0;
    /// Whether the model holds any placement at all this frame.
    bool hasPlacements = false;
    /// Whether a live resize is in progress, where an image application's
    /// per-frame delete-and-replace shows up as a momentary zero.
    bool isHoldingForResize = false;
    /// Whether a previous layer is available to carry.
    bool hasCarriedLayer = false;
};

/// Whether the model places any image on this frame's viewport.
///
/// Two protocols place images by different means — a sixel or kitty placement
/// is a record in the image store, a kitty Unicode placeholder is a run of
/// cells that names an image — and a frame that asked only one of them would
/// read the other's picture as a delete. This is the one question
/// `ImageLayerFrame::hasPlacements` is answered with, so the two arms cannot
/// answer it differently.
///
/// - Parameters:
///   - terminal: The model, read under its lock. Null places nothing — a
///     frontend between terminals has an answer rather than a gap, and
///     carrying there would pin the departed pane's picture over the new one.
///   - viewOffset: Rows the viewport is scrolled back; placeholders are
///     looked for on the rows the viewport shows.
/// - Returns: Whether anything placed an image this frame.
[[nodiscard]] inline bool hasImagePlacements(const Ubiquitty::Terminal* terminal, int viewOffset)
{
    if (!terminal)
        return false;
    return Image::placementCount(terminal) > 0 || Kitty::hasPlaceholders(terminal, viewOffset);
}

/// Decides what the frame draws.
///
/// - Parameter frame: What the frame's resolve produced.
/// - Returns: The layer to draw.
[[nodiscard]] constexpr ImageLayerAction imageLayerAction(const ImageLayerFrame& frame)
{
    if (!frame.hasPlacements) {
        // No placements at all. During a live resize this can be the gap
        // between an application's delete and its re-place, and carrying holds
        // the picture steady; otherwise it is a real delete and the layer goes.
        return (frame.isHoldingForResize && frame.hasCarriedLayer)
            ? ImageLayerAction::CarryPrevious
            : ImageLayerAction::DrawNothing;
    }
    if (!frame.canResolve)
        return frame.hasCarriedLayer ? ImageLayerAction::CarryPrevious : ImageLayerAction::DrawNothing;
    if (frame.drawableQuads > 0)
        return ImageLayerAction::UseResolved;
    // Nothing to draw, and which of the two reasons decides the frame. The
    // model placing something on the viewport that the frontend cannot draw yet
    // is an upload in flight; the model placing nothing there is the picture
    // having left the screen.
    if (frame.resolvedPlacements > 0 && frame.hasCarriedLayer)
        return ImageLayerAction::CarryPrevious;
    return ImageLayerAction::DrawNothing;
}

} // namespace Ubiquitty::Rendering
