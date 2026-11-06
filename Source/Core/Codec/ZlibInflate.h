// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"

#include <cstddef>
#include <span>

// Allocator-free decoder for the zlib container (RFC 1950) wrapping a deflate
// stream (RFC 1951), backed by the vendored Wuffs library. Thin adapter owning
// the decoder state.
//
// Streaming: caller hands in input and output spans each call; decode() consumes
// and produces as much as it can, then suspends with NeedMoreInput /
// NeedMoreOutput and the consumed/produced counts. Done means the adler32
// trailer was read and VERIFIED.
//
// The deflate history window lives inside the decoder state, so the caller may
// use a different output span each call — bytes are never read back from it.
//
// No malloc: the wuffs decoder lives by value in an opaque size-asserted storage
// blob (see WuffsCodec.h for the compile-time fit proof).

namespace Ubiquitty::Codec::Zlib {

enum class InflateStatus : uint8_t {
    Progressed, // progressed; call again
    NeedMoreInput, // exhausted input, feed more
    NeedMoreOutput, // exhausted output, drain and call again
    Done, // stream complete, adler32 verified
    Error // malformed input or adler mismatch
};

constexpr int inflateWindowSize = 32768;

class Inflater {
public:
    Inflater() { reset(); }

    // Return to the fresh-stream state; an Inflater is reusable via reset().
    void reset();

    struct DecodeResult {
        InflateStatus status { InflateStatus::Error };
        std::size_t bytesConsumed { 0 };
        std::size_t bytesProduced { 0 };
    };
    [[nodiscard]] DecodeResult decode(std::span<const uint8_t> input, std::span<uint8_t> output);

    // Storage seam for Codec::PNG::decode: re-uses this object's storage for the sibling
    // wuffs PNG decoder so decode state never hits malloc. Claiming destroys any
    // in-progress zlib stream: decode() reports Error until the next reset().
    static constexpr std::size_t codecStorageCapacity = 49152;
    [[nodiscard]] std::span<uint8_t> claimCodecStorage();

private:
    // Opaque by-value home of the wuffs_zlib__decoder (or, once claimed, the PNG
    // path's wuffs_png__decoder). Raw bytes; capacity static_asserted against the
    // real struct sizes in WuffsCodec.cpp.
    alignas(8) uint8_t m_storage[codecStorageCapacity];

    // Cumulative stream offsets across suspensions. Carried in meta.pos to keep
    // wuffs' back-reference distance validation exact when the caller switches
    // buffers between calls.
    uint64_t m_totalConsumed { 0 };
    uint64_t m_totalProduced { 0 };

    // Absorbing terminal states: decode() answers from these without touching
    // wuffs once the stream has finished, failed, or been claimed.
    bool m_streamComplete { false };
    bool m_usable { false };
};

// One-shot decode of the complete zlib stream into `output`, which must be sized
// to the EXACT expected output length. Returns true only when the stream decodes
// to exactly output.size() bytes with a valid adler32.
[[nodiscard]] bool inflateExact(std::span<const uint8_t> input, std::span<uint8_t> output);

} // namespace Ubiquitty::Codec::Zlib
