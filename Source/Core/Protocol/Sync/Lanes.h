// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Lanes.h — the two lanes one stream carries, and the bulk transfers
// that keep any frame from holding it (Docs/sync.md §6.5).
//
// A payload over one frame's cap travels as a bulk transfer: chunks of at most
// one frame, the last carrying a SHA-256 of the reassembled payload, announced
// by an interactive head frame that leaves before the first chunk. The
// scheduler hands the transport one frame at a time, so the longest an
// interactive frame waits behind bulk is one capped frame:
//
// - with only one lane pending, that lane goes;
// - with both pending, interactive goes, unless bulk has had less than its
//   guaranteed share of the bytes sent since bulk became pending *and* the
//   frame before was not bulk.
//
// The second clause is what bounds the wait: bulk never takes two frames in a
// row while an interactive frame waits. It also bounds the share bulk can be
// starved to, since a bulk frame follows any interactive frame while bulk is
// behind its share.
//

#include "Core/Protocol/Sync/BulkSource.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The bytes of a `BulkChunk` body other than its data: transfer, index, the
/// last flag, the data's three-byte length, and the SHA-256, coding and
/// inflated length the last chunk adds.
inline constexpr std::size_t bulkChunkOverheadBytes = 4 + 4 + 1 + 3 + 32 + 1 + 4;

/// The most payload bytes one chunk carries, so every chunk is one capped frame.
inline constexpr std::size_t bulkChunkPayloadBytes = maxFrameBodyBytes - bulkChunkOverheadBytes;

/// How a transfer's payload is coded on the wire.
struct PayloadCoding {
    Compression compression = Compression::None;
    /// The payload's length once inflated, for `Deflate`.
    std::uint32_t inflatedBytes = 0;
};

/// How the scheduler shares the stream.
///
/// The connection owner supplies the share; `measuredHostPolicy` provides
/// the default.
struct LanePolicy {
    /// The share of bytes sent that bulk is guaranteed while it is pending,
    /// in thousandths.
    std::uint32_t bulkSharePermille;
};

/// Whether the connection's pacing lets the next bulk frame leave.
enum class BulkPacing : std::uint8_t {
    /// The window has no room for it.
    Held,
    /// It fits the window.
    Admitted,
};

/// Which transfer the bulk lane serves first (Docs/sync.md §6.6). The bulk
/// lane sends the best-ranked transfer whose head has left, and a better one
/// queued behind a partly sent transfer overtakes it at the next chunk
/// boundary; transfers of one rank leave in the order they were queued.
/// `Checkpoint` is the best rank, so nothing overtakes a checkpoint.
enum class TransferRank : std::uint8_t {
    /// A checkpoint body.
    Checkpoint,
    /// The answer to a call: a person or an agent waits on it.
    Answer,
    /// History the user is looking at.
    Viewport,
    /// History a search result names.
    Search,
    /// History the client keeps ahead of the user.
    Backfill,
    /// Decoded image pixels, behind screen and every history class.
    Image,
};

/// What `abortTransfer` did.
enum class TransferAbort : std::uint8_t {
    /// No such transfer was queued: it had wholly left, or never existed.
    NotQueued,
    /// Its head had not left, so the receiver never heard of it: the head and
    /// the payload are gone, and nothing needs announcing.
    DroppedBeforeHead,
    /// Its head had left: its unsent chunks are gone, and the caller owes the
    /// receiver a `BulkAbort`.
    Aborted,
};

/// What a queued frame or run is to the caps that count it.
enum class FrameRole : std::uint8_t {
    /// Counted by its bytes alone.
    Message,
    /// The answer to a call: counted against the outstanding-call cap until
    /// it has left.
    Reply,
};

/// Orders a connection's outgoing stream frames across the two lanes.
class LaneScheduler {
public:
    /// Creates a scheduler.
    ///
    /// - Parameter policy: The bulk share.
    explicit LaneScheduler(const LanePolicy& policy)
        : m_policy(policy)
    {
    }

    /// Queues an interactive frame.
    ///
    /// - Parameters:
    ///   - kind: An interactive kind.
    ///   - channel: 0 for a connection kind, a pane handle otherwise.
    ///   - body: At most one frame's cap.
    ///   - role: Whether it answers a call.
    /// - Returns: The frame's bytes on the wire, or 0 when it was refused: over
    ///   the cap, of the wrong scope, or a bulk kind.
    [[nodiscard]] std::size_t enqueue(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body, FrameRole role = FrameRole::Message);

    /// The id the next `enqueueTransfer` will assign, for its head to name.
    [[nodiscard]] std::uint32_t nextTransferID() const { return m_nextTransferID; }

    /// Queues a bulk transfer behind the interactive head that announces it.
    /// No chunk leaves before the head does.
    ///
    /// - Parameters:
    ///   - headKind: The head's kind, an interactive kind.
    ///   - headChannel: The head's channel.
    ///   - headBody: The head's body, naming `nextTransferID()`.
    ///   - payload: The transfer's payload.
    ///   - rank: Where the bulk lane serves it among the transfers queued.
    ///   - role: Whether it answers a call, counted until its last chunk has
    ///     left.
    ///   - coding: How `payload` is coded, and its length once inflated,
    ///     which the last chunk declares.
    /// - Returns: The head's bytes on the wire, or 0 when the head was refused,
    ///   in which case nothing was queued.
    [[nodiscard]] std::size_t enqueueTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::vector<std::uint8_t> payload, TransferRank rank = TransferRank::Checkpoint, FrameRole role = FrameRole::Message, PayloadCoding coding = {});

    /// Copies an admitted raw payload into one exact fallible transfer owner.
    /// The caller reserves the complete retained byte count before this call;
    /// no input-sized vector growth occurs. Refusal queues no head or payload.
    ///
    /// - Parameters:
    ///   - headKind: The interactive announcement kind.
    ///   - headChannel: The announcement's channel.
    ///   - headBody: The announcement naming `nextTransferID()`.
    ///   - payload: Immutable bytes copied after validation.
    ///   - rank: Bulk service priority.
    ///   - role: Whether this transfer answers a call.
    /// - Returns: Queued head bytes, or zero on validation/allocation refusal.
    [[nodiscard]] std::size_t enqueueCopiedTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::span<const std::uint8_t> payload, TransferRank rank, FrameRole role = FrameRole::Message);

    /// Queues an already-admitted immutable source without materializing a
    /// payload vector. The source's reservation stays fully charged through
    /// partial sends, abort, or scheduler destruction; only one capped chunk
    /// is encoded at a time. Its bytes use the ordinary bulk grammar, the
    /// last chunk declaring `coding`. Failure destroys the passed owner and
    /// releases its charge. The caller must drain each output frame into its
    /// separately charged carrier owner before popping another, never
    /// accumulate a whole source.
    [[nodiscard]] std::size_t enqueueSourceTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::unique_ptr<BulkSource> source, TransferRank rank, FrameRole role = FrameRole::Message, PayloadCoding coding = {});

    /// The wire bytes of the bulk frame `popFrame` would send next, or 0 when
    /// no chunk may leave yet.
    ///
    /// - Parameter payloadLimit: Maximum payload bytes in this chunk; zero
    ///   holds bulk. The predictor and pop must use the same limit.
    [[nodiscard]] std::size_t nextBulkFrameBytes(std::size_t payloadLimit = maxFrameBodyBytes) const;

    /// Takes the next frame for the transport.
    ///
    /// - Parameters:
    ///   - output: The frame is appended here.
    ///   - bulkPacing: Whether the connection's pacing admits the bulk frame
    ///     `nextBulkFrameBytes` names.
    ///   - payloadLimit: The payload limit passed to the size predictor.
    /// - Returns: The lane the frame came from, or `nullopt` when nothing may
    ///   leave.
    [[nodiscard]] std::optional<Lane> popFrame(std::vector<std::uint8_t>& output, BulkPacing bulkPacing, std::size_t payloadLimit = maxFrameBodyBytes);

    /// Whether any frame is queued, sendable or not.
    [[nodiscard]] bool isEmpty() const { return m_interactive.empty() && m_bulkFrames.empty() && m_transfers.empty(); }

    /// The interactive frames queued.
    [[nodiscard]] std::size_t interactiveCount() const { return m_interactive.size(); }

    /// Whether an interactive frame of a kind is queued on a channel.
    ///
    /// - Parameters:
    ///   - kind: The frame's kind.
    ///   - channel: Its channel.
    /// - Returns: Whether one is.
    [[nodiscard]] bool isQueued(FrameKind kind, std::uint32_t channel) const;

    /// Drops unsent interactive frames of a kind on one channel. Transfer
    /// heads stay queued: their owner must use `abortTransfer` instead.
    /// Runs released by a dropped frame still wait for preceding frames.
    ///
    /// - Parameters:
    ///   - kind: The frame's kind.
    ///   - channel: Its channel.
    /// - Returns: The wire bytes withdrawn.
    [[nodiscard]] std::size_t withdraw(FrameKind kind, std::uint32_t channel);

    /// The transfers not yet wholly sent.
    [[nodiscard]] std::size_t transferCount() const { return m_transfers.size(); }

    /// The materialized queue's logical bytes: every interactive frame and
    /// the unsent suffix of each vector-backed transfer/run. Immutable source
    /// reservations are reported separately by `retainedSourceBytes`.
    [[nodiscard]] std::size_t queuedBytes() const { return m_queuedBytes; }

    /// The materialized transfer/run payload bytes still to send. A retained
    /// source is not another payload vector; its full reservation is separate.
    [[nodiscard]] std::size_t queuedBulkBytes() const { return m_queuedBulkBytes; }

    /// Full source reservations, including already-sent prefixes and bounded
    /// encoding scratch, until the owners are destroyed. The connection and
    /// aggregate ledgers also retain these charges after their ordinary owner
    /// is removed. Do not subtract this from an interactive queue's size.
    [[nodiscard]] std::size_t retainedSourceBytes() const { return m_retainedSourceBytes; }

    /// All materialized byte-vector capacity, including sent prefixes and
    /// spare allocation. Immutable source reservations are separate. Queue
    /// node/container bookkeeping is not a payload allocation in this count.
    /// It is kept as frames and transfers come and go, so asking costs the
    /// same however much is queued.
    [[nodiscard]] std::size_t retainedPayloadBytes() const { return m_retainedPayloadBytes; }

    /// The same capacity counted again from every queued frame and transfer:
    /// what `retainedPayloadBytes` equals, for a test to hold it to.
    [[nodiscard]] std::size_t countRetainedPayloadBytes() const;

    /// The replies queued, as frames or runs, that have not wholly left.
    [[nodiscard]] std::size_t queuedReplies() const { return m_queuedReplies; }

    /// Whether a transfer still has chunks to send.
    [[nodiscard]] bool isTransferQueued(std::uint32_t transfer) const;

    /// Whether a transfer still queued has had its head leave, so its receiver
    /// knows of it.
    ///
    /// - Parameter transfer: The transfer.
    /// - Returns: False for one not queued or whose head is still queued.
    [[nodiscard]] bool isTransferAnnounced(std::uint32_t transfer) const;

    /// Drops a transfer's unsent chunks. A chunk already taken is a whole frame
    /// ahead of anything queued after this call, so the peer never reads a
    /// chunk of an abandoned transfer behind a frame that told it to forget
    /// the transfer.
    ///
    /// - Parameter transfer: The transfer; one not queued is ignored.
    void abandonTransfer(std::uint32_t transfer);

    /// Drops every transfer's unsent chunks, as `abandonTransfer` does one's.
    void abandonTransfers();

    /// Drops everything queued: every interactive frame and every transfer.
    /// For an owner that has stopped sending what it queued and owes the peer
    /// only a frame it queues after this.
    void abandonAll();

    /// Aborts a transfer `enqueueTransfer` queued.
    ///
    /// A chunk already taken is a whole frame ahead of anything queued after
    /// this call, so a `BulkAbort` the caller queues follows every chunk of
    /// the transfer that left. A run ends by its own empty final fragment, so
    /// `abandonTransfer` is its verb, and a run's id is `NotQueued` here.
    ///
    /// - Parameter transfer: The transfer.
    /// - Returns: What became of it, and so whether a `BulkAbort` is owed.
    [[nodiscard]] TransferAbort abortTransfer(std::uint32_t transfer);

private:
    /// An interactive frame, and the transfers its departure releases.
    struct QueuedFrame {
        FrameKind kind = FrameKind::Bye;
        std::uint32_t channel = 0;
        FrameRole role = FrameRole::Message;
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint32_t> releasedTransfers;
        /// The transfer this frame is the head of.
        std::optional<std::uint32_t> headOf;
    };

    /// A typed transfer being cut into chunks.
    struct Transfer {
        std::uint32_t id = 0;
        std::vector<std::uint8_t> payload;
        std::unique_ptr<std::uint8_t[]> ownedPayload;
        std::size_t ownedPayloadBytes = 0;
        std::unique_ptr<BulkSource> source;
        std::array<std::uint8_t, 32> sha256 {};
        std::size_t offset = 0;
        std::uint32_t nextIndex = 0;
        bool isReleased = false;
        FrameRole role = FrameRole::Message;
        TransferRank rank = TransferRank::Checkpoint;
        PayloadCoding coding;

        [[nodiscard]] std::span<const std::uint8_t> bytes() const { return source ? source->bytes() : ownedPayload ? std::span<const std::uint8_t>(ownedPayload.get(), ownedPayloadBytes)
                                                                                                                   : std::span<const std::uint8_t>(payload); }
    };

    /// Uncounts what a transfer had still to send, as it leaves or is dropped.
    ///
    /// - Parameter transfer: The transfer.
    void noteTransferGone(const Transfer& transfer);

    /// Removes one interactive frame, uncounting its bytes and transferring
    /// any transfer dependencies to the preceding frame.
    ///
    /// - Parameter frame: The queued frame.
    /// - Returns: The following frame.
    std::deque<QueuedFrame>::iterator eraseFrame(std::deque<QueuedFrame>::iterator frame);

    /// The transfer the bulk lane serves next: the best-ranked one whose head
    /// has left, the earliest queued of its rank.
    ///
    /// - Returns: Its index in the queue, or `nullopt` when none may send.
    [[nodiscard]] std::optional<std::size_t> sendableTransfer() const;

    /// The frame a transfer sends next: its body's data bytes and whether it
    /// is the last.
    ///
    /// - Parameter transfer: The transfer.
    /// - Returns: The data bytes and the last flag.
    [[nodiscard]] static std::pair<std::size_t, bool> nextSlice(const Transfer& transfer, std::size_t payloadLimit);

    /// Whether any transfer may send a chunk.
    [[nodiscard]] bool hasSendableBulk() const { return sendableTransfer().has_value(); }

    /// Whether bulk has had less than its share since it became pending.
    [[nodiscard]] bool isBulkBehindShare() const;

    /// Cuts a transfer's next chunk into a frame.
    ///
    /// - Parameters:
    ///   - index: The transfer's index in the queue.
    ///   - output: The frame is appended here.
    void popChunk(std::size_t index, std::vector<std::uint8_t>& output, std::size_t payloadLimit);

    /// Records a frame sent.
    ///
    /// - Parameters:
    ///   - lane: The lane it came from.
    ///   - bytes: Its bytes on the wire.
    void noteSent(Lane lane, std::size_t bytes);

    /// The payload capacity a queued frame holds.
    ///
    /// - Parameter frame: The frame.
    [[nodiscard]] static std::size_t retainedOf(const QueuedFrame& frame);

    /// The payload capacity a queued transfer holds, its source aside.
    ///
    /// - Parameter transfer: The transfer.
    [[nodiscard]] static std::size_t retainedOf(const Transfer& transfer);

    LanePolicy m_policy;
    std::size_t m_queuedBytes = 0;
    std::size_t m_queuedBulkBytes = 0;
    std::size_t m_retainedSourceBytes = 0;
    std::size_t m_retainedPayloadBytes = 0;
    std::size_t m_queuedReplies = 0;
    std::deque<QueuedFrame> m_interactive;
    std::deque<QueuedFrame> m_bulkFrames;
    std::deque<Transfer> m_transfers;
    std::uint32_t m_nextTransferID = 1;
    std::uint64_t m_bytesSinceBulkPending = 0;
    std::uint64_t m_bulkBytesSinceBulkPending = 0;
    bool m_wasLastFrameBulk = false;
};

/// What a received chunk did to its transfer.
enum class ChunkOutcome : std::uint8_t {
    /// More chunks are owed.
    Pending,
    /// The transfer is whole and its SHA-256 matched; `take` returns it.
    Complete,
    /// The chunk named a transfer no head announced, arrived out of order,
    /// grew the transfer past its cap, or completed it with a SHA-256 that
    /// does not match. The transfer is dropped.
    Refused,
    /// The chunk belongs to a transfer the receiver abandoned; it is dropped
    /// and nothing else changes.
    Discarded,
    /// The chunk completed a deflated transfer whose stream does not inflate,
    /// or inflates short of the length it declared. The transfer is dropped.
    Corrupt,
    /// The receiver's memory owner had no room for what the chunk would make
    /// it hold. The transfer is abandoned and fenced; its sender is owed a
    /// typed refusal, never silence.
    OverBudget,
};

/// The most transfers a receiver keeps fenced at once; past it the oldest
/// fence is released first.
inline constexpr std::size_t maxFencedTransfers = 64;

/// Reassembles bulk transfers on the receiving side.
///
/// A transfer the receiver gives up on is fenced rather than forgotten: on a
/// carrier that can reorder lanes its chunks may still arrive, and a chunk of
/// a fenced transfer is discarded where one naming a transfer never announced
/// is refused. The fence lasts until the transfer's last chunk or its
/// `BulkAbort` arrives, and at most `maxFencedTransfers` are held.
class BulkReassembler {
public:
    /// Creates a reassembler whose transfers hold nothing beyond their caps.
    BulkReassembler() = default;
    /// Creates a reassembler whose transfers charge their payload capacity,
    /// and an inflated body's before it is allocated, to a memory owner.
    ///
    /// - Parameter budget: The receive owner the transfers charge.
    explicit BulkReassembler(std::shared_ptr<MemoryBudget> budget)
        : m_budget(std::move(budget))
    {
    }

    /// Records a transfer a head announced.
    ///
    /// - Parameters:
    ///   - transfer: The transfer's id.
    ///   - maxBytes: The largest payload it may reassemble to: the cap of its
    ///     head's kind (Docs/sync.md §7.9).
    /// - Returns: False when the id is already announced or fenced.
    [[nodiscard]] bool expect(std::uint32_t transfer, std::size_t maxBytes);

    /// Accepts one chunk.
    ///
    /// - Parameter chunk: The decoded chunk.
    /// - Returns: What the chunk did to its transfer.
    [[nodiscard]] ChunkOutcome accept(const BulkChunk& chunk);

    /// Takes a complete transfer's payload.
    ///
    /// - Parameters:
    ///   - transfer: The transfer's id.
    ///   - charge: Receives the payload's charge, so its owner stays charged
    ///     while the caller holds the bytes; null releases it.
    /// - Returns: The payload, empty when the transfer is not complete.
    /// - Precondition: `accept` returned `Complete` for the transfer.
    [[nodiscard]] std::vector<std::uint8_t> take(std::uint32_t transfer, MemoryCharge* charge = nullptr);

    /// Drops a transfer, complete or not, and fences its id: what of it is
    /// still to arrive is discarded.
    ///
    /// - Parameter transfer: The transfer's id; one not announced is ignored.
    void abandon(std::uint32_t transfer);

    /// Whether a transfer is announced and not yet taken or abandoned.
    [[nodiscard]] bool isExpected(std::uint32_t transfer) const { return m_transfers.contains(transfer); }

    /// Whether a transfer is fenced.
    [[nodiscard]] bool isFenced(std::uint32_t transfer) const;

    /// Releases a transfer's fence, as its `BulkAbort` does.
    ///
    /// - Parameter transfer: The transfer's id.
    /// - Returns: False when it was not fenced.
    [[nodiscard]] bool releaseFence(std::uint32_t transfer);

    /// The transfers announced and not yet taken or abandoned.
    [[nodiscard]] std::size_t transferCount() const { return m_transfers.size(); }

    /// The payload capacity the transfers hold, which their owner charges.
    [[nodiscard]] std::size_t heldBytes() const;

private:
    /// One transfer being reassembled.
    struct Transfer {
        std::vector<std::uint8_t> payload;
        /// The payload's capacity, charged before it grows.
        MemoryCharge charge;
        std::size_t maxBytes = 0;
        std::uint32_t nextIndex = 0;
        bool isComplete = false;
    };

    /// Grows a transfer's payload capacity to hold `additional` more bytes,
    /// charging the growth first. Capacity at least doubles, up to the cap, so
    /// a transfer of many chunks reallocates logarithmically often.
    ///
    /// - Returns: False, with the payload unchanged, when the owner has no room.
    [[nodiscard]] bool reserve(Transfer& transfer, std::size_t additional) const;

    /// Inflates a complete transfer's deflated payload in place.
    ///
    /// - Parameters:
    ///   - transfer: The transfer.
    ///   - inflatedBytes: The length the last chunk declares.
    /// - Returns: `Complete`; `Refused` for a declared length past the
    ///   transfer's bound or a stream that inflates past the declared length;
    ///   `Corrupt` for a stream that does not inflate or ends short of it.
    [[nodiscard]] ChunkOutcome inflate(Transfer& transfer, std::uint32_t inflatedBytes) const;

    std::shared_ptr<MemoryBudget> m_budget;
    std::unordered_map<std::uint32_t, Transfer> m_transfers;
    /// The fenced ids, oldest first.
    std::deque<std::uint32_t> m_fenced;
};

} // namespace Ubiquitty::Protocol::Sync
