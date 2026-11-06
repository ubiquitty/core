// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Ubiquitty::Codec::Zlib {
class Inflater;
}

// Minimal PNG decoder for kitty graphics `f=100` payloads, backed by the
// vendored Wuffs PNG decoder. Supports only the subset kitty mandates; anything
// else is rejected:
// - depth 8
// - colour type 2 (RGB, 3 bytes/pixel) or 6 (RGBA, 4 bytes/pixel)
// - compression method 0 (deflate), filter method 0, interlace method 0 (no Adam7)
//
// Checksums (chunk CRCs, IDAT adler32) are not enforced: kitty payloads are
// already framed by the terminal stream, and wuffs' bounds proofs keep a
// corrupted stream memory-safe.
//
// No malloc. Precondition: scratch >= width*height*bytesPerPixel +
// height*(1 + width*bytesPerPixel), else ScratchTooSmall; decoded pixels land at
// its front (`pixels` views into it). The Inflater's codec storage houses the
// PNG decoder for the call (claimCodecStorage), so it must be owned per-call and
// is left needing reset().

namespace Ubiquitty::Codec::PNG {

/// The eight bytes every PNG stream opens with.
///
/// One definition because three places need it — the decoder skipping it, the
/// encoder writing it, and the kitty graphics path deciding whether a payload
/// is a PNG at all — and three copies of a magic number agree only while
/// nobody edits one. The array carries its own length, so the 8 beside it stops
/// being a second thing to keep in step.
inline constexpr std::array<std::uint8_t, 8> signature { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

/// Whether a payload opens with the PNG signature.
///
/// - Parameter bytes: The payload; a run shorter than the signature is not one.
/// - Returns: True when the first eight bytes match.
[[nodiscard]] inline bool hasSignature(std::span<const std::uint8_t> bytes)
{
    return bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(), bytes.begin());
}

/// Why a PNG decode stopped, or that it did not.
///
/// Every enumerator carries an explicit value because ``Image::TIFF`` declares
/// an enum of the same name sharing `Success`, `BadSignature`, `Truncated`,
/// `Unsupported` and `ScratchTooSmall`. Two positional enums over overlapping
/// names agree only while nobody inserts into either, and inserting is the
/// natural edit — a new failure mode belongs beside the one it resembles, which
/// is the middle. Pinned, the two can be renumbered independently and a reader
/// comparing them is comparing values rather than positions.
enum class DecodeStatus : uint8_t {
    Success = 0,
    BadSignature = 1,
    Truncated = 2,
    BadIHDR = 3,
    Unsupported = 4,
    ScratchTooSmall = 5,
    InflateFailed = 6,
    BadFilter = 7,
    MissingChunks = 8,
    SizeMismatch = 9,
};

struct DecodeResult {
    int width = 0;
    int height = 0;
    int depth = 0;
    int colorType = 0;
    int bytesPerPixel = 0; // after decode: 3 (RGB) or 4 (RGBA)
    std::span<const uint8_t> pixels; // views into the caller's scratch
    DecodeStatus status = DecodeStatus::BadSignature;
};

[[nodiscard]] DecodeResult decode(std::span<const uint8_t> input, std::span<uint8_t> scratch, Codec::Zlib::Inflater& inflater);

} // namespace Ubiquitty::Codec::PNG
