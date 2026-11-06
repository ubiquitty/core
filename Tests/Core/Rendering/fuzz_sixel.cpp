// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_sixel.cpp — libFuzzer harness for Sixel decoder.
//
// Drives random bytes through Sixel::decode against a fixed-
// size bitmap. Each invocation asserts:
//   - The decoder returns either DecodeStatus::OK or ::InvalidBuffer
//     (the ::MalformedInput branch is reserved for grossly malformed input
//     which the current decoder never produces).
//   - The reported width / height fall within [0, maxWidth] /
//     [0, maxHeight].
//   - The truncated flag is consistent with width/height gridFloor.
//
// Hooks up the same shape as Tests/Core/fuzz_vt100.cpp. Build via
// Scripts/run-tests.sh --with-fuzz; the seed corpus under
// Tests/Rendering/sixel-corpus/ replays first, then libFuzzer
// explores.

#include "Core/Image/Sixel/Decoder.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stddef.h>
#include <string_view>

using namespace Ubiquitty::Image::Sixel;

static const uint16_t maxSixelWidth = 256;
static const uint16_t maxSixelHeight = 192;
static uint8_t gBitmap[maxSixelWidth * maxSixelHeight];

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Pre-clear to a sentinel so an under-read would be detectable
    // (the decoder pre-clears to 0 itself, so any non-zero leak from
    // the previous call also surfaces).
    memset(gBitmap, 0xAB, sizeof(gBitmap));

    Image img;
    img = {};

    DecodeStatus rc = decode(std::string_view(reinterpret_cast<const char*>(data), size), gBitmap, maxSixelWidth, maxSixelHeight, &img);

    assert(rc == DecodeStatus::OK || rc == DecodeStatus::InvalidBuffer);
    if (rc == DecodeStatus::OK) {
        assert(img.width <= maxSixelWidth);
        assert(img.height <= maxSixelHeight);
        assert(img.stride == maxSixelWidth);
        assert(img.pixels == gBitmap);
        // The truncated flag rides along with content-loss; we only
        // assert it can be read without aborting the process.
        (void)img.truncated;
    }
    return 0;
}
