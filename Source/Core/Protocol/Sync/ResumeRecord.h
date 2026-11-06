// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sync's persisted resume record (Docs/sync.md §6.9 "Keep the picture
// across a relaunch"): what a client window keeps so a relaunched process
// draws the picture it held at once and continues its attachment.
//
// The record holds the resume identity, the transport's certificate pin, how
// far each pane's input was numbered, each pane's picture, and the input that
// was typed and not reported written. The picture is a checkpoint body of the
// pane's passive terminal, kept with a digest of its bytes, and what the
// client held of the screen plane in the host's numbering, which is what a
// resume names and what the host's next update builds on.
//
// The record holds screen text. A store keeps it under the platform's
// strongest protection; `ClientAttachment` decides when it is written and
// purged, so every frontend follows the same rules:
//
//   - it is rewritten after every `Welcome`, before any input is sent on the
//     new connection, because the `Welcome` consumed the secret it held;
//   - a `HistoryReset` purges it before anything else is applied;
//   - a picture is drawn from it only if its body is the one written;
//   - its unsent input is shown as drafts and never sent;
//   - a record whose pictures pass the checkpoint cap is not written.

#pragma once

#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ScreenPlane.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// What a resume record holds of one pane.
struct ResumeRecordPane {
    ID128 paneID {};
    /// The newest input the client numbered for the pane.
    std::uint64_t lastInputSeq = 0;
    /// The view the client set, or none when the connection's own applied.
    std::optional<ViewState> view {};
    /// A checkpoint body of the pane's passive terminal; empty when the
    /// client held no picture, or withheld it after a `HistoryReset`.
    std::vector<std::uint8_t> checkpoint {};
    /// `digestOf` the checkpoint body as it was written. A checkpoint's own
    /// digest covers its rows and state blocks and not the pens, links and
    /// clusters they name, which a transport delivers whole and a store may
    /// not: a body that does not match is not drawn.
    Digest128 checkpointDigest {};
    /// What the client held of the screen plane with that picture, in the
    /// host's numbering. It means nothing without a checkpoint.
    ScreenReplica replica {};
    /// Stable-pane delivery state, independent of picture ownership. Partial
    /// consumed receipts suppress replay without acknowledging an unfinished batch.
    EffectReceiptState effects {};
    /// Opt-in encrypted history of this exact host/pane/epoch. It shares the
    /// record's sealing key and retire-before-apply transaction.
    std::vector<std::uint8_t> history {};
};

/// Input typed and not reported written when a record was made. A relaunched
/// client shows it and never sends it: the foreground program may have
/// changed since it was typed.
struct ResumeDraft {
    ID128 paneID;
    InputIntent intent;
};

/// What a client window persists to continue across a relaunch.
struct ResumeRecord {
    ID128 hostInstanceID;
    ID128 clientInstance;
    ID128 attachmentID;
    ID128 resumeSecret;
    /// The pin of the certificate the transport reached the host under; zero
    /// on a transport that pins none.
    std::array<std::uint8_t, 32> certificatePin {};
    std::vector<ResumeRecordPane> panes;
    std::vector<ResumeDraft> drafts;
};

/// The bytes of a record's pictures, which the checkpoint cap bounds.
///
/// - Parameter record: The record.
/// - Returns: The sum of its panes' checkpoint bodies.
[[nodiscard]] std::size_t pictureBytes(const ResumeRecord& record);

/// Whether a replacement reached the store's commit boundary. A persistent
/// store commits only after its protected, durable write succeeds.
enum class ResumeWriteOutcome : std::uint8_t {
    Committed,
    /// No usable replacement committed. Required retirement or blocked intent
    /// may make the previous record unavailable; callers must not assume it
    /// remains readable or use this result to admit new input/effects.
    Unchanged,
    /// The replacement happened, but its protected durable commit could not be confirmed.
    /// Neither the previous record nor the new one is a confirmed commit.
    DurabilityUnknown,
};

/// Whether the store confirms that a retired record can no longer be read.
enum class ResumePurgeOutcome : std::uint8_t {
    /// Persistent stores must also confirm retirement of the sealing key,
    /// so a copied ciphertext cannot recover the retired contents.
    Purged,
    /// Retirement is not confirmed and must be retried before another write.
    Refused,
};

/// Whether the owner explicitly permits remote history in protected storage.
enum class HistoryPersistence : std::uint8_t {
    Disabled,
    Enabled
};

/// Whether an authenticated connection may keep a protected resume record.
enum class ResumeBindingOutcome : std::uint8_t {
    Bound,
    /// The carrier is authenticated without the identity this store persists.
    MemoryOnly,
    Refused,
};

/// Where a client window's resume record is kept. One thread uses a store.
/// Calls complete synchronously and must not mutate or destroy the attached
/// client, directly or through callbacks, or reenter this store. The caller
/// relies on that contract for record and input ordering; the type does not
/// enforce it.
class ResumeRecordStore {
public:
    virtual ~ResumeRecordStore() = default;

    /// Reports the owner's opt-in; screen pictures do not imply history consent.
    ///
    /// - Returns: Disabled unless the storage owner explicitly enables history.
    [[nodiscard]] virtual HistoryPersistence historyPersistence() const { return HistoryPersistence::Disabled; }

    /// Binds the store to the current authenticated peer before its Welcome
    /// changes any replica, receipt, secret, or input eligibility. A protected
    /// store may commit its trusted descriptor here; failure admits nothing.
    /// This has the same synchronous, non-reentrant contract as write.
    ///
    /// - Parameters:
    ///   - welcome: The validated Welcome from the authenticated carrier.
    ///   - clientInstance: The stable client identity used by this connection.
    ///   - certificatePin: The pin verified by this carrier, not peer payload.
    /// - Returns: Bound for authorized storage, MemoryOnly for a live connection
    ///   whose old record must first be retired, or Refused for an identity or
    ///   authorization failure. MemoryOnly never authorizes a persistent write.
    [[nodiscard]] virtual ResumeBindingOutcome bindWelcome(const Welcome& welcome, ID128 clientInstance, const std::array<std::uint8_t, 32>& certificatePin) = 0;

    /// Replaces the stored record and reports its commit state.
    ///
    /// - Parameter record: The record.
    /// - Returns: Whether the record committed, no usable replacement committed,
    ///   or replacement happened without confirmed protected durability.
    [[nodiscard]] virtual ResumeWriteOutcome write(const ResumeRecord& record) = 0;

    /// Reads the stored record.
    ///
    /// - Returns: The record, or `nullopt` when none is stored.
    [[nodiscard]] virtual std::optional<ResumeRecord> read() = 0;

    /// Retires the stored record, including its key in a persistent store.
    ///
    /// - Returns: Whether retirement is confirmed, or must be retried.
    [[nodiscard]] virtual ResumePurgeOutcome purge() = 0;
};

/// A resume record kept in memory: it outlives a client attachment, not the
/// process.
class MemoryResumeRecordStore final : public ResumeRecordStore {
public:
    /// Accepts an identity for this explicitly process-local store.
    ///
    /// - Returns: Bound; no persistent authority is created.
    [[nodiscard]] ResumeBindingOutcome bindWelcome(const Welcome&, ID128, const std::array<std::uint8_t, 32>&) override { return ResumeBindingOutcome::Bound; }

    /// Stores a copy of the record.
    ///
    /// - Parameter record: The record.
    /// - Returns: `Committed` to this explicitly in-memory store.
    [[nodiscard]] ResumeWriteOutcome write(const ResumeRecord& record) override;

    /// Reads the stored record.
    ///
    /// - Returns: A copy of the record, or `nullopt` when none is stored.
    [[nodiscard]] std::optional<ResumeRecord> read() override;

    /// Removes the stored record.
    ///
    /// - Returns: `Purged`.
    [[nodiscard]] ResumePurgeOutcome purge() override;

private:
    std::optional<ResumeRecord> m_record;
};

} // namespace Ubiquitty::Protocol::Sync
