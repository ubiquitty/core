// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/HostHandover.h"
#include "TerminalFixture.h"

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace Host = Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace PAL = Ubiquitty::PAL;
using Screen = Host::ScreenPlaneAccess;

namespace Ubiquitty::Host {

/// Drives only the unpublished whole-host ownership boundary.
struct HandoverAccess {
    static auto& sessions(Daemon& daemon) { return daemon.m_sessions; }
    static Sync::ID128 instance(const Daemon& daemon) { return daemon.m_hostInstanceID; }
    static void configure(Daemon& daemon, const std::string& root, const std::string& socket)
    {
        daemon.m_tokenPath = root + "/tokens";
        daemon.m_layoutRecordPath = socket + ".layouts";
        daemon.m_backgroundEventPath = socket + ".background-events";
    }
    static void place(Daemon& daemon, Daemon::Session& session)
    {
        auto& workspace = daemon.hostedWorkspaceFor(session);
        const auto surface = workspace.layout.surfaceIDAt(0);
        session.placement = { surface, workspace.layout.surface(surface)->tabs[0].id, workspace.paneFor(session.id) };
        session.screenSource = std::make_unique<Sync::PaneSource>(*session.term, Sync::ID128 { session.id, daemon.m_daemonInstanceID });
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
    static bool isUnpublished(const Daemon& daemon)
    {
        return daemon.m_sessions.empty() && daemon.m_workspaces.empty() && daemon.m_attachments->isPristine();
    }
    static std::uint64_t issuer(const Daemon& daemon) { return daemon.m_daemonInstanceID; }
    /// An attachment that once held `pane` and holds it no longer.
    static void retainClosedPane(Daemon& daemon, Sync::ID128 pane)
    {
        auto& record = daemon.m_attachments->create({ 11, 29 }, Sync::ClientKind::Human,
            { "fixture-person", true, Sync::ClientKind::Human }, 17, ScreenPlaneAccess::now());
        record.paneFor(pane).isHeld = false;
    }
    static const auto& stagedSessions(const HostHandover::Candidate& candidate) { return candidate.m_sessions; }
    static void commit(HostHandover::Candidate&& candidate) { HostHandover::commit(std::move(candidate)); }
};

} // namespace Ubiquitty::Host

namespace {
using Access = Host::HandoverAccess;

struct StreamObservation {
    unsigned reads = 0;
    unsigned writes = 0;
    unsigned drains = 0;
    unsigned processQueries = 0;
    unsigned destructions = 0;
    unsigned terminations = 0;
    PAL::ProcessClosePolicy policy = PAL::ProcessClosePolicy::Terminate;
};

/// Uses the real pending-byte owner while recording every external operation.
class ObservedStream final : public PAL::Stream {
public:
    explicit ObservedStream(std::shared_ptr<StreamObservation> observation)
        : m_observation(std::move(observation))
    {
    }
    ~ObservedStream() override
    {
        ++m_observation->destructions;
        m_observation->terminations += m_observation->policy == PAL::ProcessClosePolicy::Terminate;
    }
    int read(std::span<char>) override
    {
        ++m_observation->reads;
        return std::to_underlying(PAL::StreamStatus::Again);
    }
    int write(std::span<const char>) override
    {
        ++m_observation->writes;
        return std::to_underlying(PAL::StreamStatus::Again);
    }
    int writeAll(std::span<const char> bytes) override { return m_queue.submit(bytes, sink, this); }
    bool drainPendingWrite() override
    {
        ++m_observation->drains;
        return m_queue.hasPending();
    }
    bool hasPendingWrite() const override { return m_queue.hasPending(); }
    std::size_t pendingWriteBytes() const override { return m_queue.pendingBytes(); }
    std::optional<PAL::OutputQueue::Unwritten> copyPendingWrite() const override { return m_queue.copyUnwritten(); }
    bool restorePendingWrite(PAL::OutputQueue::Unwritten& pending) override { return m_queue.restoreUnwritten(pending); }
    void setProcessClosePolicy(PAL::ProcessClosePolicy policy) override { m_observation->policy = policy; }
    void setWindowSize(int, int, int, int) override { CHECK(false); }
    int lastError() const override { return 0; }
    int fileDescriptor() const override { return -1; }
    PAL::ProcessFacts& process() override
    {
        ++m_observation->processQueries;
        return m_facts;
    }

private:
    static int sink(void* context, std::span<const char> bytes)
    {
        (void)static_cast<ObservedStream*>(context)->write(bytes);
        return 0;
    }
    std::shared_ptr<StreamObservation> m_observation;
    PAL::OutputQueue m_queue;
    Ubiquitty::TestDoubles::ProcessFactsDouble m_facts;
};

constexpr Sync::HandoverTerminalLimits terminalLimits {
    64 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 5 * 1024 * 1024, 256 * 1024, 4096,
    { 8192, Ubiquitty::Image::terminalImageBytesBudgetDefault }, { 4 * 1024 * 1024, 20000, 1024, 100000 },
    { 4 * 1024 * 1024, Ubiquitty::ClusterPool::maxEntries, Ubiquitty::RunTextStore::maxEntries, 1024 * 1024 },
    { 256 * 1024, Ubiquitty::Tek::tekMaxPrimitives }, { 32 * 1024 * 1024, 28 * 1024 * 1024 }
};
constexpr Host::HostHandoverLimits limits {
    { { terminalLimits, { 1024 * 1024, 65536, 4096, 4096, 4096 },
          72 * 1024 * 1024, 5 * 1024 * 1024, 1024 * 1024, 4 * 1024 * 1024, 32 * 1024 * 1024, 64 },
        { 4 * 1024 * 1024, 4096, 65536, 2 * 1024 * 1024 },
        { 4096, 64 }, { 1024 * 1024, 65536, 4096, 4 * 1024 * 1024 }, 80 * 1024 * 1024 },
    { 8 * 1024 * 1024,
        { 2 * 1024 * 1024, 64, 128, 256, 4096, 4096, 65536 },
        { 2 * 1024 * 1024, 16, 32, 64, 128, 32, 65536, 65536 },
        { .maxSectionBytes = 1024 * 1024, .maxGrants = 64, .maxConsentEntries = 64, .maxQUICTokens = 64, .maxStandingAgents = 64, .maxStringBytes = 65536 },
        { 2 * 1024 * 1024, 4096, 64, 64, 65536, { 1024 * 1024, 65536, 4096, 65536 } } },
    8, 168 * 1024 * 1024
};

struct Fixture {
    Ubiquitty::Test::TempDir directory;
    Host::Daemon holder;
    Host::Daemon successor;
    std::string socket;
    std::array<std::shared_ptr<StreamObservation>, 2> inherited { std::make_shared<StreamObservation>(), std::make_shared<StreamObservation>() };

    Fixture()
    {
        REQUIRE(directory.valid());
        socket = directory.path() + "/host-predecessor.sock";
        Access::configure(holder, directory.path(), socket);
        Access::configure(successor, directory.path(), directory.path() + "/host-successor.sock");
        for (unsigned index = 0; index < 2; ++index) {
            auto stream = std::make_unique<ObservedStream>(std::make_shared<StreamObservation>());
            auto& session = Screen::makeSession(holder, 24, 80, std::move(stream));
            session.coherence = Ubiquitty::Runtime::FrameCoherence({ .isPaced = false });
            session.workspaceName = "workspace-" + std::to_string(index);
            Access::place(holder, session);
            UbiquittyTest::feed(*session.term, "carried pane " + std::to_string(index));
            session.configuration.fontSize = 17 + index;
            Screen::publish(holder, session, Ubiquitty::Runtime::FrameClass::Structural);
        }
    }
    std::expected<Host::HostHandoverSnapshot, Host::HostHandover::Error> capture()
    {
        const bool isFrozen = Access::freeze(holder);
        CHECK(isFrozen);
        if (!isFrozen)
            return std::unexpected(Host::HostHandover::Error::NotAtGround);
        return Host::HostHandover::capture(holder, Screen::now(), limits);
    }
    std::vector<std::unique_ptr<PAL::Stream>> streams()
    {
        std::vector<std::unique_ptr<PAL::Stream>> result;
        for (const auto& observation : inherited)
            result.push_back(std::make_unique<ObservedStream>(observation));
        return result;
    }
    auto prepare(Host::HostHandoverSnapshot&& snapshot, const Host::HostHandoverLimits& selected = limits)
    {
        return Host::HostHandover::prepare(successor, std::move(snapshot), streams(), Access::instance(holder), Screen::now(),
            static_cast<std::uint64_t>(::time(nullptr)), socket, selected);
    }
    void checkReleasedWithoutService() const
    {
        for (const auto& observation : inherited) {
            CHECK_EQ(observation->destructions, 1u);
            CHECK_EQ(observation->terminations, 0u);
            CHECK_EQ(observation->reads, 0u);
            CHECK_EQ(observation->writes, 0u);
            CHECK_EQ(observation->drains, 0u);
            CHECK_EQ(observation->processQueries, 0u);
        }
        CHECK(Access::isUnpublished(successor));
    }
};
} // namespace

TEST(allActualSessionsAndTablesStayPrivateUntilCommit)
{
    Fixture fixture;
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    const auto firstPane = snapshot->sessions.front().paneID;
    auto candidate = fixture.prepare(std::move(*snapshot));
    REQUIRE(candidate);
    CHECK(Access::isUnpublished(fixture.successor));
    REQUIRE((*candidate)->sessionCount() == 2);
    const auto& staged = Access::stagedSessions(**candidate);
    CHECK(staged.front()->screenSource->paneID() == firstPane);
    CHECK(staged.front()->configuration.fontSize == 17);
    CHECK(staged.back()->configuration.fontSize == 18);
    CHECK(!staged.front()->ownsLandedPastePaths);
    CHECK(!staged.back()->ownsLandedPastePaths);
    const std::string_view expected = "carried pane 0";
    for (std::size_t column = 0; column < expected.size(); ++column)
        CHECK(UbiquittyTest::cell(*staged.front()->term, 0, static_cast<int>(column))->codePoint == static_cast<unsigned char>(expected[column]));
    Access::commit(std::move(**candidate));
    CHECK(Access::sessions(fixture.successor).size() == 2);
    for (const auto& session : Access::sessions(fixture.successor))
        CHECK(session->ownsLandedPastePaths);
    CHECK(Access::sessions(fixture.holder).size() == 2);
    for (const auto& observation : fixture.inherited) {
        CHECK(observation->policy == PAL::ProcessClosePolicy::Terminate);
        CHECK_EQ(observation->reads, 0u);
        CHECK_EQ(observation->writes, 0u);
        CHECK_EQ(observation->drains, 0u);
        CHECK_EQ(observation->processQueries, 0u);
    }
}

TEST(abandoningTheCompleteCandidatePublishesNothingAndKillsNoChild)
{
    Fixture fixture;
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    {
        auto candidate = fixture.prepare(std::move(*snapshot));
        REQUIRE(candidate);
    }
    fixture.checkReleasedWithoutService();
    CHECK(Access::sessions(fixture.holder).size() == 2);
}

TEST(aMalformedLastPaneAbandonsEveryPreviouslyRestoredOwner)
{
    Fixture fixture;
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    snapshot->sessions.back().record.data[10] ^= 1;
    const auto candidate = fixture.prepare(std::move(*snapshot));
    CHECK(!candidate);
    fixture.checkReleasedWithoutService();
}

TEST(aClosedPaneWhoseHostLeftTheWindowDoesNotRefuseTheHandover)
{
    Fixture fixture;
    // The host that minted the pane is neither carried nor the host of a pane
    // handed over: the pane closed after its host left the resume window.
    Access::retainClosedPane(fixture.holder, Sync::ID128 { 1, Access::issuer(fixture.holder) ^ 1 });
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    const auto candidate = fixture.prepare(std::move(*snapshot));
    CHECK(candidate.has_value());
    if (!candidate)
        std::fprintf(stderr, "host_handover_candidate: refused: %s\n", candidate.error().check.c_str());
}

TEST(individuallyValidSessionsMustBelongToTheirCarriedWorkspace)
{
    Fixture fixture;
    auto& source = Access::sessions(fixture.holder);
    source.back()->placement = source.front()->placement;
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    const auto candidate = fixture.prepare(std::move(*snapshot));
    CHECK(!candidate);
    fixture.checkReleasedWithoutService();
}

TEST(aggregateAndIdentityRefusalsProtectEvenUnvisitedEndpoints)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        Fixture fixture;
        auto snapshot = fixture.capture();
        REQUIRE(snapshot);
        auto selected = limits;
        if (kind == 0)
            selected.maxAggregateBytes = snapshot->tables.size;
        else if (kind == 1)
            snapshot->sessions.back().paneID = snapshot->sessions.front().paneID;
        else if (kind == 2)
            snapshot->predecessor.high ^= 1;
        else
            selected.maxSessions = 1;
        const auto candidate = fixture.prepare(std::move(*snapshot), selected);
        CHECK(!candidate);
        fixture.checkReleasedWithoutService();
    }
}

TEST(captureRequiresTheCurrentWholeHostFreeze)
{
    Fixture fixture;
    CHECK(!Host::HostHandover::capture(fixture.holder, Screen::now(), limits));
    REQUIRE(Access::freeze(fixture.holder));
    REQUIRE(Host::HostHandover::capture(fixture.holder, Screen::now(), limits));
    Access::thaw(fixture.holder);
    CHECK(!Host::HostHandover::capture(fixture.holder, Screen::now(), limits));
}

TEST(aDescriptorCountMismatchStillReleasesEveryInheritedChildSafely)
{
    Fixture fixture;
    auto snapshot = fixture.capture();
    REQUIRE(snapshot);
    snapshot->sessions.pop_back();
    const auto candidate = fixture.prepare(std::move(*snapshot));
    CHECK(!candidate);
    fixture.checkReleasedWithoutService();
}

UBIQUITTY_TEST_MAIN()
