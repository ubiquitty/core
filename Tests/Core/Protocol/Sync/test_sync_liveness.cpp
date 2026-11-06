// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_liveness — the client finds a dead path while the user types and
// resumes over a new one, and shows a stalled link without calling it lost
// (Docs/sync.md §6.9 "Liveness on demand" and "Stalled is not lost").

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Liveness.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;

namespace {

constexpr std::uint64_t second = 1'000'000;

/// Steps the link a millisecond at a time until a condition holds.
///
/// - Returns: When it held, or `nullopt` if it never did within `span`.
std::optional<std::uint64_t> pumpUntil(Host& host, ClientAttachment& client, std::uint64_t& now, std::uint64_t span, Carry carry, const auto& condition)
{
    for (const std::uint64_t until = now + span; now < until; now += 1'000) {
        pump(host, client, now, carry);
        if (condition())
            return now;
    }
    return std::nullopt;
}

} // namespace

// The path goes dark both ways with no event to say so. The keystroke waits
// the stall bound for its receipt, draws a probe, waits as long again, and
// the attachment is Lost and wants a redial within 2 s of the keystroke.
TEST(aBlackholedPathIsLeftWithinTwoSecondsOfAKeystroke)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    pumpFor(host, client, now, second);
    const std::uint64_t typedAt = now;
    CHECK(client.send(paneID, raw("x"), now).has_value());
    const std::optional<std::uint64_t> lostAt = pumpUntil(host, client, now, 3 * second, { .shouldCarryDown = false, .shouldCarryUp = false }, [&] { return client.state() == ClientAttachmentState::Lost; });
    REQUIRE(lostAt.has_value());
    CHECK_MSG(*lostAt - typedAt <= 1'600'000, ("lost " + std::to_string(*lostAt - typedAt) + " us after the keystroke").c_str());
    CHECK(client.wantsRedial());
    CHECK(client.connection() == nullptr);
}

// The host still holds the transport the client found dead. The redial
// resumes it there: the old transport is superseded, the pane continues with
// no checkpoint, and the keystroke the dead path swallowed is written once.
TEST(aRedialWhileTheHostHoldsTheOldTransportCostsNoCheckpoint)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, second);
    CHECK(client.send(paneID, raw("x"), now).has_value());
    REQUIRE(pumpUntil(host, client, now, 3 * second, { .shouldCarryDown = false, .shouldCarryUp = false }, [&] { return client.wantsRedial(); }).has_value());

    host.acceptKeepingTheOld();
    (void)client.connect(now);
    CHECK(!client.wantsRedial());
    pump(host, client, now);
    pump(host, client, now);
    for (const std::uint64_t superseded : host.table.takeSuperseded()) {
        if (superseded == host.connectionNumber - 1)
            host.previous->supersede();
    }
    CHECK(host.previous->state() == ConnectionState::Closed);
    pumpFor(host, client, now, 200'000);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("ax"));
    CHECK(client.state() == ClientAttachmentState::Live);
}

// A host gone silent past its probe schedule shows as no contact, not Lost;
// its next probe clears it. Input the program will not take then shows as
// undelivered while the probes keep the link alive.
TEST(aReceivedProbeClearsNoContactAndLeavesInputUndelivered)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    const Limits& limits = client.connection()->welcome()->limits;
    REQUIRE(limits.probeSteps > 0);
    // Screen data restarts the host's schedule: its first idle probe is due
    // one step later.
    UbiquittyTest::feed(host.pane.terminal, "output\r\n");
    host.pane.source->publish(FrameClass::Settled, now);
    pumpFor(host, client, now, 50'000);
    CHECK(!client.linkState(now).isSilent);

    // Nothing comes down; nothing is typed, so nothing is probed for. The
    // host's first step and the stall bound pass in silence.
    const std::optional<std::uint64_t> silentAt = pumpUntil(host, client, now, 10 * second, { .shouldCarryDown = false, .shouldCarryUp = true }, [&] { return client.linkState(now).isSilent; });
    REQUIRE(silentAt.has_value());
    LinkState link = client.linkState(now);
    CHECK(!link.isInputUndelivered);
    CHECK(link.isStalled());
    CHECK(client.state() == ClientAttachmentState::Live);
    // The host's next probe arrives: contact.
    const std::uint64_t longestStep = *std::ranges::max_element(std::span { limits.probeScheduleMicroseconds }.first(limits.probeSteps));
    REQUIRE(pumpUntil(host, client, now, longestStep + second, {}, [&] { return !client.linkState(now).isSilent; }).has_value());
    CHECK(host.connection->state() != ConnectionState::Lost);

    // The program stops reading its input.
    host.pane.writer.refuseNext(1'000'000);
    CHECK(client.send(paneID, raw("y"), now).has_value());
    const std::uint64_t typedAt = now;
    pumpFor(host, client, now, 3 * second);
    link = client.linkState(now);
    CHECK(link.isInputUndelivered);
    CHECK(!link.isSilent);
    CHECK(link.oldestUnreceiptedAgeMicroseconds.value_or(0) >= now - typedAt - 1'000);
    CHECK(client.state() == ClientAttachmentState::Live);
}

// The time a link turns stalled is the first moment `judgeLink` says so, for
// each indicator, and nothing once it shows: an owner that wakes then shows
// the stall as it begins, and one that wakes a tick earlier does not.
TEST(theNextStallIsTheMomentAnIndicatorTurns)
{
    const LinkFacts waiting { .lastHeardMicroseconds = 10 * second, .unreceiptedSinceMicroseconds = 12 * second, .silenceBoundMicroseconds = std::nullopt, .stallBoundMicroseconds = 750'000 };
    const std::optional<std::uint64_t> undelivered = nextStallMicroseconds(waiting, 12 * second);
    REQUIRE(undelivered.has_value());
    CHECK_EQ(*undelivered, 12 * second + 750'000);
    CHECK(judgeLink(waiting, *undelivered).isInputUndelivered);
    CHECK(!judgeLink(waiting, *undelivered - 1).isInputUndelivered);
    CHECK(!nextStallMicroseconds(waiting, *undelivered).has_value());

    const LinkFacts quiet { .lastHeardMicroseconds = 10 * second, .unreceiptedSinceMicroseconds = std::nullopt, .silenceBoundMicroseconds = 5 * second, .stallBoundMicroseconds = 750'000 };
    const std::optional<std::uint64_t> silent = nextStallMicroseconds(quiet, 11 * second);
    REQUIRE(silent.has_value());
    CHECK(judgeLink(quiet, *silent).isSilent);
    CHECK(!judgeLink(quiet, *silent - 1).isSilent);
    CHECK(!nextStallMicroseconds(quiet, *silent).has_value());

    // Both waiting: the earlier turns first, and the later is next.
    LinkFacts both = quiet;
    both.unreceiptedSinceMicroseconds = 14 * second;
    CHECK(nextStallMicroseconds(both, 14 * second) == std::optional { 14 * second + 750'000 });
    CHECK(nextStallMicroseconds(both, 14 * second + 750'000) == silent);
    // And the other way about: silence due before the input's bound.
    both.unreceiptedSinceMicroseconds = 14 * second + 900'000;
    CHECK(nextStallMicroseconds(both, 14 * second + 900'000) == silent);

    // A host that need not speak, with nothing sent, never stalls.
    const LinkFacts idle { .lastHeardMicroseconds = 10 * second, .unreceiptedSinceMicroseconds = std::nullopt, .silenceBoundMicroseconds = std::nullopt, .stallBoundMicroseconds = 750'000 };
    CHECK(!nextStallMicroseconds(idle, 100 * second).has_value());
}

// A quiet host backs off its probes; between two of them, however far
// apart, the client never shows it as silent.
TEST(aQuietHostOnItsBackoffIsNeverSilent)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    const std::uint64_t probesBefore = host.connection->counters().probesSent;
    bool wasSilent = false;
    for (const std::uint64_t until = now + 120 * second; now < until; now += 10'000) {
        pump(host, client, now);
        wasSilent = wasSilent || client.linkState(now).isSilent;
    }
    CHECK(host.connection->counters().probesSent >= probesBefore + 4);
    CHECK(!wasSilent);
}

// The receipt for the waiting input clears the stall.
TEST(aReceiptClearsTheStall)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    host.pane.writer.refuseNext(1'000'000);
    CHECK(client.send(paneID, raw("z"), now).has_value());
    pumpFor(host, client, now, 2 * second);
    REQUIRE(client.linkState(now).isInputUndelivered);
    host.pane.writer.refuseNext(0);
    host.pane.source->drainInput(now);
    pumpFor(host, client, now, 100'000);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("z"));
    const LinkState link = client.linkState(now);
    CHECK(!link.isInputUndelivered);
    CHECK(!link.oldestUnreceiptedAgeMicroseconds.has_value());
    CHECK(!link.isStalled());
}

// A client in the background whose panes are all hidden is probed by nobody
// and probes nobody, through ten idle minutes.
TEST(anIdleClientNobodyIsLookingAtNeverProbes)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    client.setView(paneID, ViewState::Dormant);
    client.connection()->setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    pumpFor(host, client, now, second);
    const std::uint64_t clientProbes = client.connection()->counters().probesSent;
    const std::uint64_t hostProbes = host.connection->counters().probesSent;
    for (const std::uint64_t until = now + 600 * second; now < until; now += 100'000)
        pump(host, client, now);
    CHECK_EQ(client.connection()->counters().probesSent, clientProbes);
    CHECK_EQ(host.connection->counters().probesSent, hostProbes);
    CHECK(client.state() == ClientAttachmentState::Live);
    CHECK(!client.linkState(now).isStalled());
}

// A client the OS is about to suspend says so. The host holds its attachment
// Lost in the loop pass that reads the Bye, with no silence to find, the size
// the client owned stays its own, and the client resumes when it wakes.
TEST(aSuspendingClientIsLostWithinOneHostLoopAndKeepsGeometry)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    const std::optional<ID128> owner = host.pane.source->geometry().status().owner;
    REQUIRE(owner.has_value());
    REQUIRE(client.held().has_value());
    const ID128 attachmentID = client.held()->attachmentID;
    REQUIRE(host.table.find(attachmentID) != nullptr);
    REQUIRE(host.table.find(attachmentID)->state == AttachmentState::Live);

    client.suspend();
    CHECK(client.state() == ClientAttachmentState::Lost);
    host.connection->receiveStream(client.takeStreamOutput(), now);
    host.connection->service(now);
    REQUIRE(host.table.find(attachmentID) != nullptr);
    CHECK(host.table.find(attachmentID)->state == AttachmentState::Lost);
    CHECK(host.connection->byeReason() == std::optional { ByeReason::Suspending });
    CHECK(host.pane.source->geometry().status().owner == owner);

    client.transportLost(now);
    host.connection.reset();
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    CHECK(client.held()->attachmentID == attachmentID);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK(host.pane.source->geometry().status().owner == owner);
}

// Suspended, the owner still owns the size and is no longer the primary:
// that falls to the Live human driver who typed most recently, and returns
// when the owner resumes.
TEST(suspendingMovesThePrimaryToTheLiveTypingDriver)
{
    /// A second connection of the same host.
    struct Second {
        std::unique_ptr<HostConnection> connection;
    };
    Host host({ 41, 42 });
    ClientAttachment owner(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, owner, now);

    Second second;
    second.connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
    second.connection->serveFromTable(host.table, UbiquittyTest::AttachmentLink::owner, 1'000, [&](ID128 id) { return id == paneID ? host.pane.source.get() : nullptr; });
    ClientIdentity otherIdentity = clientIdentity();
    otherIdentity.clientInstance = { 70, 80 };
    ClientAttachment other(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), otherIdentity);
    (void)other.connect(now);
    pump(second, other, now);
    pump(second, other, now);
    (void)second.connection->bindPane(*host.pane.source);
    pumpFor(second, other, now, 100'000);
    REQUIRE(other.state() == ClientAttachmentState::Live);

    // The other types, then the owner: the owner owns the size and is the
    // primary.
    REQUIRE(other.send(paneID, raw("o"), now).has_value());
    pumpFor(second, other, now, 20'000);
    REQUIRE(owner.send(paneID, raw("a"), now).has_value());
    pumpFor(host, owner, now, 20'000);
    const ID128 ownerID = owner.held()->attachmentID;
    const ID128 otherID = other.held()->attachmentID;
    REQUIRE(host.pane.source->geometry().status().owner == std::optional { ownerID });
    REQUIRE(host.table.primary(host.pane.source->geometry()) == std::optional { ownerID });

    owner.suspend();
    host.connection->receiveStream(owner.takeStreamOutput(), now);
    host.connection->service(now);
    CHECK(host.pane.source->geometry().status().owner == std::optional { ownerID });
    CHECK(host.table.primary(host.pane.source->geometry()) == std::optional { otherID });

    owner.transportLost(now);
    host.connection.reset();
    reconnect(host, owner, now);
    REQUIRE(owner.state() == ClientAttachmentState::Live);
    CHECK(host.table.primary(host.pane.source->geometry()) == std::optional { ownerID });
}

UBIQUITTY_TEST_MAIN()
