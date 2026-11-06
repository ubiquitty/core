// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/DRCSRaster.cpp — scale a DECDLD glyph matrix onto a cell
// box as R8 coverage. Pure C++ (no platform types); runs on an atlas miss, not
// per cell.

#include "Core/Rendering/DRCSRaster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace Ubiquitty::Rendering {

namespace {

// The source samples one destination index draws on, along one axis.
//
// On the upscale a destination pixel's footprint covers a fraction of one or
// two source samples, and each weight is that fraction, so the weights sum to
// one and a stroke one source pixel wide inks 4.25 destination pixels as four
// whole ones and a quarter-covered fifth rather than as four on one row and
// five on the next. On the downscale every source sample under the footprint
// is taken whole (weight one) and any lit one inks the pixel, so a stroke
// thinner than a destination pixel still inks.
constexpr int axisTapCapacity = VT::drcsMaxMatrixWidth > VT::drcsMaxMatrixHeight ? VT::drcsMaxMatrixWidth : VT::drcsMaxMatrixHeight;

struct AxisTaps {
    int count = 0;
    std::array<int, axisTapCapacity> index = {};
    std::array<double, axisTapCapacity> weight = {};
};

inline AxisTaps axisTaps(int d, int destination, int source)
{
    AxisTaps taps;
    if (destination >= source) {
        // Footprint of destination pixel d in source units: [d, d+1) * source/destination.
        const double scale = static_cast<double>(source) / static_cast<double>(destination);
        const double from = d * scale;
        const double to = from + scale;
        for (int s = static_cast<int>(std::floor(from)); s < source && s < to; ++s) {
            const double overlap = std::min(to, static_cast<double>(s + 1)) - std::max(from, static_cast<double>(s));
            if (overlap <= 0.0)
                continue;
            taps.index[taps.count] = s;
            taps.weight[taps.count] = overlap / scale;
            ++taps.count;
        }
        return taps;
    }
    int s0 = d * source / destination;
    int s1 = (d + 1) * source / destination;
    if (s1 <= s0)
        s1 = s0 + 1;
    if (s1 > source)
        s1 = source;
    for (int s = s0; s < s1; ++s) {
        taps.index[taps.count] = s;
        taps.weight[taps.count] = 1.0;
        ++taps.count;
    }
    return taps;
}

} // namespace

bool rasterizeDRCSGlyph(const VT::SoftFont& font, int index, IntSize slot, IntRect cell, std::span<std::uint8_t> coverage)
{
    // Bound to scalars once, so the coverage arithmetic below reads plain
    // names. The span is what lets the write be checked against the destination
    // rather than against a number passed beside it.
    const int slotWidth = slot.width;
    const int slotHeight = slot.height;
    const int cellX = cell.origin.x;
    const int cellY = cell.origin.y;
    const int cellWidth = cell.size.width;
    const int cellHeight = cell.size.height;
    if (std::cmp_less(coverage.size(), static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight)))
        return false;
    std::uint8_t* const coverageOut = coverage.data();
    if (!coverageOut || !softFontHasGlyph(font, index))
        return false;
    if (slotWidth <= 0 || slotHeight <= 0 || cellWidth <= 0 || cellHeight <= 0)
        return false;
    if (cellX < 0 || cellY < 0 || cellX + cellWidth > slotWidth || cellY + cellHeight > slotHeight)
        return false;

    // The caps bound the store, but clamp anyway: the matrix dimensions arrive
    // over the wire and this indexes `glyph[][]` with them.
    const int matrixWidth = font.matrixWidth < VT::drcsMaxMatrixWidth ? font.matrixWidth : VT::drcsMaxMatrixWidth;
    const int matrixHeight = font.matrixHeight < VT::drcsMaxMatrixHeight ? font.matrixHeight : VT::drcsMaxMatrixHeight;
    const std::span rows { font.glyph[static_cast<std::size_t>(index)] };
    const bool isUpscaledX = cellWidth >= matrixWidth;
    const bool isUpscaledY = cellHeight >= matrixHeight;

    std::memset(coverageOut, 0, static_cast<std::size_t>(slotWidth) * slotHeight);

    for (int dy = 0; dy < cellHeight; ++dy) {
        const AxisTaps ytaps = axisTaps(dy, cellHeight, matrixHeight);
        // One OR'd mask over the rows this destination row draws on, so a row
        // the design leaves blank costs a test and nothing more.
        std::uint16_t rowMask = 0;
        for (int k = 0; k < ytaps.count; ++k)
            rowMask = static_cast<std::uint16_t>(rowMask | rows[ytaps.index[k]]);
        if (rowMask == 0)
            continue; // the memset already left this row blank

        std::uint8_t* destination = coverageOut + static_cast<std::size_t>(cellY + dy) * slotWidth + cellX;
        for (int dx = 0; dx < cellWidth; ++dx) {
            const AxisTaps xtaps = axisTaps(dx, cellWidth, matrixWidth);
            // Coverage of this destination pixel: the covered fraction along
            // each upscaled axis, any lit sample along each downscaled one.
            double value = 0.0;
            for (int ky = 0; ky < ytaps.count; ++ky) {
                const std::uint16_t bits = rows[ytaps.index[ky]];
                double rowValue = 0.0;
                for (int kx = 0; kx < xtaps.count; ++kx) {
                    if (((bits >> xtaps.index[kx]) & 1u) == 0)
                        continue;
                    rowValue = isUpscaledX ? rowValue + xtaps.weight[kx] : 1.0;
                }
                value = isUpscaledY ? value + ytaps.weight[ky] * rowValue : std::max(value, rowValue);
            }
            destination[dx] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
        }
    }
    return true;
}

} // namespace Ubiquitty::Rendering
