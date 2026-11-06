// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Kind 35 has no transport grammar and is rejected on its header. Retained
// body ownership is exercised through FrameIngress; canonical clipboard,
// Result, lane and handle tests cover their typed delivery/resource contracts.

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using Allocation = UbiquittyTest::ScopedNothrowAllocationRefusal;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return Allocation::refuses(size) ? nullptr : ::operator new[](size);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

enum class AdmissionStage : std::uint8_t {
    Greeting,
    Attached,
    AwaitingPending,
    Pending
};

class Secrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 99 }; }

private:
    std::uint64_t m_next = 0;
};

HostIdentity hostIdentity(AdmissionStage stage)
{
    HostIdentity identity;
    identity.hostInstanceID = { 11, 17 };
    identity.verifiedKind = ClientKind::Human;
    identity.principal = "verified-fixture";
    identity.mayDrive = true;
    identity.selection = { .target = AdmissionTarget::Session, .sessionID = 11, .workspaceID = 0, .daemonInstanceID = 17, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    if (stage == AdmissionStage::AwaitingPending || stage == AdmissionStage::Pending)
        identity.pendingConsent = ConsentBinding { .requestID = 7, .sessionID = 11 };
    return identity;
}

struct Observer final : ClientPaneDelegate {
    unsigned decoded = 0;
    void connectionFrameDecoded(FrameKind, std::size_t) override { ++decoded; }
};

struct Pair {
    Secrets secrets;
    AttachmentTable table { { 11, 17 }, measuredResumePolicy(), secrets };
    HostConnection host;
    Observer observer;
    ClientConnection client;

    explicit Pair(AdmissionStage stage)
        : host(measuredHostPolicy(0), hostIdentity(stage))
        , client(measuredClientPolicy(), ClientIdentity { .clientInstance = { 7, 8 }, .kind = ClientKind::Human }, &observer)
    {
        host.serveFromTable(table, { .principal = "verified-fixture", .mayDrive = true, .verifiedKind = ClientKind::Human },
            1, [](ID128) -> PaneSource* { return nullptr; });
        client.start();
        if (stage == AdmissionStage::Greeting)
            return;
        host.receiveStream(client.takeStreamOutput(), 1);
        carry();
        if (stage == AdmissionStage::Attached) {
            CHECK(host.state() == ConnectionState::Attached);
            CHECK(client.state() == ConnectionState::Attached);
        } else {
            CHECK(host.state() == ConnectionState::AwaitingConsent);
            CHECK(client.state() == ConnectionState::AwaitingConsent);
        }
        if (stage == AdmissionStage::Pending) {
            CHECK(host.sendConsentStatus({ .question = { .requestID = 7, .sessionID = 11 }, .phase = ConsentStatusPhase::Pending }));
            carry();
        }
    }

    void carry()
    {
        std::vector<std::uint8_t> frames;
        while (host.popStreamFrame(frames, 1)) {
        }
        client.receiveStream(frames, 1);
    }
};

std::vector<std::uint8_t> frame(FrameKind kind, std::span<const std::uint8_t> body, std::uint32_t channel = 0)
{
    std::vector<std::uint8_t> bytes;
    CHECK(appendFrame(bytes, kind, channel, body));
    return bytes;
}

FrameHeaderDecision headerDecision(std::span<const std::uint8_t> bytes)
{
    FrameHeader header;
    EnvelopeError error = EnvelopeError::None;
    const auto state = readFrameHeader(bytes, header, error);
    if (state == FrameHeaderState::NeedsMore)
        return {};
    if (state == FrameHeaderState::Failed)
        return { .state = FrameHeaderDecision::State::Refused };
    return { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
}

constexpr std::array stages { AdmissionStage::Greeting, AdmissionStage::Attached, AdmissionStage::AwaitingPending, AdmissionStage::Pending };

} // namespace

TEST(removedCatalogKindIsRefusedFromItsFirstByteWithoutABody)
{
    CHECK(!frameKindOf(35));
    for (const auto& bytes : { std::vector<std::uint8_t> { 35 }, std::vector<std::uint8_t> { 35, 1, 0 },
             std::vector<std::uint8_t> { 35, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 3 } }) {
        FrameHeader header;
        EnvelopeError error = EnvelopeError::None;
        const auto attempts = Allocation::attemptCount();
        CHECK(readFrameHeader(bytes, header, error) == FrameHeaderState::Failed);
        CHECK(error == EnvelopeError::UnknownKind);
        CHECK_EQ(Allocation::attemptCount(), attempts);
        FrameIngress ingress(maxFrameBodyBytes);
        const auto result = ingress.consume(bytes, headerDecision, [](auto, auto) {
            CHECK_MSG(false, "removed kind reached delivery");
            return FrameDelivery::Continue;
        });
        CHECK(result.state == FrameIngress::State::Refused);
        CHECK_EQ(result.consumedBytes, 1u);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(Allocation::attemptCount(), attempts);
    }
}

TEST(everyAdmissionStageRefusesCatalogBeforeAllocationOrCallbacks)
{
    const std::array<std::uint8_t, 1> removed { 35 };
    for (const auto stage : stages) {
        Pair pair(stage);
        const auto before = pair.observer.decoded;
        pair.host.receiveStream(removed, 2);
        CHECK(pair.host.state() == ConnectionState::Closed);
        CHECK(pair.host.byeReason() == ByeReason::UnknownKind);
        CHECK_EQ(pair.host.inboundFrameBytes(), 0u);
        CHECK(pair.host.takeCalls().empty());
        pair.client.receiveStream(removed, 2);
        CHECK(pair.client.state() == ConnectionState::Closed);
        CHECK(pair.client.byeReason() == ByeReason::UnknownKind);
        CHECK_EQ(pair.observer.decoded, before);
        pair.host.receiveStream(removed, 3);
        pair.client.receiveStream(removed, 3);
        CHECK_EQ(pair.observer.decoded, before);
    }
}

TEST(noDatagramCanMakeTheRemovedKindValid)
{
    const std::array<std::uint8_t, 3> removed { 35, 1, 0 };
    for (const auto stage : stages) {
        Pair pair(stage);
        const auto before = pair.observer.decoded;
        pair.host.receiveDatagram(removed, 2);
        pair.client.receiveDatagram(removed, 2);
        CHECK(pair.host.state() == ConnectionState::Closed);
        CHECK(pair.host.byeReason() == ByeReason::UnknownKind);
        CHECK(pair.client.state() == ConnectionState::Closed);
        CHECK(pair.client.byeReason() == ByeReason::UnknownKind);
        CHECK_EQ(pair.observer.decoded, before);
    }
}

TEST(typedFramesForNeverBoundHandlesStillClose)
{
    Pair pair(AdmissionStage::Attached);
    const std::array<std::uint8_t, 1> input { 'x' };
    std::vector<std::uint8_t> body;
    encode(body, Raw { .inputSeq = 1, .bytes = input });
    pair.host.receiveStream(frame(FrameKind::Raw, body, 999), 2);
    CHECK(pair.host.state() == ConnectionState::Closed);
    CHECK(pair.host.byeReason() == ByeReason::UnboundHandle);
    pair.client.receiveStream(frame(FrameKind::PaneDirty, {}, 999), 2);
    CHECK(pair.client.state() == ConnectionState::Closed);
    CHECK(pair.client.byeReason() == ByeReason::UnboundHandle);

    Pair factsPair(AdmissionStage::Attached);
    PaneFacts facts;
    facts.revision = 1;
    body.clear();
    encodePaneFacts(body, facts);
    CHECK(decodePaneFacts(body));
    factsPair.client.receiveStream(frame(FrameKind::PaneFacts, body, 999), 2);
    CHECK(factsPair.client.state() == ConnectionState::Closed);
    CHECK(factsPair.client.byeReason() == ByeReason::Malformed);
}

TEST(aCanonicalCallRemainsStreamOnlyAfterAdmission)
{
    Pair pair(AdmissionStage::Attached);
    std::vector<std::uint8_t> body;
    encode(body, Call { .text = R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})" });
    pair.host.receiveDatagram(frame(FrameKind::Call, body), 2);
    CHECK(pair.host.state() == ConnectionState::Closed);
    CHECK(pair.host.byeReason() == ByeReason::Malformed);
    CHECK(pair.host.takeCalls().empty());
}

TEST(aCanonicalHeaderSplitAtEveryByteAllocatesOnlyItsExactBody)
{
    const std::vector<std::uint8_t> body(257, 0xAB);
    const auto bytes = frame(FrameKind::Notify, body);
    const auto headerBytes = bytes.size() - body.size();
    for (std::size_t split = 0; split <= headerBytes; ++split) {
        FrameIngress ingress(maxFrameBodyBytes);
        unsigned delivered = 0;
        const auto receive = [&](auto, std::span<const std::uint8_t> received) {
            CHECK_EQ(ingress.retainedBytes(), body.size());
            CHECK(std::ranges::equal(received, body));
            ++delivered;
            return FrameDelivery::Continue;
        };
        const auto attempts = Allocation::attemptCount();
        CHECK(ingress.consume(std::span(bytes).first(split), headerDecision, receive).state == FrameIngress::State::Accepting);
        CHECK_EQ(delivered, 0u);
        CHECK_EQ(ingress.retainedBytes(), split == headerBytes ? body.size() : 0u);
        CHECK(ingress.consume(std::span(bytes).subspan(split), headerDecision, receive).state == FrameIngress::State::Accepting);
        CHECK_EQ(delivered, 1u);
        CHECK_EQ(Allocation::attemptCount(), attempts + 1);
        CHECK_EQ(ingress.pendingBytes(), 0u);
        ingress.releaseRetained();
        CHECK_EQ(ingress.retainedBytes(), 0u);
    }
}

TEST(aCanonicalBodyBudgetIsCheckedBeforeAllocationAndRefusalLatches)
{
    const std::vector<std::uint8_t> body(257, 0xAB);
    const auto bytes = frame(FrameKind::Notify, body);
    FrameIngress ingress(body.size() - 1);
    unsigned delivered = 0;
    const auto receive = [&](auto, auto) { ++delivered; return FrameDelivery::Continue; };
    const auto attempts = Allocation::attemptCount();
    const auto result = ingress.consume(bytes, headerDecision, receive);
    CHECK(result.state == FrameIngress::State::Refused);
    CHECK(ingress.error() == FrameIngressError::Oversize);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(Allocation::attemptCount(), attempts);
    CHECK_EQ(ingress.consume(bytes, headerDecision, receive).consumedBytes, 0u);
    CHECK_EQ(delivered, 0u);
}

TEST(aRefusedCanonicalAllocationPublishesNothingAndReleasesItsOwner)
{
    const std::vector<std::uint8_t> body(257, 0xAB);
    const auto bytes = frame(FrameKind::Notify, body);
    FrameIngress ingress(maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto receive = [&](auto, auto) { ++delivered; return FrameDelivery::Continue; };
    const auto refusals = Allocation::refusalCount();
    {
        Allocation refuse(body.size());
        CHECK(ingress.consume(bytes, headerDecision, receive).state == FrameIngress::State::Refused);
    }
    CHECK(ingress.error() == FrameIngressError::AllocationFailed);
    CHECK_EQ(Allocation::refusalCount(), refusals + 1);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(delivered, 0u);
    ingress.reset();
    CHECK(ingress.consume(bytes, headerDecision, receive).state == FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 1u);
}

TEST(stoppedDeliveryKeepsItsBorrowChargedAndLeavesTheSuffixUnconsumed)
{
    const std::vector<std::uint8_t> body(257, 0xAB);
    const auto one = frame(FrameKind::Notify, body);
    auto bytes = one;
    bytes.insert(bytes.end(), one.begin(), one.end());
    FrameIngress ingress(maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto receive = [&](auto, std::span<const std::uint8_t> received) {
        ++delivered;
        CHECK_EQ(ingress.retainedBytes(), body.size());
        CHECK(std::ranges::equal(received, body));
        return FrameDelivery::Stop;
    };
    const auto first = ingress.consume(bytes, headerDecision, receive);
    CHECK(first.state == FrameIngress::State::Stopped);
    CHECK_EQ(first.consumedBytes, one.size());
    CHECK_EQ(delivered, 1u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.consume(std::span(bytes).subspan(first.consumedBytes), headerDecision, receive).consumedBytes, 0u);
    ingress.reset();
    CHECK_EQ(ingress.consume(std::span(bytes).subspan(first.consumedBytes), headerDecision, receive).consumedBytes, one.size());
    CHECK_EQ(delivered, 2u);
}

TEST(resetReleasesAnUnfinishedCanonicalBodyWithoutDeliveringIt)
{
    const std::vector<std::uint8_t> body(257, 0xAB);
    const auto bytes = frame(FrameKind::Notify, body);
    FrameIngress ingress(maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto receive = [&](auto, auto) { ++delivered; return FrameDelivery::Continue; };
    const auto partial = std::span(bytes).first(bytes.size() - 1);
    CHECK_EQ(ingress.consume(partial, headerDecision, receive).consumedBytes, partial.size());
    CHECK_EQ(ingress.retainedBytes(), body.size());
    CHECK(ingress.pendingBytes() > 0);
    ingress.reset();
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(delivered, 0u);
    CHECK(ingress.consume(bytes, headerDecision, receive).state == FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 1u);
}

TEST(aZeroBodyCanonicalEnvelopeNeedsNoAllocation)
{
    const auto bytes = frame(FrameKind::Notify, {});
    FrameIngress ingress(maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto attempts = Allocation::attemptCount();
    CHECK(ingress.consume(bytes, headerDecision, [&](auto, auto body) {
                     CHECK(body.empty());
                     ++delivered;
                     return FrameDelivery::Continue;
                 })
              .state
        == FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 1u);
    CHECK_EQ(Allocation::attemptCount(), attempts);
    CHECK_EQ(ingress.retainedBytes(), 0u);
}

UBIQUITTY_TEST_MAIN()
