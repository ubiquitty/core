// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/FrameIngress.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

// Only the input-sized nothrow allocation is injectable. Granted storage
// still comes from the ordinary array allocator, paired with plain delete[].
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }

namespace {

// A fixed five-byte test header exercises both short and maximum body lengths
// without giving the ingress a second implementation of the Sync grammar.
std::vector<std::uint8_t> header(std::uint8_t kind, std::uint32_t length)
{
    return { kind, static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24) };
}

FrameHeaderDecision readHeader(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 5)
        return {};
    const std::size_t length = static_cast<std::uint32_t>(bytes[1]) | (static_cast<std::uint32_t>(bytes[2]) << 8)
        | (static_cast<std::uint32_t>(bytes[3]) << 16) | (static_cast<std::uint32_t>(bytes[4]) << 24);
    return { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = length };
}

} // namespace

TEST(anOversizedHeaderIsRefusedBeforeAnyBodyAllocation)
{
    const auto oversized = header(1, maxFrameBodyBytes + 1);
    for (std::size_t split = 0; split <= oversized.size(); ++split) {
        FrameIngress ingress(maxFrameBodyBytes);
        int deliveries = 0;
        const auto deliver = [&](auto, auto) {
            ++deliveries;
            return FrameDelivery::Continue;
        };
        const auto first = ingress.consume(std::span { oversized }.first(split), readHeader, deliver);
        CHECK_EQ(first.consumedBytes, split);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        if (split < oversized.size()) {
            CHECK(first.state == FrameIngress::State::Accepting);
            const auto rest = ingress.consume(std::span { oversized }.subspan(split), readHeader, deliver);
            CHECK_EQ(rest.consumedBytes, oversized.size() - split);
            CHECK(rest.state == FrameIngress::State::Refused);
        } else {
            CHECK(first.state == FrameIngress::State::Refused);
        }
        CHECK(ingress.error() == FrameIngressError::Oversize);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(deliveries, 0);
    }
}

TEST(aHeaderWithAnOversizedBodyInTheSameSpanConsumesOnlyTheHeader)
{
    auto bytes = header(1, maxFrameBodyBytes + 1);
    bytes.resize(bytes.size() + maxFrameBodyBytes + 1, 0xAB);
    FrameIngress ingress(maxFrameBodyBytes);
    int deliveries = 0;
    const auto result = ingress.consume(bytes, readHeader, [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Continue;
    });
    CHECK(result.state == FrameIngress::State::Refused);
    CHECK_EQ(result.consumedBytes, 5u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(deliveries, 0);
}

TEST(aFrameAtTheBoundNeverRetainsMoreThanOneBody)
{
    FrameIngress ingress(maxFrameBodyBytes);
    const auto prefix = header(1, maxFrameBodyBytes);
    int deliveries = 0;
    const auto deliver = [&](auto receivedHeader, auto body) {
        ++deliveries;
        CHECK(std::ranges::equal(receivedHeader, prefix));
        CHECK_EQ(body.size(), maxFrameBodyBytes);
        CHECK(std::ranges::all_of(body, [](auto byte) { return byte == 0xAB; }));
        return FrameDelivery::Continue;
    };
    for (const auto byte : prefix) {
        const std::array piece { byte };
        CHECK_EQ(ingress.consume(piece, readHeader, deliver).consumedBytes, 1u);
        CHECK(ingress.retainedBytes() <= maxFrameBodyBytes);
    }
    CHECK_EQ(ingress.retainedBytes(), maxFrameBodyBytes);
    const std::vector<std::uint8_t> body(maxFrameBodyBytes, 0xAB);
    const auto first = ingress.consume(std::span { body }.first(body.size() - 1), readHeader, deliver);
    CHECK_EQ(first.consumedBytes, body.size() - 1);
    CHECK_EQ(deliveries, 0);
    CHECK_EQ(ingress.pendingBytes(), prefix.size() + body.size() - 1);
    CHECK_EQ(ingress.consume(std::span { body }.last(1), readHeader, deliver).consumedBytes, 1u);
    CHECK_EQ(deliveries, 1);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.retainedBytes(), maxFrameBodyBytes);
    ingress.reset();
    CHECK_EQ(ingress.retainedBytes(), 0u);
}

TEST(coalescedFramesAreDeliveredBeforeTheNextHeaderIsAdmitted)
{
    // This test-owned synthetic grammar demonstrates a policy transition;
    // it is not a transport accepted by either canonical connection reader.
    constexpr std::uint32_t largeBody = 2 * maxFrameBodyBytes;
    auto bytes = header(1, 0); // the greeting is complete at its header
    const auto bulk = header(2, largeBody);
    bytes.insert(bytes.end(), bulk.begin(), bulk.end());
    bytes.resize(bytes.size() + largeBody, 0xCD);
    FrameIngress ingress(largeBody);
    bool isWelcomed = false;
    int deliveries = 0;
    const auto result = ingress.consume(bytes, [&](auto receivedHeader) {
        const auto decision = readHeader(receivedHeader);
        if (decision.state == FrameHeaderDecision::State::Admitted && !isWelcomed && decision.bodyBytes > maxFrameBodyBytes)
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        return decision; }, [&](auto receivedHeader, auto body) {
        ++deliveries;
        if (receivedHeader.front() == 1) {
            CHECK(body.empty());
            isWelcomed = true;
        } else {
            CHECK(isWelcomed);
            CHECK_EQ(body.size(), largeBody);
            CHECK(std::ranges::all_of(body, [](auto byte) { return byte == 0xCD; }));
        }
        CHECK(ingress.retainedBytes() <= largeBody);
        return FrameDelivery::Continue; });
    CHECK(result.state == FrameIngress::State::Accepting);
    CHECK_EQ(result.consumedBytes, bytes.size());
    CHECK_EQ(deliveries, 2);
    CHECK_EQ(ingress.pendingBytes(), 0u);
}

TEST(manyCoalescedFramesDoNotAccumulateRetainedBodies)
{
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t index = 0; index < 1000; ++index) {
        const auto prefix = header(1, 1);
        bytes.insert(bytes.end(), prefix.begin(), prefix.end());
        bytes.push_back(static_cast<std::uint8_t>(index));
    }
    FrameIngress ingress(maxFrameBodyBytes);
    std::size_t deliveries = 0;
    const auto result = ingress.consume(bytes, readHeader, [&](auto, auto body) {
        CHECK_EQ(body.size(), 1u);
        CHECK_EQ(body.front(), static_cast<std::uint8_t>(deliveries++));
        CHECK_EQ(ingress.retainedBytes(), 1u);
        return FrameDelivery::Continue;
    });
    CHECK_EQ(result.consumedBytes, bytes.size());
    CHECK_EQ(deliveries, 1000u);
    CHECK_EQ(ingress.retainedBytes(), 1u);
}

TEST(aTransportCanRefuseAFrameKindWithoutAllocatingItsBody)
{
    FrameIngress ingress(maxFrameBodyBytes);
    auto bytes = header(99, maxFrameBodyBytes);
    bytes.resize(bytes.size() + maxFrameBodyBytes, 0xAB);
    int deliveries = 0;
    const auto result = ingress.consume(bytes, [](auto receivedHeader) {
        if (receivedHeader.front() == 99)
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        return readHeader(receivedHeader); }, [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Continue; });
    CHECK(result.state == FrameIngress::State::Refused);
    CHECK_EQ(result.consumedBytes, 1u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK(ingress.error() == FrameIngressError::HeaderRefused);
    CHECK_EQ(deliveries, 0);
}

TEST(aStoppedDeliveryNamesTheExactUnconsumedSuffix)
{
    auto bytes = header(1, 1);
    bytes.push_back(0xAB);
    const auto next = header(2, 0);
    bytes.insert(bytes.end(), next.begin(), next.end());
    FrameIngress ingress(maxFrameBodyBytes);
    int deliveries = 0;
    const auto deliver = [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Stop;
    };
    const auto result = ingress.consume(bytes, readHeader, deliver);
    CHECK(result.state == FrameIngress::State::Stopped);
    CHECK_EQ(result.consumedBytes, 6u);
    CHECK_EQ(deliveries, 1);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.consume(std::span { bytes }.subspan(result.consumedBytes), readHeader, deliver).consumedBytes, 0u);
    ingress.reset();
    CHECK_EQ(ingress.consume(std::span { bytes }.subspan(result.consumedBytes), readHeader, deliver).consumedBytes, next.size());
    CHECK_EQ(deliveries, 2);
}

TEST(aHeaderThatNeverEndsUsesOnlyTheFixedHeaderStorage)
{
    FrameIngress ingress(maxFrameBodyBytes);
    const std::array<std::uint8_t, maxEnvelopeHeaderBytes + 1> bytes {};
    int deliveries = 0;
    const auto result = ingress.consume(bytes, [](auto) { return FrameHeaderDecision {}; }, [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Continue; });
    CHECK(result.state == FrameIngress::State::Refused);
    CHECK_EQ(result.consumedBytes, maxEnvelopeHeaderBytes);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK(ingress.error() == FrameIngressError::HeaderTooLong);
    CHECK_EQ(deliveries, 0);
}

TEST(aRefusedHeaderReleasesThePreviousBodyAndStaysRefusedUntilReset)
{
    FrameIngress ingress(maxFrameBodyBytes);
    auto first = header(1, 32);
    first.resize(first.size() + 32, 0xAB);
    int deliveries = 0;
    const auto deliver = [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Continue;
    };
    CHECK_EQ(ingress.consume(first, readHeader, deliver).consumedBytes, first.size());
    CHECK_EQ(ingress.retainedBytes(), 32u);
    const auto bad = header(1, maxFrameBodyBytes + 1);
    CHECK(ingress.consume(bad, readHeader, deliver).state == FrameIngress::State::Refused);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.consume(first, readHeader, deliver).consumedBytes, 0u);
    CHECK_EQ(deliveries, 1);
    ingress.reset();
    CHECK(ingress.error() == FrameIngressError::None);
    CHECK_EQ(ingress.consume(first, readHeader, deliver).consumedBytes, first.size());
    CHECK_EQ(deliveries, 2);
}

TEST(aFailedBodyAllocationConsumesOnlyTheCompleteHeader)
{
    auto bytes = header(1, 32);
    bytes.resize(bytes.size() + 32, 0xAB);
    for (std::size_t split = 0; split < 5; ++split) {
        FrameIngress ingress(maxFrameBodyBytes);
        int deliveries = 0;
        const auto deliver = [&](auto, auto body) {
            ++deliveries;
            CHECK_EQ(body.size(), 32u);
            CHECK(std::ranges::all_of(body, [](auto byte) { return byte == 0xAB; }));
            return FrameDelivery::Continue;
        };
        CHECK_EQ(ingress.consume(std::span { bytes }.first(split), readHeader, deliver).consumedBytes, split);
        const std::size_t refusalsBefore = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        const auto result = [&] {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
            return ingress.consume(std::span { bytes }.subspan(split), readHeader, deliver);
        }();
        CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusalsBefore + 1);
        CHECK(result.state == FrameIngress::State::Refused);
        CHECK(ingress.error() == FrameIngressError::AllocationFailed);
        CHECK_EQ(result.consumedBytes, 5u - split);
        CHECK_EQ(ingress.pendingBytes(), 0u);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(deliveries, 0);
        CHECK_EQ(ingress.consume(bytes, readHeader, deliver).consumedBytes, 0u);
        ingress.reset();
        CHECK(ingress.error() == FrameIngressError::None);
        CHECK_EQ(ingress.consume(bytes, readHeader, deliver).consumedBytes, bytes.size());
        CHECK_EQ(deliveries, 1);
    }
}

TEST(aFailedBodyGrowReleasesTheOldAllocationWithoutAffectingAnotherIngress)
{
    FrameIngress ingress(maxFrameBodyBytes);
    auto first = header(1, 32);
    first.resize(first.size() + 32, 0xAB);
    int deliveries = 0;
    const auto deliver = [&](auto, auto) {
        ++deliveries;
        return FrameDelivery::Continue;
    };
    CHECK_EQ(ingress.consume(first, readHeader, deliver).consumedBytes, first.size());
    CHECK_EQ(ingress.retainedBytes(), 32u);
    auto larger = header(1, 64);
    larger.resize(larger.size() + 64, 0xCD);
    const std::size_t refusalsBefore = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    const auto result = [&] {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        return ingress.consume(larger, readHeader, deliver);
    }();
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusalsBefore + 1);
    CHECK(result.state == FrameIngress::State::Refused);
    CHECK(ingress.error() == FrameIngressError::AllocationFailed);
    CHECK_EQ(result.consumedBytes, 5u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(deliveries, 1);
    FrameIngress neighbor(maxFrameBodyBytes);
    CHECK_EQ(neighbor.consume(first, readHeader, deliver).consumedBytes, first.size());
    CHECK_EQ(deliveries, 2);
    ingress.reset();
    CHECK_EQ(ingress.consume(larger, readHeader, deliver).consumedBytes, larger.size());
    CHECK_EQ(ingress.retainedBytes(), 64u);
    CHECK_EQ(deliveries, 3);
}

TEST(anEmptyBodyNeedsNoAllocation)
{
    const auto bytes = header(1, 0);
    FrameIngress ingress(maxFrameBodyBytes);
    std::size_t deliveries = 0;
    const auto refusalsBefore = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    const auto result = ingress.consume(bytes, readHeader, [&](auto, auto body) {
        CHECK(body.empty());
        ++deliveries;
        return FrameDelivery::Continue;
    });
    CHECK(result.state == FrameIngress::State::Accepting);
    CHECK_EQ(result.consumedBytes, bytes.size());
    CHECK_EQ(deliveries, 1u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusalsBefore);
}

TEST(resetDiscardsAPartialHeaderOrBody)
{
    auto bytes = header(1, 4);
    bytes.resize(bytes.size() + 4, 0xAB);
    for (const std::size_t split : { 3u, 7u }) {
        FrameIngress ingress(maxFrameBodyBytes);
        std::size_t deliveries = 0;
        const auto deliver = [&](auto, auto body) {
            CHECK_EQ(body.size(), 4u);
            CHECK(std::ranges::all_of(body, [](auto byte) { return byte == 0xAB; }));
            ++deliveries;
            return FrameDelivery::Continue;
        };
        const auto partial = ingress.consume(std::span { bytes }.first(split), readHeader, deliver);
        CHECK(partial.state == FrameIngress::State::Accepting);
        CHECK_EQ(ingress.pendingBytes(), split);
        CHECK_EQ(deliveries, 0u);
        ingress.reset();
        CHECK_EQ(ingress.pendingBytes(), 0u);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK(ingress.error() == FrameIngressError::None);
        CHECK_EQ(ingress.consume(bytes, readHeader, deliver).consumedBytes, bytes.size());
        CHECK_EQ(deliveries, 1u);
    }
}

UBIQUITTY_TEST_MAIN()
