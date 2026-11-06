// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_lanes.cpp — interactive never waits for bulk.
//
// A stream is modelled as a link of fixed rate that takes the next frame from
// the scheduler whenever it falls idle, as a transport writing into a socket
// with a small send buffer does. A `Receipt` queued while an 8 MiB checkpoint
// is in transfer must leave within one capped frame's transmission time; bulk
// keeps its guaranteed share under a flood of interactive frames; and the
// reassembler takes the transfer whole, chunk by chunk behind its head, and
// refuses one whose bytes do not match the SHA-256 it ends with.

#include "Harness.h"

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Messages.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

/// The provisional bulk share Docs/sync.md §6.5 names, as a test input.
constexpr LanePolicy quarterBulk { .bulkSharePermille = 250 };

/// 1 Mbit/s, the slow link Docs/sync.md §6.5 sizes the frame cap against.
constexpr std::uint64_t linkBytesPerSecond = 125'000;

std::vector<std::uint8_t> checkpointHeadBody(std::uint32_t transfer)
{
    std::vector<std::uint8_t> body;
    encode(body, CheckpointHead { .generation = 1, .transfer = transfer, .frameClass = FrameClass::Structural, .stateDigest = {} });
    return body;
}

std::vector<std::uint8_t> payloadOf(std::size_t bytes)
{
    std::vector<std::uint8_t> payload(bytes);
    for (std::size_t index = 0; index < bytes; ++index)
        payload[index] = static_cast<std::uint8_t>(index * 2654435761u >> 13);
    return payload;
}

std::uint64_t transmitMicroseconds(std::size_t bytes)
{
    return bytes * 1'000'000 / linkBytesPerSecond;
}

std::vector<std::uint8_t> historyHeadBody(std::uint32_t transfer)
{
    std::vector<std::uint8_t> body;
    encode(body, HistoryHead { .transfer = transfer, .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending });
    return body;
}

/// What a drained stream held: the transfer each chunk belonged to, and the
/// kinds of the interactive frames, in the order they left.
struct Drained {
    std::vector<std::uint32_t> chunkTransfers;
    std::vector<FrameKind> interactiveKinds;

    [[nodiscard]] std::size_t chunksOf(std::uint32_t transfer) const { return static_cast<std::size_t>(std::ranges::count(chunkTransfers, transfer)); }
};

Drained framesOf(const std::vector<std::uint8_t>& stream)
{
    Drained drained;
    FrameDecoder decoder;
    decoder.append(stream);
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind == FrameKind::BulkChunk)
            drained.chunkTransfers.push_back(decodeBulkChunk(frame->body)->transfer);
        else
            drained.interactiveKinds.push_back(frame->kind);
    }
    return drained;
}

/// Takes `count` frames, or every frame when `count` is zero.
std::vector<std::uint8_t> pop(LaneScheduler& scheduler, int count = 0)
{
    std::vector<std::uint8_t> stream;
    for (int taken = 0; count == 0 || taken < count; ++taken) {
        if (!scheduler.popFrame(stream, BulkPacing::Admitted))
            break;
    }
    return stream;
}

class CountedSource final : public BulkSource {
public:
    CountedSource(BulkSourceReservation reservation, std::size_t bytes, std::size_t& constructions, std::size_t& destructions)
        : BulkSource(std::move(reservation))
        , m_bytes(std::make_unique<std::uint8_t[]>(bytes))
        , m_size(bytes)
        , m_destructions(destructions)
    {
        ++constructions;
        std::fill_n(m_bytes.get(), bytes, 0x5a);
    }
    ~CountedSource() override { ++m_destructions; }
    std::span<const std::uint8_t> bytes() const override { return { m_bytes.get(), m_size }; }

private:
    std::unique_ptr<std::uint8_t[]> m_bytes;
    std::size_t m_size;
    std::size_t& m_destructions;
};

std::unique_ptr<BulkSource> makeSource(std::size_t bytes, const BulkSourceAdmission& admission, std::size_t& constructions, std::size_t& destructions)
{
    auto reservation = BulkSourceReservation::acquire(admission, bytes + sizeof(CountedSource) + bulkSourceFrameReserveBytes);
    if (!reservation)
        return {};
    return std::make_unique<CountedSource>(std::move(*reservation), bytes, constructions, destructions);
}

// Preserve the daemon's existing outbound high-waters without introducing a
// second allowance for typed sources; the aggregate remains four clients.
constexpr std::size_t existingClientOutboundHighWater = maxCatalogMessageBytes + sizeof(std::uint32_t)
    + catalogFrameHeaderBytes + maxEnvelopeHeaderBytes + maxFrameBodyBytes;

BulkSourceAdmission sourceAdmission()
{
    return { .connection = std::make_shared<BulkSourceBudget>(existingClientOutboundHighWater),
        .aggregate = std::make_shared<BulkSourceBudget>(4 * existingClientOutboundHighWater) };
}

} // namespace

TEST(sourceAdmissionChecksBothExistingBudgetsBeforeAllocating)
{
    auto admission = sourceAdmission();
    std::size_t constructions = 0;
    std::size_t destructions = 0;
    admission.otherConnectionBytes = 16u << 20;
    CHECK(!makeSource(64u << 20, admission, constructions, destructions));
    CHECK_EQ(constructions, 0u);
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
    admission.otherConnectionBytes = 0;
    admission.otherAggregateBytes = 4 * existingClientOutboundHighWater - bulkSourceFrameReserveBytes;
    CHECK(!makeSource(1, admission, constructions, destructions));
    CHECK_EQ(constructions, 0u);
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    admission.otherAggregateBytes = std::numeric_limits<std::size_t>::max();
    CHECK(!makeSource(1, admission, constructions, destructions));
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
}

TEST(aMaximumDecodedImageSourceKeepsItsFullChargeThroughPartialSendAndAbort)
{
    auto admission = sourceAdmission();
    std::size_t constructions = 0;
    std::size_t destructions = 0;
    auto source = makeSource(64u << 20, admission, constructions, destructions);
    REQUIRE(source);
    const auto charge = source->retainedBytes();
    LaneScheduler scheduler(quarterBulk);
    const auto transfer = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), std::move(source), TransferRank::Image) > 0);
    CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    CHECK_EQ(scheduler.retainedSourceBytes(), charge);
    CHECK_EQ(constructions, 1u);
    for (int frameIndex = 0; frameIndex != 3; ++frameIndex) {
        std::vector<std::uint8_t> frame;
        REQUIRE(scheduler.popFrame(frame, BulkPacing::Admitted));
        CHECK(frame.size() <= maxEnvelopeHeaderBytes + maxFrameBodyBytes);
        CHECK_EQ(admission.connection->heldBytes(), charge);
        CHECK_EQ(admission.aggregate->heldBytes(), charge);
        CHECK_EQ(scheduler.retainedSourceBytes(), charge);
        CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    }
    const std::array<std::uint8_t, 1> receipt { 0 };
    REQUIRE(scheduler.enqueue(FrameKind::Receipt, 1, receipt) > 0);
    std::vector<std::uint8_t> frame;
    CHECK(scheduler.popFrame(frame, BulkPacing::Admitted) == Lane::Interactive);
    CHECK(framesOf(frame).interactiveKinds == std::vector { FrameKind::Receipt });
    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::Aborted);
    CHECK_EQ(destructions, 1u);
    CHECK_EQ(scheduler.retainedSourceBytes(), 0u);
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
    CHECK(scheduler.isEmpty());
}

TEST(aSourceCompletesWithExactFrameAdmissionAndReleasesItsReplyAndOwner)
{
    auto admission = sourceAdmission();
    std::size_t constructions = 0;
    std::size_t destructions = 0;
    constexpr auto bytes = 2 * bulkChunkPayloadBytes + 17;
    auto source = makeSource(bytes, admission, constructions, destructions);
    REQUIRE(source);
    const auto charge = source->retainedBytes();
    LaneScheduler scheduler(quarterBulk);
    const auto transfer = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), std::move(source), TransferRank::Image, FrameRole::Reply) > 0);
    CHECK_EQ(scheduler.queuedReplies(), 1u);
    BulkReassembler receiver;
    CHECK(receiver.expect(transfer, bytes));
    bool sawHead = false;
    std::size_t complete = 0;
    while (!scheduler.isEmpty()) {
        const auto expectedBulkBytes = scheduler.nextBulkFrameBytes();
        std::vector<std::uint8_t> encoded;
        const auto lane = scheduler.popFrame(encoded, BulkPacing::Admitted);
        REQUIRE(lane);
        if (*lane == Lane::Bulk)
            CHECK_EQ(encoded.size(), expectedBulkBytes);
        FrameDecoder decoder;
        decoder.append(encoded);
        const auto frame = decoder.next();
        REQUIRE(frame);
        if (frame->kind == FrameKind::HistoryHead) {
            sawHead = true;
            continue;
        }
        REQUIRE(sawHead && frame->kind == FrameKind::BulkChunk);
        const auto chunk = decodeBulkChunk(frame->body);
        REQUIRE(chunk);
        if (chunk->isLast) {
            CHECK(receiver.accept(*chunk) == ChunkOutcome::Complete);
            const auto received = receiver.take(transfer);
            CHECK_EQ(received.size(), bytes);
            CHECK(std::ranges::all_of(received, [](std::uint8_t value) { return value == 0x5a; }));
            ++complete;
        } else {
            CHECK(receiver.accept(*chunk) == ChunkOutcome::Pending);
            CHECK_EQ(admission.connection->heldBytes(), charge);
            CHECK_EQ(destructions, 0u);
        }
    }
    CHECK_EQ(complete, 1u);
    CHECK_EQ(destructions, 1u);
    CHECK_EQ(scheduler.queuedReplies(), 0u);
    CHECK_EQ(scheduler.queuedBytes(), 0u);
    CHECK_EQ(scheduler.retainedSourceBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
}

TEST(aVectorBackedReplyReleasesItsCallSlotOnTheFinalChunk)
{
    for (const std::size_t bytes : { std::size_t { 0 }, std::size_t { 1 }, bulkChunkPayloadBytes + 1 }) {
        LaneScheduler scheduler(quarterBulk);
        REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(scheduler.nextTransferID()), payloadOf(bytes), TransferRank::Answer, FrameRole::Reply) > 0);
        CHECK_EQ(scheduler.queuedReplies(), 1u);
        while (!scheduler.isEmpty()) {
            const auto expectedBulkBytes = scheduler.nextBulkFrameBytes();
            std::vector<std::uint8_t> frame;
            const auto lane = scheduler.popFrame(frame, BulkPacing::Admitted);
            REQUIRE(lane);
            if (*lane == Lane::Bulk)
                CHECK_EQ(frame.size(), expectedBulkBytes);
            CHECK_EQ(scheduler.queuedReplies(), scheduler.isEmpty() ? 0u : 1u);
        }
        CHECK_EQ(scheduler.queuedBytes(), 0u);
        CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    }
}

TEST(creditSizedChunksKeepExactAdmissionAndWholePayloadOwnership)
{
    for (int owner = 0; owner < 3; ++owner) {
        const auto admission = sourceAdmission();
        std::size_t constructions = 0;
        std::size_t destructions = 0;
        const auto plain = owner == 2 ? std::vector<std::uint8_t>(5003, 0x5a) : payloadOf(5003);
        LaneScheduler scheduler(quarterBulk);
        const auto transfer = scheduler.nextTransferID();
        if (owner == 2) {
            REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), makeSource(plain.size(), admission, constructions, destructions), TransferRank::Image) > 0);
        } else {
            const auto payload = owner ? Ubiquitty::Codec::Zlib::compress(plain) : plain;
            const PayloadCoding coding = owner ? PayloadCoding { .compression = Compression::Deflate, .inflatedBytes = static_cast<std::uint32_t>(plain.size()) } : PayloadCoding {};
            REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), payload, TransferRank::Checkpoint, FrameRole::Message, coding) > 0);
        }
        BulkReassembler receiver;
        REQUIRE(receiver.expect(transfer, plain.size()));
        std::vector<std::uint8_t> frame;
        REQUIRE(scheduler.popFrame(frame, BulkPacing::Admitted, 0) == Lane::Interactive);
        CHECK_EQ(scheduler.nextBulkFrameBytes(0), 0u);
        frame.clear();
        CHECK(!scheduler.popFrame(frame, BulkPacing::Admitted, 0));
        std::size_t index = 0;
        std::size_t complete = 0;
        constexpr std::array<std::size_t, 4> limits { 1, 127, 128, 1024 };
        while (!scheduler.isEmpty()) {
            const auto limit = limits[index++ % limits.size()];
            const auto predicted = scheduler.nextBulkFrameBytes(limit);
            frame.clear();
            REQUIRE(scheduler.popFrame(frame, BulkPacing::Admitted, limit) == Lane::Bulk);
            CHECK_EQ(frame.size(), predicted);
            FrameDecoder decoder;
            decoder.append(frame);
            const auto envelope = decoder.next();
            REQUIRE(envelope);
            const auto chunk = decodeBulkChunk(envelope->body);
            REQUIRE(chunk && !chunk->bytes.empty() && chunk->bytes.size() <= limit);
            const auto outcome = receiver.accept(*chunk);
            if (chunk->isLast) {
                CHECK(outcome == ChunkOutcome::Complete);
                CHECK(receiver.take(transfer) == plain);
                ++complete;
            } else {
                CHECK(outcome == ChunkOutcome::Pending);
                if (owner == 2)
                    CHECK_EQ(destructions, 0u);
            }
        }
        CHECK_EQ(complete, 1u);
        CHECK_EQ(admission.connection->heldBytes(), 0u);
        CHECK_EQ(destructions, owner == 2 ? 1u : 0u);
    }
}

TEST(typedBulkChunksRespectTheExactCreditSizedPayloadLimit)
{
    LaneScheduler scheduler(quarterBulk);
    const auto payload = payloadOf(257);
    const auto transfer = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), payload));
    std::vector<std::uint8_t> head;
    REQUIRE(scheduler.popFrame(head, BulkPacing::Admitted, 127) == Lane::Interactive);
    BulkReassembler receiver;
    REQUIRE(receiver.expect(transfer, payload.size()));
    std::size_t finals = 0;
    while (!scheduler.isEmpty()) {
        const auto predicted = scheduler.nextBulkFrameBytes(127);
        std::vector<std::uint8_t> bytes;
        REQUIRE(scheduler.popFrame(bytes, BulkPacing::Admitted, 127) == Lane::Bulk);
        CHECK_EQ(bytes.size(), predicted);
        FrameDecoder decoder;
        decoder.append(bytes);
        const auto frame = decoder.next();
        REQUIRE(frame && frame->kind == FrameKind::BulkChunk);
        const auto chunk = decodeBulkChunk(frame->body);
        REQUIRE(chunk && chunk->bytes.size() <= 127);
        finals += chunk->isLast ? 1 : 0;
        CHECK(receiver.accept(*chunk) == (chunk->isLast ? ChunkOutcome::Complete : ChunkOutcome::Pending));
    }
    CHECK(receiver.take(transfer) == payload);
    CHECK_EQ(finals, 1u);
}

TEST(aDeflatedReplyIncludesItsCodingTrailerInExactFrameAdmission)
{
    for (const std::size_t bytes : { std::size_t { 1 }, 2 * bulkChunkPayloadBytes + 17 }) {
        const auto plain = payloadOf(bytes);
        const auto compressed = Ubiquitty::Codec::Zlib::compress(plain);
        REQUIRE(!compressed.empty());
        LaneScheduler scheduler(quarterBulk);
        const auto transfer = scheduler.nextTransferID();
        REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), compressed, TransferRank::Answer, FrameRole::Reply,
                    { .compression = Compression::Deflate, .inflatedBytes = static_cast<std::uint32_t>(plain.size()) })
            > 0);
        BulkReassembler receiver;
        REQUIRE(receiver.expect(transfer, std::max(plain.size(), compressed.size())));
        std::size_t complete = 0;
        while (!scheduler.isEmpty()) {
            const auto expectedBulkBytes = scheduler.nextBulkFrameBytes();
            std::vector<std::uint8_t> encoded;
            const auto lane = scheduler.popFrame(encoded, BulkPacing::Admitted);
            REQUIRE(lane);
            if (*lane != Lane::Bulk)
                continue;
            CHECK_EQ(encoded.size(), expectedBulkBytes);
            FrameDecoder decoder;
            decoder.append(encoded);
            const auto frame = decoder.next();
            REQUIRE(frame && frame->kind == FrameKind::BulkChunk);
            const auto chunk = decodeBulkChunk(frame->body);
            REQUIRE(chunk);
            if (chunk->isLast) {
                CHECK(chunk->compression == Compression::Deflate);
                CHECK_EQ(chunk->inflatedBytes, plain.size());
                CHECK(receiver.accept(*chunk) == ChunkOutcome::Complete);
                CHECK(receiver.take(transfer) == plain);
                ++complete;
            } else {
                CHECK(receiver.accept(*chunk) == ChunkOutcome::Pending);
            }
            CHECK_EQ(scheduler.queuedReplies(), scheduler.isEmpty() ? 0u : 1u);
        }
        CHECK_EQ(complete, 1u);
        CHECK_EQ(scheduler.queuedReplies(), 0u);
        CHECK_EQ(scheduler.queuedBytes(), 0u);
        CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    }
}

TEST(aSourceChargeOutlivesTheOrdinaryConnectionOwnerAndEveryDropPathReleasesIt)
{
    for (int ending = 0; ending != 4; ++ending) {
        auto admission = sourceAdmission();
        const auto aggregate = admission.aggregate;
        const std::weak_ptr<BulkSourceBudget> connection = admission.connection;
        std::size_t constructions = 0;
        std::size_t destructions = 0;
        auto source = makeSource(32, admission, constructions, destructions);
        REQUIRE(source);
        const auto charge = source->retainedBytes();
        admission = {};
        CHECK(!connection.expired());
        {
            LaneScheduler scheduler(quarterBulk);
            const auto transfer = scheduler.nextTransferID();
            if (ending == 0) {
                CHECK_EQ(scheduler.enqueueSourceTransfer(FrameKind::BulkChunk, 0, {}, std::move(source), TransferRank::Image), 0u);
            } else {
                REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), std::move(source), TransferRank::Image) > 0);
                CHECK_EQ(aggregate->heldBytes(), charge);
                if (ending == 1)
                    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::DroppedBeforeHead);
                else if (ending == 2)
                    scheduler.abandonTransfers();
                // The last case destroys the scheduler with its head queued.
            }
        }
        CHECK_EQ(destructions, 1u);
        CHECK_EQ(aggregate->heldBytes(), 0u);
        CHECK(connection.expired());
    }
}

TEST(concurrentSourceReservationsKeepAggregateRoomAndMovesReleaseExactlyOnce)
{
    const auto aggregate = std::make_shared<BulkSourceBudget>(100);
    BulkSourceAdmission first { .connection = std::make_shared<BulkSourceBudget>(100), .aggregate = aggregate };
    BulkSourceAdmission second { .connection = std::make_shared<BulkSourceBudget>(100), .aggregate = aggregate };
    auto held = BulkSourceReservation::acquire(first, 60);
    REQUIRE(held);
    CHECK(!BulkSourceReservation::acquire(second, 41));
    CHECK_EQ(second.connection->heldBytes(), 0u);
    CHECK_EQ(aggregate->heldBytes(), 60u);
    auto other = BulkSourceReservation::acquire(second, 40);
    REQUIRE(other);
    CHECK_EQ(aggregate->heldBytes(), 100u);
    *other = std::move(*held);
    CHECK_EQ(first.connection->heldBytes(), 60u);
    CHECK_EQ(second.connection->heldBytes(), 0u);
    CHECK_EQ(aggregate->heldBytes(), 60u);
    held.reset();
    CHECK_EQ(aggregate->heldBytes(), 60u);
    other.reset();
    CHECK_EQ(first.connection->heldBytes(), 0u);
    CHECK_EQ(aggregate->heldBytes(), 0u);
    first.aggregate = first.connection;
    CHECK(!BulkSourceReservation::acquire(first, 1));
    CHECK(!BulkSourceReservation::acquire(second, 0));
}

TEST(retainedPayloadCapacityIncludesSpareAndAlreadySentBytesUntilTransferRelease)
{
    LaneScheduler scheduler(quarterBulk);
    auto payload = payloadOf(2 * bulkChunkPayloadBytes);
    payload.reserve(4 * bulkChunkPayloadBytes);
    const auto capacity = payload.capacity();
    const auto transfer = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), std::move(payload)) > 0);
    CHECK(scheduler.retainedPayloadBytes() > capacity); // The queued head owns storage too.
    (void)pop(scheduler, 1);
    CHECK_EQ(scheduler.retainedPayloadBytes(), capacity);
    (void)pop(scheduler, 1);
    CHECK_EQ(scheduler.queuedBytes(), bulkChunkPayloadBytes);
    CHECK_EQ(scheduler.retainedPayloadBytes(), capacity);
    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::Aborted);
    CHECK_EQ(scheduler.retainedPayloadBytes(), 0u);
}

TEST(theRetainedPayloadTotalIsTheWalkThroughEveryWayAFrameComesAndGoes)
{
    // The total is kept, not walked, so a long queue costs nothing to ask; it
    // must equal the walk after every kind of change.
    auto admission = sourceAdmission();
    std::size_t constructions = 0;
    std::size_t destructions = 0;
    LaneScheduler scheduler(quarterBulk);
    const auto isExact = [&](const char* step) {
        CHECK_MSG(scheduler.retainedPayloadBytes() == scheduler.countRetainedPayloadBytes(), step);
    };
    const std::vector<std::uint8_t> body { 1, 2, 3 };
    CHECK(scheduler.enqueue(FrameKind::Update, 1, body) > 0);
    CHECK(scheduler.enqueue(FrameKind::NativeDragCommand, 1, body) > 0);
    isExact("after frames on both lanes");
    const auto vector = scheduler.nextTransferID();
    auto payload = payloadOf(3 * bulkChunkPayloadBytes);
    payload.reserve(5 * bulkChunkPayloadBytes);
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(vector), std::move(payload)) > 0);
    const auto copied = scheduler.nextTransferID();
    CHECK(scheduler.enqueueCopiedTransfer(FrameKind::HistoryHead, 2, historyHeadBody(copied), payloadOf(bulkChunkPayloadBytes + 5), TransferRank::Backfill) > 0);
    const auto sourced = scheduler.nextTransferID();
    CHECK(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 3, historyHeadBody(sourced), makeSource(2 * bulkChunkPayloadBytes, admission, constructions, destructions), TransferRank::Image) > 0);
    CHECK(scheduler.enqueue(FrameKind::Update, 2, body) > 0);
    isExact("after transfers of every kind");
    // Withdrawing a frame ahead of a head hands its released runs on.
    CHECK(scheduler.withdraw(FrameKind::Update, 1) > 0);
    isExact("after a withdrawn frame passed its runs on");
    CHECK(scheduler.withdraw(FrameKind::NativeDragCommand, 1) > 0);
    isExact("after a withdrawn bulk frame");
    (void)pop(scheduler, 3);
    isExact("after frames and chunks left");
    CHECK(scheduler.abortTransfer(copied) != TransferAbort::NotQueued);
    isExact("after an abort");
    scheduler.abandonTransfer(sourced);
    isExact("after an abandoned transfer");
    (void)pop(scheduler);
    isExact("after the queue drained");
    CHECK(scheduler.enqueue(FrameKind::Update, 4, body) > 0);
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 4, historyHeadBody(scheduler.nextTransferID()), payloadOf(17)) > 0);
    scheduler.abandonAll();
    isExact("after everything was abandoned");
    CHECK_EQ(scheduler.retainedPayloadBytes(), 0u);
}

TEST(aHistoryTransferOvertakesAPartlySentImageSourceAtOneChunkBoundary)
{
    auto admission = sourceAdmission();
    std::size_t constructions = 0;
    std::size_t destructions = 0;
    auto source = makeSource(3 * bulkChunkPayloadBytes, admission, constructions, destructions);
    REQUIRE(source);
    LaneScheduler scheduler(quarterBulk);
    const auto image = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, historyHeadBody(image), std::move(source), TransferRank::Image) > 0);
    std::vector<std::uint32_t> transfers;
    auto takeFrame = [&] {
        std::vector<std::uint8_t> frame;
        const auto lane = scheduler.popFrame(frame, BulkPacing::Admitted);
        CHECK(lane);
        const auto decoded = framesOf(frame);
        transfers.insert(transfers.end(), decoded.chunkTransfers.begin(), decoded.chunkTransfers.end());
    };
    takeFrame(); // The image's head.
    takeFrame(); // One image chunk.
    const auto history = scheduler.nextTransferID();
    REQUIRE(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(history), payloadOf(17), TransferRank::Backfill) > 0);
    while (!scheduler.isEmpty())
        takeFrame();
    CHECK((transfers == std::vector<std::uint32_t> { image, history, image, image }));
    CHECK_EQ(destructions, 1u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
}

TEST(withdrawingOnePanesFramesPreservesTheOtherPaneAndTransferOrder)
{
    LaneScheduler scheduler(quarterBulk);
    const std::vector<std::uint8_t> body { 1, 2, 3 };
    const std::size_t otherBytes = scheduler.enqueue(FrameKind::Update, 2, body);
    const std::size_t removedBytes = scheduler.enqueue(FrameKind::HistoryInline, 1, body, FrameRole::Reply);
    const std::uint32_t transfer = scheduler.nextTransferID();
    const auto headBytes = scheduler.enqueueTransfer(FrameKind::HistoryHead, 2, historyHeadBody(transfer), body);
    REQUIRE(otherBytes > 0 && removedBytes > 0);
    CHECK_EQ(scheduler.queuedReplies(), 1u);
    CHECK_EQ(scheduler.withdraw(FrameKind::HistoryInline, 1), removedBytes);
    CHECK_EQ(scheduler.queuedReplies(), 0u);
    CHECK_EQ(scheduler.queuedBytes(), otherBytes + headBytes + body.size());
    CHECK_EQ(scheduler.nextBulkFrameBytes(), 0u);
    CHECK_EQ(scheduler.withdraw(FrameKind::Update, 1), 0u);
    const Drained first = framesOf(pop(scheduler, 1));
    CHECK(first.interactiveKinds == std::vector { FrameKind::Update });
    CHECK_EQ(scheduler.nextBulkFrameBytes(), 0u);
    const Drained head = framesOf(pop(scheduler, 1));
    CHECK(head.interactiveKinds == std::vector { FrameKind::HistoryHead });
    CHECK(scheduler.nextBulkFrameBytes() > 0);
    const Drained rest = framesOf(pop(scheduler));
    CHECK(rest.interactiveKinds.empty());
    CHECK(rest.chunkTransfers == std::vector { transfer });
    CHECK(!scheduler.isTransferQueued(transfer));
    CHECK_EQ(scheduler.queuedBytes(), 0u);
}

TEST(withdrawingAnEarlierFrameNeverReleasesBulkAheadOfItsHead)
{
    LaneScheduler scheduler(quarterBulk);
    const std::vector<std::uint8_t> body { 1 };
    const std::size_t bytes = scheduler.enqueue(FrameKind::Update, 1, body);
    const std::uint32_t transfer = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 2, historyHeadBody(transfer), body) > 0);
    CHECK_EQ(scheduler.withdraw(FrameKind::Update, 1), bytes);
    CHECK_EQ(scheduler.nextBulkFrameBytes(), 0u);
    CHECK_EQ(scheduler.withdraw(FrameKind::HistoryHead, 2), 0u);
    CHECK(scheduler.isQueued(FrameKind::HistoryHead, 2));
    const auto head = framesOf(pop(scheduler, 1));
    CHECK(head.interactiveKinds == std::vector { FrameKind::HistoryHead });
    CHECK(scheduler.nextBulkFrameBytes() > 0);
    (void)pop(scheduler);
    CHECK(!scheduler.isTransferQueued(transfer));
    CHECK_EQ(scheduler.queuedBytes(), 0u);
}

TEST(aReceiptBehindAnEightMebibyteCheckpointLeavesWithinOneFrameTime)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t transfer = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(transfer), payloadOf(8 << 20)) > 0);

    std::vector<std::uint8_t> receiptBody;
    encode(receiptBody, Receipt { .inputWritten = 10, .inputSettled = 10 });
    const std::size_t receiptFrameBytes = frameBytes(1, receiptBody.size());

    // The head leaves first, then the link is busy with the first chunk when
    // the receipt is queued, a little way into the transfer.
    std::uint64_t now = 0;
    std::uint64_t linkFreeAt = 0;
    std::optional<std::uint64_t> receiptQueuedAt;
    std::optional<std::uint64_t> receiptDeliveredAt;
    int framesSent = 0;
    while (!receiptDeliveredAt && framesSent < 1000) {
        std::vector<std::uint8_t> frame;
        const std::optional<Lane> lane = scheduler.popFrame(frame, BulkPacing::Admitted);
        REQUIRE(lane);
        const std::uint64_t start = std::max(now, linkFreeAt);
        linkFreeAt = start + transmitMicroseconds(frame.size());
        ++framesSent;
        if (receiptQueuedAt && frame[0] == static_cast<std::uint8_t>(FrameKind::Receipt))
            receiptDeliveredAt = linkFreeAt;
        if (framesSent == 5) {
            // Mid-chunk: the link is a third of the way through this frame.
            now = start + transmitMicroseconds(frame.size()) / 3;
            CHECK(scheduler.enqueue(FrameKind::Receipt, 1, receiptBody) > 0);
            receiptQueuedAt = now;
        }
        now = linkFreeAt;
    }
    REQUIRE(receiptDeliveredAt);
    const std::uint64_t waited = *receiptDeliveredAt - *receiptQueuedAt;
    const std::uint64_t oneFrameTime = transmitMicroseconds(frameBytes(0, maxFrameBodyBytes) + receiptFrameBytes);
    CHECK_MSG(waited <= oneFrameTime, ("the receipt waited " + std::to_string(waited) + " us; one frame time is " + std::to_string(oneFrameTime) + " us").c_str());
    CHECK(scheduler.transferCount() == 1);
}

TEST(bulkKeepsItsShareUnderAnInteractiveFlood)
{
    LaneScheduler scheduler(quarterBulk);
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(scheduler.nextTransferID()), payloadOf(8 << 20)) > 0);
    const std::vector<std::uint8_t> update(1000, 0x11);
    std::uint64_t bulkBytes = 0;
    std::uint64_t allBytes = 0;
    for (int index = 0; index < 4000; ++index) {
        while (scheduler.interactiveCount() < 4)
            CHECK(scheduler.enqueue(FrameKind::Update, 1, update) > 0);
        std::vector<std::uint8_t> frame;
        const std::optional<Lane> lane = scheduler.popFrame(frame, BulkPacing::Admitted);
        REQUIRE(lane);
        allBytes += frame.size();
        if (*lane == Lane::Bulk)
            bulkBytes += frame.size();
    }
    const std::uint64_t permille = bulkBytes * 1000 / allBytes;
    CHECK_MSG(permille >= 240, ("bulk took " + std::to_string(permille) + " per mille of the bytes sent").c_str());
}

TEST(bulkNeverTakesTwoFramesWhileInteractiveWaits)
{
    // A share of the whole stream: bulk is always behind it, so only the
    // alternation keeps an interactive frame from waiting two chunks.
    LaneScheduler scheduler(LanePolicy { .bulkSharePermille = 1000 });
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(scheduler.nextTransferID()), payloadOf(2 << 20)) > 0);
    const std::vector<std::uint8_t> update(100, 0x22);
    bool wasBulk = false;
    for (int index = 0; index < 200; ++index) {
        CHECK(scheduler.enqueue(FrameKind::Update, 1, update) > 0);
        std::vector<std::uint8_t> frame;
        const std::optional<Lane> lane = scheduler.popFrame(frame, BulkPacing::Admitted);
        REQUIRE(lane);
        CHECK(!(wasBulk && *lane == Lane::Bulk));
        wasBulk = *lane == Lane::Bulk;
    }
}

TEST(aTransferArrivesWholeBehindItsHeadAndIsCheckedBySHA256)
{
    LaneScheduler scheduler(quarterBulk);
    const std::vector<std::uint8_t> payload = payloadOf(300'000);
    const std::uint32_t transfer = scheduler.nextTransferID();
    // Interactive frames queued ahead of the head: no chunk may pass them,
    // whatever the share says.
    for (int index = 0; index < 3; ++index)
        CHECK(scheduler.enqueue(FrameKind::Update, 1, std::vector<std::uint8_t>(10, 1)) > 0);
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(transfer), payload) > 0);

    std::vector<std::uint8_t> stream;
    while (!scheduler.isEmpty()) {
        CHECK(scheduler.nextBulkFrameBytes() <= frameBytes(0, maxFrameBodyBytes));
        REQUIRE(scheduler.popFrame(stream, BulkPacing::Admitted));
    }
    FrameDecoder decoder;
    decoder.append(stream);
    BulkReassembler reassembler;
    std::vector<std::vector<std::uint8_t>> chunkBodies;
    bool sawHead = false;
    std::vector<std::uint8_t> received;
    while (const std::optional<Frame> frame = decoder.next()) {
        CHECK(frame->body.size() <= maxFrameBodyBytes);
        if (frame->kind == FrameKind::CheckpointHead) {
            sawHead = true;
            const std::optional<CheckpointHead> head = decodeCheckpointHead(frame->body);
            REQUIRE(head);
            CHECK(reassembler.expect(head->transfer, 8 << 20));
            continue;
        }
        if (frame->kind != FrameKind::BulkChunk)
            continue;
        CHECK(sawHead);
        chunkBodies.emplace_back(frame->body.begin(), frame->body.end());
        const std::optional<BulkChunk> chunk = decodeBulkChunk(frame->body);
        REQUIRE(chunk);
        const ChunkOutcome outcome = reassembler.accept(*chunk);
        if (outcome == ChunkOutcome::Complete)
            received = reassembler.take(transfer);
        else
            CHECK(outcome == ChunkOutcome::Pending);
    }
    CHECK(received == payload);
    CHECK_EQ(chunkBodies.size(), (payload.size() + bulkChunkPayloadBytes - 1) / bulkChunkPayloadBytes);

    // The same chunks with one payload byte flipped: refused at the last.
    BulkReassembler corrupted;
    CHECK(corrupted.expect(transfer, 8 << 20));
    chunkBodies[1][20] ^= 0x01;
    ChunkOutcome last = ChunkOutcome::Pending;
    for (const std::vector<std::uint8_t>& body : chunkBodies)
        last = corrupted.accept(*decodeBulkChunk(body));
    CHECK(last == ChunkOutcome::Refused);

    // A transfer no head announced, and one past the receiver's cap.
    BulkReassembler unannounced;
    CHECK(unannounced.accept(*decodeBulkChunk(chunkBodies[0])) == ChunkOutcome::Refused);
    BulkReassembler small;
    CHECK(small.expect(transfer, 1000));
    CHECK(small.accept(*decodeBulkChunk(chunkBodies[0])) == ChunkOutcome::Refused);
}

// Once a transfer is aborted the scheduler sends nothing more
// of it, however many chunks it had left.
TEST(anAbortedTransferSendsNoFurtherChunk)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t transfer = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), payloadOf(8 * bulkChunkPayloadBytes), TransferRank::Backfill) > 0);
    const Drained sent = framesOf(pop(scheduler, 3));
    CHECK_EQ(sent.chunksOf(transfer), std::size_t { 2 });

    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::Aborted);
    CHECK_EQ(framesOf(pop(scheduler)).chunksOf(transfer), std::size_t { 0 });
    CHECK(scheduler.isEmpty());
    CHECK_EQ(scheduler.queuedBulkBytes(), std::size_t { 0 });
    CHECK_EQ(scheduler.queuedBytes(), std::size_t { 0 });
    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::NotQueued);
}

// A transfer whose head has not left is one the receiver never heard of: its
// head goes with it, and there is nothing to announce.
TEST(aTransferAbortedBeforeItsHeadLeftAnnouncesNothing)
{
    LaneScheduler scheduler(quarterBulk);
    CHECK(scheduler.enqueue(FrameKind::Update, 1, std::vector<std::uint8_t>(10, 1)) > 0);
    const std::uint32_t transfer = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), payloadOf(3 * bulkChunkPayloadBytes), TransferRank::Viewport) > 0);
    CHECK(scheduler.enqueue(FrameKind::Receipt, 1, std::vector<std::uint8_t>(16, 0)) > 0);

    CHECK(scheduler.abortTransfer(transfer) == TransferAbort::DroppedBeforeHead);
    const Drained drained = framesOf(pop(scheduler));
    CHECK((drained.interactiveKinds == std::vector<FrameKind> { FrameKind::Update, FrameKind::Receipt }));
    CHECK(drained.chunkTransfers.empty());
    CHECK_EQ(scheduler.queuedBytes(), std::size_t { 0 });
}

// The bulk lane serves the best rank first: a viewport the user is looking at
// overtakes a backfill already under way, at the next chunk boundary, and the
// backfill resumes where it stopped.
TEST(aViewportTransferOvertakesAPartlySentBackfill)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t backfill = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(backfill), payloadOf(4 * bulkChunkPayloadBytes), TransferRank::Backfill) > 0);
    std::vector<std::uint8_t> stream = pop(scheduler, 2);
    const std::uint32_t viewport = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(viewport), payloadOf(2 * bulkChunkPayloadBytes), TransferRank::Viewport) > 0);
    const std::vector<std::uint8_t> rest = pop(scheduler);
    stream.insert(stream.end(), rest.begin(), rest.end());

    const Drained drained = framesOf(stream);
    CHECK((drained.chunkTransfers == std::vector<std::uint32_t> { backfill, viewport, viewport, backfill, backfill, backfill }));

    // Both arrive whole: the receiver keys each chunk by its transfer.
    BulkReassembler reassembler;
    CHECK(reassembler.expect(backfill, maxHistoryTransferBytes));
    CHECK(reassembler.expect(viewport, maxHistoryTransferBytes));
    FrameDecoder decoder;
    decoder.append(stream);
    int complete = 0;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind == FrameKind::BulkChunk)
            complete += reassembler.accept(*decodeBulkChunk(frame->body)) == ChunkOutcome::Complete ? 1 : 0;
    }
    CHECK_EQ(complete, 2);
}

TEST(nothingOvertakesACheckpoint)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t checkpoint = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(checkpoint), payloadOf(3 * bulkChunkPayloadBytes)) > 0);
    std::vector<std::uint8_t> stream = pop(scheduler, 2);
    const std::uint32_t viewport = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(viewport), payloadOf(2 * bulkChunkPayloadBytes), TransferRank::Viewport) > 0);
    const std::vector<std::uint8_t> rest = pop(scheduler);
    stream.insert(stream.end(), rest.begin(), rest.end());
    CHECK((framesOf(stream).chunkTransfers == std::vector<std::uint32_t> { checkpoint, checkpoint, checkpoint, viewport, viewport }));
}

// The lane bound holds with history in the bulk lane: a receipt
// queued behind a backfill and a viewport transfer still leaves within one
// capped frame's transmission time.
TEST(aReceiptStillLeavesWithinOneFrameTimeBehindHistoryTransfers)
{
    LaneScheduler scheduler(quarterBulk);
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(scheduler.nextTransferID()), payloadOf(4 << 20), TransferRank::Backfill) > 0);
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(scheduler.nextTransferID()), payloadOf(1 << 20), TransferRank::Viewport) > 0);
    std::vector<std::uint8_t> receiptBody;
    encode(receiptBody, Receipt { .inputWritten = 3, .inputSettled = 3 });
    const std::size_t receiptFrameBytes = frameBytes(1, receiptBody.size());

    std::uint64_t now = 0;
    std::uint64_t linkFreeAt = 0;
    std::uint64_t worstWait = 0;
    std::optional<std::uint64_t> receiptQueuedAt;
    for (int framesSent = 0; framesSent < 400; ++framesSent) {
        std::vector<std::uint8_t> frame;
        if (!scheduler.popFrame(frame, BulkPacing::Admitted))
            break;
        const std::uint64_t start = std::max(now, linkFreeAt);
        linkFreeAt = start + transmitMicroseconds(frame.size());
        if (receiptQueuedAt && frame[0] == static_cast<std::uint8_t>(FrameKind::Receipt)) {
            worstWait = std::max(worstWait, linkFreeAt - *receiptQueuedAt);
            receiptQueuedAt.reset();
        }
        // A receipt every seventh frame, a third of the way into it.
        if (!receiptQueuedAt && framesSent % 7 == 3) {
            now = start + transmitMicroseconds(frame.size()) / 3;
            CHECK(scheduler.enqueue(FrameKind::Receipt, 1, receiptBody) > 0);
            receiptQueuedAt = now;
        }
        now = linkFreeAt;
    }
    const std::uint64_t oneFrameTime = transmitMicroseconds(frameBytes(0, maxFrameBodyBytes) + receiptFrameBytes);
    CHECK_MSG(worstWait > 0 && worstWait <= oneFrameTime, ("a receipt waited " + std::to_string(worstWait) + " us; one frame time is " + std::to_string(oneFrameTime) + " us").c_str());
}

// A history transfer has a cap of its own, below a checkpoint's: the
// reassembler refuses the chunk that takes one past 4 MiB.
TEST(aHistoryTransferPastFourMebibytesIsRefused)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t history = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(history), payloadOf(maxHistoryTransferBytes + 1), TransferRank::Backfill) > 0);
    const std::uint32_t checkpoint = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::CheckpointHead, 1, checkpointHeadBody(checkpoint), payloadOf(maxHistoryTransferBytes + 1)) > 0);
    const std::vector<std::uint8_t> stream = pop(scheduler);

    BulkReassembler reassembler;
    CHECK(reassembler.expect(history, maxHistoryTransferBytes));
    CHECK(reassembler.expect(checkpoint, 8 << 20));
    FrameDecoder decoder;
    decoder.append(stream);
    std::optional<ChunkOutcome> historyEnd;
    std::optional<ChunkOutcome> checkpointEnd;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind != FrameKind::BulkChunk)
            continue;
        const BulkChunk chunk = *decodeBulkChunk(frame->body);
        const ChunkOutcome outcome = reassembler.accept(chunk);
        if (chunk.transfer == history && outcome != ChunkOutcome::Pending && !historyEnd)
            historyEnd = outcome;
        if (chunk.transfer == checkpoint && outcome != ChunkOutcome::Pending)
            checkpointEnd = outcome;
    }
    CHECK(historyEnd == ChunkOutcome::Refused);
    CHECK(checkpointEnd == ChunkOutcome::Complete);
}

// A transfer the receiver abandons is fenced: its late chunks are discarded,
// not refused, until its last chunk or its abort releases the fence, and the
// fences are bounded, the oldest released first.
TEST(anAbandonedTransferDiscardsItsLateChunksUntilItEnds)
{
    LaneScheduler scheduler(quarterBulk);
    const std::uint32_t transfer = scheduler.nextTransferID();
    CHECK(scheduler.enqueueTransfer(FrameKind::HistoryHead, 1, historyHeadBody(transfer), payloadOf(3 * bulkChunkPayloadBytes), TransferRank::Backfill) > 0);
    FrameDecoder decoder;
    decoder.append(pop(scheduler));
    std::vector<std::vector<std::uint8_t>> bodies;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind == FrameKind::BulkChunk)
            bodies.emplace_back(frame->body.begin(), frame->body.end());
    }
    REQUIRE(bodies.size() == 3);

    BulkReassembler reassembler;
    CHECK(reassembler.expect(transfer, maxHistoryTransferBytes));
    CHECK(reassembler.accept(*decodeBulkChunk(bodies[0])) == ChunkOutcome::Pending);
    reassembler.abandon(transfer);
    CHECK(reassembler.isFenced(transfer));
    CHECK_EQ(reassembler.transferCount(), std::size_t { 0 });
    CHECK(reassembler.accept(*decodeBulkChunk(bodies[1])) == ChunkOutcome::Discarded);
    CHECK(reassembler.accept(*decodeBulkChunk(bodies[2])) == ChunkOutcome::Discarded);
    // The last chunk released the fence: the id names nothing now.
    CHECK(!reassembler.isFenced(transfer));
    CHECK(reassembler.accept(*decodeBulkChunk(bodies[1])) == ChunkOutcome::Refused);

    // An abort releases a fence too.
    CHECK(reassembler.expect(transfer + 100, maxHistoryTransferBytes));
    reassembler.abandon(transfer + 100);
    CHECK(reassembler.releaseFence(transfer + 100));
    CHECK(!reassembler.releaseFence(transfer + 100));

    // Past the bound the oldest fence goes first.
    for (std::uint32_t index = 0; index < maxFencedTransfers + 1; ++index) {
        CHECK(reassembler.expect(1000 + index, maxHistoryTransferBytes));
        reassembler.abandon(1000 + index);
    }
    CHECK(!reassembler.isFenced(1000));
    CHECK(reassembler.isFenced(1001));
    CHECK(reassembler.isFenced(1000 + maxFencedTransfers));
}

TEST(nativeDragFramesUseBulkPacingWithoutBlockingInteractiveFrames)
{
    LaneScheduler scheduler({ .bulkSharePermille = 250 });
    std::vector<std::uint8_t> data(3072, 7);
    const auto bulkBytes = scheduler.enqueue(FrameKind::NativeDragCommand, 7, data);
    REQUIRE(bulkBytes);
    CHECK_EQ(scheduler.queuedBulkBytes(), bulkBytes);
    CHECK_EQ(scheduler.nextBulkFrameBytes(), bulkBytes);
    CHECK(scheduler.enqueue(FrameKind::Update, 7, {}) > 0);
    std::vector<std::uint8_t> output;
    CHECK(scheduler.popFrame(output, BulkPacing::Held) == Lane::Interactive);
    CHECK(!scheduler.popFrame(output, BulkPacing::Held));
    CHECK(!scheduler.popFrame(output, BulkPacing::Admitted, 1));
    CHECK(scheduler.popFrame(output, BulkPacing::Admitted) == Lane::Bulk);
    CHECK(scheduler.isEmpty());
    CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    CHECK_EQ(scheduler.queuedBytes(), 0u);
    CHECK_EQ(scheduler.retainedPayloadBytes(), 0u);
}

TEST(withdrawingNativeDragFramesIsScopedToThePane)
{
    LaneScheduler scheduler({ .bulkSharePermille = 250 });
    CHECK(scheduler.enqueue(FrameKind::NativeDragCommand, 7, {}) > 0);
    const auto retained = scheduler.enqueue(FrameKind::NativeDragCommand, 9, {});
    REQUIRE(retained);
    CHECK(scheduler.withdraw(FrameKind::NativeDragCommand, 7) > 0);
    CHECK(!scheduler.isQueued(FrameKind::NativeDragCommand, 7));
    CHECK(scheduler.isQueued(FrameKind::NativeDragCommand, 9));
    CHECK_EQ(scheduler.queuedBulkBytes(), retained);
    scheduler.abandonAll();
    CHECK(scheduler.isEmpty());
    CHECK_EQ(scheduler.queuedBytes(), 0u);
    CHECK_EQ(scheduler.queuedBulkBytes(), 0u);
    CHECK_EQ(scheduler.retainedPayloadBytes(), 0u);
}

UBIQUITTY_TEST_MAIN()
