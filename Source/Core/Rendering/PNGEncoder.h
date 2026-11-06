// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Rendering/PNGEncoder.h — encode an RGBA8 image to PNG bytes with no
// third-party compressor: a self-contained static-Huffman + LZ77 DEFLATE encoder
// (the inverse of the repo's ZlibInflate). Portable; used by the headless
// screenshot rasterizer.
//

#include "Base/Types.h"

#include "Base/Geometry.h"

#include <span>
#include <type_traits>
#include <vector>

namespace Ubiquitty::Rendering {

// PNG's colour type 6 is four 8-bit channels, and the encoder writes that
// header unconditionally. The stride arithmetic below multiplies by the same 4.
// Named once and asserted, so a future format change breaks here rather than
// emitting a header that disagrees with the bytes after it.
inline constexpr int rgbaChannelCount = 4;
static_assert(rgbaChannelCount == 4, "PNG colour type 6 is RGBA; the IHDR writer assumes it");

// The dimensions travel as one value. IntSize is two ints and nothing else, so
// passing it costs what passing the pair cost — checked rather than assumed,
// because a type that grew a member would silently change this seam's ABI.
static_assert(sizeof(IntSize) == 2 * sizeof(int));
static_assert(std::is_trivially_copyable_v<IntSize>);

/// The byte range passed from Apple snapshot code through Swift C++ interop.
using PNGPixels = std::span<const uint8_t>;

/// Encodes an RGBA image as a PNG file.
///
/// - Parameters:
///   - rgba: The pixels, row-major and top-down, 8 bits per channel. Must hold
///     exactly `size.width * size.height * 4` bytes, which this checks: a
///     buffer sized for other dimensions is refused rather than read past.
///
///     The check is on the total, so it catches a buffer of the wrong size and
///     not a transposed one: 8x2 and 4x4 are both 128 bytes and both encode.
///     Passing the dimensions as one value is what makes the transposition hard
///     to write; nothing here can detect it after the fact.
///   - size: The image's dimensions in pixels.
/// - Returns: The PNG file bytes, or empty when the dimensions are not positive
///   or `rgba` is not the size they describe.
std::vector<uint8_t> encodePNG(PNGPixels rgba, IntSize size);

} // namespace Ubiquitty::Rendering
