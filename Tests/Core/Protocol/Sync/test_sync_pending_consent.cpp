// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Wire.h"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

class Secrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 91 }; }

private:
    std::uint64_t m_next = 0;
};

const ConnectionAuthority authority { .principal = "verified-agent", .mayDrive = true, .verifiedKind = ClientKind::Agent };
constexpr ID128 host { 41, 42 };
constexpr ID128 client { 7, 8 };
constexpr ID128 pane { 11, 12 };
constexpr ConsentBinding question { .requestID = 7, .sessionID = 11 };

ServingSelection selectedFixture()
{
    return { .target = AdmissionTarget::Session, .sessionID = 11, .workspaceID = 0, .daemonInstanceID = 12, .socketName = "pending-test.sock", .buildFlags = 0, .hostPlatform = "fixture" };
}

struct Observer final : ClientPaneDelegate {
    unsigned pending = 0;
    unsigned connected = 0;
    unsigned refusals = 0;
    std::optional<ConsentStatus> refused;

    void hostWelcomed(const Welcome& welcome) override
    {
        if (welcome.admission == AttachmentAdmission::Granted)
            ++connected;
        else
            ++pending;
    }

    void consentRefused(const ConsentStatus& status) override
    {
        ++refusals;
        refused = status;
    }
};

HostPolicy hostPolicy(std::size_t capacity)
{
    HostPolicy policy = measuredHostPolicy(0);
    policy.maxQueuedInteractiveBytes = capacity;
    return policy;
}

ClientIdentity clientIdentity()
{
    ClientIdentity identity { .clientInstance = client, .kind = ClientKind::Agent, .requestedRole = Role::Agent };
    identity.facts.viewportRows = 24;
    identity.facts.viewportColumns = 80;
    identity.facts.refreshIntervalMicroseconds = 16'667;
    return identity;
}

struct PendingPair {
    Secrets secrets;
    AttachmentTable table { host, measuredResumePolicy(), secrets };
    HostConnection sender;
    Observer observer;
    ClientConnection receiver;

    explicit PendingPair(std::size_t capacity = 4096)
        : sender(hostPolicy(capacity), HostIdentity { .pendingConsent = question, .selection = selectedFixture() })
        , receiver(measuredClientPolicy(), clientIdentity(), &observer)
    {
        sender.serveFromTable(table, authority, 2, [](ID128) -> PaneSource* { return nullptr; });
        receiver.start();
        sender.receiveStream(receiver.takeStreamOutput(), 1);
        carry();
    }

    void carry()
    {
        std::vector<std::uint8_t> bytes;
        while (sender.popStreamFrame(bytes, 1)) {
        }
        receiver.receiveStream(bytes, 1);
    }

    void receiveStatus(ConsentStatus status)
    {
        const auto body = consentStatusNotify(status);
        CHECK(body);
        if (!body)
            return;
        std::vector<std::uint8_t> bytes;
        CHECK(appendFrame(bytes, FrameKind::Notify, 0, *body));
        receiver.receiveStream(bytes, 1);
    }
};

} // namespace

TEST(aPendingConsentIdentityDoesNotReplaceAnOlderLostAttachment)
{
    Secrets secrets;
    AttachmentTable table(host, measuredResumePolicy(), secrets);
    AttachmentRecord& admitted = table.create(client, ClientKind::Agent, authority, 1, 0);
    const ID128 admittedID = admitted.attachmentID;
    admitted.paneFor(pane);
    table.noteTransportLost(admittedID, 1, 1);
    REQUIRE(table.find(admittedID));
    AttachmentRecord& pending = table.createPending(client, ClientKind::Agent, authority, 2, 2);
    const ID128 pendingID = pending.attachmentID;
    CHECK(pending.admission == AttachmentAdmission::AwaitingConsent);
    CHECK(pending.panes.empty());
    CHECK(table.find(admittedID) && table.find(admittedID)->state == AttachmentState::Lost);
    CHECK(table.takeFreed().empty());
    table.noteTransportLost(pendingID, 2, 3);
    CHECK(!table.find(pendingID));
    CHECK(table.find(admittedID) && table.find(admittedID)->state == AttachmentState::Lost);
    const auto freed = table.takeFreed();
    REQUIRE(freed.size() == 1);
    CHECK(freed.front().attachmentID == pendingID);
}

TEST(onlyTheExactPendingOwnerCanPromoteItsUnchangedIdentityOnce)
{
    Secrets secrets;
    AttachmentTable table(host, measuredResumePolicy(), secrets);
    const ID128 admittedID = table.create(client, ClientKind::Agent, authority, 1, 0).attachmentID;
    table.noteTransportLost(admittedID, 1, 1);
    const ID128 pendingID = table.createPending(client, ClientKind::Agent, authority, 2, 2).attachmentID;
    auto otherAuthority = authority;
    otherAuthority.principal = "another-agent";
    CHECK(!table.promotePending(pendingID, authority, 3));
    CHECK(!table.promotePending(pendingID, otherAuthority, 2));
    otherAuthority = authority;
    otherAuthority.mayDrive = false;
    CHECK(!table.promotePending(pendingID, otherAuthority, 2));
    REQUIRE(table.find(pendingID));
    CHECK(table.find(pendingID)->admission == AttachmentAdmission::AwaitingConsent);
    CHECK(table.find(admittedID));
    CHECK(table.promotePending(pendingID, authority, 2));
    REQUIRE(table.find(pendingID));
    CHECK(table.find(pendingID)->attachmentID == pendingID);
    CHECK(table.find(pendingID)->admission == AttachmentAdmission::Granted);
    CHECK(!table.find(admittedID));
    CHECK(!table.promotePending(pendingID, authority, 2));
    const auto freed = table.takeFreed();
    REQUIRE(freed.size() == 1);
    CHECK(freed.front().attachmentID == admittedID);
}

TEST(aPendingAttachmentHasNoResumeProofOrExportedPaneAuthority)
{
    Secrets secrets;
    AttachmentTable table(host, measuredResumePolicy(), secrets);
    AttachmentRecord& pending = table.createPending(client, ClientKind::Agent, authority, 2, 0);
    const ID128 pendingID = pending.attachmentID;
    ResumeRequest request;
    request.attachmentID = pendingID;
    request.resumeSecret = pending.resumeSecret;
    const auto resumed = table.resume(request, authority, 3, 1);
    CHECK(!resumed && resumed.error() == ResumeRefusal::NotResumable);
    std::array<std::uint8_t, 32> exporter {};
    const ResumeProof proof { .attachmentID = pendingID, .mac = resumeProofMAC(pending.resumeSecret, exporter, pendingID, host) };
    const auto proven = table.admitProof(proof, exporter, 3, 1);
    CHECK(!proven && proven.error() == ResumeRefusal::NotResumable);
    CHECK(!table.provenAttachment(3));
    // Even a mistaken caller adding a baseline cannot expose pending authority.
    pending.paneFor(pane);
    CHECK(table.holding(std::array { pane }).empty());
    CHECK(!table.holdsResumablePane(host, authority.principal, pane, pendingID, 1));
    CHECK(table.exportForHandover(1).records.empty());
    REQUIRE(table.find(pendingID));
    CHECK(table.find(pendingID)->servingConnection == 2);
    CHECK(table.find(pendingID)->admission == AttachmentAdmission::AwaitingConsent);
}

TEST(aPendingReplacementCannotEraseAnAdmittedConnectionWhenItBecomesLost)
{
    Secrets secrets;
    AttachmentTable table(host, measuredResumePolicy(), secrets);
    const ID128 admittedID = table.create(client, ClientKind::Agent, authority, 1, 0).attachmentID;
    const ID128 pendingID = table.createPending(client, ClientKind::Agent, authority, 2, 1).attachmentID;
    table.noteTransportLost(admittedID, 1, 2);
    REQUIRE(table.find(admittedID));
    CHECK(table.find(admittedID)->state == AttachmentState::Lost);
    CHECK(table.find(pendingID));
    CHECK(table.takeFreed().empty());
    table.noteDetached(pendingID);
    CHECK(table.find(admittedID));
    CHECK(!table.find(pendingID));
}

TEST(aPendingWelcomeNeverPresentsConnectedAndOnlyExactApprovalAdmitsIt)
{
    PendingPair pair;
    REQUIRE(pair.sender.attachment());
    const ID128 attachmentID = pair.sender.attachment()->attachmentID;
    CHECK(pair.sender.state() == ConnectionState::AwaitingConsent);
    CHECK(pair.sender.hasHandoverBlockedOperations());
    CHECK(pair.receiver.state() == ConnectionState::AwaitingConsent);
    CHECK(pair.observer.pending == 1 && !pair.observer.connected);
    const auto maintenance = pair.receiver.takeStreamOutput();
    EnvelopeError error = EnvelopeError::None;
    const auto probe = decodeDatagram(maintenance, error);
    REQUIRE(probe);
    CHECK(probe->kind == FrameKind::Probe && probe->channel == 0);
    CHECK(!pair.sender.sendNotify(*consentResolvedNotify({ .kind = ConsentKind::Attach, .requestID = 1, .sessionID = 2 })));
    CHECK(!pair.receiver.call(R"({"jsonrpc":"2.0","id":"blocked","method":"host.describe","params":{}})"));
    CHECK(pair.receiver.takeStreamOutput().empty());
    CHECK(pair.sender.sendConsentStatus({ .question = question, .phase = ConsentStatusPhase::Pending }));
    pair.carry();
    CHECK(!pair.observer.connected);
    CHECK(pair.sender.deliverConsentApproval(question, authority, 2, 100));
    CHECK(pair.sender.state() == ConnectionState::Attached);
    CHECK(pair.table.find(attachmentID) && pair.table.find(attachmentID)->admission == AttachmentAdmission::Granted);
    CHECK(!pair.observer.connected); // Admitted output has not reached the client.
    pair.carry();
    CHECK(pair.receiver.state() == ConnectionState::Attached);
    CHECK(pair.observer.connected == 1);
    CHECK(!pair.sender.deliverConsentApproval(question, authority, 3, 100));
}

TEST(pendingCallsInputAndTransfersAreRefusedBeforeHandlerAdmission)
{
    for (const auto kind : { FrameKind::Call, FrameKind::Raw, FrameKind::Paste, FrameKind::Viewport,
             FrameKind::HistoryWant, FrameKind::ImageWant, FrameKind::ClipboardData, FrameKind::BulkChunk }) {
        PendingPair pair;
        std::vector<std::uint8_t> bytes;
        REQUIRE(appendFrame(bytes, kind, isConnectionKind(kind) ? 0 : 1, {}));
        pair.sender.receiveStream(bytes, 2);
        CHECK(pair.sender.state() == ConnectionState::Closed);
        CHECK(pair.sender.byeReason() == ByeReason::OutsideScope);
        CHECK(pair.sender.takeCalls().empty());
        CHECK(pair.sender.boundPanes().empty());
    }
}

TEST(aPendingClientRefusesPaneDataAtItsHeaderBeforeApproval)
{
    for (const auto kind : { FrameKind::PaneBind, FrameKind::PaneListed, FrameKind::WorkspaceStructure, FrameKind::Result,
             FrameKind::PaneFacts, FrameKind::Effect, FrameKind::ImageHead, FrameKind::BulkChunk }) {
        PendingPair pair;
        std::vector<std::uint8_t> bytes;
        REQUIRE(appendFrame(bytes, kind, isConnectionKind(kind) ? 0 : 1, {}));
        pair.receiver.receiveStream(bytes, 2);
        CHECK(pair.receiver.state() == ConnectionState::Closed);
        CHECK(pair.receiver.byeReason() == ByeReason::Malformed);
        CHECK(pair.receiver.boundPanes().empty());
        CHECK(!pair.observer.connected);
    }
}

TEST(ownConsentTransitionsRejectWrongQuestionsRepetitionAndTerminalBeforePending)
{
    for (const unsigned variant : { 0u, 1u, 2u, 3u }) {
        PendingPair pair;
        ConsentStatus status { .question = question, .phase = ConsentStatusPhase::Pending };
        if (variant == 0)
            ++status.question.requestID;
        else if (variant == 1)
            ++status.question.sessionID;
        else if (variant == 2)
            status.phase = ConsentStatusPhase::Approved;
        else {
            pair.receiveStatus(status);
            REQUIRE(pair.receiver.state() == ConnectionState::AwaitingConsent);
        }
        pair.receiveStatus(status);
        CHECK(pair.receiver.state() == ConnectionState::Closed);
        CHECK(pair.receiver.byeReason() == ByeReason::Malformed);
        CHECK(!pair.observer.connected);
    }
}

TEST(backpressuredApprovalCannotSurviveRevocationExpiryOrPendingDrop)
{
    for (const unsigned variant : { 0u, 1u, 2u }) {
        // One status frame and its NoConsent Bye fit, but Pending plus
        // Approved does not.
        PendingPair pair(frameBytes(0, maxConsentStatusBytes) + frameBytes(0, 3) + 1);
        REQUIRE(pair.sender.attachment());
        const ID128 attachmentID = pair.sender.attachment()->attachmentID;
        REQUIRE(pair.sender.sendConsentStatus({ .question = question, .phase = ConsentStatusPhase::Pending }));
        CHECK(!pair.sender.deliverConsentApproval(question, authority, 2, 100));
        CHECK(pair.sender.state() == ConnectionState::ApprovedAwaitingDelivery);
        CHECK(pair.sender.hasHandoverBlockedOperations());
        CHECK(pair.table.find(attachmentID)->admission == AttachmentAdmission::AwaitingConsent);
        CHECK(!pair.observer.connected);
        if (variant == 2) {
            pair.sender.withdrawAuthority();
            pair.table.noteTransportLost(attachmentID, 2, 3);
            pair.carry();
            CHECK(!pair.sender.deliverConsentApproval(question, authority, 4, 100));
            CHECK(!pair.table.find(attachmentID));
            CHECK(pair.table.exportForHandover(4).records.empty());
        } else {
            auto current = authority;
            if (variant == 0)
                current.mayDrive = false;
            pair.carry(); // Capacity is available again only after authority changed.
            CHECK(!pair.sender.deliverConsentApproval(question, current, variant ? 100 : 3, 100));
            CHECK(pair.table.find(attachmentID)->admission == AttachmentAdmission::AwaitingConsent);
            REQUIRE(pair.sender.sendConsentStatus({ .question = question, .phase = variant ? ConsentStatusPhase::Expired : ConsentStatusPhase::Denied }));
            pair.carry();
            CHECK(pair.receiver.state() == ConnectionState::Closed);
            CHECK(pair.receiver.byeReason() == ByeReason::NoConsent);
            CHECK(!pair.table.find(attachmentID));
        }
        CHECK(!pair.observer.connected);
        CHECK(pair.receiver.boundPanes().empty());
    }
}

TEST(welcomeAdmissionIsRequiredWholeAndCannotCarryResumedPendingPanes)
{
    for (const auto admission : { AttachmentAdmission::Granted, AttachmentAdmission::AwaitingConsent }) {
        Welcome welcome;
        welcome.wireVersion = wireVersion;
        welcome.hostInstanceID = host;
        welcome.attachmentID = client;
        welcome.resumeSecret = pane;
        welcome.selection = selectedFixture();
        welcome.admission = admission;
        if (admission == AttachmentAdmission::AwaitingConsent)
            welcome.consent = question;
        std::vector<std::uint8_t> body;
        encode(body, welcome);
        const auto decoded = decodeWelcome(body);
        REQUIRE(decoded);
        CHECK(decoded->admission == admission && decoded->consent == welcome.consent);
        for (std::size_t length = 0; length < body.size(); ++length)
            CHECK(!decodeWelcome(std::span(body).first(length)));
        auto invalid = body;
        invalid.push_back(0);
        CHECK(!decodeWelcome(invalid));
        // Locate admission after the complete required build/selection block.
        WireReader prefix(body);
        (void)prefix.u16();
        (void)prefix.utf8(maxAppVersionBytes);
        (void)prefix.take(32 + 16 * 3 + 1 + 1 + 4 + 4 + 8);
        (void)prefix.utf8(maxDaemonSocketNameBytes);
        (void)prefix.u8();
        (void)prefix.utf8(maxHostPlatformBytes);
        REQUIRE(prefix.ok());
        const auto admissionOffset = prefix.offset();
        invalid = body;
        invalid[admissionOffset] = 2;
        CHECK(!decodeWelcome(invalid));
        invalid = body;
        invalid.erase(invalid.begin() + admissionOffset, invalid.begin() + admissionOffset + 9);
        CHECK(!decodeWelcome(invalid));
        invalid = body;
        invalid.back() = 2; // Presence flags are booleans, not truthy bytes.
        CHECK(!decodeWelcome(invalid));
        welcome.consent = admission == AttachmentAdmission::Granted ? question : ConsentBinding {};
        body.clear();
        encode(body, welcome);
        CHECK(!decodeWelcome(body));
    }
    Welcome pending;
    pending.wireVersion = wireVersion;
    pending.selection = selectedFixture();
    pending.admission = AttachmentAdmission::AwaitingConsent;
    pending.consent = question;
    pending.resumed.push_back({ .paneID = pane, .outcome = ResumeOutcome::Checkpointed });
    std::vector<std::uint8_t> body;
    encode(body, pending);
    CHECK(!decodeWelcome(body));
}

TEST(aTerminalOwnConsentVerdictReachesPresentationBeforeNoConsentBye)
{
    for (const auto phase : { ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        PendingPair pair;
        REQUIRE(pair.sender.sendConsentStatus({ .question = question, .phase = ConsentStatusPhase::Pending }));
        pair.carry();
        CHECK(!pair.observer.refusals);
        REQUIRE(pair.sender.sendConsentStatus({ .question = question, .phase = phase }));
        pair.carry();
        CHECK(pair.observer.refusals == 1);
        REQUIRE(pair.observer.refused);
        CHECK(pair.observer.refused->question == question && pair.observer.refused->phase == phase);
        CHECK(pair.receiver.state() == ConnectionState::Closed);
        CHECK(pair.receiver.byeReason() == ByeReason::NoConsent);
        CHECK(!pair.observer.connected);
        pair.receiveStatus({ .question = question, .phase = phase });
        CHECK(pair.observer.refusals == 1);
    }
}

TEST(ownConsentStatusHasOneExactBoundedRecordAndKnownPhases)
{
    for (const auto phase : { ConsentStatusPhase::Pending, ConsentStatusPhase::Approved, ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        const ConsentStatus expected { .question = { .requestID = std::numeric_limits<std::uint32_t>::max(), .sessionID = std::numeric_limits<std::uint32_t>::max() }, .phase = phase };
        const auto body = consentStatusNotify(expected);
        REQUIRE(body);
        CHECK(body->size() == maxConsentStatusBytes);
        const auto head = decodeNotifyHead(*body);
        REQUIRE(head && head->kind == std::to_underlying(NotifyKind::ConsentStatus));
        CHECK(decodeConsentStatus(head->record) == expected);
    }
    CHECK(!consentStatusNotify({ .question = {}, .phase = ConsentStatusPhase::Pending }));
    CHECK(!consentStatusNotify({ .question = question, .phase = static_cast<ConsentStatusPhase>(255) }));
}

namespace {

/// An own-status record written by hand, so any field can be wrong.
std::vector<std::uint8_t> statusBody(std::uint32_t request, std::uint32_t session, std::uint8_t phase, std::size_t trailing = 0)
{
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::ConsentStatus));
    writer.u32(request);
    writer.u32(session);
    writer.u8(phase);
    body.resize(body.size() + trailing);
    return body;
}

} // namespace

TEST(aGrantedClientRejectsAMalformedOrUnaskedOwnStatusInsteadOfForwardingIt)
{
    for (const auto& body : {
             statusBody(7, 11, 4),
             statusBody(0, 11, 0),
             statusBody(7, 0, 0),
             statusBody(7, 11, 0, 1),
             statusBody(7, 11, 1),
         }) {
        ClientConnection receiver(measuredClientPolicy(), clientIdentity());
        receiver.start();
        (void)receiver.takeStreamOutput();
        Welcome welcome;
        welcome.wireVersion = wireVersion;
        welcome.hostInstanceID = host;
        welcome.attachmentID = client;
        welcome.resumeSecret = pane;
        welcome.selection = selectedFixture();
        std::vector<std::uint8_t> encoded;
        encode(encoded, welcome);
        std::vector<std::uint8_t> bytes;
        REQUIRE(appendFrame(bytes, FrameKind::Welcome, 0, encoded));
        receiver.receiveStream(bytes, 1);
        REQUIRE(receiver.state() == ConnectionState::Attached);
        bytes.clear();
        REQUIRE(appendFrame(bytes, FrameKind::Notify, 0, body));
        receiver.receiveStream(bytes, 2);
        CHECK(receiver.state() == ConnectionState::Closed);
        CHECK(receiver.byeReason() == ByeReason::Malformed);
        CHECK(receiver.takeNotifies().empty());
    }
}

int main(int argc, char** argv)
{
    return UbiquittyTest::runRegisteredCases(argc, argv);
}
