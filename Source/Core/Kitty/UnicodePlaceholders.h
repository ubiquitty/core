// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Kitty Unicode placeholders. A VIRTUAL placement (a=T/t/p with U=1) is
// positioned through text cells holding U+10EEEE. Each cell encodes its position
// in the image's row x column grid via combining diacritics (the kitty
// rowcolumn-diacritics table: the Nth diacritic means index N) and carries the
// image id in the cell's FOREGROUND color (8-bit in 256-color mode, 24-bit in
// truecolor; an optional 3rd diacritic supplies bits 24..31). The placement id
// rides the UNDERLINE color. Missing diacritics inherit from the cell to the left
// (RLE of a contiguous block). This module scans the grid for placeholder cells,
// decodes them, and emits ImageQuads sampling the matching image sub-rectangle.

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/Cell.h"
#include "Core/Image/TerminalImage.h"
#include <cstdint>
#include <span>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty {

namespace Image {
struct ImageQuad; // Image/ImagePlacement.h
}

namespace Kitty {

// The kitty placeholder codepoint (Private Use). A cell whose base codePoint is
// this is a Unicode-placeholder cell; its diacritics + colors locate it.
constexpr uint32_t placeholderCodePoint = 0x10EEEE;

// Map a combining-diacritic codePoint to its row/column index per the kitty
// rowcolumn-diacritics table (index == position in that table). Returns -1 when
// `codePoint` is not in the table.
[[nodiscard]] int rowColumnDiacriticIndex(uint32_t codePoint);

// True when any visible cell on `t` is a Unicode placeholder (placeholders carry
// no TerminalImagePlacement, so the frontend keeps its image pass active).
[[nodiscard]] bool hasPlaceholders(const Ubiquitty::Terminal* t, int viewOffset);

/// Resolves U+10EEEE placeholder cells into image quads.
///
/// Scans the visible grid, decodes each placeholder cell's (row, column,
/// imageID, placementID), matches it to a virtual-placement image, and appends
/// one quad per cell sampling that grid cell's sub-rectangle. The geometry
/// arguments mirror `Rendering::collectImageQuads`, the caller that combines
/// this with the grid placements.
///
/// - Parameters:
///   - t: The terminal whose visible grid is scanned.
///   - cellSizeInPixels: One cell, in device pixels.
///   - origin: The grid's top-left in device pixels.
///   - scale: Points to device pixels. The cell and the origin already carry
///     it, so it moves nothing here.
///   - viewOffset: Rows scrolled back from the live tail.
///   - out: Receives the quads; cells past its capacity are dropped.
/// - Returns: The quad count, at most `out.size()`, in row-major cell order.
///   Every quad sits at z-index 0, so a caller combining them with grid
///   placements sorts the whole set.
int resolvePlaceholders(const Ubiquitty::Terminal* t, FloatSize cellSizeInPixels, FloatPoint origin, float scale, int viewOffset, std::span<Image::ImageQuad> out);

/// The rows a frame copied out of its terminal, and the reader for the
/// clusters they name.
///
/// What a renderer drawing from a capture resolves placeholders from: it holds
/// the cells and images the capture took and no `Terminal`, and a placeholder's
/// row and column ride in its cluster's marks.
struct UBIQUITTY_NONESCAPABLE PlaceholderGrid {
    /// Writes a pooled cluster, base code point first, and returns how many
    /// code points it wrote. The same contract as
    /// `Rendering::BuildInputs::ClusterResolver`.
    using ClusterReader = int (*)(const Cell*, std::span<std::uint32_t>, void*);

    /// The visible cells, `size.rows` rows of `size.columns`, top row first.
    std::span<const Cell> cells {};
    GridSize size {};
    /// Reads a cell whose cluster lives outside it. Null leaves such a cell's
    /// marks unread, and a placeholder whose marks are unread places nothing.
    ClusterReader readCluster = nullptr;
    /// Handed to `readCluster` as its last argument.
    void* readClusterContext = nullptr;
};

/// Resolves the placeholder cells of a captured grid into image quads.
///
/// The decode, inheritance and tiling of the `Terminal` overload, read from
/// cells and images a frame already copied, so a renderer drawing from the
/// copy places every tile where a renderer reading the live terminal does.
///
/// - Parameters:
///   - grid: The captured rows and the reader for their clusters.
///   - images: The image table the placeholders name. Only images carrying a
///     virtual placement match.
///   - cellSizeInPixels: One cell, in device pixels.
///   - origin: The grid's top-left in device pixels.
///   - out: Receives the quads; cells past its capacity are dropped.
/// - Returns: The quad count, at most `out.size()`, in row-major cell order.
int resolvePlaceholders(const PlaceholderGrid& grid, std::span<const Image::TerminalImage> images, FloatSize cellSizeInPixels, FloatPoint origin, std::span<Image::ImageQuad> out);

} // namespace Kitty
} // namespace Ubiquitty
