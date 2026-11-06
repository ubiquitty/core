// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Rendering/fuzz_png.cpp — libFuzzer harness for the PNG decoder.
//
// PNG bytes are untrusted: they arrive as inline-image payloads (Kitty t=d /
// iTerm 1337) straight off the terminal byte stream. Codec::PNG::decode parses an
// attacker-shaped IHDR (width/height/depth/colorType), walks chunk lengths, and
// unfilters scanlines into a fixed scratch arena — classic integer-overflow /
// OOB-write territory. Drive it with arbitrary bytes; ASan catches any OOB in
// the dimension/chunk/scanline math, and the post-decode asserts pin the
// returned pixels strictly inside the scratch arena.

#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"

#include <cassert>
#include <climits>
#include <cstdint>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > static_cast<size_t>(INT_MAX))
        return 0;

    // Bounded decode arena; pixels view into it. Codec::PNG::decode resets the
    // inflater on each call, so a static instance is sufficient.
    static uint8_t scratch[8 * 1024 * 1024];
    static Ubiquitty::Codec::Zlib::Inflater inflater;

    const Ubiquitty::Codec::PNG::DecodeResult out = Ubiquitty::Codec::PNG::decode({ data, size }, { scratch, sizeof(scratch) }, inflater);

    if (out.status == Ubiquitty::Codec::PNG::DecodeStatus::Success) {
        // A successful decode must keep its pixel run wholly within scratch.
        assert(out.pixels.data() >= scratch);
        assert(out.pixels.data() + out.pixels.size() <= scratch + sizeof(scratch));
    }
    return 0;
}
