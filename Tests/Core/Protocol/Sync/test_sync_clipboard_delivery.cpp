// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "SyncLink.h"

#include <algorithm>
#include <functional>
#include <new>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

constexpr ID128 primary { 5, 6 };

class Recorder final : public ClientPaneDelegate {
public:
    std::vector<ClipboardBuffer> buffers;
    std::vector<std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> writes;
    std::function<void(const EffectClipboardBundle&)> received;
    std::function<bool(const ClipboardRead&)> requested;

    bool paneClipboardRequest(std::uint32_t, ID128, const ClipboardRead& request) override
    {
        return requested && requested(request);
    }

    void paneEffect(std::uint32_t, const EffectView&, const EffectBody& body, EffectDisposition disposition) override
    {
        const auto* write = std::get_if<ClipboardWriteEffect>(&body);
        REQUIRE(write);
        REQUIRE(disposition == EffectDisposition::Post);
        const auto bundle = decodeEffectClipboard(write->payload.inlineBytes, maxEffectClipboardWriteBytes);
        REQUIRE(bundle);
        buffers.push_back(write->buffer);
        auto& copied = writes.emplace_back();
        for (std::size_t index = 0; index < bundle->count; ++index) {
            const auto& item = bundle->items[index];
            copied.emplace_back(item.mime, std::vector<std::uint8_t>(item.data.begin(), item.data.end()));
        }
        if (received)
            received(*bundle);
    }
};

struct Link {
    Recorder recorder;
    ReplicaSet replicas;
    UbiquittyTest::OwnedPane owned;
    HostConnection host;
    ClientConnection client;
    std::uint32_t handle = 0;
    std::uint64_t now = 1;
    std::size_t bulkFrames = 0;

    explicit Link(ID128 recipient = primary)
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 31, 32 }, .attachmentID = { 33, 34 }, .resumeSecret = { 35, 36 }, .principal = "clipboard", .mayDrive = true, .verifiedKind = ClientKind::Human, .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "clipboard-test.sock", .buildFlags = 0, .hostPlatform = "fixture" } })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = recipient, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts() }, &recorder, nullptr, &replicas)
    {
        client.start();
        pump();
        pump();
        handle = host.bindPane(*owned.source);
        for (int index = 0; index < 12; ++index)
            pump();
    }

    std::vector<std::vector<std::uint8_t>> takeHostFrames()
    {
        now += 5'000;
        host.service(now);
        std::vector<std::vector<std::uint8_t>> frames;
        std::vector<std::uint8_t> frame;
        while (host.popStreamFrame(frame, now)) {
            frames.push_back(std::move(frame));
            frame.clear();
        }
        return frames;
    }

    void pump()
    {
        host.receiveStream(client.takeStreamOutput(), now);
        for (const auto& frame : takeHostFrames())
            client.receiveStream(frame, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    }

    void append(std::span<const std::uint8_t> data, ClipboardBuffer buffer = ClipboardBuffer::Clipboard)
    {
        EffectClipboardBundle bundle;
        bundle.items[0] = { "application/octet-stream", data };
        bundle.count = 1;
        REQUIRE(owned.source->appendClipboardWrite(buffer, bundle, primary));
    }

    std::vector<std::vector<std::uint8_t>> holdBulk()
    {
        std::vector<std::vector<std::uint8_t>> bulk;
        for (int index = 0; index < 80; ++index) {
            for (const auto& frame : takeHostFrames()) {
                if (frame[0] == std::to_underlying(FrameKind::BulkChunk)) {
                    ++bulkFrames;
                    EnvelopeError error = EnvelopeError::None;
                    const auto decoded = decodeDatagram(frame, error);
                    CHECK(decoded);
                    if (!decoded)
                        return {};
                    const auto chunk = decodeBulkChunk(decoded->body);
                    CHECK(chunk);
                    if (!chunk)
                        return {};
                    if (chunk->isLast)
                        bulk.push_back(frame);
                    else
                        client.receiveStream(frame, now);
                    CHECK(recorder.writes.empty());
                } else {
                    client.receiveStream(frame, now);
                }
            }
            client.tick(now);
            host.receiveStream(client.takeStreamOutput(), now);
        }
        return bulk;
    }
};

TEST(inlineClipboardPreservesAllRepresentationsAndAnEmptyClear)
{
    Link link;
    const std::array<std::uint8_t, 3> plain { 'a', 0, 'b' };
    const std::array<std::uint8_t, 4> html { '<', 'b', '/', '>' };
    EffectClipboardBundle bundle;
    bundle.count = 2;
    bundle.items[0] = { "text/plain", plain };
    bundle.items[1] = { "text/html", html };
    REQUIRE(link.owned.source->appendClipboardWrite(ClipboardBuffer::Primary, bundle, primary));
    for (int index = 0; index < 6; ++index)
        link.pump();
    REQUIRE(link.recorder.writes.size() == 1);
    REQUIRE(link.recorder.writes[0].size() == 2);
    CHECK(link.recorder.buffers[0] == ClipboardBuffer::Primary);
    CHECK(std::ranges::equal(link.recorder.writes[0][0].second, plain));
    CHECK(std::ranges::equal(link.recorder.writes[0][1].second, html));
    REQUIRE(link.owned.source->appendClipboardWrite(ClipboardBuffer::Clipboard, {}, primary));
    for (int index = 0; index < 6; ++index)
        link.pump();
    REQUIRE(link.recorder.writes.size() == 2);
    CHECK(link.recorder.writes[1].empty());
}

TEST(everyClipboardWriteAroundTheInlineBoundaryArrivesOnceOverAnOpenConnection)
{
    // The host carries a write inline only if its whole record fits one frame;
    // a write a few bytes larger takes a transfer. Neither side of that edge
    // may close the connection.
    constexpr std::size_t inlineLimit = maxFrameBodyBytes - effectRecordHeaderBytes - effectPacketHeaderBytes - 9;
    constexpr std::size_t calibration = 60'000;
    EffectClipboardBundle measured;
    const std::vector<std::uint8_t> sample(calibration, 0x5A);
    measured.items[0] = { "application/octet-stream", sample };
    measured.count = 1;
    const auto calibrated = effectClipboardEncodedBytes(measured, maxEffectClipboardWriteBytes);
    REQUIRE(calibrated);
    const std::size_t largestInline = inlineLimit - (*calibrated - calibration);
    for (std::size_t size = largestInline - 2; size <= largestInline + 6; ++size) {
        Link link;
        const std::vector<std::uint8_t> data(size, static_cast<std::uint8_t>(size));
        link.append(data);
        for (int index = 0; index < 80 && link.recorder.writes.empty(); ++index)
            link.pump();
        CHECK_MSG(link.client.state() != ConnectionState::Closed && link.host.state() != ConnectionState::Closed, "a clipboard write closed the connection");
        REQUIRE(link.recorder.writes.size() == 1);
        CHECK(link.recorder.writes[0][0].second == data);
    }
}

TEST(clipboardBulkDoesNotConsumeOrAcknowledgeBeforeItsFinalVerifiedBytes)
{
    Link link;
    const std::vector<std::uint8_t> original(3 * bulkChunkPayloadBytes, 0xA5);
    link.append(original);
    const auto bulk = link.holdBulk();
    REQUIRE(bulk.size() == 1);
    CHECK(link.bulkFrames >= 3);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 0u);
    link.client.receiveStream(bulk.back(), ++link.now);
    REQUIRE(link.recorder.writes.size() == 1);
    CHECK(link.recorder.writes[0][0].second == original);
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 1u);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
}

TEST(aDifferentRecipientReceivesNoClipboardMetadataOrData)
{
    Link link({ 88, 99 });
    const std::vector<std::uint8_t> secret(2 * bulkChunkPayloadBytes, 0xA5);
    link.append(secret);
    for (int index = 0; index < 12; ++index) {
        for (const auto& bytes : link.takeHostFrames()) {
            EnvelopeError error = EnvelopeError::None;
            const auto frame = decodeDatagram(bytes, error);
            REQUIRE(frame);
            CHECK(frame->kind != FrameKind::BulkChunk);
            if (frame->kind == FrameKind::Effect) {
                const auto packet = decodeEffectPacket(link.owned.source->paneID(), frame->body);
                REQUIRE(packet);
                CHECK(!packet->record);
            }
            link.client.receiveStream(bytes, link.now);
        }
    }
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 1u);
}

TEST(aClipboardHashFailureLeavesTheReceiptAndSinkUntouched)
{
    Link link;
    link.append(std::vector<std::uint8_t>(2 * bulkChunkPayloadBytes, 0x25));
    auto bulk = link.holdBulk();
    REQUIRE(!bulk.empty());
    bulk.back().back() ^= 1;
    for (const auto& frame : bulk)
        link.client.receiveStream(frame, ++link.now);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
}

TEST(aClipboardHeadCannotAuthorizeAnAcknowledgementForUnsentBulk)
{
    Link link;
    link.append(std::vector<std::uint8_t>(2 * bulkChunkPayloadBytes, 0x25));
    link.host.service(++link.now);
    bool sawHead = false;
    std::vector<std::uint8_t> bytes;
    while (link.host.popStreamFrame(bytes, link.now)) {
        EnvelopeError error = EnvelopeError::None;
        const auto frame = decodeDatagram(bytes, error);
        REQUIRE(frame);
        if (frame->kind == FrameKind::Effect) {
            const auto packet = decodeEffectPacket(link.owned.source->paneID(), frame->body);
            REQUIRE(packet && packet->record);
            const auto body = decodeEffectBody(packet->record->kind, packet->record->payload);
            REQUIRE(body);
            REQUIRE(std::get<ClipboardWriteEffect>(*body).payload.transfer != 0);
            sawHead = true;
            break;
        }
        bytes.clear();
    }
    REQUIRE(sawHead);
    std::vector<std::uint8_t> ack;
    encode(ack, Ack { .panes = { { .handle = link.handle, .generation = link.client.paneGeneration(link.handle), .effectsAcknowledged = 1 } } });
    std::vector<std::uint8_t> framed;
    REQUIRE(appendFrame(framed, FrameKind::Ack, 0, ack));
    link.host.receiveStream(framed, ++link.now);
    CHECK(link.host.state() == ConnectionState::Closed);
}

TEST(aClipboardAllocationRefusalDoesNotConsumeItsOperation)
{
    Link link;
    const std::vector<std::uint8_t> data(2 * bulkChunkPayloadBytes, 0x25);
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "application/octet-stream", data };
    const auto size = effectClipboardEncodedBytes(bundle, maxEffectClipboardWriteBytes);
    REQUIRE(size);
    link.append(data);
    const auto frames = link.takeHostFrames();
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(*size);
        for (const auto& bytes : frames)
            link.client.receiveStream(bytes, link.now);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 1);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
}

TEST(aSenderClipboardStagingRefusalQueuesNeitherHeadNorPayload)
{
    Link link;
    const std::vector<std::uint8_t> data(2 * bulkChunkPayloadBytes, 0x25);
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "application/octet-stream", data };
    const auto size = effectClipboardEncodedBytes(bundle, maxEffectClipboardWriteBytes);
    REQUIRE(size);
    link.append(data);
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(*size);
        for (const auto& bytes : link.takeHostFrames()) {
            EnvelopeError error = EnvelopeError::None;
            const auto frame = decodeDatagram(bytes, error);
            REQUIRE(frame);
            CHECK(frame->kind != FrameKind::Effect);
            CHECK(frame->kind != FrameKind::BulkChunk);
        }
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 1);
    CHECK(link.host.state() == ConnectionState::Closed);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    const auto retained = link.owned.source->effects().readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(retained && retained->size == 1);
    const auto original = decodeEffectClipboard(retained->records[0].data, maxEffectClipboardWriteBytes);
    REQUIRE(original && original->count == 1);
    CHECK(std::ranges::equal(original->items[0].data, data));
}

TEST(aClipboardPayloadRemainsOwnedDuringCallbackAbandon)
{
    Link link;
    const std::vector<std::uint8_t> original(2 * bulkChunkPayloadBytes, 0x9D);
    link.recorder.received = [&](const EffectClipboardBundle& bundle) {
        const auto held = link.client.inboundEffectBytes();
        CHECK(held >= original.size());
        link.client.abandon();
        CHECK_EQ(link.client.inboundEffectBytes(), held);
        CHECK(std::ranges::equal(bundle.items[0].data, original));
    };
    link.append(original);
    for (int index = 0; index < 12 && link.client.state() == ConnectionState::Attached; ++index)
        link.pump();
    REQUIRE(link.recorder.writes.size() == 1);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
}

TEST(unbindingAPendingClipboardRetiresItsOwnerWithoutAcknowledgement)
{
    Link link;
    link.append(std::vector<std::uint8_t>(2 * bulkChunkPayloadBytes, 0x25));
    const auto bulk = link.holdBulk();
    REQUIRE(!bulk.empty());
    CHECK(link.client.inboundEffectBytes() > 0);
    link.host.unbindPane(link.handle, 0);
    for (int index = 0; index < 4; ++index)
        link.pump();
    for (const auto& frame : bulk)
        link.client.receiveStream(frame, ++link.now);
    CHECK(link.client.state() == ConnectionState::Attached);
    CHECK(link.recorder.writes.empty());
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
}

TEST(typedClipboardReadPreservesItsExactOperationAndBinaryAnswer)
{
    Link link;
    std::size_t questions = 0;
    link.recorder.requested = [&](const ClipboardRead& request) {
        ++questions;
        CHECK_EQ(request.requestID, 71u);
        CHECK(request.buffer == ClipboardBuffer::Primary);
        CHECK(request.kind == EffectClipboardRequestKind::Item);
        CHECK(request.mime == "application/octet-stream");
        return true;
    };
    REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 71, .buffer = ClipboardBuffer::Primary, .kind = EffectClipboardRequestKind::Item, .mime = "application/octet-stream" }));
    link.pump();
    CHECK_EQ(questions, 1u);
    const std::array<std::uint8_t, 3> bytes { 9, 0, 255 };
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "text/plain", bytes };
    CHECK(!link.client.answerClipboard(71, EffectClipboardStatus::OK, bundle));
    bundle.items[0].mime = "application/octet-stream";
    REQUIRE(link.client.answerClipboard(71, EffectClipboardStatus::OK, bundle));
    CHECK(!link.client.answerClipboard(71, EffectClipboardStatus::OK, bundle));
    link.pump();
    std::size_t answers = 0;
    REQUIRE(link.host.drainClipboardReplies([&](std::uint32_t handle, const ClipboardRead& request, EffectClipboardStatus status, const EffectClipboardBundle& reply) {
        ++answers;
        CHECK_EQ(handle, link.handle);
        CHECK_EQ(request.requestID, 71u);
        CHECK(status == EffectClipboardStatus::OK);
        CHECK_EQ(reply.count, 1u);
        CHECK(std::ranges::equal(reply.items[0].data, bytes));
        return true;
    }));
    CHECK_EQ(answers, 1u);
    CHECK_EQ(link.host.inboundClipboardBytes(), 0u);
}

TEST(anUnconfiguredClipboardReaderAnswersRefusedWithoutExternalAccess)
{
    Link link;
    REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 72, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Types, .mime = {} }));
    link.pump();
    std::size_t answers = 0;
    REQUIRE(link.host.drainClipboardReplies([&](std::uint32_t, const ClipboardRead&, EffectClipboardStatus status, const EffectClipboardBundle& reply) {
        ++answers;
        CHECK(status == EffectClipboardStatus::Refused);
        CHECK_EQ(reply.count, 0u);
        return true;
    }));
    CHECK_EQ(answers, 1u);
}

TEST(aClipboardReadBulkAnswerWaitsForItsExactFinalAndRetainsDeliveryOwnership)
{
    Link link;
    link.recorder.requested = [](const ClipboardRead&) { return true; };
    REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 73, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Item, .mime = "image/png" }));
    link.pump();
    const std::vector<std::uint8_t> bytes(200'000, 173);
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "image/png", bytes };
    REQUIRE(link.client.answerClipboard(73, EffectClipboardStatus::OK, bundle));
    std::size_t answers = 0;
    for (int index = 0; index < 64 && !answers; ++index) {
        link.host.receiveStream(link.client.takeStreamOutput(), ++link.now);
        REQUIRE(link.host.state() == ConnectionState::Attached);
        REQUIRE(link.host.drainClipboardReplies([&](std::uint32_t, const ClipboardRead& request, EffectClipboardStatus status, const EffectClipboardBundle& reply) {
            ++answers;
            CHECK_EQ(request.requestID, 73u);
            CHECK(status == EffectClipboardStatus::OK);
            CHECK(std::ranges::equal(reply.items[0].data, bytes));
            const auto charged = link.host.inboundClipboardBytes();
            CHECK(charged > bytes.size());
            link.host.discardInboundClipboard();
            CHECK_EQ(link.host.inboundClipboardBytes(), charged);
            CHECK(std::ranges::equal(reply.items[0].data, bytes));
            return true;
        }));
        if (index == 0)
            CHECK_EQ(answers, 0u);
    }
    CHECK_EQ(answers, 1u);
    CHECK_EQ(link.host.inboundClipboardBytes(), 0u);
}

TEST(clipboardReadAuthorityIsRecheckedBeforeAllocationAndEveryBulkChunk)
{
    for (const bool shouldWithdrawAfterHead : { false, true }) {
        Link link;
        bool isAuthorized = true;
        link.host.setClipboardReplyAdmission([&](std::uint32_t handle, std::uint32_t request) {
            return isAuthorized && handle == link.handle && request == 74;
        });
        link.recorder.requested = [](const ClipboardRead&) { return true; };
        REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 74, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Item, .mime = "image/png" }));
        link.pump();
        const std::vector<std::uint8_t> bytes(200'000, 41);
        EffectClipboardBundle bundle;
        bundle.count = 1;
        bundle.items[0] = { "image/png", bytes };
        REQUIRE(link.client.answerClipboard(74, EffectClipboardStatus::OK, bundle));
        if (shouldWithdrawAfterHead) {
            link.host.receiveStream(link.client.takeStreamOutput(), ++link.now);
            REQUIRE(link.host.state() == ConnectionState::Attached);
            CHECK(link.host.inboundClipboardBytes() > bytes.size());
        }
        isAuthorized = false;
        link.host.receiveStream(link.client.takeStreamOutput(), ++link.now);
        CHECK(link.host.state() == ConnectionState::Closed);
        CHECK_EQ(link.host.inboundClipboardBytes(), 0u);
    }
}

TEST(clipboardReadCancellationAndUnbindRetireTheExactRequest)
{
    for (const bool shouldUnbind : { false, true }) {
        Link link;
        link.recorder.requested = [](const ClipboardRead&) { return true; };
        REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 75, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Types, .mime = {} }));
        link.pump();
        REQUIRE(link.host.inboundClipboardBytes() > 0);
        if (shouldUnbind)
            link.host.unbindPane(link.handle, 0);
        else
            link.host.cancelClipboardRequest(75);
        CHECK_EQ(link.host.inboundClipboardBytes(), 0u);
        if (shouldUnbind) {
            link.pump();
            CHECK(!link.client.answerClipboard(75, EffectClipboardStatus::Refused));
        } else {
            REQUIRE(link.client.answerClipboard(75, EffectClipboardStatus::Refused));
            link.host.receiveStream(link.client.takeStreamOutput(), ++link.now);
            CHECK(link.host.state() == ConnectionState::Closed);
        }
    }
}

TEST(unansweredClipboardQuestionsStayBoundedAfterHostCancellation)
{
    Link link;
    std::size_t questions = 0;
    link.recorder.requested = [&](const ClipboardRead&) { ++questions; return true; };
    // There is no question-cancel frame. Host cancellation retires authority,
    // not an asynchronous frontend prompt: an unresponsive frontend closes at
    // its fixed pending cap rather than retaining unbounded request owners.
    for (std::uint32_t request = 1; request <= maxPendingClipboardReads; ++request) {
        REQUIRE(link.host.requestClipboard(link.handle, { .requestID = request, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Types, .mime = {} }));
        link.pump();
        REQUIRE(link.client.state() == ConnectionState::Attached);
        link.host.cancelClipboardRequest(request);
    }
    CHECK_EQ(questions, maxPendingClipboardReads);
    REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 100, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Types, .mime = {} }));
    link.pump();
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK_EQ(questions, maxPendingClipboardReads);
}

TEST(clipboardReadSenderAllocationRefusalPreservesThePendingOperationForRetry)
{
    Link link;
    link.recorder.requested = [](const ClipboardRead&) { return true; };
    REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 76, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Item, .mime = "image/png" }));
    link.pump();
    const std::vector<std::uint8_t> bytes(200'000, 71);
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "image/png", bytes };
    const auto size = effectClipboardEncodedBytes(bundle, maxEffectClipboardReadBytes);
    REQUIRE(size);
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(*size);
        CHECK(!link.client.answerClipboard(76, EffectClipboardStatus::OK, bundle));
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 1);
    CHECK(link.client.takeStreamOutput().empty());
    REQUIRE(link.client.answerClipboard(76, EffectClipboardStatus::OK, bundle));
    CHECK(link.client.nextTickMicroseconds() == 0);
}

TEST(clipboardReadReceiverAdmissionPrecedesAllocationAndHashMismatchNeverDelivers)
{
    for (const bool shouldRefuseCapacity : { false, true }) {
        Link link;
        link.recorder.requested = [](const ClipboardRead&) { return true; };
        REQUIRE(link.host.requestClipboard(link.handle, { .requestID = 77, .buffer = ClipboardBuffer::Clipboard, .kind = EffectClipboardRequestKind::Item, .mime = "image/png" }));
        link.pump();
        const std::vector<std::uint8_t> bytes(200'000, 19);
        EffectClipboardBundle bundle;
        bundle.count = 1;
        bundle.items[0] = { "image/png", bytes };
        const auto size = effectClipboardEncodedBytes(bundle, maxEffectClipboardReadBytes);
        REQUIRE(size);
        REQUIRE(link.client.answerClipboard(77, EffectClipboardStatus::OK, bundle));
        if (shouldRefuseCapacity)
            link.host.setInboundAssemblyAdmission([](std::size_t) { return false; });
        std::size_t answers = 0;
        const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        for (int index = 0; index < 64 && link.host.state() == ConnectionState::Attached; ++index) {
            auto output = link.client.takeStreamOutput();
            REQUIRE(!output.empty());
            if (index > 0 && !shouldRefuseCapacity) {
                EnvelopeError error = EnvelopeError::None;
                const auto frame = decodeDatagram(output, error);
                REQUIRE(frame && frame->kind == FrameKind::BulkChunk);
                auto chunk = decodeBulkChunk(frame->body);
                REQUIRE(chunk);
                if (chunk->isLast) {
                    chunk->sha256[0] ^= 1;
                    std::vector<std::uint8_t> body;
                    encode(body, *chunk);
                    output.clear();
                    REQUIRE(appendFrame(output, FrameKind::BulkChunk, 0, body));
                }
            }
            {
                UbiquittyTest::ScopedNothrowAllocationRefusal refusal(shouldRefuseCapacity ? *size : 0);
                link.host.receiveStream(output, ++link.now);
            }
            (void)link.host.drainClipboardReplies([&](std::uint32_t, const ClipboardRead&, EffectClipboardStatus, const EffectClipboardBundle&) {
                ++answers;
                return true;
            });
        }
        CHECK(link.host.state() == ConnectionState::Closed);
        CHECK_EQ(answers, 0u);
        CHECK_EQ(link.host.inboundClipboardBytes(), 0u);
        CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before);
    }
}

} // namespace

UBIQUITTY_TEST_MAIN()
