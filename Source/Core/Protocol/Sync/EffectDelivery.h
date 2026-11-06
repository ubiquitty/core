// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/EffectCodec.h"

#include <optional>

namespace Ubiquitty::Protocol::Sync {

/// Fixed framing for one record of a bounded effect batch.
inline constexpr std::size_t effectPacketHeaderBytes = 2 * sizeof(std::uint64_t) + sizeof(std::uint16_t);

/// Borrows one record and the completion boundary of its immutable batch.
///
/// remainingRecords counts records after this packet. An empty record is valid
/// only for an empty completed batch, including a span of skipped sounds.
struct EffectPacket {
    std::uint64_t generation = 0;
    std::uint64_t throughSequence = 0;
    std::uint16_t remainingRecords = 0;
    std::optional<EffectView> record;
};

/// Encodes one whole packet after checking its record and final frame bound.
///
/// - Parameters:
///   - paneID: The nonzero authenticated stable pane binding.
///   - generation: The immutable screen dependency of the whole selected batch.
///   - throughSequence: The batch's final logical cursor, including skipped records.
///   - remainingRecords: Records still to follow, below maxEffectRecords.
///   - encodedRecord: One complete encoded Effect, or empty for an empty batch.
/// - Returns: One exact nothrow owner, or a refusal with no partial output.
[[nodiscard]] std::expected<EffectCodecBytes, EffectCodecError> encodeEffectPacket(ID128 paneID,
    std::uint64_t generation, std::uint64_t throughSequence, std::uint16_t remainingRecords, std::span<const std::uint8_t> encodedRecord);

/// Decodes a complete packet without allocating or retaining its input.
///
/// - Parameters:
///   - paneID: The authenticated stable pane binding, never a channel number.
///   - bytes: The whole packet; every returned view borrows these bytes.
/// - Returns: One validated packet, or a refusal.
[[nodiscard]] std::expected<EffectPacket, EffectCodecError> decodeEffectPacket(ID128 paneID, std::span<const std::uint8_t> bytes);

/// Whether a packet contains a record the application has not consumed.
enum class EffectReception : std::uint8_t {
    /// Its record is new and must be offered once to the application.
    Record,
    /// The record was already consumed, or this is an empty batch completion.
    CursorOnly,
};

/// The two independent cursors retained across a connection interruption.
struct EffectReceiptState {
    /// The final cursor of the last complete batch.
    std::uint64_t acknowledged = 0;
    /// Records at or below this cursor need no application action. It includes
    /// consumed records in a partial batch and skipped records in a complete one.
    std::uint64_t consumed = 0;
};

/// Commits a stable pane's receipt before presentation or acknowledgement.
/// Calls are synchronous and must not mutate, destroy, or reenter the client.
/// A durable consumed cursor prevents replay after a crash; an effect whose
/// external sink has not run when the process dies may remain unposted.
class EffectReceiptSink {
public:
    virtual ~EffectReceiptSink() = default;

    /// Commits both high-water marks under the current authenticated attachment.
    ///
    /// - Parameters:
    ///   - paneID: The exact nonzero stable pane binding.
    ///   - state: The candidate receipt, with consumed at least acknowledged.
    /// - Returns: Whether its persistence boundary completed successfully.
    [[nodiscard]] virtual bool commitEffectReceipt(ID128 paneID, EffectReceiptState state) = 0;
};

/// The receiving pane's actual view and applied picture, not values from a packet.
struct EffectScreenBoundary {
    ViewState view = ViewState::Live;
    std::uint64_t appliedGeneration = 0;
};

/// Validates ordered batches and separates once-only delivery from final ACK.
///
/// One instance belongs to one bound handle of a stable pane. Alias handles
/// have independent batches; their owner merges consumed receipts for once-only
/// presentation. State advances before an application callback so a callback
/// that reconnects can retain the consumed identity. The
/// caller must resolve payload readiness and authority before acceptance, and
/// retain any deferred action before it exposes the updated receipt state.
class EffectReceiveCursor {
public:
    /// Creates an empty cursor for a stable pane.
    ///
    /// - Parameter paneID: The nonzero authenticated pane identity.
    explicit EffectReceiveCursor(ID128 paneID)
        : m_paneID(paneID)
    {
    }

    /// Restores authenticated local receipt state before any packet arrives.
    ///
    /// - Parameter state: Complete-batch and consumed-record high-water marks.
    /// - Returns: Success, or a refusal leaving the cursor unchanged.
    [[nodiscard]] std::expected<void, EffectCodecError> restore(EffectReceiptState state);

    /// Validates and consumes one decoded packet without allocating.
    ///
    /// - Parameters:
    ///   - packet: One complete packet decoded under this pane's binding.
    ///   - screen: The actual view and applied generation; a visible pane must
    ///     hold the packet's dependency before this call.
    /// - Returns: Whether its record is newly consumed, or a refusal with no mutation.
    [[nodiscard]] std::expected<EffectReception, EffectCodecError> accept(const EffectPacket& packet, EffectScreenBoundary screen);

    /// Captures the receipt state independently of a transport's partial batch.
    ///
    /// - Returns: Both high-water marks for this stable pane.
    [[nodiscard]] EffectReceiptState state() const { return m_state; }

    /// Reports whether another packet must complete the current batch.
    ///
    /// - Returns: True until the declared record countdown reaches zero.
    [[nodiscard]] bool hasPendingBatch() const { return m_pendingThrough != 0; }

private:
    const ID128 m_paneID;
    EffectReceiptState m_state;
    std::uint64_t m_pendingGeneration = 0;
    std::uint64_t m_pendingThrough = 0;
    std::uint64_t m_lastBatchSequence = 0;
    std::uint16_t m_expectedRemaining = 0;
    bool m_hasReceived = false;
};

} // namespace Ubiquitty::Protocol::Sync
