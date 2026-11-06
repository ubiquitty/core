// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"

#include <cstddef>
#include <span>

// Minimal baseline TIFF decoder for iTerm inline images (chafa's `-f iterm`
// output). Supported subset (else rejected with DecodeStatus::Unsupported):
// - byte order II or MM; 8 bits per sample
// - SamplesPerPixel 3 (RGB) or 4 (RGBA); PhotometricInterpretation 2 (RGB)
// - Compression 1 (uncompressed); PlanarConfiguration 1 (chunky)
// - one or more strips (RowsPerStrip), top-to-bottom (Orientation 1)
// Output is straight RGBA8 into the caller's scratch (`pixels` views into it).
// No malloc.

namespace Ubiquitty::Image::TIFF {

/// Why a TIFF decode stopped, or that it did not.
///
/// Every enumerator carries an explicit value because ``Codec::PNG`` declares an
/// enum of the same name sharing five of these six enumerators — and the two
/// already disagree on what they mean: `Unsupported` is 3 here and 4 there,
/// `ScratchTooSmall` 4 here and 5 there. Nothing converts between them, and
/// nothing would report it if something did, because the values are in range
/// for both. Pinned, the numbering is a decision rather than a consequence of
/// where an enumerator was typed.
enum class DecodeStatus : uint8_t {
    Success = 0,
    BadSignature = 1,
    Truncated = 2,
    Unsupported = 3,
    ScratchTooSmall = 4,
    BadDimensions = 5,
};

struct DecodeResult {
    int width = 0;
    int height = 0;
    std::span<const uint8_t> pixels; // views into the caller's scratch; RGBA8
    DecodeStatus status = DecodeStatus::BadSignature;
};

// Whether `bytes` begins with a TIFF byte-order mark ("II" / "MM" + version 42).
[[nodiscard]] bool isTIFF(std::span<const uint8_t> bytes);

[[nodiscard]] DecodeResult decode(std::span<const uint8_t> input, std::span<uint8_t> scratch);

} // namespace Ubiquitty::Image::TIFF
