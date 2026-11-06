// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// SHA-256 (FIPS 180-4) with a two-tier backend, dispatched per-target at compile
// time: UBIQUITTY_QUIC targets use AWS-LC's hardware SHA256(); everything else uses
// the portable implementation below. Both paths are bit-identical. The portable
// path is not constant-time and not for high-rate use.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>

#if defined(UBIQUITTY_QUIC)
#include <openssl/sha.h>
#endif

namespace Ubiquitty::Crypto {

#if !defined(UBIQUITTY_QUIC)
namespace SHA256Detail {

class State {
public:
    void update(std::span<const unsigned char> part)
    {
        while (!part.empty()) {
            if (m_tail == 0 && part.size() >= m_block.size()) {
                compress(part.data());
                part = part.subspan(m_block.size());
                continue;
            }
            const std::size_t take = std::min(part.size(), m_block.size() - m_tail);
            std::memcpy(m_block.data() + m_tail, part.data(), take);
            m_tail += take;
            part = part.subspan(take);
            if (m_tail == m_block.size()) {
                compress(m_block.data());
                m_tail = 0;
            }
        }
    }

    void finalize(std::uint64_t bitLength, std::span<unsigned char, 32> digest)
    {
        // The marker and length need another block when the tail passes byte 55.
        std::size_t k = m_tail;
        m_block[k++] = 0x80;
        if (k > 56) {
            while (k < 64)
                m_block[k++] = 0;
            compress(m_block.data());
            k = 0;
        }
        while (k < 56)
            m_block[k++] = 0;
        for (int i = 7; i >= 0; --i)
            m_block[k++] = static_cast<unsigned char>((bitLength >> (i * 8)) & 0xFF);
        compress(m_block.data());

        for (int i = 0; i < 8; ++i) {
            digest[i * 4] = static_cast<unsigned char>((m_hash[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<unsigned char>((m_hash[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<unsigned char>((m_hash[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<unsigned char>(m_hash[i] & 0xFF);
        }
    }

private:
    // Written without calls in the rounds: at -O0 a helper call per rotation
    // and per element access was most of the cost, and Debug builds hash
    // whole executables with this path.
    void compress(const unsigned char* p)
    {
        static constexpr std::uint32_t K[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t x = w[i - 15];
            const std::uint32_t y = w[i - 2];
            const std::uint32_t s0 = ((x >> 7) | (x << 25)) ^ ((x >> 18) | (x << 14)) ^ (x >> 3);
            const std::uint32_t s1 = ((y >> 17) | (y << 15)) ^ ((y >> 19) | (y << 13)) ^ (y >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t* const h = m_hash.data();
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::array<std::uint32_t, 8> m_hash { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::array<unsigned char, 64> m_block {};
    std::size_t m_tail = 0;
};

} // namespace SHA256Detail
#endif

/// Why an incremental digest refused an operation.
enum class SHA256Error : std::uint8_t {
    Finalized,
    InputTooLong,
    BackendFailure,
};

/// Hashes byte ranges with fixed storage and no retained caller ownership.
///
/// Empty updates are valid. Finalization succeeds once; later updates or
/// finalization refuse without changing the caller's digest. Inputs longer
/// than SHA-256's 64-bit bit count refuse before consuming the new range.
/// AWS-LC supplies the backend when QUIC is enabled; the portable path shares
/// its compression and padding code with the one-shot overloads.
class SHA256Hasher {
public:
    SHA256Hasher()
    {
#if defined(UBIQUITTY_QUIC)
        m_hasBackendFailure = SHA256_Init(&m_state) != 1;
#endif
    }
    SHA256Hasher(const SHA256Hasher&) = delete;
    SHA256Hasher& operator=(const SHA256Hasher&) = delete;

    /// Consumes bytes before returning, retaining at most an unfinished block.
    /// - Parameter bytes: The next consecutive range, borrowed only for this call.
    /// - Returns: Consumption, or refusal before a finalized/overlong update.
    [[nodiscard]] std::expected<void, SHA256Error> update(std::span<const unsigned char> bytes)
    {
        if (m_isFinalized)
            return std::unexpected(SHA256Error::Finalized);
        if (m_hasBackendFailure)
            return std::unexpected(SHA256Error::BackendFailure);
        constexpr auto maxBytes = std::numeric_limits<std::uint64_t>::max() / 8;
        if (bytes.size() > maxBytes - m_byteCount)
            return std::unexpected(SHA256Error::InputTooLong);
#if defined(UBIQUITTY_QUIC)
        if (SHA256_Update(&m_state, bytes.data(), bytes.size()) != 1) {
            m_hasBackendFailure = true;
            return std::unexpected(SHA256Error::BackendFailure);
        }
#else
        m_state.update(bytes);
#endif
        m_byteCount += static_cast<std::uint64_t>(bytes.size());
        return {};
    }

    /// Publishes the complete digest only after the backend finishes successfully.
    /// - Parameter digest: Receives all 32 bytes on the first successful call.
    /// - Returns: Completion, or refusal with the destination unchanged.
    [[nodiscard]] std::expected<void, SHA256Error> finalize(std::span<unsigned char, 32> digest)
    {
        if (m_isFinalized)
            return std::unexpected(SHA256Error::Finalized);
        if (m_hasBackendFailure)
            return std::unexpected(SHA256Error::BackendFailure);
        std::array<unsigned char, 32> complete;
#if defined(UBIQUITTY_QUIC)
        if (SHA256_Final(complete.data(), &m_state) != 1) {
            m_hasBackendFailure = true;
            return std::unexpected(SHA256Error::BackendFailure);
        }
#else
        m_state.finalize(m_byteCount * 8, complete);
#endif
        std::ranges::copy(complete, digest.begin());
        m_isFinalized = true;
        return {};
    }

private:
#if defined(UBIQUITTY_QUIC)
    SHA256_CTX m_state;
#else
    SHA256Detail::State m_state;
#endif
    std::uint64_t m_byteCount = 0;
    bool m_isFinalized = false;
    bool m_hasBackendFailure = false;
};

/// Computes the SHA-256 digest of two consecutive byte ranges without copying.
///
/// Two backends, chosen per target at compile time and bit-identical: builds
/// with `UBIQUITTY_QUIC` use AWS-LC's hardware implementation, everything else
/// the portable one below. The portable path is **not constant-time** and not
/// for high-rate use, so a caller hashing a secret at speed wants the other one.
///
/// - Parameters:
///   - prefix: The bytes before `data` in the hash input.
///   - data: The bytes to hash.
///   - digest: Receives the digest. Its extent is fixed at 32, so a buffer of
///     the wrong size is a compile error rather than a hash written past its end.
inline void sha256(std::span<const unsigned char> prefix, std::span<const unsigned char> data, std::span<unsigned char, 32> digest)
{
#if defined(UBIQUITTY_QUIC)
    SHA256_CTX state;
    SHA256_Init(&state);
    SHA256_Update(&state, prefix.data(), prefix.size());
    SHA256_Update(&state, data.data(), data.size());
    SHA256_Final(digest.data(), &state);
#else
    SHA256Detail::State state;
    state.update(prefix);
    state.update(data);
    const std::uint64_t bitLength = (static_cast<std::uint64_t>(prefix.size()) + static_cast<std::uint64_t>(data.size())) * 8;
    state.finalize(bitLength, digest);
#endif
}

/// Computes the SHA-256 digest of one byte range.
///
/// - Parameters:
///   - data: The bytes to hash.
///   - digest: Receives the complete 32-byte digest.
inline void sha256(std::span<const unsigned char> data, std::span<unsigned char, 32> digest)
{
    sha256({}, data, digest);
}

/// Returns the SHA-256 digest of a string as lowercase hexadecimal.
///
/// - Parameter input: The bytes to hash.
/// - Returns: 64 lowercase hex characters.
inline std::string sha256Hex(const std::string& input)
{
    std::array<unsigned char, 32> digest {};
    sha256({ reinterpret_cast<const unsigned char*>(input.data()), input.size() }, digest);
    static const char* hex = "0123456789abcdef";
    std::string outputBuffer;
    outputBuffer.reserve(64);
    for (const unsigned char byte : digest) {
        outputBuffer.push_back(hex[(byte >> 4) & 0xF]);
        outputBuffer.push_back(hex[byte & 0xF]);
    }
    return outputBuffer;
}

} // namespace Ubiquitty::Crypto
