// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Rendering/fuzz_tiff.cpp — libFuzzer harness for the TIFF decoder.
//
// TIFF bytes are untrusted: chafa's `-f iterm` emits baseline uncompressed
// TIFF, which arrives as an iTerm OSC 1337 inline-image payload straight off
// the terminal byte stream (Source/Core/OSC/ITerm2.cpp decodeContainerToRGBA).
// Image::TIFF::decode parses an attacker-shaped IFD — width/height, SamplesPerPixel,
// RowsPerStrip, and per-strip offsets/byte-counts — then copies RGBA rows into
// a fixed scratch arena. The strip-offset / RowsPerStrip / SamplesPerPixel
// product math is the integer-overflow → OOB-write surface.
// Drive it with arbitrary bytes; ASan flags any OOB in the dimension/strip/row
// math, UBSan flags the integer overflow itself, and the post-decode asserts
// pin the returned pixels strictly inside the scratch arena so a silent
// over-write that happens to land in-arena is still caught.

#include "Core/Image/TIFFDecoder.h"

#include <cassert>
#include <climits>
#include <cstdint>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > static_cast<size_t>(INT_MAX))
        return 0;

    // Bounded decode arena; pixels point inside it. Matches fuzz_png's 8 MiB
    // arena — comfortably above the OSC 1337 call site's per-image cap, so a
    // legitimate decode never trips ScratchTooSmall but an OOB write still hits
    // ASan's redzone past the end.
    static uint8_t scratch[8 * 1024 * 1024];

    const Ubiquitty::Image::TIFF::DecodeResult out = Ubiquitty::Image::TIFF::decode({ data, size }, { scratch, sizeof(scratch) });

    if (out.status == Ubiquitty::Image::TIFF::DecodeStatus::Success) {
        // A successful decode must keep its whole RGBA run inside scratch, and
        // the pixel span must equal the dimension math (width * height * 4
        // bytes/pixel). Recompute in 64-bit and bound-check independently of
        // the decoder so a silent over-write that lands in-arena is still caught.
        assert(out.width > 0 && out.height > 0);
        const long long bytesPerPixel = 4;
        const long long expected = static_cast<long long>(out.width) * static_cast<long long>(out.height) * bytesPerPixel;
        assert(static_cast<long long>(out.pixels.size()) == expected);
        assert(out.pixels.data() >= scratch);
        assert(out.pixels.size() <= sizeof(scratch));
        assert(out.pixels.data() + out.pixels.size() <= scratch + sizeof(scratch));
    }
    return 0;
}
