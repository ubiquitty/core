// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// BackgroundImageLayout.cpp — see BackgroundImageLayout.h for the contract.
// Pure float math, no heap, no platform headers.
//

#include "Core/Rendering/BackgroundImageLayout.h"

namespace Ubiquitty::Rendering {

BackgroundImagePlacement backgroundImagePlacement(BackgroundImageMode mode,
    float viewWidthInPixels,
    float viewHeightInPixels,
    float imageWidthInPixels,
    float imageHeightInPixels)
{
    BackgroundImagePlacement placement;
    if (viewWidthInPixels <= 0.0f || viewHeightInPixels <= 0.0f)
        return placement;
    if (imageWidthInPixels <= 0.0f || imageHeightInPixels <= 0.0f)
        return placement;

    placement.rightInPixels = viewWidthInPixels;
    placement.bottomInPixels = viewHeightInPixels;

    switch (mode) {
    case BackgroundImageMode::Stretch:
        // The whole view showing the whole image, aspect abandoned, which is
        // what the default placement above already spells.
        break;
    case BackgroundImageMode::Fit: {
        const float scale = (viewWidthInPixels / imageWidthInPixels < viewHeightInPixels / imageHeightInPixels)
            ? viewWidthInPixels / imageWidthInPixels
            : viewHeightInPixels / imageHeightInPixels;
        const float drawWidth = imageWidthInPixels * scale;
        const float drawHeight = imageHeightInPixels * scale;
        placement.leftInPixels = (viewWidthInPixels - drawWidth) * 0.5f;
        placement.topInPixels = (viewHeightInPixels - drawHeight) * 0.5f;
        placement.rightInPixels = placement.leftInPixels + drawWidth;
        placement.bottomInPixels = placement.topInPixels + drawHeight;
        break;
    }
    case BackgroundImageMode::Center: {
        // Each axis decided on its own: an image wider than the view but
        // shorter than it is cropped horizontally and centred vertically in the
        // same frame, so a single branch on total size would be wrong.
        if (imageWidthInPixels <= viewWidthInPixels) {
            placement.leftInPixels = (viewWidthInPixels - imageWidthInPixels) * 0.5f;
            placement.rightInPixels = placement.leftInPixels + imageWidthInPixels;
        } else {
            const float halfWindow = (viewWidthInPixels * 0.5f) / imageWidthInPixels;
            placement.u0 = 0.5f - halfWindow;
            placement.u1 = 0.5f + halfWindow;
        }
        if (imageHeightInPixels <= viewHeightInPixels) {
            placement.topInPixels = (viewHeightInPixels - imageHeightInPixels) * 0.5f;
            placement.bottomInPixels = placement.topInPixels + imageHeightInPixels;
        } else {
            const float halfWindow = (viewHeightInPixels * 0.5f) / imageHeightInPixels;
            placement.v0 = 0.5f - halfWindow;
            placement.v1 = 0.5f + halfWindow;
        }
        break;
    }
    case BackgroundImageMode::Tile:
        // Coordinates past 1 are the repetition: one quad over the whole view
        // asking the sampler for as many copies as fit at native size.
        placement.u1 = viewWidthInPixels / imageWidthInPixels;
        placement.v1 = viewHeightInPixels / imageHeightInPixels;
        placement.shouldRepeat = true;
        break;
    case BackgroundImageMode::Fill: {
        // Scale to cover, then narrow the coordinates to the visible window so
        // the overflow is cropped rather than squashed.
        const float scale = (viewWidthInPixels / imageWidthInPixels > viewHeightInPixels / imageHeightInPixels)
            ? viewWidthInPixels / imageWidthInPixels
            : viewHeightInPixels / imageHeightInPixels;
        const float coverWidth = imageWidthInPixels * scale;
        const float coverHeight = imageHeightInPixels * scale;
        const float halfWindowU = (viewWidthInPixels / coverWidth) * 0.5f;
        const float halfWindowV = (viewHeightInPixels / coverHeight) * 0.5f;
        placement.u0 = 0.5f - halfWindowU;
        placement.v0 = 0.5f - halfWindowV;
        placement.u1 = 0.5f + halfWindowU;
        placement.v1 = 0.5f + halfWindowV;
        break;
    }
    }
    return placement;
}

} // namespace Ubiquitty::Rendering
