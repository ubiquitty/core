// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_pacer.cpp — one attachment's window and round trip.
//
// The round trip subtracts the time the client held its `Ack`, so coalescing
// acks never inflates it. The window is the floor until a fresh round trip and
// a delivery rate exist, grows with what was delivered, falls back to the
// floor when the round trip goes stale, and never admits bytes past itself
// unless nothing is outstanding. Datagrams leave the window when acknowledged
// or when the loss timeout counts them lost.

#include "Harness.h"

#include "Core/Protocol/Sync/Pacer.h"

#include <cstdint>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

/// The provisional constants Docs/sync.md §6.4 names, as test inputs.
constexpr PacingPolicy policy {
    .windowFloorBytes = 16 * 1024,
    .probeIdleMicroseconds = 2'000'000,
    .roundTripStaleMicroseconds = 10'000'000,
    .rateFilterMicroseconds = 10'000'000,
    .datagramLossFloorMicroseconds = 20'000,
};

constexpr std::uint64_t start = 1'000'000;

} // namespace

TEST(theRoundTripLeavesOutTheClientsHold)
{
    AttachmentPacer pacer(policy);
    pacer.noteStreamBytes(500);
    // Sent at `start`; the client held its Ack 60 ms; it arrives 100 ms after.
    pacer.noteAck(start + 100'000, start, 60'000, 500);
    CHECK(pacer.smoothedRoundTripMicroseconds() == 40'000u);
    pacer.noteStreamBytes(500);
    pacer.noteAck(start + 300'000, start + 200'000, 20'000, 1000);
    // RFC 6298: 7/8 of 40 ms plus 1/8 of 80 ms.
    CHECK(pacer.smoothedRoundTripMicroseconds() == 45'000u);
    // An echo from the future or a hold longer than the interval is no sample.
    pacer.noteAck(start + 400'000, start + 500'000, 0, 1000);
    pacer.noteAck(start + 400'000, start + 390'000, 20'000, 1000);
    CHECK(pacer.smoothedRoundTripMicroseconds() == 45'000u);
}

TEST(theWindowGrowsWithDeliveryAndFallsToItsFloorWhenStale)
{
    AttachmentPacer pacer(policy);
    CHECK_EQ(pacer.windowBytes(start), policy.windowFloorBytes);
    // 100 ms round trips, 100 KB delivered each 100 ms: 1 MB/s.
    std::uint64_t sent = 0;
    for (int index = 1; index <= 5; ++index) {
        pacer.noteStreamBytes(100'000);
        sent += 100'000;
        const std::uint64_t now = start + static_cast<std::uint64_t>(index) * 100'000;
        pacer.noteAck(now, now - 100'000, 0, sent);
    }
    const std::uint64_t now = start + 500'000;
    CHECK_EQ(pacer.deliveryRateBytesPerSecond(), 1'000'000u);
    // Twice the rate times the round trip: 2 × 1 MB/s × 100 ms.
    CHECK_EQ(pacer.windowBytes(now), std::uint64_t { 200'000 });
    CHECK_EQ(pacer.windowBytes(now + policy.roundTripStaleMicroseconds + 1), policy.windowFloorBytes);
}

TEST(theWindowNeverAdmitsPastItself)
{
    AttachmentPacer pacer(policy);
    // Nothing outstanding: a frame larger than the window still goes alone.
    CHECK(pacer.admits(100'000, start));
    pacer.noteStreamBytes(10'000);
    CHECK(pacer.admits(policy.windowFloorBytes - 10'000, start));
    CHECK(!pacer.admits(policy.windowFloorBytes - 10'000 + 1, start));
    CHECK(pacer.isOpen(start));
    pacer.noteStreamBytes(policy.windowFloorBytes - 10'000);
    CHECK(!pacer.isOpen(start));
    // A stale or forged cumulative count retires nothing.
    pacer.noteAck(start + 1, 0, 0, policy.windowFloorBytes * 4);
    CHECK_EQ(pacer.outstandingBytes(), policy.windowFloorBytes);
    pacer.noteAck(start + 2, 0, 0, policy.windowFloorBytes);
    CHECK_EQ(pacer.outstandingBytes(), 0u);
}

TEST(queuedReservationsNeverEnterAcknowledgedStreamOffsets)
{
    AttachmentPacer pacer(policy);
    constexpr std::uint64_t queued = 10'000;
    CHECK(pacer.admits(policy.windowFloorBytes - queued, start, queued));
    CHECK(!pacer.admits(policy.windowFloorBytes - queued + 1, start, queued));
    CHECK(!pacer.isOpen(start, policy.windowFloorBytes));
    CHECK_EQ(pacer.outstandingBytes(), 0u);
    // A chunk overtakes queued interactive frames. Its actual stream end,
    // not their reservation, determines when its bulk debt is retired.
    pacer.noteBulkStreamBytes(1000);
    CHECK(pacer.admitsBulk(1000, start, queued));
    CHECK(!pacer.admitsBulk(1000, start, policy.windowFloorBytes));
    pacer.noteAck(start + 1, 0, 0, 1000);
    CHECK_EQ(pacer.bulkOutstandingBytes(), 0u);
    CHECK_EQ(pacer.outstandingBytes(), 0u);
    // Withdrawing the queued frames needs no cumulative-count correction.
    pacer.noteStreamBytes(30);
    pacer.noteAck(start + 2, 0, 0, 1030);
    CHECK_EQ(pacer.outstandingBytes(), 0u);
}

TEST(cancellingADatagramPreservesOtherPanesAndCountsNoDelivery)
{
    AttachmentPacer pacer(policy);
    pacer.noteAck(start, 0, 0, 0);
    pacer.noteDatagram(1, 5, 800, start);
    pacer.noteDatagram(1, 6, 900, start);
    pacer.noteDatagram(2, 6, 700, start);
    pacer.cancelDatagram(1, 6);
    CHECK_EQ(pacer.outstandingBytes(), 1500u);
    pacer.cancelDatagram(1, 6);
    CHECK_EQ(pacer.outstandingBytes(), 1500u);
    pacer.noteAck(start + 1, 0, 0, 0);
    CHECK_EQ(pacer.deliveryRateBytesPerSecond(), 0u);
    const std::vector<DatagramInFlight> lost = pacer.expireDatagrams(start + 20'000);
    REQUIRE(lost.size() == 2);
    CHECK_EQ(lost[0].handle, 1u);
    CHECK_EQ(lost[0].generation, 5u);
    CHECK_EQ(lost[1].handle, 2u);
    CHECK_EQ(pacer.outstandingBytes(), 0u);
}

TEST(aDatagramLeavesTheWindowWhenAcknowledgedOrLost)
{
    AttachmentPacer pacer(policy);
    pacer.noteDatagram(1, 5, 800, start);
    pacer.noteDatagram(1, 6, 900, start + 1'000);
    pacer.noteDatagram(2, 6, 700, start + 1'000);
    CHECK_EQ(pacer.outstandingBytes(), 2400u);
    pacer.retireDatagrams(1, 5);
    CHECK_EQ(pacer.outstandingBytes(), 1600u);
    CHECK(pacer.expireDatagrams(start + 20'000).empty());
    const std::vector<DatagramInFlight> lost = pacer.expireDatagrams(start + 21'000);
    CHECK_EQ(lost.size(), 2u);
    CHECK_EQ(pacer.outstandingBytes(), 0u);
}

TEST(aProbeIsDueOnlyAfterTheIdleInterval)
{
    AttachmentPacer pacer(policy);
    pacer.noteScreenData(start);
    CHECK(!pacer.isProbeDue(start + policy.probeIdleMicroseconds));
    CHECK(pacer.isProbeDue(start + policy.probeIdleMicroseconds + 1));
    pacer.noteProbe(start + policy.probeIdleMicroseconds + 1, start + policy.probeIdleMicroseconds + 1);
    CHECK(!pacer.isProbeDue(start + policy.probeIdleMicroseconds + 2));
    // An Echo of a time no probe carried is no sample; the probe's own is.
    CHECK(!pacer.noteEcho(start + policy.probeIdleMicroseconds + 30'001, start + 5, 0));
    CHECK(!pacer.smoothedRoundTripMicroseconds().has_value());
    CHECK(pacer.noteEcho(start + policy.probeIdleMicroseconds + 30'001, start + policy.probeIdleMicroseconds + 1, 0));
    CHECK(pacer.smoothedRoundTripMicroseconds() == 30'000u);
}

UBIQUITTY_TEST_MAIN()
