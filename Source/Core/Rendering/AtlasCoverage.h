// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphSlotCache.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>

namespace Ubiquitty::Rendering {

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
