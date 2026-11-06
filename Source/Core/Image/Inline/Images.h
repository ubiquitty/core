// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

#include <cstdint>
#include <span>

// iTerm2 OSC 1337 File= inline image decoder backend. Owns the
// width/height/preserveAspectRatio token parsing, the base64 + container
// (PNG/TIFF) decode, and installing the result via Ubiquitty::Image.

namespace Ubiquitty::Image::Inline {

/// Decodes an OSC 1337 inline image and places it at the cursor.
///
/// A no-op on malformed input — a missing separator, bad base64, an undecodable
/// container, or a full image store — so a hostile stream cannot do worse than
/// waste the decode.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - arguments: Everything after `File=`: the key-value list, a `:`, then the
///     base64 payload.
void handleInlineImage(Ubiquitty::Terminal& t, const char* arguments);

/// Decodes an encoded image container to tightly packed straight RGBA.
///
/// The frontend's decoder when it installed one (`PAL::imageDecoder`), else
/// Core's own PNG and TIFF decoders. Refused past the size an inline image is
/// held to, so a hostile file cannot make the decode allocate without bound.
///
/// - Parameters:
///   - bytes: The container's bytes.
///   - rgbaOut: Receives a `malloc`'d buffer the caller releases with `std::free`.
///   - widthOut: Receives the width in pixels.
///   - heightOut: Receives the height in pixels.
/// - Returns: Whether the bytes decoded.
[[nodiscard]] bool decodeImageContainer(std::span<const std::uint8_t> bytes, unsigned char** rgbaOut, int* widthOut, int* heightOut);

} // namespace Ubiquitty::Image::Inline
