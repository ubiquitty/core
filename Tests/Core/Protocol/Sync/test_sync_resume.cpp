// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_resume — attachments outlive their connections (Docs/sync.md
// §6.9): a resume is admitted only for this host's attachment, under its
// principal, with the secret the last Welcome issued, and continues each pane
// from the baseline the client still holds rather than with a checkpoint.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::OwnedPane;

namespace {

/// Secrets drawn in sequence, so a test knows what was issued.
class SequenceSecrets final : public SecretSource {
public:
    [[nodiscard]] ID128 nextSecret() override { return { ++m_next, 0xC0FFEE }; }

private:
    std::uint64_t m_next = 0;
};

constexpr ID128 hostInstance { 41, 42 };
constexpr ID128 paneID { 9, 10 };
constexpr ID128 otherPaneID { 11, 12 };
constexpr ID128 clientInstance { 7, 8 };

/// One connection between a client and the host, over an instant link.
struct Session {
    HostConnection host;
    ClientConnection client;
    std::uint64_t streamBytes = 0;

    Session(AttachmentTable& table, ReplicaSet& replicas, PaneSource& source, ConnectionAuthority authority, std::uint64_t connection, std::optional<ResumeRequest> resume, PaneSource* otherSource = nullptr)
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "resume-fixture.sock", .buildFlags = 0, .hostPlatform = "test" } })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = clientInstance, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts(), .resume = std::move(resume) }, nullptr, nullptr, &replicas)
    {
        host.serveFromTable(table, std::move(authority), connection, [&source, otherSource](ID128 id) -> PaneSource* {
            if (id == paneID)
                return &source;
            return id == otherPaneID ? otherSource : nullptr;
        });
        client.start();
    }

    void pump(std::uint64_t now)
    {
        host.service(now);
        std::vector<std::uint8_t> down;
        while (host.popStreamFrame(down, now)) {
        }
        streamBytes += down.size();
        client.receiveStream(down, now);
        client.tick(now);
        const std::vector<std::uint8_t> up = client.takeStreamOutput();
        streamBytes += up.size();
        host.receiveStream(up, now);
    }

    void pumpUntil(std::uint64_t& now, std::uint64_t until)
    {
        for (; now < until; now += 1'000)
            pump(now);
    }
};

/// What the client holds of the pane, as a resume names it.
ResumeRequest resumeOf(ReplicaSet& replicas, ID128 attachmentID, std::optional<ID128> secret)
{
    ResumeRequest request { .attachmentID = attachmentID, .resumeSecret = secret, .panes = {} };
    if (ReplicaSet::Held* held = replicas.find(paneID)) {
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = ViewState::Live });
    }
    return request;
}

const ConnectionAuthority owner { .principal = "alice", .mayDrive = true };

/// A table, a pane and a client's replicas, attached once: the first
/// connection is gone and the attachment `Lost`.
struct Dropped {
    SequenceSecrets secrets;
    AttachmentTable table { hostInstance, measuredResumePolicy(), secrets };
    OwnedPane pane;
    ReplicaSet replicas;
    ID128 attachmentID;
    ID128 secret;
    std::uint64_t now = 1;

    explicit Dropped(int writes = 3, int bells = 0)
    {
        Session first(table, replicas, *pane.source, owner, 1, std::nullopt);
        first.pump(now);
        first.pump(now);
        (void)first.host.bindPane(*pane.source);
        first.pumpUntil(now, now + 100'000);
        for (int write = 0; write < writes; ++write) {
            UbiquittyTest::feed(pane.terminal, "line " + std::to_string(write) + "\r\n");
            pane.source->publish(FrameClass::Settled, now);
            first.pumpUntil(now, now + 100'000);
        }
        for (int bell = 0; bell < bells; ++bell) {
            REQUIRE(pane.source->appendEffect(BellEffect {}, clientInstance));
            first.pumpUntil(now, now + 100'000);
        }
        CHECK_EQ(replicas.effectReceipt(paneID).acknowledged, static_cast<std::uint64_t>(bells));
        REQUIRE(first.client.welcome().has_value());
        attachmentID = first.client.welcome()->attachmentID;
        secret = first.client.welcome()->resumeSecret;
        CHECK(first.client.paneGeneration(1) == pane.source->generation());
        first.client.storeReplicas();
        table.noteTransportLost(attachmentID, 1, now);
    }
};

} // namespace

// A resume from another principal is refused, and the record is as it was;
// the right principal then resumes it.
TEST(aResumeOverAnotherPrincipalIsRefusedAndChangesNothing)
{
    Dropped dropped;
    const AttachmentRecord before = [&] {
        const AttachmentRecord& record = *dropped.table.find(dropped.attachmentID);
        AttachmentRecord copy;
        copy.attachmentID = record.attachmentID;
        copy.resumeSecret = record.resumeSecret;
        copy.authority = record.authority;
        copy.state = record.state;
        copy.servingConnection = record.servingConnection;
        copy.lostSinceMicroseconds = record.lostSinceMicroseconds;
        return copy;
    }();
    const auto result = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), { .principal = "mallory", .mayDrive = true }, 2, dropped.now);
    REQUIRE(!result.has_value());
    CHECK(result.error() == ResumeRefusal::OtherPrincipal);
    const AttachmentRecord& after = *dropped.table.find(dropped.attachmentID);
    CHECK(after.resumeSecret == before.resumeSecret);
    CHECK(after.authority == before.authority);
    CHECK(after.state == before.state);
    CHECK(after.servingConnection == before.servingConnection);
    CHECK(after.lostSinceMicroseconds == before.lostSinceMicroseconds);
    CHECK(dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 3, dropped.now).has_value());
}

// The wrong secret, a consumed one, and another instance's attachment are
// refused.
TEST(aWrongSecretIsRefused)
{
    Dropped dropped;
    const ID128 wrong { dropped.secret.low + 1, dropped.secret.high };
    const auto refused = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, wrong), owner, 2, dropped.now);
    CHECK(!refused && refused.error() == ResumeRefusal::WrongSecret);
    const auto withNone = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, std::nullopt), owner, 2, dropped.now);
    CHECK(!withNone && withNone.error() == ResumeRefusal::WrongSecret);
    CHECK(dropped.table.find(dropped.attachmentID)->state == AttachmentState::Lost);
}

TEST(aConsumedSecretIsRefused)
{
    Dropped dropped;
    REQUIRE(dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 2, dropped.now).has_value());
    CHECK(dropped.table.find(dropped.attachmentID)->resumeSecret != dropped.secret);
    // Once the resumed client has spoken, the consumed secret is a wrong one.
    dropped.table.noteFirstFrame(dropped.attachmentID, 2);
    dropped.table.noteTransportLost(dropped.attachmentID, 2, dropped.now);
    const auto replayed = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 3, dropped.now);
    CHECK(!replayed && replayed.error() == ResumeRefusal::WrongSecret);
}

TEST(anAttachmentFromAnotherInstanceIsUnknown)
{
    Dropped dropped;
    ID128 elsewhere = dropped.attachmentID;
    elsewhere.high ^= 1;
    const auto refused = dropped.table.resume(resumeOf(dropped.replicas, elsewhere, dropped.secret), owner, 2, dropped.now);
    CHECK(!refused && refused.error() == ResumeRefusal::UnknownAttachment);
}

// Past the grace the attachment is gone; what was written to the pane is the
// pane's and outlives it.
TEST(aResumeAfterTheGraceIsRefusedAndLastWrittenSurvives)
{
    Dropped dropped;
    const std::uint64_t grace = measuredResumePolicy().lostGraceMicroseconds;
    CHECK(dropped.table.nextExpiryMicroseconds() == std::optional<std::uint64_t> { dropped.now + grace });
    const auto late = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 2, dropped.now + grace);
    CHECK(!late && late.error() == ResumeRefusal::UnknownAttachment);
    CHECK(dropped.table.find(dropped.attachmentID) == nullptr);
}

// An attachment whose connection idles with nothing outstanding stays Live.
TEST(anIdleLiveAttachmentNeverEntersLost)
{
    SequenceSecrets secrets;
    AttachmentTable table(hostInstance, measuredResumePolicy(), secrets);
    OwnedPane pane;
    ReplicaSet replicas;
    Session session(table, replicas, *pane.source, owner, 1, std::nullopt);
    std::uint64_t now = 1;
    session.pump(now);
    session.pump(now);
    (void)session.host.bindPane(*pane.source);
    session.pumpUntil(now, 200'000);
    // Five idle minutes, the client answering probes as it does.
    for (now = 200'000; now < 300'000'000; now += 500'000)
        session.pump(now);
    REQUIRE(session.host.attachment() != nullptr);
    CHECK(session.host.attachment()->state == AttachmentState::Live);
    CHECK(session.host.state() == ConnectionState::Attached);
}

// A resume after a drop continues the pane with a diff, and the client's
// picture is the host's after one update.
TEST(aResumeAfterADropSendsADiffNotACheckpoint)
{
    Dropped dropped;
    UbiquittyTest::feed(dropped.pane.terminal, "while away\r\n");
    dropped.pane.source->publish(FrameClass::Settled, dropped.now);
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(second.client.welcome().has_value());
    REQUIRE(second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.host.counters().checkpointsSent, 0u);
    CHECK(second.host.counters().chainUpdatesSent + second.host.counters().supersetUpdatesSent >= 1);
    const Ubiquitty::Terminal* replica = second.client.paneTerminal(1);
    REQUIRE(replica != nullptr);
    CHECK_MSG(UbiquittyTest::firstDisagreement(dropped.pane.terminal, *replica).empty(), UbiquittyTest::firstDisagreement(dropped.pane.terminal, *replica).c_str());
    CHECK(second.client.welcome()->attachmentID == dropped.attachmentID);
}

TEST(anInheritedAttachmentResumesThroughTheHostWithoutACheckpoint)
{
    Dropped dropped;
    REQUIRE(dropped.pane.source->queueInput(clientInstance, 17, RawInput { .bytes = { 'x' } }, dropped.now, dropped.attachmentID) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(dropped.pane.writer.bytes(), std::string("x"));
    auto carried = dropped.table.exportForHandover(dropped.now);
    SequenceSecrets successorSecrets;
    (void)successorSecrets.nextSecret();
    const ID128 successorID { 51, 52 };
    AttachmentTable successor(successorID, measuredResumePolicy(), successorSecrets);
    REQUIRE(successor.inherit(std::move(carried), hostInstance, dropped.now).has_value());
    REQUIRE(successor.find(dropped.attachmentID));
    CHECK(successor.find(dropped.attachmentID)->issuer == hostInstance);
    CHECK(successor.find(dropped.attachmentID)->state == AttachmentState::Lost);
    UbiquittyTest::feed(dropped.pane.terminal, "after handover\r\n");
    dropped.pane.source->publish(FrameClass::Settled, dropped.now);
    Session resumed(successor, dropped.replicas, *dropped.pane.source, owner, 2,
        resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    resumed.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(resumed.client.welcome());
    REQUIRE(resumed.client.welcome()->resumed.size() == 1);
    const auto& welcome = *resumed.client.welcome();
    CHECK(welcome.hostInstanceID == successorID);
    CHECK(welcome.predecessorHostInstanceID == std::optional { hostInstance });
    CHECK(welcome.attachmentID == dropped.attachmentID);
    CHECK(welcome.resumeSecret != dropped.secret);
    CHECK(welcome.resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(welcome.resumed[0].inputWritten == std::optional<std::uint64_t> { 17 });
    CHECK_EQ(resumed.host.counters().checkpointsSent, 0u);
    CHECK_EQ(resumed.host.counters().historyBytesSent, 0u);
    CHECK(successor.find(dropped.attachmentID)->issuer == successorID);
    const auto* picture = resumed.client.paneTerminal(1);
    REQUIRE(picture);
    CHECK(UbiquittyTest::firstDisagreement(dropped.pane.terminal, *picture).empty());
}

TEST(inheritedResumeRetainsItsPrincipalSecretAndAbsoluteGrace)
{
    Dropped dropped;
    const auto lostAt = dropped.now;
    const auto nineMinutes = 540'000'000ULL;
    SequenceSecrets secrets;
    AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(dropped.table.exportForHandover(lostAt + nineMinutes), hostInstance, lostAt + nineMinutes));
    const auto* held = successor.find(dropped.attachmentID);
    REQUIRE(held);
    CHECK_EQ(held->lostSinceMicroseconds, lostAt);
    const auto request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    const auto other = successor.resume(request, { .principal = "mallory", .mayDrive = true }, 2, lostAt + nineMinutes);
    CHECK(!other && other.error() == ResumeRefusal::OtherPrincipal);
    auto wrong = request;
    wrong.resumeSecret->low ^= 1;
    const auto rejected = successor.resume(wrong, owner, 2, lostAt + nineMinutes);
    CHECK(!rejected && rejected.error() == ResumeRefusal::WrongSecret);
    CHECK(held->resumeSecret == dropped.secret);
    CHECK(held->authority == owner);
    CHECK(held->state == AttachmentState::Lost);
    CHECK(!successor.isResumableIssuer({ 99, 100 }));
    successor.expire(lostAt + nineMinutes + 61'000'000);
    CHECK(!successor.find(dropped.attachmentID));
}

TEST(inheritedProofUsesTheIssuerOfTheCurrentSecret)
{
    Dropped dropped;
    SequenceSecrets secrets;
    const ID128 successorID { 51, 52 };
    AttachmentTable successor(successorID, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(dropped.table.exportForHandover(dropped.now), hostInstance, dropped.now));
    const std::array<std::uint8_t, 32> exporter { 17 };
    ResumeProof proof { .attachmentID = dropped.attachmentID,
        .mac = resumeProofMAC(dropped.secret, exporter, dropped.attachmentID, successorID) };
    const auto wrong = successor.admitProof(proof, exporter, 2, dropped.now);
    CHECK(!wrong && wrong.error() == ResumeRefusal::WrongSecret);
    CHECK(!successor.provenAttachment(2));
    proof.mac = resumeProofMAC(dropped.secret, exporter, dropped.attachmentID, hostInstance);
    REQUIRE(successor.admitProof(proof, exporter, 2, dropped.now));
    CHECK(successor.provenAttachment(2) == std::optional { dropped.attachmentID });
    REQUIRE(successor.resume(resumeOf(dropped.replicas, dropped.attachmentID, std::nullopt), owner, 2, dropped.now));
    CHECK(successor.find(dropped.attachmentID)->issuer == successorID);
}

TEST(handoverCarriesOnlyTheUnconsumedSecretAndNoPhysicalHandshake)
{
    Dropped dropped;
    const auto resumed = dropped.table.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 2, dropped.now);
    REQUIRE(resumed);
    const auto currentSecret = (*resumed)->resumeSecret;
    REQUIRE((*resumed)->consumedSecret.has_value());
    const auto frozenAt = dropped.now + 100;
    auto carried = dropped.table.exportForHandover(frozenAt);
    REQUIRE(carried.records.size() == 1);
    CHECK(carried.records[0]->state == AttachmentState::Lost);
    CHECK_EQ(carried.records[0]->lostSinceMicroseconds, frozenAt);
    CHECK(!carried.records[0]->servingConnection);
    CHECK(!carried.records[0]->handshakeConnection);
    CHECK(!carried.records[0]->consumedSecret);
    CHECK(!carried.records[0]->proofConnection);
    CHECK((*resumed)->state == AttachmentState::Live);
    CHECK((*resumed)->handshakeConnection == std::optional<std::uint64_t> { 2 });
    SequenceSecrets secrets;
    AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(std::move(carried), hostInstance, frozenAt));
    const auto consumed = successor.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 3, frozenAt);
    CHECK(!consumed && consumed.error() == ResumeRefusal::WrongSecret);
    REQUIRE(successor.resume(resumeOf(dropped.replicas, dropped.attachmentID, currentSecret), owner, 3, frozenAt));
    CHECK(successor.takeSuperseded().empty());
}

TEST(inheritanceRejectsCorruptIdentityWithoutTakingOwners)
{
    Dropped dropped;
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto carried = dropped.table.exportForHandover(dropped.now);
        switch (fault) {
        case 0:
            carried.hostInstanceID.low ^= 1;
            break;
        case 1:
            carried.records[0]->issuer = { 99, 100 };
            break;
        case 2:
            carried.records[0]->lostSinceMicroseconds = dropped.now + 1;
            break;
        case 3:
            carried.records[0]->servingConnection = 1;
            break;
        case 4:
            carried.records[0]->proofConnection = 1;
            break;
        case 5:
            carried.inheritedInstances = { hostInstance };
            break;
        case 6:
            carried.notifyRevision = UINT64_MAX;
            break;
        }
        const auto* retained = carried.records[0].get();
        SequenceSecrets secrets;
        AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
        CHECK(!successor.inherit(std::move(carried), hostInstance, dropped.now));
        CHECK(carried.records[0].get() == retained);
        CHECK(!successor.find(dropped.attachmentID));
        CHECK(!successor.isResumableIssuer(hostInstance));
    }
}

TEST(aNinthAncestorRetiresOnlyItsUnrotatedAttachments)
{
    SequenceSecrets secrets;
    auto table = std::make_unique<AttachmentTable>(hostInstance, measuredResumePolicy(), secrets);
    const auto oldest = table->create(clientInstance, ClientKind::Human, owner, 1, 1).attachmentID;
    const auto detached = table->create({ 19, 20 }, ClientKind::Human, owner, 2, 1).attachmentID;
    table->noteDetached(detached);
    ID128 current = hostInstance;
    ID128 newest;
    std::uint64_t revisionBeforeRetirement = 0;
    for (unsigned handover = 0; handover < 9; ++handover) {
        const ID128 next { 100 + handover, 200 + handover };
        auto successor = std::make_unique<AttachmentTable>(next, measuredResumePolicy(), secrets);
        if (handover == 8)
            revisionBeforeRetirement = table->exportForHandover(18).revision;
        REQUIRE(successor->inherit(table->exportForHandover(10 + handover), current, 10 + handover));
        CHECK(!successor->find(detached));
        if (handover < 8)
            CHECK(successor->find(oldest));
        if (handover == 0)
            newest = successor->create({ 21, 22 }, ClientKind::Human, owner, 3, 10).attachmentID;
        table = std::move(successor);
        current = next;
    }
    CHECK(!table->find(oldest));
    CHECK(!table->isResumableIssuer(hostInstance));
    CHECK(table->find(newest));
    CHECK(table->isResumableIssuer({ 100, 200 }));
    CHECK_EQ(table->exportForHandover(18).revision, revisionBeforeRetirement + 1);
}

TEST(inheritanceRefusesRetirementRevisionExhaustionWithoutTakingOwners)
{
    SequenceSecrets secrets;
    AttachmentTable predecessor(hostInstance, measuredResumePolicy(), secrets);
    const auto first = predecessor.create(clientInstance, ClientKind::Human, owner, 1, 1).attachmentID;
    const auto second = predecessor.create({ 90, 91 }, ClientKind::Human, owner, 2, 1).attachmentID;
    predecessor.noteTransportLost(first, 1, 1);
    predecessor.noteTransportLost(second, 2, 1);
    const auto now = measuredResumePolicy().lostGraceMicroseconds + 1;
    for (const bool shouldRetireOldest : { false, true }) {
        auto carried = predecessor.exportForHandover(now);
        carried.revision = UINT64_MAX - 1;
        if (shouldRetireOldest) {
            carried.inheritedInstances = { { 100, 200 }, { 101, 201 }, { 102, 202 }, { 103, 203 }, { 104, 204 }, { 105, 205 }, { 106, 206 }, { 107, 207 } };
            for (auto& record : carried.records) {
                record->issuer = carried.inheritedInstances.front();
                record->lostSinceMicroseconds = now;
            }
        }
        const auto* firstOwner = carried.records[0].get();
        const auto* secondOwner = carried.records[1].get();
        AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
        const auto result = successor.inherit(std::move(carried), hostInstance, now);
        CHECK(!result && result.error() == AttachmentTable::InheritanceRefusal::ExhaustedIdentity);
        if (result)
            continue;
        REQUIRE(carried.records.size() == 2);
        CHECK(carried.records[0].get() == firstOwner);
        CHECK(carried.records[1].get() == secondOwner);
        CHECK(!successor.find(first));
        CHECK(!successor.find(second));
        CHECK(!successor.isResumableIssuer(hostInstance));
        CHECK(successor.takeFreed().empty());
        CHECK_EQ(successor.exportForHandover(now).revision, 0U);
    }
}

TEST(inheritedIdentityCountersNeverCollideWithFreshAttachments)
{
    Dropped dropped;
    SequenceSecrets secrets;
    AttachmentTable successor({ hostInstance.low, hostInstance.high + 1 }, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(dropped.table.exportForHandover(dropped.now), hostInstance, dropped.now));
    const auto fresh = successor.create({ 70, 80 }, ClientKind::Human, owner, 2, dropped.now).attachmentID;
    CHECK(fresh != dropped.attachmentID);
    CHECK_EQ(fresh.high, dropped.attachmentID.high);
    CHECK(fresh.low > dropped.attachmentID.low);
    CHECK(successor.find(dropped.attachmentID));
}

TEST(aFreshLoginReplacesAnOlderInheritedLiveAttachmentWhenItIsLost)
{
    Dropped dropped;
    SequenceSecrets secrets;
    AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(dropped.table.exportForHandover(dropped.now), hostInstance, dropped.now));
    REQUIRE(successor.resume(resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret), owner, 2, dropped.now));
    const auto fresh = successor.create(clientInstance, ClientKind::Human, owner, 3, dropped.now).attachmentID;
    CHECK_EQ(fresh.low, dropped.attachmentID.low);
    CHECK(fresh.high != dropped.attachmentID.high);
    successor.noteTransportLost(dropped.attachmentID, 2, dropped.now);
    CHECK(!successor.find(dropped.attachmentID));
    CHECK(successor.find(fresh));
}

TEST(inheritedLostDeadlineDoesNotWrapNearTheClockLimit)
{
    SequenceSecrets secrets;
    AttachmentTable predecessor(hostInstance, measuredResumePolicy(), secrets);
    const auto lostAt = UINT64_MAX - 100;
    const auto id = predecessor.create(clientInstance, ClientKind::Human, owner, 1, lostAt).attachmentID;
    predecessor.noteTransportLost(id, 1, lostAt);
    AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
    REQUIRE(successor.inherit(predecessor.exportForHandover(lostAt), hostInstance, lostAt));
    successor.expire(UINT64_MAX);
    CHECK(successor.find(id));
    CHECK(successor.nextExpiryMicroseconds() == std::optional<std::uint64_t> { UINT64_MAX });
}

// A resume says how the client holds each pane, so the host knows which
// carrier a pane's status takes without a View: a pane resumed hidden is sent
// its status set.
TEST(aPaneResumedHiddenIsSentItsStatusSetWithoutAView)
{
    Dropped dropped;
    Ubiquitty::OSC::ProgramStatus report;
    report.id = "job";
    report.app = "agent";
    report.state = Ubiquitty::OSC::ProgramState::Working;
    StatusRecords records;
    records.store.records.push_back(report);
    dropped.pane.source->setStatus(records);
    ResumeRequest request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    REQUIRE(request.panes.size() == 1);
    request.panes[0].view = ViewState::Dormant;
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, request);
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(second.client.welcome().has_value());
    CHECK(second.host.paneView(1) == std::optional { ViewState::Dormant });
    CHECK_EQ(second.host.counters().statusSetsSent, 1u);
    REQUIRE(second.client.paneStatus(1).has_value());
    CHECK(second.client.paneStatus(1)->store == dropped.pane.source->status().store);
}

// The update the drop swallowed was applied by the client but never
// acknowledged: the digest names an in-flight baseline, and the resume still
// diffs.
TEST(aResumeWhoseLastUpdateWasLostInTheDropStillDiffs)
{
    SequenceSecrets secrets;
    AttachmentTable table(hostInstance, measuredResumePolicy(), secrets);
    OwnedPane pane;
    ReplicaSet replicas;
    std::uint64_t now = 1;
    ID128 attachmentID;
    ID128 secret;
    {
        Session first(table, replicas, *pane.source, owner, 1, std::nullopt);
        first.pump(now);
        first.pump(now);
        (void)first.host.bindPane(*pane.source);
        first.pumpUntil(now, now + 100'000);
        attachmentID = first.client.welcome()->attachmentID;
        secret = first.client.welcome()->resumeSecret;
        // The client reads the update, but its Ack never reaches the host.
        UbiquittyTest::feed(pane.terminal, "unacknowledged\r\n");
        pane.source->publish(FrameClass::Settled, now);
        first.host.service(now);
        std::vector<std::uint8_t> down;
        while (first.host.popStreamFrame(down, now)) {
        }
        first.client.receiveStream(down, now);
        CHECK(first.client.paneGeneration(1) == pane.source->generation());
        first.client.storeReplicas();
        table.noteTransportLost(attachmentID, 1, now);
    }
    Session second(table, replicas, *pane.source, owner, 2, resumeOf(replicas, attachmentID, secret));
    second.pumpUntil(now, now + 200'000);
    REQUIRE(second.client.welcome().has_value() && second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.host.counters().checkpointsSent, 0u);
}

// The update in flight at the drop never reached the client: its digest is
// the acknowledged baseline's, older than what was sent, and the resume diffs
// from that.
TEST(aResumeWhoseLastUpdateNeverArrivedDiffsFromTheAcknowledged)
{
    Dropped dropped;
    {
        // A connection that sends one more update into a transport that
        // carries nothing to the client, then is lost.
        ResumeRequest request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
        Session lost(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, request);
        lost.pumpUntil(dropped.now, dropped.now + 100'000);
        REQUIRE(lost.client.welcome().has_value());
        dropped.secret = lost.client.welcome()->resumeSecret;
        lost.client.storeReplicas();
        UbiquittyTest::feed(dropped.pane.terminal, "never arrived\r\n");
        dropped.pane.source->publish(FrameClass::Settled, dropped.now);
        for (int pass = 0; pass < 4; ++pass) {
            lost.host.service(dropped.now);
            std::vector<std::uint8_t> discarded;
            while (lost.host.popStreamFrame(discarded, dropped.now)) {
            }
            dropped.now += 20'000;
        }
        CHECK(lost.host.counters().chainUpdatesSent + lost.host.counters().supersetUpdatesSent >= 1);
        dropped.table.noteTransportLost(dropped.attachmentID, 2, dropped.now);
    }
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 3, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(second.client.welcome().has_value() && second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.host.counters().checkpointsSent, 0u);
    CHECK(UbiquittyTest::firstDisagreement(dropped.pane.terminal, *second.client.paneTerminal(1)).empty());
}

// A digest the host never sent names no baseline: the pane takes a
// checkpoint.
// A client that lost its effect receipts, as an editor restarted from a
// record older than its last acknowledgement does, names a cursor behind the
// one its attachment holds. The session survives: the pane is drawn again,
// since the picture is as old as the cursor, and a completion through the
// acknowledged effects tells the client which it will not be sent again.
TEST(aResumeFromLostEffectReceiptsIsCheckpointedAndToldWhatItMissed)
{
    Dropped dropped(3, 2);
    ResumeRequest resume = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    REQUIRE(resume.panes.size() == 1);
    resume.panes[0].effectsAcknowledged = 0;
    dropped.replicas.forgetEffectReceipt(paneID);
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, std::move(resume));
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    CHECK_MSG(second.host.state() == ConnectionState::Attached, "a resume behind its acknowledged effects was refused");
    REQUIRE(second.client.welcome().has_value());
    REQUIRE(second.client.welcome()->resumed.size() == 1);
    CHECK_MSG(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Checkpointed, "a picture as old as its lost receipts was continued");
    CHECK(second.host.counters().checkpointsSent >= 1);
    CHECK_MSG(dropped.replicas.effectReceipt(paneID).acknowledged == 2, "the client was not told the effects it will not be sent again");
    // What rings after the resume reaches it.
    REQUIRE(dropped.pane.source->appendEffect(BellEffect {}, clientInstance));
    second.pumpUntil(dropped.now, dropped.now + 100'000);
    CHECK_EQ(dropped.replicas.effectReceipt(paneID).acknowledged, 3u);
}

// A cursor beyond anything the attachment was sent acknowledges effects no
// client was given: it is forged or broken, and refused.
TEST(aResumeAcknowledgingEffectsNeverSentIsRefused)
{
    Dropped dropped(3, 2);
    ResumeRequest resume = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    REQUIRE(resume.panes.size() == 1);
    resume.panes[0].effectsAcknowledged = 99;
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, std::move(resume));
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    CHECK(second.host.state() == ConnectionState::Closed);
    CHECK(second.host.byeReason() == ByeReason::Malformed);
}

TEST(aStaleDigestGetsACheckpoint)
{
    Dropped dropped;
    ResumeRequest request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    request.panes[0].ackedRowHashesDigest.low ^= 1;
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, request);
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(second.client.welcome().has_value() && second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Checkpointed);
    CHECK_EQ(second.host.counters().checkpointsSent, 1u);
    CHECK(UbiquittyTest::firstDisagreement(dropped.pane.terminal, *second.client.paneTerminal(1)).empty());
}

// A resume continues only the panes its attachment held: a pane the host
// serves, but never to this attachment, is Gone and is not bound.
TEST(aResumeNamingAPaneTheAttachmentNeverHeldIsGone)
{
    Dropped dropped;
    OwnedPane other;
    ResumeRequest request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    request.panes.push_back({ .paneID = otherPaneID, .ackedGeneration = other.source->generation(), .ackedRowHashesDigest = {}, .historyEpoch = 0, .cachedRanges = {}, .lastInputSeqSent = 0, .view = ViewState::Live });
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, request, other.source.get());
    second.pumpUntil(dropped.now, dropped.now + 200'000);
    REQUIRE(second.client.welcome().has_value() && second.client.welcome()->resumed.size() == 2);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(second.client.welcome()->resumed[1].paneID == otherPaneID);
    CHECK(second.client.welcome()->resumed[1].outcome == ResumeOutcome::Gone);
    CHECK_EQ(second.host.boundPanes().size(), 1u);
}

// After five idle minutes away, a resume costs under a kilobyte both ways.
TEST(aResumeAfterFiveIdleMinutesCostsUnderOneKilobyte)
{
    Dropped dropped(20);
    dropped.now += 300'000'000;
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    second.pumpUntil(dropped.now, dropped.now + 100'000);
    REQUIRE(second.client.welcome().has_value() && second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_MSG(second.streamBytes < 1024, ("a resume cost " + std::to_string(second.streamBytes) + " bytes").c_str());
}

// A connection with bytes outstanding and nothing heard for the silence bound
// is Lost, and so is its attachment.
TEST(aSilentConnectionWithBytesOutstandingIsLostAfterThirtySeconds)
{
    SequenceSecrets secrets;
    AttachmentTable table(hostInstance, measuredResumePolicy(), secrets);
    OwnedPane pane;
    ReplicaSet replicas;
    Session session(table, replicas, *pane.source, owner, 1, std::nullopt);
    std::uint64_t now = 1;
    session.pump(now);
    session.pump(now);
    (void)session.host.bindPane(*pane.source);
    session.pumpUntil(now, 100'000);
    REQUIRE(session.host.attachment() != nullptr);
    const ID128 attachmentID = session.host.attachment()->attachmentID;
    // The client goes silent; the host keeps sending.
    UbiquittyTest::feed(pane.terminal, "unheard\r\n");
    pane.source->publish(FrameClass::Settled, now);
    for (; now < 100'000 + 31'000'000; now += 100'000) {
        session.host.service(now);
        std::vector<std::uint8_t> discarded;
        while (session.host.popStreamFrame(discarded, now)) {
        }
    }
    CHECK(session.host.state() == ConnectionState::Lost);
    // The connection lets go of the record, which the table may free.
    CHECK(session.host.attachment() == nullptr);
    REQUIRE(table.find(attachmentID) != nullptr);
    CHECK(table.find(attachmentID)->state == AttachmentState::Lost);
}

// A new connection starts its pacing from the window's floor, whatever the
// old one had learned.
TEST(pacingRestartsAtTheFloorOnTheNewConnection)
{
    Dropped dropped;
    Session second(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    second.pump(dropped.now);
    CHECK_EQ(second.host.pacer().windowBytes(dropped.now), UbiquittyTest::syncHostPolicy(0).pacing.windowFloorBytes);
}

/// A table, a pane and a client attached on a connection that is still open.
struct Live {
    SequenceSecrets secrets;
    AttachmentTable table { hostInstance, measuredResumePolicy(), secrets };
    OwnedPane pane;
    ReplicaSet replicas;
    std::unique_ptr<Session> first;
    ID128 attachmentID;
    ID128 secret;
    std::uint64_t now = 1;

    Live()
    {
        first = std::make_unique<Session>(table, replicas, *pane.source, owner, 1, std::nullopt);
        first->pump(now);
        first->pump(now);
        (void)first->host.bindPane(*pane.source);
        first->pumpUntil(now, now + 100'000);
        attachmentID = first->client.welcome()->attachmentID;
        secret = first->client.welcome()->resumeSecret;
        first->client.storeReplicas();
    }

    /// Closes what the table says was superseded, as the owner does.
    void closeSuperseded()
    {
        for (const std::uint64_t connection : table.takeSuperseded()) {
            if (connection == 1)
                first->host.supersede();
        }
    }
};

// A phone that changed networks resumes before the host saw the old transport
// die: the resume succeeds first, then the old connection is closed.
TEST(aResumeWhileTheOldTransportIsOpenMovesOwnershipAtOnce)
{
    Live live;
    Session second(live.table, live.replicas, *live.pane.source, owner, 2, resumeOf(live.replicas, live.attachmentID, live.secret));
    second.pump(live.now);
    second.pump(live.now);
    live.closeSuperseded();
    REQUIRE(second.client.welcome().has_value());
    CHECK(live.first->host.byeReason() == std::optional<ByeReason> { ByeReason::Superseded });
    CHECK(live.first->host.state() == ConnectionState::Closed);
    REQUIRE(second.client.welcome()->resumed.size() == 1);
    CHECK(second.client.welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(live.table.find(live.attachmentID)->servingConnection == 2u);
    // The old connection reads nothing more: a late frame changes nothing.
    const std::uint64_t updatesBefore = live.first->host.counters().chainUpdatesSent;
    UbiquittyTest::feed(live.pane.terminal, "after\r\n");
    live.pane.source->publish(FrameClass::Settled, live.now);
    live.first->pump(live.now + 50'000);
    CHECK_EQ(live.first->host.counters().chainUpdatesSent, updatesBefore);
    second.pumpUntil(live.now, live.now + 200'000);
    CHECK(UbiquittyTest::firstDisagreement(live.pane.terminal, *second.client.paneTerminal(1)).empty());
}

// A wrong secret attaches the second connection fresh and leaves the first
// serving: updates keep arriving there.
TEST(aWrongSecretLeavesTheLiveTransportServing)
{
    Live live;
    const ID128 wrong { live.secret.low + 7, live.secret.high };
    Session second(live.table, live.replicas, *live.pane.source, owner, 2, resumeOf(live.replicas, live.attachmentID, wrong));
    second.pump(live.now);
    second.pump(live.now);
    live.closeSuperseded();
    CHECK(second.host.resumeRefusal() == std::optional<ResumeRefusal> { ResumeRefusal::WrongSecret });
    CHECK(second.client.welcome()->attachmentID != live.attachmentID);
    CHECK(live.first->host.state() == ConnectionState::Attached);
    const std::uint64_t updatesBefore = live.first->host.counters().chainUpdatesSent;
    UbiquittyTest::feed(live.pane.terminal, "still here\r\n");
    live.pane.source->publish(FrameClass::Settled, live.now);
    live.first->pumpUntil(live.now, live.now + 100'000);
    CHECK(live.first->host.counters().chainUpdatesSent > updatesBefore);
}

// Two resumes race with one secret: the second, inside the first's
// handshake, is answered ResumeInProgress; one attachment remains, and the
// input the winner sends is written once.
TEST(twoRacingResumesYieldOneAttachmentAndExactlyOnceInput)
{
    Dropped dropped;
    Session winner(dropped.table, dropped.replicas, *dropped.pane.source, owner, 2, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    // The winner is admitted; its client has not yet spoken.
    winner.host.receiveStream(winner.client.takeStreamOutput(), dropped.now);
    Session loser(dropped.table, dropped.replicas, *dropped.pane.source, owner, 3, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    loser.pump(dropped.now);
    CHECK(loser.host.byeReason() == std::optional<ByeReason> { ByeReason::ResumeInProgress });
    CHECK(loser.host.resumeRefusal() == std::optional<ResumeRefusal> { ResumeRefusal::InProgress });
    CHECK(!loser.host.attachment());
    winner.pumpUntil(dropped.now, dropped.now + 100'000);
    const std::array<std::uint8_t, 1> typed { 'x' };
    CHECK(winner.client.sendRaw(1, typed).has_value());
    winner.pumpUntil(dropped.now, dropped.now + 50'000);
    CHECK_STR_EQ(dropped.pane.writer.bytes(), std::string("x"));
    CHECK(dropped.table.find(dropped.attachmentID)->servingConnection == 2u);
    // The winner's client has spoken, so its handshake is over: the consumed
    // secret now attaches fresh as a wrong one.
    Session late(dropped.table, dropped.replicas, *dropped.pane.source, owner, 4, resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret));
    late.pump(dropped.now);
    late.pump(dropped.now);
    CHECK(late.host.resumeRefusal() == std::optional<ResumeRefusal> { ResumeRefusal::WrongSecret });
}

// Once the winner's client has spoken, the window is over: the consumed
// secret is a wrong one, and attaches fresh.
TEST(aSecondResumeDuringTheHandshakeGetsResumeInProgress)
{
    Dropped dropped;
    const ResumeRequest request = resumeOf(dropped.replicas, dropped.attachmentID, dropped.secret);
    REQUIRE(dropped.table.resume(request, owner, 2, dropped.now).has_value());
    const auto racing = dropped.table.resume(request, owner, 3, dropped.now);
    CHECK(!racing && racing.error() == ResumeRefusal::InProgress);
    dropped.table.noteFirstFrame(dropped.attachmentID, 2);
    const auto late = dropped.table.resume(request, owner, 4, dropped.now);
    CHECK(!late && late.error() == ResumeRefusal::WrongSecret);
}

// The right secret from another principal is refused, and the live transport
// is not superseded.
TEST(aResumeWithTheRightSecretFromAnotherPrincipalLeavesTheLiveTransportUnchanged)
{
    Live live;
    Session intruder(live.table, live.replicas, *live.pane.source, { .principal = "mallory", .mayDrive = true }, 2, resumeOf(live.replicas, live.attachmentID, live.secret));
    intruder.pump(live.now);
    intruder.pump(live.now);
    live.closeSuperseded();
    CHECK(intruder.host.resumeRefusal() == std::optional<ResumeRefusal> { ResumeRefusal::OtherPrincipal });
    CHECK(live.first->host.state() == ConnectionState::Attached);
    CHECK(live.table.find(live.attachmentID)->servingConnection == 1u);
    CHECK(live.table.find(live.attachmentID)->resumeSecret == live.secret);
}

UBIQUITTY_TEST_MAIN()
