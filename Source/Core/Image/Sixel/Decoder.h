// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/Types.h"
#include <cstdint>
#include <span>
#include <string_view>

// DEC sixel graphics decoder. Consumes the payload of a DCS sixel sequence and
// writes a palette-indexed bitmap into a caller-supplied buffer. No allocation.
//
// Sixel encoding:
// - Printable bytes 0x3F..0x7E each encode six vertical pixels in one column:
//   bit 0 = topmost.
// - "#Pc"               select palette entry Pc
// - "#Pc;Pu;Px;Py;Pz"   define + select entry Pc. Pu=1 → DEC HLS (hue 0..360,
//                       lightness/saturation 0..100); Pu=2 → RGB (0..100 percent).
// - "!Pn ch"            repeat sextet ch Pn times
// - "-"                 advance one sixel-row (y += 6, x = 0)
// - "$"                 carriage-return within the sixel-row
// - ""Pan;Pad;Ph;Pv"    raster attributes; Ph/Pv set canvas extent.
//
// The caller's bitmap has row stride = maxWidth and room for maxHeight rows. The
// decoder pre-clears the active region to palette index 0. Pixels outside the
// supplied area set the truncated flag and are dropped.

namespace Ubiquitty::Image::Sixel {

struct Image {
    uint16_t width; // decoded image width in pixels
    uint16_t height; // decoded image height in pixels
    uint16_t stride; // row stride in the pixels[] buffer (= maxWidth)
    uint8_t palette[256][3]; // RGB 0..255 per palette index
    uint8_t* pixels; // row-major; pixels[y * stride + x] = palette index
    // Optional per-pixel written mask (same stride): 1 = a sixel bit painted
    // this pixel, 0 = never painted. Null when the caller didn't request it.
    // Honoring sixel P2=1 (transparent background) needs this to tell a pixel
    // explicitly set to color 0 from one that was never touched.
    uint8_t* opaque;
    bool truncated; // true if image dimensions exceeded the buffer
    // Raster pixel-aspect from a "Pan;Pad;Ph;Pv command (0 = none given).
    // Pan:Pad is the vertical:horizontal pixel aspect; overrides the legacy DCS Pa.
    uint16_t rasterPan;
    uint16_t rasterPad;
};

/// The byte size of one palette: 256 registers of RGB.
constexpr std::size_t paletteBytes = 256 * 3;

/// Writes the DEC default palette: the sixteen VT340 colours, black beyond.
///
/// - Parameter palette: The registers to seed, ``paletteBytes`` long.
void defaultPalette(std::span<std::uint8_t, paletteBytes> palette);

/// Why a sixel decode stopped, or that it did not.
enum class DecodeStatus : int {
    OK = 0,
    MalformedInput = -1,
    InvalidBuffer = -2
};

// Decode the sixel payload `bytes` into `bitmap` (maxWidth * maxHeight bytes,
// pre-cleared to palette entry 0). Fills out->{pixels, stride=maxWidth, width,
// height, palette, truncated}. Returns OK; InvalidBuffer if maxWidth/maxHeight is
// zero; MalformedInput never (unknown bytes are skipped). `opaqueMask`, when
// non-null (also maxWidth * maxHeight), gets 1 wherever a sixel bit paints, so
// the caller can render never-painted pixels transparent under sixel P2=1.
// `initialPalette`, when it is `paletteBytes` long, is the registers the image
// starts from (xterm ?1070 reset: the previous image's); empty starts from the
// DEC default palette.
DecodeStatus decode(std::string_view bytes, uint8_t* bitmap, uint16_t maxWidth, uint16_t maxHeight, Image* out, uint8_t* opaqueMask = nullptr, std::span<const std::uint8_t> initialPalette = {});

/// Rows one sixel byte encodes, and so the height of one band.
constexpr int bandHeight = 6;

/// What one payload will cost `decode`: the canvas it fills and the work it does.
struct PayloadMeasure {
    /// The smallest canvas `decode` can fill, never larger than the ceiling.
    Ubiquitty::IntSize canvas;
    /// Column emissions `decode` will perform in total — the term the canvas does
    /// not bound. `$` repaints the current band and `-` moves past the canvas
    /// bottom, and neither stops the emit loop from running: both spend work per
    /// byte of payload while the canvas stays the size it was.
    long long emittedColumns;
};

/// Measures a payload without decoding it.
///
/// `decode` writes and clears `canvas.width * canvas.height` bytes twice over —
/// once for the bitmap and once for the opaque mask — so sizing those from the
/// ceiling makes a two-byte sixel cost what a full-canvas one does. Scanning
/// first costs one pass over bytes that are about to be parsed anyway.
///
/// The canvas covers what `decode` will report, not only what the payload
/// paints: a raster attribute sets the reported size directly, so a
/// `"1;1;800;600` carrying two pixels still needs the canvas it declared or it
/// would come back truncated.
///
/// `emittedColumns` is the separate question of how long the decode runs. A `!`
/// run is capped against the canvas, but `$` returns to column zero and `-` walks
/// past the canvas bottom, and the emit loop keeps iterating either way: both
/// fill a 16 MiB payload with billions of emissions over a canvas of four million
/// pixels. Counting mirrors that loop, including its stop at the row edge, so a
/// caller can price the decode before starting it.
///
/// - Parameters:
///   - bytes: The sixel payload, the same bytes `decode` will read.
///   - ceiling: The largest canvas allowed; neither axis of the result exceeds it.
/// - Returns: The canvas, each axis at least 1, and the column count.
[[nodiscard]] PayloadMeasure measurePayload(std::string_view bytes, Ubiquitty::IntSize ceiling);

} // namespace Ubiquitty::Image::Sixel
