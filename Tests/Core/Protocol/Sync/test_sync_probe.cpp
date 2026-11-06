// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_probe.cpp — probes both ways, the backing-off idle schedule, and
// the silence that loses an attachment (Docs/sync.md §6.4, §6.9).
//
// The properties held, on a fake clock:
//
// - After screen data stops, the host probes at 2, 4, 8, 16 and then every
//   30 s, and screen data starts the schedule again.
// - It probes only while some pane is Live and the client is focused: an
//   unfocused or Dormant-only connection is sent no probe at all.
// - A connection with nothing outstanding is never lost however long it
//   idles; one whose bytes stay outstanding with nothing heard for 30 s is,
//   and an Echo counts as heard. Bytes that carried only probes are answered
//   by the Echo and owe no Ack, so an idle connection's 30 s probe step
//   never races the 30 s silence bound.
// - The client probes too, and its Echo gives it a round trip on its own clock.
// - Input that no frame follows draws a probe, and the same span again with no
//   frame takes the client to Lost: a blackholed path is found within 2 s of a
//   keystroke at 100 ms round trips.
// - A probe flood owes one Echo, an Echo that answers no probe is no sample,
//   and an Ack naming a generation never sent retires no datagram.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

constexpr std::uint64_t second = 1'000'000;

/// Facts for a client in the background.
Facts unfocusedFacts()
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.isFocused = false;
    return facts;
}

/// The times the host's probes reach the client.
///
/// - Parameter link: The link, tapped from now on.
/// - Returns: The times, filled as the link runs.
std::shared_ptr<std::vector<std::uint64_t>> tapProbes(SyncLink& link)
{
    auto times = std::make_shared<std::vector<std::uint64_t>>();
    link.onHostFrame([times, &link](FrameKind kind, std::uint32_t, Carrier, std::size_t) {
        if (kind == FrameKind::Probe)
            times->push_back(link.now());
    });
    return times;
}

/// Runs the instant link until a time.
///
/// - Parameters:
///   - link: The link.
///   - until: The time.
void runUntil(SyncLink& link, std::uint64_t until)
{
    while (link.now() < until)
        link.step();
}

} // namespace

// Screen data stops at a time; the probes follow at 2, 6, 14, 30, 60 and 90 s
// after it, the step doubling to 30 s and staying there. A write starts the
// schedule again.
TEST(anIdleFocusedConnectionIsProbedOnTheBackingOffSchedule)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.write("last output");
    link.step();
    const std::uint64_t quietFrom = link.now();
    const auto probes = tapProbes(link);
    runUntil(link, quietFrom + 95 * second);
    std::vector<std::uint64_t> afterQuiet;
    for (const std::uint64_t at : *probes)
        afterQuiet.push_back((at - quietFrom + second / 2) / second);
    CHECK(afterQuiet == (std::vector<std::uint64_t> { 2, 6, 14, 30, 60, 90 }));

    probes->clear();
    link.write("more output");
    link.step();
    const std::uint64_t restartedFrom = link.now();
    runUntil(link, restartedFrom + 3 * second);
    REQUIRE(probes->size() == 1);
    CHECK_EQ((probes->front() - restartedFrom + second / 2) / second, std::uint64_t { 2 });
}

// A client in the background is sent no probe, and neither is one whose every
// pane is Dormant: a phone's radio stays asleep.
TEST(anUnfocusedOrDormantOnlyConnectionIsNeverProbed)
{
    {
        SyncLink link(0, unfocusedFacts());
        (void)link.addPane();
        runUntil(link, link.now() + 600 * second);
        CHECK_EQ(link.host().counters().probesSent, std::uint64_t { 0 });
        CHECK(link.host().state() == ConnectionState::Attached);
    }
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        (void)link.addPane();
        link.client().setView(link.handle(), ViewState::Dormant);
        link.step();
        const std::uint64_t before = link.host().counters().probesSent;
        runUntil(link, link.now() + 600 * second);
        CHECK_EQ(link.host().counters().probesSent, before);
    }
}

// Ten idle minutes, focused and Live, with every Echo delivered: probed on its
// schedule, never lost.
TEST(anIdleLiveAttachmentNeverEntersLost)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    runUntil(link, link.now() + 600 * second);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(link.host().counters().probesSent >= 20);
}

// Bytes outstanding with no Ack and no Echo for 30 s lose the attachment; at
// 29.9 s it still stands. With nothing outstanding the same silence loses
// nothing.
TEST(bytesOutstandingWithNothingHeardForThirtySecondsAreLost)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdUpstream();
    link.write("unacknowledged");
    link.step();
    const std::uint64_t sentAt = link.now();
    REQUIRE(link.host().pacer().outstandingBytes() > 0);
    runUntil(link, sentAt + 29'900'000);
    CHECK(link.host().state() == ConnectionState::Attached);
    runUntil(link, sentAt + 30'100'000);
    CHECK(link.host().state() == ConnectionState::Lost);

    SyncLink idle(0, unfocusedFacts());
    (void)idle.addPane();
    idle.settleAttach();
    idle.holdUpstream();
    runUntil(idle, idle.now() + 600 * second);
    CHECK(idle.host().state() == ConnectionState::Attached);
}

// A client whose Acks stop and whose Echoes still flow is heard: the silence
// rule does not lose it.
TEST(anEchoCountsAsHeardForTheSilenceRule)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdUpstream(FrameKind::Ack);
    link.write("unacknowledged");
    link.step();
    runUntil(link, link.now() + 60 * second);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(link.host().counters().probesSent > 0);
}

// The client probes when its input waits, and the host answers. On a link
// with 50 ms each way the client's round trip converges on 100 ms: the
// handshake's sample on the instant link was 0, and thirty samples later the
// smoothing has all but forgotten it. The client is unfocused, so no probe of
// the host's own answers its waits first.
TEST(aClientProbeIsAnsweredAndGivesTheClientARoundTrip)
{
    SyncLink link(0, unfocusedFacts());
    (void)link.addPane();
    link.settleAttach();
    link.shape({ .bytesPerSecond = 10'000'000, .oneWayDelayMicroseconds = 50'000 });
    const std::uint64_t probesBefore = link.client().counters().probesSent;
    for (int wait = 0; wait < 30; ++wait) {
        link.client().noteInputSent(link.now());
        link.advance(2 * second);
    }
    CHECK_EQ(link.client().counters().probesSent - probesBefore, std::uint64_t { 30 });
    CHECK(link.host().counters().echoesSent >= 30);
    REQUIRE(link.client().smoothedRoundTripMicroseconds().has_value());
    const std::uint64_t roundTrip = *link.client().smoothedRoundTripMicroseconds();
    CHECK_MSG(roundTrip >= 95'000 && roundTrip <= 100'000 + UbiquittyTest::syncFrameInterval, ("the round trip read " + std::to_string(roundTrip) + " us").c_str());
}

// The path is blackholed both ways with no event to say so. A keystroke waits
// 750 ms for a frame, draws a probe, waits as long again, and the client is
// Lost — within 2 s of the keystroke at 100 ms round trips.
TEST(aBlackholedPathTakesAKeystrokeToLostWithinTwoSeconds)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    link.shape({ .bytesPerSecond = 10'000'000, .oneWayDelayMicroseconds = 50'000 });
    (void)link.addPane();
    link.advance(second);
    link.holdDownstream();
    link.holdUpstream();
    const std::uint64_t typedAt = link.now();
    link.client().noteInputSent(typedAt);
    const std::uint64_t probesBefore = link.client().counters().probesSent;
    std::uint64_t lostAt = 0;
    for (int tick = 0; tick < 4'000 && lostAt == 0; ++tick) {
        link.advance(1'000);
        if (link.client().state() == ConnectionState::Lost)
            lostAt = link.now();
    }
    REQUIRE(lostAt != 0);
    CHECK_MSG(lostAt - typedAt <= 2 * second, ("lost " + std::to_string(lostAt - typedAt) + " us after the keystroke").c_str());
    CHECK_EQ(link.client().counters().probesSent, probesBefore + 1);
}

// A client that writes 10,000 probes and reads nothing is owed one Echo.
TEST(aProbeFloodOwesOneEcho)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdDownstream();
    std::vector<std::uint8_t> body;
    encode(body, Probe { .time = 77 });
    std::vector<std::uint8_t> flood;
    for (int probe = 0; probe < 10'000; ++probe)
        CHECK(appendFrame(flood, FrameKind::Probe, 0, body));
    const std::uint64_t echoesBefore = link.host().counters().echoesSent;
    link.host().receiveStream(flood, link.now());
    link.step();
    CHECK_EQ(link.host().counters().echoesSent - echoesBefore, std::uint64_t { 1 });
    CHECK(link.host().state() == ConnectionState::Attached);
}

// An Echo naming a time the host never probed with is no round-trip sample:
// a peer cannot set the host's pacing by forging one. The forged time is a
// plausible 100 ms back, a sample the pacer would otherwise take.
TEST(anEchoThatAnswersNoProbeIsNotARoundTrip)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    for (int step = 0; step < 30; ++step) {
        link.write("x");
        link.step();
    }
    const std::optional<std::uint64_t> before = link.host().pacer().smoothedRoundTripMicroseconds();
    std::vector<std::uint8_t> body;
    encode(body, Echo { .timeEcho = link.now() - 100'000, .holdMicroseconds = 0 });
    link.sendToHost(FrameKind::Echo, body);
    CHECK(link.host().pacer().smoothedRoundTripMicroseconds() == before);
}

// An Ack naming a generation the host never sent acknowledges nothing: it
// retires none of the datagrams still in flight.
TEST(anAckNamingAGenerationNeverSentRetiresNoDatagram)
{
    SyncLink link(UbiquittyTest::syncMaxDatagramBytes, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdUpstream();
    for (int step = 0; step < 4; ++step) {
        link.write("superset " + std::to_string(step));
        link.step();
    }
    const std::uint64_t outstanding = link.host().pacer().outstandingBytes();
    REQUIRE(outstanding > 0);
    Ack forged;
    forged.panes.push_back({ .handle = link.handle(), .generation = link.newest() + 1'000 });
    std::vector<std::uint8_t> body;
    encode(body, forged);
    link.sendToHost(FrameKind::Ack, body);
    CHECK_EQ(link.host().pacer().outstandingBytes(), outstanding);
}

// The Welcome names the schedule a client derives its silence bound from.
TEST(theWelcomeAdvertisesTheProbeSchedule)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    REQUIRE(link.client().welcome().has_value());
    const Limits& limits = link.client().welcome()->limits;
    REQUIRE(limits.probeSteps == 5);
    CHECK_EQ(limits.probeScheduleMicroseconds[0], std::uint32_t { 2'000'000 });
    CHECK_EQ(limits.probeScheduleMicroseconds[3], std::uint32_t { 16'000'000 });
    CHECK_EQ(limits.probeScheduleMicroseconds[4], std::uint32_t { 30'000'000 });

    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.limits.probeSteps = maxProbeSteps + 1;
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    CHECK(!decodeWelcome(body).has_value());
}

UBIQUITTY_TEST_MAIN()

// On a carrier with datagrams the client's own probe leaves as one, and the
// host's Echo comes back on the carrier the probe came on.
TEST(aClientProbeTravelsAsADatagramWhenTheCarrierHasThem)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.datagramMaxBytes = 1'200;
    // Unfocused, so the host sends nothing of its own to answer for the
    // probe.
    SyncLink link(1'200, unfocusedFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane();
    link.settleAttach();
    link.shape({ .bytesPerSecond = 10'000'000, .oneWayDelayMicroseconds = 50'000 });
    auto echoCarriers = std::make_shared<std::vector<Carrier>>();
    link.onHostFrame([echoCarriers](FrameKind kind, std::uint32_t, Carrier carrier, std::size_t) {
        if (kind == FrameKind::Echo)
            echoCarriers->push_back(carrier);
    });
    const std::uint64_t probesBefore = link.client().counters().probesSent;
    // Input no receipt covers draws the client's own probe.
    link.client().noteInputSent(link.now());
    link.advance(2 * second);
    CHECK(link.client().counters().probesSent > probesBefore);
    REQUIRE(!echoCarriers->empty());
    CHECK(echoCarriers->front() == Carrier::Datagram);
}

// The probe a client sends before it trusts a connection again, after a wake
// or a path event, is answered by its own Echo and nothing else: screen
// frames the host sent without having read it say nothing of the path. It
// travels on the stream even where the carrier has datagrams, so one lost
// packet is not a verdict, and the connection itself never declares the
// silence a loss: the owner times it.
TEST(aLivenessCheckIsAnsweredOnlyByItsOwnEcho)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.datagramMaxBytes = 1'200;
    SyncLink link(1'200, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane();
    link.settleAttach();
    auto echoCarriers = std::make_shared<std::vector<Carrier>>();
    link.onHostFrame([echoCarriers](FrameKind kind, std::uint32_t, Carrier carrier, std::size_t) {
        if (kind == FrameKind::Echo)
            echoCarriers->push_back(carrier);
    });
    CHECK(!link.client().isLivenessUnproven());
    link.holdUpstream(FrameKind::Probe);
    const std::uint64_t probesBefore = link.client().counters().probesSent;
    link.client().checkLiveness(link.now());
    CHECK_EQ(link.client().counters().probesSent, probesBefore + 1);
    CHECK(link.client().isLivenessUnproven());
    // A second suspicion sends nothing more while the first is unanswered.
    link.client().checkLiveness(link.now());
    CHECK_EQ(link.client().counters().probesSent, probesBefore + 1);
    const std::uint64_t heldBefore = link.held();
    link.write("the host writes on\r\n");
    runUntil(link, link.now() + 5 * second);
    CHECK(link.held() > heldBefore);
    CHECK(link.client().isLivenessUnproven());
    CHECK(link.client().state() == ConnectionState::Attached);
    // Nor does the Echo of another probe.
    std::vector<std::uint8_t> stale;
    encode(stale, Echo { .timeEcho = link.now() - 7, .holdMicroseconds = 0 });
    std::vector<std::uint8_t> staleFrame;
    REQUIRE(appendFrame(staleFrame, FrameKind::Echo, 0, stale));
    link.deliverToClient(staleFrame);
    CHECK(link.client().isLivenessUnproven());
    echoCarriers->clear();
    link.releaseUpstream();
    runUntil(link, link.now() + second);
    CHECK(!link.client().isLivenessUnproven());
    REQUIRE(!echoCarriers->empty());
    CHECK(echoCarriers->front() == Carrier::Stream);
}

// A client that loses focus says so with Facts, and the host stops probing
// its idle connection; regaining focus starts the schedule again.
TEST(aFocusChangeReachesTheHostAndGatesItsProbes)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.client().setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    link.step();
    CHECK(!link.host().facts().isFocused);
    const std::uint64_t before = link.host().counters().probesSent;
    runUntil(link, link.now() + 600 * second);
    CHECK_EQ(link.host().counters().probesSent, before);

    link.client().setFocus(Ubiquitty::Modes::FocusState::Focused);
    link.write("back");
    link.step();
    CHECK(link.host().facts().isFocused);
    runUntil(link, link.now() + 3 * second);
    CHECK(link.host().counters().probesSent > before);
}

// Five quiet minutes with nothing outstanding count for nothing: the silence
// bound runs from the first byte sent after them, not from the last Ack.
TEST(silenceCountsOnlyWhileBytesAreOutstanding)
{
    SyncLink link(0, unfocusedFacts());
    (void)link.addPane();
    runUntil(link, link.now() + 300 * second);
    CHECK(!link.host().silenceDeadlineMicroseconds().has_value());
    link.holdUpstream();
    link.write("after the quiet");
    link.step();
    const std::uint64_t sentAt = link.now();
    REQUIRE(link.host().pacer().outstandingBytes() > 0);
    const std::optional<std::uint64_t> deadline = link.host().silenceDeadlineMicroseconds();
    REQUIRE(deadline.has_value());
    CHECK_EQ(*deadline, sentAt + 30 * second);
}

// Input reaching the program restarts the idle schedule as screen data does:
// the next probe is 2 s after it, not the 30 s the schedule had reached.
TEST(inputResetsTheProbeSchedule)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    const auto probes = tapProbes(link);
    runUntil(link, link.now() + 100 * second);
    REQUIRE(probes->size() >= 5);
    probes->clear();
    const std::uint64_t typedAt = link.now();
    link.host().noteClientInput(typedAt);
    runUntil(link, typedAt + 3 * second);
    REQUIRE(probes->size() == 1);
    CHECK_EQ((probes->front() - typedAt + second / 2) / second, std::uint64_t { 2 });
}

// An idle focused connection is probed every 30 s once the schedule has
// backed off, and the silence bound is 30 s too. When the path's delay grows,
// each Echo arrives later than the last; the attachment stays, because the
// silence clock starts at a probe and not at the Echo before it.
TEST(anIdleConnectionWhoseEchoesArriveLaterIsNotLost)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.settleAttach();
    link.shape({ .bytesPerSecond = 10'000'000, .oneWayDelayMicroseconds = 50'000 });
    link.advance(100 * second);
    REQUIRE(link.host().state() == ConnectionState::Attached);
    link.shape({ .bytesPerSecond = 10'000'000, .oneWayDelayMicroseconds = 500'000 });
    link.advance(120 * second);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK_EQ(link.host().counters().lostDeclared, std::uint64_t { 0 });
}
