// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphSlotCache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace Ubiquitty::Rendering {

/// Selects native linear coverage or the bitmap tier's exact texel coverage.
enum class CoverageSampling : std::uint8_t {
    Bitmap,
    Native,
};

/// Samples an R8 coverage sheet using the native or bitmap filtering contract.
///
/// Coordinates name texel edges, so the first texel's centre is `(0.5, 0.5)`.
/// Native sampling interpolates between texel centres with transparent values
/// beyond the sheet, matching the native atlas sampler. Bitmap sampling takes
/// the containing texel and repeats the edge texel outside the sheet.
/// - Parameters:
///   - coverage: The complete row-major R8 coverage plane.
///   - width: The plane's width in texels.
///   - height: The plane's height in texels.
///   - x: The sample's horizontal coordinate in atlas pixels.
///   - y: The sample's vertical coordinate in atlas pixels.
///   - sampling: The coverage filtering and boundary convention.
/// - Returns: Normalized coverage, or zero for an invalid plane or coordinate.
[[nodiscard]] inline float sampleAtlasCoverage(std::span<const std::uint8_t> coverage,
    int width, int height, float x, float y, CoverageSampling sampling)
{
    if (width <= 0 || height <= 0 || coverage.size() < static_cast<std::size_t>(width) * height
        || !std::isfinite(x) || !std::isfinite(y))
        return 0.0f;
    if (sampling == CoverageSampling::Bitmap) {
        const int column = x <= 0.0f ? 0 : x >= static_cast<float>(width - 1) ? width - 1
                                                                              : static_cast<int>(x);
        const int row = y <= 0.0f ? 0 : y >= static_cast<float>(height - 1) ? height - 1
                                                                            : static_cast<int>(y);
        return coverage[static_cast<std::size_t>(row) * width + column] / 255.0f;
    }
    // Outside this range every bilinear texel is transparent. Bound the
    // conversion before casting so even a large finite coordinate is safe.
    if (x <= -0.5f || y <= -0.5f || x >= static_cast<float>(width) + 0.5f || y >= static_cast<float>(height) + 0.5f)
        return 0.0f;
    const float texelX = x - 0.5f;
    const float texelY = y - 0.5f;
    const int left = static_cast<int>(std::floor(texelX));
    const int top = static_cast<int>(std::floor(texelY));
    const float fractionX = texelX - left;
    const float fractionY = texelY - top;
    const auto texel = [&](int column, int row) {
        if (column < 0 || row < 0 || column >= width || row >= height)
            return 0.0f;
        return coverage[static_cast<std::size_t>(row) * width + column] / 255.0f;
    };
    const float topLeft = texel(left, top);
    const float topRight = texel(left + 1, top);
    const float bottomLeft = texel(left, top + 1);
    const float bottomRight = texel(left + 1, top + 1);
    const float firstRow = topLeft + fractionX * (topRight - topLeft);
    const float secondRow = bottomLeft + fractionX * (bottomRight - bottomLeft);
    return firstRow + fractionY * (secondRow - firstRow);
}

/// Writes a packed R8 glyph plane into a strided atlas region.
///
/// The region must fit the atlas. An empty plane clears the region, as a
/// missing glyph does; otherwise the plane contains exactly its pixels.
/// Source and destination do not overlap. Row copies keep this operation
/// batched even when the rasterizer and atlas have different strides.
///
/// - Parameters:
///   - atlas: The atlas's owned pixel storage.
///   - rowBytes: Bytes in each atlas row.
///   - region: The rectangle to write, in atlas pixels.
///   - coverage: Packed coverage, or empty to clear the rectangle.
inline void writeAtlasCoverage(std::span<std::uint8_t> atlas, int rowBytes,
    AtlasSlotRegion region, std::span<const std::uint8_t> coverage)
{
    assert(rowBytes > 0 && region.x >= 0 && region.y >= 0);
    assert(region.width > 0 && region.width <= rowBytes && region.height > 0);
    assert(region.x <= rowBytes - region.width);
    const auto stride = static_cast<std::size_t>(rowBytes);
    const auto width = static_cast<std::size_t>(region.width);
    const auto height = static_cast<std::size_t>(region.height);
    const auto first = static_cast<std::size_t>(region.y) * stride + region.x;
    assert(first <= atlas.size() && (height - 1) * stride + width <= atlas.size() - first);
    assert(coverage.empty() || coverage.size() == width * height);
    for (std::size_t row = 0; row < height; ++row) {
        auto* destination = atlas.data() + first + row * stride;
        if (coverage.empty())
            std::memset(destination, 0, width);
        else
            std::memcpy(destination, coverage.data() + row * width, width);
    }
}

} // namespace Ubiquitty::Rendering
