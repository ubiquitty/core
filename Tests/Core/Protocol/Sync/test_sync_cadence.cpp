// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_cadence.cpp — how often the client acknowledges, and that an
// owed acknowledgement leaves with input (Docs/sync.md §6.4).
//
// The `Ack` interval follows the client's round trip and what its link
// costs: `clamp(srtt / 4, 4 ms, 50 ms)` unmetered and `clamp(srtt / 2, 20 ms,
// 100 ms)` metered, doubled under a power saver. A fixed short interval keeps
// a cellular radio's uplink awake through a token stream. The properties
// held:
//
// - At an srtt of 100 ms a token stream draws at most 40 `Ack`s a second
//   unmetered and at most 20 metered, and half as many under a saver.
// - Before the client has a round trip, it holds an `Ack` for the floor.
// - Input sends the `Ack` owed with it, before its interval is up.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::LinkShape;
using UbiquittyTest::SyncLink;

namespace {

constexpr std::uint64_t second = 1'000'000;

/// A link of 10 Mbit/s and 50 ms each way: an srtt of 100 ms.
constexpr LinkShape hundredMillisecondRoundTrip { .bytesPerSecond = 1'250'000, .oneWayDelayMicroseconds = 50'000 };

/// Facts of a link's cost and a power state.
///
/// - Parameters:
///   - link: What the link costs.
///   - power: The power state.
/// - Returns: The facts.
Facts factsFor(LinkCost link, PowerState power = PowerState::Normal)
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.link = link;
    facts.power = power;
    return facts;
}

/// Streams a token a frame for four seconds after one of warm-up, and counts
/// the `Ack`s the host is sent in those four.
///
/// - Parameter facts: What the client reports.
/// - Returns: The `Ack`s a second.
double acksPerSecond(const Facts& facts)
{
    SyncLink link(0, facts, std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    const std::uint32_t pane = link.addPane();
    int token = 0;
    const std::uint64_t warmUntil = link.now() + second;
    while (link.now() < warmUntil) {
        link.write(pane, "tok" + std::to_string(token++) + " ");
        link.step();
    }
    const std::size_t acksBefore = link.ackTimes().size();
    const std::uint64_t from = link.now();
    while (link.now() < from + 4 * second) {
        link.write(pane, "tok" + std::to_string(token++) + " ");
        link.step();
    }
    return static_cast<double>(link.ackTimes().size() - acksBefore) / 4.0;
}

} // namespace

// The client's own probe at the welcome gives it the 100 ms round trip the
// interval is computed from.
TEST(theClientKnowsItsRoundTripFromTheWelcome)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    REQUIRE(link.client().smoothedRoundTripMicroseconds().has_value());
    const std::uint64_t roundTrip = *link.client().smoothedRoundTripMicroseconds();
    CHECK_MSG(roundTrip >= 100'000 && roundTrip <= 100'000 + UbiquittyTest::syncFrameInterval, ("the round trip read " + std::to_string(roundTrip) + " us").c_str());
}

// A token a frame at an srtt of 100 ms: unmetered at most 40 Acks a second,
// metered at most 20, and a saver halves each.
TEST(aMeteredTokenStreamDrawsAtMostTwentyAcksASecond)
{
    const double unmetered = acksPerSecond(factsFor(LinkCost::Unmetered));
    const double metered = acksPerSecond(factsFor(LinkCost::Metered));
    const double constrained = acksPerSecond(factsFor(LinkCost::Constrained));
    const double meteredSaver = acksPerSecond(factsFor(LinkCost::Metered, PowerState::Saver));
    CHECK_MSG(unmetered <= 40.0 && unmetered > 20.0, ("unmetered sent " + std::to_string(unmetered) + " Acks a second").c_str());
    CHECK_MSG(metered <= 20.0, ("metered sent " + std::to_string(metered) + " Acks a second").c_str());
    CHECK_MSG(constrained <= 20.0, ("constrained sent " + std::to_string(constrained) + " Acks a second").c_str());
    CHECK_MSG(meteredSaver <= 10.0, ("a metered saver sent " + std::to_string(meteredSaver) + " Acks a second").c_str());
}

// Input sends the owed Ack at once: on a metered link its interval is 50 ms,
// and the Ack leaves with the keystroke 1 ms after the update arrived.
TEST(anOwedAckLeavesWithInput)
{
    SyncLink link(0, factsFor(LinkCost::Metered), std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    const std::uint32_t pane = link.addPane();
    link.advance(second);
    const std::uint64_t appliedBefore = link.client().counters().updatesApplied;
    link.write(pane, "output");
    // Run until the update arrives, and not a frame longer.
    for (int tick = 0; tick < 1'000 && link.client().counters().updatesApplied == appliedBefore; ++tick)
        link.advance(1'000);
    REQUIRE(link.client().counters().updatesApplied > appliedBefore);
    const std::uint64_t acksBefore = link.client().counters().acksSent;
    link.advance(1'000);
    REQUIRE(link.client().counters().acksSent == acksBefore);
    link.client().noteInputSent(link.now());
    CHECK_EQ(link.client().counters().acksSent, acksBefore + 1);
    // The Ack is in the client's output now, beside where the input goes.
    const std::vector<std::uint8_t> output = link.client().takeStreamOutput();
    FrameDecoder decoder;
    decoder.append(output);
    bool hasAck = false;
    while (const std::optional<Frame> frame = decoder.next())
        hasAck = hasAck || frame->kind == FrameKind::Ack;
    CHECK(hasAck);
}

// A due Ack leaves on the client's tick whether or not input follows: it is
// never held back for a keystroke that may not come.
TEST(aDueAckIsNeverHeldForInput)
{
    SyncLink link(0, factsFor(LinkCost::Metered), std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    const std::uint32_t pane = link.addPane();
    link.advance(second);
    const std::uint64_t acksBefore = link.client().counters().acksSent;
    link.write(pane, "output");
    link.advance(400'000);
    CHECK(link.client().counters().acksSent > acksBefore);
}

// Input with no Ack owed sends nothing for the Ack: the client's output holds
// no Ack frame.
TEST(nothingIsSentForInputWhenNoAckIsOwed)
{
    SyncLink link(0, factsFor(LinkCost::Metered), std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    (void)link.addPane();
    link.advance(5 * second);
    const std::uint64_t acksBefore = link.client().counters().acksSent;
    link.client().noteInputSent(link.now());
    CHECK_EQ(link.client().counters().acksSent, acksBefore);
    const std::vector<std::uint8_t> output = link.client().takeStreamOutput();
    FrameDecoder decoder;
    decoder.append(output);
    while (const std::optional<Frame> frame = decoder.next())
        CHECK(frame->kind != FrameKind::Ack);
}

// Ten idle minutes draw no Ack: with nothing applied, nothing is owed.
TEST(noAckIsSentWhileIdle)
{
    SyncLink link(0, factsFor(LinkCost::Unmetered), std::nullopt, ClientKind::Human, hundredMillisecondRoundTrip);
    (void)link.addPane();
    link.advance(5 * second);
    const std::size_t acksBefore = link.ackTimes().size();
    link.advance(600 * second);
    CHECK_EQ(link.ackTimes().size(), acksBefore);
}

namespace {

/// Changes a pane every frame for five seconds on the instant link and counts
/// the updates the client applies.
///
/// - Parameter facts: What the client reports.
/// - Returns: The updates.
std::uint64_t updatesInFiveSeconds(const Facts& facts)
{
    SyncLink link(0, facts);
    const std::uint32_t pane = link.addPane();
    const std::uint64_t before = link.client().counters().updatesApplied;
    const std::uint64_t from = link.now();
    int token = 0;
    while (link.now() < from + 5 * second) {
        link.write(pane, "tok" + std::to_string(token++) + " ");
        link.step();
    }
    return link.client().counters().updatesApplied - before;
}

} // namespace

// A constrained link — a phone in low-data mode — is sent at most 20 updates a
// second per pane however fast its pane changes (Docs/sync.md §6.4), against
// the 60 a metered link at the same refresh rate is sent.
TEST(aConstrainedLinkGetsAtMostTwentyUpdatesASecondPerPane)
{
    const std::uint64_t constrained = updatesInFiveSeconds(factsFor(LinkCost::Constrained));
    CHECK_MSG(constrained <= 101, ("a constrained link was sent " + std::to_string(constrained) + " updates in 5 s").c_str());
    const std::uint64_t metered = updatesInFiveSeconds(factsFor(LinkCost::Metered));
    CHECK_MSG(metered >= 250, ("a metered link was sent " + std::to_string(metered) + " updates in 5 s").c_str());
}

UBIQUITTY_TEST_MAIN()
