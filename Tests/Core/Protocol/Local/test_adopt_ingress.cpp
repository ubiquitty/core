// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/AdoptIngress.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "StreamPartitions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

// Only the ingress's input-sized nothrow array allocation is injectable.
// Successful storage uses the ordinary allocator and plain delete[].
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

namespace Local = Ubiquitty::Protocol::Local;
namespace Sync = Ubiquitty::Protocol::Sync;
using AllocationRefusal = UbiquittyTest::ScopedNothrowAllocationRefusal;

Local::AdoptWireOperation operation()
{
    return { .operationID = { .low = 11, .high = 13 }, .daemonInstanceID = 17 };
}

std::vector<std::uint8_t> prepareRecord(std::size_t bodyBytes)
{
    std::vector<std::uint8_t> body(bodyBytes);
    for (std::size_t index = 0; index < body.size(); ++index)
        body[index] = static_cast<std::uint8_t>(index * 37);
    Local::AdoptWirePrepare prepare { .correlation = 19,
        .preparation = { .operation = operation(), .bodyBytes = body.size() },
        .terminalBody = body };
    Ubiquitty::Crypto::sha256(body, prepare.preparation.bodySHA256);
    const auto encoded = Local::encodeAdoptWirePrepare(prepare, Local::adoptWirePrepareHeaderBytes + body.size());
    CHECK(encoded.has_value());
    if (!encoded)
        return {};
    std::vector<std::uint8_t> record(encoded->header.begin(), encoded->header.end());
    record.insert(record.end(), encoded->terminalBody.begin(), encoded->terminalBody.end());
    return record;
}

std::vector<std::uint8_t> commandRecord(Local::AdoptWireMessage kind = Local::AdoptWireMessage::Status)
{
    const auto encoded = Local::encodeAdoptWireCommand({ .kind = kind, .correlation = 23, .operation = operation() });
    CHECK(encoded.has_value());
    if (!encoded)
        return {};
    return { encoded->begin(), encoded->end() };
}

} // namespace

TEST(adoptIngressDeliversWholeCanonicalRecordsAcrossEveryFragmentStride)
{
    std::vector<std::vector<std::uint8_t>> records { prepareRecord(7), commandRecord(Local::AdoptWireMessage::Commit),
        commandRecord(Local::AdoptWireMessage::Abort), commandRecord() };
    const auto outcome = Local::encodeAdoptWireOutcome({ .correlation = 29, .operation = operation(), .disposition = Local::AdoptWireDisposition::Committed, .target = { .sessionID = 31, .paneID = { .low = 37, .high = 41 } } });
    const auto refused = Local::encodeAdoptWireRefused({ .correlation = 43, .operation = operation(), .reason = Local::AdoptWireRefusal::Busy });
    REQUIRE(outcome.has_value());
    REQUIRE(refused.has_value());
    records.emplace_back(outcome->begin(), outcome->end());
    records.emplace_back(refused->begin(), refused->end());
    std::vector<std::uint8_t> bytes;
    for (const auto& record : records) {
        REQUIRE(!record.empty());
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    constexpr std::size_t maxRecordBytes = 100;
    for (std::size_t stride = 1; stride <= bytes.size(); ++stride) {
        Local::AdoptIngress ingress(maxRecordBytes);
        std::size_t delivered = 0;
        std::size_t admitted = 0;
        for (std::size_t start = 0; start < bytes.size(); start += stride) {
            const auto piece = std::span(bytes).subspan(start, std::min(stride, bytes.size() - start));
            const auto result = ingress.consume(piece, [&](auto kind, auto bodyBytes, auto requiredCapacity) {
                CHECK_EQ(admitted, delivered);
                CHECK(delivered < records.size());
                if (delivered >= records.size())
                    return false;
                ++admitted;
                CHECK_EQ(static_cast<std::uint8_t>(kind), records[delivered].front());
                CHECK_EQ(bodyBytes + Local::adoptWireFrameHeaderBytes, records[delivered].size());
                CHECK(requiredCapacity >= records[delivered].size());
                CHECK(requiredCapacity <= maxRecordBytes);
                return true; }, [&](Local::AdoptIngress::Frame frame) {
                CHECK(delivered < records.size());
                if (delivered >= records.size())
                    return Sync::FrameDelivery::Stop;
                CHECK(std::ranges::equal(frame.record, records[delivered]));
                CHECK_EQ(static_cast<std::uint8_t>(frame.kind), frame.record.front());
                if (frame.kind == Local::AdoptWireMessage::Prepare) {
                    const auto decoded = Local::decodeAdoptWirePrepare(frame.record, maxRecordBytes);
                    CHECK(decoded.has_value());
                    if (decoded) {
                        CHECK(decoded->terminalBody.data() == frame.record.data() + Local::adoptWirePrepareHeaderBytes);
                        CHECK_EQ(decoded->terminalBody.size(), 7u);
                    }
                } else if (frame.kind == Local::AdoptWireMessage::Outcome)
                    CHECK(Local::decodeAdoptWireOutcome(frame.record).has_value());
                else if (frame.kind == Local::AdoptWireMessage::Refused)
                    CHECK(Local::decodeAdoptWireRefused(frame.record).has_value());
                else
                    CHECK(Local::decodeAdoptWireCommand(frame.record).has_value());
                ++delivered;
                return Sync::FrameDelivery::Continue; });
            CHECK_EQ(result.consumedBytes, piece.size());
            CHECK(result.state == Sync::FrameIngress::State::Accepting);
            CHECK(ingress.retainedBytes() <= maxRecordBytes);
        }
        CHECK_EQ(delivered, records.size());
        CHECK_EQ(admitted, records.size());
        CHECK_EQ(ingress.pendingBytes(), 0u);
        ingress.releaseRetained();
        CHECK_EQ(ingress.retainedBytes(), 0u);
    }
}

TEST(adoptIngressAdmitsTheCompleteHeaderBeforeAllocationOrBodyRetention)
{
    const auto bytes = prepareRecord(17);
    REQUIRE(!bytes.empty());
    Local::AdoptIngress ingress(bytes.size());
    const auto attempts = AllocationRefusal::attemptCount();
    std::size_t admissions = 0;
    std::size_t deliveries = 0;
    const auto admit = [&](auto kind, auto bodyBytes, auto requiredCapacity) {
        ++admissions;
        CHECK(kind == Local::AdoptWireMessage::Prepare);
        CHECK_EQ(bodyBytes, bytes.size() - Local::adoptWireFrameHeaderBytes);
        CHECK_EQ(requiredCapacity, bytes.size());
        CHECK_EQ(ingress.pendingBytes(), Local::adoptWireFrameHeaderBytes);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts);
        return true;
    };
    const auto deliver = [&](auto) {
        ++deliveries;
        return Sync::FrameDelivery::Continue;
    };
    CHECK_EQ(ingress.readCapacity(1000), 1u);
    CHECK_EQ(ingress.readCapacity(0), 0u);
    for (std::size_t index = 0; index < Local::adoptWireFrameHeaderBytes; ++index) {
        const auto result = ingress.consume(std::span(bytes).subspan(index, 1), admit, deliver);
        CHECK_EQ(result.consumedBytes, 1u);
        CHECK(result.state == Sync::FrameIngress::State::Accepting);
        CHECK_EQ(ingress.pendingBytes(), index + 1);
        if (index + 1 < Local::adoptWireFrameHeaderBytes) {
            CHECK_EQ(admissions, 0u);
            CHECK_EQ(ingress.retainedBytes(), 0u);
            CHECK_EQ(ingress.readCapacity(1000), Local::adoptWireFrameHeaderBytes - index - 1);
        }
        ingress.releaseRetained();
        CHECK_EQ(ingress.pendingBytes(), index + 1);
    }
    CHECK_EQ(admissions, 1u);
    CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
    CHECK_EQ(ingress.retainedBytes(), bytes.size());
    CHECK_EQ(ingress.readCapacity(1000), bytes.size() - Local::adoptWireFrameHeaderBytes);
    const auto body = std::span(bytes).subspan(Local::adoptWireFrameHeaderBytes);
    CHECK_EQ(ingress.consume(body.first(body.size() - 1), admit, deliver).consumedBytes, body.size() - 1);
    CHECK_EQ(ingress.readCapacity(1000), 1u);
    CHECK_EQ(deliveries, 0u);
    CHECK_EQ(ingress.consume(body.last(1), admit, deliver).consumedBytes, 1u);
    CHECK_EQ(deliveries, 1u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.readCapacity(1000), 1u);
    ingress.releaseRetained();
    CHECK_EQ(ingress.retainedBytes(), 0u);
}

TEST(adoptIngressRejectsMalformedAndOversizedHeadersBeforeAdmission)
{
    const std::array headers {
        std::array<std::uint8_t, 5> { 0x7F, 32, 0, 0, 0 },
        std::array<std::uint8_t, 5> { 2, 31, 0, 0, 0 },
        std::array<std::uint8_t, 5> { 1, 72, 0, 0, 0 },
        std::array<std::uint8_t, 5> { 1, 0xFF, 0xFF, 0xFF, 0xFF },
    };
    for (const auto& header : headers) {
        const auto decoded = Local::decodeAdoptWireHeader(header, 100);
        REQUIRE(!decoded.has_value());
        std::vector<std::uint8_t> bytes(header.begin(), header.end());
        bytes.resize(200, 0xA5);
        Local::AdoptIngress ingress(100);
        const auto attempts = AllocationRefusal::attemptCount();
        bool wasAdmitted = false;
        bool wasDelivered = false;
        const auto admit = [&](auto, auto, auto) { wasAdmitted = true; return true; };
        const auto deliver = [&](auto) { wasDelivered = true; return Sync::FrameDelivery::Continue; };
        const auto result = ingress.consume(bytes, admit, deliver);
        CHECK_EQ(result.consumedBytes, Local::adoptWireFrameHeaderBytes);
        CHECK(result.state == Sync::FrameIngress::State::Refused);
        CHECK(ingress.faulted());
        CHECK(!ingress.isAccepting());
        CHECK(ingress.error() == (decoded.error() == Local::AdoptWireError::TooLong ? Sync::FrameIngressError::Oversize : Sync::FrameIngressError::HeaderRefused));
        CHECK(!wasAdmitted);
        CHECK(!wasDelivered);
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(ingress.pendingBytes(), 0u);
        CHECK_EQ(ingress.readCapacity(1000), 0u);
        CHECK_EQ(ingress.consume(bytes, admit, deliver).consumedBytes, 0u);
        ingress.releaseRetained();
        CHECK(ingress.faulted());
    }
}

TEST(adoptIngressRefusesTheWholeRecordCeilingIncludingItsHeader)
{
    const auto bytes = commandRecord();
    REQUIRE(!bytes.empty());
    for (const auto allowance : std::array<std::size_t, 4> { 0, 4, bytes.size() - 1, std::numeric_limits<std::size_t>::max() }) {
        Local::AdoptIngress ingress(allowance);
        const bool fits = allowance >= bytes.size();
        bool wasAdmitted = false;
        const auto result = ingress.consume(bytes, [&](auto, auto, auto) { wasAdmitted = true; return true; }, [](auto) { return Sync::FrameDelivery::Continue; });
        CHECK_EQ(wasAdmitted, fits);
        CHECK_EQ(result.consumedBytes, fits ? bytes.size() : Local::adoptWireFrameHeaderBytes);
        CHECK(result.state == (fits ? Sync::FrameIngress::State::Accepting : Sync::FrameIngress::State::Refused));
        if (!fits)
            CHECK(ingress.error() == Sync::FrameIngressError::Oversize);
    }
}

TEST(adoptIngressAccountsForIdleCapacityBeforeReuseAndGrowth)
{
    const auto large = prepareRecord(32);
    const auto small = commandRecord();
    const auto larger = prepareRecord(64);
    REQUIRE(!large.empty());
    REQUIRE(!small.empty());
    REQUIRE(!larger.empty());
    Local::AdoptIngress ingress(larger.size());
    const auto attempts = AllocationRefusal::attemptCount();
    const auto deliver = [](auto) { return Sync::FrameDelivery::Continue; };
    CHECK_EQ(ingress.consume(large, [&](auto, auto, auto capacity) {
        CHECK_EQ(capacity, large.size());
        return true; }, deliver).consumedBytes, large.size());
    CHECK_EQ(ingress.retainedBytes(), large.size());
    CHECK_EQ(ingress.consume(small, [&](auto, auto, auto capacity) {
        CHECK_EQ(capacity, large.size());
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
        return true; }, deliver).consumedBytes, small.size());
    CHECK_EQ(ingress.retainedBytes(), large.size());
    CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
    CHECK_EQ(ingress.consume(larger, [&](auto, auto, auto capacity) {
        CHECK_EQ(capacity, larger.size());
        CHECK_EQ(ingress.retainedBytes(), large.size());
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
        return true; }, deliver).consumedBytes, larger.size());
    CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 2);
    CHECK_EQ(ingress.retainedBytes(), larger.size());
    ingress.releaseRetained();
    CHECK_EQ(ingress.retainedBytes(), 0u);
}

TEST(adoptIngressPolicyRefusalReleasesIdleStorageAndConsumesNoBody)
{
    const auto bytes = commandRecord();
    REQUIRE(!bytes.empty());
    Local::AdoptIngress ingress(bytes.size());
    const auto deliver = [](auto) { return Sync::FrameDelivery::Continue; };
    CHECK_EQ(ingress.consume(bytes, [](auto, auto, auto) { return true; }, deliver).consumedBytes, bytes.size());
    const auto attempts = AllocationRefusal::attemptCount();
    const auto result = ingress.consume(bytes, [&](auto, auto, auto capacity) {
        CHECK_EQ(capacity, bytes.size());
        CHECK_EQ(ingress.pendingBytes(), Local::adoptWireFrameHeaderBytes);
        CHECK_EQ(ingress.retainedBytes(), bytes.size());
        return false; }, deliver);
    CHECK_EQ(result.consumedBytes, Local::adoptWireFrameHeaderBytes);
    CHECK(result.state == Sync::FrameIngress::State::Refused);
    CHECK(ingress.error() == Sync::FrameIngressError::HeaderRefused);
    CHECK_EQ(AllocationRefusal::attemptCount(), attempts);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.consume(bytes, [](auto, auto, auto) { return true; }, deliver).consumedBytes, 0u);
}

TEST(adoptIngressStopPreservesTheExactCoalescedSuffixAndStaysStopped)
{
    const auto first = commandRecord();
    const auto second = prepareRecord(7);
    REQUIRE(!first.empty());
    REQUIRE(!second.empty());
    auto bytes = first;
    bytes.insert(bytes.end(), second.begin(), second.end());
    Local::AdoptIngress ingress(second.size());
    CHECK(ingress.isAccepting());
    std::size_t admissions = 0;
    std::size_t deliveries = 0;
    const auto admit = [&](auto, auto, auto) { ++admissions; return true; };
    const auto deliver = [&](Local::AdoptIngress::Frame frame) {
        ++deliveries;
        CHECK(std::ranges::equal(frame.record, first));
        return Sync::FrameDelivery::Stop;
    };
    const auto result = ingress.consume(bytes, admit, deliver);
    CHECK_EQ(result.consumedBytes, first.size());
    CHECK(result.state == Sync::FrameIngress::State::Stopped);
    CHECK_EQ(admissions, 1u);
    CHECK_EQ(deliveries, 1u);
    CHECK(!ingress.faulted());
    // A Stop ends the exchange as a refusal does: nothing more is accepted.
    CHECK(!ingress.isAccepting());
    CHECK(ingress.error() == Sync::FrameIngressError::None);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.retainedBytes(), 0u);
    CHECK_EQ(ingress.readCapacity(1000), 0u);
    ingress.releaseRetained();
    CHECK_EQ(ingress.consume(std::span(bytes).subspan(result.consumedBytes), admit, deliver).consumedBytes, 0u);
    CHECK_EQ(admissions, 1u);
    CHECK_EQ(deliveries, 1u);
}

TEST(adoptIngressAllocationFailureReleasesAnyOldOwnerAndStaysRefused)
{
    const auto first = commandRecord();
    const auto next = prepareRecord(32);
    REQUIRE(!first.empty());
    REQUIRE(!next.empty());
    for (const bool hasPreviousRecord : { false, true }) {
        Local::AdoptIngress ingress(next.size());
        std::size_t deliveries = 0;
        const auto admit = [](auto, auto, auto) { return true; };
        const auto deliver = [&](auto) { ++deliveries; return Sync::FrameDelivery::Continue; };
        if (hasPreviousRecord)
            CHECK_EQ(ingress.consume(first, admit, deliver).consumedBytes, first.size());
        const auto attempts = AllocationRefusal::attemptCount();
        const auto refusals = AllocationRefusal::refusalCount();
        {
            const AllocationRefusal refuse(next.size());
            const auto result = ingress.consume(next, admit, deliver);
            CHECK_EQ(result.consumedBytes, Local::adoptWireFrameHeaderBytes);
            CHECK(result.state == Sync::FrameIngress::State::Refused);
        }
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
        CHECK_EQ(AllocationRefusal::refusalCount(), refusals + 1);
        CHECK_EQ(deliveries, hasPreviousRecord ? 1u : 0u);
        CHECK(ingress.faulted());
        CHECK(ingress.error() == Sync::FrameIngressError::AllocationFailed);
        CHECK_EQ(ingress.pendingBytes(), 0u);
        CHECK_EQ(ingress.retainedBytes(), 0u);
        CHECK_EQ(ingress.readCapacity(1000), 0u);
        CHECK_EQ(ingress.consume(next, admit, deliver).consumedBytes, 0u);
        CHECK_EQ(AllocationRefusal::attemptCount(), attempts + 1);
    }
}

TEST(adoptRecordsKeepTheirContentsAcrossAllCutsAndSeededPartitions)
{
    for (const std::size_t bodyBytes : { 7u, 65535u, 65536u, 65537u }) {
        const std::array records { prepareRecord(bodyBytes), commandRecord() };
        std::vector<std::uint8_t> stream;
        for (const auto& record : records)
            stream.insert(stream.end(), record.begin(), record.end());
        CHECK(UbiquittyTest::sweepStreamPartitions(stream.size(), bodyBytes == 7 ? UbiquittyTest::PartitionCoverage::Exhaustive : UbiquittyTest::PartitionCoverage::Boundaries, [&](auto ends, const std::string& label) {
            const auto allowance = records.front().size();
            Local::AdoptIngress ingress(allowance);
            std::vector<std::vector<std::uint8_t>> actual;
            bool isValid = true;
            UbiquittyTest::feedStreamPartition(std::span<const std::uint8_t>(stream), ends, [&](auto piece) {
                const auto result = ingress.consume(piece, [&](auto, auto, auto capacity) { return capacity <= allowance; }, [&](auto frame) {
                    actual.emplace_back(frame.record.begin(), frame.record.end());
                    return Sync::FrameDelivery::Continue; });
                isValid &= result.consumedBytes == piece.size() && result.state == Sync::FrameIngress::State::Accepting;
                isValid &= ingress.retainedBytes() <= allowance;
            });
            const bool matches = isValid && !ingress.pendingBytes() && std::ranges::equal(actual, records);
            CHECK_MSG(matches, ("body=" + std::to_string(bodyBytes) + " " + label).c_str());
            return matches;
        }));
    }
}

UBIQUITTY_TEST_MAIN()
