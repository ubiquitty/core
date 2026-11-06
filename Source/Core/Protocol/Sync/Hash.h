// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Hash.h — the two hashes the Sync screen plane is checked by.
//
// A row is named by XXH3-64 over its canonical encoding, and everything an
// update produces is named by one XXH3-128 state digest (Docs/sync.md §6.2).
// The row hash is a cache key and a diff key, never a
// correctness boundary: a host reuses a cached encoding only after a memcmp of
// the row it encoded, and a client's divergence is caught by the digest, which
// is wide enough that a row-hash collision cannot also hide in it.
//
// Both sides hash bytes, never structures, so the value depends on the wire
// form alone and not on either end's layout.
//

#include <cstdint>
#include <span>

namespace Ubiquitty::Protocol::Sync {

/// A 128-bit digest, serialized as little-endian low then high words.
struct Digest128 {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    friend constexpr bool operator==(const Digest128&, const Digest128&) = default;
};

/// Hashes one row's canonical encoding.
///
/// - Parameter encoding: The row record exactly as it travels.
/// - Returns: XXH3-64 of the bytes, seed 0.
[[nodiscard]] std::uint64_t rowHash(std::span<const std::uint8_t> encoding);

/// Hashes a block of bytes to 64 bits, for a section the digest names by hash.
///
/// - Parameter bytes: The block as it travels.
/// - Returns: XXH3-64 of the bytes, seed 0.
[[nodiscard]] std::uint64_t blockHash(std::span<const std::uint8_t> bytes);

/// Hashes the digest input to 128 bits.
///
/// - Parameter bytes: The serialized digest input; see `StateDigestInput`.
/// - Returns: XXH3-128 of the bytes, seed 0.
[[nodiscard]] Digest128 digestOf(std::span<const std::uint8_t> bytes);

/// Addresses decoded pixels by the first 16 bytes of SHA-256 over the ASCII
/// domain `Ubiquitty.Image` plus NUL, format:u8, width:u32LE, height:u32LE and
/// pixels. Digest bytes keep their order on the wire. No image ID or placement
/// participates, so moving the same pixels does not mint another asset.
[[nodiscard]] Digest128 imageHash(std::span<const std::uint8_t> pixels, std::uint8_t format, std::uint32_t width, std::uint32_t height);

} // namespace Ubiquitty::Protocol::Sync
