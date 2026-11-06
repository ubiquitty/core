// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "SyncLink.h"

#include <array>
#include <new>
#include <span>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

HostIdentity servingIdentity()
{
    HostIdentity identity;
    identity.hostInstanceID = { 11, 17 };
    identity.selection = { .target = AdmissionTarget::Session, .sessionID = 11, .workspaceID = 0, .daemonInstanceID = 17, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    return identity;
}

} // namespace

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(liveInnerReadersRefuseBadHeadersBeforeBodyAllocation)
{
    const std::array<std::vector<std::uint8_t>, 3> headers {
        std::vector<std::uint8_t> { std::to_underlying(FrameKind::Greet), 0, 0x80, 0x80, 4 }, // 65,536
        std::vector<std::uint8_t> { std::to_underlying(FrameKind::Welcome), 0, 1 }, // wrong direction
        std::vector<std::uint8_t> { std::to_underlying(FrameKind::Greet), 1, 1 }, // wrong scope
    };
    for (const auto& header : headers) {
        for (std::size_t split = 0; split <= header.size(); ++split) {
            HostConnection host(UbiquittyTest::syncHostPolicy(0), {});
            const std::size_t before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
            {
                UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
                host.receiveStream(std::span { header }.first(split), 1);
                host.receiveStream(std::span { header }.subspan(split), 2);
            }
            CHECK(host.state() == ConnectionState::Closed);
            CHECK_EQ(host.inboundFrameBytes(), 0u);
            CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before);
        }
    }
    ClientConnection client(UbiquittyTest::syncClientPolicy, {});
    const std::array<std::uint8_t, 3> wrongDirection { std::to_underlying(FrameKind::Greet), 0, 1 };
    const std::size_t before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        client.receiveStream(wrongDirection, 1);
    }
    CHECK(client.byeReason() == std::optional { ByeReason::Malformed });
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before);
}

TEST(liveInnerReadersRefuseAnAllocationWithoutReceivingItsBody)
{
    const std::array<std::uint8_t, 5> hostHeader { std::to_underlying(FrameKind::Greet), 0, 0xFF, 0xFF, 3 };
    const std::array<std::uint8_t, 5> clientHeader { std::to_underlying(FrameKind::Welcome), 0, 0xFF, 0xFF, 3 };
    HostConnection host(UbiquittyTest::syncHostPolicy(0), {});
    ClientConnection client(UbiquittyTest::syncClientPolicy, {});
    const std::size_t before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(maxFrameBodyBytes);
        host.receiveStream(hostHeader, 1);
        client.receiveStream(clientHeader, 1);
    }
    CHECK(host.byeReason() == std::optional { ByeReason::TransferRefused });
    CHECK(client.byeReason() == std::optional { ByeReason::TransferRefused });
    CHECK_EQ(host.inboundFrameBytes(), 0u);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 2);
}

TEST(aRealGreetChangesAdmissionForTheFollowingFrameInOneRead)
{
    Greet greet;
    greet.wireVersion = wireVersion;
    Facts facts;
    facts.viewportRows = 37;
    facts.viewportColumns = 91;
    std::vector<std::uint8_t> body;
    encode(body, greet);
    std::vector<std::uint8_t> bytes;
    REQUIRE(appendFrame(bytes, FrameKind::Greet, 0, body));
    body.clear();
    encode(body, facts);
    REQUIRE(appendFrame(bytes, FrameKind::Facts, 0, body));
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        HostConnection host(UbiquittyTest::syncHostPolicy(0), servingIdentity());
        host.receiveStream(std::span { bytes }.first(split), 1);
        host.receiveStream(std::span { bytes }.subspan(split), 2);
        CHECK(host.state() == ConnectionState::Attached);
        CHECK_EQ(host.facts().viewportRows, 37u);
        CHECK_EQ(host.facts().viewportColumns, 91u);
        CHECK_EQ(host.pendingInboundFrameBytes(), 0u);
        CHECK(host.inboundFrameBytes() <= maxFrameBodyBytes);
    }
}

TEST(aCarrierAdmitsEachLengthOnlyAfterThePreviousDelivery)
{
    constexpr std::size_t afterWelcomeBytes = 257;
    FrameIngress ingress(maxFrameBodyBytes);
    std::vector<std::uint8_t> bytes;
    REQUIRE(appendFrame(bytes, FrameKind::Welcome, 0, {}));
    const std::vector<std::uint8_t> body(afterWelcomeBytes, 0xAA);
    REQUIRE(appendFrame(bytes, FrameKind::Notify, 0, body));
    bool isWelcomed = false;
    unsigned deliveries = 0;
    const auto result = ingress.consume(bytes, [&](std::span<const std::uint8_t> prefix) {
        FrameHeader header;
        EnvelopeError error = EnvelopeError::None;
        const auto state = readFrameHeader(prefix, header, error);
        if (state == FrameHeaderState::NeedsMore)
            return FrameHeaderDecision {};
        if (state == FrameHeaderState::Failed || (!isWelcomed && header.bodyBytes > 1))
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        return FrameHeaderDecision { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t>, std::span<const std::uint8_t> received) {
        ++deliveries;
        if (!isWelcomed) {
            CHECK_EQ(received.size(), 0u);
            CHECK_EQ(ingress.retainedBytes(), 0u);
            isWelcomed = true;
        } else {
            CHECK_EQ(received.size(), afterWelcomeBytes);
        }
        return FrameDelivery::Continue; });
    CHECK(result.state == FrameIngress::State::Accepting);
    CHECK_EQ(result.consumedBytes, bytes.size());
    CHECK_EQ(deliveries, 2u);
    CHECK_EQ(ingress.retainedBytes(), afterWelcomeBytes);
    ingress.releaseRetained();
    CHECK_EQ(ingress.retainedBytes(), 0u);
}

TEST(aHostCapacityGateRunsBeforeTheAllocatorAndDoesNotAffectANeighbor)
{
    HostConnection refused(UbiquittyTest::syncHostPolicy(0), {});
    HostConnection neighbor(UbiquittyTest::syncHostPolicy(0), servingIdentity());
    std::size_t asked = 0;
    refused.setInboundFrameAdmission([&](std::size_t capacity) {
        asked = capacity;
        return false;
    });
    const std::array<std::uint8_t, 5> header { std::to_underlying(FrameKind::Greet), 0, 0xFF, 0xFF, 3 };
    const std::size_t before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        refused.receiveStream(header, 1);
    }
    CHECK_EQ(asked, maxFrameBodyBytes);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before);
    CHECK(refused.byeReason() == std::optional { ByeReason::TransferRefused });
    std::vector<std::uint8_t> body;
    Greet greet;
    greet.wireVersion = wireVersion;
    encode(body, greet);
    std::vector<std::uint8_t> bytes;
    REQUIRE(appendFrame(bytes, FrameKind::Greet, 0, body));
    neighbor.receiveStream(bytes, 1);
    CHECK(neighbor.state() == ConnectionState::Attached);
}

UBIQUITTY_TEST_MAIN()
