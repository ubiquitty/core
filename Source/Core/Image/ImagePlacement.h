// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"

#include <span>

// Frontend-agnostic resolution of the Terminal's live terminal-graphics
// placements (kitty / sixel / iTerm, one placement model) into device-pixel
// quads sampling a sub-rectangle of a source image, scrolled and clipped with the
// grid. Uploading each image to a GPU texture stays in the frontend, which maps
// ImageQuad::imageID to its own texture handle.

#include "Base/Types.h"

#include <cstdint>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Image {
struct TerminalImage;
struct TerminalImagePlacement;
}

namespace Ubiquitty::Image {

/// How a quad's texels reach the device pixels it covers.
///
/// One policy for every arm that draws images, decided where the quad is
/// resolved, so no arm can filter a picture another draws sharp.
enum class ImageFilter : std::uint8_t {
    /// One source texel per device pixel block, unblended. Only a whole-number
    /// enlargement on both axes — 1:1 included — takes it: each texel then
    /// covers an exact block of pixels, and pixel art and sixel bands keep
    /// their edges.
    Nearest,
    /// Bilinear within a mip level and blended between the two levels that
    /// bracket the minification. Every other scale takes it: nearest at a
    /// fractional scale drops and doubles source rows and columns, which is
    /// the stair-stepping a scaled curve or diagonal shows.
    Smooth,
};

/// The filter a quad of `destination` device pixels over a source rectangle of
/// `sourceWidth` by `sourceHeight` texels takes.
///
/// - Parameters:
///   - destinationWidth: The quad's unclipped width, in device pixels.
///   - destinationHeight: The quad's unclipped height, in device pixels.
///   - sourceWidth: The source rectangle's width, in texels.
///   - sourceHeight: The source rectangle's height, in texels.
/// - Returns: `Nearest` when both axes enlarge by a whole number to within a
///   hundredth of a pixel across the quad, `Smooth` otherwise.
[[nodiscard]] ImageFilter imageFilterForScale(float destinationWidth, float destinationHeight, int sourceWidth, int sourceHeight);

/// The mip level a filtered quad samples, as a real number: zero is the
/// source, one is half its size.
///
/// Every arm reads the same level for the same quad, because a quad is
/// axis-aligned and its minification is the same at every pixel; the GPU's own
/// derivative estimate is that number too.
///
/// - Parameters:
///   - texelsAcross: Source texels the quad spans horizontally.
///   - texelsDown: Source texels the quad spans vertically.
///   - pixelsAcross: Device pixels the quad covers horizontally.
///   - pixelsDown: Device pixels the quad covers vertically.
/// - Returns: The base-two logarithm of the larger of the two texel-per-pixel
///   ratios, and zero for a quad that enlarges on both axes.
[[nodiscard]] float imageLevelOfDetail(float texelsAcross, float texelsDown, float pixelsAcross, float pixelsDown);

/// How many mip levels an image of the given size carries, down to one texel.
///
/// - Parameters:
///   - width: Level zero's width, in texels.
///   - height: Level zero's height, in texels.
/// - Returns: At least one.
[[nodiscard]] int imageMipLevelCount(int width, int height);

/// Gives every fully transparent texel the colour of the nearest texel that is
/// not, leaving every alpha as it was.
///
/// A filter blends a texel's colour with its neighbours' whatever their alpha,
/// so a transparent texel's colour — black, as decoders write it — would bleed
/// into the edge of a straight-alpha picture as a dark fringe. Its colour is
/// never seen on its own, so taking the edge's colour instead changes nothing
/// a nearest draw shows and removes the fringe a filtered one would.
///
/// - Parameters:
///   - rgba: The texels, four bytes each, straight alpha, rewritten in place.
///   - width: Texels across.
///   - height: Texels down.
void bleedIntoTransparentTexels(std::uint8_t* rgba, int width, int height);

/// Writes the next mip level down from one level.
///
/// Each texel is the 2x2 block above it — the last row or column repeated
/// where the level is odd — averaged with each colour weighted by its alpha,
/// then the transparent texels of the result take their neighbours' colour as
/// ``bleedIntoTransparentTexels`` gives them.
///
/// - Parameters:
///   - source: The level above, four bytes a texel, straight alpha.
///   - sourceWidth: Its width.
///   - sourceHeight: Its height.
///   - destination: The level below: `max(1, sourceWidth / 2)` by
///     `max(1, sourceHeight / 2)` texels, four bytes each.
void downsampleImageMipLevel(const std::uint8_t* source, int sourceWidth, int sourceHeight, std::uint8_t* destination);

// One resolved image draw. Pixels are top-left-origin device space; uv0..uv1 are
// normalized into the source image. `imageID` → the frontend's texture.
// `straightAlpha` is true for the over-blended RGBA path. `filter` is how the
// texels are read; see ``ImageFilter``.
struct ImageQuad {
    float minXInPixels;
    float minYInPixels;
    float maxXInPixels;
    float maxYInPixels;
    float u0;
    float v0;
    float u1;
    float v1;
    int32_t zIndex;
    bool straightAlpha;
    ImageFilter filter;
    uint32_t imageID;
};

// Array-based overload with no Terminal dependency (the Terminal* overload
// delegates here). `images`/`placements` are the Terminal's arrays.
//
/// How a resolved image fills its destination. Anchored = the placement's own cell
/// anchor + size. The rest are DECSET 7402 fullscreen fits, centered with the
/// residual as letterbox: ContainFit = aspect-preserving contain (default);
/// Stretch = fit a box of app aspect (fitAspectWidth:Height), independent X/Y
/// scale; Native = largest integer scale that fits, unstretched; Scale = explicit
/// integer multiplier (fitScaleMultiplier). fitAspectWidth/Height apply only to
/// Stretch, fitScaleMultiplier only to Scale.
enum class ImageFit {
    Anchored,
    ContainFit,
    Stretch,
    Native,
    Scale
};

/// How a DECSET 7402 fullscreen fit maps the image onto the drawable.
///
/// Grouped because four of these five are read only for particular `fit`
/// values — `aspect` by `Stretch`, `scaleMultiplier` by `Scale` — and as loose
/// trailing parameters with defaults they read as universally meaningful. A
/// caller that sets an aspect under `Native` has written something that does
/// nothing, and the signature said it was allowed.
struct ImageFitOptions {
    /// Which fit rule applies; the default anchors each placement.
    ImageFit fit = ImageFit::Anchored;
    /// The drawable to fit within, in device pixels. Zero means the grid.
    FloatSize drawablePixels {};
    /// The app aspect `Stretch` fits a box of. Read only under `Stretch`.
    IntSize aspect {};
    /// The integer multiplier `Scale` applies. Read only under `Scale`.
    int scaleMultiplier = 0;
};

/// Resolves every visible image placement into drawable quads.
///
/// - Parameters:
///   - images: The decoded images the placements refer to.
///   - placements: Where each image sits on the grid.
///   - grid: The grid's extent.
///   - cell: One cell's size in device pixels.
///   - origin: The grid's top-left in device pixels.
///   - viewOffset: Scrollback rows above the live tail; 0 is the tail.
///   - out: Destination quads; the count written is clamped to its size.
///   - fitOptions: The fullscreen fit rule and its parameters.
///   - alternateActive: Whether the alternate screen is the active one. A
///     placement emitted on the other screen resolves to no quad, so the two
///     buffers' pictures stay separate.
/// - Returns: How many quads were written.
int resolveImagePlacements(std::span<const TerminalImage> images, std::span<const TerminalImagePlacement> placements,
    GridSize grid, FloatSize cell, FloatPoint origin, int viewOffset,
    std::span<ImageQuad> out, ImageFitOptions fitOptions = {}, bool alternateActive = false);

/// Orders resolved quads for drawing and says where the text goes between them.
///
/// The Kitty graphics protocol puts a placement's `zIndex` in charge of depth,
/// and gives the text its own place in that order: a negative `zIndex` draws
/// beneath the cells, a non-negative one above them. So a renderer needs both an
/// order and the one index that splits it, and the two have to come from the
/// same pass or a placement can sort into one band and composite in the other.
///
/// The sort is STABLE, which is the half a reader is most likely to drop.
/// Placements sharing a `zIndex` keep the order the resolve produced, so the
/// later of two equal-depth placements draws on top — the protocol's own
/// tie-break, and the only thing distinguishing this from any ascending sort.
/// It is observable exactly when two same-`zIndex` placements overlap a pixel,
/// which is why an unstable sort passes every test that does not arrange one.
///
/// - Parameter quads: The resolved quads, reordered in place.
/// - Returns: The index the text draws at — quads before it composite under the
///   cells, quads from it on composite over them.
[[nodiscard]] int orderImageQuadsForDrawing(std::span<ImageQuad> quads);

} // namespace Ubiquitty::Image
