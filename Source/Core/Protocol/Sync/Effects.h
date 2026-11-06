// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A pane owns its effect identities and payloads independently of any carrier.
// Retention cursors name effects, not screen generations: several effects can
// follow one generation. A refused append changes neither identities nor data.
// This store grants no authority to produce or deliver a clipboard write.

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Roles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace Ubiquitty::Protocol::Sync {

/// The program-observable event carried beside a pane's state.
enum class EffectKind : std::uint8_t {
    Bell,
    Notification,
    CommandFinished,
    Sound,
    Attention,
    ClipboardWrite,
};

/// The fixed storage ceilings, independent of payload-controlled lengths.
inline constexpr std::size_t maxEffectRecords = 256;
inline constexpr std::size_t maxEffectReaders = 64;

/// Bounds chosen by the owner before it accepts program output.
struct EffectPolicy {
    std::size_t maxRecords = maxEffectRecords;
    std::size_t maxPayloadBytes = 1024 * 1024;
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
    std::size_t notificationsKept = 8;
    std::uint64_t lostGraceMicroseconds = measuredResumePolicy().lostGraceMicroseconds;
};

/// The admitted kind body to copy into the pane's log.
struct EffectDraft {
    std::uint64_t generation = 0;
    ID128 primary {};
    EffectKind kind = EffectKind::Bell;
    std::uint32_t count = 1;
    /// The clipboard selector; zero for other kinds.
    std::uint8_t target = 0;
    /// A canonical kind body. Its codec owns field and text validation.
    std::span<const std::uint8_t> payload {};
    /// Immutable sample or clipboard transfer bytes. Their typed producer
    /// validates format and operation limits before the log copies them.
    std::span<const std::uint8_t> data {};
};

/// A borrowed effect, valid until its log is mutated or destroyed.
struct EffectView {
    ID128 paneID;
    std::uint64_t generation = 0;
    /// Unique within the pane, including across trimming.
    std::uint64_t sequence = 0;
    /// Elected when the effect is created; never changed by a later election.
    ID128 primary {};
    EffectKind kind = EffectKind::Bell;
    std::uint32_t count = 1;
    std::uint8_t target = 0;
    std::span<const std::uint8_t> payload {};
    /// Transfer bytes owned by the same log entry, never a mutable live bank.
    std::span<const std::uint8_t> data {};
};

/// Why an operation left the log unchanged.
enum class EffectRefusal : std::uint8_t {
    Invalid,
    OverLimit,
    NoMemory,
    ExhaustedIdentity,
    UnknownReader,
    InvalidAcknowledgement,
    RetiredCursor,
};

/// Whether a reader missed intermediate presentations.
enum class EffectReadMode : std::uint8_t {
    Consecutive,
    Skipped,
};

/// A bounded borrowed batch, in sequence order.
struct EffectBatch {
    std::array<EffectView, maxEffectRecords> records;
    std::size_t size = 0;
    /// The complete input span, including stale sounds omitted by coalescing.
    std::uint64_t throughSequence = 0;

    /// Borrows the populated prefix of this batch.
    ///
    /// - Returns: Effects in sequence order, borrowing their log's payloads.
    [[nodiscard]] std::span<const EffectView> effects() const { return { records.data(), size }; }
};

/// Whether a frontend posts, badges, or ignores an effect.
enum class EffectDisposition : std::uint8_t {
    Post,
    Badge,
    Ignore,
};

/// A non-resumable carrier's lifetime, independent of any borrowed pane owner.
/// Its last strong reference expires that carrier's effect retention.
struct EffectReaderLifetime { };

/// Resolves presentation without granting a viewer or agent a clipboard sink.
///
/// - Parameters:
///   - effect: The admitted effect, including its immutable elected primary.
///   - clientInstance: The receiving frontend's authenticated identity.
///   - kind: The receiving frontend's authenticated client classification.
///   - role: The receiving attachment's current role.
/// - Returns: Post only for a human primary driver; badge or ignore otherwise.
[[nodiscard]] EffectDisposition effectDisposition(const EffectView& effect, ID128 clientInstance, ClientKind kind, Role role);

/// Owns one pane's bounded effects and the cursors that retain them.
class EffectLog {
public:
    /// Portable retention of one resumable reader; carrier leases never transfer.
    struct ReaderContinuation {
        ID128 attachmentID;
        ID128 clientInstance;
        AttachmentState state = AttachmentState::Detached;
        std::uint64_t acknowledged = 0;
        std::uint64_t sent = 0;
        std::uint64_t lostSinceMicroseconds = 0;
    };

    /// Borrows immutable records while carrying all bounded retention metadata.
    /// Transient readers are excluded because their physical carrier is retired.
    struct Continuation {
        ID128 paneID;
        EffectPolicy policy;
        std::array<EffectView, maxEffectRecords> records;
        std::size_t recordCount = 0;
        std::array<ReaderContinuation, maxEffectReaders> readers;
        std::size_t readerCount = 0;
        std::uint64_t lastSequence = 0;
        std::uint64_t lastGeneration = 0;
        std::uint64_t retiredThrough = 0;
    };

    /// Borrows exact retained bodies without trimming or changing a cursor.
    /// - Returns: Fixed metadata and payload spans valid until this log mutates.
    [[nodiscard]] Continuation continuation() const;

    /// Installs admitted retained identities into a pristine matching log.
    ///
    /// Complete metadata and byte admission precedes candidate allocation.
    /// The authenticated handover owner reconciles imported readers with its
    /// inherited attachment table before allowing delivery or trimming.
    /// - Parameter continuation: Unchanged borrowed bytes and exact matching policy.
    /// - Returns: Refusal without mutation, or success with independent owners.
    [[nodiscard]] std::expected<void, EffectRefusal> restoreContinuation(const Continuation& continuation);

    /// Creates a log with explicit policy and an optional inherited high-water mark.
    ///
    /// - Parameters:
    ///   - paneID: The stable pane identity.
    ///   - policy: Bounds; values outside the fixed ceilings refuse appends.
    ///   - lastSequence: An authenticated predecessor's last issued identity.
    explicit EffectLog(ID128 paneID, EffectPolicy policy = {}, std::uint64_t lastSequence = 0);

    /// Copies an admitted effect and its immutable transfer data together, or
    /// refuses it without evicting unacknowledged data. The kind body obeys
    /// maxPayloadBytes; body and data together obey maxRetainedBytes.
    ///
    /// - Parameter draft: Validated kind bytes and their generation and primary.
    /// - Returns: A new pane-local sequence, or a refusal with no state change.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> append(const EffectDraft& draft);

    /// Takes an exact immutable transfer owner without allocating a second copy.
    /// Its kind body is copied only after the same append admission succeeds.
    ///
    /// - Parameters:
    ///   - draft: Validated metadata and body, with an empty borrowed data span.
    ///   - data: An exclusive array of at least dataBytes; consumed only on success.
    ///   - dataBytes: Exact initialized bytes to retain under the log's budget.
    /// - Returns: A new sequence, or refusal leaving the owner and log unchanged.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendOwnedData(EffectDraft draft,
        std::unique_ptr<std::uint8_t[]>& data, std::size_t dataBytes);

    /// Borrows everything after a cursor, coalescing only when requested.
    ///
    /// The owner sends the whole batch before recording its through cursor.
    ///
    /// - Parameters:
    ///   - sequence: The reader's last retained effect cursor.
    ///   - mode: Whether intermediate presentations were skipped.
    /// - Returns: A complete borrowed batch or a refusal; the log is unchanged.
    [[nodiscard]] std::expected<EffectBatch, EffectRefusal> readAfter(std::uint64_t sequence, EffectReadMode mode) const;

    /// Tracks an authenticated reader from its retained effect cursor.
    ///
    /// Repeating a reader identity is refused rather than resetting its standing.
    ///
    /// - Parameters:
    ///   - attachmentID: A nonzero authenticated attachment identity.
    ///   - clientInstance: Its nonzero authenticated frontend identity.
    ///   - acknowledgedSequence: Its retained cursor within the available log.
    /// - Returns: Success or a refusal without adding or resetting a reader.
    [[nodiscard]] std::expected<void, EffectRefusal> addReader(ID128 attachmentID, ID128 clientInstance, std::uint64_t acknowledgedSequence);
    /// Tracks a non-resumable reader only while its carrier's lifetime exists.
    ///
    /// - Parameters:
    ///   - attachmentID: The nonzero identity of this non-resumable attachment.
    ///   - clientInstance: Its nonzero authenticated frontend identity.
    ///   - acknowledgedSequence: Its initial cursor within the retained log.
    ///   - lifetime: A live carrier-owned lease; the log never owns the carrier.
    /// - Returns: Success, or a refusal that leaves reader slots unchanged.
    [[nodiscard]] std::expected<void, EffectRefusal> addTransientReader(ID128 attachmentID, ID128 clientInstance,
        std::uint64_t acknowledgedSequence, std::weak_ptr<const EffectReaderLifetime> lifetime);
    /// Continues an authenticated reader without resetting its sent boundary.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment admitted by the owner's resume gate.
    ///   - clientInstance: The same authenticated frontend that owned the reader.
    ///   - acknowledgedSequence: A retained complete-batch cursor already sent.
    /// - Returns: Success, or a refusal leaving identity and standing unchanged.
    [[nodiscard]] std::expected<void, EffectRefusal> resumeReader(ID128 attachmentID, ID128 clientInstance, std::uint64_t acknowledgedSequence);
    /// Reconciles retention with the authoritative attachment table.
    ///
    /// This does not admit a connection or resume it. Missing, detached, or
    /// replaced identities lose their retention; Lost keeps the table's clock.
    ///
    /// - Parameters:
    ///   - table: The same host's authenticated attachment owner.
    ///   - nowMicroseconds: The monotonic clock for Lost retention expiry.
    void reconcileReaders(const AttachmentTable& table, std::uint64_t nowMicroseconds);
    /// Records the greatest complete batch handed to this reader's carrier.
    ///
    /// - Parameters:
    ///   - attachmentID: The live reader whose batch was accepted in full.
    ///   - throughSequence: Its monotonic sent cursor, at most the newest effect.
    /// - Returns: Success or a refusal that leaves its sent cursor unchanged.
    [[nodiscard]] std::expected<void, EffectRefusal> noteSent(ID128 attachmentID, std::uint64_t throughSequence);
    /// Accepts only monotonic acknowledgements of effects sent to this reader.
    ///
    /// - Parameters:
    ///   - attachmentID: The authenticated live reader sending the acknowledgement.
    ///   - throughSequence: Its cursor, at most what its own carrier was handed.
    /// - Returns: Success or a refusal that leaves retention unchanged.
    [[nodiscard]] std::expected<void, EffectRefusal> acknowledge(ID128 attachmentID, std::uint64_t throughSequence);
    /// Changes retention standing; repeated Lost reports do not restart grace.
    ///
    /// - Parameters:
    ///   - attachmentID: The reader whose authenticated attachment changed state.
    ///   - state: Live, Lost, or Detached; detachment releases its slot.
    ///   - nowMicroseconds: The owner's monotonic clock for starting Lost grace.
    /// - Returns: Success or a refusal for an unknown reader or invalid state.
    [[nodiscard]] std::expected<void, EffectRefusal> noteState(ID128 attachmentID, AttachmentState state, std::uint64_t nowMicroseconds);
    /// Expires Lost cursors and releases the prefix acknowledged by every reader.
    ///
    /// - Parameter nowMicroseconds: The owner's monotonic clock; a backwards value expires nothing.
    void trim(std::uint64_t nowMicroseconds);

    /// Counts retained effect identities.
    ///
    /// - Returns: The number of records, never above the policy's fixed ceiling.
    [[nodiscard]] std::size_t size() const { return m_size; }
    /// Counts owned kind-body and transfer bytes independently of fixed metadata.
    ///
    /// - Returns: The sum of the retained records' body and data sizes.
    [[nodiscard]] std::size_t retainedBytes() const { return m_retainedBytes; }
    /// Reports capacity for an admitted retained payload and producer scratch.
    ///
    /// - Returns: The unused bytes of this log's configured retained-data bound.
    [[nodiscard]] std::size_t availableRetainedBytes() const { return m_policy.maxRetainedBytes - m_retainedBytes; }
    /// Reports the pane's issued identity high-water mark.
    ///
    /// - Returns: The newest issued sequence, including trimmed effects.
    [[nodiscard]] std::uint64_t newestSequence() const { return m_lastSequence; }
    /// Reports the cursor before which complete replay is unavailable.
    ///
    /// - Returns: The greatest retired sequence; a lower read cursor is refused.
    [[nodiscard]] std::uint64_t retiredThrough() const { return m_retiredThrough; }
    /// Counts readers whose standing still retains effects.
    ///
    /// - Returns: Live and untrimmed Lost readers, never detached readers.
    [[nodiscard]] std::size_t readerCount() const;

private:
    /// Owns the bytes borrowed by one effect's view.
    struct Entry {
        EffectView view;
        std::unique_ptr<std::uint8_t[]> payload;
        std::unique_ptr<std::uint8_t[]> data;
    };

    /// Keeps one authenticated attachment's sent and acknowledged cursors.
    struct Reader {
        ID128 attachmentID;
        ID128 clientInstance;
        AttachmentState state = AttachmentState::Detached;
        std::uint64_t acknowledged = 0;
        std::uint64_t sent = 0;
        std::uint64_t lostSinceMicroseconds = 0;
        bool isTransient = false;
        std::weak_ptr<const EffectReaderLifetime> lifetime;
    };

    /// Finds a reader whose slot has not been detached.
    ///
    /// - Parameter attachmentID: The authenticated reader identity to locate.
    /// - Returns: Its slot, or null when it is not tracked.
    [[nodiscard]] Reader* reader(ID128 attachmentID);
    /// Checks owner-selected bounds against the fixed record storage.
    ///
    /// - Returns: Whether the pane identity and limits admit an append.
    [[nodiscard]] bool isPolicyValid() const;
    /// Validates an append before allocation or ownership transfer.
    ///
    /// - Parameter draft: Exact body and data spans from a typed producer.
    /// - Returns: Admission, or the reason the log must remain unchanged.
    [[nodiscard]] std::expected<void, EffectRefusal> admit(const EffectDraft& draft) const;

    const ID128 m_paneID;
    const EffectPolicy m_policy;
    std::array<Entry, maxEffectRecords> m_entries;
    std::array<Reader, maxEffectReaders> m_readers;
    std::size_t m_size = 0;
    std::size_t m_retainedBytes = 0;
    std::uint64_t m_lastSequence = 0;
    std::uint64_t m_lastGeneration = 0;
    std::uint64_t m_retiredThrough = 0;
};

} // namespace Ubiquitty::Protocol::Sync
