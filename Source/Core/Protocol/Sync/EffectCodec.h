// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Effects.h"
#include "Core/Protocol/Sync/Samples.h"
#include "PAL/PAL.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

namespace Ubiquitty::Protocol::Sync {

/// Bounds the logical clipboard bundle independently of its frame or transfer.
inline constexpr std::size_t maxEffectClipboardItems = 32;
inline constexpr std::size_t maxEffectClipboardMIMEBytes = 65'535;
inline constexpr std::size_t maxEffectClipboardWriteBytes = 1024 * 1024;
inline constexpr std::size_t maxEffectClipboardReadBytes = 4u * 1024u * 1024u;
/// The complete canonical read bundle, including bounded MIME metadata.
inline constexpr std::size_t maxEffectClipboardEncodedReadBytes = 1 + maxEffectClipboardItems * (3 + maxEffectClipboardMIMEBytes + 4) + maxEffectClipboardReadBytes;

/// The fixed fields that precede an effect record's body: generation,
/// sequence, primary client, kind and coalesced count.
inline constexpr std::size_t effectRecordHeaderBytes = 8 + 8 + 16 + 1 + 4;

/// Why a complete effect or clipboard record could not be encoded or decoded.
enum class EffectCodecError : std::uint8_t {
    Malformed,
    TooLarge,
    NoMemory,
};

/// Owns one complete encoding, allocated only after validation and measurement.
struct EffectCodecBytes {
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size = 0;

    /// Borrows the complete encoded record.
    ///
    /// - Returns: Bytes valid while this owner remains alive and unchanged.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { bytes.get(), size }; }
};

/// Borrows one MIME name and its binary payload from a complete clipboard bundle.
struct EffectClipboardItem {
    std::string_view mime;
    std::span<const std::uint8_t> data;
};

/// Holds fixed metadata for a complete clipboard, including an empty clear.
struct EffectClipboardBundle {
    std::array<EffectClipboardItem, maxEffectClipboardItems> items;
    std::size_t count = 0;
};

/// Names either a complete inline bundle or an admitted bulk transfer.
///
/// Transfer zero includes exactly encodedBytes inline bytes. A nonzero transfer
/// has no inline bytes; its owner must bind admission to the original operation
/// and validate the completed bundle before any clipboard sink sees it.
struct EffectClipboardPayload {
    std::uint32_t transfer = 0;
    std::uint32_t encodedBytes = 0;
    std::span<const std::uint8_t> inlineBytes;
};

/// Represents a bell without a kind-specific payload.
struct BellEffect { };
/// Represents an attention request without a kind-specific payload.
struct AttentionEffect { };

/// Borrows the complete program notification, including replacement and timing.
struct NotificationEffect {
    std::string_view title {};
    std::string_view body {};
    std::string_view identifier {};
    NotificationOccasion occasion = NotificationOccasion::Always;
};

/// Records one completed command, its elapsed time, and borrowed shell context.
struct CommandFinishedEffect {
    std::int32_t exitStatus = 0;
    std::uint64_t durationMicroseconds = 0;
    std::string_view currentDirectory {};
    std::string_view commandLine {};
};

/// Carries a DECPS request whose sample slot is resolved by the receiving pane.
/// No PCM pointer or sample frames belong in the encoded request.
struct SoundEffect {
    PAL::PlayTonesRequest request;
    /// Required exact sample identity for wave 6; zero for other voices.
    SampleIdentity sample;
};

/// Carries an already-authorized write to one exact clipboard buffer.
struct ClipboardWriteEffect {
    ClipboardBuffer buffer = ClipboardBuffer::Clipboard;
    EffectClipboardPayload payload;
};

/// The typed body of one effect; borrowed fields never outlive their source.
using EffectBody = std::variant<BellEffect, NotificationEffect, CommandFinishedEffect,
    SoundEffect, AttentionEffect, ClipboardWriteEffect>;

/// Distinguishes a MIME inventory from the bytes of one named MIME type.
enum class EffectClipboardRequestKind : std::uint8_t {
    Types,
    Item,
};

/// Preserves why a clipboard response contains no data.
enum class EffectClipboardStatus : std::uint8_t {
    OK,
    Empty,
    Refused,
    TooLarge,
};

/// Asks one authenticated recipient for an exact clipboard operation.
struct ClipboardRead {
    std::uint32_t requestID = 0;
    ClipboardBuffer buffer = ClipboardBuffer::Clipboard;
    EffectClipboardRequestKind kind = EffectClipboardRequestKind::Types;
    std::string_view mime {};
};

/// Answers a pending request; non-OK statuses carry no payload.
struct ClipboardData {
    std::uint32_t requestID = 0;
    EffectClipboardStatus status = EffectClipboardStatus::Empty;
    EffectClipboardPayload payload;
};

/// Identifies the kind represented by a typed effect body.
///
/// - Parameter body: The typed body.
/// - Returns: Its matching EffectKind.
[[nodiscard]] EffectKind effectKind(const EffectBody& body);

/// Validates and encodes one kind body for storage in an EffectLog.
///
/// - Parameter body: The typed body; all borrowed data remains stable during the call.
/// - Returns: One complete bounded owner, or a refusal without partial output.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeEffectBody(const EffectBody& body);

/// Decodes one whole kind body without allocating or modifying its input.
///
/// - Parameters:
///   - kind: The admitted effect kind.
///   - bytes: The complete body, which must outlive every returned view.
/// - Returns: A validated borrowed body, or a refusal.
[[nodiscard]] std::expected<EffectBody, EffectCodecError> decodeEffectBody(EffectKind kind, std::span<const std::uint8_t> bytes);

/// Encodes one pane-addressed effect, checking its canonical body and target.
///
/// - Parameter effect: The log view, including nonzero pane identity and sequence.
/// - Returns: One frame body, or a refusal before allocating partial output.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeEffect(const EffectView& effect);

/// Decodes one effect under its authenticated stable pane binding.
///
/// - Parameters:
///   - paneID: The nonzero stable identity bound to this pane channel.
///   - bytes: The whole frame body; returned payload views borrow it.
/// - Returns: A validated view; no authority or posting permission is granted.
[[nodiscard]] std::expected<EffectView, EffectCodecError> decodeEffect(ID128 paneID, std::span<const std::uint8_t> bytes);

/// Encodes a complete clipboard bundle within its operation's data budget.
///
/// - Parameters:
///   - bundle: At most 32 distinct nonempty MIME names and their binary data.
///   - maxDataBytes: The admitted aggregate data budget, no greater than the read cap.
/// - Returns: One complete inline or bulk payload, or a refusal.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeEffectClipboard(const EffectClipboardBundle& bundle, std::size_t maxDataBytes);

/// Measures the validated complete bundle without allocating.
///
/// - Parameters:
///   - bundle: Borrowed MIME names and binary data from one operation.
///   - maxDataBytes: The operation's aggregate data bound.
/// - Returns: Exact encoded bytes for pre-allocation ownership admission.
[[nodiscard]] std::expected<std::size_t, EffectCodecError> effectClipboardEncodedBytes(const EffectClipboardBundle& bundle, std::size_t maxDataBytes);

/// Writes the canonical bundle into an already admitted, exactly sized owner.
/// - Parameters:
///   - bundle: Borrowed MIME names and bytes, unchanged throughout this call.
///   - maxDataBytes: The same aggregate bound used for measurement.
///   - output: Exactly effectClipboardEncodedBytes bytes of admitted storage.
/// - Returns: A refusal before writing for invalid input or a mismatched owner.
[[nodiscard]] std::expected<void, EffectCodecError> writeEffectClipboard(const EffectClipboardBundle& bundle, std::size_t maxDataBytes, std::span<std::uint8_t> output);

/// Validates a complete inline or reassembled clipboard bundle without allocation.
///
/// - Parameters:
///   - bytes: The complete bundle; all returned views borrow it.
///   - maxDataBytes: The original operation's budget, no greater than the read cap.
/// - Returns: Fixed metadata with borrowed values, or a refusal.
[[nodiscard]] std::expected<EffectClipboardBundle, EffectCodecError> decodeEffectClipboard(std::span<const std::uint8_t> bytes, std::size_t maxDataBytes);

/// Encodes an exact clipboard request within one frame.
///
/// - Parameter request: Nonzero request identity and its typed operation.
/// - Returns: A complete frame body, or a refusal.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeClipboardRead(const ClipboardRead& request);
/// Decodes an exact clipboard request without allocating.
///
/// - Parameter bytes: The complete frame body, borrowed by the result.
/// - Returns: A validated request, or a refusal.
[[nodiscard]] std::expected<ClipboardRead, EffectCodecError> decodeClipboardRead(std::span<const std::uint8_t> bytes);
/// Encodes one clipboard answer, never silently dropping MIME data.
///
/// - Parameter answer: The pending request's answer and complete payload descriptor.
/// - Returns: A complete frame body, or a refusal.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeClipboardData(const ClipboardData& answer);
/// Decodes one clipboard answer without establishing request or sender authority.
///
/// - Parameter bytes: The complete frame body, borrowed by the result.
/// - Returns: A validated answer, or a refusal.
[[nodiscard]] std::expected<ClipboardData, EffectCodecError> decodeClipboardData(std::span<const std::uint8_t> bytes);

} // namespace Ubiquitty::Protocol::Sync
