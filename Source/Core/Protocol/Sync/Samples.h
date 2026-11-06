// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/VT/Sound.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

struct BulkChunk;

/// Binds a downloaded sample to its exact signed-16 little-endian PCM, not
/// merely a mutable bank slot. Zero frames and a zero digest mean silence.
struct SampleIdentity {
    std::uint32_t serial = 0;
    std::uint32_t frames = 0;
    std::array<std::uint8_t, 32> sha256 {};

    friend bool operator==(const SampleIdentity&, const SampleIdentity&) = default;
};

inline constexpr std::size_t sampleIdentityBytes = 4 + 4 + 32;
inline constexpr std::size_t sampleHeadBytes = 1 + sampleIdentityBytes + 4;

/// Announces one immutable sample on its authenticated pane channel. A
/// nonempty sample requires a nonzero transfer; empty state has no transfer.
struct SampleHead {
    std::uint8_t slot = 0;
    SampleIdentity sample;
    std::uint32_t transfer = 0;
};

/// Checks sample bounds and the canonical empty identity.
///
/// - Parameter identity: The wire identity, before allocating its PCM.
/// - Returns: Whether it names at most the existing ten-second sample bound.
[[nodiscard]] bool isValidSampleIdentity(const SampleIdentity& identity);

/// Validates and appends one complete sample announcement.
///
/// - Parameters:
///   - output: The destination; refusal leaves it untouched.
///   - head: One bounded slot, exact identity and transfer assignment.
/// - Returns: Whether the head was encoded.
[[nodiscard]] bool encodeSampleHead(std::vector<std::uint8_t>& output, const SampleHead& head);

/// Reads a whole fixed-size announcement without allocating.
///
/// - Parameter bytes: Exactly one SampleHead body.
/// - Returns: The bounded head, or no value for malformed/trailing bytes.
[[nodiscard]] std::optional<SampleHead> decodeSampleHead(std::span<const std::uint8_t> bytes);

/// Owns canonical PCM copied at effect creation. The log copies these bytes
/// under its retained-data budget before this temporary owner is released.
struct SampleBytes {
    SampleIdentity sample;
    std::unique_ptr<std::uint8_t[]> bytes;

    /// Captures exact samples without truncating an oversized slot.
    ///
    /// - Parameters:
    ///   - serial: The original bank serial, including a reset to zero.
    ///   - pcm: The selected slot's mono signed-16 frames at 44,100Hz.
    ///   - availableBytes: Capacity admitted before allocating the snapshot.
    /// - Returns: An immutable snapshot, including an empty clear, or no owner.
    [[nodiscard]] static std::optional<SampleBytes> capture(std::uint32_t serial, std::span<const std::int16_t> pcm, std::size_t availableBytes);

    /// Borrows the complete canonical little-endian payload.
    ///
    /// - Returns: Exactly two bytes per frame, valid while this owner lives.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { bytes.get(), bytes ? static_cast<std::size_t>(sample.frames) * 2 : 0 }; }
};

/// A verified native-endian sample owner. A sink may borrow it only while an
/// owning lease is retained across its callback; no live bank lookup is needed.
struct SamplePCM {
    SampleIdentity sample;
    std::unique_ptr<std::int16_t[]> frames;

    /// Borrows the verified frames for synchronous playback.
    ///
    /// - Returns: Exactly the announced frame count, or empty after a move.
    [[nodiscard]] std::span<const std::int16_t> view() const { return { frames.get(), frames ? sample.frames : 0 }; }
};

/// Whether an exact sample transfer needs more bytes, completed, or refused.
enum class SampleChunkResult : std::uint8_t {
    Pending,
    Complete,
    Refused,
};

/// Reassembles one raw sample in one exact aligned allocation. A chunk may
/// split a 16-bit frame; conversion to native endian happens after both SHA
/// identities match. Refusal releases storage and never publishes partial PCM.
class SampleTransfer {
public:
    SampleTransfer(const SampleTransfer&) = delete;
    SampleTransfer& operator=(const SampleTransfer&) = delete;
    SampleTransfer(SampleTransfer&&) noexcept = default;
    SampleTransfer& operator=(SampleTransfer&&) noexcept = default;

    /// Admits the complete allocation before accepting any chunk.
    ///
    /// - Parameters:
    ///   - head: A nonempty bounded sample and its unique transfer ID.
    ///   - availableBytes: The receiver's remaining retained-byte allowance.
    /// - Returns: The exact owner, or no allocation on invalid/over-budget input.
    [[nodiscard]] static std::optional<SampleTransfer> begin(const SampleHead& head, std::size_t availableBytes);

    /// Accepts one ordered raw chunk and verifies the exact final bytes.
    ///
    /// - Parameter chunk: Nonempty bytes of the announced transfer.
    /// - Returns: Pending, complete, or refusal with the owner released.
    [[nodiscard]] SampleChunkResult accept(const BulkChunk& chunk);

    /// Moves verified PCM out exactly once.
    ///
    /// - Returns: The complete owner, or no value before completion/after take.
    [[nodiscard]] std::optional<SamplePCM> take();

    /// Counts full allocated capacity, not just the received prefix.
    ///
    /// - Returns: Retained bytes until refusal or ownership transfer.
    [[nodiscard]] std::size_t heldBytes() const { return m_pcm.frames ? static_cast<std::size_t>(m_pcm.sample.frames) * 2 : 0; }

private:
    SampleTransfer(std::uint32_t transfer, SamplePCM pcm)
        : m_transfer(transfer)
        , m_pcm(std::move(pcm))
    {
    }

    std::uint32_t m_transfer;
    SamplePCM m_pcm;
    std::size_t m_received = 0;
    std::uint32_t m_nextIndex = 0;
    bool m_isComplete = false;
};

} // namespace Ubiquitty::Protocol::Sync
