// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/Samples.h"

#include <algorithm>
#include <functional>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr ID128 primary { 5, 6 };

class Recorder final : public ClientPaneDelegate {
public:
    std::vector<std::vector<std::int16_t>> sounds;
    std::vector<EffectKind> kinds;
    std::function<void(const PAL::PlayTonesRequest&)> receivedSound;

    void paneEffect(std::uint32_t, const EffectView& record, const EffectBody& body, EffectDisposition disposition) override
    {
        kinds.push_back(record.kind);
        if (const auto* sound = std::get_if<SoundEffect>(&body)) {
            CHECK(disposition == EffectDisposition::Post);
            const auto& request = sound->request;
            if (request.sampleFrames)
                sounds.emplace_back(request.samplePCM, request.samplePCM + request.sampleFrames);
            else
                sounds.emplace_back();
            if (receivedSound)
                receivedSound(request);
        }
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

    Link()
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 31, 32 }, .attachmentID = { 33, 34 }, .resumeSecret = { 35, 36 }, .principal = "samples", .mayDrive = true, .verifiedKind = ClientKind::Human, .selection = UbiquittyTest::syncDefaultSelection() })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = primary, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts() }, &recorder, nullptr, &replicas)
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

    void receive(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
    {
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, channel, body));
        client.receiveStream(frame, ++now);
    }

    void install(std::span<const std::int16_t> pcm, std::uint32_t serial)
    {
        auto& bank = ensureSampleBank(owned.terminal);
        bank.slots[0].assign(pcm.begin(), pcm.end());
        bank.serial = serial;
    }

    void appendSound()
    {
        PAL::PlayTonesRequest request;
        request.wave = 6;
        request.sampleSlot = 1;
        const auto* bank = owned.terminal.sampleBank.get();
        if (bank) {
            if (const auto* slot = bank->slot(1)) {
                request.samplePCM = slot->data();
                request.sampleFrames = static_cast<std::uint32_t>(slot->size());
            }
        }
        REQUIRE(owned.source->appendSound(request, bank ? bank->serial : 0, primary));
    }

    void receiveSound(const SampleIdentity& sample, std::uint64_t sequence, std::uint64_t through, std::uint16_t remaining)
    {
        SoundEffect sound;
        sound.request.wave = 6;
        sound.request.sampleSlot = 1;
        sound.sample = sample;
        const auto body = encodeEffectBody(sound);
        REQUIRE(body);
        const auto generation = client.paneGeneration(handle);
        const auto record = encodeEffect({ .paneID = owned.source->paneID(), .generation = generation, .sequence = sequence, .primary = primary, .kind = EffectKind::Sound, .count = 1, .target = 0, .payload = body->view() });
        REQUIRE(record);
        const auto packet = encodeEffectPacket(owned.source->paneID(), generation, through, remaining, record->view());
        REQUIRE(packet);
        receive(FrameKind::Effect, handle, packet->view());
    }
};

FrameKind kindOf(const std::vector<std::uint8_t>& bytes)
{
    FrameDecoder decoder;
    decoder.append(bytes);
    const auto frame = decoder.next();
    CHECK(frame.has_value());
    return frame->kind;
}

TEST(sampledEffectWaitsForBulkAndUsesItsCapturedOwner)
{
    Link link;
    const std::array<std::int16_t, 4> original { -32768, -1, 0, 32767 };
    link.install(original, 1);
    link.appendSound();
    link.install(std::array<std::int16_t, 2> { 5, 6 }, 2);
    std::vector<std::vector<std::uint8_t>> bulk;
    for (int pass = 0; pass < 4; ++pass) {
        for (const auto& frame : link.takeHostFrames()) {
            if (kindOf(frame) == FrameKind::BulkChunk)
                bulk.push_back(frame);
            else
                link.client.receiveStream(frame, link.now);
        }
    }
    REQUIRE(!bulk.empty());
    CHECK(link.client.state() == ConnectionState::Attached);
    CHECK(link.recorder.sounds.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    for (const auto& frame : bulk)
        link.client.receiveStream(frame, ++link.now);
    REQUIRE(link.recorder.sounds.size() == 1);
    CHECK(std::ranges::equal(link.recorder.sounds[0], original));
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 1u);
}

TEST(sampleSlotReplacementWaitsForThePreviousCompletedBatchAcknowledgement)
{
    Link link;
    link.install(std::array<std::int16_t, 1> { 11 }, 1);
    link.appendSound();
    link.install(std::array<std::int16_t, 1> { 22 }, 2);
    link.appendSound();
    std::size_t heads = 0;
    for (int pass = 0; pass < 6; ++pass) {
        for (const auto& frame : link.takeHostFrames()) {
            heads += kindOf(frame) == FrameKind::SampleHead ? 1 : 0;
            link.client.receiveStream(frame, link.now);
        }
    }
    CHECK_EQ(heads, 1u);
    REQUIRE(link.recorder.sounds.size() == 1);
    CHECK_EQ(link.recorder.sounds[0][0], 11);
    link.client.tick(link.now += 50'000);
    link.host.receiveStream(link.client.takeStreamOutput(), link.now);
    for (int pass = 0; pass < 6; ++pass)
        link.pump();
    REQUIRE(link.recorder.sounds.size() == 2);
    CHECK_EQ(link.recorder.sounds[1][0], 22);
}

TEST(sampleLeaseSurvivesAPlaybackCallbackThatAbandonsTheConnection)
{
    Link link;
    const std::array<std::int16_t, 3> original { 44, -55, 66 };
    link.install(original, 1);
    link.recorder.receivedSound = [&](const PAL::PlayTonesRequest& request) {
        link.client.abandon();
        CHECK_EQ(request.sampleFrames, original.size());
        CHECK(std::equal(original.begin(), original.end(), request.samplePCM));
    };
    link.appendSound();
    for (int pass = 0; pass < 5 && link.client.state() == ConnectionState::Attached; ++pass)
        link.pump();
    CHECK(link.client.state() == ConnectionState::Closed);
    REQUIRE(link.recorder.sounds.size() == 1);
    CHECK(std::ranges::equal(link.recorder.sounds[0], original));
}

TEST(corruptedSampleNeverConsumesOrAcknowledgesItsEffect)
{
    Link link;
    link.install(std::array<std::int16_t, 2> { 11, 22 }, 1);
    link.appendSound();
    bool didCorrupt = false;
    for (int pass = 0; pass < 4 && link.client.state() == ConnectionState::Attached; ++pass) {
        for (auto frame : link.takeHostFrames()) {
            if (kindOf(frame) == FrameKind::BulkChunk) {
                frame.back() ^= 1;
                didCorrupt = true;
            }
            link.client.receiveStream(frame, link.now);
        }
    }
    CHECK(didCorrupt);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK(link.recorder.sounds.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 0u);
}

TEST(skippedSoundsFinishTheirBatchWithoutTransferringSamples)
{
    Link link;
    link.client.setView(link.handle, ViewState::RecordsOnly);
    link.pump();
    link.install(std::array<std::int16_t, 2> { 11, 22 }, 1);
    link.appendSound();
    for (int pass = 0; pass < 4; ++pass) {
        for (const auto& frame : link.takeHostFrames()) {
            CHECK(kindOf(frame) != FrameKind::SampleHead);
            link.client.receiveStream(frame, link.now);
        }
    }
    CHECK(link.recorder.sounds.empty());
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).acknowledged, 1u);
}

TEST(deferredSamplePacketsHaveAFixedConnectionWideBound)
{
    Link link;
    const auto captured = SampleBytes::capture(1, std::array<std::int16_t, 1> { 12 }, 2);
    REQUIRE(captured);
    std::vector<std::uint8_t> head;
    REQUIRE(encodeSampleHead(head, { .slot = 1, .sample = captured->sample, .transfer = 901 }));
    link.receive(FrameKind::SampleHead, link.handle, head);
    for (std::size_t index = 0; index < maxEffectRecords; ++index) {
        link.receiveSound(captured->sample, index + 1, maxEffectRecords, static_cast<std::uint16_t>(maxEffectRecords - index - 1));
        REQUIRE(link.client.state() == ConnectionState::Attached);
    }
    CHECK(link.client.inboundEffectBytes() > captured->view().size());
    CHECK(link.recorder.sounds.empty());
    link.receiveSound(captured->sample, maxEffectRecords + 1, maxEffectRecords + 1, 0);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
    CHECK_EQ(link.replicas.effectReceipt(link.owned.source->paneID()).consumed, 0u);
}

TEST(sampleUnbindReleasesDeferredOwnersAndDiscardsLateBulk)
{
    Link link;
    link.install(std::array<std::int16_t, 2> { 11, 22 }, 1);
    link.appendSound();
    std::vector<std::vector<std::uint8_t>> bulk;
    for (const auto& frame : link.takeHostFrames()) {
        if (kindOf(frame) == FrameKind::BulkChunk)
            bulk.push_back(frame);
        else
            link.client.receiveStream(frame, link.now);
    }
    REQUIRE(link.client.inboundEffectBytes() != 0);
    link.host.unbindPane(link.handle, 0);
    for (const auto& frame : link.takeHostFrames())
        link.client.receiveStream(frame, link.now);
    for (const auto& frame : bulk)
        link.client.receiveStream(frame, ++link.now);
    CHECK(link.client.state() == ConnectionState::Attached);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
    CHECK(link.recorder.sounds.empty());
}

TEST(replacingASampleStillOwedToADeferredSoundIsRefused)
{
    Link link;
    const auto first = SampleBytes::capture(1, std::array<std::int16_t, 1> { 11 }, 2);
    const auto second = SampleBytes::capture(2, std::array<std::int16_t, 1> { 22 }, 2);
    REQUIRE(first && second);
    std::vector<std::uint8_t> head;
    REQUIRE(encodeSampleHead(head, { .slot = 1, .sample = first->sample, .transfer = 901 }));
    link.receive(FrameKind::SampleHead, link.handle, head);
    link.receiveSound(first->sample, 1, 1, 0);
    head.clear();
    REQUIRE(encodeSampleHead(head, { .slot = 1, .sample = second->sample, .transfer = 902 }));
    link.receive(FrameKind::SampleHead, link.handle, head);
    CHECK(link.client.state() == ConnectionState::Closed);
    CHECK_EQ(link.client.inboundEffectBytes(), 0u);
    CHECK(link.recorder.sounds.empty());
}

TEST(logRefusalLeavesTheCapturedSoundAvailableForAnExactRetry)
{
    UbiquittyTest::OwnedPane owned;
    VT::PendingSound pending;
    std::array<std::int16_t, 2> original { -9, 17 };
    PAL::PlayTonesRequest request;
    request.wave = 6;
    request.sampleSlot = 1;
    request.samplePCM = original.data();
    request.sampleFrames = static_cast<std::uint32_t>(original.size());
    REQUIRE(pending.push(request, 73) == VT::PendingSoundAdmission::Queued);
    original.fill(99);
    auto& log = owned.source->effects();
    for (std::size_t index = 0; index < maxEffectRecords; ++index)
        REQUIRE(owned.source->appendEffect(BellEffect {}, primary));
    const auto* held = pending.front();
    REQUIRE(held);
    const auto refused = owned.source->appendSound(held->request, held->sampleSerial, primary);
    CHECK(!refused && refused.error() == EffectRefusal::OverLimit);
    CHECK(pending.front() == held);
    CHECK_EQ(pending.pcmBytes, 4u);
    CHECK_EQ(held->request.samplePCM[0], -9);
    log.trim(1);
    REQUIRE(owned.source->appendSound(held->request, held->sampleSerial, primary));
    pending.pop();
    CHECK(!pending.front());
    CHECK_EQ(pending.pcmBytes, 0u);
    const auto batch = log.readAfter(log.retiredThrough(), EffectReadMode::Consecutive);
    REQUIRE(batch && batch->size == 1);
    const auto decoded = decodeEffectBody(EffectKind::Sound, batch->records[0].payload);
    REQUIRE(decoded && std::holds_alternative<SoundEffect>(*decoded));
    CHECK_EQ(std::get<SoundEffect>(*decoded).sample.serial, 73u);
    const std::array<std::uint8_t, 4> canonical { 247, 255, 17, 0 };
    CHECK(std::ranges::equal(batch->records[0].data, canonical));
}

} // namespace

UBIQUITTY_TEST_MAIN()
