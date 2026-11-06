// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// RGB/RGBA depth-8 PNG decoder for kitty `f=100`, backed by the vendored Wuffs
// PNG decoder. Two passes:
// 1. Structural walk (no decompression): signature, chunk bounds, IHDR subset
//    policy, IHDR/IDAT/IEND presence. Fires every pre-decode DecodeStatus with a
//    precise value (finer than wuffs' status strings), rejecting hostile
//    IHDR/chunk-length values with arithmetic-safe checks.
// 2. Pixel decode (wuffs): inflate + unfilter + swizzle into the caller's scratch
//    in PNG's own byte layout (RGB / nonpremul RGBA), so output needs no conversion.
//
// Checksums (chunk CRCs, IDAT adler32) run with the ignore-checksum quirk: kitty
// payloads are already framed by the terminal stream, and wuffs' bounds proofs
// keep a corrupted stream memory-safe.
//
// No malloc. The wuffs decoder is carved from the caller's Inflater via
// claimCodecStorage() (mutated throughout, so must be owned per-call). Pixels
// land at scratch[0 .. width*height*bytesPerPixel); the wuffs workbuf follows
// (exactly height*(1 + width*bytesPerPixel)). If they don't fit, ScratchTooSmall.

#include "Core/Codec/PNGDecoder.h"
#include "Base/Types.h"

#include "Core/Codec/WuffsCodec.h"
#include "Core/Codec/ZlibInflate.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>

namespace Ubiquitty::Codec::PNG {

// Reject crafted IHDRs with dimensions larger than any real payload uses; the
// cap keeps w*h*bpp clear of INT_MAX before arithmetic can wrap.
static const int maxDimension = 32768;

// Cap the total raw inflated size (4K RGBA = 64 MiB). Rejected before decoding.
static const size_t maxRawBytes = static_cast<size_t>(64) * 1024 * 1024;

static uint32_t readBigEndian32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]); }

// Map a wuffs decode failure onto the caller-facing status vocabulary.
// truncated_input (IDAT ended before the advertised scanlines) → SizeMismatch;
// bad_filter → BadFilter; every other pixel-pipeline failure → InflateFailed.
static DecodeStatus mapWuffsDecodeFailure(const wuffs_base__status& status)
{
    if (status.repr == wuffs_png__error__bad_filter)
        return DecodeStatus::BadFilter;
    if (status.repr == wuffs_png__error__truncated_input)
        return DecodeStatus::SizeMismatch;
    return DecodeStatus::InflateFailed;
}

DecodeResult decode(std::span<const uint8_t> input, std::span<uint8_t> scratch, Codec::Zlib::Inflater& inflater)
{
    const uint8_t* in = input.data();
    const int inputLength = saturatingCast<int>(input.size());
    int pos = 8;
    bool haveIHDR = false;
    bool haveIEND = false;
    bool haveIDAT = false;
    size_t pixelBytes = 0;
    size_t rawBytes = 0;

    DecodeResult result;
    result.status = DecodeStatus::Success;

    if (!in || !hasSignature(input)) {
        result.status = DecodeStatus::BadSignature;
        return result;
    }
    if (scratch.empty()) {
        result.status = DecodeStatus::ScratchTooSmall;
        return result;
    }

    // Pass 1 — structural walk, so every pre-decode rejection carries its
    // precise status independent of how wuffs would word the failure.
    while (pos + 8 <= inputLength) {
        uint32_t chunkLength;
        uint32_t chunkType;
        const uint8_t* chunkData;

        chunkLength = readBigEndian32(in + pos);
        chunkType = readBigEndian32(in + pos + 4);
        pos += 8;
        // Reject any chunkLength that won't fit in the remaining input (incl. the
        // 4-byte CRC). uint32_t throughout so hostile values can't wrap a signed int.
        if (chunkLength > static_cast<uint32_t>(0x7FFFFFFF)) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        if (chunkLength > static_cast<uint32_t>(inputLength - pos) || static_cast<uint32_t>(inputLength - pos) - chunkLength < 4) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        chunkData = in + pos;

        if (chunkType == 0x49484452u) {
            int w, h;
            int depth, ct, cm, fm, il;
            if (haveIHDR || chunkLength != 13) {
                result.status = DecodeStatus::BadIHDR;
                return result;
            }
            w = static_cast<int>(readBigEndian32(chunkData));
            h = static_cast<int>(readBigEndian32(chunkData + 4));
            depth = chunkData[8];
            ct = chunkData[9];
            cm = chunkData[10];
            fm = chunkData[11];
            il = chunkData[12];
            if (w <= 0 || h <= 0) {
                result.status = DecodeStatus::BadIHDR;
                return result;
            }
            if (w >= maxDimension || h >= maxDimension) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
            if (depth != 8) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
            if (ct != 2 && ct != 6) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
            if (cm != 0 || fm != 0 || il != 0) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
            const int bytesPerPixel = (ct == 2) ? 3 : 4;
            // size_t math throughout to stay clear of INT_MAX before the cap.
            const size_t rawRowSizeBytes = static_cast<size_t>(1) + static_cast<size_t>(w) * static_cast<size_t>(bytesPerPixel);
            rawBytes = rawRowSizeBytes * static_cast<size_t>(h);
            if (rawBytes > maxRawBytes) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
            pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(bytesPerPixel);
            // Scratch must hold the decoded pixels plus the wuffs workbuf
            // (the filtered-scanline total, rawBytes).
            if (pixelBytes > scratch.size() || rawBytes > scratch.size() - pixelBytes) {
                result.status = DecodeStatus::ScratchTooSmall;
                return result;
            }
            result.width = w;
            result.height = h;
            result.depth = depth;
            result.colorType = ct;
            result.bytesPerPixel = bytesPerPixel;
            haveIHDR = true;
        } else if (chunkType == 0x49444154u) {
            if (!haveIHDR) {
                result.status = DecodeStatus::MissingChunks;
                return result;
            }
            haveIDAT = true;
        } else if (chunkType == 0x49454e44u) {
            haveIEND = true;
        }
        // Skip data + CRC (CRC not verified).
        pos += static_cast<int>(chunkLength) + 4;
        if (haveIEND)
            break;
    }

    if (!haveIHDR || !haveIEND) {
        result.status = DecodeStatus::MissingChunks;
        return result;
    }
    if (!haveIDAT) {
        // Structurally complete but no scanlines: header promised pixels the
        // data cannot supply.
        result.status = DecodeStatus::SizeMismatch;
        return result;
    }

    // Pass 2 — wuffs pixel decode. The decoder overwrites the caller's Inflater
    // storage; WuffsCodec.cpp proves it fits, so the guards below only fail closed.
    std::span<uint8_t> decoderStorage = inflater.claimCodecStorage();
    if (sizeof__wuffs_png__decoder() > decoderStorage.size()) {
        result.status = DecodeStatus::InflateFailed;
        return result;
    }
    wuffs_png__decoder* decoder = reinterpret_cast<wuffs_png__decoder*>(decoderStorage.data());
    wuffs_base__status status = wuffs_png__decoder__initialize(decoder, sizeof__wuffs_png__decoder(), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) {
        result.status = DecodeStatus::InflateFailed;
        return result;
    }
    // Set the ignore-checksum quirk before any decoding so the first chunk is exempt.
    status = wuffs_png__decoder__set_quirk(decoder, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);
    if (!wuffs_base__status__is_ok(&status)) {
        result.status = DecodeStatus::InflateFailed;
        return result;
    }

    // Whole file in memory, so source is closed: a dry IDAT becomes
    // truncated_input (→ SizeMismatch) instead of suspending forever.
    wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader(const_cast<uint8_t*>(in), static_cast<size_t>(inputLength), true);

    wuffs_base__image_config imageConfig = wuffs_base__null_image_config();
    status = wuffs_png__decoder__decode_image_config(decoder, &imageConfig, &source);
    if (!wuffs_base__status__is_ok(&status)) {
        result.status = mapWuffsDecodeFailure(status);
        return result;
    }
    // A disagreement between pass 1 and wuffs on the IHDR means the subset policy
    // drifted; fail closed rather than trust either.
    if (wuffs_base__pixel_config__width(&imageConfig.pixcfg) != static_cast<uint32_t>(result.width) || wuffs_base__pixel_config__height(&imageConfig.pixcfg) != static_cast<uint32_t>(result.height)) {
        result.status = DecodeStatus::BadIHDR;
        return result;
    }

    // Decode into PNG's own byte layout (RGB / nonpremul RGBA) — no conversion.
    const uint32_t pixelFormat = (result.colorType == 2) ? WUFFS_BASE__PIXEL_FORMAT__RGB : WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL;
    wuffs_base__pixel_config__set(&imageConfig.pixcfg, pixelFormat, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, static_cast<uint32_t>(result.width), static_cast<uint32_t>(result.height));

    wuffs_base__pixel_buffer pixelBuffer;
    status = wuffs_base__pixel_buffer__set_from_slice(&pixelBuffer, &imageConfig.pixcfg, wuffs_base__make_slice_u8(scratch.data(), pixelBytes));
    if (!wuffs_base__status__is_ok(&status)) {
        result.status = DecodeStatus::InflateFailed;
        return result;
    }

    // Re-check the workbuf against wuffs' own answer so an upstream formula change
    // degrades to a clean ScratchTooSmall instead of an undersized buffer.
    const uint64_t workbufLength = wuffs_png__decoder__workbuf_len(decoder).max_incl;
    if (workbufLength > scratch.size() - pixelBytes) {
        result.status = DecodeStatus::ScratchTooSmall;
        return result;
    }

    status = wuffs_png__decoder__decode_frame(decoder, &pixelBuffer, &source, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(scratch.data() + pixelBytes, static_cast<size_t>(workbufLength)), nullptr);
    if (!wuffs_base__status__is_ok(&status)) {
        result.status = mapWuffsDecodeFailure(status);
        return result;
    }

    result.pixels = { scratch.data(), pixelBytes };
    return result;
}

} // namespace Ubiquitty::Codec::PNG
