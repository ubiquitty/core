// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Rendering/fuzz_zlib.cpp — libFuzzer harness for the inflate decoder.
//
// The zlib/inflate path is the decompressor under PNG IDAT and Kitty o=z image
// payloads — all attacker-supplied. Codec::Zlib::inflateExact builds Huffman tables from
// the stream, copies back-references through a sliding window, and writes into a
// fixed output buffer: Huffman/window/output OOB are the failure modes. Treat
// the fuzz bytes as a raw zlib stream; ASan catches any out-of-bounds.

#include "Core/Codec/ZlibInflate.h"

#include <climits>
#include <cstdint>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > static_cast<size_t>(INT_MAX))
        return 0;

    // Bounded output arena — a hostile stream must never write past it.
    static uint8_t outputBuffer[4 * 1024 * 1024];
    (void)Ubiquitty::Codec::Zlib::inflateExact({ data, size }, { outputBuffer, sizeof(outputBuffer) });
    return 0;
}
