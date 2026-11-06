// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"
#include "Helpers/TempDir.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/HostTableHandover.h"

#include <array>
#include <ctime>

namespace Host = Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Layout = Ubiquitty::Session::Layout;
namespace Identity = Ubiquitty::Session::Identity;
using Screen = Host::ScreenPlaneAccess;
using Error = Sync::HandoverBodyError;

namespace Ubiquitty::Host {

/// Exercises admission and publication against actual private daemon owners.
struct HandoverAccess {
    using Session = Daemon::Session;
    static Sync::ID128 instance(const Daemon& daemon) { return daemon.m_hostInstanceID; }
    static std::uint64_t issuer(const Daemon& daemon) { return daemon.m_daemonInstanceID; }
    static auto& sessions(Daemon& daemon) { return daemon.m_sessions; }
    static auto& workspaces(Daemon& daemon) { return daemon.m_workspaces; }
    static auto& attachments(Daemon& daemon) { return *daemon.m_attachments; }
    static auto& globals(Daemon& daemon) { return daemon.m_events; }
    static void paths(Daemon& daemon, const std::string& root, const std::string& socket)
    {
        daemon.m_tokenPath = root + "/tokens";
        daemon.m_layoutRecordPath = socket + ".layouts";
        daemon.m_backgroundEventPath = socket + ".background-events";
    }
    static void place(Daemon& daemon, Session& session)
    {
        auto& workspace = daemon.hostedWorkspaceFor(session);
        const auto surface = workspace.layout.surfaceIDAt(0);
        const auto* window = workspace.layout.surface(surface);
        session.placement = { surface, window->tabs[0].id, workspace.paneFor(session.id) };
    }
    static void seed(Daemon& daemon, Session& session)
    {
        daemon.m_nextClientID = 19;
        daemon.m_nextStructureConsentID = 29;
        daemon.m_nextConsentRequestID = 37;
        daemon.m_nextClipboardRequestID = 41;
        daemon.m_nextScrollbackFoldMilliseconds = 71;
        daemon.m_lastScrollbackFoldSessionID = session.id;
        daemon.m_configuration.fontSize = 19.25;
        daemon.m_configuration.titleWindowText = "carried title";
        daemon.m_alwaysAllowedAgents = { "fixture-agent" };
        daemon.m_grantStore.grant(Capability::DesktopNotification, CapabilityOrigin::Agent, "fixture-agent", CapabilityGrantScope::Session, session.id);
        const std::string token(32, 'a');
        daemon.m_pendingQUICTokens.push_back({ token, 999999, 17, Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe }, Crypto::sha256Hex(token), false });
        daemon.m_closuresOwed.push_back(daemon.m_workspaces.front()->workspaceID);
        daemon.m_terminationsOwed.emplace_back(session.id, session.screenSource->paneID());
        daemon.m_events.append({ .kind = Ubiquitty::Session::EventKind::TitleChanged, .sessionID = session.id, .text = "retained event" });
        daemon.m_everHadSession = true;
    }
    static bool freeze(Daemon& daemon)
    {
        const auto now = ScreenPlaneAccess::now();
        if (!daemon.beginHandoverQuiesce(now))
            return false;
        daemon.serviceHandoverQuiesce(now);
        daemon.serviceHandoverQuiesce(now + 10'000);
        return daemon.isQuiesced();
    }
    static void thaw(Daemon& daemon) { daemon.endHandoverQuiesce(); }
    static bool hasOriginalDestination(const Daemon& daemon)
    {
        return daemon.m_sessions.empty() && daemon.m_workspaces.empty() && daemon.m_attachments->isPristine()
            && daemon.m_nextClientID == 1 && daemon.m_configuration.fontSize != 19.25
            && daemon.m_pendingCapabilityGrants.empty() && daemon.m_pendingQUICTokens.empty();
    }
    static void checkPublication(const Daemon& daemon)
    {
        CHECK_EQ(daemon.m_workspaces.size(), 2u);
        CHECK_EQ(daemon.m_nextClientID, 19u);
        CHECK_EQ(daemon.m_nextStructureConsentID, 29u);
        CHECK_EQ(daemon.m_nextConsentRequestID, 37u);
        CHECK_EQ(daemon.m_nextClipboardRequestID, 41u);
        CHECK_EQ(daemon.m_nextScrollbackFoldMilliseconds, 71u);
        CHECK(daemon.m_configuration.fontSize == 19.25);
        CHECK(daemon.m_configuration.titleWindowText == "carried title");
        CHECK(daemon.m_alwaysAllowedAgents == std::vector<std::string> { "fixture-agent" });
        CHECK(daemon.m_pendingCapabilityGrants.empty());
        CHECK_EQ(daemon.m_pendingQUICTokens.size(), 1u);
        CHECK(daemon.m_consentQueue.continuation(64)->pending.empty());
        CHECK_EQ(daemon.m_closuresOwed.size(), 1u);
        CHECK_EQ(daemon.m_terminationsOwed.size(), 1u);
        CHECK_EQ(daemon.m_events.continuation().count, 1u);
        CHECK(daemon.m_everHadSession);
    }
    static void badGrantedSession(Daemon& daemon)
    {
        daemon.m_grantStore.grant(Capability::DesktopNotification, CapabilityOrigin::Agent, "fixture-agent", CapabilityGrantScope::Session, 200);
    }
    static void retainQuestion(Daemon& daemon, Session& session)
    {
        daemon.m_pendingCapabilityGrants.emplace(31, Daemon::PendingCapabilityGrant {
                                                         .capability = Capability::DesktopNotification,
                                                         .origin = CapabilityOrigin::Agent,
                                                         .identity = "fixture-agent",
                                                         .sessionID = session.id,
                                                         .recipients = { { .clientID = 17, .sessionID = session.id } },
                                                     });
        CHECK(daemon.m_consentQueue.recordPending(Capability::DesktopNotification, CapabilityOrigin::Agent, "fixture-agent"));
    }
    static bool hasQuestion(const Daemon& daemon) { return daemon.m_pendingCapabilityGrants.contains(31); }
    static void badOwedPane(Daemon& daemon) { daemon.m_terminationsOwed.front().second.high ^= 1; }
    static Sync::ID128 retainHistoricalPane(Daemon& daemon, Sync::ID128 pane, bool isHeld) // bool-ok: Copies a stored baseline field.
    {
        auto& record = daemon.m_attachments->create({ 11, 29 }, Sync::ClientKind::Human,
            { "fixture-person", true, Sync::ClientKind::Human }, 17, ScreenPlaneAccess::now());
        record.paneFor(pane).isHeld = isHeld;
        daemon.m_nextSessionID = 7;
        return record.attachmentID;
    }
    static void commit(Daemon& daemon, HostTableHandover::Candidate&& candidate) { HostTableHandover::commit(daemon, std::move(candidate)); }
    static void label(Daemon& daemon, Identity::ObjectID object, std::string_view text) { CHECK(daemon.m_labels.set(object, text) == Identity::LabelVerdict::Accepted); }
    static std::string label(const Daemon& daemon, Identity::ObjectID object) { return std::string(daemon.m_labels.label(object)); }
    static void tag(Daemon& daemon, Identity::ObjectID object, const std::vector<std::string>& names) { CHECK(daemon.m_tags.set(object, names) == Identity::TagVerdict::Accepted); }
    static std::vector<std::string> tags(const Daemon& daemon, Identity::ObjectID object)
    {
        const auto held = daemon.m_tags.tags(object);
        return { held.begin(), held.end() };
    }
    static void settleNothingOwed(Daemon& daemon)
    {
        daemon.m_closuresOwed.clear();
        daemon.m_terminationsOwed.clear();
    }
    static const auto* structure(const Daemon& daemon, std::uint32_t workspaceID)
    {
        const auto found = daemon.m_structureRecords.find(workspaceID);
        return found == daemon.m_structureRecords.end() ? nullptr : &found->second;
    }
};

} // namespace Ubiquitty::Host

namespace {
using Access = Host::HandoverAccess;
constexpr Host::HostTableHandover::Limits limits {
    8 * 1024 * 1024,
    { 2 * 1024 * 1024, 64, 128, 256, 4096, 4096, 65536 },
    { 2 * 1024 * 1024, 16, 32, 64, 128, 32, 65536, 65536 },
    { .maxSectionBytes = 1024 * 1024, .maxGrants = 64, .maxConsentEntries = 64, .maxQUICTokens = 64, .maxStandingAgents = 64, .maxStringBytes = 65536 },
    { 2 * 1024 * 1024, 4096, 64, 64, 65536, { 1024 * 1024, 65536, 4096, 65536 } }
};

struct Fixture {
    Ubiquitty::Test::TempDir directory;
    Host::Daemon holder;
    Host::Daemon successor;
    std::string socket;
    std::vector<std::unique_ptr<Access::Session>> sessions;

    Fixture()
    {
        REQUIRE(directory.valid());
        socket = directory.path() + "/host-predecessor.sock";
        Access::paths(holder, directory.path(), socket);
        Access::paths(successor, directory.path(), directory.path() + "/host-successor.sock");
        for (unsigned index = 0; index < 2; ++index) {
            auto& source = Screen::makeSession(holder, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
            source.screenSource = std::make_unique<Sync::PaneSource>(*source.term, Sync::ID128 { source.id, Access::issuer(holder) });
            source.coherence = Ubiquitty::Runtime::FrameCoherence({ .isPaced = false });
            source.workspaceName = "workspace-" + std::to_string(index);
            Access::place(holder, source);
            Screen::publish(holder, source, Ubiquitty::Runtime::FrameClass::Structural);
            auto session = std::make_unique<Access::Session>();
            session->id = source.id;
            session->placement = source.placement;
            session->term = std::make_unique<Ubiquitty::Terminal>();
            session->screenSource = std::make_unique<Sync::PaneSource>(*session->term, source.screenSource->paneID());
            sessions.push_back(std::move(session));
        }
        Access::seed(holder, *Access::sessions(holder).front());
    }

    std::expected<Sync::HandoverBytes, Error> capture()
    {
        const bool isFrozen = Access::freeze(holder);
        CHECK(isFrozen);
        if (!isFrozen)
            return std::unexpected(Error::NotAtGround);
        return Host::HostTableHandover::capture(holder, Screen::now(), limits);
    }
    auto prepare(std::span<const std::uint8_t> record)
    {
        return Host::HostTableHandover::prepare(successor, record, Access::instance(holder), Screen::now(), static_cast<std::uint64_t>(::time(nullptr)), socket, limits);
    }
};

/// Replaces only the canonical authority section of a valid captured record
/// with one whose pending-question count names a question, preserving all
/// other sections and reminting both digests.
std::optional<std::vector<std::uint8_t>> withUnapprovedQuestion(std::span<const std::uint8_t> original)
{
    Sync::WireReader reader(original);
    const auto version = reader.u16();
    const auto magic = reader.u32();
    std::array<std::span<const std::uint8_t>, 4> sections;
    for (auto& section : sections) {
        const auto size = reader.u32();
        section = reader.take(size);
    }
    const auto digestOffset = reader.offset();
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (!reader.ok() || !reader.isAtEnd() || Sync::digestOf(original.first(digestOffset)) != digest)
        return std::nullopt;
    auto authority = Host::decodeHandoverAuthority(sections[2], limits.authority);
    if (!authority)
        return std::nullopt;
    const Ubiquitty::CapabilityGrantKey key { Ubiquitty::Capability::DesktopNotification, Ubiquitty::CapabilityOrigin::Agent, "fixture-agent" };
    authority->pendingConsent.push_back(key);
    // Nothing follows the count but these, the request counters, two flags
    // and the digest, so its place is known from the end.
    authority->quicTokens.clear();
    authority->alwaysAllowedAgents.clear();
    const auto encoded = Host::encodeHandoverAuthority(*authority, limits.authority);
    if (!encoded || !Host::decodeHandoverAuthority(encoded->view(), limits.authority))
        return std::nullopt;
    constexpr std::size_t afterCount = 4 + 4 + 4 + 4 + 1 + 1 + 16;
    std::vector<std::uint8_t> questioned(encoded->view().begin(), encoded->view().end());
    if (questioned.size() < afterCount + 4)
        return std::nullopt;
    questioned[questioned.size() - afterCount - 4] = 1;
    questioned.resize(questioned.size() - 16);
    const auto sectionDigest = Sync::digestOf(questioned);
    Sync::WireWriter sectionWriter(questioned);
    sectionWriter.u64(sectionDigest.low);
    sectionWriter.u64(sectionDigest.high);
    sections[2] = questioned;
    std::vector<std::uint8_t> replaced;
    Sync::WireWriter writer(replaced);
    writer.u16(version);
    writer.u32(magic);
    for (const auto section : sections) {
        writer.u32(static_cast<std::uint32_t>(section.size()));
        writer.raw(section);
    }
    const auto replacementDigest = Sync::digestOf(replaced);
    writer.u64(replacementDigest.low);
    writer.u64(replacementDigest.high);
    return replaced;
}
} // namespace

TEST(actualHostTablesRemainPrivateUntilTheWholeHostCommits)
{
    Fixture fixture;
    const auto record = fixture.capture();
    REQUIRE(record);
    auto candidate = fixture.prepare(record->view());
    REQUIRE(candidate);
    CHECK(Access::hasOriginalDestination(fixture.successor));
    CHECK((*candidate)->validateSessions(fixture.sessions));
    CHECK((*candidate)->workspaces().nextClientID == 19);
    CHECK((*candidate)->globals().events.ring[0].text == "retained event");
    Access::commit(fixture.successor, std::move(**candidate));
    Access::checkPublication(fixture.successor);
    CHECK(Access::workspaces(fixture.holder).size() == 2);
}

TEST(aHostsWorkspacesKeepTheirIdentityAndTheirStructureRevisionAcrossTheTables)
{
    Fixture fixture;
    auto& holder = fixture.holder;
    auto& session = *Access::sessions(holder).front();
    const auto& workspace = *Access::workspaces(holder).front();
    const std::uint32_t workspaceID = workspace.workspaceID;
    const auto [window, tab, pane] = session.placement;
    const Identity::ObjectID workspaceObject { Identity::ObjectKind::Workspace, workspaceID };
    const Identity::ObjectID windowObject { Identity::ObjectKind::Window, std::to_underlying(window) };
    const Identity::ObjectID tabObject { Identity::ObjectKind::Tab, std::to_underlying(tab) };
    const Identity::ObjectID paneObject { Identity::ObjectKind::Pane, std::to_underlying(pane) };
    const Identity::ObjectID sessionObject { Identity::ObjectKind::Session, session.id };
    for (const auto& object : { workspaceObject, windowObject, tabObject, paneObject, sessionObject }) {
        Access::label(holder, object, "label " + std::to_string(std::to_underlying(object.kind)));
        Access::tag(holder, object, { "tag-" + std::to_string(std::to_underlying(object.kind)), "carried" });
    }

    // A person reads the structure, so the host records it; their link then
    // goes, as every link does when a host hands over, and the structure is
    // left to be rebuilt. The host's pass that serves them would settle the
    // closure and the ending the fixture owes, which this case does not hand
    // over.
    Access::settleNothingOwed(holder);
    const std::size_t person = Screen::attach(holder, session.id);
    Screen::grantScope(holder, person, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::markInteractiveUser(holder, person);
    Screen::bindWorkspace(holder, person, session);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(holder, person, client);
    const auto* recorded = Access::structure(holder, workspaceID);
    REQUIRE(recorded && recorded->revision > 0 && !recorded->arrangement.empty());
    const std::uint64_t revision = recorded->revision;
    const std::string arrangement = recorded->arrangement;
    // The attachment outlives its grace, so no baseline of it needs a pane
    // this fixture's sessions stand in for.
    Screen::reap(holder, person);
    Screen::reapAttachments(holder, Screen::now() + Sync::measuredResumePolicy().lostGraceMicroseconds + 1);

    const auto record = fixture.capture();
    REQUIRE(record);
    auto candidate = fixture.prepare(record->view());
    REQUIRE(candidate);
    REQUIRE((*candidate)->validateSessions(fixture.sessions));
    Access::commit(fixture.successor, std::move(**candidate));

    const auto& successor = fixture.successor;
    REQUIRE(Access::workspaces(fixture.successor).size() == 2);
    const auto carried = std::ranges::find(Access::workspaces(fixture.successor), workspaceID, [](const auto& held) { return held->workspaceID; });
    REQUIRE(carried != Access::workspaces(fixture.successor).end());
    CHECK_MSG((*carried)->name == workspace.name, "the workspace's name was not carried");
    CHECK_MSG((*carried)->layout.surfaceIDAt(0) == window && (*carried)->layout.surface(window) && (*carried)->layout.surface(window)->tabs[0].id == tab
            && (*carried)->paneFor(session.id) == pane,
        "the window, tab and pane do not keep their ids");
    for (const auto& object : { workspaceObject, windowObject, tabObject, paneObject, sessionObject }) {
        CHECK_MSG(Access::label(successor, object) == Access::label(holder, object), ("a label was not carried for kind " + std::to_string(std::to_underlying(object.kind))).c_str());
        CHECK_MSG(Access::tags(successor, object) == Access::tags(holder, object), ("tags were not carried for kind " + std::to_string(std::to_underlying(object.kind))).c_str());
    }
    // The revision continues, and the arrangement it says is known, so a
    // layout operation asked about it stays current until the arrangement
    // moves rather than being refused at the first change of anything.
    const auto* continued = Access::structure(successor, workspaceID);
    REQUIRE(continued);
    CHECK_MSG(continued->revision == revision, "the structure's revision did not continue");
    CHECK_MSG(continued->arrangement == arrangement, "the arrangement the handed-over revision says was not known to the successor");
    CHECK_MSG(continued->arrangedAt == revision, "a layout operation asked about the handed-over revision is not current");
}

TEST(abandoningPreparedHostTablesLeavesTheSuccessorAndPredecessorUnchanged)
{
    Fixture fixture;
    const auto record = fixture.capture();
    REQUIRE(record);
    {
        auto candidate = fixture.prepare(record->view());
        REQUIRE(candidate);
        REQUIRE((*candidate)->validateSessions(fixture.sessions));
    }
    CHECK(Access::hasOriginalDestination(fixture.successor));
    CHECK(Access::workspaces(fixture.holder).size() == 2);
    const auto repeated = Host::HostTableHandover::capture(fixture.holder, Screen::now(), limits);
    REQUIRE(repeated);
    CHECK(std::ranges::equal(repeated->view(), record->view()));
}

TEST(pendingCapabilityQuestionsNeverCrossTheHostTableBoundary)
{
    Fixture fixture;
    const auto original = fixture.capture();
    REQUIRE(original);
    const auto baseline = fixture.prepare(original->view());
    REQUIRE(baseline);
    CHECK(Access::hasOriginalDestination(fixture.successor));

    // Capture rechecks the non-transferable question even when the daemon
    // had already reached its frozen boundary before the question was seeded.
    auto& session = *Access::sessions(fixture.holder).front();
    Access::retainQuestion(fixture.holder, session);
    const auto refused = Host::HostTableHandover::capture(fixture.holder, Screen::now(), limits);
    CHECK(!refused && refused.error() == Error::NotAtGround);
    CHECK(Access::hasQuestion(fixture.holder));

    // A record naming a question cannot give it new recipient owners during
    // import, nor partially publish any otherwise valid host table.
    const auto offered = withUnapprovedQuestion(original->view());
    REQUIRE(offered);
    const auto imported = fixture.prepare(*offered);
    CHECK(!imported && imported.error().error == Error::NotAtGround);
    CHECK(Access::hasOriginalDestination(fixture.successor));
    CHECK(Access::hasQuestion(fixture.holder));
}

TEST(sessionCrossBindingsRejectMissingDuplicateRenumberedAndMisplacedOwners)
{
    Fixture fixture;
    const auto record = fixture.capture();
    REQUIRE(record);
    auto candidate = fixture.prepare(record->view());
    REQUIRE(candidate);
    const auto validates = [&] { return (*candidate)->validateSessions(fixture.sessions); };
    CHECK(validates());
    auto removed = std::move(fixture.sessions.back());
    fixture.sessions.pop_back();
    CHECK(!validates());
    fixture.sessions.push_back(std::move(removed));
    const auto identity = fixture.sessions.back()->id;
    fixture.sessions.back()->id = fixture.sessions.front()->id;
    CHECK(!validates());
    fixture.sessions.back()->id = identity;
    const auto placement = fixture.sessions.back()->placement;
    fixture.sessions.back()->placement = fixture.sessions.front()->placement;
    CHECK(!validates());
    fixture.sessions.back()->placement = placement;
    // A pane keeps the identity the host that started it minted after that
    // host has left the carried identities, so a pane minted outside them is
    // still the pane handed over. Its number is still its session's.
    const auto pane = fixture.sessions.back()->screenSource->paneID();
    fixture.sessions.back()->screenSource = std::make_unique<Sync::PaneSource>(*fixture.sessions.back()->term, Sync::ID128 { pane.low, pane.high ^ 1 });
    CHECK(validates());
    fixture.sessions.back()->screenSource = std::make_unique<Sync::PaneSource>(*fixture.sessions.back()->term, Sync::ID128 { pane.low + 100, pane.high });
    CHECK(!validates());
    // It must name a host.
    fixture.sessions.back()->screenSource = std::make_unique<Sync::PaneSource>(*fixture.sessions.back()->term, Sync::ID128 { pane.low, 0 });
    CHECK(!validates());
    CHECK(Access::hasOriginalDestination(fixture.successor));
}

TEST(authorityAndOwedTerminationMustResolveToTheirExactSession)
{
    for (unsigned kind = 0; kind < 2; ++kind) {
        Fixture fixture;
        if (kind)
            Access::badOwedPane(fixture.holder);
        else
            Access::badGrantedSession(fixture.holder);
        const auto record = fixture.capture();
        REQUIRE(record);
        auto candidate = fixture.prepare(record->view());
        REQUIRE(candidate);
        CHECK(!(*candidate)->validateSessions(fixture.sessions));
        CHECK(Access::hasOriginalDestination(fixture.successor));
    }
}

TEST(historicalBaselinesMayOutlivePanesButHeldBaselinesNeedALiveOwner)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        Fixture fixture;
        auto pane = Sync::ID128 { 6, Access::issuer(fixture.holder) };
        if (kind == 2)
            pane.high ^= 1;
        if (kind == 3)
            pane.low = 7;
        Access::retainHistoricalPane(fixture.holder, pane, kind == 1);
        const auto record = fixture.capture();
        REQUIRE(record);
        auto candidate = fixture.prepare(record->view());
        REQUIRE(candidate);
        CHECK((*candidate)->validateSessions(fixture.sessions) == !kind);
        CHECK(Access::hasOriginalDestination(fixture.successor));
    }
}

TEST(anUnheldBaselineMayNameTheHostOfAPaneHandedOver)
{
    Fixture fixture;
    const auto departed = Access::issuer(fixture.holder) ^ 1;
    Access::retainHistoricalPane(fixture.holder, Sync::ID128 { 6, departed }, false);
    const auto record = fixture.capture();
    REQUIRE(record);
    auto candidate = fixture.prepare(record->view());
    REQUIRE(candidate);
    CHECK(!(*candidate)->validateSessions(fixture.sessions));
    // The host that minted a pane handed over is known, though it has left
    // the carried identities, so the baseline is kept.
    auto& session = *fixture.sessions.back();
    const auto number = session.screenSource->paneID().low;
    session.screenSource = std::make_unique<Sync::PaneSource>(*session.term, Sync::ID128 { number, departed });
    CHECK_EQ((*candidate)->forgetBaselinesOfDepartedPanes(fixture.sessions), 0u);
    CHECK((*candidate)->validateSessions(fixture.sessions));
}

TEST(aClosedPaneWhoseHostLeftTheWindowCostsOnlyItsBaselines)
{
    Fixture fixture;
    const Sync::ID128 closed { 6, Access::issuer(fixture.holder) ^ 1 };
    const auto attachment = Access::retainHistoricalPane(fixture.holder, closed, false);
    const auto record = fixture.capture();
    REQUIRE(record);
    auto candidate = fixture.prepare(record->view());
    REQUIRE(candidate);
    CHECK(!(*candidate)->validateSessions(fixture.sessions));
    // The attachment forgets the pane, so a later bind of it is sent a
    // checkpoint, and the handover goes ahead.
    CHECK_EQ((*candidate)->forgetBaselinesOfDepartedPanes(fixture.sessions), 1u);
    const auto* kept = (*candidate)->attachments().find(attachment);
    CHECK(kept && std::ranges::none_of(kept->panes, [&](const auto& pane) { return pane->paneID == closed; }));
    CHECK((*candidate)->validateSessions(fixture.sessions));
    CHECK(Access::hasOriginalDestination(fixture.successor));
}

TEST(hostAdmissionPinsPathsAndIssuerBeforeAnyTableIsPublished)
{
    Fixture fixture;
    const auto record = fixture.capture();
    REQUIRE(record);
    const std::string otherSocket = fixture.socket + ".other";
    auto wrongPath = Host::HostTableHandover::prepare(fixture.successor, record->view(), Access::instance(fixture.holder), Screen::now(), 0, otherSocket, limits);
    CHECK(!wrongPath);
    // The refusal names the check: where the records are, and the socket the
    // holder was reached at.
    CHECK(!wrongPath && wrongPath.error().error == Error::Malformed && wrongPath.error().check.find(fixture.socket + ".layouts") != std::string::npos
        && wrongPath.error().check.find(otherSocket) != std::string::npos);
    auto wrongIssuer = Host::HostTableHandover::prepare(fixture.successor, record->view(), Sync::ID128 { 99, 88 }, Screen::now(), 0, fixture.socket, limits);
    CHECK(!wrongIssuer);
    CHECK(Access::hasOriginalDestination(fixture.successor));
    const auto wrongBound = Host::HostTableHandover::prepare(fixture.successor, record->view(), Access::instance(fixture.holder), Screen::now(), 0, fixture.socket,
        Host::HostTableHandover::Limits { .maxRecordBytes = record->size - 1 });
    CHECK(!wrongBound && wrongBound.error().error == Error::OverCapacity);
}

TEST(recordsAreKeptBesideExactlyOneSocket)
{
    const std::string socket = "/tmp/ubq-control/host-0123456789abcdef.sock";
    CHECK(Host::HostTableHandover::areRecordsKeptBeside(socket + ".layouts", socket + ".background-events", socket));
    CHECK(!Host::HostTableHandover::areRecordsKeptBeside(socket + ".layouts", socket + ".background-events", "/tmp/ubq-control/host-fedcba9876543210.sock"));
    CHECK(!Host::HostTableHandover::areRecordsKeptBeside(socket + ".layouts", "/tmp/ubq-control/host-fedcba9876543210.sock.background-events", socket));
    CHECK(!Host::HostTableHandover::areRecordsKeptBeside(socket + "x.layouts", socket + "x.background-events", socket));
    CHECK(!Host::HostTableHandover::areRecordsKeptBeside(socket + ".layouts", socket + ".background-events", socket.substr(0, socket.size() - 1)));
}

TEST(unfrozenOrDamagedTablesRefuseWithoutDestinationChanges)
{
    Fixture fixture;
    const auto live = Host::HostTableHandover::capture(fixture.holder, Screen::now(), limits);
    CHECK(!live && live.error() == Error::NotAtGround);
    auto record = fixture.capture();
    REQUIRE(record);
    CHECK(!fixture.prepare(record->view().first(record->size - 1)));
    record->data[record->size - 17] ^= 1;
    CHECK(!fixture.prepare(record->view()));
    CHECK(Access::hasOriginalDestination(fixture.successor));
}

UBIQUITTY_TEST_MAIN()
