// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace Ubiquitty::Rendering {

/// Describes the source cells and horizontal ink owned by one shaped glyph.
struct ShapedGlyphExtent {
    int firstCell = 0;
    int cellCount = 1;
    double inkLeftInCells = 0;
    double inkRightInCells = 0;
};

/// A contiguous group whose glyph coverage cannot cross another group.
struct ShapedGlyphGroup {
    int firstCell = 0;
    int cellCount = 0;
};

using ShapedGlyphExtentSpan = std::span<const ShapedGlyphExtent>;
using ShapedGlyphBoundarySpan = std::span<unsigned char>;
using ShapedGlyphGroupSpan = std::span<ShapedGlyphGroup>;

/// Partitions cells without cutting a glyph's source coverage or painted ink.
///
/// Empty glyphs retain source coverage. Ink from a ligature carrier joins its
/// placeholder cells even when the shaper assigns each placeholder a distinct
/// source index. The caller supplies padded ink bounds in cell coordinates.
///
/// - Returns: The number of groups, or zero when scratch/output storage is insufficient.
inline int partitionShapedGlyphGroups(std::span<const ShapedGlyphExtent> glyphs, int cellCount,
    std::span<unsigned char> boundaries, std::span<ShapedGlyphGroup> groups)
{
    if (cellCount <= 0 || boundaries.size() < static_cast<std::size_t>(cellCount)
        || groups.size() < static_cast<std::size_t>(cellCount)) {
        return 0;
    }
    std::fill_n(boundaries.begin(), cellCount, 1);
    for (const auto& glyph : glyphs) {
        const int first = std::clamp(glyph.firstCell, 0, cellCount);
        const int sourceEnd = first + std::clamp(glyph.cellCount, 0, cellCount - first);
        int begin = first;
        int end = sourceEnd;
        if (std::isfinite(glyph.inkLeftInCells) && std::isfinite(glyph.inkRightInCells)
            && glyph.inkRightInCells > glyph.inkLeftInCells) {
            begin = std::min(begin, static_cast<int>(std::floor(std::clamp(glyph.inkLeftInCells, 0.0, static_cast<double>(cellCount)))));
            end = std::max(end, static_cast<int>(std::ceil(std::clamp(glyph.inkRightInCells, 0.0, static_cast<double>(cellCount)))));
        }
        for (int boundary = begin + 1; boundary < end; ++boundary) {
            boundaries[static_cast<std::size_t>(boundary)] = 0;
        }
    }
    int groupCount = 0;
    int first = 0;
    for (int boundary = 1; boundary <= cellCount; ++boundary) {
        if (boundary < cellCount && !boundaries[static_cast<std::size_t>(boundary)]) {
            continue;
        }
        groups[static_cast<std::size_t>(groupCount++)] = { first, boundary - first };
        first = boundary;
    }
    return groupCount;
}

} // namespace Ubiquitty::Rendering
