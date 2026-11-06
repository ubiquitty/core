// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

ServingSelection selectionFixture()
{
    return { .target = AdmissionTarget::Session, .sessionID = 17, .workspaceID = 23, .daemonInstanceID = 55, .socketName = "host-fixture.sock", .buildFlags = knownServingBuildFlags, .hostPlatform = "linux" };
}

HostIdentity hostFixture()
{
    HostIdentity identity;
    identity.hostInstanceID = { 55, 56 };
    identity.attachmentID = { 57, 58 };
    identity.resumeSecret = { 59, 60 };
    identity.principal = "verified-fixture";
    identity.mayDrive = true;
    identity.verifiedKind = ClientKind::Human;
    identity.selection = selectionFixture();
    identity.appVersion = "1.2.3";
    return identity;
}

Greet greetFixture()
{
    Greet greet;
    greet.wireVersion = wireVersion;
    greet.appVersion = "1.2.3";
    greet.clientInstance = { 7, 8 };
    greet.clientKind = ClientKind::Human;
    greet.request = { .target = AdmissionTarget::Session, .sessionID = 17, .workspaceID = 0, .expectedDaemonInstanceID = 55, .workspaceName = {}, .agentLabel = "claimed", .sshHost = {}, .isAttachOnly = true, .endsWhenDetached = false, .servesClipboard = true };
    greet.credential = std::vector<std::uint8_t> { 0, 0xFF, 7 };
    greet.facts.viewportRows = 24;
    greet.facts.viewportColumns = 80;
    greet.historyCacheLines = 2000;
    greet.backfillDepthLines = 1000;
    greet.capabilities.add(ClientCapability::EchoContext);
    return greet;
}

Welcome welcomeFixture()
{
    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.appVersion = "1.2.3";
    welcome.hostInstanceID = { 55, 56 };
    welcome.attachmentID = { 57, 58 };
    welcome.resumeSecret = { 59, 60 };
    welcome.selection = selectionFixture();
    welcome.principal = "verified-fixture";
    return welcome;
}

ClientIdentity clientFixture()
{
    ClientIdentity identity;
    identity.clientInstance = { 7, 8 };
    identity.kind = ClientKind::Human;
    identity.request = greetFixture().request;
    identity.credential = greetFixture().credential;
    identity.appVersion = "1.2.3";
    identity.platform = "macos";
    return identity;
}

template<typename Message>
std::vector<std::uint8_t> bodyOf(const Message& message)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    return body;
}

std::vector<std::uint8_t> frameOf(FrameKind kind, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, kind, 0, body));
    return frame;
}

std::optional<Bye> firstBye(HostConnection& host)
{
    std::vector<std::uint8_t> frame;
    if (!host.popStreamFrame(frame, 1))
        return std::nullopt;
    EnvelopeError error = EnvelopeError::None;
    const auto decoded = decodeDatagram(frame, error);
    if (!decoded || decoded->kind != FrameKind::Bye)
        return std::nullopt;
    return decodeBye(decoded->body);
}

/// Independent field-by-field wire fixture, including unshortened claimed labels.
std::vector<std::uint8_t> rawGreet(const Greet& greet)
{
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.u16(greet.wireVersion);
    writer.utf8(greet.appVersion);
    writer.raw(greet.engineFingerprint);
    writer.u64(greet.clientInstance.low);
    writer.u64(greet.clientInstance.high);
    writer.u8(std::to_underlying(greet.clientKind));
    writer.u8(std::to_underlying(greet.requestedRole));
    writer.u8(std::to_underlying(greet.request.target));
    writer.u32(greet.request.sessionID);
    writer.u32(greet.request.workspaceID);
    writer.u64(greet.request.expectedDaemonInstanceID);
    writer.utf8(greet.request.workspaceName);
    writer.utf8(greet.request.agentLabel);
    writer.utf8(greet.request.sshHost);
    writer.u8(greet.request.isAttachOnly ? 1 : 0);
    writer.u8(greet.request.endsWhenDetached ? 1 : 0);
    writer.u8(greet.request.servesClipboard ? 1 : 0);
    writer.u8(greet.credential ? 1 : 0);
    if (greet.credential)
        writer.bytes(*greet.credential);
    writer.u16(greet.facts.viewportRows);
    writer.u16(greet.facts.viewportColumns);
    writer.u16(greet.facts.cellPixelWidth);
    writer.u16(greet.facts.cellPixelHeight);
    writer.u16(greet.facts.textAreaPixelWidth);
    writer.u16(greet.facts.textAreaPixelHeight);
    writer.u32(greet.facts.refreshIntervalMicroseconds);
    writer.u8(std::to_underlying(greet.facts.link));
    writer.u8(std::to_underlying(greet.facts.power));
    writer.u8(greet.facts.isFocused ? 1 : 0);
    writer.u32(greet.historyCacheLines);
    writer.u32(greet.backfillDepthLines);
    writer.varint(greet.capabilities.toRaw());
    writer.u8(greet.resume ? 1 : 0);
    if (greet.resume) {
        writer.u64(greet.resume->attachmentID.low);
        writer.u64(greet.resume->attachmentID.high);
        writer.u8(greet.resume->resumeSecret ? 1 : 0);
        if (greet.resume->resumeSecret) {
            writer.u64(greet.resume->resumeSecret->low);
            writer.u64(greet.resume->resumeSecret->high);
        }
        writer.varint(greet.resume->panes.size());
        for (const auto& pane : greet.resume->panes) {
            writer.u64(pane.paneID.low);
            writer.u64(pane.paneID.high);
            writer.u64(pane.ackedGeneration);
            writer.u64(pane.ackedRowHashesDigest.low);
            writer.u64(pane.ackedRowHashesDigest.high);
            writer.u32(pane.historyEpoch);
            writer.varint(pane.cachedRanges.size());
            for (const auto& range : pane.cachedRanges) {
                writer.u64(range.from);
                writer.u64(range.to);
            }
            writer.u64(pane.lastInputSeqSent);
            writer.u8(std::to_underlying(pane.view));
            writer.u64(pane.effectsAcknowledged);
        }
    }
    return body;
}

class Secrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 91 }; }

private:
    std::uint64_t m_next = 0;
};

struct FrameObserver final : ClientPaneDelegate {
    ClientAttachment* retire = nullptr;
    std::vector<std::pair<FrameKind, std::size_t>> frames;
    unsigned welcomes = 0;
    bool wasFrameReportedFirst = false;

    void connectionFrameDecoded(FrameKind kind, std::size_t bytes) override
    {
        frames.emplace_back(kind, bytes);
        if (retire)
            retire->transportLost(2);
    }
    void hostWelcomed(const Welcome&) override
    {
        ++welcomes;
        wasFrameReportedFirst = !frames.empty() && frames.back().first == FrameKind::Welcome;
    }
};

} // namespace

TEST(aCanonicalGreetHasOneExactAdmissionFieldOrder)
{
    const auto expected = rawGreet(greetFixture());
    CHECK(bodyOf(greetFixture()) == expected);
    const auto decoded = decodeGreet(expected);
    REQUIRE(decoded);
    CHECK(decoded->request.target == AdmissionTarget::Session);
    CHECK_EQ(decoded->request.sessionID, 17u);
    CHECK_EQ(decoded->request.expectedDaemonInstanceID, 55u);
    CHECK(decoded->request.agentLabel == "claimed");
    CHECK(decoded->credential == greetFixture().credential);
    CHECK_EQ(decoded->facts.viewportRows, 24u);
    CHECK_EQ(decoded->facts.viewportColumns, 80u);
    CHECK(decoded->appVersion == "1.2.3");
    CHECK(!decoded->resume);
}

TEST(canonicalGreetingsRequireEveryPresenceFlagAndExactEnd)
{
    const auto greet = bodyOf(greetFixture());
    const auto welcome = bodyOf(welcomeFixture());
    REQUIRE(decodeGreet(greet));
    REQUIRE(decodeWelcome(welcome));
    for (std::size_t length = 0; length < greet.size(); ++length)
        CHECK(!decodeGreet(std::span(greet).first(length)));
    for (std::size_t length = 0; length < welcome.size(); ++length)
        CHECK(!decodeWelcome(std::span(welcome).first(length)));
    auto extra = greet;
    extra.push_back(0);
    CHECK(!decodeGreet(extra));
    extra = welcome;
    extra.push_back(0);
    CHECK(!decodeWelcome(extra));
    auto invalid = greet;
    invalid.back() = 2;
    CHECK(!decodeGreet(invalid));
    invalid = welcome;
    invalid.back() = 2;
    CHECK(!decodeWelcome(invalid));
}

TEST(canonicalAdmissionTargetsRefuseIgnoredSelectorsAndImplicitReplacement)
{
    for (const auto target : { AdmissionTarget::Session, AdmissionTarget::Workspace, AdmissionTarget::Control, AdmissionTarget::Management }) {
        auto greet = greetFixture();
        greet.request = {};
        greet.request.target = target;
        REQUIRE(decodeGreet(bodyOf(greet)));
        if (target == AdmissionTarget::Control || target == AdmissionTarget::Management) {
            greet.request.workspaceName = "not-a-control-field";
            CHECK(bodyOf(greet).empty());
            CHECK(!decodeGreet(rawGreet(greet)));
            greet.request.workspaceName.clear();
        }
        greet.request.sessionID = 17;
        CHECK(bodyOf(greet).empty());
        CHECK(!decodeGreet(rawGreet(greet)));
    }
    auto selected = greetFixture();
    selected.request.workspaceID = 23;
    CHECK(bodyOf(selected).empty());
    CHECK(!decodeGreet(rawGreet(selected)));
    selected.request.target = AdmissionTarget::Workspace;
    REQUIRE(decodeGreet(bodyOf(selected)));
    selected.request.sshHost = "other-host";
    CHECK(bodyOf(selected).empty());
    CHECK(!decodeGreet(rawGreet(selected)));
    selected.request.target = static_cast<AdmissionTarget>(4);
    CHECK(!decodeGreet(rawGreet(selected)));
}

TEST(canonicalLabelsPreserveUTF8SafePresentationWithoutClaimingAuthority)
{
    auto greet = greetFixture();
    greet.request.agentLabel = std::string(255, 'x') + "\xC2\xA3";
    const auto decoded = decodeGreet(rawGreet(greet));
    REQUIRE(decoded);
    CHECK(decoded->request.agentLabel == std::string(255, 'x'));
    CHECK(decoded->credential == greet.credential);
    const auto normalized = decodeGreet(bodyOf(greet));
    REQUIRE(normalized);
    CHECK(normalized->request.agentLabel == decoded->request.agentLabel);
    greet.request.agentLabel.push_back('\0');
    CHECK(!decodeGreet(rawGreet(greet)));
    greet.request.agentLabel = "\xFF";
    CHECK(!decodeGreet(rawGreet(greet)));
}

TEST(canonicalGreetingBoundsRefuseBeforeAppendingAnyBody)
{
    auto greet = greetFixture();
    greet.credential = std::vector<std::uint8_t>(maxCredentialBytes + 1, 7);
    CHECK(bodyOf(greet).empty());
    CHECK(!decodeGreet(rawGreet(greet)));
    greet = greetFixture();
    greet.request.workspaceName.assign(4097, 'x');
    CHECK(bodyOf(greet).empty());
    CHECK(!decodeGreet(rawGreet(greet)));
    greet = greetFixture();
    greet.request.sessionID = 0;
    greet.request.sshHost.assign(256, 'x');
    CHECK(bodyOf(greet).empty());
    CHECK(!decodeGreet(rawGreet(greet)));
    greet = greetFixture();
    greet.appVersion.assign(maxAppVersionBytes + 1, 'x');
    CHECK(bodyOf(greet).empty());
    CHECK(!decodeHandshakeBuild(rawGreet(greet)));
    CHECK(!decodeGreet(rawGreet(greet)));
    auto welcome = welcomeFixture();
    welcome.selection.socketName.assign(maxDaemonSocketNameBytes, 'x');
    std::vector<std::uint8_t> output { 0xA5 };
    encode(output, welcome);
    CHECK(output == std::vector<std::uint8_t> { 0xA5 });
    greet = greetFixture();
    PaneResume pane;
    for (std::uint64_t index = 0; index < maxResumeCachedRanges; ++index)
        pane.cachedRanges.push_back({ index * 2, index * 2 + 1 });
    greet.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = ID128 { 5, 6 }, .panes = std::vector<PaneResume>(maxPanesPerConnection, pane) };
    encode(output, greet);
    CHECK(output == std::vector<std::uint8_t> { 0xA5 });
}

TEST(aWelcomeCannotInventOrOmitItsServingSelection)
{
    auto welcome = welcomeFixture();
    REQUIRE(decodeWelcome(bodyOf(welcome)));
    welcome.selection.daemonInstanceID = 0;
    CHECK(bodyOf(welcome).empty());
    welcome = welcomeFixture();
    welcome.selection.socketName = "../other.sock";
    CHECK(bodyOf(welcome).empty());
    welcome = welcomeFixture();
    welcome.selection.buildFlags = 4;
    CHECK(bodyOf(welcome).empty());
    welcome = welcomeFixture();
    welcome.selection = {};
    CHECK(bodyOf(welcome).empty());
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        welcome = welcomeFixture();
        welcome.selection.target = target;
        CHECK(bodyOf(welcome).empty());
        welcome.selection.sessionID = 0;
        welcome.selection.workspaceID = 0;
        REQUIRE(decodeWelcome(bodyOf(welcome)));
    }
}

TEST(aWelcomeNamesTheInstanceItRestoredAWorkspaceFromOnlyForThatWorkspace)
{
    auto welcome = welcomeFixture();
    welcome.selection.target = AdmissionTarget::Workspace;
    welcome.selection.restoredFromDaemonInstanceID = 54;
    const auto decoded = decodeWelcome(bodyOf(welcome));
    REQUIRE(decoded);
    CHECK(decoded->selection.restoredFromDaemonInstanceID == 54);
    // Not for a lone session, and never the instance that answers.
    welcome.selection.target = AdmissionTarget::Session;
    CHECK(bodyOf(welcome).empty());
    welcome.selection.target = AdmissionTarget::Workspace;
    welcome.selection.restoredFromDaemonInstanceID = welcome.selection.daemonInstanceID;
    CHECK(bodyOf(welcome).empty());

    const AdmissionRequest request { .target = AdmissionTarget::Workspace, .sessionID = 99, .workspaceID = 23, .expectedDaemonInstanceID = 54 };
    ServingSelection restored = selectionFixture();
    restored.target = AdmissionTarget::Workspace;
    restored.restoredFromDaemonInstanceID = 54;
    CHECK(isRestoredReferenceAnswer(request, restored));
    // The instance that restored it lists it under an id of its own when the
    // one the reference held is another workspace's there: the host matched
    // the record to the reference, and the answer names the restored id.
    auto renumbered = restored;
    renumbered.workspaceID = 24;
    CHECK(isRestoredReferenceAnswer(request, renumbered));
    renumbered.restoredFromDaemonInstanceID = 0;
    CHECK(!isRestoredReferenceAnswer(request, renumbered));
    auto unnumbered = request;
    unnumbered.workspaceID = 0;
    CHECK(!isRestoredReferenceAnswer(unnumbered, restored));
    // The answer must name the instance asked for.
    auto otherInstance = restored;
    otherInstance.restoredFromDaemonInstanceID = 53;
    CHECK(!isRestoredReferenceAnswer(request, otherInstance));
    auto plain = restored;
    plain.restoredFromDaemonInstanceID = 0;
    CHECK(!isRestoredReferenceAnswer(request, plain));
    auto session = request;
    session.target = AdmissionTarget::Session;
    CHECK(!isRestoredReferenceAnswer(session, restored));
}

TEST(anUnboundWelcomeCannotCarryPaneResumeOutcomes)
{
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        auto welcome = welcomeFixture();
        welcome.resumed.push_back({ .paneID = { 17, 55 }, .outcome = ResumeOutcome::Gone, .inputWritten = std::nullopt });
        auto raw = bodyOf(welcome);
        REQUIRE(decodeWelcome(raw));
        WireReader prefix(raw);
        (void)prefix.u16();
        (void)prefix.utf8(maxAppVersionBytes);
        (void)prefix.take(32 + 16 * 3 + 1);
        REQUIRE(prefix.ok());
        const auto selection = prefix.offset();
        raw[selection] = std::to_underlying(target);
        for (std::size_t index = 1; index <= 8; ++index)
            raw[selection + index] = 0;
        CHECK(!decodeWelcome(raw));
        welcome.selection.target = target;
        welcome.selection.sessionID = 0;
        welcome.selection.workspaceID = 0;
        CHECK(bodyOf(welcome).empty());
    }
}

TEST(aCanonicalByeCarriesOptionalStructuredBuildDiagnosticsExactly)
{
    const auto absent = bodyOf(Bye {});
    CHECK(absent == std::vector<std::uint8_t>({ 0, 0, 0 }));
    CHECK(!decodeBye(std::span(absent).first(2)));
    auto invalid = absent;
    invalid.back() = 2;
    CHECK(!decodeBye(invalid));
    const auto reason = ByeReason::TokenRevoked;
    const auto message = spokenBye(wireVersion, "1.2.3", "linux", reason, "revoked");
    const auto body = bodyOf(message);
    const auto decoded = decodeBye(body);
    REQUIRE(decoded && decoded->peer);
    CHECK(decoded->reason == reason);
    CHECK(decoded->peer->build.wireVersion == wireVersion);
    CHECK(decoded->peer->build.appVersion == "1.2.3");
    CHECK(decoded->peer->platform == "linux");
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeBye(std::span(body).first(length)));
    invalid = body;
    invalid.push_back(0);
    CHECK(!decodeBye(invalid));
}

TEST(theGreetingVersionIsCheckedBeforeAdmissionOrTheRestOfTheBody)
{
    for (const bool hasCurrentVersion : { false, true }) {
        HostConnection host(measuredHostPolicy(0), hostFixture());
        unsigned admitted = 0;
        host.setGreetingAdmission([&](const Greet&) -> std::expected<HostIdentity, Bye> {
            ++admitted;
            return hostFixture();
        });
        std::vector<std::uint8_t> prefix;
        WireWriter writer(prefix);
        writer.u16(hasCurrentVersion ? wireVersion : wireVersion + 1);
        writer.u8(0xFF);
        host.receiveStream(frameOf(FrameKind::Greet, prefix), 1);
        CHECK_EQ(admitted, 0u);
        CHECK(host.state() == ConnectionState::Closed);
        const auto refusal = firstBye(host);
        REQUIRE(refusal && refusal->peer);
        CHECK(refusal->reason == (hasCurrentVersion ? ByeReason::Malformed : ByeReason::VersionMismatch));
        CHECK(refusal->peer->build.appVersion == "1.2.3");
        CHECK(refusal->peer->platform == "linux");
    }
}

TEST(greetingAdmissionReplacesPlaceholderAuthorityBeforeCreatingTheAttachment)
{
    Ubiquitty::Terminal terminal;
    PaneSource source(terminal, { 17, 55 });
    Secrets secrets;
    const auto configured = hostFixture();
    AttachmentTable table(configured.hostInstanceID, measuredResumePolicy(), secrets);
    HostConnection host(measuredHostPolicy(0), configured);
    host.serveFromTable(table, { .principal = "placeholder", .mayDrive = false, .verifiedKind = ClientKind::Human },
        4, [](ID128) -> PaneSource* { return nullptr; });
    unsigned calls = 0;
    host.setGreetingAdmission([&](const Greet& request) -> std::expected<HostIdentity, Bye> {
        ++calls;
        CHECK(request.credential == greetFixture().credential);
        CHECK(!host.attachment());
        auto verified = hostFixture();
        verified.principal = "verified-agent";
        verified.verifiedKind = ClientKind::Agent;
        verified.maxRole = Role::Agent;
        return verified;
    });
    const auto greeting = frameOf(FrameKind::Greet, bodyOf(greetFixture()));
    host.receiveStream(greeting, 1);
    REQUIRE(host.attachment());
    CHECK(host.attachment()->authority.principal == "verified-agent");
    CHECK(host.attachment()->authority.mayDrive);
    CHECK(host.attachment()->authority.verifiedKind == ClientKind::Agent);
    CHECK(host.attachment()->kind == ClientKind::Agent);
    CHECK(host.grantedRole() == Role::Agent);
    const auto handle = host.bindPane(source);
    REQUIRE(handle);
    CHECK(host.paneView(handle) == ViewState::RecordsOnly);
    host.receiveStream(greeting, 2);
    CHECK_EQ(calls, 1u);
    CHECK(host.state() == ConnectionState::Closed);
}

TEST(aTypedAdmissionRefusalCreatesNoAttachmentAndRetainsItsDiagnostics)
{
    Secrets secrets;
    const auto configured = hostFixture();
    AttachmentTable table(configured.hostInstanceID, measuredResumePolicy(), secrets);
    HostConnection host(measuredHostPolicy(0), configured);
    host.serveFromTable(table, {}, 4, [](ID128) -> PaneSource* { return nullptr; });
    host.setGreetingAdmission([](const Greet&) -> std::expected<HostIdentity, Bye> {
        return std::unexpected(spokenBye(wireVersion, "1.2.3", "linux", ByeReason::TokenRevoked, "revoked"));
    });
    host.receiveStream(frameOf(FrameKind::Greet, bodyOf(greetFixture())), 1);
    CHECK(!host.attachment());
    CHECK(!table.find({ 1, configured.hostInstanceID.low }));
    const auto refusal = firstBye(host);
    REQUIRE(refusal && refusal->peer);
    CHECK(refusal->reason == ByeReason::TokenRevoked);
    CHECK(refusal->peer->platform == "linux");
    ClientConnection client(measuredClientPolicy(), clientFixture());
    client.start();
    client.receiveStream(frameOf(FrameKind::Bye, bodyOf(*refusal)), 1);
    REQUIRE(client.bye() && client.bye()->peer);
    CHECK(client.bye()->peer->build.appVersion == "1.2.3");
}

TEST(aCanonicalWelcomeCannotReplaceAnExplicitRequestedSession)
{
    FrameObserver observer;
    ClientConnection client(measuredClientPolicy(), clientFixture(), &observer);
    client.start();
    auto welcome = welcomeFixture();
    welcome.selection.sessionID = 18;
    client.receiveStream(frameOf(FrameKind::Welcome, bodyOf(welcome)), 1);
    CHECK(client.state() == ConnectionState::Closed);
    CHECK(client.byeReason() == ByeReason::SessionGone);
    CHECK(!client.welcome());
    CHECK_EQ(observer.welcomes, 0u);
}

TEST(frameTracingCountsCanonicalFramesRatherThanReadChunks)
{
    FrameObserver observer;
    ClientConnection client(measuredClientPolicy(), clientFixture(), &observer);
    client.start();
    const auto welcome = frameOf(FrameKind::Welcome, bodyOf(welcomeFixture()));
    const auto probe = frameOf(FrameKind::Probe, bodyOf(Probe { .time = 9 }));
    for (const auto byte : welcome)
        client.receiveStream(std::span(&byte, 1), 1);
    client.receiveStream(probe, 2);
    REQUIRE(observer.frames.size() == 2);
    CHECK(observer.frames[0].first == FrameKind::Welcome);
    CHECK_EQ(observer.frames[0].second, welcome.size());
    CHECK(observer.frames[1].first == FrameKind::Probe);
    CHECK_EQ(observer.frames[1].second, probe.size());
    CHECK(observer.wasFrameReportedFirst);
    client.receiveDatagram(probe, 3);
    REQUIRE(observer.frames.size() == 3);
    CHECK(observer.frames.back().first == FrameKind::Probe);
    CHECK_EQ(observer.frames.back().second, probe.size());
}

TEST(aFrameTraceCallbackCanRetireTheReaderBeforeWelcomeIsApplied)
{
    FrameObserver observer;
    ClientAttachment attachment(measuredClientPolicy(), measuredLivenessPolicy(), clientFixture(), &observer);
    observer.retire = &attachment;
    attachment.connect(0);
    attachment.receiveStream(frameOf(FrameKind::Welcome, bodyOf(welcomeFixture())), 1);
    CHECK_EQ(observer.frames.size(), 1u);
    CHECK_EQ(observer.welcomes, 0u);
    CHECK(attachment.state() == ClientAttachmentState::Lost);
    CHECK(!attachment.held());
}

TEST(unboundGreetingsNeverCarryASecretOrProofResume)
{
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        auto greet = greetFixture();
        greet.request = { .target = target };
        REQUIRE(decodeGreet(bodyOf(greet)));
        for (const auto secret : { std::optional<ID128> {}, std::optional { ID128 { 5, 6 } } }) {
            greet.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = secret, .panes = {} };
            CHECK(bodyOf(greet).empty());
            CHECK(!decodeGreet(rawGreet(greet)));
        }
    }
}

TEST(anUnboundIdentityCannotResumeSupersedeOrCrossAHandover)
{
    Secrets secrets;
    AttachmentTable table({ 55, 56 }, measuredResumePolicy(), secrets);
    const ConnectionAuthority authority { .principal = "verified-fixture", .mayDrive = true, .verifiedKind = ClientKind::Human };
    const ID128 instance { 7, 8 };
    auto& lost = table.create(instance, ClientKind::Human, authority, 1, 0);
    const auto lostID = lost.attachmentID;
    table.noteTransportLost(lostID, 1, 1);
    auto& ephemeral = table.createUnbound(instance, ClientKind::Human, authority, 2, 2);
    const auto ephemeralID = ephemeral.attachmentID;
    const auto secret = ephemeral.resumeSecret;
    REQUIRE(table.find(lostID));
    CHECK(table.find(lostID)->state == AttachmentState::Lost);
    CHECK(!ephemeral.isResumable);
    const ResumeRequest resume { .attachmentID = ephemeralID, .resumeSecret = secret, .panes = {} };
    const auto refused = table.resume(resume, authority, 3, 3);
    REQUIRE(!refused);
    CHECK(refused.error() == ResumeRefusal::NotResumable);
    const std::array<std::uint8_t, 32> exporter {};
    const ResumeProof proof { .attachmentID = ephemeralID, .mac = resumeProofMAC(secret, exporter, ephemeralID, table.hostInstanceID()) };
    const auto proven = table.admitProof(proof, exporter, 3, 3);
    REQUIRE(!proven);
    CHECK(proven.error() == ResumeRefusal::NotResumable);
    const auto continuation = table.exportForHandover(3);
    REQUIRE(continuation.records.size() == 1);
    CHECK(continuation.records.front()->attachmentID == lostID);
    table.noteTransportLost(ephemeralID, 99, 4);
    REQUIRE(table.find(ephemeralID));
    table.noteTransportLost(ephemeralID, 2, 4);
    CHECK(!table.find(ephemeralID));
    REQUIRE(table.find(lostID));
    const ResumeRequest oldResume { .attachmentID = lostID, .resumeSecret = table.find(lostID)->resumeSecret, .panes = {} };
    REQUIRE(table.resume(oldResume, authority, 4, 5));
    const auto secondID = table.createUnbound(instance, ClientKind::Human, authority, 5, 6).attachmentID;
    table.noteTransportLost(lostID, 4, 7);
    REQUIRE(table.find(lostID));
    CHECK(table.find(lostID)->state == AttachmentState::Lost);
    CHECK(table.find(secondID));
}

TEST(unboundControlConnectionsCarryCallsButNeverBindPanes)
{
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        Ubiquitty::Terminal terminal;
        PaneSource source(terminal, { 17, 55 });
        Secrets secrets;
        auto identity = hostFixture();
        identity.selection.target = target;
        identity.selection.sessionID = 0;
        identity.selection.workspaceID = 0;
        AttachmentTable table(identity.hostInstanceID, measuredResumePolicy(), secrets);
        HostConnection host(measuredHostPolicy(0), identity);
        host.serveFromTable(table, { .principal = identity.principal, .mayDrive = true, .verifiedKind = ClientKind::Human },
            4, [&](ID128) { return &source; });
        auto clientIdentity = clientFixture();
        clientIdentity.request = { .target = target };
        ClientConnection client(measuredClientPolicy(), clientIdentity);
        client.start();
        host.receiveStream(client.takeStreamOutput(), 1);
        REQUIRE(host.state() == ConnectionState::Attached);
        REQUIRE(host.attachment());
        CHECK(!host.attachment()->isResumable);
        CHECK_EQ(host.bindPane(source), 0u);
        CHECK(host.boundPanes().empty());
        CHECK(host.attachment()->panes.empty());
        std::vector<std::uint8_t> welcome;
        REQUIRE(host.popStreamFrame(welcome, 1));
        client.receiveStream(welcome, 1);
        REQUIRE(client.state() == ConnectionState::Attached);
        const std::string call = R"({"jsonrpc":"2.0","id":"control","method":"host.methods","params":{}})";
        REQUIRE(client.call(call));
        host.receiveStream(client.takeStreamOutput(), 2);
        const auto calls = host.takeCalls();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().text == call);
        client.receiveStream(frameOf(FrameKind::PaneBind, bodyOf(PaneBind { .handle = 1, .paneID = source.paneID() })), 3);
        CHECK(client.state() == ConnectionState::Closed);
        CHECK(client.boundPanes().empty());
    }
}

TEST(aProofAdmittedPaneCannotBeRetargetedToUnboundControl)
{
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        Secrets secrets;
        auto identity = hostFixture();
        identity.selection.target = target;
        identity.selection.sessionID = 0;
        identity.selection.workspaceID = 0;
        AttachmentTable table(identity.hostInstanceID, measuredResumePolicy(), secrets);
        const ConnectionAuthority authority { .principal = identity.principal, .mayDrive = true, .verifiedKind = ClientKind::Human };
        const auto& prior = table.create({ 7, 8 }, ClientKind::Human, authority, 1, 0);
        const auto priorID = prior.attachmentID;
        const auto secret = prior.resumeSecret;
        table.noteTransportLost(priorID, 1, 1);
        const std::array<std::uint8_t, 32> exporter {};
        REQUIRE(table.admitProof({ .attachmentID = priorID, .mac = resumeProofMAC(secret, exporter, priorID, table.hostInstanceID()) }, exporter, 4, 2));
        HostConnection host(measuredHostPolicy(0), identity);
        host.serveFromTable(table, authority, 4, [](ID128) -> PaneSource* { return nullptr; });
        auto greet = greetFixture();
        greet.request = { .target = target };
        host.receiveStream(frameOf(FrameKind::Greet, bodyOf(greet)), 3);
        CHECK(host.state() == ConnectionState::Closed);
        CHECK(host.byeReason() == ByeReason::Malformed);
        CHECK(!host.attachment());
        CHECK(!table.provenAttachment(4));
        REQUIRE(table.find(priorID));
        CHECK(table.find(priorID)->state == AttachmentState::Lost);
        CHECK(table.find(priorID)->resumeSecret == secret);
    }
}

TEST(anUnboundClientPreservesItsPaneResumeWithoutSendingIt)
{
    for (const auto target : { AdmissionTarget::Control, AdmissionTarget::Management }) {
        ClientAttachment attachment(measuredClientPolicy(), measuredLivenessPolicy(), clientFixture());
        const auto prior = welcomeFixture();
        attachment.restore({ .hostInstanceID = prior.hostInstanceID, .attachmentID = prior.attachmentID, .resumeSecret = prior.resumeSecret, .panes = {} });
        attachment.setAdmission({ .target = target }, std::nullopt);
        const std::array<std::uint8_t, 32> exporter {};
        CHECK(!attachment.resumeProof(exporter));
        attachment.connect(1, ResumeCarrier::Proof);
        const auto bytes = attachment.takeStreamOutput();
        EnvelopeError error = EnvelopeError::None;
        const auto frame = decodeDatagram(bytes, error);
        REQUIRE(frame && frame->kind == FrameKind::Greet);
        const auto greet = decodeGreet(frame->body);
        REQUIRE(greet);
        CHECK(!greet->resume);
        auto controlWelcome = welcomeFixture();
        controlWelcome.hostInstanceID = { 90, 91 };
        controlWelcome.attachmentID = { 92, 93 };
        controlWelcome.resumeSecret = { 94, 95 };
        controlWelcome.selection.target = target;
        controlWelcome.selection.sessionID = 0;
        controlWelcome.selection.workspaceID = 0;
        attachment.receiveStream(frameOf(FrameKind::Welcome, bodyOf(controlWelcome)), 2);
        CHECK(attachment.state() == ClientAttachmentState::Live);
        const auto held = attachment.held();
        REQUIRE(held);
        CHECK(held->hostInstanceID == prior.hostInstanceID);
        CHECK(held->attachmentID == prior.attachmentID);
        CHECK(held->resumeSecret == prior.resumeSecret);
        attachment.transportLost(3);
        attachment.setAdmission(greetFixture().request, std::nullopt);
        REQUIRE(attachment.resumeProof(exporter));
        attachment.connect(4);
        const auto resumedBytes = attachment.takeStreamOutput();
        const auto resumedFrame = decodeDatagram(resumedBytes, error);
        REQUIRE(resumedFrame && resumedFrame->kind == FrameKind::Greet);
        const auto resumed = decodeGreet(resumedFrame->body);
        REQUIRE(resumed && resumed->resume);
        CHECK(resumed->resume->attachmentID == prior.attachmentID);
        CHECK(resumed->resume->resumeSecret == prior.resumeSecret);
    }
}

UBIQUITTY_TEST_MAIN()
