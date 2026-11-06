// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Image/ImagePlacement.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Ubiquitty::Image {

namespace {

// Whether `destination` device pixels over `source` texels is a whole-number
// enlargement, 1:1 included. The tolerance is across the whole quad, so a
// float span that is the exact product up to rounding still counts.
bool isWholeEnlargement(float destination, int source)
{
    if (source <= 0)
        return false;
    const float factor = std::round(destination / static_cast<float>(source));
    if (factor < 1.0f)
        return false;
    return std::fabs(destination - factor * static_cast<float>(source)) < 0.01f;
}

} // namespace

ImageFilter imageFilterForScale(float destinationWidth, float destinationHeight, int sourceWidth, int sourceHeight)
{
    return isWholeEnlargement(destinationWidth, sourceWidth) && isWholeEnlargement(destinationHeight, sourceHeight)
        ? ImageFilter::Nearest
        : ImageFilter::Smooth;
}

float imageLevelOfDetail(float texelsAcross, float texelsDown, float pixelsAcross, float pixelsDown)
{
    if (pixelsAcross <= 0.0f || pixelsDown <= 0.0f)
        return 0.0f;
    const float ratio = std::max(texelsAcross / pixelsAcross, texelsDown / pixelsDown);
    return ratio > 1.0f ? std::log2(ratio) : 0.0f;
}

int imageMipLevelCount(int width, int height)
{
    int levels = 1;
    while (width > 1 || height > 1) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
        ++levels;
    }
    return levels;
}

void bleedIntoTransparentTexels(std::uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    // A breadth-first fill outward from the visible texels: each ring takes the
    // mean colour of the coloured neighbours it isTouchingColour, so a texel's colour
    // comes from the nearest edge, and each texel is visited a bounded number
    // of times however wide the transparent region is.
    enum State : std::uint8_t {
        Uncoloured,
        Queued,
        Coloured,
    };
    std::vector<std::uint8_t> state(count, Uncoloured);
    bool hasTransparent = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (rgba[index * 4 + 3] != 0)
            state[index] = Coloured;
        else
            hasTransparent = true;
    }
    if (!hasTransparent)
        return;
    const auto forEachNeighbour = [width, height](std::size_t index, auto&& visit) {
        const int x = static_cast<int>(index % static_cast<std::size_t>(width));
        const int y = static_cast<int>(index / static_cast<std::size_t>(width));
        for (int deltaY = -1; deltaY <= 1; ++deltaY) {
            for (int deltaX = -1; deltaX <= 1; ++deltaX) {
                const int neighbourX = x + deltaX;
                const int neighbourY = y + deltaY;
                if ((deltaX == 0 && deltaY == 0) || neighbourX < 0 || neighbourY < 0 || neighbourX >= width || neighbourY >= height)
                    continue;
                visit(static_cast<std::size_t>(neighbourY) * static_cast<std::size_t>(width) + static_cast<std::size_t>(neighbourX));
            }
        }
    };
    std::vector<std::size_t> ring;
    std::vector<std::size_t> next;
    for (std::size_t index = 0; index < count; ++index) {
        if (state[index] != Uncoloured)
            continue;
        bool isTouchingColour = false;
        forEachNeighbour(index, [&](std::size_t neighbour) { isTouchingColour = isTouchingColour || state[neighbour] == Coloured; });
        if (isTouchingColour) {
            state[index] = Queued;
            ring.push_back(index);
        }
    }
    while (!ring.empty()) {
        // The whole ring is coloured from the rings before it, then marked, so
        // no texel of a ring reads another texel of the same ring.
        for (const std::size_t index : ring) {
            std::array<std::uint32_t, 3> sums {};
            std::uint32_t taken = 0;
            forEachNeighbour(index, [&](std::size_t neighbour) {
                if (state[neighbour] != Coloured)
                    return;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    sums[channel] += rgba[neighbour * 4 + channel];
                ++taken;
            });
            for (std::size_t channel = 0; channel < 3; ++channel)
                rgba[index * 4 + channel] = static_cast<std::uint8_t>((sums[channel] + taken / 2) / taken);
        }
        next.clear();
        for (const std::size_t index : ring)
            state[index] = Coloured;
        for (const std::size_t index : ring) {
            forEachNeighbour(index, [&](std::size_t neighbour) {
                if (state[neighbour] != Uncoloured)
                    return;
                state[neighbour] = Queued;
                next.push_back(neighbour);
            });
        }
        ring.swap(next);
    }
}

void downsampleImageMipLevel(const std::uint8_t* source, int sourceWidth, int sourceHeight, std::uint8_t* destination)
{
    if (!source || !destination || sourceWidth <= 0 || sourceHeight <= 0)
        return;
    const int width = std::max(1, sourceWidth / 2);
    const int height = std::max(1, sourceHeight / 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint32_t alphaSum = 0;
            std::array<std::uint32_t, 3> colourSums {};
            std::array<std::uint32_t, 3> plainSums {};
            for (int deltaY = 0; deltaY < 2; ++deltaY) {
                for (int deltaX = 0; deltaX < 2; ++deltaX) {
                    const int sourceX = std::min(x * 2 + deltaX, sourceWidth - 1);
                    const int sourceY = std::min(y * 2 + deltaY, sourceHeight - 1);
                    const std::uint8_t* texel = source + (static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(sourceWidth) + static_cast<std::size_t>(sourceX)) * 4;
                    alphaSum += texel[3];
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        colourSums[channel] += static_cast<std::uint32_t>(texel[channel]) * texel[3];
                        plainSums[channel] += texel[channel];
                    }
                }
            }
            std::uint8_t* out = destination + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                // Alpha-weighted where anything shows; where nothing does, the
                // block's own colours, which a bled level above already made
                // the edge's.
                out[channel] = alphaSum > 0
                    ? static_cast<std::uint8_t>((colourSums[channel] + alphaSum / 2) / alphaSum)
                    : static_cast<std::uint8_t>((plainSums[channel] + 2) / 4);
            }
            out[3] = static_cast<std::uint8_t>((alphaSum + 2) / 4);
        }
    }
}

int resolveImagePlacements(std::span<const TerminalImage> images, std::span<const TerminalImagePlacement> placements,
    GridSize grid, FloatSize cell, FloatPoint origin, int viewOffset,
    std::span<ImageQuad> out, ImageFitOptions fitOptions, bool alternateActive)
{
    // Bound to scalars once, so the arithmetic below reads plain names; a
    // member path at every use is a place to swap rows for columns.
    const int imageCount = static_cast<int>(images.size());
    const int placementCount = static_cast<int>(placements.size());
    const int gridRows = grid.rows;
    const int gridColumns = grid.columns;
    const float cellWidth = cell.width;
    const float cellHeight = cell.height;
    const float originX = origin.x;
    const float originY = origin.y;
    const int maxOut = static_cast<int>(out.size());
    const ImageFit fit = fitOptions.fit;
    const float drawablePixelWidth = fitOptions.drawablePixels.width;
    const float drawablePixelHeight = fitOptions.drawablePixels.height;
    const int fitAspectWidth = fitOptions.aspect.width;
    const int fitAspectHeight = fitOptions.aspect.height;
    const int fitScaleMultiplier = fitOptions.scaleMultiplier;

    // A span cannot carry a live pointer with a wrong count, so the four
    // guards the pointer form needed collapse into three emptiness tests.
    if (images.empty() || placements.empty() || out.empty())
        return 0;
    if (cellWidth <= 0.0f || cellHeight <= 0.0f)
        return 0;

    const float gridRight = originX + static_cast<float>(gridColumns) * cellWidth;
    const float gridBottom = originY + static_cast<float>(gridRows) * cellHeight;
    int count = 0;

    for (int pi = 0; pi < placementCount && count < maxOut; ++pi) {
        const TerminalImagePlacement* p = &placements[pi];
        if (!p->used)
            continue;
        if (!placementIsOnActiveScreen(*p, alternateActive))
            continue;

        const TerminalImage* image = nullptr;
        for (int ii = 0; ii < imageCount; ++ii) {
            const TerminalImage* cand = &images[ii];
            if (cand->used && cand->pixels && cand->width > 0 && cand->height > 0 && cand->imageID == p->imageID) {
                image = cand;
                break;
            }
        }
        if (!image)
            continue;

        // Source sub-rectangle (0 w/h = remainder).
        int sourceX = static_cast<int>(p->sourceX);
        int sourceY = static_cast<int>(p->sourceY);
        int sourceWidth = (p->sourceWidth > 0) ? static_cast<int>(p->sourceWidth) : (image->width - sourceX);
        int sourceHeight = (p->sourceHeight > 0) ? static_cast<int>(p->sourceHeight) : (image->height - sourceY);
        if (sourceX < 0 || sourceY < 0 || sourceWidth <= 0 || sourceHeight <= 0)
            continue;
        if (sourceX + sourceWidth > image->width)
            sourceWidth = image->width - sourceX;
        if (sourceY + sourceHeight > image->height)
            sourceHeight = image->height - sourceY;
        if (sourceWidth <= 0 || sourceHeight <= 0)
            continue;

        // Destination geometry. viewOffset shifts the row so images flow with
        // their text and stay visible in scrollback.
        const int screenRow = static_cast<int>(p->cursorY) + viewOffset;
        float destinationX, destinationY, destinationWidth, destinationHeight;
        const bool isFullscreenFit = (fit == ImageFit::ContainFit || fit == ImageFit::Stretch || fit == ImageFit::Native || fit == ImageFit::Scale);
        if (isFullscreenFit) {
            // DECSET 7402 fullscreen content: center in the FULL DRAWABLE (not the
            // cell grid), so the only bars are symmetric letterbox. Falls back to the
            // grid viewport when the drawable size is unknown (0); ignores the cell anchor.
            const float fitViewportWidth = (drawablePixelWidth > 0.0f) ? drawablePixelWidth : (gridRight - originX);
            const float fitViewportHeight = (drawablePixelHeight > 0.0f) ? drawablePixelHeight : (gridBottom - originY);
            const float fitOriginX = (drawablePixelWidth > 0.0f) ? 0.0f : originX;
            const float fitOriginY = (drawablePixelHeight > 0.0f) ? 0.0f : originY;
            if (fit == ImageFit::Stretch && fitAspectWidth > 0 && fitAspectHeight > 0) {
                // Stretch: contain a box of app aspect (fitAspectWidth:Height), then
                // STRETCH the source to fill it (independent X/Y scale; source pixel
                // aspect discarded — DOS pixel correction).
                float boxScale = fitViewportWidth / static_cast<float>(fitAspectWidth);
                const float boxScaleByHeight = fitViewportHeight / static_cast<float>(fitAspectHeight);
                if (boxScaleByHeight < boxScale)
                    boxScale = boxScaleByHeight;
                destinationWidth = static_cast<float>(fitAspectWidth) * boxScale;
                destinationHeight = static_cast<float>(fitAspectHeight) * boxScale;
            } else if (fit == ImageFit::Native) {
                // Native: largest INTEGER scale that fits (>= 1x), unstretched, in
                // device pixels.
                const float scaleByWidth = fitViewportWidth / static_cast<float>(sourceWidth);
                const float scaleByHeight = fitViewportHeight / static_cast<float>(sourceHeight);
                float integerScale = (scaleByHeight < scaleByWidth) ? scaleByHeight : scaleByWidth;
                integerScale = static_cast<float>(static_cast<int>(integerScale)); // floor (positive)
                if (integerScale < 1.0f)
                    integerScale = 1.0f;
                destinationWidth = static_cast<float>(sourceWidth) * integerScale;
                destinationHeight = static_cast<float>(sourceHeight) * integerScale;
            } else if (fit == ImageFit::Scale && fitScaleMultiplier > 0) {
                // Scale: exact app-set source-px -> device-px multiplier, unstretched + centered.
                destinationWidth = static_cast<float>(sourceWidth) * static_cast<float>(fitScaleMultiplier);
                destinationHeight = static_cast<float>(sourceHeight) * static_cast<float>(fitScaleMultiplier);
            } else {
                // Contain (default): aspect-preserving contain-fit of the source.
                float fitScale = fitViewportWidth / static_cast<float>(sourceWidth);
                const float fitScaleByHeight = fitViewportHeight / static_cast<float>(sourceHeight);
                if (fitScaleByHeight < fitScale)
                    fitScale = fitScaleByHeight;
                destinationWidth = static_cast<float>(sourceWidth) * fitScale;
                destinationHeight = static_cast<float>(sourceHeight) * fitScale;
            }
            destinationX = fitOriginX + (fitViewportWidth - destinationWidth) * 0.5f;
            destinationY = fitOriginY + (fitViewportHeight - destinationHeight) * 0.5f;
        } else {
            destinationX = originX + static_cast<float>(p->cursorX) * cellWidth + static_cast<float>(p->pixelOffsetX);
            destinationY = originY + static_cast<float>(screenRow) * cellHeight + static_cast<float>(p->pixelOffsetY);
            // The two axes are sized on ONE basis. A cell box states the size in
            // cells; no box at all states it in source pixels. Taking each axis
            // independently would let a placement that names only its width be
            // sized in cells across and in pixels down — a stretch along one
            // axis, which is what every protocol that allows a single-axis box
            // (kitty's c/r, iTerm2's width/height) can produce. The stated axis
            // wins and the other follows the source's aspect.
            const bool hasColumns = p->cellColumns > 0;
            const bool hasRows = p->cellRows > 0;
            if (p->drawnPixelWidth > 0 && p->drawnPixelHeight > 0) {
                // A size the protocol stated in device pixels is drawn at
                // exactly that size on every arm; the cell box only says
                // which cells the picture covers.
                destinationWidth = static_cast<float>(p->drawnPixelWidth);
                destinationHeight = static_cast<float>(p->drawnPixelHeight);
            } else if (hasColumns && hasRows) {
                destinationWidth = static_cast<float>(p->cellColumns) * cellWidth;
                destinationHeight = static_cast<float>(p->cellRows) * cellHeight;
            } else if (hasColumns) {
                destinationWidth = static_cast<float>(p->cellColumns) * cellWidth;
                destinationHeight = destinationWidth * static_cast<float>(sourceHeight) / static_cast<float>(sourceWidth);
            } else if (hasRows) {
                destinationHeight = static_cast<float>(p->cellRows) * cellHeight;
                destinationWidth = destinationHeight * static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
            } else {
                // Protocol pixels and the terminal's cell metrics are device pixels.
                destinationWidth = static_cast<float>(sourceWidth);
                destinationHeight = static_cast<float>(sourceHeight);
            }
            // Within a pixel of the source on both axes is the source's own
            // size: drawn there, one texel a pixel, from the same corner,
            // rather than resampled by a fraction that can only blur it. A
            // box of whole cells at a fractional advance lands here for an
            // image made for it.
            if (std::fabs(destinationWidth - static_cast<float>(sourceWidth)) < 1.0f
                && std::fabs(destinationHeight - static_cast<float>(sourceHeight)) < 1.0f) {
                destinationWidth = static_cast<float>(sourceWidth);
                destinationHeight = static_cast<float>(sourceHeight);
            }
        }
        if (destinationWidth <= 0.0f || destinationHeight <= 0.0f)
            continue;

        // Clip to the viewport: the full drawable for a fullscreen fit, else the cell grid.
        const bool shouldFillDrawable = isFullscreenFit && (drawablePixelWidth > 0.0f) && (drawablePixelHeight > 0.0f);
        const float clipLeft = shouldFillDrawable ? 0.0f : originX;
        const float clipTop = shouldFillDrawable ? 0.0f : originY;
        const float clipRight = shouldFillDrawable ? drawablePixelWidth : gridRight;
        const float clipBottom = shouldFillDrawable ? drawablePixelHeight : gridBottom;
        float x0 = destinationX, y0 = destinationY, x1 = destinationX + destinationWidth, y1 = destinationY + destinationHeight;
        if (x0 < clipLeft)
            x0 = clipLeft;
        if (y0 < clipTop)
            y0 = clipTop;
        if (x1 > clipRight)
            x1 = clipRight;
        if (y1 > clipBottom)
            y1 = clipBottom;
        if (x0 >= x1 || y0 >= y1)
            continue;

        // Map the clipped device rect back to normalized UVs over the source
        // sub-rectangle.
        const float u0 = static_cast<float>(sourceX) / static_cast<float>(image->width);
        const float v0 = static_cast<float>(sourceY) / static_cast<float>(image->height);
        const float uWidth = static_cast<float>(sourceWidth) / static_cast<float>(image->width);
        const float vHeight = static_cast<float>(sourceHeight) / static_cast<float>(image->height);
        const float fx0 = (x0 - destinationX) / destinationWidth, fx1 = (x1 - destinationX) / destinationWidth;
        const float fy0 = (y0 - destinationY) / destinationHeight, fy1 = (y1 - destinationY) / destinationHeight;

        ImageQuad q;
        q.minXInPixels = x0;
        q.minYInPixels = y0;
        q.maxXInPixels = x1;
        q.maxYInPixels = y1;
        q.u0 = u0 + fx0 * uWidth;
        q.v0 = v0 + fy0 * vHeight;
        q.u1 = u0 + fx1 * uWidth;
        q.v1 = v0 + fy1 * vHeight;
        q.zIndex = p->zIndex;
        q.straightAlpha = true;
        q.filter = imageFilterForScale(destinationWidth, destinationHeight, sourceWidth, sourceHeight);
        q.imageID = p->imageID;
        out[count++] = q;
    }

    return count;
}

int orderImageQuadsForDrawing(std::span<ImageQuad> quads)
{
    // Insertion sort rather than std::stable_sort: a placement table holds a
    // couple of dozen entries at most, and this runs per frame on a path with
    // no allocation of its own. std::stable_sort would be free to take a
    // temporary buffer for a range this size.
    const int count = static_cast<int>(quads.size());
    for (int i = 1; i < count; ++i) {
        const ImageQuad moving = quads[static_cast<std::size_t>(i)];
        int place = i - 1;
        // Strictly greater, so an equal zIndex stops the shift and the moving
        // quad lands behind the one already there. That comparison is the whole
        // of the stability guarantee: flipping it to >= reverses equal-depth
        // placements while sorting just as correctly.
        while (place >= 0 && quads[static_cast<std::size_t>(place)].zIndex > moving.zIndex) {
            quads[static_cast<std::size_t>(place + 1)] = quads[static_cast<std::size_t>(place)];
            --place;
        }
        quads[static_cast<std::size_t>(place + 1)] = moving;
    }

    int split = 0;
    while (split < count && quads[static_cast<std::size_t>(split)].zIndex < 0)
        ++split;
    return split;
}

} // namespace Ubiquitty::Image
