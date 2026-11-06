// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ImageQuads.h"

#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/Terminal.h"

#include <cmath>

namespace Ubiquitty::Rendering {

int collectImageQuads(const Ubiquitty::Terminal* terminal, FloatSize cellSizeInPixels, FloatPoint origin, float scale, int viewOffset, std::span<Image::ImageQuad> out, FloatSize drawableSizeInPixels, float presentationScale)
{
    if (!std::isfinite(presentationScale) || presentationScale <= 0)
        return 0;
    if (presentationScale != 1) {
        // Resolve in the original pixel domain, including crop and offsets,
        // then transform every edge together. Scaling only the cells leaves
        // intrinsic-size images and subcell offsets at their native size.
        const int count = collectImageQuads(terminal,
            { cellSizeInPixels.width / presentationScale, cellSizeInPixels.height / presentationScale }, {},
            scale / presentationScale, viewOffset, out,
            { drawableSizeInPixels.width / presentationScale, drawableSizeInPixels.height / presentationScale });
        for (auto& quad : out.first(static_cast<std::size_t>(count))) {
            quad.minXInPixels = origin.x + quad.minXInPixels * presentationScale;
            quad.maxXInPixels = origin.x + quad.maxXInPixels * presentationScale;
            quad.minYInPixels = origin.y + quad.minYInPixels * presentationScale;
            quad.maxYInPixels = origin.y + quad.maxYInPixels * presentationScale;
            if (presentationScale < 1)
                quad.filter = Image::ImageFilter::Smooth;
        }
        return count;
    }
    // Bound to scalars once, so the quad arithmetic below reads plain names.
    const float cellWidth = cellSizeInPixels.width;
    const float cellHeight = cellSizeInPixels.height;
    const float originX = origin.x;
    const float originY = origin.y;
    const float drawablePixelWidth = drawableSizeInPixels.width;
    const float drawablePixelHeight = drawableSizeInPixels.height;
    const int maxOut = static_cast<int>(out.size());
    if (!terminal)
        return 0;
    // Map the app-selected DECSET 7402 fit to the image fit (+ Stretch aspect /
    // Scale multiplier). Non-7402 keeps Anchored.
    Image::ImageFit imageFit = Image::ImageFit::Anchored;
    int fitAspectWidth = 0, fitAspectHeight = 0;
    int fitScaleMultiplier = 0;
    if (terminal->modes.fullscreenContent) {
        switch (terminal->fullscreenContentDisplay.fit) {
        case FullscreenContentFit::Stretch:
            imageFit = Image::ImageFit::Stretch;
            fitAspectWidth = terminal->fullscreenContentDisplay.aspectWidth;
            fitAspectHeight = terminal->fullscreenContentDisplay.aspectHeight;
            break;
        case FullscreenContentFit::Native:
            imageFit = Image::ImageFit::Native;
            break;
        case FullscreenContentFit::Scale:
            imageFit = Image::ImageFit::Scale;
            fitScaleMultiplier = terminal->fullscreenContentDisplay.scaleMultiplier;
            break;
        case FullscreenContentFit::Contain:
            imageFit = Image::ImageFit::ContainFit;
            break;
        }
    }
    int count = Image::resolveImagePlacements(
        { terminal->images.stored.data(), Image::maxTerminalImageCount },
        { terminal->images.placements.data(), Image::maxTerminalImagePlacementCount },
        { terminal->columns, terminal->rows }, { cellWidth, cellHeight }, { originX, originY },
        viewOffset, out,
        { imageFit, { drawablePixelWidth, drawablePixelHeight },
            { fitAspectWidth, fitAspectHeight }, fitScaleMultiplier },
        terminal->screens.isUsingAlternateScreen);
    // Kitty Unicode placeholders (U+10EEEE cells) produce one quad per cell.
    // hasPlaceholders short-circuits on a small image-array scan when no virtual
    // placement exists, so direct-placement clients skip the per-cell grid scan.
    if (count < maxOut && Ubiquitty::Kitty::hasPlaceholders(terminal, viewOffset))
        count += Ubiquitty::Kitty::resolvePlaceholders(terminal, cellSizeInPixels, origin, scale, viewOffset, out.subspan(static_cast<std::size_t>(count)));
    return count;
}

} // namespace Ubiquitty::Rendering
