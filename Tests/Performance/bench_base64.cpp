// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_base64.cpp — Codec::base64Decode and base64Encode over the payload sizes
// the escape sequences carry.
//
// Base64 is on the image path, not beside it: kitty transmits pixels through it,
// OSC 1337 File= transmits a whole encoded image through it, and OSC 52 moves the
// clipboard through it. A 4 MiB framebuffer arrives as 5.5 MiB of text, and every
// byte of that goes through this codec before anything is drawn.
//
// Three sizes, because the answer is not one number: a 4 KiB placement is
// dominated by whatever fixed cost the call has, and a 3 MiB frame is dominated
// by memory bandwidth. A change that helps one can cost the other.
//
// Decode is measured on well-formed padded input, which is what a producer emits
// and therefore what the SIMD path actually runs on. A corpus of rejects would
// measure the error exit instead — every malformed input leaves through the same
// branch, so its cost says nothing about the throughput this lane exists to hold.

#include "BenchHarness.h"

#include "Core/Codec/Base64.h"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Bench;

namespace {

// A deterministic payload: the same bytes every run, so two runs a week apart
// differ by the machine and the build rather than by the corpus.
std::vector<char> payloadBytes(std::size_t byteCount)
{
    std::vector<char> raw(byteCount);
    std::uint32_t state = 0x9E3779B9u;
    for (std::size_t i = 0; i < byteCount; ++i) {
        state = state * 1664525u + 1013904223u;
        raw[i] = static_cast<char>(state >> 24);
    }
    return raw;
}

struct PayloadSize {
    const char* name;
    std::size_t bytes;
};

} // namespace

int main()
{
    // 4 KiB is a small placement, 256 KiB a modest image, 3 MiB a framebuffer
    // frame near kitty's own accumulated-payload ceiling.
    const PayloadSize sizes[] = { { "4KiB", 4 * 1024 }, { "256KiB", 256 * 1024 }, { "3MiB", 3 * 1024 * 1024 } };
    std::vector<Result> results;

    for (const PayloadSize& size : sizes) {
        const std::vector<char> raw = payloadBytes(size.bytes);
        std::vector<char> encoded(Ubiquitty::Codec::base64EncodedLength(size.bytes));
        const auto characters = Ubiquitty::Codec::base64Encode(raw, encoded);
        if (!characters || characters->size() != encoded.size()) {
            std::fprintf(stderr, "bench_base64: encode wrote %zu of %zu — the lane is measuring nothing\n",
                characters ? characters->size() : 0, encoded.size());
            return 1;
        }
        std::vector<char> decoded(size.bytes);

        // The decode is checked once outside the timed region, so a codec that
        // started rejecting this input would fail here rather than post a
        // throughput number for an early return.
        const std::string_view encodedView(encoded.data(), encoded.size());
        const auto checked = Ubiquitty::Codec::base64Decode(encodedView, decoded);
        if (!checked || checked->size() != size.bytes) {
            std::fprintf(stderr, "bench_base64: decode refused a payload it encoded — the lane is measuring nothing\n");
            return 1;
        }

        const Timing decodeTiming = measure(3, 25, [&] {
            auto written = Ubiquitty::Codec::base64Decode(encodedView, decoded);
            doNotOptimize(written);
        });
        const Timing encodeTiming = measure(3, 25, [&] {
            auto written = Ubiquitty::Codec::base64Encode(raw, encoded);
            doNotOptimize(written);
        });

        // Per decoded byte for decode and per input byte for encode: both are
        // the payload the terminal actually holds, so the two rates compare.
        results.push_back(throughput(std::string("base64_decode_") + size.name, static_cast<double>(size.bytes), decodeTiming));
        results.push_back(throughput(std::string("base64_encode_") + size.name, static_cast<double>(size.bytes), encodeTiming));
    }

    emitJSON(results, "core", std::getenv("UBIQUITTY_BENCH_COMMIT"));
    return 0;
}
