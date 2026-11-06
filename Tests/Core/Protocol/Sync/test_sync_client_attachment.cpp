// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_client_attachment — the client's side of an attachment that
// outlives its connections (Docs/sync.md §6.7 "Exactly-once across a drop",
// §6.9): input sent at a drop is written once, an outcome the host
// cannot report is shown in doubt and never resent unasked, numbering never
// reuses what the host wrote, the picture is one terminal throughout, and an
// unplanned restart ends the old panes. The attachment is also the owner
// that sends an owed Ack with the input after it (§6.4).

#include "AttachmentLink.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::OwnedPane;

using namespace UbiquittyTest::AttachmentLink;

TEST(detachingReleasesEffectReadersBeforeAnotherServiceOrBinding)
{
    for (const std::uint64_t serviceInterval : { 0, 3 }) {
        Host host({ 41, 42 });
        for (std::uint64_t index = 1; index <= 2 * maxEffectReaders; ++index) {
            host.accept();
            ClientIdentity identity = clientIdentity();
            identity.clientInstance.low = index;
            ClientConnection client(UbiquittyTest::syncClientPolicy, identity);
            client.start();
            host.connection->receiveStream(client.takeStreamOutput(), index);
            REQUIRE(host.connection->state() == ConnectionState::Attached);
            REQUIRE(host.connection->bindPane(*host.pane.source) != 0);
            REQUIRE(host.connection->attachment());
            if (serviceInterval && index % serviceInterval == 0)
                host.connection->service(index);
            host.table.noteDetached(host.connection->attachment()->attachmentID);
            CHECK_EQ(host.pane.source->effects().readerCount(), 0u);
            host.connection.reset();
        }
    }
}

TEST(aFreshBindingKeepsReadersOfLiveAttachmentsAtTheBound)
{
    Host host({ 41, 42 });
    for (std::uint64_t index = 1; index <= maxEffectReaders + 1; ++index) {
        host.accept();
        ClientIdentity identity = clientIdentity();
        identity.clientInstance.low = index;
        ClientConnection client(UbiquittyTest::syncClientPolicy, identity);
        client.start();
        host.connection->receiveStream(client.takeStreamOutput(), index);
        REQUIRE(host.connection->state() == ConnectionState::Attached);
        const std::uint32_t handle = host.connection->bindPane(*host.pane.source);
        CHECK_EQ(handle != 0, index <= maxEffectReaders);
        if (!handle) {
            std::vector<std::uint8_t> down;
            while (host.connection->popStreamFrame(down, index)) {
            }
            client.receiveStream(down, index);
            CHECK(client.byeReason() == ByeReason::HostFull);
        }
        host.connection.reset();
    }
}

TEST(aPlannedHandoverKeepsThePictureAndRequestsAnImmediateRedial)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    const auto* picture = client.paneTerminal(paneID);
    REQUIRE(picture);
    CHECK(client.send(paneID, raw("x"), now).has_value());
    std::vector<std::uint8_t> body;
    encode(body, Bye { .reason = ByeReason::HandingOver, .detail = {} });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Bye, 0, body));
    client.receiveStream(frame, now);
    CHECK(client.state() == ClientAttachmentState::Lost);
    CHECK(client.wantsRedial());
    CHECK(client.wantsHandoverRedial());
    CHECK(client.paneTerminal(paneID) == picture);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *picture).empty());
    CHECK(!client.isEnded(paneID));
    CHECK_EQ(client.pendingCount(paneID), 1u);
    CHECK(client.send(paneID, raw("y"), now).has_value());
    CHECK_EQ(client.pendingCount(paneID), 2u);
    (void)client.connect(now);
    CHECK(!client.wantsRedial());
    CHECK(!client.wantsHandoverRedial());
    CHECK(client.paneTerminal(paneID) == picture);
}

// An event the host wrote whose receipt the drop swallowed is not written
// again; one the drop swallowed on the way up is resent and written.
TEST(aDropBetweenWriteAndReceiptDeliversExactlyOnce)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    // `x` reaches the host; nothing comes back.
    CHECK_EQ(client.send(paneID, raw("x"), now).value_or(0), 1u);
    pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = true });
    // `y` never leaves.
    CHECK_EQ(client.send(paneID, raw("y"), now).value_or(0), 2u);
    pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = false });
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("x"));
    CHECK_EQ(client.pendingCount(paneID), 2u);
    drop(host, client, now);

    host.accept();
    (void)client.connect(now);
    for (const std::uint64_t until = now + 200'000; now < until; now += 1'000)
        pumpFrameByFrame(host, client, now);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("xy"));
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].inputWritten == std::optional<std::uint64_t> { 1 });
    // Only `y` was sent again: the host had nothing to drop.
    CHECK_EQ(host.connection->counters().inputDuplicates, 0u);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK(client.inDoubt(paneID).empty());
}

// Past the grace the attachment is gone and the resume is refused: the event
// the host wrote is shown in doubt, not resent; a resend the user asks for
// is still caught by the pane's ledger, which outlives the attachment.
TEST(afterTheGraceExpiresTheEventIsWrittenOnceShownInDoubtAndAUserResendIsStillDeduplicated)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("y"), now).has_value());
    pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = true });
    drop(host, client, now);
    now += measuredResumePolicy().lostGraceMicroseconds + 1;
    host.table.expire(now);

    host.accept();
    (void)client.connect(now);
    pump(host, client, now);
    pump(host, client, now);
    CHECK(host.connection->resumeRefusal() == std::optional { ResumeRefusal::UnknownAttachment });
    (void)host.connection->bindPane(*host.pane.source);
    pumpFor(host, client, now, 200'000);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("y"));
    REQUIRE(client.inDoubt(paneID).size() == 1);
    CHECK_EQ(client.inDoubt(paneID)[0].inputSeq, 1u);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK_EQ(host.connection->counters().inputDuplicates, 0u);

    client.resendInDoubt(paneID, now);
    pumpFor(host, client, now, 100'000);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("y"));
    CHECK_EQ(host.connection->counters().inputDuplicates, 1u);
    CHECK(client.inDoubt(paneID).empty());
    CHECK_EQ(client.pendingCount(paneID), 0u);
}

// The host wrote nothing of this client's, so it reports no count: the event
// lost on the way up is in doubt and is not sent.
TEST(anAbsentInputWrittenNeverTriggersAResend)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("z"), now).has_value());
    pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = false });
    drop(host, client, now);

    reconnect(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(!client.connection()->welcome()->resumed[0].inputWritten.has_value());
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string());
    CHECK_EQ(client.inDoubt(paneID).size(), 1u);
    CHECK_EQ(client.pendingCount(paneID), 0u);
}

// A client restored from a record taken at 100, the host having written 150,
// numbers its next event 151, so the host does not drop it as written.
TEST(aClientRestoredAt100WithTheHostAt150NumbersItsNext151)
{
    Host host({ 41, 42 });
    std::uint64_t now = 1;
    std::optional<HeldAttachment> record;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
        attachFresh(host, first, now);
        for (int event = 1; event <= 150; ++event) {
            CHECK(first.send(paneID, raw("."), now).has_value());
            pump(host, first, now);
            now += 1'000;
            if (event == 100)
                record = first.held();
        }
        pumpFor(host, first, now, 100'000);
        drop(host, first, now);
    }
    REQUIRE(record.has_value());
    REQUIRE(record->panes.size() == 1);
    CHECK_EQ(record->panes[0].lastInputSeq, 100u);
    CHECK_EQ(host.pane.writer.bytes().size(), 150u);

    ClientAttachment restored(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    restored.restore(*record);
    reconnect(host, restored, now);
    REQUIRE(restored.state() == ClientAttachmentState::Live);
    CHECK_EQ(restored.send(paneID, raw("!"), now).value_or(0), 151u);
    pumpFor(host, restored, now, 50'000);
    CHECK_EQ(host.pane.writer.bytes().size(), 151u);
    CHECK_EQ(host.pane.writer.bytes().back(), '!');
}

// The same client typed into before it reconnects numbers that event 101,
// below what the host wrote. No connection took it, so the host's count does
// not swallow it: it goes as 151, and what follows it is numbered after.
TEST(anEventNoConnectionTookIsNumberedPastTheHostsCountNotCountedWritten)
{
    Host host({ 41, 42 });
    std::uint64_t now = 1;
    std::optional<HeldAttachment> record;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
        attachFresh(host, first, now);
        for (int event = 1; event <= 150; ++event) {
            CHECK(first.send(paneID, raw("."), now).has_value());
            pump(host, first, now);
            now += 1'000;
            if (event == 100)
                record = first.held();
        }
        pumpFor(host, first, now, 100'000);
        drop(host, first, now);
    }
    REQUIRE(record.has_value());

    ClientAttachment restored(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    restored.restore(*record);
    CHECK_EQ(restored.send(paneID, raw("!"), now).value_or(0), 101u);
    reconnect(host, restored, now);
    REQUIRE(restored.state() == ClientAttachmentState::Live);
    CHECK_EQ(host.pane.writer.bytes().size(), 151u);
    CHECK_EQ(host.pane.writer.bytes().back(), '!');
    CHECK_EQ(host.connection->counters().inputDuplicates, 0u);
    CHECK_EQ(restored.pendingCount(paneID), 0u);
    CHECK_EQ(restored.send(paneID, raw("?"), now).value_or(0), 152u);
}

// The pane draws into one terminal across transports, and what the host
// wrote meanwhile arrives in it.
TEST(theReplicaIsTheSameTerminalAcrossTransports)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    const Ubiquitty::Terminal* before = client.paneTerminal(paneID);
    REQUIRE(before != nullptr);
    drop(host, client, now);
    CHECK(client.paneTerminal(paneID) == before);
    UbiquittyTest::feed(host.pane.terminal, "while away\r\n");
    host.pane.source->publish(FrameClass::Settled, now);

    reconnect(host, client, now);
    CHECK(client.paneTerminal(paneID) == before);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *before).empty());
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
}

namespace {

/// A client that draws each pane into a terminal of its own, as a frontend
/// whose window owns the terminal it renders does.
struct OwnTerminals final : ClientPaneDelegate {
    Ubiquitty::Terminal terminal;

    OwnTerminals()
    {
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 24, 80);
    }

    /// Lends the client's terminal for the pane.
    Ubiquitty::Terminal* terminalForPane(ID128) override { return &terminal; }
};

} // namespace

// A client that draws into a terminal of its own keeps what is known of it
// across transports as one that leaves the terminal to the attachment does:
// the resume names what the terminal holds, the host continues it with a
// diff, and no checkpoint is sent.
TEST(aClientsOwnTerminalIsContinuedWithADiff)
{
    Host host({ 41, 42 });
    OwnTerminals own;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &own);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(UbiquittyTest::firstDisagreement(host.pane.terminal, own.terminal).empty());
    CHECK(client.paneTerminal(paneID) == &own.terminal);
    drop(host, client, now);
    UbiquittyTest::feed(host.pane.terminal, "while away\r\n");
    host.pane.source->publish(FrameClass::Settled, now);
    reconnect(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, own.terminal).empty());
}

TEST(repeatedResumesKeepTheNewestReplica)
{
    for (const bool isLent : { false, true }) {
        Host host({ 41, 42 });
        OwnTerminals own;
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), isLent ? &own : nullptr);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        const auto* terminal = client.paneTerminal(paneID);
        REQUIRE(terminal);
        for (const auto text : { "first\r\n", "second\r\n", "third\r\n" }) {
            UbiquittyTest::feed(host.pane.terminal, text);
            host.pane.source->publish(FrameClass::Settled, now);
            pumpFor(host, client, now, 100'000);
            REQUIRE(UbiquittyTest::firstDisagreement(host.pane.terminal, *terminal).empty());
            drop(host, client, now);
            reconnect(host, client, now);
            REQUIRE(client.connection() && client.connection()->welcome());
            const auto& resumed = client.connection()->welcome()->resumed;
            REQUIRE(resumed.size() == 1);
            CHECK(resumed.front().paneID == paneID);
            CHECK(resumed.front().outcome == ResumeOutcome::Diffed);
            CHECK_EQ(client.connection()->counters().checkpointsApplied, 0u);
            CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
            CHECK_EQ(client.connection()->counters().digestMismatches, 0u);
            CHECK_EQ(client.connection()->counters().resyncsSent, 0u);
            CHECK(client.paneTerminal(paneID) == terminal);
            CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *terminal).empty());
        }
    }
}

TEST(twoSynchronousResumesKeepTheOuterReaderAliveAndTheNewestReplica)
{
    struct Delegate final : ClientPaneDelegate {
        Host* host = nullptr;
        ClientAttachment* client = nullptr;
        std::uint64_t* now = nullptr;
        unsigned remaining = 0;
        unsigned completed = 0;

        void paneChanged(std::uint32_t, ID128) override
        {
            if (!remaining)
                return;
            --remaining;
            ClientConnection* entered = client->connection();
            REQUIRE(entered->welcome());
            const ID128 issuedSecret = entered->welcome()->resumeSecret;
            drop(*host, *client, *now);
            REQUIRE(client->held());
            CHECK(client->held()->resumeSecret == issuedSecret);
            UbiquittyTest::feed(host->pane.terminal, remaining ? "second\r\n" : "third\r\n");
            host->pane.source->publish(FrameClass::Settled, *now);
            reconnect(*host, *client, *now);
            CHECK(!host->connection->resumeRefusal());
            // This old reader remains alive even when the nested reader also
            // reconnects. Its destructor must not republish an older replica.
            CHECK(entered->state() == ConnectionState::Closed);
            CHECK(client->connection() != entered);
            ++completed;
        }
    } delegate;
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &delegate);
    std::uint64_t now = 1;
    delegate.host = &host;
    delegate.client = &client;
    delegate.now = &now;
    attachFresh(host, client, now);
    delegate.remaining = 2;
    UbiquittyTest::feed(host.pane.terminal, "first\r\n");
    host.pane.source->publish(FrameClass::Settled, now);
    pumpFor(host, client, now, 100'000);
    REQUIRE(delegate.completed == 2);
    const auto* terminal = client.paneTerminal(paneID);
    REQUIRE(terminal);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *terminal).empty());
    drop(host, client, now);
    reconnect(host, client, now);
    REQUIRE(client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed.front().outcome == ResumeOutcome::Diffed);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *terminal).empty());
}

// What is known of a client's own terminal is known of that terminal as the
// last connection left it: a pane the client then draws elsewhere, and one it
// wrote to itself, as a local clear does, are each sent whole.
TEST(aChangedOrReplacedOwnTerminalIsSentWhole)
{
    for (const bool isReplaced : { true, false }) {
        Host host({ 41, 42 });
        OwnTerminals own;
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &own);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        drop(host, client, now);
        OwnTerminals other;
        if (isReplaced)
            client.setPaneDelegate(&other);
        else
            UbiquittyTest::feed(own.terminal, "written here\r\n");
        UbiquittyTest::feed(host.pane.terminal, "while away\r\n");
        host.pane.source->publish(FrameClass::Settled, now);
        reconnect(host, client, now);
        REQUIRE(client.connection() && client.connection()->welcome());
        REQUIRE(client.connection()->welcome()->resumed.size() == 1);
        CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Checkpointed);
        CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, isReplaced ? other.terminal : own.terminal).empty());
    }
}

// A terminal lent after the pane was bound, as a pane core that attaches to
// a carrier already serving its pane lends one, is continued as one lent at
// the bind is.
TEST(aTerminalLentAfterTheBindIsContinuedWithADiff)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    OwnTerminals own;
    client.connection()->setPaneTerminal(client.connection()->handleForPane(paneID), &own.terminal);
    pumpFor(host, client, now, 200'000);
    REQUIRE(UbiquittyTest::firstDisagreement(host.pane.terminal, own.terminal).empty());
    drop(host, client, now);
    client.setPaneDelegate(&own);
    UbiquittyTest::feed(host.pane.terminal, "while away\r\n");
    host.pane.source->publish(FrameClass::Settled, now);
    reconnect(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, own.terminal).empty());
}

// An unplanned restart ends a pane drawn into the client's own terminal as
// it ends any other, but the ended pane keeps no terminal: the terminal was
// lent, and the new instance's pane is drawn into it whole.
TEST(anUnplannedRestartEndsALentPaneAndKeepsNoTerminal)
{
    OwnTerminals own;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &own);
    std::uint64_t now = 1;
    {
        Host old({ 41, 42 });
        attachFresh(old, client, now);
        drop(old, client, now);
    }
    Host restarted({ 51, 52 });
    UbiquittyTest::feed(restarted.pane.terminal, "new session\r\n");
    restarted.pane.source->publish(FrameClass::Settled, now);
    restarted.accept();
    (void)client.connect(now);
    pump(restarted, client, now);
    pump(restarted, client, now);
    REQUIRE(client.endedPanes().size() == 1);
    CHECK(client.endedPanes()[0].reason == ReplicaSet::EndReason::HostRestarted);
    CHECK(client.endedPanes()[0].held->picture() == nullptr);
    (void)restarted.connection->bindPane(*restarted.pane.source);
    pumpFor(restarted, client, now, 200'000);
    CHECK(client.paneTerminal(paneID) == &own.terminal);
    CHECK_EQ(restarted.connection->counters().checkpointsSent, 1u);
    CHECK(UbiquittyTest::firstDisagreement(restarted.pane.terminal, own.terminal).empty());
}

// A host that restarted unplanned ends the old pane, which stays readable
// and labelled with its instance; the new pane, under the same ID, gets a
// terminal of its own, and nothing typed for the old pane reaches it.
TEST(anUnplannedHostRestartEndsTheOldPaneAndAttachesFresh)
{
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    const ID128 oldInstance { 41, 42 };
    const ID128 newInstance { 51, 52 };
    const Ubiquitty::Terminal* oldTerminal = nullptr;
    {
        Host old(oldInstance);
        UbiquittyTest::feed(old.pane.terminal, "old session\r\n");
        old.pane.source->publish(FrameClass::Settled, now);
        attachFresh(old, client, now);
        oldTerminal = client.paneTerminal(paneID);
        REQUIRE(oldTerminal != nullptr);
        CHECK(UbiquittyTest::firstDisagreement(old.pane.terminal, *oldTerminal).empty());
        CHECK(client.send(paneID, raw("q"), now).has_value());
        pumpFor(old, client, now, 20'000, { .shouldCarryDown = false, .shouldCarryUp = false });
        drop(old, client, now);
    }

    Host restarted(newInstance);
    UbiquittyTest::feed(restarted.pane.terminal, "new session\r\n");
    restarted.pane.source->publish(FrameClass::Settled, now);
    restarted.accept();
    (void)client.connect(now);
    pump(restarted, client, now);
    pump(restarted, client, now);
    CHECK(restarted.connection->resumeRefusal() == std::optional { ResumeRefusal::UnknownAttachment });
    REQUIRE(client.endedPanes().size() == 1);
    CHECK(client.endedPanes()[0].hostInstanceID == oldInstance);
    CHECK(client.endedPanes()[0].paneID == paneID);
    CHECK(client.endedPanes()[0].held->terminal.get() == oldTerminal);
    CHECK(client.endedPanes()[0].reason == ReplicaSet::EndReason::HostRestarted);
    CHECK_EQ(client.endedPanes()[0].discardedInputEvents, 1u);
    CHECK(client.paneTerminal(paneID) == nullptr);

    (void)restarted.connection->bindPane(*restarted.pane.source);
    pumpFor(restarted, client, now, 200'000);
    const Ubiquitty::Terminal* fresh = client.paneTerminal(paneID);
    REQUIRE(fresh != nullptr);
    CHECK(fresh != oldTerminal);
    CHECK(UbiquittyTest::firstDisagreement(restarted.pane.terminal, *fresh).empty());
    CHECK(!UbiquittyTest::firstDisagreement(restarted.pane.terminal, *client.endedPanes()[0].held->terminal).empty());
    CHECK_STR_EQ(restarted.pane.writer.bytes(), std::string());
    CHECK(!client.isEnded(paneID));
    CHECK(client.inDoubt(paneID).empty());
    CHECK(client.held().has_value() && client.held()->hostInstanceID == newInstance);
}

namespace {

/// A client whose event `z` was lost on the way up at a drop, resumed with
/// no count from the host: `z` is in doubt.
struct InDoubt {
    Host host { { 41, 42 } };
    ClientAttachment client { UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity() };
    std::uint64_t now = 1;

    InDoubt()
    {
        attachFresh(host, client, now);
        CHECK(client.send(paneID, raw("z"), now).has_value());
        pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = false });
        drop(host, client, now);
        reconnect(host, client, now);
        CHECK_EQ(client.inputStatus(paneID).inDoubt, 1u);
    }
};

} // namespace

// The host writes one client's input to a pane in the order of its numbers,
// and drops a number at or below one it wrote: an event in doubt is never
// overtaken, so the highest number written tells written from not.
TEST(aPaneWritesInNumberOrderAndDropsANumberBelowOneWritten)
{
    OwnedPane pane;
    const auto queue = [&](std::uint64_t inputSeq, const char* text) { return pane.source->queueInput(clientInstance, inputSeq, raw(text), 1); };
    CHECK(queue(1, "a") == EnqueueOutcome::Queued);
    CHECK(queue(2, "b") == EnqueueOutcome::Queued);
    CHECK(queue(3, "c") == EnqueueOutcome::Queued);
    pane.source->drainInput(1);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("abc"));
    CHECK(queue(2, "B") == EnqueueOutcome::Duplicate);
    CHECK(queue(5, "e") == EnqueueOutcome::Queued);
    CHECK(queue(4, "d") == EnqueueOutcome::Duplicate);
    pane.source->drainInput(1);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("abce"));
}

// Input typed while an event is in doubt waits behind it: nothing of the
// pane's goes until the person decides.
TEST(newInputIsHeldWhileAnEventIsInDoubt)
{
    InDoubt doubt;
    CHECK_EQ(doubt.client.send(paneID, raw("n"), doubt.now).value_or(0), 2u);
    pumpFor(doubt.host, doubt.client, doubt.now, 100'000);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string());
    const PaneInputStatus status = doubt.client.inputStatus(paneID);
    CHECK_EQ(status.inDoubt, 1u);
    CHECK_EQ(status.waiting, 1u);
    CHECK(status.isAwaitingDecision());
    // What waits counts against the bound, attached or not.
    const std::string chunk(1024, 'w');
    std::optional<InputRefusal> refusal;
    for (int event = 0; event < 80 && !refusal; ++event) {
        if (const auto sent = doubt.client.send(paneID, raw(chunk), doubt.now); !sent)
            refusal = sent.error();
    }
    CHECK(refusal == std::optional { InputRefusal::LostQueueFull });
}

// Resent, the event in doubt goes first and what waited follows, each
// written once.
TEST(aResendThenTheHeldInputIsDeliveredInOrderOnce)
{
    InDoubt doubt;
    CHECK(doubt.client.send(paneID, raw("n"), doubt.now).has_value());
    pumpFor(doubt.host, doubt.client, doubt.now, 50'000);
    // The decision sends at once, not at the host's next frame.
    doubt.client.resendInDoubt(paneID, doubt.now);
    doubt.host.connection->receiveStream(doubt.client.takeStreamOutput(), doubt.now);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("zn"));
    pumpFor(doubt.host, doubt.client, doubt.now, 100'000);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("zn"));
    CHECK_EQ(doubt.host.connection->counters().inputDuplicates, 0u);
    const PaneInputStatus status = doubt.client.inputStatus(paneID);
    CHECK_EQ(status.inDoubt, 0u);
    CHECK_EQ(status.waiting, 0u);
    CHECK_EQ(status.discarded, 0u);
    CHECK_EQ(doubt.client.pendingCount(paneID), 0u);
}

// Discarded, the event in doubt is counted and what waited goes.
TEST(aDiscardThenTheHeldInputIsDeliveredWithTheDiscardCounted)
{
    InDoubt doubt;
    CHECK(doubt.client.send(paneID, raw("n"), doubt.now).has_value());
    pumpFor(doubt.host, doubt.client, doubt.now, 50'000);
    // The decision sends at once, not at the host's next frame.
    doubt.client.discardInDoubt(paneID, doubt.now);
    doubt.host.connection->receiveStream(doubt.client.takeStreamOutput(), doubt.now);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("n"));
    pumpFor(doubt.host, doubt.client, doubt.now, 100'000);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("n"));
    const PaneInputStatus status = doubt.client.inputStatus(paneID);
    CHECK_EQ(status.inDoubt, 0u);
    CHECK_EQ(status.waiting, 0u);
    CHECK_EQ(status.discarded, 1u);
}

// A pane the host no longer has ends, and its unwritten input is discarded
// and counted, never sent anywhere.
TEST(aGonePanesInputIsDiscardedAndCounted)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("q"), now).has_value());
    pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = false });
    drop(host, client, now);
    host.hasPane = false;
    reconnect(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Gone);
    CHECK(client.isEnded(paneID));
    REQUIRE(client.endedPanes().size() == 1);
    CHECK(client.endedPanes()[0].reason == ReplicaSet::EndReason::PaneGone);
    CHECK_EQ(client.endedPanes()[0].discardedInputEvents, 1u);
    CHECK_EQ(client.inputStatus(paneID).discarded, 1u);
    CHECK(!client.send(paneID, raw("r"), now).has_value());
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string());
}

namespace {

/// A host instance with eight panes.
struct EightPanes {
    static constexpr std::size_t count = 8;
    SequenceSecrets secrets;
    AttachmentTable table { { 41, 42 }, measuredResumePolicy(), secrets };
    std::array<Ubiquitty::Terminal, count> terminals;
    std::vector<std::unique_ptr<PaneSource>> sources;
    std::unique_ptr<HostConnection> connection;
    std::uint64_t connectionNumber = 0;

    EightPanes()
    {
        for (std::size_t index = 0; index < count; ++index) {
            Ubiquitty::resize(terminals[index], 24, 80);
            sources.push_back(std::make_unique<PaneSource>(terminals[index], idOf(index)));
            UbiquittyTest::feed(terminals[index], "$ ");
            sources.back()->publish(FrameClass::Structural, 1);
        }
    }

    static ID128 idOf(std::size_t index) { return { 100 + index, 1 }; }

    void accept()
    {
        connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
        connection->serveFromTable(table, owner, ++connectionNumber, [this](ID128 id) -> PaneSource* {
            const auto found = std::ranges::find(sources, id, [](const std::unique_ptr<PaneSource>& source) { return source->paneID(); });
            return found == sources.end() ? nullptr : found->get();
        });
    }

    void lose(std::uint64_t now)
    {
        if (connection && connection->attachment())
            table.noteTransportLost(connection->attachment()->attachmentID, connectionNumber, now);
        connection.reset();
    }
};

} // namespace

// A resume names each pane's view: with one of eight panes shown, the
// client is sent one diff and nothing for the other seven, and sends no
// View to say so (Docs/sync.md §6.8).
TEST(aResumeNamingEightPanesOneLiveSendsOneDiffAndNothingElse)
{
    EightPanes host;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 2;
    host.accept();
    (void)client.connect(now);
    pump(host, client, now);
    pump(host, client, now);
    for (const std::unique_ptr<PaneSource>& source : host.sources)
        (void)host.connection->bindPane(*source);
    pumpFor(host, client, now, 100'000);
    for (std::size_t index = 1; index < EightPanes::count; ++index) {
        const ID128 hidden = EightPanes::idOf(index);
        client.setView(hidden, ViewState::Dormant);
        CHECK(client.connection()->paneView(client.connection()->handleForPane(hidden)) == std::optional { ViewState::Dormant });
    }
    pumpFor(host, client, now, 100'000);
    drop(host, client, now);
    for (std::size_t index = 0; index < EightPanes::count; ++index) {
        UbiquittyTest::feed(host.terminals[index], "changed while away\r\n");
        host.sources[index]->publish(FrameClass::Settled, now);
    }

    host.accept();
    (void)client.connect(now);
    std::size_t viewsSent = 0;
    for (const std::uint64_t until = now + 200'000; now < until; now += 1'000) {
        host.connection->service(now);
        std::vector<std::uint8_t> down;
        while (host.connection->popStreamFrame(down, now)) {
        }
        client.receiveStream(down, now);
        client.tick(now);
        const std::vector<std::uint8_t> up = client.takeStreamOutput();
        FrameDecoder decoder;
        decoder.append(up);
        while (const std::optional<Frame> frame = decoder.next())
            viewsSent += frame->kind == FrameKind::View ? 1 : 0;
        host.connection->receiveStream(up, now);
    }
    REQUIRE(client.connection() && client.connection()->welcome());
    CHECK_EQ(client.connection()->welcome()->resumed.size(), EightPanes::count);
    CHECK_EQ(viewsSent, 0u);
    const HostCounters& counters = host.connection->counters();
    CHECK_EQ(counters.checkpointsSent, 0u);
    CHECK_EQ(counters.chainUpdatesSent + counters.supersetUpdatesSent, 1u);
    CHECK(UbiquittyTest::firstDisagreement(host.terminals[0], *client.paneTerminal(EightPanes::idOf(0))).empty());
    for (std::size_t index = 1; index < EightPanes::count; ++index)
        CHECK(client.view(EightPanes::idOf(index)) == std::optional { ViewState::Dormant });
}

namespace {

/// Drops the client, lets the host forget the attachment, and connects
/// again: the resume is refused and the host binds the pane fresh.
void reattachAfterTheGrace(Host& host, ClientAttachment& client, std::uint64_t& now)
{
    drop(host, client, now);
    now += measuredResumePolicy().lostGraceMicroseconds + 1;
    host.table.expire(now);
    host.accept();
    (void)client.connect(now);
    pump(host, client, now);
    pump(host, client, now);
    CHECK(host.connection->resumeRefusal() == std::optional { ResumeRefusal::UnknownAttachment });
    (void)host.connection->bindPane(*host.pane.source);
    pumpFor(host, client, now, 200'000);
}

} // namespace

// A refused resume binds the pane fresh, Dormant on the host: the client
// shows it as before, and it is sent its checkpoint.
TEST(aRefusedResumeShowsThePaneItHadShown)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    UbiquittyTest::feed(host.pane.terminal, "after\r\n");
    host.pane.source->publish(FrameClass::Settled, now);
    reattachAfterTheGrace(host, client, now);
    CHECK_EQ(host.connection->counters().checkpointsSent, 1u);
    REQUIRE(client.paneTerminal(paneID) != nullptr);
    CHECK(UbiquittyTest::firstDisagreement(host.pane.terminal, *client.paneTerminal(paneID)).empty());
}

// A pane the client keeps hidden stays hidden when a refused resume binds
// it fresh: it costs no checkpoint.
TEST(aRefusedResumeKeepsAHiddenPaneHidden)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    client.setView(paneID, ViewState::Dormant);
    pumpFor(host, client, now, 50'000);
    reattachAfterTheGrace(host, client, now);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    REQUIRE(client.connection() != nullptr);
    CHECK(client.connection()->paneView(client.connection()->handleForPane(paneID)) == std::optional { ViewState::Dormant });
}

// Input typed while no connection carried the pane cannot have reached the
// host, so a refused resume puts none of it in doubt: it goes, once, when
// the host binds the pane again.
TEST(aRefusedResumePutsNothingInDoubtThatNoConnectionTook)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    drop(host, client, now);
    CHECK(client.send(paneID, raw("w"), now).has_value());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);
    reattachAfterTheGrace(host, client, now);
    CHECK(client.inDoubt(paneID).empty());
    CHECK(!client.inputStatus(paneID).isAwaitingDecision());
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("w"));
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK_EQ(host.connection->counters().inputDuplicates, 0u);
}

namespace {

/// A client whose `z` left on a connection and was lost on the way up, and
/// whose `w` was typed with no connection at all, resumed with no count from
/// the host: only `z` is in doubt.
struct InDoubtAheadOfUnsent {
    Host host { { 41, 42 } };
    ClientAttachment client { UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity() };
    std::uint64_t now = 1;

    InDoubtAheadOfUnsent()
    {
        attachFresh(host, client, now);
        CHECK(client.send(paneID, raw("z"), now).has_value());
        pumpFor(host, client, now, 50'000, { .shouldCarryDown = false, .shouldCarryUp = false });
        drop(host, client, now);
        CHECK(client.send(paneID, raw("w"), now).has_value());
        reconnect(host, client, now);
    }
};

} // namespace

// A resume with no count puts in doubt only what a connection took; what
// none took waits behind it rather than going first.
TEST(onlyWhatAConnectionTookIsInDoubtAndWhatNoneTookWaitsBehindIt)
{
    InDoubtAheadOfUnsent doubt;
    REQUIRE(doubt.client.connection() && doubt.client.connection()->welcome());
    REQUIRE(doubt.client.connection()->welcome()->resumed.size() == 1);
    CHECK(!doubt.client.connection()->welcome()->resumed[0].inputWritten.has_value());
    REQUIRE(doubt.client.inDoubt(paneID).size() == 1);
    CHECK_EQ(doubt.client.inDoubt(paneID)[0].inputSeq, 1u);
    const PaneInputStatus status = doubt.client.inputStatus(paneID);
    CHECK_EQ(status.inDoubt, 1u);
    CHECK_EQ(status.waiting, 1u);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string());
}

// Resent, the event in doubt goes first and what no connection took follows.
TEST(aResendSendsWhatWasInDoubtThenWhatNoConnectionTook)
{
    InDoubtAheadOfUnsent doubt;
    REQUIRE(doubt.client.inputStatus(paneID).inDoubt == 1);
    doubt.client.resendInDoubt(paneID, doubt.now);
    pumpFor(doubt.host, doubt.client, doubt.now, 100'000);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("zw"));
    CHECK_EQ(doubt.host.connection->counters().inputDuplicates, 0u);
    CHECK_EQ(doubt.client.pendingCount(paneID), 0u);
    CHECK_EQ(doubt.client.inputStatus(paneID).discarded, 0u);
}

// Discarded, only the event in doubt is counted; what no connection took
// still goes.
TEST(aDiscardCountsWhatWasInDoubtAndSendsWhatNoConnectionTook)
{
    InDoubtAheadOfUnsent doubt;
    REQUIRE(doubt.client.inputStatus(paneID).inDoubt == 1);
    doubt.client.discardInDoubt(paneID, doubt.now);
    pumpFor(doubt.host, doubt.client, doubt.now, 100'000);
    CHECK_STR_EQ(doubt.host.pane.writer.bytes(), std::string("w"));
    CHECK_EQ(doubt.client.pendingCount(paneID), 0u);
    CHECK_EQ(doubt.client.inputStatus(paneID).discarded, 1u);
}

// A raw event with no bytes is no event: the wire has no empty `Raw`, so the
// attachment numbers none, and the input after it is not held behind an
// event no connection would send.
TEST(anEmptyRawEventIsRefusedAndHoldsNothingBack)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    const auto empty = client.send(paneID, raw(""), now);
    CHECK(!empty && empty.error() == InputRefusal::Unsendable);
    CHECK_EQ(client.send(paneID, raw("a"), now).value_or(0), 1u);
    pumpFor(host, client, now, 50'000);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("a"));
    CHECK_EQ(client.pendingCount(paneID), 0u);
}

// While no connection carries input it queues up to 64 KiB; past that an
// event is refused and its sender told, and what was queued is written in
// order once the attachment resumes.
TEST(lostInputPastItsBoundIsRefusedNotDropped)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    drop(host, client, now);

    const std::string chunk(1024, 'k');
    std::size_t accepted = 0;
    std::optional<InputRefusal> refusal;
    for (int event = 0; event < 80 && !refusal; ++event) {
        const auto sent = client.send(paneID, raw(chunk), now);
        if (sent)
            ++accepted;
        else
            refusal = sent.error();
    }
    CHECK_EQ(accepted, maxLostInputBytes / chunk.size());
    CHECK(refusal == std::optional { InputRefusal::LostQueueFull });

    reconnect(host, client, now);
    CHECK_EQ(host.pane.writer.bytes().size(), 1 + accepted * chunk.size());
    CHECK_EQ(client.pendingCount(paneID), 0u);
    // The queue drained, so the next event is taken.
    CHECK(client.send(paneID, raw(chunk), now).has_value());
}

// The owner half of an owed Ack riding input (Docs/sync.md §6.4): an update
// arrives and its Ack is not yet due; the person types at that instant; the
// one output the attachment hands its carrier holds the input and the Ack
// together. Before the input the output held no Ack, so the Ack did not leave
// on a write of its own.
TEST(anOwedAckLeavesInTheSameWriteAsTheInputAfterIt)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    pumpFor(host, client, now, 200'000);
    REQUIRE(client.connection());
    const auto kindsOf = [](std::span<const std::uint8_t> output) {
        std::vector<FrameKind> kinds;
        FrameDecoder decoder;
        decoder.append(output);
        while (const std::optional<Frame> frame = decoder.next())
            kinds.push_back(frame->kind);
        return kinds;
    };

    UbiquittyTest::feed(host.pane.terminal, "output");
    host.pane.source->publish(FrameClass::Settled, now);
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    const std::uint64_t appliedBefore = client.connection()->counters().updatesApplied;
    client.receiveStream(down, now);
    REQUIRE(client.connection()->counters().updatesApplied > appliedBefore);
    client.tick(now);
    const std::vector<FrameKind> beforeInput = kindsOf(client.takeStreamOutput());
    CHECK_MSG(std::ranges::find(beforeInput, FrameKind::Ack) == beforeInput.end(), "premise: the update's Ack left before any input");

    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    const std::vector<FrameKind> withInput = kindsOf(client.takeStreamOutput());
    CHECK_MSG(std::ranges::find(withInput, FrameKind::Raw) != withInput.end(), "the input did not leave");
    CHECK_MSG(std::ranges::find(withInput, FrameKind::Ack) != withInput.end(), "the owed Ack did not leave in the write that carried the input");
}

UBIQUITTY_TEST_MAIN()
