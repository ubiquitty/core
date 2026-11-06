// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/ResumeRecord.h"

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Session {

/// Why a persisted resume record cannot be used.
enum class ResumeRecordError {
    Malformed,
    VersionMismatch,
    TooLarge,
    AllocationFailed,
};

/// One exactly sized plaintext owner. It must be sealed before disk storage.
struct EncodedResumeRecord {
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size = 0;

    /// Borrows the complete plaintext without copying it.
    ///
    /// - Returns: Bytes valid while this owner remains alive and unchanged.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { bytes.get(), size }; }
};

/// A framed, digest-checked pane's borrowed storage. Replica bytes preserve
/// the host's pen dictionary and hashes, not those reconstructed from the
/// passive picture. Restoring a picture still requires applyCheckpoint's
/// transactional semantic validation; this view alone authorizes no mutation.
struct ResumePaneView {
    Protocol::Sync::ID128 paneID;
    std::uint64_t lastInputSeq = 0;
    std::optional<Protocol::Sync::ViewState> view;
    std::span<const std::uint8_t> checkpoint;
    Protocol::Sync::Digest128 checkpointDigest;
    std::span<const std::uint8_t> replica;
    Protocol::Sync::EffectReceiptState effects {};
    std::span<const std::uint8_t> history;
};

/// A record validated without allocating or mutating a terminal. All spans
/// borrow the input, which must remain immutable and outlive this view.
/// Draft bytes are display-only; they are never a retransmission queue.
struct ResumeRecordView {
    Protocol::Sync::ID128 hostInstanceID;
    Protocol::Sync::ID128 clientInstance;
    Protocol::Sync::ID128 attachmentID;
    Protocol::Sync::ID128 resumeSecret;
    std::array<std::uint8_t, 32> certificatePin {};
    std::array<ResumePaneView, Protocol::Sync::maxPanesPerConnection> panes {};
    std::size_t paneCount = 0;
    std::span<const std::uint8_t> drafts;
    std::size_t draftCount = 0;
    /// Requested storage for materialized objects and payloads, excluding
    /// allocator bookkeeping and the separately admitted terminal pictures.
    std::size_t materializedBytes = 0;
};

/// Encodes the current record format and wire version. Measures and validates
/// the whole record before one exact, fallible allocation.
///
/// - Parameters:
///   - record: The identity, pictures, original replicas and unsent drafts.
///   - maxBytes: The caller's total plaintext storage budget, including metadata.
/// - Returns: Owned plaintext, or a refusal with no partial output.
[[nodiscard]] std::expected<EncodedResumeRecord, ResumeRecordError> encodeResumeRecord(const Protocol::Sync::ResumeRecord& record, std::size_t maxBytes);

/// Validates the complete record, including picture byte digests, before any
/// materialization. This is not authentication: only an authenticated sealed
/// store may supply records to a live attachment.
///
/// - Parameters:
///   - bytes: The complete immutable plaintext record.
///   - maxBytes: The caller's total plaintext storage budget.
/// - Returns: A borrowed view, or a refusal. Other wire versions are not read.
[[nodiscard]] std::expected<ResumeRecordView, ResumeRecordError> decodeResumeRecord(std::span<const std::uint8_t> bytes, std::size_t maxBytes);

/// Copies a validated record into independent owners, including the host's
/// original dictionary and display-only drafts. It creates no terminal and
/// sends no input. Replica dictionaries and hashes allocate fallibly into their
/// final immutable owners. Other standard container allocations retain their
/// existing fatal failure behavior; refusals publish no partial record.
///
/// - Parameters:
///   - bytes: Immutable authenticated plaintext, borrowed for this call.
///   - maxBytes: The complete plaintext budget.
///   - maxMaterializedBytes: The sum of record, pane, dictionary, hash, draft,
///     event and payload storage, checked before copying any of them.
/// - Returns: Independent state, or a validation/storage-budget refusal.
[[nodiscard]] std::expected<Protocol::Sync::ResumeRecord, ResumeRecordError> materializeResumeRecord(std::span<const std::uint8_t> bytes, std::size_t maxBytes, std::size_t maxMaterializedBytes);

/// Where a frontend without a platform keychain keeps a window's encoded
/// record: browser storage, or a private file under an editor's data
/// directory. The frontend owns only these bytes; the record's shape, its
/// validation and when it is written or purged stay with Core.
class ResumeRecordSlot {
public:
    virtual ~ResumeRecordSlot() = default;

    /// Replaces the stored bytes, returning only after the replacement holds.
    ///
    /// - Parameter bytes: The complete encoded record.
    /// - Returns: Whether the bytes were stored whole.
    [[nodiscard]] virtual bool write(std::span<const std::uint8_t> bytes) = 0;

    /// Reads the stored bytes, at most `maxBytes` of them.
    ///
    /// - Parameter maxBytes: The most the caller accepts.
    /// - Returns: The bytes, or `nullopt` when none are stored or they are
    ///   longer than `maxBytes`.
    [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> read(std::size_t maxBytes) = 0;

    /// Removes the stored bytes.
    ///
    /// - Returns: Whether nothing is stored afterwards.
    [[nodiscard]] virtual bool erase() = 0;
};

/// A resume record kept as its encoded bytes in a frontend's slot. It is not
/// sealed: the slot's own access control is the only protection, so it never
/// keeps history, and it is for a client whose platform offers no keychain
/// (Docs/sync.md §6.9 says which clients those are).
class EncodedResumeRecordStore final : public Protocol::Sync::ResumeRecordStore {
public:
    /// - Parameters:
    ///   - slot: Where the bytes live; it outlives the store.
    ///   - maxBytes: The encoded record budget, which also bounds a read.
    EncodedResumeRecordStore(ResumeRecordSlot& slot, std::size_t maxBytes);

    /// Accepts the authenticated peer: the slot belongs to one window, and
    /// the attachment checks that the record it rewrites is that window's.
    ///
    /// - Returns: Bound.
    [[nodiscard]] Protocol::Sync::ResumeBindingOutcome bindWelcome(const Protocol::Sync::Welcome&, Protocol::Sync::ID128, const std::array<std::uint8_t, 32>&) override { return Protocol::Sync::ResumeBindingOutcome::Bound; }

    /// Encodes the record and stores it whole.
    ///
    /// - Parameter record: The record.
    /// - Returns: `Committed` once the slot holds it; `Unchanged` when it does
    ///   not encode within the budget or the slot refused it.
    [[nodiscard]] Protocol::Sync::ResumeWriteOutcome write(const Protocol::Sync::ResumeRecord& record) override;

    /// Reads, validates and materializes the stored record.
    ///
    /// - Returns: The record, or `nullopt` when none is stored or it is refused.
    [[nodiscard]] std::optional<Protocol::Sync::ResumeRecord> read() override;

    /// Erases the slot.
    ///
    /// - Returns: `Purged` once the slot holds nothing.
    [[nodiscard]] Protocol::Sync::ResumePurgeOutcome purge() override;

private:
    ResumeRecordSlot& m_slot;
    std::size_t m_maxBytes;
};

// A client that keeps no Core in its process — an editor, which speaks the
// wire itself and holds its pane's picture as the host's own wire bytes —
// stores and reads its record through the host binary
// (`ubiquitty-host resume-record`), which runs these two functions. The
// exchange grammars, every integer little-endian and every `bytes` a u32
// length and its bytes:
//
//     PictureRequest := hostInstance:u128 clientInstance:u128 attachmentID:u128
//                       resumeSecret:u128 paneID:u128 lastInputSeq:u64
//                       view:u8 (0xff for none) effectsAcknowledged:u64
//                       effectsConsumed:u64 checkpoint:bytes
//     ResumeReply    := clientInstance:u128 greetTail:bytes paneCount:u16
//                       (paneID:u128 lastInputSeq:u64 effectsAcknowledged:u64
//                        effectsConsumed:u64 checkpoint:bytes)×paneCount
//
// `checkpoint` is a checkpoint body in the host's numbering, empty when the
// client holds no picture. `greetTail` is what the client's Greet ends with
// (`encodeGreetResume`). A reply's pane carries a checkpoint only when the
// stored picture validated, and the client draws only that one.

/// Builds the record a `PictureRequest` describes. The picture is applied to
/// a scratch terminal, which is what derives the host's numbering the resume
/// names from the body, so a body that does not apply is refused.
///
/// - Parameters:
///   - bytes: The complete request.
///   - maxBytes: The record budget, which bounds the checkpoint.
/// - Returns: The record, or a refusal.
[[nodiscard]] std::expected<Protocol::Sync::ResumeRecord, ResumeRecordError> recordFromPictureRequest(std::span<const std::uint8_t> bytes, std::size_t maxBytes);

/// Reads the stored record into a `ResumeReply`, naming each pane as the
/// client attachment's own resume would.
///
/// - Parameter store: The window's store.
/// - Returns: The reply, or `nullopt` when no usable record is stored.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> resumeReplyFromStore(Protocol::Sync::ResumeRecordStore& store);

} // namespace Ubiquitty::Session
