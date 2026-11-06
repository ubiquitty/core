// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Core/Image/ImagePlacement.h"
#include "Forward.h"
#include <span>

// Every image quad a frame draws, from both sources that produce them: the
// grid placements a Terminal stores, and the kitty Unicode-placeholder cells
// that carry their position in the glyph stream instead.
//
// The composition lives here rather than in Core/Image because it is the one
// thing that needs both modules at once. Image is the model and knows nothing
// of a protocol; Kitty is a decoder backend and already depends on Image. A
// composing entry point inside Image would point that dependency back the way
// it came, which is the Image <-> Kitty cycle in its entirety.

namespace Ubiquitty::Rendering {

/// Resolves every image quad visible in the current frame.
///
/// Combines the grid placements stored on `terminal` with the kitty Unicode
/// placeholders resolved from its cells, applying the DECSET 7402 fullscreen
/// fit when that mode is active. Order is unspecified: a caller that needs
/// stacking order sorts the result by `ImageQuad::zIndex`.
///
/// - Parameters:
///   - terminal: The terminal to read placements and placeholder cells from.
///     A null terminal yields no quads.
///   - cellSizeInPixels: Cell dimensions after local presentation scaling.
///   - origin: The presented grid's origin in device pixels.
///   - scale: Points to device pixels, including local presentation scaling.
///   - viewOffset: The scrollback offset; 0 is the live bottom.
///   - out: Destination for the resolved quads.
///   - drawableSizeInPixels: Drawable dimensions, for the fullscreen fits.
///   - presentationScale: Uniform local scaling of cells and intrinsic image pixels.
/// - Returns: The number of quads written to `out`.
int collectImageQuads(const Ubiquitty::Terminal* terminal, FloatSize cellSizeInPixels, FloatPoint origin, float scale, int viewOffset, std::span<Image::ImageQuad> out, FloatSize drawableSizeInPixels = {}, float presentationScale = 1.0f);

/// The destination span, named for the language seam: Swift imports a span
/// only through an alias.
using ImageQuadSpan = std::span<Image::ImageQuad>;

/// ``collectImageQuads`` for a caller across the language seam, which passes
/// the span through its alias and always names the drawable.
///
/// - Parameters:
///   - terminal: The terminal to read placements from.
///   - cellSizeInPixels: One cell's device-pixel size.
///   - origin: The grid's top-left in the drawable.
///   - scale: Points to device pixels, including local presentation scaling.
///   - viewOffset: Scrollback rows above the live tail; 0 is the tail.
///   - out: Destination for the resolved quads.
///   - drawableSizeInPixels: The drawable, for the fullscreen fits.
///   - presentationScale: Uniform local scaling of cells and intrinsic image pixels.
/// - Returns: The number of quads written, in the protocol's draw order.
inline int collectImageQuadsOrdered(const Ubiquitty::Terminal* terminal, FloatSize cellSizeInPixels, FloatPoint origin, float scale, int viewOffset, ImageQuadSpan out, FloatSize drawableSizeInPixels, float presentationScale = 1.0f)
{
    if (!terminal)
        return 0;
    const int count = collectImageQuads(terminal, cellSizeInPixels, origin, scale, viewOffset, out, drawableSizeInPixels, presentationScale);
    (void)Image::orderImageQuadsForDrawing(out.first(static_cast<std::size_t>(count > 0 ? count : 0)));
    return count;
}

} // namespace Ubiquitty::Rendering
