// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Hash.h"

#include "Core/Crypto/SHA256.h"

// The vendored header is compiled into this translation unit alone, inlined,
// so no other unit sees its macros and no symbol of it is exported.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#define XXH_INLINE_ALL
#include "ThirdParty/xxhash/xxhash.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Ubiquitty::Protocol::Sync {

std::uint64_t rowHash(std::span<const std::uint8_t> encoding)
{
    return XXH3_64bits(encoding.data(), encoding.size());
}

std::uint64_t blockHash(std::span<const std::uint8_t> bytes)
{
    return XXH3_64bits(bytes.data(), bytes.size());
}

Digest128 digestOf(std::span<const std::uint8_t> bytes)
{
    const XXH128_hash_t hash = XXH3_128bits(bytes.data(), bytes.size());
    return { .low = hash.low64, .high = hash.high64 };
}

Digest128 imageHash(std::span<const std::uint8_t> pixels, std::uint8_t format, std::uint32_t width, std::uint32_t height)
{
    // The fixed domain and metadata precede pixels, so native and foreign
    // readers hash the same bytes without an image-sized concatenation.
    const std::uint8_t prefix[] { 'U', 'b', 'i', 'q', 'u', 'i', 't', 't', 'y', '.', 'I', 'm', 'a', 'g', 'e', 0, format,
        static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8),
        static_cast<std::uint8_t>(width >> 16), static_cast<std::uint8_t>(width >> 24),
        static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(height >> 8),
        static_cast<std::uint8_t>(height >> 16), static_cast<std::uint8_t>(height >> 24) };
    std::array<std::uint8_t, 32> digest {};
    Crypto::sha256(prefix, pixels, digest);
    Digest128 hash;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        hash.low |= static_cast<std::uint64_t>(digest[byte]) << (8 * byte);
        hash.high |= static_cast<std::uint64_t>(digest[8 + byte]) << (8 * byte);
    }
    return hash;
}

} // namespace Ubiquitty::Protocol::Sync
