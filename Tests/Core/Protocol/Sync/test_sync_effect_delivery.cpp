// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/Envelope.h"

#include <algorithm>
#include <new>
#include <vector>

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

constexpr ID128 pane { 1, 2 };
constexpr ID128 otherPane { 3, 4 };
constexpr ID128 primary { 5, 6 };
constexpr EffectScreenBoundary visible { .view = ViewState::Live, .appliedGeneration = 8 };

class EffectRecorder final : public ClientPaneDelegate {
public:
    struct Received {
        std::uint64_t sequence;
        EffectKind kind;
        EffectDisposition disposition;
        std::int32_t exitStatus;
        std::string commandLine;
    };
    std::vector<Received> received;
    std::function<void()> didReceive;
    std::function<void(std::uint32_t, ID128)> didUnbind;

    void paneEffect(std::uint32_t, const EffectView& effect, const EffectBody& body, EffectDisposition disposition) override
    {
        const auto* command = std::get_if<CommandFinishedEffect>(&body);
        received.push_back({ effect.sequence, effect.kind, disposition, command ? command->exitStatus : 0, command ? std::string(command->commandLine) : std::string {} });
        if (didReceive)
            didReceive();
    }

    void paneUnbound(std::uint32_t handle, ID128 paneID) override
    {
        if (didUnbind)
            didUnbind(handle, paneID);
    }
};

struct EffectLink {
    HostConnection host;
    ClientConnection client;
    std::uint32_t handle = 0;
    std::uint64_t now = 1;

    EffectLink(EffectRecorder& recorder, ClientKind kind, ReplicaSet* replicas = nullptr)
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 31, 32 }, .attachmentID = { 33, 34 }, .resumeSecret = { 35, 36 }, .principal = "effects", .mayDrive = true, .verifiedKind = ClientKind::Human, .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "effects-test.sock", .buildFlags = 0, .hostPlatform = "fixture" } })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = primary, .kind = kind, .facts = UbiquittyTest::syncDefaultFacts() }, &recorder, nullptr, replicas)
    {
        client.start();
        pump();
        pump();
    }

    void bind(PaneSource& source)
    {
        handle = host.bindPane(source);
        for (int index = 0; index < 12; ++index)
            pump();
    }

    void pump()
    {
        now += 5'000;
        host.service(now);
        std::vector<std::uint8_t> down;
        while (host.popStreamFrame(down, now)) {
        }
        client.receiveStream(down, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    }
};

class EffectSecrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 0xEFEC7 }; }

private:
    std::uint64_t m_next = 0;
};

struct RetainedEffectLink {
    HostConnection host;
    ClientConnection client;
    std::uint64_t now = 1;

    RetainedEffectLink(PaneSource& source, AttachmentTable& table, ReplicaSet& replicas, EffectRecorder& recorder,
        std::uint64_t connection, std::optional<ResumeRequest> resume = {})
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "effects-test.sock", .buildFlags = 0, .hostPlatform = "fixture" } })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = primary, .kind = ClientKind::Agent, .facts = UbiquittyTest::syncDefaultFacts(), .resume = std::move(resume) }, &recorder, nullptr, &replicas)
    {
        host.serveFromTable(table, { .principal = "effects", .mayDrive = true, .verifiedKind = ClientKind::Agent }, connection,
            [&source](ID128 identity) { return identity == source.paneID() ? &source : nullptr; });
        client.start();
        pump();
        pump();
    }

    void pump()
    {
        now += 5'000;
        host.service(now);
        std::vector<std::uint8_t> down;
        while (host.popStreamFrame(down, now)) {
        }
        client.receiveStream(down, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    }
};

EffectView bell(std::uint64_t sequence)
{
    return { .paneID = pane, .generation = 8, .sequence = sequence, .primary = primary, .kind = EffectKind::Bell, .count = 1, .target = 0, .payload = {} };
}

EffectPacket packet(std::uint64_t sequence, std::uint64_t through, std::uint16_t remaining)
{
    return { .generation = 8, .throughSequence = through, .remainingRecords = remaining, .record = bell(sequence) };
}

std::expected<EffectCodecBytes, EffectCodecError> clipboardRecord(std::size_t dataBytes)
{
    const std::vector<std::uint8_t> data(dataBytes, 'x');
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "text/plain", data };
    auto encodedBundle = encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes);
    if (!encodedBundle)
        return std::unexpected(encodedBundle.error());
    auto body = encodeEffectBody(ClipboardWriteEffect { ClipboardBuffer::Clipboard,
        { .transfer = 0, .encodedBytes = static_cast<std::uint32_t>(encodedBundle->size), .inlineBytes = encodedBundle->view() } });
    if (!body)
        return std::unexpected(body.error());
    auto effect = bell(1);
    effect.kind = EffectKind::ClipboardWrite;
    effect.target = std::to_underlying(ClipboardBuffer::Clipboard);
    effect.payload = body->view();
    return encodeEffect(effect);
}

} // namespace

TEST(packetFramingPreservesTheImmutableBatchAndRejectsPartialBodies)
{
    const auto record = encodeEffect(bell(2));
    REQUIRE(record);
    const auto encoded = encodeEffectPacket(pane, 8, 7, 1, record->view());
    REQUIRE(encoded);
    CHECK(encoded->size == effectPacketHeaderBytes + record->size);
    const auto decoded = decodeEffectPacket(pane, encoded->view());
    REQUIRE(decoded);
    CHECK(decoded->generation == 8);
    CHECK(decoded->throughSequence == 7);
    CHECK(decoded->remainingRecords == 1);
    REQUIRE(decoded->record);
    CHECK(decoded->record->paneID == pane);
    CHECK(decoded->record->sequence == 2);
    CHECK(decoded->record->primary == primary);
    for (std::size_t size = 0; size < encoded->size; ++size)
        CHECK(!decodeEffectPacket(pane, encoded->view().first(size)));
    auto bytes = std::vector(encoded->view().begin(), encoded->view().end());
    bytes.push_back(0);
    CHECK(!decodeEffectPacket(pane, bytes));
    CHECK(!decodeEffectPacket({}, encoded->view()));
    CHECK(!encodeEffectPacket(pane, 7, 7, 1, record->view()));
    CHECK(!encodeEffectPacket(pane, 8, 2, 1, record->view()));
    CHECK(!encodeEffectPacket(pane, 8, 7, 256, record->view()));
    CHECK(!encodeEffectPacket(pane, 8, 0, 0, record->view()));
}

TEST(emptyCompletionRetiresASkippedTailWithoutInventingAnEffect)
{
    const auto encoded = encodeEffectPacket(pane, 8, 10, 0, {});
    REQUIRE(encoded);
    CHECK(encoded->size == effectPacketHeaderBytes);
    const auto decoded = decodeEffectPacket(pane, encoded->view());
    REQUIRE(decoded);
    CHECK(!decoded->record);
    EffectReceiveCursor cursor(pane);
    const auto received = cursor.accept(*decoded, visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::CursorOnly);
    CHECK(cursor.state().acknowledged == 10);
    CHECK(cursor.state().consumed == 10);
    CHECK(!cursor.hasPendingBatch());
    CHECK(!encodeEffectPacket(pane, 8, 10, 1, {}));
    CHECK(!encodeEffectPacket(pane, 8, 0, 0, {}));
}

TEST(anEmptyCompletionCannotSatisfyAnUnappliedScreenDependency)
{
    const EffectPacket empty { .generation = 8, .throughSequence = 10, .remainingRecords = 0, .record = std::nullopt };
    EffectReceiveCursor cursor(pane);
    CHECK(!cursor.accept(empty, { .view = ViewState::Live, .appliedGeneration = 7 }));
    CHECK(cursor.state().acknowledged == 0);
    CHECK(cursor.state().consumed == 0);
    CHECK(cursor.accept(empty, visible));
    for (const ViewState view : { ViewState::Dormant, ViewState::RecordsOnly }) {
        EffectReceiveCursor hidden(pane);
        CHECK(hidden.accept(empty, { .view = view, .appliedGeneration = 0 }));
        CHECK(hidden.state().acknowledged == 10);
    }
    EffectReceiveCursor invalidView(pane);
    CHECK(!invalidView.accept(empty, { .view = static_cast<ViewState>(255), .appliedGeneration = 8 }));
}

TEST(onlyTheFinalRecordMakesTheWholeBatchAcknowledgable)
{
    EffectReceiveCursor cursor(pane);
    auto received = cursor.accept(packet(2, 5, 1), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::Record);
    CHECK(cursor.state().consumed == 2);
    CHECK(cursor.state().acknowledged == 0);
    CHECK(cursor.hasPendingBatch());
    received = cursor.accept(packet(4, 5, 0), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::Record);
    CHECK(cursor.state().acknowledged == 5);
    CHECK(cursor.state().consumed == 5);
    CHECK(!cursor.hasPendingBatch());
}

TEST(invalidBatchContinuationCannotChangeEitherCursor)
{
    EffectReceiveCursor cursor(pane);
    REQUIRE(cursor.accept(packet(2, 8, 2), visible));
    auto changed = packet(4, 8, 1);
    changed.throughSequence = 9;
    CHECK(!cursor.accept(changed, visible));
    changed = packet(4, 8, 1);
    changed.generation = 9;
    CHECK(!cursor.accept(changed, { .view = ViewState::Live, .appliedGeneration = 9 }));
    CHECK(!cursor.accept(packet(4, 8, 0), visible));
    CHECK(!cursor.accept(packet(4, 8, 2), visible));
    CHECK(!cursor.accept(packet(2, 8, 1), visible));
    CHECK(!cursor.accept(packet(1, 8, 1), visible));
    changed = packet(4, 8, 1);
    changed.record->paneID = otherPane;
    CHECK(!cursor.accept(changed, visible));
    const EffectPacket empty { .generation = 8, .throughSequence = 8, .remainingRecords = 0, .record = std::nullopt };
    CHECK(!cursor.accept(empty, visible));
    CHECK(cursor.state().acknowledged == 0);
    CHECK(cursor.state().consumed == 2);
    REQUIRE(cursor.accept(packet(4, 8, 1), visible));
    REQUIRE(cursor.accept(packet(8, 8, 0), visible));
    CHECK(cursor.state().acknowledged == 8);
}

TEST(aReconnectionReplaysTheBatchWithoutRepeatingConsumedCallbacks)
{
    EffectReceiveCursor original(pane);
    REQUIRE(original.accept(packet(2, 8, 2), visible));
    REQUIRE(original.accept(packet(4, 8, 1), visible));
    CHECK(original.state().acknowledged == 0);
    EffectReceiveCursor resumed(pane);
    REQUIRE(resumed.restore(original.state()));
    auto received = resumed.accept(packet(2, 8, 2), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::CursorOnly);
    received = resumed.accept(packet(4, 8, 1), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::CursorOnly);
    received = resumed.accept(packet(8, 8, 0), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::Record);
    CHECK(resumed.state().acknowledged == 8);
    CHECK(resumed.state().consumed == 8);
    CHECK(!resumed.restore({ 0, 0 }));
}

TEST(receiptRestoreAndBackwardBatchesFailWithoutMutation)
{
    EffectReceiveCursor cursor(pane);
    CHECK(!cursor.restore({ 5, 4 }));
    CHECK(cursor.state().acknowledged == 0);
    REQUIRE(cursor.restore({ 5, 8 }));
    CHECK(!cursor.accept(packet(6, 7, 0), visible));
    CHECK(!cursor.accept(packet(4, 4, 0), visible));
    CHECK(cursor.state().acknowledged == 5);
    CHECK(cursor.state().consumed == 8);
    const auto received = cursor.accept(packet(8, 8, 0), visible);
    REQUIRE(received);
    CHECK(*received == EffectReception::CursorOnly);
    CHECK(cursor.state().acknowledged == 8);
    EffectReceiveCursor unbound({});
    CHECK(!unbound.restore({ 0, 0 }));
    CHECK(!unbound.accept(packet(1, 1, 0), visible));
}

TEST(theLargestDeclaredBatchContainsExactly256Records)
{
    EffectReceiveCursor cursor(pane);
    for (std::uint64_t sequence = 1; sequence <= maxEffectRecords; ++sequence) {
        const auto received = cursor.accept(packet(sequence, maxEffectRecords,
                                                static_cast<std::uint16_t>(maxEffectRecords - sequence)),
            visible);
        REQUIRE(received);
        CHECK(*received == EffectReception::Record);
        CHECK(cursor.state().acknowledged == (sequence == maxEffectRecords ? maxEffectRecords : 0));
    }
    CHECK(!cursor.hasPendingBatch());
    CHECK(!cursor.accept(packet(257, 513, 256), visible));
    CHECK(cursor.state().acknowledged == maxEffectRecords);
}

TEST(packetSizeIncludesItsHeaderAndNeverTruncatesALegalClipboard)
{
    constexpr std::size_t initialDataBytes = 64000;
    const auto initial = clipboardRecord(initialDataBytes);
    REQUIRE(initial);
    const std::size_t fittingDataBytes = initialDataBytes + maxFrameBodyBytes - effectPacketHeaderBytes - initial->size;
    const auto fitting = clipboardRecord(fittingDataBytes);
    REQUIRE(fitting);
    const auto encoded = encodeEffectPacket(pane, 8, 1, 0, fitting->view());
    REQUIRE(encoded);
    CHECK(encoded->size == maxFrameBodyBytes);
    CHECK(decodeEffectPacket(pane, encoded->view()));
    const auto tooLarge = clipboardRecord(fittingDataBytes + 1);
    REQUIRE(tooLarge);
    const auto refused = encodeEffectPacket(pane, 8, 1, 0, tooLarge->view());
    REQUIRE(!refused);
    CHECK(refused.error() == EffectCodecError::TooLarge);
}

TEST(packetValidationPrecedesAllocationAndRefusalOwnsNothing)
{
    const auto record = encodeEffect(bell(1));
    REQUIRE(record);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal failure;
    CHECK(!encodeEffectPacket(pane, 8, 0, 0, record->view()));
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    const auto refused = encodeEffectPacket(pane, 8, 1, 0, record->view());
    REQUIRE(!refused);
    CHECK(refused.error() == EffectCodecError::NoMemory);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts + 1);
}

TEST(liveConnectionDeliversEveryCommandAndItsTypedPresentationOnce)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Human, &replicas);
    link.bind(*owned.source);
    REQUIRE(link.handle);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 7, .durationMicroseconds = 41, .currentDirectory = "/tmp", .commandLine = "first" }, primary));
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 0, .durationMicroseconds = 42, .currentDirectory = "/tmp", .commandLine = "second" }, primary));
    for (int index = 0; index < 10; ++index)
        link.pump();
    REQUIRE(recorder.received.size() == 3);
    CHECK(recorder.received[0].kind == EffectKind::Bell);
    CHECK(recorder.received[0].disposition == EffectDisposition::Post);
    CHECK_EQ(recorder.received[1].exitStatus, 7);
    CHECK(recorder.received[1].commandLine == "first");
    CHECK_EQ(recorder.received[2].exitStatus, 0);
    CHECK(recorder.received[2].commandLine == "second");
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).acknowledged, 3u);
    CHECK_EQ(owned.source->effects().retiredThrough(), 3u);
    CHECK(link.host.state() == ConnectionState::Attached);
    CHECK(link.client.state() == ConnectionState::Attached);
}

TEST(recordsOnlyAgentsReceiveEveryCommandWithoutAScreenAcknowledgement)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    EffectLink link(recorder, ClientKind::Agent);
    link.bind(*owned.source);
    REQUIRE(link.handle);
    CHECK_EQ(link.host.counters().checkpointsSent, 0u);
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 13, .commandLine = "failed" }, primary));
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 0, .commandLine = "passed" }, primary));
    for (int index = 0; index < 8; ++index)
        link.pump();
    REQUIRE(recorder.received.size() == 2);
    CHECK_EQ(recorder.received[0].exitStatus, 13);
    CHECK_EQ(recorder.received[1].exitStatus, 0);
    CHECK(recorder.received[0].disposition == EffectDisposition::Ignore);
    CHECK_EQ(link.host.counters().checkpointsSent, 0u);
    CHECK_EQ(owned.source->effects().retiredThrough(), 2u);
}

TEST(clipboardProducerRetainsTheWholeMaximumBundleWithoutAPayloadCopy)
{
    UbiquittyTest::OwnedPane owned;
    EffectClipboardBundle bundle;
    std::array<std::string, maxEffectClipboardItems> names;
    std::vector<std::uint8_t> data(maxEffectClipboardWriteBytes, 0xA5);
    bundle.count = bundle.items.size();
    for (std::size_t index = 0; index < names.size(); ++index) {
        names[index] = std::string(maxEffectClipboardMIMEBytes, 'a');
        names[index][0] = static_cast<char>('A' + index);
        bundle.items[index] = { names[index], index ? std::span<const std::uint8_t> {} : std::span<const std::uint8_t>(data) };
    }
    const auto measured = effectClipboardEncodedBytes(bundle, maxEffectClipboardWriteBytes);
    REQUIRE(measured);
    REQUIRE(*measured > 2 * maxEffectClipboardWriteBytes);
    REQUIRE(owned.source->appendClipboardWrite(ClipboardBuffer::Clipboard, bundle, primary));
    const auto batch = owned.source->effects().readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(batch && batch->size == 1);
    const auto& effect = batch->records[0];
    CHECK(effect.primary == primary);
    CHECK(effect.kind == EffectKind::ClipboardWrite);
    CHECK_EQ(effect.data.size(), *measured);
    const auto decoded = decodeEffectClipboard(effect.data, maxEffectClipboardWriteBytes);
    REQUIRE(decoded);
    CHECK_EQ(decoded->count, maxEffectClipboardItems);
    CHECK(std::ranges::equal(decoded->items[0].data, data));
    CHECK(decoded->items.back().mime == names.back());
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    const auto refused = owned.source->appendClipboardWrite(ClipboardBuffer::Clipboard, bundle, primary);
    CHECK(!refused && refused.error() == EffectRefusal::OverLimit);
    // Only the fixed nine-byte descriptor is allocated before the retained
    // budget refuses another complete input-sized owner.
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), before + 1);
    CHECK_EQ(owned.source->effects().newestSequence(), 1u);
    CHECK(!owned.source->appendClipboardWrite(ClipboardBuffer::Clipboard, bundle, {}));
}

TEST(effectEncodingPreemptsAnImageBeforeAllocatingItsRecordAndPacket)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    EffectLink link(recorder, ClientKind::Human);
    link.bind(*owned.source);
    Ubiquitty::processBytes(&owned.terminal, "\033_Ga=T,f=32,s=1,v=1,i=7,q=2,C=1;/////w==\033\\");
    owned.source->publish(FrameClass::Structural, link.now);
    for (int pass = 0; pass < 12; ++pass)
        link.pump();
    auto connection = std::make_shared<BulkSourceBudget>(maxCatalogMessageBytes);
    auto aggregate = std::make_shared<BulkSourceBudget>(4 * maxCatalogMessageBytes);
    std::size_t other = 0;
    std::size_t preemptions = 0;
    link.host.setImageSourceAdmission([&] {
        const auto retained = other + link.host.retainedOutboundPayloadBytes();
        return BulkSourceAdmission { .connection = connection, .aggregate = aggregate,
            .otherConnectionBytes = retained, .otherAggregateBytes = retained }; }, [&] {
        ++preemptions;
        link.host.preemptImageTransfers(); });
    link.host.service(link.now += 16'667);
    REQUIRE(link.host.counters().imageTransfersSent == 1);
    REQUIRE(connection->heldBytes() > 0);
    other = maxCatalogMessageBytes - connection->heldBytes() - link.host.retainedOutboundPayloadBytes();
    REQUIRE(owned.source->appendEffect(NotificationEffect { "finished", "result", "job", NotificationOccasion::Unfocused }, primary));
    link.host.service(++link.now);
    CHECK_EQ(preemptions, 1u);
    CHECK_EQ(connection->heldBytes(), 0u);
    CHECK(link.host.state() == ConnectionState::Attached);
    other = 0;
    for (int pass = 0; pass < 20; ++pass)
        link.pump();
    REQUIRE(recorder.received.size() == 1);
    CHECK(recorder.received.front().kind == EffectKind::Notification);
    CHECK(recorder.received.front().disposition == EffectDisposition::Post);
    CHECK(link.client.state() == ConnectionState::Attached);
}

TEST(anAcknowledgementCannotRetireAnEffectPacketStillInTheScheduler)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    EffectLink link(recorder, ClientKind::Agent);
    link.bind(*owned.source);
    REQUIRE(link.handle);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    link.host.service(link.now + 1);
    Ack forged;
    forged.panes.push_back({ .handle = link.handle, .effectsAcknowledged = 1 });
    std::vector<std::uint8_t> body;
    encode(body, forged);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Ack, 0, body));
    link.host.receiveStream(frame, link.now + 2);
    CHECK(link.host.state() == ConnectionState::Closed);
    CHECK_EQ(owned.source->effects().retiredThrough(), 0u);
    CHECK_EQ(owned.source->effects().size(), 1u);
    CHECK(recorder.received.empty());
}

TEST(aFreshReaderAdvancesPastOldEffectsWithoutPresentingThem)
{
    UbiquittyTest::OwnedPane owned;
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    REQUIRE(owned.source->appendEffect(NotificationEffect { .title = "old", .body = "not replayed" }, primary));
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Agent, &replicas);
    link.bind(*owned.source);
    REQUIRE(link.handle);
    CHECK(recorder.received.empty());
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).acknowledged, 2u);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    for (int index = 0; index < 5; ++index)
        link.pump();
    REQUIRE(recorder.received.size() == 1);
    CHECK_EQ(recorder.received[0].sequence, 3u);
}

// An agent binds a pane for its records and then shows it to read the screen.
// The host serves the bind under the view it bound the pane in: a fresh
// reader's completion, naming the pane's current generation, can be on the
// wire when the client shows the pane, and it arrives before any picture. It
// waits for the picture the shown view asks for rather than being refused.
TEST(anEffectSentBeforeAPaneWasShownWaitsForItsPicture)
{
    UbiquittyTest::OwnedPane owned;
    UbiquittyTest::feed(owned.terminal, "build\r\n$ ");
    owned.source->publish(FrameClass::Settled, 2);
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 5, .commandLine = "before" }, primary));
    EffectRecorder recorder;
    EffectLink link(recorder, ClientKind::Agent);
    link.handle = link.host.bindPane(*owned.source);
    REQUIRE(link.handle);
    const auto step = [&](bool showsBeforeDelivery) {
        link.now += 5'000;
        link.host.service(link.now);
        std::vector<std::uint8_t> down;
        while (link.host.popStreamFrame(down, link.now)) {
        }
        if (showsBeforeDelivery)
            link.client.setView(link.handle, ViewState::Live);
        link.client.receiveStream(down, link.now);
        link.client.tick(link.now);
        link.host.receiveStream(link.client.takeStreamOutput(), link.now);
    };
    // The bind reaches the client, and its acknowledgement the host.
    step(false);
    REQUIRE(link.client.paneView(link.handle) == ViewState::RecordsOnly);
    // What the host sends next was chosen for a pane it still holds as
    // records only; the client shows the pane before it arrives.
    step(true);
    CHECK_MSG(link.client.state() == ConnectionState::Attached, "the client refused an effect the host sent before it learned the pane is shown");
    for (int index = 0; index < 12; ++index)
        link.pump();
    CHECK(recorder.received.empty());
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 0, .commandLine = "after" }, primary));
    for (int index = 0; index < 10; ++index)
        link.pump();
    REQUIRE(recorder.received.size() == 1);
    CHECK(recorder.received[0].commandLine == "after");
    CHECK(link.client.state() == ConnectionState::Attached);
    CHECK(link.host.state() == ConnectionState::Attached);
}

TEST(effectReceiptsSurvivePictureReplacementAndAreStoredBeforeCallbackAbandon)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Agent, &replicas);
    link.bind(*owned.source);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    recorder.didReceive = [&] { link.client.abandon(); };
    link.pump();
    REQUIRE(recorder.received.size() == 1);
    const EffectReceiptState receipt = replicas.effectReceipt(owned.source->paneID());
    CHECK_EQ(receipt.consumed, 1u);
    CHECK_EQ(receipt.acknowledged, 0u);
    replicas.forget(owned.source->paneID());
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 1u);
    replicas.noteHostInstance({ 99, 100 }, std::nullopt);
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 0u);
}

TEST(definitiveUnbindRetiresOnlyItsReceiptBeforeTheCallbackCanAbandon)
{
    UbiquittyTest::OwnedPane owned;
    UbiquittyTest::OwnedPane neighbor;
    neighbor.source = std::make_unique<PaneSource>(neighbor.terminal, otherPane);
    neighbor.source->publish(FrameClass::Structural, 1);
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Agent, &replicas);
    link.bind(*owned.source);
    const auto handle = link.handle;
    REQUIRE(handle);
    link.bind(*neighbor.source);
    REQUIRE(link.handle);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    REQUIRE(neighbor.source->appendEffect(BellEffect {}, primary));
    for (int index = 0; index < 6; ++index)
        link.pump();
    REQUIRE(replicas.effectReceipt(owned.source->paneID()).consumed == 1);
    REQUIRE(replicas.effectReceipt(otherPane).consumed == 1);

    // Removing a listing is not removing the pane's bound attachment.
    const auto listed = link.host.listPane(*owned.source, ListedDetail::WithoutArguments);
    REQUIRE(listed);
    link.pump();
    link.host.unlistPane(listed);
    link.pump();
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 1u);

    bool wasUnbound = false;
    recorder.didUnbind = [&](std::uint32_t unbound, ID128 paneID) {
        CHECK_EQ(unbound, handle);
        CHECK(paneID == owned.source->paneID());
        CHECK_EQ(replicas.effectReceipt(paneID).consumed, 0u);
        CHECK_EQ(replicas.effectReceipt(paneID).acknowledged, 0u);
        CHECK_EQ(replicas.effectReceipt(otherPane).consumed, 1u);
        wasUnbound = true;
        link.client.abandon();
    };
    link.host.unbindPane(handle, 0);
    link.pump();
    REQUIRE(wasUnbound);
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 0u);
    CHECK_EQ(replicas.effectReceipt(otherPane).consumed, 1u);
}

TEST(serialPaneUnbindsDoNotExhaustTheBoundedReceiptSlots)
{
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Agent, &replicas);
    for (std::size_t index = 0; index < maxPanesPerConnection + 2; ++index) {
        UbiquittyTest::OwnedPane owned;
        const ID128 paneID { 1'000 + index, 2'000 };
        owned.source = std::make_unique<PaneSource>(owned.terminal, paneID);
        owned.source->publish(FrameClass::Structural, 1);
        link.bind(*owned.source);
        REQUIRE(link.handle);
        REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
        for (int pass = 0; pass < 4; ++pass)
            link.pump();
        REQUIRE(link.client.state() == ConnectionState::Attached);
        REQUIRE(recorder.received.size() == index + 1);
        link.host.unbindPane(link.handle, 0);
        for (int pass = 0; pass < 4; ++pass)
            link.pump();
        REQUIRE(link.client.state() == ConnectionState::Attached);
        // Picture eviction is independent: it must neither retain a dead
        // receipt slot nor erase a receipt needed by a live resumed pane.
        replicas.forget(paneID);
    }
    CHECK_EQ(recorder.received.size(), maxPanesPerConnection + 2);
}

TEST(unbindingOneOfTwoHandlesForAPaneKeepsItsReceiptUntilTheLastUnbind)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    ReplicaSet replicas;
    EffectLink link(recorder, ClientKind::Agent, &replicas);
    link.bind(*owned.source);
    REQUIRE(link.handle);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    for (int pass = 0; pass < 4; ++pass)
        link.pump();
    REQUIRE(replicas.effectReceipt(owned.source->paneID()).consumed == 1);

    // PaneBind's grammar permits distinct handles naming the same stable
    // pane. Exercise the client's receive boundary without a second host
    // reader: unbinding one handle is not definitive for the other.
    const auto secondHandle = link.handle + 1;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> frame;
    encode(body, PaneBind { .handle = secondHandle, .paneID = owned.source->paneID() });
    REQUIRE(appendFrame(frame, FrameKind::PaneBind, 0, body));
    link.client.receiveStream(frame, ++link.now);
    REQUIRE(link.client.state() == ConnectionState::Attached);
    REQUIRE(link.client.paneTerminal(secondHandle));
    for (const auto handle : { link.handle, secondHandle }) {
        body.clear();
        frame.clear();
        encode(body, PaneUnbind { .handle = handle, .reason = 0 });
        REQUIRE(appendFrame(frame, FrameKind::PaneUnbind, 0, body));
        link.client.receiveStream(frame, ++link.now);
        REQUIRE(link.client.state() == ConnectionState::Attached);
        CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, handle == link.handle ? 1u : 0u);
    }
}

TEST(aliasEffectBatchesInterleaveWithoutDuplicateCallbacksOrPrematureAcknowledgement)
{
    for (const bool hasRetainedReplicas : { false, true }) {
        UbiquittyTest::OwnedPane owned;
        EffectRecorder recorder;
        ReplicaSet replicas;
        EffectLink link(recorder, ClientKind::Agent, hasRetainedReplicas ? &replicas : nullptr);
        link.bind(*owned.source);
        const auto secondHandle = link.handle + 1;
        std::vector<std::uint8_t> body;
        std::vector<std::uint8_t> frame;
        encode(body, PaneBind { .handle = secondHandle, .paneID = owned.source->paneID() });
        REQUIRE(appendFrame(frame, FrameKind::PaneBind, 0, body));
        link.client.receiveStream(frame, ++link.now);
        (void)link.client.takeStreamOutput();
        const auto receive = [&](std::uint32_t handle, std::uint64_t sequence, std::uint64_t through, std::uint16_t remaining) {
            auto record = bell(sequence);
            record.paneID = owned.source->paneID();
            const auto encoded = encodeEffect(record);
            REQUIRE(encoded);
            const auto packet = encodeEffectPacket(record.paneID, 8, through, remaining, encoded->view());
            REQUIRE(packet);
            frame.clear();
            REQUIRE(appendFrame(frame, FrameKind::Effect, handle, packet->view()));
            link.client.receiveStream(frame, ++link.now);
        };
        receive(link.handle, 1, 2, 1);
        receive(secondHandle, 1, 2, 1);
        CHECK_EQ(recorder.received.size(), 1u);
        receive(link.handle, 2, 2, 0);
        link.client.tick(link.now += 50'000);
        FrameDecoder acknowledgementFrames;
        acknowledgementFrames.append(link.client.takeStreamOutput());
        std::uint64_t firstAcknowledged = 0;
        std::uint64_t secondAcknowledged = 0;
        while (const auto acknowledgementFrame = acknowledgementFrames.next()) {
            if (acknowledgementFrame->kind != FrameKind::Ack)
                continue;
            const auto acknowledgement = decodeAck(acknowledgementFrame->body);
            REQUIRE(acknowledgement);
            for (const auto& acknowledgedPane : acknowledgement->panes) {
                if (acknowledgedPane.handle == link.handle)
                    firstAcknowledged = acknowledgedPane.effectsAcknowledged;
                if (acknowledgedPane.handle == secondHandle)
                    secondAcknowledged = acknowledgedPane.effectsAcknowledged;
            }
        }
        CHECK_EQ(firstAcknowledged, 2u);
        CHECK_EQ(secondAcknowledged, 0u);
        receive(secondHandle, 2, 2, 0);
        REQUIRE(link.client.state() == ConnectionState::Attached);
        CHECK_EQ(recorder.received.size(), 2u);
        receive(link.handle, 3, 4, 1);
        receive(link.handle, 4, 4, 0);
        receive(secondHandle, 3, 4, 1);
        receive(secondHandle, 4, 4, 0);
        REQUIRE(link.client.state() == ConnectionState::Attached);
        CHECK_EQ(recorder.received.size(), 4u);
        receive(link.handle, 5, 6, 1);
        body.clear();
        frame.clear();
        encode(body, PaneUnbind { .handle = link.handle, .reason = 0 });
        REQUIRE(appendFrame(frame, FrameKind::PaneUnbind, 0, body));
        link.client.receiveStream(frame, ++link.now);
        receive(secondHandle, 5, 6, 1);
        receive(secondHandle, 6, 6, 0);
        REQUIRE(link.client.state() == ConnectionState::Attached);
        CHECK_EQ(recorder.received.size(), 6u);
        if (hasRetainedReplicas) {
            CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).acknowledged, 6u);
            CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 6u);
        }
    }
}

TEST(theReceiptLimitStillRefusesOverflowAndEndReleasesOnlyItsNamedSlot)
{
    ReplicaSet replicas;
    for (std::size_t index = 0; index < maxPanesPerConnection; ++index)
        REQUIRE(replicas.keepEffectReceipt({ index + 1, 10 }, { .acknowledged = 1, .consumed = 2 }));
    constexpr ID128 extra { 1'000, 10 };
    CHECK(!replicas.keepEffectReceipt(extra, { .acknowledged = 1, .consumed = 2 }));
    replicas.end({ 2, 10 }, ReplicaSet::EndReason::PaneGone);
    CHECK_EQ(replicas.effectReceipt({ 2, 10 }).consumed, 0u);
    CHECK_EQ(replicas.effectReceipt({ 1, 10 }).consumed, 2u);
    REQUIRE(replicas.keepEffectReceipt(extra, { .acknowledged = 1, .consumed = 2 }));
    CHECK(!replicas.keepEffectReceipt({ 1'001, 10 }, { .acknowledged = 1, .consumed = 2 }));
}

TEST(nonResumableConnectionTeardownReleasesItsEffectReaderWithoutTouchingThePane)
{
    UbiquittyTest::OwnedPane owned;
    EffectRecorder recorder;
    ReplicaSet replicas;
    const Terminal* picture = nullptr;
    {
        EffectLink first(recorder, ClientKind::Human, &replicas);
        first.bind(*owned.source);
        REQUIRE(first.handle);
        REQUIRE(owned.source->effects().readerCount() == 1);
        picture = first.client.paneTerminal(first.handle);
        REQUIRE(picture);
    }
    CHECK_EQ(owned.source->effects().readerCount(), 0u);
    EffectLink next(recorder, ClientKind::Human, &replicas);
    next.bind(*owned.source);
    REQUIRE(next.handle);
    CHECK(next.client.paneTerminal(next.handle) == picture);
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    for (int index = 0; index < 5; ++index)
        next.pump();
    REQUIRE(recorder.received.size() == 1);
    CHECK_EQ(owned.source->effects().retiredThrough(), 1u);
}

TEST(aRetainedAttachmentReplaysAPartialBatchWithoutRepeatingItsConsumedEffect)
{
    UbiquittyTest::OwnedPane owned;
    EffectSecrets secrets;
    AttachmentTable table({ 31, 32 }, measuredResumePolicy(), secrets);
    ReplicaSet replicas;
    EffectRecorder recorder;
    auto first = std::make_unique<RetainedEffectLink>(*owned.source, table, replicas, recorder, 1);
    REQUIRE(first->host.bindPane(*owned.source));
    for (int index = 0; index < 5; ++index)
        first->pump();
    REQUIRE(first->client.welcome());
    const Welcome welcome = *first->client.welcome();
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 7, .commandLine = "first" }, primary));
    REQUIRE(owned.source->appendEffect(CommandFinishedEffect { .exitStatus = 0, .commandLine = "second" }, primary));
    first->pump();
    REQUIRE(recorder.received.size() == 1);
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).consumed, 1u);
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).acknowledged, 0u);
    table.noteTransportLost(welcome.attachmentID, 1, first->now);
    first.reset();
    CHECK_EQ(owned.source->effects().readerCount(), 1u);
    ResumeRequest resume { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    resume.panes.push_back({ .paneID = owned.source->paneID(), .view = ViewState::RecordsOnly, .effectsAcknowledged = replicas.effectReceipt(owned.source->paneID()).acknowledged });
    RetainedEffectLink next(*owned.source, table, replicas, recorder, 2, std::move(resume));
    for (int index = 0; index < 8; ++index)
        next.pump();
    CHECK(next.host.state() == ConnectionState::Attached);
    CHECK(next.client.state() == ConnectionState::Attached);
    REQUIRE(recorder.received.size() == 2);
    CHECK(recorder.received[0].commandLine == "first");
    CHECK_EQ(recorder.received[0].exitStatus, 7);
    CHECK(recorder.received[1].commandLine == "second");
    CHECK_EQ(recorder.received[1].exitStatus, 0);
    CHECK_EQ(replicas.effectReceipt(owned.source->paneID()).acknowledged, 2u);
    CHECK_EQ(owned.source->effects().retiredThrough(), 2u);
}

TEST(aResumeCannotAcknowledgeAPartialBatchWhoseFinalPacketNeverLeft)
{
    UbiquittyTest::OwnedPane owned;
    EffectSecrets secrets;
    AttachmentTable table({ 31, 32 }, measuredResumePolicy(), secrets);
    ReplicaSet replicas;
    EffectRecorder recorder;
    auto first = std::make_unique<RetainedEffectLink>(*owned.source, table, replicas, recorder, 1);
    REQUIRE(first->host.bindPane(*owned.source));
    for (int index = 0; index < 5; ++index)
        first->pump();
    REQUIRE(first->client.welcome());
    const Welcome welcome = *first->client.welcome();
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    first->pump();
    REQUIRE(recorder.received.size() == 1);
    table.noteTransportLost(welcome.attachmentID, 1, first->now);
    first.reset();
    ResumeRequest resume { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    resume.panes.push_back({ .paneID = owned.source->paneID(), .view = ViewState::RecordsOnly, .effectsAcknowledged = 1 });
    RetainedEffectLink next(*owned.source, table, replicas, recorder, 2, std::move(resume));
    CHECK(next.host.state() == ConnectionState::Closed);
    CHECK(next.host.byeReason() == ByeReason::Malformed);
    CHECK_EQ(owned.source->effects().retiredThrough(), 0u);
    CHECK_EQ(owned.source->effects().size(), 2u);
    CHECK_EQ(recorder.received.size(), 1u);
}

UBIQUITTY_TEST_MAIN()
