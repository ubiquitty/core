// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/CellContent.h"
#include "Core/CellPen.h"
#include "Core/Color.h"

#include "Base/Geometry.h"
#include "Core/Cell.h"
#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace Ubiquitty::Kitty {

// The kitty rowcolumn-diacritics table (verbatim from upstream). A codePoint's
// index in this array is the row/column/MSB value it encodes. Sorted ascending
// for binary search.
static constexpr uint32_t rowColumnDiacritics[] = { 0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483,
    0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A, 0x065B, 0x065D,
    0x065E, 0x06D6, 0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2, 0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF,
    0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819, 0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822, 0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17,
    0x1A75, 0x1A76, 0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D, 0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1, 0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4, 0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4,
    0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1, 0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1, 0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7, 0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
    0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA, 0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D, 0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2,
    0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED, 0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7, 0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23, 0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187, 0x1D188,
    0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244 };
static constexpr int rowColumnDiacriticsCount = static_cast<int>(sizeof(rowColumnDiacritics) / sizeof(rowColumnDiacritics[0]));

int rowColumnDiacriticIndex(uint32_t codePoint)
{
    int lo = 0;
    int hi = rowColumnDiacriticsCount - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) >> 1);
        const uint32_t v = rowColumnDiacritics[mid];
        if (v == codePoint)
            return mid;
        if (v < codePoint)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

namespace {

// One decoded placeholder cell. `valid` is false when the cell is not a
// placeholder (so the inherit-from-left chain breaks).
struct DecodedPlaceholder {
    bool valid;
    bool hasRow; // a row diacritic was present
    bool hasColumn; // a column diacritic was present
    bool hasMostSignificantByte; // a 3rd (MSB) diacritic was present
    int row;
    int column;
    uint32_t mostSignificantByte; // image-id bits 24..31
    uint32_t foregroundKey; // fg-color identity (for the inherit "same fg" test)
    uint32_t underlineKey; // underline-color identity (for the inherit test)
    uint32_t imageIDLow; // image id from the fg color (bits 0..23 / 8-bit index)
    bool hasPlacementID;
    uint32_t placementID; // from the underline color
};

// Fold a cell's foreground color into a 24-bit-or-index image id and an opaque
// identity key. Per kitty: 8-bit palette INDEX in 256-color mode, 24-bit RGB
// value in truecolor mode. colorDefault carries no id (key 0, idLow 0).
void foregroundImageID(const Pen* pen, uint32_t* idLow, uint32_t* key)
{
    if (pen->foregroundIndex == colorRGB) {
        const uint32_t rgb = (static_cast<uint32_t>(pen->foregroundRed) << 16) | (static_cast<uint32_t>(pen->foregroundGreen) << 8) | static_cast<uint32_t>(pen->foregroundBlue);
        *idLow = rgb;
        *key = 0x01000000u | rgb; // tag the truecolor space distinct from indices
    } else if (pen->foregroundIndex < colorDefault) {
        *idLow = pen->foregroundIndex; // palette index 0..255 == 8-bit id
        *key = 0x02000000u | pen->foregroundIndex;
    } else {
        *idLow = 0;
        *key = 0; // default fg: no encoded id
    }
}

// Fold a cell's underline color into a placement id + identity key (24-bit value
// / 8-bit index). 0 or "follow text" means unspecified.
void underlinePlacementID(const Pen* pen, bool* hasID, uint32_t* placementID, uint32_t* key)
{
    if (pen->underlineIndex == colorRGB) {
        const uint32_t rgb = (static_cast<uint32_t>(pen->underlineRed) << 16) | (static_cast<uint32_t>(pen->underlineGreen) << 8) | static_cast<uint32_t>(pen->underlineBlue);
        *placementID = rgb;
        *hasID = rgb != 0;
        *key = 0x01000000u | rgb;
    } else if (pen->underlineIndex < colorDefault) {
        *placementID = pen->underlineIndex;
        *hasID = pen->underlineIndex != 0;
        *key = 0x02000000u | pen->underlineIndex;
    } else {
        *placementID = 0;
        *hasID = false;
        *key = 0; // underlineIndex == colorDefault → follows fg; no placement id
    }
}

// Decode one cell into a DecodedPlaceholder (without inheritance). Not a
// placeholder → valid=false. `readCluster(cell, codePoints)` writes the cell's
// cluster and returns its length, whichever store the cell's grid reads from,
// or -1 when its marks cannot be read. Such a cell decodes as no placeholder:
// its position is unknown, and a cell inheriting from it would be placed by a
// guess.
template<typename ReadCluster>
DecodedPlaceholder decodeCell(const Cell* cell, const ReadCluster& readCluster)
{
    DecodedPlaceholder d {};
    if (!cell || cell->codePoint != placeholderCodePoint)
        return d;

    // Cluster sequence: [base(0x10EEEE), diacritic0, diacritic1, diacritic2...].
    std::array<uint32_t, clusterPoolMaximumCodePoints> cluster;
    const int readLength = readCluster(cell, std::span { cluster });
    if (readLength < 0)
        return d;
    d.valid = true;
    const int clusterLength = std::min(readLength, clusterPoolMaximumCodePoints);
    int seen = 0; // count of recognized row/col/MSB diacritics
    for (int i = 1; i < clusterLength && seen < 3; ++i) {
        const int index = rowColumnDiacriticIndex(cluster[i]);
        if (index < 0)
            continue; // a stray non-table mark (e.g. VS) — skip it
        if (seen == 0) {
            d.row = index;
            d.hasRow = true;
        } else if (seen == 1) {
            d.column = index;
            d.hasColumn = true;
        } else {
            d.mostSignificantByte = static_cast<uint32_t>(index) & 0xFFu;
            d.hasMostSignificantByte = true;
        }
        ++seen;
    }

    const Pen& placeholderPen = Ubiquitty::cellPen(*cell);
    foregroundImageID(&placeholderPen, &d.imageIDLow, &d.foregroundKey);
    underlinePlacementID(&placeholderPen, &d.hasPlacementID, &d.placementID, &d.underlineKey);
    return d;
}

// Find a virtual-placement image matching an id, and a placement id when the
// placeholder specified one. Returns nullptr when none matches.
//
// The placement id is optional rather than a value plus a flag: the two were
// only ever meaningful together, and a caller could pass a placement id with
// the flag clear and have it silently ignored.
const Image::TerminalImage* findVirtualImage(std::span<const Image::TerminalImage> images, uint32_t imageID, std::optional<uint32_t> placementID)
{
    const Image::TerminalImage* fallback = nullptr;
    for (const Image::TerminalImage& candidate : images) {
        const Image::TerminalImage* image = &candidate;
        if (!image->used || !image->virtualPlacement || !image->pixels)
            continue;
        if (image->width == 0 || image->height == 0)
            continue;
        if (image->imageID != imageID)
            continue;
        if (placementID) {
            if (image->virtualPlacementID == *placementID)
                return image; // exact placement match wins
            continue;
        }
        if (!fallback)
            fallback = image; // any virtual placement of the id (spec: p omitted/0)
    }
    return fallback;
}

// Whether any image in the table carries a virtual placement: without one no
// placeholder cell can resolve, so the grid scan is skipped.
bool hasVirtualImage(std::span<const Image::TerminalImage> images)
{
    return std::ranges::any_of(images, [](const Image::TerminalImage& image) {
        return image.used && image.virtualPlacement;
    });
}

// The scan both grid sources share. `rowAt(screenRow)` resolves one visible
// row, top row first; `readCluster(cell, codePoints)` writes a cell's cluster.
template<typename RowAt, typename ReadCluster>
int resolveRows(GridSize grid, const RowAt& rowAt, const ReadCluster& readCluster, std::span<const Image::TerminalImage> images,
    FloatSize cellSizeInPixels, FloatPoint origin, std::span<Image::ImageQuad> out)
{
    const float cellWidth = cellSizeInPixels.width;
    const float cellHeight = cellSizeInPixels.height;
    const float originX = origin.x;
    const float originY = origin.y;
    const int maxOut = static_cast<int>(out.size());
    const int rows = grid.rows;
    const int columns = grid.columns;

    int count = 0;
    for (int screenRow = 0; screenRow < rows && count < maxOut; ++screenRow) {
        // Once per row: addressing a cell by row and column re-derives the
        // history depth and re-resolves the ring slot for that one cell, and
        // this loop asks for every cell of the screen on every frame a
        // placeholder image is up.
        const Ubiquitty::Screen::VisibleRow row = rowAt(screenRow);
        DecodedPlaceholder prev {}; // the cell to the left (for inheritance)
        for (int col = 0; col < columns && count < maxOut; ++col) {
            const Cell* cell = Ubiquitty::Screen::cellInResolvedRow(row, col, columns);
            DecodedPlaceholder d = decodeCell(cell, readCluster);
            if (!d.valid) {
                prev = d; // breaks the inherit chain
                continue;
            }

            // Inheritance from the placeholder cell to the left (kitty spec).
            // The chain requires matching fg + underline colors throughout.
            const bool colorsMatchLeft = prev.valid && prev.foregroundKey == d.foregroundKey && prev.underlineKey == d.underlineKey;
            if (!d.hasRow && colorsMatchLeft) {
                // Rule 1: no diacritics → row = left.row, column = left.column+1, MSB inherited.
                d.row = prev.row;
                d.hasRow = true;
                d.column = prev.column + 1;
                d.hasColumn = true;
                d.mostSignificantByte = prev.mostSignificantByte;
                d.hasMostSignificantByte = true;
            } else if (d.hasRow && !d.hasColumn && colorsMatchLeft && prev.row == d.row) {
                // Rule 2: row only, same row → column = left.column+1, MSB inherited.
                d.column = prev.column + 1;
                d.hasColumn = true;
                d.mostSignificantByte = prev.mostSignificantByte;
                d.hasMostSignificantByte = true;
            } else if (d.hasRow && d.hasColumn && !d.hasMostSignificantByte && colorsMatchLeft && prev.row == d.row && prev.column + 1 == d.column) {
                // Rule 3: row+column contiguous with left → MSB inherited.
                d.mostSignificantByte = prev.mostSignificantByte;
                d.hasMostSignificantByte = true;
            }

            prev = d; // advance the left-cell state regardless of placement success

            // A placeholder with no resolvable (row, column) can't be placed.
            if (!d.hasRow || !d.hasColumn)
                continue;

            const uint32_t imageID = d.imageIDLow | (d.mostSignificantByte << 24);
            const Image::TerminalImage* image = findVirtualImage(images, imageID, d.hasPlacementID ? std::optional<uint32_t> { d.placementID } : std::nullopt);
            if (!image)
                continue;

            // Partition the image into a virtualRows x virtualColumns grid; cell
            // (d.row, d.column) shows that grid cell. A 0 virtual dimension is
            // unspecified — fall back to at least (row+1, column+1).
            int gridRows = image->virtualRows;
            int gridColumns = image->virtualColumns;
            if (gridRows <= 0)
                gridRows = d.row + 1;
            if (gridColumns <= 0)
                gridColumns = d.column + 1;
            if (d.row >= gridRows || d.column >= gridColumns)
                continue; // cell outside the declared grid — ignore

            // Source sub-rectangle for grid cell (d.row, d.column). Use rounded
            // boundaries so adjacent cells tile the image without gaps/overlap.
            const int imageWidth = image->width;
            const int imageHeight = image->height;
            const int sx0 = static_cast<int>(static_cast<int64_t>(d.column) * imageWidth / gridColumns);
            const int sx1 = static_cast<int>(static_cast<int64_t>(d.column + 1) * imageWidth / gridColumns);
            const int sy0 = static_cast<int>(static_cast<int64_t>(d.row) * imageHeight / gridRows);
            const int sy1 = static_cast<int>(static_cast<int64_t>(d.row + 1) * imageHeight / gridRows);
            if (sx1 <= sx0 || sy1 <= sy0)
                continue;

            // Destination: this one screen cell.
            const float dx = originX + static_cast<float>(col) * cellWidth;
            const float dy = originY + static_cast<float>(screenRow) * cellHeight;
            Image::ImageQuad q {};
            q.minXInPixels = dx;
            q.minYInPixels = dy;
            q.maxXInPixels = dx + cellWidth;
            q.maxYInPixels = dy + cellHeight;
            q.u0 = static_cast<float>(sx0) / static_cast<float>(imageWidth);
            q.v0 = static_cast<float>(sy0) / static_cast<float>(imageHeight);
            q.u1 = static_cast<float>(sx1) / static_cast<float>(imageWidth);
            q.v1 = static_cast<float>(sy1) / static_cast<float>(imageHeight);
            q.zIndex = 0;
            q.straightAlpha = true;
            q.filter = Image::imageFilterForScale(cellWidth, cellHeight, sx1 - sx0, sy1 - sy0);
            q.imageID = imageID;
            out[static_cast<std::size_t>(count++)] = q;
        }
    }
    return count;
}

} // namespace

bool hasPlaceholders(const Ubiquitty::Terminal* t, int viewOffset)
{
    if (!t || t->rows <= 0 || t->columns <= 0)
        return false;
    // Only worth scanning when a virtual placement actually exists.
    if (!hasVirtualImage(t->images.stored))
        return false;
    // Resolve each visible row once, then scan its Cell span. Cells past the row's
    // length are blank (never placeholders), so [0, length) clamped to columns is
    // the whole detectable extent.
    for (int r = 0; r < t->rows; ++r) {
        const Ubiquitty::Screen::VisibleRow row = Ubiquitty::Screen::resolveVisibleRow(t, viewOffset, r);
        const int limit = row.length < t->columns ? row.length : t->columns;
        for (int c = 0; c < limit; ++c)
            if (row.base[c].codePoint == placeholderCodePoint)
                return true;
    }
    return false;
}

int resolvePlaceholders(const Ubiquitty::Terminal* t, FloatSize cellSizeInPixels, FloatPoint origin, float scale, int viewOffset, std::span<Image::ImageQuad> out)
{
    if (!t || out.empty())
        return 0;
    if (cellSizeInPixels.width <= 0.0f || cellSizeInPixels.height <= 0.0f)
        return 0;
    // The cell and the origin arrive in device pixels, so the scale has
    // nothing left to convert.
    (void)scale;
    if (t->rows <= 0 || t->columns <= 0)
        return 0;
    const auto rowAt = [t, viewOffset](int screenRow) {
        return Ubiquitty::Screen::resolveVisibleRow(t, viewOffset, screenRow);
    };
    const auto readCluster = [t](const Cell* cell, std::span<uint32_t> codePoints) {
        return Ubiquitty::cellClusterCodePoints(t, cell, codePoints.data(), static_cast<int>(codePoints.size()));
    };
    return resolveRows({ t->columns, t->rows }, rowAt, readCluster, t->images.stored, cellSizeInPixels, origin, out);
}

int resolvePlaceholders(const PlaceholderGrid& grid, std::span<const Image::TerminalImage> images, FloatSize cellSizeInPixels, FloatPoint origin, std::span<Image::ImageQuad> out)
{
    if (out.empty() || grid.size.rows <= 0 || grid.size.columns <= 0)
        return 0;
    if (cellSizeInPixels.width <= 0.0f || cellSizeInPixels.height <= 0.0f)
        return 0;
    if (!hasVirtualImage(images))
        return 0;
    // A span shorter than the extent it claims draws the rows it holds.
    const std::size_t columns = static_cast<std::size_t>(grid.size.columns);
    const int rows = static_cast<int>(std::min(static_cast<std::size_t>(grid.size.rows), grid.cells.size() / columns));
    const auto rowAt = [&grid, columns](int screenRow) {
        return Ubiquitty::Screen::VisibleRow {
            .base = grid.cells.data() + static_cast<std::size_t>(screenRow) * columns,
            .length = static_cast<int>(columns),
            .needsShaping = false,
        };
    };
    // The capture's rule for reading a cluster: a cell whose marks fit inline
    // holds them, and only a pooled one asks the reader. A reader with no
    // entry for the cell leaves its inline marks, as the terminal's own store
    // does. With no reader at all a pooled cell's marks are unknown: its inline
    // buffer keeps only the first of them.
    const auto readCluster = [&grid](const Cell* cell, std::span<uint32_t> codePoints) {
        if (cell->clusterID() != 0) {
            if (!grid.readCluster)
                return -1;
            const int stored = grid.readCluster(cell, codePoints, grid.readClusterContext);
            if (stored > 0)
                return stored;
        }
        return Ubiquitty::cellInlineClusterCodePoints(*cell, codePoints.data(), static_cast<int>(codePoints.size()));
    };
    return resolveRows({ grid.size.columns, rows }, rowAt, readCluster, images, cellSizeInPixels, origin, out);
}

} // namespace Ubiquitty::Kitty
