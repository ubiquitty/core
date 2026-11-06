// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Platform/StreamDouble.h"
#include "ScreenPlaneFixture.h"
#include "WorkspaceStructureFixture.h"

// test_attach_over_carrier.cpp — the attach door an embedder supplies the carrier through.
//
// `attachRemote` obtains carriers by SPAWNING: its QUIC arm runs the bootstrap command as a
// subprocess to learn the daemon's port, token and certificate pin, and its fallback spawns the
// transport command through SubprocessChannel. A frontend that cannot spawn a process — iOS — can
// use neither, while the carriers themselves are portable: a QUICChannel dials directly and forks
// nothing. `attachRemoteOverCarrier` is the door for that frontend.
//
// What these cases hold to:
//
//   The embedder's factory is the one that gets called, and it is called for the FIRST attempt as
//   well as for the reconnects after it. A seam that only consulted it once would work in a demo
//   and strand a phone the moment its network moved.
//
//   `attachRemote` keeps its own spawning factory. It is the path macOS ships, and the two doors
//   share every line except which factory the core is handed — a session that asked an
//   embedder's factory for carriers would break every existing caller.
//
//   Neither door attaches twice, and a null factory is refused rather than dereferenced.
//
// The cases below them are about what an attached session does with the host it reached: a clear,
// a tab close, a theme, and the grid. The host owns the model behind all four, so each asks the
// same question from a different side — what crosses the wire, and what the mirror leaves to the
// host rather than doing on its own.
//
// Exit 0 on success.

#include "Core/Image/TerminalImage.h"
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Runtime/TmuxControlHost.h"
#include "Core/Runtime/WorkspacePresentation.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/ScriptedListingHost.h"
#include "Core/Session/WalkedProcessFacts.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/Themes.h"
#include "Core/Title/Directory.h"
#include "Core/Title/Format.h"
#include "Core/Title/ProcessIcons.h"
#include "Core/Transport/Channel.h"
#include "PAL/Stream.h"
#include "Platform/Linux/PaneViewport.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Ubiquitty::Runtime::TerminalSession;

// A carrier that connects to nothing. The session under test never gets a Welcome, which is
// fine: what is being asserted is which factory the core asks, not what comes back.
class StubChannel final : public Ubiquitty::Transport::Channel {
public:
    /// `liveSlot` is the factory's `lastChannel`. The core owns the channel and destroys it on a
    /// drop, so the channel has to say when it dies: otherwise the slot names freed memory the
    /// moment a reconnect fails to produce a replacement, and a case that should fail cleanly
    /// takes a signal instead.
    explicit StubChannel(StubChannel** liveSlot)
        : m_liveSlot(liveSlot)
    {
    }

    ~StubChannel() override
    {
        // Only if the slot still names this channel — a later one may have taken it already, and
        // clearing that would hide the live one.
        if (m_liveSlot && *m_liveSlot == this)
            *m_liveSlot = nullptr;
    }

    bool start(Ubiquitty::Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }

    /// Drops the carrier the way a network going away does.
    void dropIt(const std::string& reason)
    {
        if (m_observer)
            m_observer->onClose(reason);
    }
    void send(std::span<const std::uint8_t> bytes) override
    {
        sent.emplace_back(bytes.begin(), bytes.end());
        if (onSend)
            onSend();
    }
    std::function<void()> onSend;
    void connect() { m_observer->onConnect(); }
    void receive(const std::vector<std::uint8_t>& bytes) { m_observer->onData(bytes); }
    std::vector<std::vector<std::uint8_t>> sent;
    void stop() override { }
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const override
    {
        return Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, "" };
    }
    [[nodiscard]] Ubiquitty::Transport::Health health() const override
    {
        return Ubiquitty::Transport::Health::Connecting;
    }
    [[nodiscard]] std::string description() const override { return "stub"; }

private:
    StubChannel** m_liveSlot = nullptr;
    Ubiquitty::Transport::ChannelObserver* m_observer = nullptr;
};

// Counts what the core asked it for. Standing in for the iOS factory, which would build a
// QUICChannel from a bootstrap fetched over its own in-process SSH client.
class CountingFactory final : public Ubiquitty::Session::ChannelFactory {
public:
    void createChannel(Ubiquitty::Session::TransportMode, Done done) override
    {
        if (!createCount)
            pinAtFirstCreate = pinnedSocket;
        ++createCount;
        auto channel = std::make_unique<StubChannel>(&lastChannel);
        lastChannel = channel.get();
        done(std::move(channel), false);
    }

    bool pinDaemonSocket(std::string_view socketName) override
    {
        ++pinCount;
        if (!acceptsPins || (!pinnedSocket.empty() && pinnedSocket != socketName))
            return false;
        pinnedSocket = socketName;
        return true;
    }

    void clearHandoverReach() override
    {
        ++handoverClearCount;
        pinnedSocket.clear();
    }

    int createCount = 0;
    int pinCount = 0;
    int handoverClearCount = 0;
    bool acceptsPins = true;
    std::string pinnedSocket;
    std::string pinAtFirstCreate;
    StubChannel* lastChannel = nullptr; // the carrier this factory handed over most recently
};

// The session's platform loop. It queues the tokens rather than running them: these cases assert
// which factory is asked, not what a reconnect does. A queued token is still owed a redemption —
// one merely dropped leaves its unit pending in Core's table for the rest of the run. Both entry
// points must exist: a failing attach schedules a reconnect through the delayed one, and a session
// refuses a loop that lacks either.
// The queue a posted token lands in, drained by `pumpPostedWork`. A global because the loop is a
// pair of plain function pointers with no context.
std::vector<Ubiquitty::Runtime::PostedWorkToken>& postedWork()
{
    static std::vector<Ubiquitty::Runtime::PostedWorkToken> queue;
    return queue;
}

void queuePostedWork(Ubiquitty::Runtime::PostedWorkToken token) { postedWork().push_back(token); }

void queueDelayedWork(double, Ubiquitty::Runtime::PostedWorkToken token)
{
    // The delay is dropped rather than honoured: these cases assert what the reconnect does, and
    // sleeping out a backoff would measure the machine instead.
    postedWork().push_back(token);
}

Ubiquitty::Runtime::PostedWorkLoop queueingLoop() { return { .runSoon = &queuePostedWork, .runAfter = &queueDelayedWork }; }

/// Runs what the session posted, including anything those runs post in turn.
///
/// Bounded rather than "until empty": a reconnect that never settles would otherwise hang the
/// suite instead of failing it.
void pumpPostedWork(int rounds = 8)
{
    for (int round = 0; round < rounds && !postedWork().empty(); ++round) {
        std::vector<Ubiquitty::Runtime::PostedWorkToken> batch;
        batch.swap(postedWork());
        for (const Ubiquitty::Runtime::PostedWorkToken token : batch)
            Ubiquitty::Runtime::runPostedSessionWork(token);
    }
}

/// Disposes whatever is queued without running it, so one case cannot leak into the next.
///
/// Destroyed rather than run: a token left over from a finished case names a session that case has
/// already released, so running it would touch freed state. Destroying it is what the far side of
/// the seam owes for a token it will not run.
void discardPostedWork()
{
    for (const Ubiquitty::Runtime::PostedWorkToken token : postedWork())
        Ubiquitty::Runtime::discardPostedSessionWork(token);
    postedWork().clear();
}

// A transport command that spawns nothing.
//
// `attachRemote`'s fallback builds a SubprocessChannel from whatever it is given, and a real
// command would run here: "ssh host --attach" starts an ssh whose fatal signal takes the test
// process down with it. That `attachRemote` spawns at all is the reason `attachRemoteOverCarrier`
// exists.
constexpr const char* spawnsNothing = "";

/// Makes a session and starts it, or answers null having already failed the case.
///
/// CHECK is not an assert — it records a failure and walks on — so a case that checks
/// `session != nullptr` and then calls `session->start(...)` dereferences null when `make()`
/// returns nothing, and takes a SIGNAL. A signal ends the binary, not the case: the other cases
/// then report no verdict at all, which reads as a suite that stopped rather than a case that
/// failed, and an ablation of the factory selection cannot say which cases caught it.
///
/// A helper rather than a guard in each case, so a new case cannot reintroduce the dereference
/// by being written the obvious way.
///
/// - Returns: A started session, or null — in which case the case has already been failed and
///   the caller should return.
[[nodiscard]] TerminalSession* makeStartedSession()
{
    TerminalSession* session = TerminalSession::make();
    CHECK_MSG(session != nullptr, "TerminalSession::make() returned nothing");
    if (session == nullptr)
        return nullptr;
    session->start(24, 80);
    return session;
}

/// Welcomes only the explicit held workspace, without binding a pane before
/// that workspace's structure has reached the client.
///
/// - Parameters:
///   - factory: The factory holding the live carrier.
///   - host: The host's end of the session's screen plane.
///   - socketName: The exact fixture-owned basename selected by this host.
void acknowledgeFixtureHost(CountingFactory& factory, UbiquittyTest::HostScreen& host, std::string socketName = "fixture.sock")
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto& connection = host.plane().connection();
    REQUIRE(connection.state() == Sync::ConnectionState::Greeting);
    connection.setGreetingAdmission([socketName = std::move(socketName)](const Sync::Greet& greeting) -> std::expected<Sync::HostIdentity, Sync::Bye> {
        const auto& request = greeting.request;
        if (request.target != Sync::AdmissionTarget::Workspace
            || (request.sessionID && request.sessionID != 7)
            || (request.workspaceID && request.workspaceID != 91)
            || (request.expectedDaemonInstanceID && request.expectedDaemonInstanceID != 17))
            return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone, .detail = "the fixture holds only session 7 in workspace 91 on daemon 17" });
        Sync::HostIdentity identity;
        identity.selection = { .target = Sync::AdmissionTarget::Workspace,
            .sessionID = 7,
            .workspaceID = 91,
            .daemonInstanceID = 17,
            .socketName = socketName,
            .buildFlags = 0,
            .hostPlatform = "fixture" };
        identity.mayDrive = true;
        identity.maxRole = Sync::Role::Owner;
        identity.verifiedKind = Sync::ClientKind::Human;
        return identity;
    });
    const auto writes = factory.lastChannel->sent;
    for (const auto& bytes : writes)
        connection.receiveStream(bytes, host.plane().now());
    host.passOver(writes.size());
    REQUIRE(connection.state() == Sync::ConnectionState::Attached);
    const auto reply = host.flush();
    Sync::FrameDecoder decoder;
    decoder.append(reply);
    std::size_t welcomes = 0;
    while (const auto frame = decoder.next()) {
        welcomes += frame->kind == Sync::FrameKind::Welcome;
        CHECK(frame->kind != Sync::FrameKind::PaneBind);
    }
    CHECK_EQ(welcomes, 1u);
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    factory.lastChannel->receive(reply);
    pumpPostedWork();
}

/// Sends the one-pane workspace before binding its screen handle.
void receiveWorkspaceStructure(CountingFactory& factory, UbiquittyTest::HostScreen& host, std::uint64_t revision = 1, const std::string& tabName = {})
{
    if (host.plane().connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Greeting)
        acknowledgeFixtureHost(factory, host);
    const UbiquittyTest::StructurePane only { .pane = 1, .sessionID = 7 };
    Ubiquitty::Session::TreeWorkspace workspace = UbiquittyTest::rowWorkspace(91, {}, { &only, 1 }, 17, revision);
    workspace.windows[0].tabs[0].name = tabName;
    CHECK(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
}

/// Attaches a started session over a stub carrier and lets the host answer.
///
/// - Parameters:
///   - session: The session to attach.
///   - factory: The carrier factory it attaches over.
///   - host: The actual host that will later publish the fixture's pane.
/// - Returns: Whether the attach reached an acknowledged host.
[[nodiscard]] bool attachAndAcknowledge(TerminalSession* session, CountingFactory& factory,
    UbiquittyTest::HostScreen& host)
{
    CHECK_MSG(session->attachRemoteOverCarrier(
                  reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
                  0, queueingLoop()),
        "the attach did not start");
    CHECK_MSG(factory.lastChannel != nullptr, "the attach produced no carrier");
    if (factory.lastChannel == nullptr)
        return false;
    factory.lastChannel->connect();
    pumpPostedWork();
    acknowledgeFixtureHost(factory, host);
    return factory.lastChannel != nullptr;
}

/// The newest `Raw` event among what a carrier was written: its pane handle
/// and its bytes.
///
/// - Parameter sent: Every write, each whole Sync frames.
/// - Returns: The event, or `nullopt` when none was written.
std::optional<std::pair<std::uint32_t, std::string>> lastRawOnCarrier(const std::vector<std::vector<std::uint8_t>>& sent)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    std::optional<std::pair<std::uint32_t, std::string>> last;
    Sync::FrameDecoder envelopes;
    for (const std::vector<std::uint8_t>& write : sent) {
        envelopes.append(write);
        while (const auto envelope = envelopes.next()) {
            if (envelope->kind != Sync::FrameKind::Raw)
                continue;
            if (const auto raw = Sync::decodeRaw(envelope->body))
                last = std::pair { envelope->channel, std::string(raw->bytes.begin(), raw->bytes.end()) };
        }
    }
    CHECK(envelopes.error() == Sync::EnvelopeError::None && envelopes.bufferedBytes() == 0);
    return last;
}

/// Sends the client the host's screen at one grid.
///
/// A real screen plane over a real Terminal rather than a hand-built body: the
/// grid a mirror ends up holding is the one the plane put on the wire. What
/// the client wrote since the last frame — its greeting, its acknowledgements,
/// a resync — reaches the host first, as it would over the carrier.
///
/// - Parameters:
///   - factory: The factory holding the live carrier.
///   - host: The host's terminal and screen plane.
///   - rows: The host grid's rows.
///   - columns: Its columns.
void receiveHostScreen(CountingFactory& factory, UbiquittyTest::HostScreen& host, int rows, int columns)
{
    Ubiquitty::resize(host.terminal(), rows, columns);
    // Twice: a frame the client refused for its local grid is answered with a
    // resync, and the checkpoint that answers it is the frame it applies.
    for (int round = 0; round < 2; ++round) {
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.publish());
        pumpPostedWork();
    }
}

struct ProjectionSessionRelease {
    void operator()(TerminalSession* session) const { releaseTerminalSession(session); }
};

/// Owns only passive terminals, a fake carrier, and the in-memory host plane.
/// The anchor is released before queued callbacks or their factory disappear.
struct WorkspacePresentationFixture {
    CountingFactory factory;
    UbiquittyTest::HostScreen host { 7, 17 };
    std::unique_ptr<TerminalSession, ProjectionSessionRelease> anchor { makeStartedSession() };
    bool isReady = anchor && attachAndAcknowledge(anchor.get(), factory, host);

    ~WorkspacePresentationFixture()
    {
        anchor.reset();
        discardPostedWork();
    }

    bool offer(const Ubiquitty::Session::TreeWorkspace& workspace)
    {
        if (!isReady || !factory.lastChannel)
            return false;
        if (!UbiquittyTest::offerStructure(host.plane().connection(), workspace))
            return false;
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        return true;
    }
};

struct PassiveProjectionSessions {
    unsigned made = 0;
    bool shouldRefuse = false;
    bool sawAStream = false;

    static TerminalSession* make(void* context)
    {
        auto& state = *static_cast<PassiveProjectionSessions*>(context);
        if (state.shouldRefuse)
            return nullptr;
        auto* session = makeStartedSession();
        if (session) {
            ++state.made;
            state.sawAStream = state.sawAStream || session->stream() || session->fileDescriptor() != -1;
        }
        return session;
    }
};

/// Two authoritative windows, three ordered tabs, and six pane identities.
/// The first tab has nested splits along both axes with unequal ratios.
Ubiquitty::Session::TreeWorkspace projectionWorkspace(std::uint64_t revision = 1)
{
    namespace Layout = Ubiquitty::Session::Layout;
    const UbiquittyTest::StructurePane first[] = { { 1, 7 }, { 2, 8 }, { 3, 9 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "workshop", first, 17, revision, 1);
    auto& firstWindow = workspace.windows.front();
    firstWindow.isHostActive = false;
    firstWindow.tabs.front().name = "editor";
    firstWindow.tabs.front().isHostActive = false;
    firstWindow.tabs.front().split.ratio = 0.35;
    firstWindow.tabs.front().split.children[1].axis = Ubiquitty::Protocol::Sync::SplitAxis::Vertical;
    firstWindow.tabs.front().split.children[1].ratio = 0.6;
    Ubiquitty::Session::SessionTreeDetail::sizeSplit(firstWindow.tabs.front().split.children[1]);
    Ubiquitty::Session::SessionTreeDetail::sizeSplit(firstWindow.tabs.front().split);
    Ubiquitty::Session::SessionTreeDetail::placeNode(firstWindow.tabs.front().split, 0, 0);
    const UbiquittyTest::StructurePane fourth { 4, 10 };
    auto secondTab = UbiquittyTest::rowWorkspace(91, {}, { &fourth, 1 }, 17, revision).windows.front().tabs.front();
    secondTab.id = Layout::TabID { 2 };
    secondTab.name = "build";
    firstWindow.tabs.push_back(std::move(secondTab));
    firstWindow.hostActiveTab = 1;
    const UbiquittyTest::StructurePane last[] = { { 5, 11 }, { 6, 12 } };
    auto secondWindow = UbiquittyTest::rowWorkspace(91, {}, last, 17, revision, 1).windows.front();
    secondWindow.id = Layout::SurfaceID { 2 };
    secondWindow.tabs.front().id = Layout::TabID { 3 };
    secondWindow.tabs.front().name = "logs";
    secondWindow.hostZoomedPane = Ubiquitty::Session::TreeZoom { 0, 1 };
    workspace.windows.push_back(std::move(secondWindow));
    workspace.hostActiveWindow = 1;
    return workspace;
}

} // namespace

TEST(aWorkspacePresentationKeepsEveryHostWindowTabAndNestedSplit)
{
    namespace Layout = Ubiquitty::Session::Layout;
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Unavailable);
    REQUIRE(fixture.offer(projectionWorkspace()));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    const auto windows = presentation.windows();
    REQUIRE(windows.size() == 2);
    CHECK(presentation.activeWindow() == Layout::SurfaceID { 2 });
    CHECK(windows[0].id == Layout::SurfaceID { 1 });
    REQUIRE(windows[0].tabs.size() == 2);
    CHECK(windows[0].tabs[0].id == Layout::TabID { 1 });
    CHECK(windows[0].tabs[0].title == "editor");
    CHECK(windows[0].tabs[1].id == Layout::TabID { 2 });
    CHECK(windows[0].activeTab == Layout::TabID { 2 });
    const auto& tree = windows[0].tabs[0].panes;
    const auto root = tree.node(tree.root());
    CHECK(root.orientation == Ubiquitty::Panes::Orientation::Vertical);
    CHECK(root.ratio == 0.35f);
    const auto nested = tree.node(root.second);
    CHECK(nested.orientation == Ubiquitty::Panes::Orientation::Horizontal);
    CHECK(nested.ratio == 0.6f);
    CHECK(tree.focusedPaneID() == 2);
    CHECK(windows[1].zoomedPane == Layout::PaneID { 6 });
    REQUIRE(windows[1].tabs.size() == 1);
    CHECK(windows[1].tabs[0].panes.focusedPaneID() == 6);
    for (std::uint32_t pane = 1; pane <= 6; ++pane) {
        auto* session = presentation.session(Layout::PaneID { pane });
        REQUIRE(session);
        CHECK(session->stream() == nullptr);
        CHECK(session->fileDescriptor() == -1);
        CHECK(session->sharesRemoteWorkspaceConnection(*fixture.anchor));
        CHECK(session->remoteSessionID() == pane + 6);
    }
    CHECK(!made.sawAStream);
    CHECK(made.made == 5);
    CHECK(fixture.factory.createCount == 1);
    CHECK(presentation.refresh() == Projection::RefreshOutcome::Unchanged);
    CHECK(made.made == 5);
    CHECK(fixture.factory.createCount == 1);
}

TEST(aWorkspacePresentationMovesAndReordersWithoutReplacingItsPanes)
{
    namespace Layout = Ubiquitty::Session::Layout;
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    auto workspace = projectionWorkspace();
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    auto* pane = presentation.session(Layout::PaneID { 2 });
    REQUIRE(pane);
    Ubiquitty::Terminal* terminal = nullptr;
    {
        const std::scoped_lock hold(*pane->modelLockPointer());
        terminal = pane->paneView().terminal;
    }
    auto moved = std::move(workspace.windows[0].tabs[0]);
    workspace.windows[0].tabs.erase(workspace.windows[0].tabs.begin());
    workspace.windows[0].hostActiveTab = 0;
    workspace.windows[0].tabs[0].isHostActive = true;
    workspace.windows[1].tabs[0].isHostActive = false;
    moved.isHostActive = true;
    workspace.windows[1].tabs.insert(workspace.windows[1].tabs.begin(), std::move(moved));
    workspace.windows[1].hostActiveTab = 0;
    workspace.windows[1].hostZoomedPane.reset();
    workspace.revision = 2;
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    REQUIRE(presentation.windows().size() == 2);
    REQUIRE(presentation.windows()[1].tabs.size() == 2);
    CHECK(presentation.windows()[1].tabs[0].id == Layout::TabID { 1 });
    CHECK(presentation.windows()[1].tabs[1].id == Layout::TabID { 3 });
    CHECK(presentation.session(Layout::PaneID { 2 }) == pane);
    {
        const std::scoped_lock hold(*pane->modelLockPointer());
        CHECK(pane->paneView().terminal == terminal);
    }
    CHECK(made.made == 5);
    CHECK(fixture.factory.createCount == 1);
}

TEST(aWorkspacePresentationKeepsItsLastCompletePlanWhenANewPaneCannotBind)
{
    namespace Layout = Ubiquitty::Session::Layout;
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    const UbiquittyTest::StructurePane root { 1, 7 };
    REQUIRE(fixture.offer(UbiquittyTest::rowWorkspace(91, {}, { &root, 1 }, 17)));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    auto* first = presentation.session(Layout::PaneID { 1 });
    REQUIRE(first);
    made.shouldRefuse = true;
    REQUIRE(fixture.offer(projectionWorkspace(2)));
    CHECK(presentation.refresh() == Projection::RefreshOutcome::Refused);
    REQUIRE(presentation.windows().size() == 1);
    CHECK(presentation.session(Layout::PaneID { 1 }) == first);
    CHECK(presentation.session(Layout::PaneID { 2 }) == nullptr);
    CHECK(fixture.factory.createCount == 1);
    made.shouldRefuse = false;
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    CHECK(presentation.windows().size() == 2);
    CHECK(presentation.session(Layout::PaneID { 1 }) == first);
    CHECK(made.made == 5);
    CHECK(!made.sawAStream);
}

TEST(aWorkspacePresentationPromotesASurvivorBeforeItsAnchorLeavesThePlan)
{
    namespace Layout = Ubiquitty::Session::Layout;
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    auto workspace = projectionWorkspace();
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    auto* survivor = presentation.session(Layout::PaneID { 5 });
    REQUIRE(survivor);
    workspace.windows.erase(workspace.windows.begin());
    workspace.hostActiveWindow = 0;
    workspace.revision = 2;
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    CHECK(presentation.session(Layout::PaneID { 1 }) == nullptr);
    CHECK(presentation.session(Layout::PaneID { 5 }) == survivor);
    const UbiquittyTest::StructurePane added { 7, 13 };
    auto tab = UbiquittyTest::rowWorkspace(91, {}, { &added, 1 }, 17).windows.front().tabs.front();
    tab.id = Layout::TabID { 4 };
    tab.isHostActive = false;
    workspace.windows[0].tabs.push_back(std::move(tab));
    workspace.revision = 3;
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    auto* joined = presentation.session(Layout::PaneID { 7 });
    REQUIRE(joined);
    CHECK(joined->remoteSessionID() == 13);
    CHECK(joined->sharesRemoteWorkspaceConnection(*survivor));
    CHECK(joined->stream() == nullptr);
    CHECK(joined->fileDescriptor() == -1);
    CHECK(made.made == 6);
    CHECK(fixture.factory.createCount == 1);
}

TEST(aWorkspacePresentationDoesNotTurnAnUnboundLocalRootIntoAWindow)
{
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    auto* model = Ubiquitty::Runtime::WorkspaceSession::make(
        Ubiquitty::Runtime::WorkspaceSession::IngestPolicy::CallerDrained, Ubiquitty::Runtime::DrawnText::SizedAndProportional);
    REQUIRE(model);
    {
        Projection presentation(*model);
        CHECK(presentation.refresh() == Projection::RefreshOutcome::Unchanged);
        CHECK(presentation.windows().empty());
        CHECK(presentation.activeWindow() == Ubiquitty::Session::Layout::invalidSurface);
        CHECK(model->surfaceCount() == 1);
        CHECK(model->surfaceAt(0) != Ubiquitty::Session::Layout::invalidSurface);
        CHECK(model->tabTitle(model->surfaceAt(0), model->activeTab(model->surfaceAt(0))).empty());
    }
    releaseWorkspaceSession(model);
}

TEST(aWorkspacePresentationPublishesHostLabelsWithoutReattachingItsPanes)
{
    namespace Layout = Ubiquitty::Session::Layout;
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    auto workspace = projectionWorkspace();
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    auto* pane = presentation.session(Layout::PaneID { 2 });
    REQUIRE(pane);
    const auto generation = fixture.anchor->remoteWorkspaceMirrorGeneration();
    (void)fixture.anchor->takeEventFlags();

    workspace.revision = 2;
    workspace.label = "release";
    workspace.windows[0].label = "Editor\nwork";
    workspace.windows[0].tabs[0].label = "source:1";
    REQUIRE(fixture.offer(workspace));
    CHECK(fixture.anchor->takeEventFlags() & (1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::WorkspaceChanged)));
    CHECK(fixture.anchor->remoteWorkspaceMirrorGeneration() > generation);
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    CHECK(presentation.name() == "workshop");
    CHECK(presentation.label() == "release");
    REQUIRE(presentation.windows().size() == 2);
    CHECK(presentation.windows()[0].label == "Editor\nwork");
    CHECK(presentation.windows()[0].tabs[0].title == "editor");
    CHECK(presentation.windows()[0].tabs[0].label == "source:1");
    CHECK(presentation.session(Layout::PaneID { 2 }) == pane);
    CHECK(pane->remoteWorkspaceStructureSnapshot() == fixture.anchor->remoteWorkspaceStructureSnapshot());
    CHECK(made.made == 5);
    CHECK(fixture.factory.createCount == 1);

    workspace.revision = 3;
    const auto labelledGeneration = fixture.anchor->remoteWorkspaceMirrorGeneration();
    REQUIRE(fixture.offer(workspace));
    CHECK(!(fixture.anchor->takeEventFlags() & (1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::WorkspaceChanged))));
    CHECK(fixture.anchor->remoteWorkspaceMirrorGeneration() == labelledGeneration);
    const auto* snapshot = fixture.anchor->remoteWorkspaceStructureSnapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->revision == 3);
    CHECK(snapshot->windows[0].label == "Editor\nwork");
    CHECK(presentation.refresh() == Projection::RefreshOutcome::Unchanged);
    CHECK(presentation.session(Layout::PaneID { 2 }) == pane);
    CHECK(made.made == 5);
    CHECK(fixture.factory.createCount == 1);
}

TEST(aWorkspacePresentationDoesNotOfferItsPreviousOwnerSnapshotAsLiveDuringReconnect)
{
    using Projection = Ubiquitty::Runtime::WorkspacePresentation;
    WorkspacePresentationFixture fixture;
    REQUIRE(fixture.isReady);
    CHECK(!fixture.anchor->remoteWorkspaceStructureSnapshot());
    PassiveProjectionSessions made;
    Projection presentation(*fixture.anchor, &PassiveProjectionSessions::make, &made);
    auto workspace = projectionWorkspace();
    workspace.windows[0].label = "Editor";
    REQUIRE(fixture.offer(workspace));
    REQUIRE(presentation.refresh() == Projection::RefreshOutcome::Changed);
    REQUIRE(fixture.anchor->remoteWorkspaceStructureSnapshot());

    fixture.factory.lastChannel->dropIt("test carrier lost");
    pumpPostedWork();
    CHECK(!fixture.anchor->remoteWorkspaceStructureSnapshot());
    CHECK(presentation.refresh() == Projection::RefreshOutcome::Unavailable);
    REQUIRE(presentation.windows().size() == 2);
    CHECK(presentation.windows()[0].label == "Editor");
    CHECK(made.made == 5);
}

TEST(aLocalPaneLinkOwnsItsViewAndNeverStalls)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    session->lockForRead();
    const Ubiquitty::Session::PaneView view = session->paneView();
    CHECK(view.terminal);
    CHECK(!view.history);
    CHECK(!view.facts);
    session->unlockForRead();

    session->hideRemotePane();
    session->showRemotePane();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Unfocused);
    session->lockForRead();
    CHECK(session->paneView().terminal == view.terminal);
    CHECK(!session->paneView().terminal->windowFocused);
    session->unlockForRead();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);
    session->lockForRead();
    CHECK(session->paneView().terminal->windowFocused);
    session->unlockForRead();

    const auto state = session->paneLinkState(60'000'000);
    CHECK(state.phase == Ubiquitty::Session::LinkPhase::Live);
    CHECK(!state.isStalled());
    CHECK(state.transport.lastHeardMicroseconds == 60'000'000);
    CHECK(!state.transport.oldestUnreceiptedAgeMicroseconds);

    const std::array<Sync::LineRange, 1> valid { Sync::LineRange { 0, 10 } };
    const std::array<Sync::LineRange, 1> empty { Sync::LineRange { 7, 7 } };
    CHECK(session->wantPaneHistory(Sync::HistoryPriority::Viewport, valid));
    CHECK(!session->wantPaneHistory(Sync::HistoryPriority::Viewport, empty));
    CHECK(!session->wantPaneHistory(static_cast<Sync::HistoryPriority>(255), valid));
}

TEST(aLocalPanePublishesCanonicalFactsOutsideItsModelLock)
{
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    stream->facts.name = "local-job";
    stream->facts.isShell = false;
    stream->facts.group = 42;
    stream->facts.currentDirectory = "/sampled";
    stream->pushInbound("\x1b]2;local tests\x1b\\");
    auto* const input = stream.get();
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    (void)session->drain();
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        TerminalSession* session = nullptr;
        std::vector<Ubiquitty::Protocol::Sync::PaneFacts> facts;
        void factsChanged() override
        {
            REQUIRE(session->tryLockForRead());
            const auto* record = session->paneView().facts;
            CHECK(record);
            if (record)
                facts.push_back(*record);
            session->unlockForRead();
        }
    } observer;
    observer.session = session.get();
    session->observePaneLink(observer);
    (void)session->takeEventFlags();
    REQUIRE(observer.facts.size() == 1);
    CHECK_STR_EQ(observer.facts.back().programTitle, "local tests");
    CHECK_STR_EQ(observer.facts.back().currentDirectory, "/sampled");
    CHECK_STR_EQ(observer.facts.back().foreground.name, "local-job");
    CHECK_EQ(observer.facts.back().foreground.processID, 42u);
    CHECK(!observer.facts.back().isAtPrompt);
    const auto nameProbes = input->facts.nameProbes;
    const auto walkProbes = input->facts.gatherProbes;
    (void)session->takeEventFlags();
    CHECK_EQ(observer.facts.size(), 1u);
    CHECK_EQ(input->facts.nameProbes, nameProbes);
    CHECK_EQ(input->facts.gatherProbes, walkProbes);

    // Dormant suppresses pictures, not the facts a tab or listing needs.
    session->hideRemotePane();
    input->pushInbound("\x1b]9;4;1;42\x1b\\");
    (void)session->drain();
    (void)session->takeEventFlags();
    REQUIRE(observer.facts.size() == 2);
    CHECK_EQ(observer.facts.back().progress.value, 42);
    CHECK_EQ(observer.facts.back().progress.state, 1);
    CHECK(observer.facts.back().revision > observer.facts.front().revision);
    (void)session->takeEventFlags();
    CHECK_EQ(observer.facts.size(), 2u);
    Observer replacement;
    replacement.session = session.get();
    session->observePaneLink(replacement);
    (void)session->takeEventFlags();
    CHECK_EQ(replacement.facts.size(), 1u);
}

TEST(anAdoptedPanePublishesOnlyItsActualModelsFacts)
{
    Ubiquitty::Terminal external;
    Ubiquitty::resize(external, 12, 40);
    Ubiquitty::processBytes(&external, "\x1b]2;adopted tests\x1b\\\x1b]7;file://remote/owned\x1b\\");
    std::mutex externalLock;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    stream->facts.name = "unused adapter";
    stream->facts.group = 99;
    stream->pushInbound("\x1b]2;not this terminal\x1b\\");
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    (void)session->drain();
    session->adoptExternalTerminal(&external, reinterpret_cast<std::uintptr_t>(&externalLock));
    (void)session->takeEventFlags();
    session->lockForRead();
    const auto* facts = session->paneView().facts;
    CHECK(facts);
    if (facts) {
        CHECK_STR_EQ(facts->programTitle, "adopted tests");
        CHECK_STR_EQ(facts->currentDirectory, "/owned");
        CHECK_EQ(facts->rows, 12);
        CHECK_EQ(facts->columns, 40);
        CHECK(facts->foreground.name.empty());
        CHECK(facts->processChain.empty());
        CHECK_EQ(facts->foreground.processID, 0u);
        CHECK(facts->isAdopted);
    }
    session->unlockForRead();
    session->endExternalAdoption();
    (void)session->takeEventFlags();
    session->lockForRead();
    const auto* local = session->paneView().facts;
    CHECK(local);
    if (local) {
        CHECK_STR_EQ(local->programTitle, "not this terminal");
        CHECK_STR_EQ(local->foreground.name, "unused adapter");
        CHECK(!local->isAdopted);
    }
    session->unlockForRead();
}

TEST(aLocalPaneViewportStillResizesItsOwnModelAndStream)
{
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto* const observed = stream.get();
    std::string lines;
    for (unsigned index = 0; index < 40; ++index)
        lines += "local history\r\n";
    stream->pushInbound(lines);
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    (void)session->drain();
    const int before = observed->windowSizeCalls;
    session->resize(12, 40, TerminalSession::ResizeGesture::Interactive, Ubiquitty::Modes::AnchorMode::Bottom, 800, 400);
    session->lockForRead();
    const auto* const terminal = session->paneView().terminal;
    CHECK(terminal);
    if (terminal) {
        CHECK_EQ(terminal->rows, 12);
        CHECK_EQ(terminal->columns, 40);
        CHECK_EQ(terminal->pendingHistoryRewrapColumns, 40);
        CHECK(terminal->pendingHistoryRewrapAnchor == Ubiquitty::Modes::AnchorMode::Bottom);
    }
    session->unlockForRead();
    CHECK_EQ(observed->columns, 40);
    CHECK_EQ(observed->rows, 12);
    CHECK_EQ(observed->xPixels, 800);
    CHECK_EQ(observed->yPixels, 400);
    CHECK_EQ(observed->windowSizeCalls, before + 1);

    // An extent update is not another request to reflow the model.
    session->setWindowSize(41, 13, 820, 420);
    session->lockForRead();
    CHECK_EQ(session->paneView().terminal->rows, 12);
    CHECK_EQ(session->paneView().terminal->columns, 40);
    session->unlockForRead();
    CHECK_EQ(observed->columns, 41);
    CHECK_EQ(observed->rows, 13);
    CHECK_EQ(observed->xPixels, 820);
    CHECK_EQ(observed->yPixels, 420);
    CHECK_EQ(observed->windowSizeCalls, before + 2);
}

TEST(aLinuxViewportStagesPixelsBeforeItsSingleLocalResize)
{
    using namespace Ubiquitty;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto stream = std::make_unique<TestDoubles::StreamDouble>();
    auto* const observed = stream.get();
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    const int before = observed->windowSizeCalls;
    Linux::PaneViewportPublisher publisher;
    const PixelGeometry pixels { { 9, 18 }, { 360, 216 }, { 380, 236 } };
    const auto publication = publisher.publish(*session, { 40, 12 }, pixels);
    CHECK(publication == Linux::ViewportPublication::Grid);
    CHECK_EQ(observed->windowSizeCalls, before + 1);
    CHECK_EQ(observed->columns, 40);
    CHECK_EQ(observed->rows, 12);
    CHECK_EQ(observed->xPixels, 360);
    CHECK_EQ(observed->yPixels, 216);
    session->lockForRead();
    const auto* const terminal = session->paneView().terminal;
    CHECK(terminal);
    if (terminal) {
        CHECK_EQ(terminal->columns, 40);
        CHECK_EQ(terminal->rows, 12);
        CHECK_EQ(terminal->cellPixelWidth, 9);
        CHECK_EQ(terminal->cellPixelHeight, 18);
        CHECK_EQ(terminal->textAreaPixelWidth, 360);
        CHECK_EQ(terminal->drawablePixelHeight, 236);
    }
    session->unlockForRead();
    CHECK(publisher.publish(*session, { 40, 12 }, pixels) == Linux::ViewportPublication::Unchanged);
    CHECK_EQ(observed->windowSizeCalls, before + 1);
    CHECK(publisher.publish(*session, {}, pixels) == Linux::ViewportPublication::Unchanged);
    CHECK_EQ(observed->windowSizeCalls, before + 1);
    session->lockForRead();
    processBytes(session->paneView().terminal, "\x1b[?7402h");
    const auto fullExtent = reportedTextAreaPixels(*session->paneView().terminal, pixels);
    CHECK((fullExtent == IntSize { 380, 236 }));
    CHECK_EQ(session->paneView().terminal->textAreaPixelWidth, 360);
    session->unlockForRead();
    CHECK(publisher.publish(*session, { 40, 12 }, pixels) == Linux::ViewportPublication::Pixels);
    CHECK(publisher.publish(*session, { 40, 12 }, pixels) == Linux::ViewportPublication::Unchanged);
}

TEST(aLinuxRemoteViewportKeepsTheHostPictureAndSendsOneCompleteAsk)
{
    using namespace Ubiquitty;
    namespace Sync = Protocol::Sync;
    discardPostedWork();
    CountingFactory factory;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 24, 80);
    (void)host.plane().connection().takeViewportReports();
    session->lockForRead();
    const auto oldEpoch = session->paneView().terminal->absoluteRowEpoch;
    const auto oldCellWidth = session->paneView().terminal->cellPixelWidth;
    const auto oldTextAreaWidth = session->paneView().terminal->textAreaPixelWidth;
    session->unlockForRead();
    unsigned sends = 0;
    bool isUnlockedDuringSend = true;
    factory.lastChannel->onSend = [&] {
        ++sends;
        const bool acquired = session->tryLockForRead();
        isUnlockedDuringSend &= acquired;
        if (acquired)
            session->unlockForRead();
    };
    Linux::PaneViewportPublisher publisher;
    CHECK(publisher.publish(*session, { 96, 31 }, { { 17, 29 }, { 1632, 899 }, { 1644, 911 } }) == Linux::ViewportPublication::Grid);
    factory.lastChannel->onSend = {};
    CHECK(sends > 0);
    CHECK(isUnlockedDuringSend);
    host.receive(factory.lastChannel->sent);
    const auto reports = host.plane().connection().takeViewportReports();
    REQUIRE(reports.size() == 1);
    CHECK_EQ(reports.front().handle, host.plane().handle());
    CHECK((reports.front().viewport == Sync::Viewport { .rows = 31, .columns = 96, .cellPixelWidth = 17, .cellPixelHeight = 29, .textAreaPixelWidth = 1632, .textAreaPixelHeight = 899, .drawablePixelWidth = 1644, .drawablePixelHeight = 911 }));
    session->lockForRead();
    CHECK_EQ(session->paneView().terminal->columns, 80);
    CHECK_EQ(session->paneView().terminal->rows, 24);
    CHECK_EQ(session->paneView().terminal->absoluteRowEpoch, oldEpoch);
    CHECK_EQ(session->paneView().terminal->cellPixelWidth, oldCellWidth);
    CHECK_EQ(session->paneView().terminal->textAreaPixelWidth, oldTextAreaWidth);
    session->unlockForRead();
    CHECK(publisher.publish(*session, { 96, 31 }, { { 17, 29 }, { 1632, 899 }, { 1644, 911 } }) == Linux::ViewportPublication::Unchanged);
    host.receive(factory.lastChannel->sent);
    CHECK(host.plane().connection().takeViewportReports().empty());
    session.reset();
    discardPostedWork();
}

TEST(nativePixelMetricsAreKeptAcrossAttachmentAndNeverBorrowedFromTheReplica)
{
    using namespace Ubiquitty;
    discardPostedWork();
    CountingFactory factory;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    session->publishCellPixelSize(9, 18, 720, 432, 740, 450);
    // The model is not the client's published viewport cache. Replacing its
    // pixels before attachment must not silently replace the surface's metrics.
    session->lockForRead();
    publishPixelGeometry(*session->paneView().terminal, { { 2, 3 }, { 160, 72 }, { 170, 82 } });
    session->unlockForRead();
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 24, 80);
    const auto first = host.plane().connection().takeViewportReports();
    REQUIRE(first.size() == 1);
    CHECK_EQ(first.front().viewport.cellPixelWidth, 9);
    CHECK_EQ(first.front().viewport.textAreaPixelWidth, 720);
    CHECK_EQ(first.front().viewport.drawablePixelHeight, 450);
    session->lockForRead();
    const auto replicaCellWidth = session->paneView().terminal->cellPixelWidth;
    const auto replicaTextWidth = session->paneView().terminal->textAreaPixelWidth;
    session->unlockForRead();
    session->publishCellPixelSize(18, 36, 1440, 864, 1480, 900);
    host.receive(factory.lastChannel->sent);
    CHECK(host.plane().connection().takeViewportReports().empty());
    session->setWindowSize(80, 24, 1440, 864);
    host.receive(factory.lastChannel->sent);
    const auto rescaled = host.plane().connection().takeViewportReports();
    REQUIRE(rescaled.size() == 1);
    CHECK_EQ(rescaled.front().viewport.cellPixelWidth, 18);
    CHECK_EQ(rescaled.front().viewport.textAreaPixelWidth, 1440);
    CHECK_EQ(rescaled.front().viewport.drawablePixelHeight, 900);
    session->publishCellPixelSize(-1, 65536, -3, 65540, 65543, -5);
    session->setWindowSize(80, 24, 0, 65535);
    host.receive(factory.lastChannel->sent);
    const auto bounded = host.plane().connection().takeViewportReports();
    REQUIRE(bounded.size() == 1);
    CHECK_EQ(bounded.front().viewport.cellPixelWidth, 0);
    CHECK_EQ(bounded.front().viewport.cellPixelHeight, 65535);
    CHECK_EQ(bounded.front().viewport.textAreaPixelWidth, 0);
    CHECK_EQ(bounded.front().viewport.textAreaPixelHeight, 65535);
    CHECK_EQ(bounded.front().viewport.drawablePixelWidth, 65535);
    CHECK_EQ(bounded.front().viewport.drawablePixelHeight, 0);
    session->lockForRead();
    CHECK_EQ(session->paneView().terminal->cellPixelWidth, replicaCellWidth);
    CHECK_EQ(session->paneView().terminal->textAreaPixelWidth, replicaTextWidth);
    session->unlockForRead();
    session.reset();
    discardPostedWork();
}

TEST(aLocalPaneLinkDeliversItsCoherentFrameOutsideTheModelLock)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    std::uint64_t now = 12'000'000'000;
    session->setCoherenceClock([](void* context) { return *static_cast<std::uint64_t*>(context); }, &now);
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        TerminalSession* session = nullptr;
        std::vector<Ubiquitty::Session::FrameReady> frames;
        std::optional<std::uint64_t> heard;
        void frameReady(const Ubiquitty::Session::FrameReady& frame) override
        {
            REQUIRE(session->tryLockForRead());
            CHECK(session->paneView().terminal);
            session->unlockForRead();
            frames.push_back(frame);
        }
        void linkStateChanged(const Ubiquitty::Session::LinkState& state) override { heard = state.transport.lastHeardMicroseconds; }
    } observer;
    observer.session = session.get();
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto* script = stream.get();
    stream->pushInbound("\x1b[?2026hfirst geometry\x1b[?2026l");
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    (void)session->drain();
    session->observePaneLink(observer);
    now += 100'000;
    script->pushInbound("\x1b[?2026hhello\x1b[?2026l");
    (void)session->drain();
    CHECK(observer.frames.empty());
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 1);
    CHECK(observer.frames.front().frameClass == Sync::FrameClass::Boundary);
    CHECK(observer.heard == now);
    (void)session->takeEventFlags();
    CHECK_EQ(observer.frames.size(), 1u);
}

TEST(aDormantLocalPaneLinkPublishesOnlyTheNewestPictureOnReveal)
{
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    std::uint64_t now = 1'000'000;
    session->setCoherenceClock([](void* context) { return *static_cast<std::uint64_t*>(context); }, &now);
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        std::vector<Ubiquitty::Session::FrameReady> frames;
        void frameReady(const Ubiquitty::Session::FrameReady& frame) override { frames.push_back(frame); }
    } observer;
    session->observePaneLink(observer);
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto* script = stream.get();
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    session->hideRemotePane();
    script->pushInbound("\x1b[?2026hone\x1b[?2026l");
    (void)session->drain();
    (void)session->takeEventFlags();
    now += 100'000;
    script->pushInbound("\x1b[?2026htwo\x1b[?2026l");
    (void)session->drain();
    (void)session->takeEventFlags();
    CHECK(observer.frames.empty());
    session->showRemotePane();
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 1);
    CHECK(observer.frames.front().generation >= 2);
}

TEST(anAdoptedPaneLinkDoesNotBorrowTheUnusedRuntimePublication)
{
    Ubiquitty::Terminal external;
    Ubiquitty::resize(external, 24, 80);
    std::mutex externalLock;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    stream->pushInbound("\x1b[?2026hlocal frame\x1b[?2026l");
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    (void)session->drain();
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        unsigned frames = 0;
        void frameReady(const Ubiquitty::Session::FrameReady&) override { ++frames; }
    } observer;
    session->adoptExternalTerminal(&external, reinterpret_cast<std::uintptr_t>(&externalLock));
    session->observePaneLink(observer);
    (void)session->takeEventFlags();
    session->lockForRead();
    CHECK(session->paneView().terminal == &external);
    CHECK(!session->paneView().hasFrameNotifications);
    session->unlockForRead();
    CHECK_EQ(observer.frames, 0u);
    session->endExternalAdoption();
    (void)session->takeEventFlags();
    CHECK_EQ(observer.frames, 1u);
}

TEST(anAdoptedHostedPaneLinkUsesItsRealOwnersPublicationAndClock)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::resize(primary, 24, 80);
    std::mutex primaryLock;
    Ubiquitty::TestDoubles::StreamDouble stream;
    std::uint64_t now = 12'000'000'000;
    Ubiquitty::Runtime::HostedWindowSeams seams;
    seams.primary = &primary;
    seams.primaryLock = &primaryLock;
    seams.stream = &stream;
    seams.sessionContext = &now;
    seams.clockMicroseconds = [](void* context) { return *static_cast<std::uint64_t*>(context); };
    Ubiquitty::Runtime::TmuxControlHost host(seams);
    host.attach();
    const auto feed = [&](std::string_view bytes) {
        const std::scoped_lock hold(primaryLock);
        Ubiquitty::processBytes(&primary, bytes);
    };
    feed("%begin 1 1 0\n%end 1 1 0\n%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n");
    const auto facts = host.paneFacts(1);
    REQUIRE(facts.terminalAddress && facts.lockAddress);
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    session->adoptExternalTerminal(reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress), facts.lockAddress);
    session->routeResizeThroughHostedSource(reinterpret_cast<std::uintptr_t>(&host), 1);
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        TerminalSession* session = nullptr;
        std::vector<Ubiquitty::Session::FrameReady> frames;
        std::optional<std::uint64_t> heard;
        void frameReady(const Ubiquitty::Session::FrameReady& frame) override
        {
            REQUIRE(session->tryLockForRead());
            session->unlockForRead();
            frames.push_back(frame);
        }
        void linkStateChanged(const Ubiquitty::Session::LinkState& state) override { heard = state.transport.lastHeardMicroseconds; }
    } observer;
    observer.session = session.get();
    session->observePaneLink(observer);
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 1);
    CHECK(observer.frames.front().frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    CHECK(observer.heard == now);
    CHECK(session->paneView().hasFrameNotifications);
    feed("%output %1 \\033[?2026hpartial\n");
    (void)session->takeEventFlags();
    CHECK_EQ(observer.frames.size(), 1u);
    now += 1'000;
    feed("%output %1 complete\\033[?2026l\n");
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 2);
    CHECK(observer.frames.back().frameClass == Ubiquitty::Protocol::Sync::FrameClass::Boundary);
    CHECK_EQ(observer.frames.back().lagMicroseconds, 1'000u);
    session->hideRemotePane();
    feed("%output %1 \\033[?2026hone\\033[?2026l\n");
    feed("%output %1 \\033[?2026htwo\\033[?2026l\n");
    (void)session->takeEventFlags();
    CHECK_EQ(observer.frames.size(), 2u);
    session->showRemotePane();
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 3);
    CHECK_EQ(observer.frames.back().generation, 4u);
    // A frontend resize reaches this source's model, not the adapter's
    // unused local runtime, and is published as structural output.
    now += Ubiquitty::Runtime::readQuiescenceMicroseconds;
    session->resize(30, 100, TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Bottom);
    session->lockForRead();
    CHECK_EQ(session->paneView().terminal->rows, 30);
    CHECK_EQ(session->paneView().terminal->columns, 100);
    session->unlockForRead();
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 4);
    CHECK(observer.frames.back().frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    CHECK_EQ(observer.frames.back().generation, 5u);
    session->endExternalAdoption();
    session.reset();
    host.detach();
}

TEST(anAdoptedHostedPaneFocusReachesItsRealOwnerAndProgram)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::resize(primary, 24, 80);
    std::mutex primaryLock;
    Ubiquitty::TestDoubles::StreamDouble stream;
    Ubiquitty::Runtime::HostedWindowSeams seams;
    seams.primary = &primary;
    seams.primaryLock = &primaryLock;
    seams.stream = &stream;
    Ubiquitty::Runtime::TmuxControlHost host(seams);
    host.attach();
    const auto feed = [&](std::string_view bytes) {
        const std::scoped_lock hold(primaryLock);
        Ubiquitty::processBytes(&primary, bytes);
    };
    feed("%begin 1 1 0\n%end 1 1 0\n%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n");
    feed("%output %1 \\033[?1004h\n");
    const auto facts = host.paneFacts(1);
    REQUIRE(facts.terminalAddress && facts.lockAddress);
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    session->adoptExternalTerminal(reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress), facts.lockAddress);
    session->routeResizeThroughHostedSource(reinterpret_cast<std::uintptr_t>(&host), 1);
    stream.written.clear();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Unfocused);
    session->lockForRead();
    CHECK(!session->paneView().terminal->windowFocused);
    session->unlockForRead();
    CHECK_STR_EQ(stream.written, "send-keys -t %1 -H 1b 5b 4f\n");
    stream.written.clear();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);
    session->lockForRead();
    CHECK(session->paneView().terminal->windowFocused);
    session->unlockForRead();
    CHECK_STR_EQ(stream.written, "send-keys -t %1 -H 1b 5b 49\n");
    session->endExternalAdoption();
    session.reset();
    host.detach();
}

TEST(aNativeRemoteFocusUpdatesItsMirrorAndReportsOnlyToItsOwner)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    discardPostedWork();
    CountingFactory factory;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    Ubiquitty::processBytes(&host.terminal(), "\x1b[?1004h");
    receiveHostScreen(factory, host, 24, 80);
    const auto before = factory.lastChannel->sent.size();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Unfocused);
    session->lockForRead();
    CHECK(!session->paneView().terminal->windowFocused);
    session->unlockForRead();
    session->setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);
    session->lockForRead();
    CHECK(session->paneView().terminal->windowFocused);
    CHECK_EQ(session->paneView().terminal->rows, 24);
    CHECK_EQ(session->paneView().terminal->columns, 80);
    std::array<char, 8> localReply;
    CHECK_EQ(Ubiquitty::drainOutput(session->paneView().terminal, localReply), 0);
    session->unlockForRead();
    std::vector<bool> focus;
    Sync::FrameDecoder decoder;
    for (std::size_t index = before; index < factory.lastChannel->sent.size(); ++index) {
        decoder.append(factory.lastChannel->sent[index]);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Sync::FrameKind::Focus)
                continue;
            CHECK_EQ(frame->channel, host.plane().handle());
            const auto decoded = Sync::decodeFocus(frame->body);
            REQUIRE(decoded);
            focus.push_back(decoded->isFocused);
        }
    }
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    CHECK((focus == std::vector<bool> { false, true }));
    session.reset();
    discardPostedWork();
}

TEST(aNativeRemotePaneSubscribesToTheCommonLinkState)
{
    discardPostedWork();
    CountingFactory factory;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 24, 80);
    const auto hasLinkEvent = [](std::uint32_t flags) {
        return !!(flags & (1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::LinkStateChanged)));
    };
    CHECK(hasLinkEvent(session->takeEventFlags()));
    CHECK(session->paneLinkIndicatorText().empty());
    CHECK(!hasLinkEvent(session->takeEventFlags()));
    REQUIRE(factory.lastChannel);
    factory.lastChannel->dropIt("mock network lost");
    pumpPostedWork();
    CHECK(hasLinkEvent(session->takeEventFlags()));
    CHECK_STR_EQ(session->paneLinkIndicatorText(), "Reconnecting\xe2\x80\xa6");
    session.reset();
    discardPostedWork();
}

namespace {

/// Whether a drained event word carries the in-doubt event.
bool hasInputInDoubtEvent(std::uint32_t flags)
{
    return !!(flags & (1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::InputInDoubtChanged)));
}

/// The key a person types for one printable character.
Ubiquitty::Runtime::KeyPress characterPress(char character)
{
    Ubiquitty::Runtime::KeyPress press;
    press.unshiftedCodePoint = static_cast<unsigned char>(character);
    press.shiftedCodePoint = static_cast<unsigned char>(character);
    press.text[0] = static_cast<unsigned char>(character);
    press.textLength = 1;
    return press;
}

/// A remote pane whose `x` went into a carrier that died before its host
/// read it, and whose `w` was typed while the session reconnected, attached
/// again to a host that keeps no count of either: `x` is in doubt, and `w`
/// waits behind it.
struct RemoteInputInDoubt {
    CountingFactory factory;
    UbiquittyTest::HostScreen first { 7, 17 };
    UbiquittyTest::HostScreen second { 7, 17 };
    TerminalSession* session = nullptr;

    RemoteInputInDoubt()
    {
        discardPostedWork();
        session = makeStartedSession();
        if (!session)
            return;
        if (!attachAndAcknowledge(session, factory, first)) {
            releaseTerminalSession(session);
            session = nullptr;
            return;
        }
        receiveWorkspaceStructure(factory, first);
        receiveHostScreen(factory, first, 24, 80);
        std::array<char, 64> encoded {};
        CHECK(session->sendKey(characterPress('x'), encoded).wasAccepted);
        factory.lastChannel->dropIt("mock network lost");
        pumpPostedWork();
        CHECK(session->sendKey(characterPress('w'), encoded).wasAccepted);
        CHECK_MSG(factory.lastChannel != nullptr, "the session did not redial");
        if (!factory.lastChannel)
            return;
        factory.lastChannel->connect();
        pumpPostedWork();
        acknowledgeFixtureHost(factory, second);
        receiveWorkspaceStructure(factory, second);
        receiveHostScreen(factory, second, 24, 80);
    }

    ~RemoteInputInDoubt()
    {
        if (session)
            releaseTerminalSession(session);
        discardPostedWork();
    }

    RemoteInputInDoubt(const RemoteInputInDoubt&) = delete;
    RemoteInputInDoubt& operator=(const RemoteInputInDoubt&) = delete;

    /// Carries what the client wrote to the second host, and its answer back.
    void exchange()
    {
        second.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(second.flush());
        pumpPostedWork();
    }

    /// The text of the key events the second host's program was given.
    [[nodiscard]] std::string typed()
    {
        std::string text;
        for (const auto& event : second.plane().typed().events()) {
            if (const auto* keys = std::get_if<Ubiquitty::Protocol::Sync::KeyInput>(&event)) {
                for (const auto& key : keys->events)
                    text += key.text;
            }
        }
        return text;
    }
};

} // namespace

// A pane whose typing a reconnect could not account for tells its frontend:
// the session raises its event, the status names what is in doubt and what
// waits behind it, and the notice says so in Core's words. Nothing is sent
// again unasked.
TEST(aRemotePaneWithInputInDoubtTellsItsFrontendAndSendsNothingUnasked)
{
    RemoteInputInDoubt doubt;
    REQUIRE(doubt.session && doubt.factory.lastChannel);
    CHECK(hasInputInDoubtEvent(doubt.session->takeEventFlags()));
    const auto status = doubt.session->remoteInputStatus();
    CHECK_EQ(status.inDoubt, 1u);
    CHECK_EQ(status.waiting, 1u);
    CHECK(status.isAwaitingDecision());
    const auto notice = doubt.session->remoteInputInDoubtNotice();
    CHECK_STR_EQ(notice.message, "Some of what you typed may not have reached the host");
    CHECK_STR_EQ(notice.resendButton, "Send Again");
    CHECK_STR_EQ(notice.discardButton, "Discard");
    doubt.exchange();
    CHECK_STR_EQ(doubt.typed(), "");
    CHECK(!hasInputInDoubtEvent(doubt.session->takeEventFlags()));
}

// Sent again at the person's word, what was in doubt goes first and what
// waited follows; the session says the decision is made.
TEST(aRemotePaneSendsItsInputInDoubtAgainOnlyWhenAsked)
{
    RemoteInputInDoubt doubt;
    REQUIRE(doubt.session && doubt.factory.lastChannel);
    (void)doubt.session->takeEventFlags();
    doubt.session->resendRemoteInputInDoubt();
    pumpPostedWork();
    doubt.exchange();
    CHECK_STR_EQ(doubt.typed(), "xw");
    CHECK(hasInputInDoubtEvent(doubt.session->takeEventFlags()));
    CHECK(!doubt.session->remoteInputStatus().isAwaitingDecision());
    CHECK(doubt.session->remoteInputInDoubtNotice().message.empty());
}

// Discarded at the person's word, what was in doubt is counted and never
// written, and what waited behind it goes.
TEST(aRemotePaneDiscardsItsInputInDoubtAndLetsWhatWaitedGo)
{
    RemoteInputInDoubt doubt;
    REQUIRE(doubt.session && doubt.factory.lastChannel);
    (void)doubt.session->takeEventFlags();
    doubt.session->discardRemoteInputInDoubt();
    pumpPostedWork();
    doubt.exchange();
    CHECK_STR_EQ(doubt.typed(), "w");
    CHECK(hasInputInDoubtEvent(doubt.session->takeEventFlags()));
    const auto status = doubt.session->remoteInputStatus();
    CHECK(!status.isAwaitingDecision());
    CHECK_EQ(status.discarded, 1u);
}

TEST(nativeTypographyReachesTheHostFromBeforeAttachAndNeverHoldsTheModelLockDuringSend)
{
    discardPostedWork();
    CountingFactory factory;
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 9 * 64;
    facts.cellHeight = 18 * 64;
    facts.faceCount = 1;
    facts.faces[0].lineHeight = 2048;
    facts.faces[0].averageAdvance = 1024;
    facts.faces[0].advances.fill(1024);
    REQUIRE(session->publishTypography(facts));
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 24, 80);
    REQUIRE(host.plane().connection().typography(1));
    CHECK(*host.plane().connection().typography(1) == facts);
    unsigned sends = 0;
    bool isUnlockedDuringSend = true;
    factory.lastChannel->onSend = [&] {
        ++sends;
        const bool acquired = session->tryLockForRead();
        isUnlockedDuringSend &= acquired;
        if (acquired)
            session->unlockForRead();
    };
    facts.cellWidth = 10 * 64;
    REQUIRE(session->publishTypography(facts));
    factory.lastChannel->onSend = {};
    CHECK(sends > 0);
    CHECK(isUnlockedDuringSend);
    host.receive(factory.lastChannel->sent);
    REQUIRE(host.plane().connection().typography(1));
    CHECK(*host.plane().connection().typography(1) == facts);
    session.reset();
    discardPostedWork();
}

TEST(aProtectedColdPictureNotifiesTheLinkBeforeAnyCarrierExists)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto source = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*source, 4, 24);
    Ubiquitty::processBytes(source.get(), "cached before network");
    Sync::ResumeRecord record {};
    record.hostInstanceID = { 90, 91 };
    record.clientInstance = { 30, 31 };
    record.attachmentID = { 40, 41 };
    record.resumeSecret = { 50, 51 };
    record.certificatePin[0] = 7;
    Sync::ResumeRecordPane pane {};
    pane.paneID = { 7, 17 };
    pane.checkpoint = UbiquittyTest::checkpointBody(*source, 12);
    pane.checkpointDigest = Sync::digestOf(pane.checkpoint);
    auto inspected = std::make_unique<Ubiquitty::Terminal>();
    REQUIRE(Sync::applyCheckpoint(*inspected, pane.checkpoint, pane.replica) == Sync::ApplyOutcome::Applied);
    record.panes.push_back(std::move(pane));
    auto store = std::make_shared<Sync::MemoryResumeRecordStore>();
    REQUIRE(store->write(record) == Sync::ResumeWriteOutcome::Committed);
    auto prepared = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
    REQUIRE(prepared->prepareResumeStore(store, record.clientInstance, record.certificatePin));
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    struct Observer final : Ubiquitty::Session::PaneLinkObserver {
        std::vector<Ubiquitty::Session::FrameReady> frames;
        void frameReady(const Ubiquitty::Session::FrameReady& frame) override { frames.push_back(frame); }
    } observer;
    session->observePaneLink(observer);
    REQUIRE(session->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&prepared), 7, 17));
    CHECK(!prepared);
    (void)session->takeEventFlags();
    REQUIRE(observer.frames.size() == 1);
    CHECK(observer.frames.front().origin == Ubiquitty::Session::FrameOrigin::Restored);
    CHECK(observer.frames.front().frameClass == Sync::FrameClass::Structural);
    CHECK_EQ(observer.frames.front().generation, 12u);
    CHECK_EQ(observer.frames.front().lagMicroseconds, 0u);
    CHECK(session->paneLinkState(0).phase == Ubiquitty::Session::LinkPhase::Connecting);
    CHECK_STR_EQ(session->paneLinkIndicatorText(), "Reconnecting\xe2\x80\xa6");
    CHECK(session->remoteState() == Ubiquitty::Session::State::Idle);
    CHECK(!session->isRemoteAttached());
    session->lockForRead();
    CHECK_EQ(Ubiquitty::Screen::visibleCell(session->terminal(), 0, 0, 0)->codePoint, static_cast<std::uint32_t>('c'));
    session->unlockForRead();
    (void)session->takeEventFlags();
    CHECK_EQ(observer.frames.size(), 1u);
    session->detachRemote();
    auto unselected = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
    REQUIRE(unselected->prepareResumeStore(store, record.clientInstance, record.certificatePin));
    REQUIRE(session->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&unselected), 0, 0));
    Observer replacement;
    session->observePaneLink(replacement);
    (void)session->takeEventFlags();
    CHECK(replacement.frames.empty());
}

TEST(aPreparedProtectedPictureDrawsBeforeAnyCarrierAndItsExactOwnerMovesIntoTheWorkspace)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    discardPostedWork();
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    auto source = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*source, 4, 24);
    const std::string text = "cached before network";
    Ubiquitty::processBytes(source.get(), text);
    Sync::ResumeRecord record {};
    record.hostInstanceID = { 90, 91 };
    record.clientInstance = { 30, 31 };
    record.attachmentID = { 40, 41 };
    record.resumeSecret = { 50, 51 };
    record.certificatePin[0] = 7;
    Sync::ResumeRecordPane pane {};
    pane.paneID = { 7, 17 };
    pane.checkpoint = UbiquittyTest::checkpointBody(*source, 12);
    pane.checkpointDigest = Sync::digestOf(pane.checkpoint);
    auto inspected = std::make_unique<Ubiquitty::Terminal>();
    REQUIRE(Sync::applyCheckpoint(*inspected, pane.checkpoint, pane.replica) == Sync::ApplyOutcome::Applied);
    record.panes.push_back(std::move(pane));
    auto store = std::make_shared<Sync::MemoryResumeRecordStore>();
    REQUIRE(store->write(record) == Sync::ResumeWriteOutcome::Committed);
    std::weak_ptr<Sync::ResumeRecordStore> lifetime = store;
    auto prepared = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
    REQUIRE(prepared->prepareResumeStore(store, record.clientInstance, record.certificatePin));
    REQUIRE(prepared->heldAttachment().has_value());
    CHECK(prepared->heldAttachment()->resumeSecret == record.resumeSecret);
    REQUIRE(session->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&prepared), 7, 17));
    CHECK(!prepared);
    CHECK(!session->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&prepared), 7, 17));
    session->lockForRead();
    CHECK_EQ(session->terminal()->rows, 4);
    CHECK_EQ(Ubiquitty::Screen::visibleCell(session->terminal(), 0, 0, 0)->codePoint, static_cast<std::uint32_t>('c'));
    session->unlockForRead();

    class ResumeFactory final : public Ubiquitty::Session::ChannelFactory {
    public:
        bool canResumeChannel() const override { return true; }
        void createChannel(Ubiquitty::Session::TransportMode, Done done) override
        {
            ++bootstraps;
            done(nullptr, false);
        }
        void resumeChannel(Ubiquitty::Session::ResumeKey key, Done done) override
        {
            ++proofs;
            CHECK(!key.buildPreamble(std::array<std::uint8_t, 32> { 8 }).empty());
            auto channel = std::make_unique<StubChannel>(&last);
            last = channel.get();
            done(std::move(channel), true);
        }
        int bootstraps = 0;
        int proofs = 0;
        StubChannel* last = nullptr;
    } factory;
    CHECK_EQ(factory.proofs, 0);
    store.reset();
    REQUIRE(!lifetime.expired());
    REQUIRE(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(&factory), 0, "work", false, "", 0, queueingLoop()));
    CHECK_EQ(factory.bootstraps, 0);
    CHECK_EQ(factory.proofs, 1);
    CHECK_EQ(session->remoteClientInstanceLow(), 30u);
    CHECK_EQ(session->remoteClientInstanceHigh(), 31u);
    REQUIRE(!lifetime.expired());
    session->detachRemote();
    CHECK(lifetime.expired());
    session.reset();
    discardPostedWork();
}

namespace {

/// A stored record whose one pane has a picture, and a core prepared from it,
/// as a platform bridge prepares one before any I/O.
struct PreparedRecord {
    std::shared_ptr<Ubiquitty::Protocol::Sync::MemoryResumeRecordStore> store = std::make_shared<Ubiquitty::Protocol::Sync::MemoryResumeRecordStore>();
    std::unique_ptr<Ubiquitty::Protocol::Sync::SyncClientCore> prepared;

    PreparedRecord()
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        auto source = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*source, 4, 24);
        Ubiquitty::processBytes(source.get(), "stored before the restart");
        Sync::ResumeRecord record {};
        record.hostInstanceID = { 90, 91 };
        record.clientInstance = { 30, 31 };
        record.attachmentID = { 40, 41 };
        record.resumeSecret = { 50, 51 };
        record.certificatePin[0] = 7;
        Sync::ResumeRecordPane pane {};
        pane.paneID = { 7, 17 };
        pane.checkpoint = UbiquittyTest::checkpointBody(*source, 12);
        pane.checkpointDigest = Sync::digestOf(pane.checkpoint);
        auto inspected = std::make_unique<Ubiquitty::Terminal>();
        CHECK(Sync::applyCheckpoint(*inspected, pane.checkpoint, pane.replica) == Sync::ApplyOutcome::Applied);
        record.panes.push_back(std::move(pane));
        CHECK(store->write(record) == Sync::ResumeWriteOutcome::Committed);
        prepared = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
        CHECK(prepared->prepareResumeStore(store, record.clientInstance, record.certificatePin));
    }
};

/// The request a macOS window's attach makes for one workspace.
Ubiquitty::Session::WorkspaceAttachRequest restartRequest()
{
    return { .authority = { .endpointIdentity = "restart-host", .principal = "restart-user", .daemonSocket = "", .daemonInstanceID = 0 },
        .workspaceID = 0,
        .sessionID = 7,
        .workspaceName = "restart",
        .reattachPolicy = Ubiquitty::Session::ReattachPolicy::AttachOrCreate };
}

} // namespace

// The attach a macOS window makes names its workspace by a request and builds
// the connection's factory itself. A prepared protected owner installed on the
// session before it joins that connection, so the restored attachment is the
// one the carrier continues, and a QUIC reach a previous process kept reaches
// the factory it builds, so the first carrier is a resume proof.
TEST(theRequestAttachAdoptsThePreparedOwnerAndSeedsItsFactoryReach)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    discardPostedWork();
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> session(makeStartedSession(), release);
    REQUIRE(session);
    PreparedRecord stored;
    std::weak_ptr<Sync::ResumeRecordStore> lifetime = stored.store;
    REQUIRE(session->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&stored.prepared), 7, 17));
    stored.store.reset();
    REQUIRE(!lifetime.expired());
    session->lockForRead();
    CHECK_EQ(Ubiquitty::Screen::visibleCell(session->terminal(), 0, 0, 0)->codePoint, static_cast<std::uint32_t>('s'));
    session->unlockForRead();

    // Only a numeric address, a port and a nonzero 32-byte pin are kept.
    const std::string pin(32, '\x5c');
    CHECK(!session->seedRemoteResumeReach("restart.example", 4433, pin));
    CHECK(!session->seedRemoteResumeReach("127.0.0.1", 0, pin));
    CHECK(!session->seedRemoteResumeReach("127.0.0.1", 4433, std::string(32, '\0')));
    CHECK(!session->seedRemoteResumeReach("127.0.0.1", 4433, pin.substr(0, 31)));
    REQUIRE(session->seedRemoteResumeReach("127.0.0.1", 4433, pin));

    REQUIRE(session->attachRemote(spawnsNothing, "127.0.0.1", "", restartRequest(), "restart", false, queueingLoop())
        == TerminalSession::RemoteWorkspaceAttachOutcome::Started);
    CHECK_EQ(session->remoteClientInstanceLow(), 30u);
    CHECK_EQ(session->remoteClientInstanceHigh(), 31u);
    CHECK_STR_EQ(session->remoteResumeReachAddress(), "127.0.0.1");
    CHECK_EQ(session->remoteResumeReachPort(), 4433u);
    CHECK(session->remoteResumeReachPin() == pin);
    // The connection owns the prepared store from here; the seed went with it.
    CHECK(!lifetime.expired());
    CHECK(!session->seedRemoteResumeReach("127.0.0.1", 4433, pin));
    session->detachRemote();
    CHECK(lifetime.expired());
    session.reset();
    discardPostedWork();
}

// A second window's prepared owner cannot join a workspace connection this
// process already carries: that connection has its attachment. The owner,
// and the store it holds, are let go of rather than kept beside it.
TEST(aRequestAttachThatReusesAConnectionLetsThePreparedOwnerGo)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    discardPostedWork();
    const auto release = [](TerminalSession* value) { releaseTerminalSession(value); };
    std::unique_ptr<TerminalSession, decltype(release)> first(makeStartedSession(), release);
    std::unique_ptr<TerminalSession, decltype(release)> second(makeStartedSession(), release);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->attachRemote(spawnsNothing, "", "", restartRequest(), "restart", false, queueingLoop())
        == TerminalSession::RemoteWorkspaceAttachOutcome::Started);
    PreparedRecord stored;
    std::weak_ptr<Sync::ResumeRecordStore> lifetime = stored.store;
    REQUIRE(second->prepareRemoteResume(reinterpret_cast<std::uintptr_t>(&stored.prepared), 7, 17));
    stored.store.reset();
    REQUIRE(!lifetime.expired());
    (void)second->attachRemote(spawnsNothing, "", "", restartRequest(), "restart", false, queueingLoop());
    CHECK(lifetime.expired());
    second->detachRemote();
    first->detachRemote();
    second.reset();
    first.reset();
    discardPostedWork();
}

TEST(preparedResumeRefusesAnotherStoredClientOrPinBeforeDrawing)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto store = std::make_shared<Sync::MemoryResumeRecordStore>();
    Sync::ResumeRecord record {};
    record.hostInstanceID = { 1, 2 };
    record.clientInstance = { 3, 4 };
    record.attachmentID = { 5, 6 };
    record.resumeSecret = { 7, 8 };
    record.certificatePin[0] = 9;
    REQUIRE(store->write(record) == Sync::ResumeWriteOutcome::Committed);
    Sync::SyncClientCore wrongClient(nullptr, nullptr);
    CHECK(!wrongClient.prepareResumeStore(store, { 3, 5 }, record.certificatePin));
    Sync::SyncClientCore wrongPin(nullptr, nullptr);
    CHECK(!wrongPin.prepareResumeStore(store, record.clientInstance, std::array<std::uint8_t, 32> { 10 }));
    Sync::SyncClientCore correct(nullptr, nullptr);
    CHECK(correct.prepareResumeStore(store, record.clientInstance, record.certificatePin));
    CHECK(!correct.prepareResumeStore(store, record.clientInstance, record.certificatePin));
}

TEST(touchScrollUsesRemoteApplicationModesAndSendsInputOverSync)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, host));
    receiveWorkspaceStructure(factory, host);
    Ubiquitty::processBytes(&host.terminal(), "\033[?1049h\033[?1h");

    TerminalSession::ScrollFact swipe;
    swipe.isPrecise = true;
    swipe.isDirectTouch = true;
    swipe.deltaY = -16;
    swipe.row = 3;
    swipe.column = 5;
    Ubiquitty::Screen::ScrollTuning::Policy tuning;
    tuning.pointsPerLine = 16;
    for (const auto isMouseTrackingEnabled : { false, true }) {
        if (isMouseTrackingEnabled) {
            Ubiquitty::processBytes(&host.terminal(), "\033[?1000h\033[?1006h");
        }
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.publish());
        pumpPostedWork();
        const auto outcome = session->scrollFact(swipe, tuning);
        CHECK(outcome.wasConsumed && !outcome.isViewportScroll);
        CHECK(outcome.linesToOffsetView == 0);
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        // The bytes the remote program's modes give, a wheel report or an
        // arrow, as one event on the pane's own channel.
        const std::string input = isMouseTrackingEnabled ? "\033[<65;6;4M" : "\033OB";
        const std::optional<std::pair<std::uint32_t, std::string>> raw = lastRawOnCarrier(factory.lastChannel->sent);
        REQUIRE(raw.has_value());
        CHECK_EQ(raw->first, host.plane().handle());
        CHECK_STR_EQ(raw->second, input);
        CHECK(host.plane().typed().bytes().ends_with(input));
    }
    releaseTerminalSession(session);
    discardPostedWork();
}

// What a person does at a remote pane travels as what they did, and the host
// encodes it under the modes its program set: a key press as a key event, a
// paste as its text, a click the program tracks as a pointer event. None of
// them leaves as bytes this mirror's modes gave.
TEST(aRemoteKeyPasteAndClickTravelAsIntentsNotBytes)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, host));
    receiveWorkspaceStructure(factory, host);
    Ubiquitty::processBytes(&host.terminal(), "\033[?1000h\033[?1006h\033[?2004h");
    const auto exchange = [&] {
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.publish());
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
    };
    exchange();
    const std::vector<Sync::InputIntent>& events = host.plane().typed().events();
    REQUIRE(events.empty());

    Ubiquitty::Runtime::KeyPress press;
    press.unshiftedCodePoint = 'a';
    press.shiftedCodePoint = 'a';
    press.text[0] = 'a';
    press.textLength = 1;
    std::array<char, 64> encoded {};
    const Ubiquitty::Runtime::KeyEncoding encoding = session->sendKey(press, encoded);
    CHECK_EQ(encoding.length, 1);
    CHECK(encoding.wasAccepted);
    CHECK_EQ(encoded[0], 'a');
    exchange();
    REQUIRE(events.size() == 1);
    const auto* keys = std::get_if<Sync::KeyInput>(&events[0]);
    REQUIRE(keys != nullptr && keys->events.size() == 1);
    CHECK(keys->events[0] == Ubiquitty::Runtime::keyEventOf(press));

    // A press that encodes to nothing here is not sent.
    Ubiquitty::Runtime::KeyPress nothing;
    const auto ignored = session->sendKey(nothing, encoded);
    CHECK_EQ(ignored.length, 0);
    CHECK(!ignored.wasAccepted);
    exchange();
    CHECK_EQ(events.size(), 1u);

    const std::string wrapped = session->sendPaste("two\nlines");
    CHECK_STR_EQ(wrapped, std::string("\033[200~two\rlines\033[201~"));
    exchange();
    REQUIRE(events.size() == 2);
    const auto* paste = std::get_if<Sync::PasteInput>(&events[1]);
    REQUIRE(paste != nullptr);
    CHECK(std::string(paste->text.begin(), paste->text.end()) == "two\nlines");
    CHECK(paste->end == Sync::PasteEnd::Final);

    Ubiquitty::Input::PointerFact click;
    click.kind = Ubiquitty::Input::PointerFact::Kind::Press;
    click.button = Ubiquitty::Input::PointerButton::Left;
    click.row = 8;
    click.column = 33;
    click.clickCount = 1;
    (void)session->pointerFact(click, nullptr);
    exchange();
    REQUIRE(events.size() == 3);
    const auto* pointer = std::get_if<Sync::PointerInput>(&events[2]);
    REQUIRE(pointer != nullptr && pointer->events.size() == 1);
    const Sync::PointerEvent pressed { .kind = Sync::PointerEventKind::Press, .button = 0, .modifiers = 0, .row = 8, .column = 33, .pixelX = Sync::noPointerPixel, .pixelY = Sync::noPointerPixel };
    CHECK(pointer->events[0] == pressed);
    CHECK(host.plane().typed().bytes().empty());

    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aRemoteKeyUsesHostModesWhenItsLocalEncodingIsEmpty)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, host));
    receiveWorkspaceStructure(factory, host);
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.publish());
    pumpPostedWork();

    struct Sink final : Ubiquitty::Runtime::ByteSink {
        Sync::WriteStatus writeAll(Sync::ID128, std::span<const std::uint8_t> input) override
        {
            bytes.append(input.begin(), input.end());
            return Sync::WriteStatus::Written;
        }
        std::string bytes;
    } sink;
    Ubiquitty::Runtime::RemoteInputEncoder encoder(host.terminal(), sink);
    host.plane().source().setInputWriter(encoder);

    // The program asks for releases, but that mode update has not reached
    // the mirror. Its empty encoding is not authority to discard the event.
    Ubiquitty::processBytes(&host.terminal(), "\033[>7u");
    Ubiquitty::Runtime::KeyPress release;
    release.unshiftedCodePoint = 'c';
    release.shiftedCodePoint = 'c';
    release.wasModified = true;
    release.modifiers = Ubiquitty::Keys::Keymap::keymapModifierCtrl;
    release.eventType = Ubiquitty::Keys::KeyEventType::Release;
    std::array<char, 64> encoded {};
    REQUIRE(session->encodeKey(release, encoded).length == 0);
    const auto accepted = session->sendKey(release, encoded);
    CHECK_EQ(accepted.length, 0);
    CHECK(accepted.wasAccepted);
    host.receive(factory.lastChannel->sent);
    CHECK_STR_EQ(sink.bytes, "\033[99;5:3u");

    // Conversely, the same release is harmless once the host no longer
    // wants it, even while the client has not heard that reset either.
    sink.bytes.clear();
    Ubiquitty::processBytes(&host.terminal(), "\033[<u");
    CHECK_EQ(session->sendKey(release, encoded).length, 0);
    host.receive(factory.lastChannel->sent);
    CHECK(sink.bytes.empty());

    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aRemoteKeyIsNotAcceptedBeforeItsPaneIsNamed)
{
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    REQUIRE(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    REQUIRE(factory.lastChannel != nullptr);
    const auto before = factory.lastChannel->sent.size();
    Ubiquitty::Runtime::KeyPress press;
    press.unshiftedCodePoint = 'a';
    press.shiftedCodePoint = 'a';
    std::array<char, 64> encoded {};
    const auto encoding = session->sendKey(press, encoded);
    CHECK_EQ(encoding.length, 1);
    CHECK(!encoding.wasAccepted);
    CHECK_EQ(factory.lastChannel->sent.size(), before);
    releaseTerminalSession(session);
    discardPostedWork();
}

namespace {

/// The text each clipboard write put on this machine's clipboard.
struct ClipboardWriteLog final : Ubiquitty::ClipboardClient {
    std::vector<std::string> writes;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> bytes) override { writes.emplace_back(bytes.data(), bytes.size()); }
};

/// A session attached to a scripted host over an embedder's carrier, which
/// the client cannot place, so the host is another machine's. Its pane is in a
/// background tab, where the host sends what the program did without waiting
/// for a picture the client never draws here, and its clipboard is recorded.
struct RemoteClipboardPane {
    static constexpr Ubiquitty::Protocol::Sync::ID128 clientInstance { .low = 0x51, .high = 0x52 };
    TerminalSession* session = nullptr;
    CountingFactory factory;
    UbiquittyTest::HostScreen host { 7, 17 };
    ClipboardWriteLog clipboard;

    RemoteClipboardPane()
    {
        discardPostedWork();
        session = makeStartedSession();
        if (!session)
            return;
        session->setRemoteClientInstance(clientInstance.low, clientInstance.high);
        if (!attachAndAcknowledge(session, factory, host))
            return;
        receiveWorkspaceStructure(factory, host);
        receiveHostScreen(factory, host, 24, 80);
        session->hideRemotePane();
        pumpPostedWork();
        session->terminal()->clipboardClient = &clipboard;
    }

    ~RemoteClipboardPane()
    {
        if (session) {
            session->terminal()->clipboardClient = nullptr;
            releaseTerminalSession(session);
        }
        discardPostedWork();
    }

    /// Whether the host was reached and the pane is bound.
    [[nodiscard]] bool isReady() { return session && factory.lastChannel && host.plane().handle() != 0; }

    /// Exchanges what each side has queued, a few turns.
    void exchange()
    {
        for (int round = 0; round < 6; ++round) {
            host.receive(factory.lastChannel->sent);
            factory.lastChannel->receive(round == 0 ? host.publish() : host.flush());
            pumpPostedWork();
        }
    }

    /// The host delivers a clipboard write its own policy admitted for this
    /// client, the pane's elected human.
    ///
    /// - Parameter text: The write's one text item.
    void deliver(std::string_view text)
    {
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        Ubiquitty::Protocol::Sync::EffectClipboardBundle bundle;
        bundle.count = 1;
        bundle.items[0] = { "text/plain", bytes };
        REQUIRE(host.plane().source().appendClipboardWrite(Ubiquitty::ClipboardBuffer::Clipboard, bundle, clientInstance));
        exchange();
    }

    /// The host asks its own capability question about this pane's session.
    ///
    /// - Parameters:
    ///   - requestID: The question.
    ///   - capability: What it asks for.
    void hostAsks(std::uint32_t requestID, Ubiquitty::Capability capability)
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        const Sync::ConsentQuestion question {
            .requestID = requestID,
            .sessionID = 7,
            .peerProcessID = 0,
            .peerProcess = {},
            .agentLabel = {},
            .action = std::to_underlying(Sync::ConsentKind::Capability),
            .capabilityCode = std::to_underlying(capability),
            .capabilityOrigin = std::to_underlying(Ubiquitty::CapabilityOrigin::Remote),
            .isRecipientsOwnRequest = true,
        };
        const auto notification = Sync::consentQuestionNotify(question);
        REQUIRE(notification);
        REQUIRE(host.plane().connection().sendNotify(*notification));
        exchange();
    }

    /// The decision word the client sent the host for a question, if any.
    ///
    /// - Parameters:
    ///   - kind: The question's kind.
    ///   - requestID: The question.
    /// - Returns: The word, or nothing when no answer names it.
    [[nodiscard]] std::optional<std::string> sentDecision(Ubiquitty::Protocol::Sync::ConsentKind kind, std::uint32_t requestID) const
    {
        const std::string named = "consent:" + std::to_string(std::to_underlying(kind)) + ":" + std::to_string(requestID);
        constexpr std::string_view key = "\"decision\":\"";
        for (const auto& write : factory.lastChannel->sent) {
            const std::string_view text(reinterpret_cast<const char*>(write.data()), write.size());
            if (text.find(named) == std::string_view::npos)
                continue;
            const auto at = text.find(key);
            if (at == std::string_view::npos)
                continue;
            const auto begin = at + key.size();
            const auto end = text.find('"', begin);
            if (end != std::string_view::npos)
                return std::string(text.substr(begin, end - begin));
        }
        return std::nullopt;
    }

    /// Whether the client told the host it could not show a question, without
    /// answering it.
    ///
    /// - Parameter requestID: The question.
    /// - Returns: Whether a decline as unshown names it.
    [[nodiscard]] bool hasDeclinedUnshown(std::uint32_t requestID) const
    {
        const std::string named = "consent:" + std::to_string(std::to_underlying(Ubiquitty::Protocol::Sync::ConsentKind::Capability)) + ":" + std::to_string(requestID);
        return std::ranges::any_of(factory.lastChannel->sent, [&](const std::vector<std::uint8_t>& write) {
            const std::string_view text(reinterpret_cast<const char*>(write.data()), write.size());
            return text.find(named) != std::string_view::npos && text.find("\"unshown\"") != std::string_view::npos;
        });
    }

    /// Whether the client has sent the host any answer to a question.
    ///
    /// - Parameter requestID: The question.
    /// - Returns: Whether an answer's call names it.
    [[nodiscard]] bool hasAnswered(std::uint32_t requestID) const
    {
        const std::string named = "consent:" + std::to_string(std::to_underlying(Ubiquitty::Protocol::Sync::ConsentKind::Capability)) + ":" + std::to_string(requestID);
        return std::ranges::any_of(factory.lastChannel->sent, [&](const std::vector<std::uint8_t>& write) {
            return std::string_view(reinterpret_cast<const char*>(write.data()), write.size()).find(named) != std::string_view::npos;
        });
    }

    /// The host delivers a notification its own policy admitted for this
    /// client.
    ///
    /// - Parameter title: The notification's title.
    void deliverNotification(std::string_view title)
    {
        const std::string owned(title);
        REQUIRE(host.plane().source().appendEffect(Ubiquitty::Protocol::Sync::NotificationEffect { owned, "body", "", Ubiquitty::NotificationOccasion::Always }, clientInstance));
        exchange();
    }

    /// Whether the question shown is this client's own, about its host's
    /// clipboard write, rather than one the host asked.
    [[nodiscard]] bool isClientsOwnClipboardQuestion() const
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        return session->pendingConsentToken() != 0 && session->pendingConsentRequestID() == 0
            && session->pendingConsentAction() == std::to_underlying(Sync::ConsentKind::Capability)
            && session->pendingConsentCapabilityCode() == std::to_underlying(Ubiquitty::Capability::ClipboardWrite)
            && session->pendingConsentCapabilityOrigin() == std::to_underlying(Ubiquitty::CapabilityOrigin::Remote)
            && session->pendingConsentIsOwnRequest() && session->pendingConsentIsClientHeld()
            && !session->pendingConsentGrantIsPerUse();
    }

    /// Answers the question shown.
    ///
    /// - Parameter choice: The person's answer.
    void answer(Ubiquitty::Protocol::Sync::ConsentChoice choice)
    {
        session->respondToConsent(session->pendingConsentToken(), std::to_underlying(choice));
        pumpPostedWork();
    }
};

} // namespace

// The host admitted a clipboard write for the person driving the pane and
// delivers it, and the client applies its own Remote-origin policy rather than
// the host's decision. The write waits as this device's own capability
// question; a refusal discards it, and only an allow puts it on this machine's
// clipboard. A session answer covers the writes after it.
TEST(aRemoteHostsClipboardWriteWaitsForThePersonsAnswerOnTheClient)
{
    using Ubiquitty::Protocol::Sync::ConsentChoice;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("first");
    CHECK_MSG(pane.clipboard.writes.empty(), "a remote host's clipboard write reached this machine's clipboard unasked");
    CHECK_MSG(pane.isClientsOwnClipboardQuestion(), "the client did not ask the person about its host's clipboard write");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.answer(ConsentChoice::AllowOnce);
    REQUIRE(pane.clipboard.writes.size() == 1);
    CHECK(pane.clipboard.writes[0] == "first");
    CHECK_EQ(pane.session->pendingConsentToken(), 0u);

    pane.deliver("second");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.answer(ConsentChoice::Deny);
    CHECK_EQ(pane.clipboard.writes.size(), 1u);

    pane.deliver("third");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.answer(ConsentChoice::AllowSession);
    pane.deliver("fourth");
    CHECK_EQ(pane.session->pendingConsentToken(), 0u);
    CHECK((pane.clipboard.writes == std::vector<std::string> { "first", "third", "fourth" }));
}

// A host question that arrives while another question is shown waits its turn.
// Nothing is answered for it until the person has seen it.
TEST(aHostQuestionWaitsBehindTheShownQuestion)
{
    using Ubiquitty::Protocol::Sync::ConsentChoice;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("waiting");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.hostAsks(7, Ubiquitty::Capability::DesktopNotification);
    CHECK(pane.isClientsOwnClipboardQuestion());
    CHECK_MSG(!pane.hasAnswered(7), "the host was answered for a question the person never saw");
    pane.answer(ConsentChoice::Deny);
    CHECK_MSG(pane.session->pendingConsentRequestID() == 7 && !pane.session->pendingConsentIsClientHeld(),
        "a host question that arrived behind a shown question was never shown");
    CHECK(!pane.hasAnswered(7));
}

// The line behind the shown question is short. A host question that finds it
// full is never answered unseen: an answer from any recipient decides the
// question for all of them, so this client would be refusing for a person
// looking at it elsewhere. It is declined as unshown instead, which leaves it to
// the others asked. The ones in line are still shown in the order they came.
TEST(aHostQuestionPastAFullLineIsDeclinedAsUnshown)
{
    using Ubiquitty::Protocol::Sync::ConsentChoice;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("waiting");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    for (const std::uint32_t requestID : { 11u, 12u, 13u, 14u, 15u })
        pane.hostAsks(requestID, Ubiquitty::Capability::DesktopNotification);
    for (const std::uint32_t requestID : { 11u, 12u, 13u, 14u })
        CHECK_MSG(!pane.hasAnswered(requestID), "the host was answered for a question waiting in line");
    CHECK_MSG(pane.hasDeclinedUnshown(15), "a question past a full line was neither shown nor declined");
    pane.answer(ConsentChoice::Deny);
    for (const std::uint32_t requestID : { 11u, 12u, 13u, 14u }) {
        CHECK_EQ(pane.session->pendingConsentRequestID(), requestID);
        pane.answer(ConsentChoice::Deny);
    }
    CHECK_EQ(pane.session->pendingConsentToken(), 0u);
}

namespace Ubiquitty::Session {

/// Does to a pane's endpoint what its workspace does to one it has not
/// acknowledged.
struct RemoteSessionWorkspacePaneAccess {
    /// Re-establishes the pane on its workspace's carrier: the endpoint resets,
    /// withdrawing every effect it held, with no change of state after it.
    ///
    /// - Parameter core: The pane's endpoint.
    static void reestablish(RemoteSessionCore& core) { core.establishWorkspacePane(core.sessionID()); }
};

} // namespace Ubiquitty::Session

// Only a person's answer reaches the host as an answer. Every way a question
// leaves without one, declined by the frontend that could not show it, a
// structure question it could not show, or a value that is no answer, reaches
// the host as unshown or as the refusal the person's own Deny is, and an
// effect this client holds is discarded here rather than sent anywhere.
// A pane that leaves its connection declines every host question it holds,
// the one shown and those waiting behind it, so the host, whose connection may
// carry other panes still, does not wait on it for them.
TEST(aPaneLeavingItsConnectionDeclinesTheQuestionsItHolds)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.hostAsks(51, Ubiquitty::Capability::DesktopNotification);
    pane.hostAsks(52, Ubiquitty::Capability::ClipboardWrite);
    REQUIRE(pane.session->pendingConsentRequestID() == 51);
    // The carrier goes with the pane, so what it carried is kept here.
    std::vector<std::string> carried;
    StubChannel* channel = pane.factory.lastChannel;
    REQUIRE(channel);
    channel->onSend = [&carried, channel] {
        carried.emplace_back(reinterpret_cast<const char*>(channel->sent.back().data()), channel->sent.back().size());
    };
    pane.session->detachRemote();
    pumpPostedWork();
    const auto isDeclined = [&](std::uint32_t requestID) {
        const std::string named = "consent:" + std::to_string(std::to_underlying(Sync::ConsentKind::Capability)) + ":" + std::to_string(requestID);
        return std::ranges::any_of(carried, [&](const std::string& text) { return text.find(named) != std::string::npos && text.find("\"decision\":\"unshown\"") != std::string::npos; });
    };
    CHECK_MSG(isDeclined(51), "the question a leaving pane showed was left to the host's deadline");
    CHECK_MSG(isDeclined(52), "a question waiting in a leaving pane was left to the host's deadline");
    CHECK(pane.session->pendingConsentToken() == 0);
}

TEST(onlyAPersonsAnswerReachesTheHostAsAnAnswer)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.hostAsks(41, Ubiquitty::Capability::DesktopNotification);
    REQUIRE(pane.session->pendingConsentRequestID() == 41);
    pane.session->declineConsent(pane.session->pendingConsentToken());
    pumpPostedWork();
    CHECK_MSG(pane.sentDecision(Sync::ConsentKind::Capability, 41) == std::optional<std::string> { "unshown" }, "a question the frontend could not show was answered for the person");

    pane.hostAsks(42, Ubiquitty::Capability::DesktopNotification);
    REQUIRE(pane.session->pendingConsentRequestID() == 42);
    pane.session->respondToConsent(pane.session->pendingConsentToken(), std::to_underlying(Sync::ConsentChoice::Unshown));
    pumpPostedWork();
    CHECK_MSG(pane.sentDecision(Sync::ConsentKind::Capability, 42) == std::optional<std::string> { "deny" }, "a value that is no answer reached the host as a decline");

    const Sync::StructureConsent change {
        .requestID = 43,
        .sessionID = 7,
        .workspaceID = 3,
        .revision = 1,
        .workspaceName = "work",
        .method = "window.close",
        .parameters = R"({"window":4})",
        .agentLabel = "helper",
        .peerProcess = "/usr/bin/tool",
        .peerProcessID = 7,
    };
    pane.session->noteStructureConsentForTest(change);
    REQUIRE(pane.session->pendingStructureConsentToken() != 0);
    pane.session->declineStructureConsent(pane.session->pendingStructureConsentToken());
    pumpPostedWork();
    CHECK_MSG(pane.sentDecision(Sync::ConsentKind::Structure, 43) == std::optional<std::string> { "unshown" }, "a tree change the frontend could not show was denied for the person");

    pane.deliver("held");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.session->declineConsent(pane.session->pendingConsentToken());
    pumpPostedWork();
    CHECK(pane.clipboard.writes.empty() && pane.session->pendingConsentToken() == 0);
}

// A held effect's question closes whatever took the effect, including a
// connection the endpoint ended with no change of state after it, as a
// workspace re-establishing the pane does: no dead prompt stays up for an
// effect that is gone, and what waited behind it is shown a turn later. A
// dropped connection's change of state ends its questions before that turn.
TEST(aHeldEffectsQuestionClosesWhenItsConnectionEndsWithoutAStateChange)
{
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("held");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.hostAsks(31, Ubiquitty::Capability::DesktopNotification);
    auto* core = pane.session->remoteCoreForTest();
    REQUIRE(core);
    Ubiquitty::Session::RemoteSessionWorkspacePaneAccess::reestablish(*core);
    pumpPostedWork();
    // The carrier the pane is re-established on is still the one the host
    // question came over, and no change of state follows, so what waited
    // behind the held effect is shown a turn later rather than left hidden.
    CHECK_MSG(pane.session->pendingConsentRequestID() == 31, "a question that waited behind it stayed hidden after its pane was re-established");
    CHECK(!pane.hasAnswered(31));
    pane.answer(Ubiquitty::Protocol::Sync::ConsentChoice::Deny);
    pane.deliver("held again");
    CHECK_MSG(pane.isClientsOwnClipboardQuestion(), "a write after the withdrawal was not asked about");
}

// One question is shown at a time and the rest wait their turn: a notification
// that arrives while a clipboard question is shown is asked about once that
// one is answered, never dropped.
TEST(aSecondKindWaitsItsTurnBehindTheShownQuestion)
{
    using Ubiquitty::Protocol::Sync::ConsentChoice;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("waiting");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.deliverNotification("built");
    CHECK(pane.isClientsOwnClipboardQuestion());
    pane.answer(ConsentChoice::Deny);
    CHECK_MSG(pane.session->pendingConsentToken() != 0 && pane.session->pendingConsentIsClientHeld()
            && pane.session->pendingConsentCapabilityCode() == std::to_underlying(Ubiquitty::Capability::DesktopNotification),
        "a notification that arrived behind a shown question was never asked about");
    CHECK(pane.clipboard.writes.empty());
}

// A held effect goes with the connection that delivered it, and its question
// closes rather than standing as a dead prompt in front of the next one. The
// questions waiting behind it end with the connection too, and so do they when
// the question shown is the host's: what ends them is the connection, not
// which question happens to be shown.
TEST(aDroppedConnectionClosesTheQuestionForItsHeldEffect)
{
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("held");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.hostAsks(21, Ubiquitty::Capability::DesktopNotification);
    const std::uint64_t token = pane.session->pendingConsentToken();
    pane.factory.lastChannel->dropIt("network changed");
    pumpPostedWork();
    CHECK_MSG(pane.session->pendingConsentToken() == 0, "the question for an effect the dropped connection held stayed open");
    pane.session->respondToConsent(token, std::to_underlying(Ubiquitty::Protocol::Sync::ConsentChoice::AllowOnce));
    pumpPostedWork();
    CHECK_MSG(pane.session->pendingConsentToken() == 0, "a question that waited behind it outlived the connection");
    CHECK(pane.clipboard.writes.empty());

    RemoteClipboardPane asked;
    REQUIRE(asked.isReady());
    asked.hostAsks(22, Ubiquitty::Capability::DesktopNotification);
    asked.deliver("held behind the host's question");
    REQUIRE(asked.session->pendingConsentRequestID() == 22);
    asked.factory.lastChannel->dropIt("network changed");
    pumpPostedWork();
    CHECK_MSG(asked.session->pendingConsentToken() == 0, "the host's question outlived its connection");
    CHECK(asked.clipboard.writes.empty());
}

// The client's verdicts are its settings', read when the effect arrives: a
// Deny set while the pane is attached refuses the next write unasked, even
// after the person allowed writes for the connection.
TEST(aSettingsChangeReachesAnAttachedRemotePane)
{
    using Ubiquitty::Protocol::Sync::ConsentChoice;
    RemoteClipboardPane pane;
    REQUIRE(pane.isReady());
    pane.deliver("first");
    REQUIRE(pane.isClientsOwnClipboardQuestion());
    pane.answer(ConsentChoice::AllowSession);
    REQUIRE(pane.clipboard.writes.size() == 1);
    (void)Ubiquitty::Preferences::configure("[capabilities.remote]\nhost_escaping = \"deny\"\n", Ubiquitty::Preferences::ParseMode::Strict);
    pane.deliver("after the change");
    (void)Ubiquitty::Preferences::configure(std::string(), Ubiquitty::Preferences::ParseMode::Permissive);
    CHECK_MSG(pane.clipboard.writes.size() == 1, "a Deny set in settings did not reach the attached pane");
    CHECK_EQ(pane.session->pendingConsentToken(), 0u);
}

TEST(anEmbedderFactoryIsTheOneTheSessionAsks)
{
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CHECK(!session->isRemoteAttached());

    CountingFactory factory;
    const bool attached = session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop());

    CHECK_MSG(attached, "the delegated attach did not start");
    CHECK_MSG(session->isRemoteAttached(), "the session is not a thin client after attaching");
    CHECK_MSG(factory.createCount > 0, "the session built a carrier without asking the embedder's factory");

    releaseTerminalSession(session);
}

// A hosted pane names its program from the host's record, and the host's own
// sample of who holds the terminal decides whether a status record keys it:
// names alone cannot tell a nested shell running a script from the shell at
// its prompt. Once the host says the session ended, nothing is named, whether
// or not its last facts arrived first.
TEST(aHostedPaneNamesItsProgramFromTheHostsSample)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Ubiquitty::Runtime::EventKind;
    using Ubiquitty::Runtime::ForegroundProgram;
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CountingFactory factory;
    REQUIRE(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)), 0, queueingLoop()));
    const std::uint8_t codex = Ubiquitty::Title::processIconOrdinal(Ubiquitty::Title::processIconWithKey("codex"));
    constexpr auto processChanged = 1u << static_cast<unsigned>(EventKind::ProcessStateChanged);

    // The screen can land before the host's facts: its record keys nothing yet.
    CHECK(session->processIconKey(codex).empty());
    CHECK(session->foregroundProgram(codex).name.empty());

    Sync::PaneFacts facts;
    facts.revision = 1;
    facts.foreground.name = "bash";
    facts.shellCommandName = "bash";
    facts.processChain = "bash ◂ -bash";
    facts.processChainWithArguments = "bash deploy.sh ◂ -bash";
    facts.isAtPrompt = false;
    session->applyRemoteTitlePartsForTest(facts);
    const ForegroundProgram script = session->foregroundProgram(codex);
    CHECK_STR_EQ(script.name, "bash");
    CHECK_STR_EQ(script.key, "codex");
    CHECK_STR_EQ(script.chain, "bash ◂ -bash");
    CHECK_STR_EQ(script.chainWithArguments, "bash deploy.sh ◂ -bash");
    CHECK_STR_EQ(session->processIconKey(codex), "codex");

    // The host's shell has the terminal back: only who holds it moved.
    (void)session->takeEventFlags();
    facts.revision = 2;
    facts.isAtPrompt = true;
    session->applyRemoteTitlePartsForTest(facts);
    CHECK((session->takeEventFlags() & processChanged) != 0);
    CHECK(session->processIconKey(codex).empty());
    CHECK(session->foregroundProgram(codex).key.empty());

    // A host that never resolved its shell's name still says who holds it.
    facts.revision = 3;
    facts.shellCommandName.clear();
    session->applyRemoteTitlePartsForTest(facts);
    CHECK(session->processIconKey(codex).empty());

    // The end arrives ahead of the facts that would clear the names.
    session->noteHostedSessionEndedForTest();
    CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::Exited))) != 0);
    const ForegroundProgram ended = session->foregroundProgram(codex);
    CHECK(ended.name.empty() && ended.key.empty() && ended.chain.empty() && ended.chainWithArguments.empty());

    releaseTerminalSession(session);
}

TEST(aNullFactoryIsRefusedRatherThanDereferenced)
{
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CHECK_MSG(!session->attachRemoteOverCarrier(0, 0, queueingLoop()),
        "a zero factory address was accepted");
    CHECK_MSG(!session->isRemoteAttached(), "a refused attach still marked the session remote");

    releaseTerminalSession(session);
}

TEST(aDirectoryOwnerIsPinnedBeforeTheFirstDelegatedCarrier)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory factory;
    CHECK(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        7, "", false, "listed.sock", 17, queueingLoop()));
    CHECK_EQ(factory.createCount, 1);
    CHECK(factory.pinAtFirstCreate == "listed.sock");
    REQUIRE(factory.lastChannel);
    factory.lastChannel->connect();
    pumpPostedWork();
    REQUIRE(!factory.lastChannel->sent.empty());
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::FrameDecoder decoder;
    decoder.append(factory.lastChannel->sent.front());
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet && frame->channel == 0);
    const auto greeting = Sync::decodeGreet(frame->body);
    REQUIRE(greeting);
    CHECK_EQ(greeting->request.expectedDaemonInstanceID, 17u);
    CHECK_EQ(greeting->request.sessionID, 7u);
    CHECK(greeting->request.isAttachOnly);
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(anInvalidDirectoryOwnerCreatesNoDelegatedCarrier)
{
    for (const auto& [socketName, instanceID] : std::array {
             std::pair { std::string("../other.sock"), std::uint64_t { 17 } },
             std::pair { std::string("listed.sock;echo"), std::uint64_t { 17 } },
             std::pair { std::string(65'536, 'a'), std::uint64_t { 17 } },
             std::pair { std::string("listed.sock"), std::uint64_t { 0 } },
             std::pair { std::string(), std::uint64_t { 17 } } }) {
        TerminalSession* session = makeStartedSession();
        REQUIRE(session);
        CountingFactory factory;
        CHECK(!session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
            7, "", false, socketName, instanceID, queueingLoop()));
        CHECK(!session->isRemoteAttached());
        CHECK_EQ(factory.createCount, 0);
        CHECK_EQ(factory.pinCount, 0);
        releaseTerminalSession(session);
    }
    discardPostedWork();
}

TEST(aRefusedDirectoryPinCreatesNoDelegatedCarrier)
{
    TerminalSession* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory factory;
    factory.acceptsPins = false;
    CHECK(!session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        7, "", false, "listed.sock", 17, queueingLoop()));
    CHECK(!session->isRemoteAttached());
    CHECK_EQ(factory.pinCount, 1);
    CHECK_EQ(factory.createCount, 0);
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aDelegatedAttachRejectsAnUnrelatedDirectoryOwner)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory factory;
    REQUIRE(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        7, "", false, "listed.sock", 17, queueingLoop()));
    REQUIRE(factory.lastChannel);
    factory.lastChannel->connect();
    pumpPostedWork();
    Sync::Welcome welcome;
    welcome.wireVersion = Sync::wireVersion;
    welcome.hostInstanceID = { 22, 0 };
    welcome.attachmentID = { 1, 22 };
    welcome.resumeSecret = { 2, 22 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = 7,
        .workspaceID = 91,
        .daemonInstanceID = 22,
        .socketName = "other.sock",
        .buildFlags = 0,
        .hostPlatform = "fixture" };
    std::vector<std::uint8_t> body, frame;
    Sync::encode(body, welcome);
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Welcome, 0, body));
    factory.lastChannel->receive(frame);
    pumpPostedWork();
    CHECK(session->wasReattachRefused());
    CHECK(factory.pinnedSocket == "listed.sock");
    CHECK_EQ(factory.createCount, 1);
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aBorrowedFactoryForwardsTheAcknowledgedPinAndHandoverClear)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory factory;
    REQUIRE(session->attachRemoteOverCarrier(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    REQUIRE(factory.lastChannel);
    factory.lastChannel->connect();
    pumpPostedWork();
    UbiquittyTest::HostScreen host(7, 17);
    acknowledgeFixtureHost(factory, host, "predecessor.sock");
    CHECK(factory.pinnedSocket == "predecessor.sock");
    REQUIRE(factory.lastChannel);
    receiveWorkspaceStructure(factory, host);
    REQUIRE(factory.lastChannel);
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::HandingOver, .detail = {} });
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
    factory.lastChannel->receive(frame);
    pumpPostedWork();
    CHECK_EQ(factory.handoverClearCount, 1);
    CHECK(factory.pinnedSocket.empty());
    CHECK_EQ(factory.createCount, 2);
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aDirectoryOwnerChangesOnlyWhenItsReplacementAnswerArrives)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory firstFactory;
    CountingFactory secondFactory;
    UbiquittyTest::HeldWorkspace firstHeld;
    UbiquittyTest::HeldWorkspace secondHeld;
    firstHeld.name = "first owner";
    secondHeld.name = "second owner";
    UbiquittyTest::ScriptedHost firstHost;
    UbiquittyTest::ScriptedHost secondHost;
    firstHost.held = &firstHeld;
    secondHost.held = &secondHeld;
    std::map<UbiquittyTest::ScriptedHost*, std::size_t> writesConsumed;

    // Each inbound packet posts one decode. Timers are discarded because
    // this case advances protocol events without advancing any deadline.
    const auto deliver = [](CountingFactory& factory, const std::vector<std::uint8_t>& bytes) {
        if (!factory.lastChannel)
            return false;
        factory.lastChannel->receive(bytes);
        pumpPostedWork(1);
        discardPostedWork();
        return true;
    };
    const auto acknowledge = [&](CountingFactory& factory, UbiquittyTest::ScriptedHost& host, std::string socketName, std::uint64_t instanceID) {
        Sync::HostIdentity identity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = Sync::Role::Owner, .principal = "test", .mayDrive = true, .verifiedKind = Sync::ClientKind::Human, .selection = { .target = Sync::AdmissionTarget::Control, .sessionID = 0, .workspaceID = 0, .daemonInstanceID = instanceID, .socketName = std::move(socketName), .buildFlags = 0, .hostPlatform = "fixture" } };
        host.connection.emplace(UbiquittyTest::syncHostPolicy(0), std::move(identity));
        const auto address = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));
        if (!session->listHostSessions(address, queueingLoop()) || !factory.lastChannel)
            return false;
        discardPostedWork();
        factory.lastChannel->connect();
        pumpPostedWork(1);
        discardPostedWork();
        const auto writes = factory.lastChannel->sent;
        for (const auto& bytes : writes)
            host.connection->receiveStream(bytes, ++host.now);
        writesConsumed[&host] = writes.size();
        std::vector<std::uint8_t> bytes;
        host.collect(bytes, 1);
        return deliver(factory, bytes);
    };
    const auto list = [&](CountingFactory& factory, UbiquittyTest::ScriptedHost& host) {
        if (!factory.lastChannel)
            return false;
        auto& consumed = writesConsumed[&host];
        const auto receive = [&] {
            const auto writes = factory.lastChannel->sent;
            for (; consumed < writes.size(); ++consumed) {
                host.connection->receiveStream(writes[consumed], ++host.now);
            }
        };
        receive();
        const auto calls = host.connection->takeCalls();
        if (calls.size() != 1)
            return false;
        const auto request = Sync::decodeCallRequest(calls.front().text);
        if (request.outcome != Ubiquitty::Protocol::Control::RequestOutcome::Dispatch || request.method != "host.sessions.subscribe")
            return false;
        if (host.connection->answerCall(calls.front().callIndex, host.subscribed(calls.front().text)) != Sync::CallAnswer::Sent)
            return false;
        std::vector<std::uint8_t> rows;
        host.collect(rows, 2 * Sync::listedRecordsIntervalMicroseconds);
        return !rows.empty() && deliver(factory, rows);
    };

    REQUIRE(acknowledge(firstFactory, firstHost, "first.sock", 17));
    CHECK(!session->hasHostAnsweredListing());
    CHECK(session->hostSessionDaemonSocketName().empty());
    REQUIRE(list(firstFactory, firstHost));
    REQUIRE(session->hasHostAnsweredListing());
    CHECK(session->hostOutlineRows({}).title(0) == "first owner");
    CHECK(session->hostSessionDaemonSocketName() == "first.sock");
    CHECK_EQ(session->hostSessionDaemonInstanceID(), 17u);

    REQUIRE(acknowledge(secondFactory, secondHost, "second.sock", 22));
    CHECK(session->hasHostAnsweredListing());
    CHECK(session->hostOutlineRows({}).title(0) == "first owner");
    CHECK(session->hostSessionDaemonSocketName() == "first.sock");
    CHECK_EQ(session->hostSessionDaemonInstanceID(), 17u);
    REQUIRE(list(secondFactory, secondHost));
    REQUIRE(session->hasHostAnsweredListing());
    CHECK(session->hostOutlineRows({}).title(0) == "second owner");
    CHECK(session->hostSessionDaemonSocketName() == "second.sock");
    CHECK_EQ(session->hostSessionDaemonInstanceID(), 22u);

    REQUIRE(acknowledge(secondFactory, secondHost, "second.sock", 22));
    REQUIRE(secondFactory.lastChannel);
    secondFactory.lastChannel->dropIt("directory interrupted before listing");
    pumpPostedWork(1);
    discardPostedWork();
    CHECK(!session->hasHostAnsweredListing());
    CHECK(session->hostSessionDaemonSocketName().empty());
    CHECK_EQ(session->hostSessionDaemonInstanceID(), 0u);
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(neitherDoorAttachesASessionTwice)
{
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CountingFactory factory;
    const std::uintptr_t address
        = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));
    CHECK(session->attachRemoteOverCarrier(address, 0, queueingLoop()));

    const int afterFirst = factory.createCount;
    CHECK_MSG(!session->attachRemoteOverCarrier(address, 0, queueingLoop()),
        "a second delegated attach was accepted over a live one");
    // The refusal must be total: a second attach that built a carrier and then bailed would leave
    // a channel nobody owns.
    CHECK_EQ(factory.createCount, afterFirst);
    CHECK_MSG(!session->attachRemote(spawnsNothing, "", "", 0, queueingLoop()),
        "attachRemote was accepted over a live delegated session");

    releaseTerminalSession(session);
}

/// A detached remote half leaves the session attachable again.
///
/// A frontend re-homes a surface to another session, or returns to the picker
/// after a refused reattach, by detaching the remote half. Closing instead
/// disposes the terminal, and every attach after it is refused for want of one:
/// the QUIC carrier was then abandoned for SSH, whose bytes fed nothing.
TEST(aDetachedSessionAttachesAgainOnTheSameTerminal)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CountingFactory factory;
    const std::uintptr_t address
        = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));
    CHECK(session->attachRemoteOverCarrier(address, 7, queueingLoop()));
    session->detachRemote();
    pumpPostedWork();
    CHECK(!session->isRemoteAttached());
    CHECK_MSG(session->attachRemoteOverCarrier(address, 12, queueingLoop()),
        "a session whose remote half was detached refused the next attach");
    CHECK(session->terminal() != nullptr);
    session->close();
    pumpPostedWork();
    releaseTerminalSession(session);
    discardPostedWork();
}

/// A detach after a refused reattach leaves an Idle session with no refusal.
///
/// A frontend answers a refusal by detaching and going back to the picker. The
/// refusal flag and its sentence describe a Failed session; left on the Idle one
/// the detach publishes, the frontend reads the refusal again and answers it a
/// second time.
TEST(aDetachAfterARefusedReattachLeavesNoRefusalBehind)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CountingFactory factory;
    const std::uintptr_t address
        = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));
    CHECK(session->attachRemoteOverCarrier(address, 7, queueingLoop()));
    REQUIRE(factory.lastChannel != nullptr);
    factory.lastChannel->connect();
    pumpPostedWork();
    namespace Sync = Ubiquitty::Protocol::Sync;
    std::vector<std::uint8_t> body, frame;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::SessionGone });
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
    factory.lastChannel->receive(frame);
    pumpPostedWork();
    REQUIRE(session->wasReattachRefused());

    session->detachRemote();
    pumpPostedWork();
    CHECK(session->remoteState() == Ubiquitty::Session::State::Idle);
    CHECK_MSG(!session->wasReattachRefused(), "an Idle session still reads as a refused reattach");
    CHECK_MSG(session->refusedReattachSessionID() == 0, "an Idle session still names the refused session");
    CHECK_MSG(session->lastRemoteFailure().empty(), "an Idle session still carries the refusal's sentence");
    session->close();
    pumpPostedWork();
    releaseTerminalSession(session);
    discardPostedWork();
}

/// `attachRemote` keeps its own spawning factory.
///
/// The two doors share every line except which factory the core is handed. A shared path that
/// defaulted to an embedder's would attach every existing caller — the macOS app among them —
/// over a factory nobody set.
TEST(attachRemoteStillUsesItsOwnFactory)
{
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CountingFactory factory; // set up but never handed over
    CHECK(session->attachRemote(spawnsNothing, "", "", 0, queueingLoop()));
    CHECK_MSG(session->isRemoteAttached(), "attachRemote no longer attaches");
    CHECK_MSG(factory.createCount == 0,
        "attachRemote reached a factory it was never given");

    releaseTerminalSession(session);
    // This case stands a real carrier, so its channel's I/O thread posts an onConnect the case
    // never pumps. Disposed here rather than by whichever case the registry happens to run next:
    // a token owned by the case that made it survives a reordering. The release above has already
    // joined that thread, so nothing is still posting.
    discardPostedWork();
}

/// The embedder's factory is asked again on every reconnect, not once at attach.
///
/// This is the invariant a phone lives on. A seam that consulted the factory only for the first
/// carrier would attach fine in a demo and then strand the session the moment its network moved:
/// the core would have nothing to build the next carrier from. The counter has to climb.
TEST(theEmbedderFactoryIsAskedAgainOnEveryReconnect)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CountingFactory factory;
    CHECK(session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    CHECK_EQ(factory.createCount, 1);

    // The network goes away under the carrier the factory just handed over.
    // Guarded for the reason spelled out on the second drop below: CHECK records
    // and walks on, so an attach that produced no carrier would dereference null
    // here and take a signal — which ends the whole suite, not this case, and
    // takes the other cases' results with it.
    CHECK_MSG(factory.lastChannel != nullptr,
        "the attach produced no carrier, so there is nothing to drop");
    if (factory.lastChannel == nullptr) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }
    factory.lastChannel->dropIt("network changed");
    pumpPostedWork();

    CHECK_MSG(factory.createCount > 1,
        "the session did not ask the embedder's factory for a carrier after the drop");
    // Guarded rather than asserted and walked past: the channel nulls this slot when the core
    // destroys it, so a session that did not redial leaves nothing here, and dereferencing it
    // would take a signal instead of reporting the failure above.
    CHECK_MSG(factory.lastChannel != nullptr,
        "the reconnect produced no carrier, so there is nothing to drop again");
    if (factory.lastChannel != nullptr) {
        // And again, because "asked twice" and "asked every time" are different claims.
        const int afterFirstDrop = factory.createCount;
        factory.lastChannel->dropIt("network changed again");
        pumpPostedWork();
        CHECK_MSG(factory.createCount > afterFirstDrop,
            "the session stopped asking the embedder's factory after the second drop");
    }

    releaseTerminalSession(session);
    discardPostedWork();
}

// The loop still holds a session's tokens when its pane closes. Each unit
// holds what it reaches weakly, so running them then reaches nothing the
// session owned — a connect and a drop that never ran arm no reconnect — and
// every token still redeems exactly once.
TEST(workAClosedSessionPostedRunsAsNothing)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CountingFactory factory;
    CHECK(session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    CHECK_MSG(factory.lastChannel != nullptr, "the attach produced no carrier");
    if (factory.lastChannel == nullptr) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }
    factory.lastChannel->connect();
    factory.lastChannel->dropIt("network changed");
    CHECK_MSG(!postedWork().empty(), "the carrier's connect and drop posted nothing");
    const int createdBeforeClose = factory.createCount;
    const std::size_t pendingBeforeClose = Ubiquitty::Runtime::PostedWorkTable::singleton().pendingCount();
    CHECK(pendingBeforeClose >= postedWork().size());

    releaseTerminalSession(session);
    std::vector<Ubiquitty::Runtime::PostedWorkToken> leftover;
    leftover.swap(postedWork());
    for (const Ubiquitty::Runtime::PostedWorkToken token : leftover)
        CHECK(Ubiquitty::Runtime::runPostedSessionWork(token) == Ubiquitty::Runtime::PostedWorkOutcome::Ran);
    CHECK_EQ(factory.createCount, createdBeforeClose);
    for (const Ubiquitty::Runtime::PostedWorkToken token : leftover)
        CHECK(Ubiquitty::Runtime::runPostedSessionWork(token) == Ubiquitty::Runtime::PostedWorkOutcome::Refused);
    CHECK_MSG(postedWork().empty(), "work a closed session posted posted more");
    discardPostedWork();
}

// Every clear command on an attached session asks the host to run it and
// leaves the mirror's history to the frame that answers: none is refused,
// and none edits the mirror the host's next delta is computed against.
TEST(remoteClearWaitsForAuthority)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    {
        TerminalSession* session = makeStartedSession();
        CHECK(session != nullptr);
        if (!session)
            return;
        std::string text;
        for (int row = 0; row < 40; ++row)
            text += "completed output\r\n";
        text += "prompt$ typed";
        session->feedBytes(std::span<const char>(text.data(), text.size()));
        const int history = session->readSnapshot().scrollbackCount;
        CHECK(history > 0);
        CountingFactory factory;
        CHECK(session->attachRemoteOverCarrier(
            reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
            0, queueingLoop()));
        factory.lastChannel->connect();
        pumpPostedWork();
        CHECK(factory.lastChannel != nullptr);
        if (!factory.lastChannel) {
            releaseTerminalSession(session);
            discardPostedWork();
            return;
        }
        // The host binds the pane, which is what a frame to it is addressed by.
        // Its checkpoint holds the screen, and the backfill behind it the
        // history the session already shows (Docs/sync.md §6.6).
        UbiquittyTest::HostScreen host(7, 17);
        receiveWorkspaceStructure(factory, host);
        Ubiquitty::processBytes(&host.terminal(), text);
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.publish());
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        CHECK(session->readSnapshot().scrollbackCount == history);
        using Ubiquitty::Screen::ClearCommand;
        const std::array names { "buffer", "scrollback", "screen", "previous-mark" };
        std::size_t commandIndex = 0;
        std::uint64_t priorID = 0;
        for (const ClearCommand command : { ClearCommand::Buffer, ClearCommand::Scrollback, ClearCommand::Screen, ClearCommand::ToPreviousMark }) {
            CHECK(session->runClearCommand(command, nullptr));
            CHECK(session->readSnapshot().scrollbackCount == history);
            pumpPostedWork();
            host.receive(factory.lastChannel->sent);
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            CHECK(request.stringOr("method") == "pane.clear");
            CHECK(request.find("params")->numberOr("pane") == host.plane().handle());
            CHECK(request.find("params")->stringOr("command") == names[commandIndex++]);
            CHECK(request.numberOr("id") > priorID);
            priorID = static_cast<std::uint64_t>(request.numberOr("id"));
        }
        releaseTerminalSession(session);
        discardPostedWork();
    }
}

TEST(remoteWheelReadsHistoryAfterApplicationModeTransitions)
{
    using namespace Ubiquitty;
    discardPostedWork();
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    const auto exchange = [&] {
        for (int turn = 0; turn < 8; ++turn) {
            REQUIRE(factory.lastChannel);
            host.receive(factory.lastChannel->sent);
            factory.lastChannel->receive(turn == 0 ? host.publish() : host.flush());
            pumpPostedWork();
        }
    };
    const auto output = [&](std::string_view prefix) {
        for (int row = 0; row < 120; ++row)
            processBytes(&host.terminal(), std::string(prefix) + std::to_string(row) + "\r\n");
        exchange();
    };
    const Input::ScrollFact wheel { .deltaY = 5 };
    const Screen::ScrollTuning::Policy tuning;
    const auto checkHistory = [&] {
        REQUIRE(factory.lastChannel);
        CHECK(session->readSnapshot().history.retainedRows > 60);
        const auto outcome = session->scrollFact(wheel, tuning);
        CHECK(outcome.isViewportScroll);
        CHECK_EQ(outcome.linesToOffsetView, 5);
        session->lockForRead();
        const Terminal* mirror = session->terminal();
        CHECK(mirror);
        if (mirror) {
            CHECK_EQ(mirror->scrollbackCount, host.terminal().scrollbackCount);
            for (int row = 0; row < mirror->rows; ++row) {
                for (int column = 0; column < mirror->columns; ++column) {
                    const Cell* actual = Screen::visibleCell(mirror, 5, row, column);
                    const Cell* expected = Screen::visibleCell(&host.terminal(), 5, row, column);
                    CHECK_EQ(actual ? actual->codePoint : 0, expected ? expected->codePoint : 0);
                }
            }
        }
        session->unlockForRead();
    };

    output("before-");
    checkHistory();
    processBytes(&host.terminal(), "\x1b[?1000;1006h");
    exchange();
    CHECK(!session->scrollFact(wheel, tuning).isViewportScroll);
    processBytes(&host.terminal(), "\x1b[?1000;1006l");
    exchange();
    checkHistory();
    processBytes(&host.terminal(), "\x1b[?7403h");
    exchange();
    CHECK(!session->scrollFact(wheel, tuning).isViewportScroll);
    int captureOwner = 4242;
    CHECK(Mouse::retireTrackingIfStale(host.terminal(), captureOwner, 99, true));
    exchange();
    CHECK(!session->readSnapshot().mouseRelativePointerLockActive);
    checkHistory();
    processBytes(&host.terminal(), "\x1b[?1049h\x1b[?1000;1006h");
    exchange();
    CHECK_EQ(session->readSnapshot().history.retainedRows, 0);
    processBytes(&host.terminal(), "\x1b[?1000;1006l\x1b[?1049l");
    exchange();
    checkHistory();
    output("after-");
    checkHistory();
    session.reset();
    discardPostedWork();
}

TEST(remoteWheelReturnsToHistoryWhenAnInitiallyAttachedApplicationExits)
{
    using namespace Ubiquitty;
    for (const std::string_view initialModes : { "\x1b[?1000;1006h", "\x1b[?1049h\x1b[?1000;1006h", "\x1b[?7403h" }) {
        discardPostedWork();
        std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
        REQUIRE(session);
        CountingFactory factory;
        UbiquittyTest::HostScreen host(7, 17);
        REQUIRE(attachAndAcknowledge(session.get(), factory, host));
        for (int row = 0; row < 120; ++row)
            processBytes(&host.terminal(), "retained-" + std::to_string(row) + "\r\n");
        processBytes(&host.terminal(), initialModes);
        receiveWorkspaceStructure(factory, host);
        const auto exchange = [&] {
            for (int turn = 0; turn < 8; ++turn) {
                REQUIRE(factory.lastChannel);
                host.receive(factory.lastChannel->sent);
                factory.lastChannel->receive(turn == 0 ? host.publish() : host.flush());
                pumpPostedWork();
            }
        };
        exchange();
        const Input::ScrollFact wheel { .deltaY = 5 };
        const Screen::ScrollTuning::Policy tuning;
        CHECK(!session->scrollFact(wheel, tuning).isViewportScroll);
        processBytes(&host.terminal(), "\x1b[?1000;1006;7403l\x1b[?1049l");
        exchange();
        CHECK(session->readSnapshot().history.retainedRows > 60);
        const auto outcome = session->scrollFact(wheel, tuning);
        CHECK(outcome.isViewportScroll);
        CHECK_EQ(outcome.linesToOffsetView, 5);
        session.reset();
        discardPostedWork();
    }
}

namespace {

/// The viewport wants a client wrote, from one write on.
///
/// - Parameters:
///   - sent: Every write, each whole Sync frames.
///   - from: The first write read.
/// - Returns: Each `HistoryWant` at `Viewport` priority, in order.
std::vector<Ubiquitty::Protocol::Sync::HistoryWant> viewportWantsOnCarrier(const std::vector<std::vector<std::uint8_t>>& sent, std::size_t from)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    std::vector<Sync::HistoryWant> wants;
    Sync::FrameDecoder envelopes;
    for (std::size_t index = from; index < sent.size(); ++index) {
        envelopes.append(sent[index]);
        while (const auto envelope = envelopes.next()) {
            if (envelope->kind != Sync::FrameKind::HistoryWant)
                continue;
            const auto want = Sync::decodeHistoryWant(envelope->body);
            CHECK(want.has_value());
            if (want && want->priority == Sync::HistoryPriority::Viewport)
                wants.push_back(*want);
        }
    }
    CHECK(envelopes.error() == Sync::EnvelopeError::None && envelopes.bufferedBytes() == 0);
    return wants;
}

/// The text of the oldest row a session's mirror holds, under its read lock.
///
/// - Parameter session: The session.
/// - Returns: The row's ASCII, trailing blanks dropped.
std::string oldestHistoryRow(TerminalSession& session)
{
    session.lockForRead();
    std::string text;
    if (const Ubiquitty::Terminal* mirror = session.terminal()) {
        for (int column = 0; column < mirror->columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(mirror, mirror->scrollbackCount, 0, column);
            const std::uint32_t codePoint = cell ? cell->codePoint : 0;
            text.push_back(codePoint > ' ' && codePoint < 0x7f ? static_cast<char>(codePoint) : ' ');
        }
    }
    session.unlockForRead();
    return text.substr(0, text.find_last_not_of(' ') + 1);
}

} // namespace

// The user scrolls past the 2,000 lines the backfill sent. The view reaching
// the oldest row asks the host for the lines above it with one viewport want,
// however often the view moves there before they arrive, and the lines it
// asked for become the view's history (Docs/sync.md §6.6 "On demand").
TEST(aScrollPastTheBackfillWantsTheGapOnceAndFillsIt)
{
    using namespace Ubiquitty;
    namespace Sync = Protocol::Sync;
    discardPostedWork();
    // A replica deep enough to hold more than the backfill, seeded as a
    // workspace seeds a session it creates.
    Preferences::Values configuration;
    configuration.scrollbackLines = 10'000;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(TerminalSession::make(configuration), releaseTerminalSession);
    REQUIRE(session);
    session->start(24, 80);
    REQUIRE(session->terminal());
    (void)Runtime::seedTerminalConfiguration(*session->terminal(), configuration);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    setScrollbackLimit(&host.terminal(), 10'000);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    std::string lines;
    for (int line = 0; line < 3'000; ++line)
        lines += "history-" + std::to_string(line) + "\r\n";
    processBytes(&host.terminal(), lines);
    const auto exchange = [&] {
        for (int turn = 0; turn < 64; ++turn) {
            REQUIRE(factory.lastChannel);
            host.receive(factory.lastChannel->sent);
            factory.lastChannel->receive(turn == 0 ? host.publish() : host.flush());
            pumpPostedWork();
        }
    };
    exchange();
    const int backfilled = session->readSnapshot().history.retainedRows;
    CHECK_EQ(backfilled, static_cast<int>(Sync::defaultBackfillDepthLines));
    const std::string oldest = oldestHistoryRow(*session);
    REQUIRE(oldest.starts_with("history-"));
    const int oldestLine = std::stoi(oldest.substr(8));

    // At the oldest row, and on down the screen above it as the reader keeps
    // scrolling: one want, for the lines just older than those held.
    const std::size_t before = factory.lastChannel->sent.size();
    for (int offset = backfilled; offset > backfilled - 10; --offset)
        session->setPresentedViewOffset(offset);
    pumpPostedWork();
    std::vector<Sync::HistoryWant> wants = viewportWantsOnCarrier(factory.lastChannel->sent, before);
    REQUIRE(wants.size() == 1);
    REQUIRE(wants[0].ranges.size() == 1);
    const std::uint64_t first = wants[0].ranges[0].from;
    const std::uint64_t end = wants[0].ranges[0].to;
    CHECK_EQ(end - first, std::uint64_t { 48 });

    // A view nearer the tail asks for nothing.
    const std::size_t afterWant = factory.lastChannel->sent.size();
    session->setPresentedViewOffset(backfilled / 2);
    pumpPostedWork();
    CHECK(viewportWantsOnCarrier(factory.lastChannel->sent, afterWant).empty());

    // The host sends them, and they land above the rows the view had.
    exchange();
    CHECK_EQ(session->readSnapshot().history.retainedRows, backfilled + 48);
    CHECK_STR_EQ(oldestHistoryRow(*session), "history-" + std::to_string(oldestLine - 48));

    // The next gap is the next want's.
    const std::size_t afterFill = factory.lastChannel->sent.size();
    session->setPresentedViewOffset(backfilled + 48);
    pumpPostedWork();
    wants = viewportWantsOnCarrier(factory.lastChannel->sent, afterFill);
    REQUIRE(wants.size() == 1);
    CHECK((wants[0].ranges == std::vector<Sync::LineRange> { { first - 48, first } }));
    session.reset();
    discardPostedWork();
}

TEST(remotePaneChangesWaitForTheirOwnHostReply)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Outcome = TerminalSession::RemoteWorkspacePaneChange::Outcome;
    auto* session = makeStartedSession();
    REQUIRE(session);
    CountingFactory factory;
    UbiquittyTest::HostScreen structureHost(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, structureHost));
    receiveWorkspaceStructure(factory, structureHost);
    std::uint64_t structureRevision = 1;
    for (const auto wasAccepted : { false, true }) {
        CHECK(session->remoteWorkspaceAddTab());
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
        pumpPostedWork();
        structureHost.receive(factory.lastChannel->sent);
        const auto calls = structureHost.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(request.stringOr("method") == "workspace.addTab");

        CHECK(session->remoteWorkspaceRenameTab(Ubiquitty::Session::Layout::TabID { 1 }, "Working"));
        pumpPostedWork();
        structureHost.receive(factory.lastChannel->sent);
        const auto renameCalls = structureHost.plane().connection().takeCalls();
        REQUIRE(renameCalls.size() == 1);
        const auto rename = Ubiquitty::Codec::JSON::parse(renameCalls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(rename.stringOr("method") == "tab.rename");
        CHECK(structureHost.plane().connection().answerCall(renameCalls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + rename.stringOr("id") + R"(","result":{"tab":"tab:1","name":"Working"}})")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(structureHost.flush());
        pumpPostedWork();
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
        // The structure that carries the rename settles no pane change.
        receiveWorkspaceStructure(factory, structureHost, ++structureRevision, "Working");
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);

        const auto body = wasAccepted ? R"("result":{"tab":"tab:9","pane":"pane:42","session":"session:77"})"
                                      : R"("error":{"code":-32002,"message":"refused"})";
        CHECK(structureHost.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + "\"," + body + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(structureHost.flush());
        pumpPostedWork();
        const auto change = session->takeRemoteWorkspacePaneChange();
        CHECK(change.outcome == (wasAccepted ? Outcome::Accepted : Outcome::Refused));
        CHECK_EQ(change.createdPaneID, wasAccepted ? 42u : 0u);
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Idle);
    }
    for (const auto action : { TerminalSession::PaneVerb::SplitBeside, TerminalSession::PaneVerb::SplitBelow, TerminalSession::PaneVerb::Close }) {
        CHECK(session->requestPaneVerb(action) == TerminalSession::PaneVerbOutcome::Handled);
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
        pumpPostedWork();
        structureHost.receive(factory.lastChannel->sent);
        const auto calls = structureHost.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(structureHost.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","error":{"code":-32002,"message":"refused"}})")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(structureHost.flush());
        pumpPostedWork();
        CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Refused);
    }
    CHECK(session->remoteWorkspaceAddTab());
    pumpPostedWork();
    factory.lastChannel->dropIt("network lost");
    pumpPostedWork();
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Interrupted);
    releaseTerminalSession(session);
    discardPostedWork();
}

namespace {

struct WindowCloseWorkCleanup {
    ~WindowCloseWorkCleanup() { discardPostedWork(); }
};

} // namespace

TEST(remoteWindowCreationReportsRefusalAndRequiresAnotherExplicitRequest)
{
    using Outcome = TerminalSession::RemoteWorkspacePaneChange::Outcome;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWorkspaceStructure(factory, host);
    REQUIRE(session->remoteWorkspaceAddSurface());
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
    CHECK(!session->remoteWorkspaceAddSurface());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false; // bool-ok: parser output
    const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
    REQUIRE(isJSON && request.find("params"));
    CHECK(request.stringOr("method") == "workspace.addWindow");
    CHECK(request.find("params")->stringOr("expectedRevision") == "1");

    // A newer record with the same visible topology advances the authority
    // revision, but neither that record nor an unrelated answer settles this ask.
    receiveWorkspaceStructure(factory, host, 2);
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Pending);
    CHECK(host.plane().connection().answerCall(calls[0].callIndex,
              R"({"jsonrpc":"2.0","id":")" + request.stringOr("id")
                  + R"(","error":{"code":-32002,"message":"structure changed"}})")
        == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Refused);
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Idle);
    host.receive(factory.lastChannel->sent);
    CHECK(host.plane().connection().takeCalls().empty());

    REQUIRE(session->remoteWorkspaceAddSurface());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    const auto retried = host.plane().connection().takeCalls();
    REQUIRE(retried.size() == 1);
    const auto next = Ubiquitty::Codec::JSON::parse(retried[0].text, isJSON);
    REQUIRE(isJSON && next.find("params"));
    CHECK(next.stringOr("id") != request.stringOr("id"));
    CHECK(next.find("params")->stringOr("expectedRevision") == "2");
    CHECK(host.plane().connection().answerCall(retried[0].callIndex,
              R"({"jsonrpc":"2.0","id":")" + next.stringOr("id")
                  + R"(","result":{"sourceWindow":"window:1","sourcePane":"pane:1","window":"window:2","tab":"tab:2","pane":"pane:42","session":"session:77"}})")
        == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    const auto change = session->takeRemoteWorkspacePaneChange();
    CHECK(change.outcome == Outcome::Accepted && change.createdPaneID == 42);
    CHECK(session->takeRemoteWorkspacePaneChange().outcome == Outcome::Idle);
}

TEST(remoteTabClosePreservesUIUntilAuthoritySettles)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Outcome = TerminalSession::RemoteWorkspaceCloseOutcome;
    TerminalSession* session = makeStartedSession();
    CHECK(session != nullptr);
    if (!session)
        return;
    CountingFactory factory;
    CHECK(session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    factory.lastChannel->connect();
    pumpPostedWork();
    CHECK(factory.lastChannel != nullptr);
    if (!factory.lastChannel) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }
    UbiquittyTest::HostScreen structureHost(7, 17);
    receiveWorkspaceStructure(factory, structureHost);
    for (const auto wasAccepted : { false, true }) {
        CHECK(session->remoteWorkspaceCloseTab(Ubiquitty::Session::Layout::TabID { 1 }));
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Pending);
        CHECK(!session->remoteWorkspaceCloseTab(Ubiquitty::Session::Layout::TabID { 1 }));
        pumpPostedWork();
        structureHost.receive(factory.lastChannel->sent);
        const auto calls = structureHost.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(request.stringOr("method") == "workspace.closeTab");
        const auto body = wasAccepted ? R"("result":{"tab":"tab:1","sessionsEnded":1})"
                                      : R"("error":{"code":-32002,"message":"refused"})";
        CHECK(structureHost.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + "\"," + body + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(structureHost.flush());
        pumpPostedWork();
        CHECK(session->remoteWorkspaceCloseOutcome() == (wasAccepted ? Outcome::Accepted : Outcome::Refused));
        CHECK(session->remoteWorkspaceCloseOutcome() == (wasAccepted ? Outcome::Accepted : Outcome::Refused));
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == (wasAccepted ? Outcome::Accepted : Outcome::Refused));
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
        CHECK(session->remoteWorkspaceTab() == Ubiquitty::Session::Layout::TabID { 1 });
    }
    CHECK(session->requestExtraction(TerminalSession::TransferScope::Tab, TerminalSession::ExtractionPlacement::Window)
        == TerminalSession::PaneVerbOutcome::Handled);
    pumpPostedWork();
    structureHost.receive(factory.lastChannel->sent);
    const auto calls = structureHost.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false; // bool-ok: parser output
    const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
    REQUIRE(isJSON && request.find("params"));
    CHECK(request.stringOr("method") == "tab.extractWindow");
    CHECK(request.find("params")->numberOr("window") == 1 && request.find("params")->numberOr("tab") == 1);
    CHECK(request.find("params")->stringOr("expectedRevision") == "1");
    (void)session->takeEventFlags();
    CHECK(structureHost.plane().connection().answerCall(calls[0].callIndex,
              R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","error":{"code":-32002,"message":"refused"}})")
        == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
    factory.lastChannel->receive(structureHost.flush());
    pumpPostedWork();
    CHECK(session->takeEventFlags() & (1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::WorkspaceChanged)));
    CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
    CHECK(session->remoteWorkspaceTab() == Ubiquitty::Session::Layout::TabID { 1 });
    releaseTerminalSession(session);
    discardPostedWork();
}

namespace {

Ubiquitty::Session::TreeWorkspace windowCloseWorkspace(std::uint64_t revision = 1)
{
    namespace Layout = Ubiquitty::Session::Layout;
    const UbiquittyTest::StructurePane firstPanes[] { { .pane = 1, .sessionID = 7 }, { .pane = 2, .sessionID = 8 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", firstPanes, 17, revision);
    const UbiquittyTest::StructurePane hidden { .pane = 3, .sessionID = 9 };
    auto hiddenTab = UbiquittyTest::rowWorkspace(91, {}, { &hidden, 1 }, 17);
    hiddenTab.windows[0].tabs[0].id = Layout::TabID { 2 };
    hiddenTab.windows[0].tabs[0].isHostActive = false;
    workspace.windows[0].tabs.push_back(std::move(hiddenTab.windows[0].tabs[0]));
    const UbiquittyTest::StructurePane other { .pane = 4, .sessionID = 10 };
    auto otherWindow = UbiquittyTest::rowWorkspace(91, {}, { &other, 1 }, 17);
    otherWindow.windows[0].id = Layout::SurfaceID { 2 };
    otherWindow.windows[0].isHostActive = false;
    otherWindow.windows[0].tabs[0].id = Layout::TabID { 3 };
    workspace.windows.push_back(std::move(otherWindow.windows[0]));
    return workspace;
}

void receiveWindowCloseWorkspace(CountingFactory& factory, UbiquittyTest::HostScreen& host, const Ubiquitty::Session::TreeWorkspace& workspace)
{
    if (host.plane().connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Greeting)
        acknowledgeFixtureHost(factory, host);
    CHECK(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
}

/// Every emitted frame uses the current client direction and exact envelope.
bool sentFramesAreCanonical(const CountingFactory& factory)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    if (!factory.lastChannel)
        return false;
    Sync::FrameDecoder decoder;
    for (const auto& bytes : factory.lastChannel->sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (Sync::directionOf(frame->kind) == Sync::Direction::HostToClient)
                return false;
        }
    }
    return decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0;
}

} // namespace

TEST(remotePaneChangesUseTypedCallsAndExactMintedIdentities)
{
    using Change = TerminalSession::RemoteWorkspacePaneChange::Outcome;
    using Close = TerminalSession::RemoteWorkspaceCloseOutcome;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWindowCloseWorkspace(factory, host, windowCloseWorkspace());
    for (const std::string_view method : { "workspace.addTab", "workspace.split", "workspace.closeTab", "workspace.closePane" }) {
        if (method == "workspace.addTab")
            CHECK(session->remoteWorkspaceAddTab());
        else if (method == "workspace.split")
            CHECK(session->requestPaneVerb(TerminalSession::PaneVerb::SplitBeside) == TerminalSession::PaneVerbOutcome::Handled);
        else if (method == "workspace.closeTab")
            CHECK(session->remoteWorkspaceCloseTab(Ubiquitty::Session::Layout::TabID { 1 }));
        else
            CHECK(session->requestPaneVerb(TerminalSession::PaneVerb::Close) == TerminalSession::PaneVerbOutcome::Handled);
        pumpPostedWork();
        CHECK(sentFramesAreCanonical(factory));
        host.receive(factory.lastChannel->sent);
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(request.stringOr("method") == method);
        if (method == "workspace.closeTab")
            CHECK(session->takeRemoteWorkspaceCloseOutcome() == Close::Pending);
        else
            CHECK(session->takeRemoteWorkspacePaneChange().outcome == Change::Pending);
        const auto* parameters = request.find("params");
        REQUIRE(parameters);
        if (method == "workspace.addTab") {
            CHECK(parameters->numberOr("window") == 1);
            CHECK(parameters->numberOr("pane") == 1);
        } else if (method == "workspace.closeTab")
            CHECK(parameters->numberOr("tab") == 1);
        else {
            CHECK(parameters->numberOr("pane") == 1);
            if (method == "workspace.split") {
                CHECK(parameters->stringOr("orientation") == "vertical");
                CHECK(parameters->numberOr("ratio") == Ubiquitty::Panes::defaultRatio);
            }
        }
        const std::string_view result = method == "workspace.addTab" ? R"({"tab":"tab:9","pane":"pane:42","session":"session:77"})"
            : method == "workspace.split"                            ? R"({"pane":"pane:43","session":"session:78"})"
            : method == "workspace.closeTab"                         ? R"({"tab":"tab:1","sessionsEnded":2})"
                                                                     : R"({"pane":"pane:1","sessionsEnded":1})";
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + std::string(result) + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        if (method == "workspace.closeTab")
            CHECK(session->takeRemoteWorkspaceCloseOutcome() == Close::Accepted);
        else {
            const auto change = session->takeRemoteWorkspacePaneChange();
            CHECK(change.outcome == Change::Accepted);
            CHECK_EQ(change.createdPaneID, method == "workspace.addTab" ? 42u : method == "workspace.split" ? 43u
                                                                                                            : 0u);
        }
    }
    CHECK(!session->remoteWorkspaceCloseTab(Ubiquitty::Session::Layout::TabID { 3 }));
    CHECK(session->takeRemoteWorkspaceCloseOutcome() == Close::Refused);
    session.reset();
}

TEST(remoteCreationRejectsIncompleteOrForeignTypedResultIdentities)
{
    using Change = TerminalSession::RemoteWorkspacePaneChange::Outcome;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    receiveWindowCloseWorkspace(factory, host, windowCloseWorkspace());
    for (const std::string_view result : {
             R"({"tab":"tab:9","pane":"pane:0","session":"session:77"})",
             R"({"tab":"tab:9","pane":"pane:42","session":"session:4294967296"})",
             R"({"tab":"window:9","pane":"pane:42","session":"session:77"})",
             R"({"tab":"tab:9","pane":42,"session":"session:77"})",
             R"({"tab":"tab:9","pane":"pane:42"})" }) {
        REQUIRE(session->remoteWorkspaceAddTab());
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + std::string(result) + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        const auto change = session->takeRemoteWorkspacePaneChange();
        CHECK(change.outcome == Change::Refused);
        CHECK_EQ(change.createdPaneID, 0u);
    }
    session.reset();
}

TEST(remoteWindowCloseCapturesEveryTabAndConsumesOnlyItsOwnAnswer)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Decision = Ubiquitty::Session::RemoteWindowCloseDecision;
    using Outcome = TerminalSession::RemoteWorkspaceCloseOutcome;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    CHECK_EQ(session->prepareRemoteWindowClose(), 0u);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    CHECK_EQ(session->prepareRemoteWindowClose(), 0u);
    receiveWindowCloseWorkspace(factory, host, windowCloseWorkspace());
    const auto first = session->prepareRemoteWindowClose();
    REQUIRE(first != 0);
    CHECK_EQ(session->pendingRemoteWindowCloseSessionCount(), 3u);
    const auto replacement = session->prepareRemoteWindowClose();
    REQUIRE(replacement != 0 && replacement != first);
    CHECK(!session->answerRemoteWindowClose(first, Decision::CloseWindow));
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), replacement);
    CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
    CHECK(!session->answerRemoteWindowClose(replacement, Decision::Cancel));
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK_EQ(session->pendingRemoteWindowCloseSessionCount(), 0u);
    pumpPostedWork();
    CHECK(sentFramesAreCanonical(factory));
    for (const bool wasAccepted : { false, true }) {
        const auto token = session->prepareRemoteWindowClose();
        REQUIRE(token != 0);
        CHECK(session->answerRemoteWindowClose(token, Decision::CloseWindow));
        CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Pending);
        CHECK(!session->answerRemoteWindowClose(token, Decision::CloseWindow));
        CHECK_EQ(session->prepareRemoteWindowClose(), 0u);
        CHECK(!session->remoteWorkspaceCloseTab(Ubiquitty::Session::Layout::TabID { 1 }));
        pumpPostedWork();
        CHECK(sentFramesAreCanonical(factory));
        host.receive(factory.lastChannel->sent);
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false;
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(request.stringOr("method") == "window.close");
        const auto* parameters = request.find("params");
        REQUIRE(parameters);
        CHECK(parameters->numberOr("window") == 1);
        CHECK(parameters->stringOr("expectedRevision") == std::to_string(windowCloseWorkspace().revision));
        const auto id = request.stringOr("id");
        REQUIRE(id.starts_with("ubiquitty.window-close."));
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Pending);
        const auto body = wasAccepted ? R"("result":{"window":"window:1","sessionsEnded":3})"
                                      : R"("error":{"code":-32002,"message":"stale tree"})";
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + id + "\"," + body + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        factory.lastChannel->receive(host.flush());
        pumpPostedWork();
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == (wasAccepted ? Outcome::Accepted : Outcome::Refused));
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
        CHECK_EQ(session->remoteWorkspaceSurfaceCount(), 2);
        CHECK_EQ(session->remoteWorkspaceTabCount(Ubiquitty::Session::Layout::SurfaceID { 1 }), 2);
    }
    session.reset();
    discardPostedWork();
}

TEST(remoteWindowCloseRejectsChangedThenRestoredTopologyAndBindings)
{
    using Decision = Ubiquitty::Session::RemoteWindowCloseDecision;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    auto workspace = windowCloseWorkspace();
    receiveWindowCloseWorkspace(factory, host, workspace);
    const auto token = session->prepareRemoteWindowClose();
    REQUIRE(token != 0);
    workspace.revision = 2;
    workspace.windows[0].tabs[1].name = "Changed while confirming";
    receiveWindowCloseWorkspace(factory, host, workspace);
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    workspace.revision = 3;
    workspace.windows[0].tabs[1].name.clear();
    receiveWindowCloseWorkspace(factory, host, workspace);
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK(!session->answerRemoteWindowClose(token, Decision::CloseWindow));
    const auto rebound = session->prepareRemoteWindowClose();
    REQUIRE(rebound != 0);
    workspace.revision = 4;
    workspace.windows[0].tabs[1].panes[0].sessionID = 19;
    workspace.windows[0].tabs[1].split.paneID.low = 19;
    receiveWindowCloseWorkspace(factory, host, workspace);
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK(!session->answerRemoteWindowClose(rebound, Decision::CloseWindow));
    const auto moved = session->prepareRemoteWindowClose();
    REQUIRE(moved != 0);
    workspace.revision = 5;
    workspace.windows[0].id = Ubiquitty::Session::Layout::SurfaceID { 2 };
    workspace.windows[1].id = Ubiquitty::Session::Layout::SurfaceID { 1 };
    receiveWindowCloseWorkspace(factory, host, workspace);
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK(!session->answerRemoteWindowClose(moved, Decision::CloseWindow));
    pumpPostedWork();
    CHECK(sentFramesAreCanonical(factory));
    session.reset();
    discardPostedWork();
}

TEST(remoteWindowCloseRejectsARevisionChangeWithoutAMirrorChange)
{
    using Decision = Ubiquitty::Session::RemoteWindowCloseDecision;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, host));
    auto workspace = windowCloseWorkspace();
    receiveWindowCloseWorkspace(factory, host, workspace);
    const auto token = session->prepareRemoteWindowClose();
    REQUIRE(token);
    ++workspace.revision;
    receiveWindowCloseWorkspace(factory, host, workspace);
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK(!session->answerRemoteWindowClose(token, Decision::CloseWindow));
    const auto current = session->prepareRemoteWindowClose();
    REQUIRE(current);
    CHECK(session->answerRemoteWindowClose(current, Decision::CloseWindow));
    pumpPostedWork();
    CHECK(sentFramesAreCanonical(factory));
    host.receive(factory.lastChannel->sent);
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false;
    const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
    REQUIRE(isJSON && request.find("params"));
    CHECK(request.find("params")->stringOr("expectedRevision") == "2");
}

TEST(remoteWindowCloseRejectsAnAnswerForAnotherWindowOrMalformedResult)
{
    using Decision = Ubiquitty::Session::RemoteWindowCloseDecision;
    using Outcome = TerminalSession::RemoteWorkspaceCloseOutcome;
    for (const char* result : { R"({"window":"window:2","sessionsEnded":3})",
             R"({"window":"window:1","sessionsEnded":1.5})", R"({"window":"window:1","sessionsEnded":-1})",
             R"({"window":"window:1","sessionsEnded":65})", R"({"window":"window:1"})", R"("closed")" }) {
        WindowCloseWorkCleanup cleanup;
        CountingFactory factory;
        std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
        REQUIRE(session);
        UbiquittyTest::HostScreen host(7, 17);
        REQUIRE(attachAndAcknowledge(session.get(), factory, host));
        receiveWindowCloseWorkspace(factory, host, windowCloseWorkspace());
        const auto token = session->prepareRemoteWindowClose();
        REQUIRE(token && session->answerRemoteWindowClose(token, Decision::CloseWindow));
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false;
        const auto request = Ubiquitty::Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Ubiquitty::Protocol::Sync::CallAnswer::Sent);
        const auto answer = host.flush();
        factory.lastChannel->receive(answer);
        pumpPostedWork();
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Refused);
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
        factory.lastChannel->receive(answer);
        pumpPostedWork();
        CHECK(session->takeRemoteWorkspaceCloseOutcome() == Outcome::Idle);
        CHECK_EQ(session->remoteWorkspaceSurfaceCount(), 2);
    }
}

TEST(remoteWindowCloseRejectsAReplacedCarrierAndAReattachedRuntime)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Decision = Ubiquitty::Session::RemoteWindowCloseDecision;
    WindowCloseWorkCleanup cleanup;
    CountingFactory factory;
    std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(makeStartedSession(), releaseTerminalSession);
    REQUIRE(session);
    UbiquittyTest::HostScreen firstHost(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, firstHost));
    receiveWindowCloseWorkspace(factory, firstHost, windowCloseWorkspace());
    const auto lost = session->prepareRemoteWindowClose();
    REQUIRE(lost != 0);
    factory.lastChannel->dropIt("connection replaced while confirming");
    pumpPostedWork();
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    CHECK(!session->answerRemoteWindowClose(lost, Decision::CloseWindow));
    REQUIRE(factory.lastChannel);
    factory.lastChannel->connect();
    pumpPostedWork();
    REQUIRE(factory.lastChannel);
    UbiquittyTest::HostScreen nextHost(7, 17);
    receiveWindowCloseWorkspace(factory, nextHost, windowCloseWorkspace());
    const auto current = session->prepareRemoteWindowClose();
    REQUIRE(current != 0 && current != lost);
    CHECK(!session->answerRemoteWindowClose(lost, Decision::CloseWindow));
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), current);
    CHECK(sentFramesAreCanonical(factory));
    session->detachRemote();
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), 0u);
    UbiquittyTest::HostScreen lastHost(7, 17);
    REQUIRE(attachAndAcknowledge(session.get(), factory, lastHost));
    receiveWindowCloseWorkspace(factory, lastHost, windowCloseWorkspace());
    const auto reattached = session->prepareRemoteWindowClose();
    REQUIRE(reattached != 0 && reattached != current);
    CHECK(!session->answerRemoteWindowClose(current, Decision::CloseWindow));
    CHECK_EQ(session->pendingRemoteWindowCloseToken(), reattached);
    CHECK(!session->answerRemoteWindowClose(reattached, Decision::Cancel));
    pumpPostedWork();
    CHECK(sentFramesAreCanonical(factory));
    session.reset();
    discardPostedWork();
}

/// A theme reaches the program on the far side, and reaches it only there.
///
/// A window whose session lives on a host renders the host's snapshots: the
/// colours a program there chooses come from the terminal the host runs, so a
/// theme applied on this machine has to cross. It crosses as the whole palette
/// and its reset baseline — an OSC 110/111/112 after a theme change lands on
/// the theme in force — and it does not touch the mirror, which is the host's
/// to write. A client that recoloured its own mirror would show the new theme
/// for the moment it took the next snapshot to arrive and overwrite it, and
/// the program on the far side would go on drawing for the old one.
TEST(aThemeAppliedToAThinClientCrossesToTheHostAndLeavesTheMirrorAlone)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    if (!attachAndAcknowledge(session, factory, host)) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }

    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 24, 80);

    // A theme whose default background this mirror is not already holding, so
    // "the mirror did not move" is a claim with something behind it.
    int themeIndex = -1;
    session->lockForRead();
    const auto mirrorBackground
        = session->terminal() != nullptr ? session->terminal()->palette.defaultBackground : Ubiquitty::PaletteRGB {};
    session->unlockForRead();
    for (int index = 0; index < Ubiquitty::Themes::count(); ++index) {
        Ubiquitty::TermPalette themed;
        themed.seedDefaults();
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(index);
        if (theme == nullptr)
            continue;
        Ubiquitty::Themes::applyToTermPalette(*theme, themed);
        if (themed.defaultBackground.red != mirrorBackground.red) {
            themeIndex = index;
            break;
        }
    }
    CHECK_MSG(themeIndex >= 0, "no theme differs from the mirror's background");
    if (themeIndex < 0) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }

    CHECK(session->applyTheme(themeIndex));
    pumpPostedWork();

    host.receive(factory.lastChannel->sent);
    const auto& decoded = host.plane().connection().facts().themePalette;
    CHECK(decoded.has_value());
    if (decoded) {
        Ubiquitty::TermPalette themed;
        themed.seedDefaults();
        Ubiquitty::Themes::applyToTermPalette(*Ubiquitty::Themes::get(themeIndex), themed);
        CHECK(decoded->live.defaultBackground.red == themed.defaultBackground.red);
        // The reset baseline travels beside the live palette, so a program
        // reverting its colours after the switch lands on the new theme.
        CHECK(decoded->baseline.defaultBackground.red == themed.defaultBackground.red);
    }

    session->lockForRead();
    if (const Ubiquitty::Terminal* terminal = session->terminal())
        CHECK_MSG(terminal->palette.defaultBackground.red == mirrorBackground.red,
            "the client recoloured the mirror the host owns");
    session->unlockForRead();

    releaseTerminalSession(session);
    discardPostedWork();
}

/// The host's grid is the mirror's to show, and it moves no size of this
/// client's.
///
/// A thin client states the grid its drawable holds and the host decides what
/// the pane is sized to, among every client showing it. The mirror takes the
/// host's grid from its frames; the client's own size stays the one its
/// frontend reported, stated when it changes and at no other time. A frame at
/// a grid another client set therefore reaches the mirror and sends nothing
/// back, which is what keeps two clients of one pane from answering each
/// other's sizes.
TEST(aHostsGridIsTheMirrorsAndStatesNoSizeOfThisClients)
{
    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;

    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    if (!attachAndAcknowledge(session, factory, host)) {
        releaseTerminalSession(session);
        discardPostedWork();
        return;
    }
    const auto stated = [&] { return session->remoteSyncTraceReport().totals.viewportsStated; };
    const auto mirrorGrid = [&] {
        session->lockForRead();
        const Ubiquitty::Terminal* terminal = session->terminal();
        const Ubiquitty::GridSize grid { terminal ? terminal->columns : 0, terminal ? terminal->rows : 0 };
        session->unlockForRead();
        return grid;
    };

    // A reattach: the host answers with the grid its session kept.
    receiveWorkspaceStructure(factory, host);
    receiveHostScreen(factory, host, 30, 100);
    CHECK(mirrorGrid() == (Ubiquitty::GridSize { 100, 30 }));
    const std::uint64_t afterAttach = stated();

    // The person resizes the window: one statement.
    const auto settled = TerminalSession::ResizeGesture::Settled;
    session->resize(40, 120, settled, Ubiquitty::Modes::AnchorMode::Bottom);
    pumpPostedWork();
    CHECK(stated() == afterAttach + 1);
    receiveHostScreen(factory, host, 40, 120);
    CHECK(mirrorGrid() == (Ubiquitty::GridSize { 120, 40 }));
    CHECK(stated() == afterAttach + 1);

    // Another client's size applies: the mirror follows, and nothing is said.
    receiveHostScreen(factory, host, 20, 90);
    CHECK(mirrorGrid() == (Ubiquitty::GridSize { 90, 20 }));
    CHECK(stated() == afterAttach + 1);

    // The drawable still holds what it held: reporting it again says nothing.
    session->resize(40, 120, settled, Ubiquitty::Modes::AnchorMode::Bottom);
    pumpPostedWork();
    CHECK(stated() == afterAttach + 1);

    // The drawable comes to hold the host's grid: this client's size moved,
    // so the host is told, whatever grid it is at.
    session->resize(20, 90, settled, Ubiquitty::Modes::AnchorMode::Bottom);
    pumpPostedWork();
    CHECK(stated() == afterAttach + 2);

    releaseTerminalSession(session);
    discardPostedWork();
}

namespace {

/// The embedder's trust store, held by this case.
struct TrustStore {
    std::string bytes;

    static std::size_t load(void* context, const char*, std::uint8_t* buffer, std::size_t capacity)
    {
        const std::string& bytes = static_cast<TrustStore*>(context)->bytes;
        if (buffer != nullptr)
            std::memcpy(buffer, bytes.data(), std::min(capacity, bytes.size()));
        return bytes.size();
    }

    static void store(void* context, const char*, const std::uint8_t* bytes, std::size_t byteCount)
    {
        static_cast<TrustStore*>(context)->bytes.assign(reinterpret_cast<const char*>(bytes), byteCount);
    }
};

/// Attaches a session that offers a deploy to `sshHost`, and has the host
/// answer that it runs no `ubiquitty-host`.
///
/// The bundled binary is a path nothing is at, so a push that does start
/// fails reading it before anything is spawned.
///
/// - Returns: The event flags the deploy request raised.
std::uint64_t flagsAfterADeployRequest(TerminalSession* session, CountingFactory& factory, const std::string& sshHost)
{
    CHECK(session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    session->offerHostDeploy(sshHost);
    session->addDeployBinary("Linux aarch64", "/nonexistent/ubiquitty-host-for-a-test");
    (void)session->takeEventFlags();
    factory.lastChannel->connect();
    pumpPostedWork();
    const std::string sentinel = "UBIQUITTY-NEED-DEPLOY Linux aarch64\n";
    factory.lastChannel->receive(std::vector<std::uint8_t>(sentinel.begin(), sentinel.end()));
    pumpPostedWork();
    return session->takeEventFlags();
}

} // namespace

// Whether a host may receive a push is read from Core's one trust list, and
// the person's consent is written there: a host trusted from any window or
// from Connect is pushed to without a second question, and one that is not
// asks first.
TEST(aDeployReadsAndWritesCoresTrustList)
{
    using Ubiquitty::Runtime::EventKind;
    using Ubiquitty::Runtime::HostDirectory;
    const std::uint64_t confirmationPending = std::uint64_t { 1 } << static_cast<unsigned>(EventKind::DeployConfirmationPending);
    (void)Ubiquitty::Preferences::configure("[remote]\nauto_install_host = true\n", Ubiquitty::Preferences::ParseMode::Strict);
    TrustStore store;
    HostDirectory::setDeployTrustPersistence(&TrustStore::load, &TrustStore::store, &store);
    HostDirectory::trustDeploy("user@trusted.invalid");

    TerminalSession* trusted = makeStartedSession();
    if (!trusted)
        return;
    CountingFactory trustedFactory;
    const std::uint64_t trustedFlags = flagsAfterADeployRequest(trusted, trustedFactory, "user@trusted.invalid");
    CHECK_MSG(!(trustedFlags & confirmationPending), "a host in Core's trust list was asked about again");
    // The push started without asking, and settles failed on the unreadable
    // binary before the host is asked anything; waiting for it also keeps any
    // posted settle inside this case.
    const std::uint64_t deployFailed = std::uint64_t { 1 } << static_cast<unsigned>(EventKind::DeployFailed);
    bool didSettle = (trustedFlags & deployFailed) != 0;
    for (int attempt = 0; attempt < 200 && !didSettle; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pumpPostedWork();
        didSettle = (trusted->takeEventFlags() & deployFailed) != 0;
    }
    CHECK_MSG(didSettle, "a trusted host's push never ran");
    releaseTerminalSession(trusted);
    discardPostedWork();

    TerminalSession* untrusted = makeStartedSession();
    if (!untrusted)
        return;
    CountingFactory untrustedFactory;
    CHECK_MSG(flagsAfterADeployRequest(untrusted, untrustedFactory, "user@fresh.invalid") & confirmationPending,
        "a host nobody trusted was pushed to without asking");
    CHECK(!HostDirectory::isDeployTrusted("user@fresh.invalid"));
    untrusted->approveHostDeploy();
    CHECK_MSG(HostDirectory::isDeployTrusted("user@fresh.invalid"), "the consent was not recorded in Core's trust list");
    releaseTerminalSession(untrusted);
    discardPostedWork();
    HostDirectory::setDeployTrustPersistence(nullptr, nullptr, nullptr);
}

// A push reaches a shell command line, so the destination is checked where
// the push starts, not only by whoever offered it: a destination the ssh
// grammar refuses fails the push before the bundled binary is read or ssh is
// run, even from a host the person already trusted.
TEST(aDeployToADestinationSSHRefusesFailsBeforeAnythingRuns)
{
    using Ubiquitty::Runtime::EventKind;
    using Ubiquitty::Runtime::HostDirectory;
    (void)Ubiquitty::Preferences::configure("[remote]\nauto_install_host = true\n", Ubiquitty::Preferences::ParseMode::Strict);
    TrustStore store;
    HostDirectory::setDeployTrustPersistence(&TrustStore::load, &TrustStore::store, &store);
    const std::uint64_t deployFailed = std::uint64_t { 1 } << static_cast<unsigned>(EventKind::DeployFailed);
    for (const std::string destination : { "user@bad host.invalid", "-oProxyCommand=true", "host.invalid;true" }) {
        HostDirectory::trustDeploy(destination);
        TerminalSession* session = makeStartedSession();
        if (!session)
            break;
        CountingFactory factory;
        std::uint64_t flags = flagsAfterADeployRequest(session, factory, destination);
        for (int attempt = 0; attempt < 200 && !(flags & deployFailed); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pumpPostedWork();
            flags |= session->takeEventFlags();
        }
        CHECK_MSG(flags & deployFailed, "a push to a destination ssh refuses did not settle as failed");
        // The refusal names the destination; a push that got as far as the
        // binary would name the unreadable binary instead.
        CHECK_MSG(session->lastDeployFailure() == "the SSH destination is not valid",
            "a push to a destination ssh refuses went past the destination check");
        releaseTerminalSession(session);
        discardPostedWork();
        HostDirectory::revokeDeployTrust(destination);
    }
    HostDirectory::setDeployTrustPersistence(nullptr, nullptr, nullptr);
}

// The token a session holds rides its first Greet as an explicit credential, so a
// host that requires one admits the attach without a refusal round trip.
TEST(aSessionPresentsItsTokenOnTheFirstHandshake)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    if (!session)
        return;
    session->setRemoteAuthToken("token-for-the-host");
    CountingFactory factory;
    CHECK(session->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    factory.lastChannel->connect();
    pumpPostedWork();
    REQUIRE(factory.lastChannel != nullptr && !factory.lastChannel->sent.empty());
    Sync::FrameDecoder decoder;
    decoder.append(factory.lastChannel->sent.front());
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet && frame->channel == 0);
    const auto decoded = Sync::decodeGreet(frame->body);
    REQUIRE(decoded && decoded->credential);
    CHECK_STR_EQ(std::string(decoded->credential->begin(), decoded->credential->end()), "token-for-the-host");
    releaseTerminalSession(session);
    discardPostedWork();
}

// A window refused by an older host reads which side is older off its session.
// The update it asks for runs in a session that installs before it dials: the
// carrier is never asked for a channel, so nothing runs on the host the update
// is replacing, and the deploy decision still asks before pushing to a host
// nobody trusted.
TEST(anUpdateDialsNothingAndAsksAnUntrustedHostFirst)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Ubiquitty::Runtime::EventKind;
    using Ubiquitty::Runtime::HostDirectory;
    (void)Ubiquitty::Preferences::configure("[remote]\nauto_install_host = true\n", Ubiquitty::Preferences::ParseMode::Strict);
    TrustStore store;
    HostDirectory::setDeployTrustPersistence(&TrustStore::load, &TrustStore::store, &store);

    TerminalSession* refused = makeStartedSession();
    if (!refused)
        return;
    CountingFactory refusedFactory;
    CHECK(refused->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&refusedFactory)),
        0, queueingLoop()));
    refusedFactory.lastChannel->connect();
    pumpPostedWork();
    // The wire has one version, so the refusing host is a newer one whose
    // range leaves 1 out; the refusal still records the platform it runs.
    constexpr std::uint16_t newer = Sync::wireVersion + 1;
    Sync::Bye denied { .reason = Sync::ByeReason::VersionMismatch, .detail = {}, .peer = Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = newer, .appVersion = {} }, .platform = "Linux aarch64" } };
    std::vector<std::uint8_t> body, frame;
    Sync::encode(body, denied);
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
    refusedFactory.lastChannel->receive(frame);
    pumpPostedWork();
    const Ubiquitty::Session::HostRefusal refusal = refused->lastHostRefusal();
    CHECK(refusal.hasRefusal);
    CHECK(refusal.olderSide == Sync::OlderSide::Client);
    CHECK_STR_EQ(refusal.hostPlatform, "Linux aarch64");
    releaseTerminalSession(refused);
    discardPostedWork();

    TerminalSession* updating = makeStartedSession();
    if (!updating)
        return;
    updating->requestHostUpdateBeforeAttach(refusal.hostPlatform);
    CountingFactory factory;
    CHECK(updating->attachRemoteOverCarrier(
        reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)),
        0, queueingLoop()));
    updating->offerHostDeploy("user@older.invalid");
    updating->addDeployBinary("Linux aarch64", "/nonexistent/ubiquitty-host-for-a-test");
    (void)updating->takeEventFlags();
    pumpPostedWork();
    CHECK_MSG(factory.createCount == 0, "the update dialled the host it is replacing");
    const std::uint64_t confirmationPending = std::uint64_t { 1 } << static_cast<unsigned>(EventKind::DeployConfirmationPending);
    CHECK_MSG(updating->takeEventFlags() & confirmationPending, "an untrusted older host was updated without asking");
    CHECK_EQ(updating->hostUpdateLiveSessionCount(), std::size_t { 0 });
    updating->declineHostDeploy();
    pumpPostedWork();
    CHECK_MSG(factory.createCount == 0, "a declined update dialled the host anyway");
    releaseTerminalSession(updating);
    discardPostedWork();
    HostDirectory::setDeployTrustPersistence(nullptr, nullptr, nullptr);
}

namespace {

/// Walks a thin client's cursor through two blink periods on the pinned frame clock.
///
/// - Parameters:
///   - session: The attached, focused session.
///   - weights: Filled with the cursor weight at every sampled tick.
/// - Returns: Whether the schedule reported the cursor as animating at every sampled tick.
bool walkTheBlink(TerminalSession& session, std::vector<float>& weights)
{
    bool isAlwaysAnimating = true;
    for (int tick = 0; tick <= 130; tick += 5) {
        setenv("UBIQUITTY_FAKE_TICKS", std::to_string(tick).c_str(), 1);
        weights.push_back(session.frameClockFactsNow().cursorMixAlpha);
        isAlwaysAnimating = isAlwaysAnimating && session.nextAnimationDelay().hasAnimatingContent;
    }
    unsetenv("UBIQUITTY_FAKE_TICKS");
    return isAlwaysAnimating;
}

} // namespace

// A thin client's cursor blinks on the frame clock alone: no frame arrives between the host's
// screen updates, the passive terminal is focused by the surface, and nothing the coherence rule holds
// applies to a model the drain never writes. What the checkpoint does carry is the host's att610
// blink mode, and an ordinary shell clears it with xterm's `cnorm`; that does not silence the
// reader's blink, whether they stated it or left it at its default, so a frontend that offers no
// setting needs to state nothing. Only a stated `blink = false` stops it.
TEST(aThinClientCursorBlinksWhateverTheHostsMode12Says)
{
    struct Leg {
        const char* hostBytes;
        std::optional<bool> preference;
        bool shouldBlink;
    };
    constexpr std::string_view cnorm = "\033[?12l\033[?25h";
    for (const Leg leg : { Leg { "", std::nullopt, true }, Leg { cnorm.data(), std::nullopt, true }, Leg { cnorm.data(), true, true },
             Leg { cnorm.data(), false, false } }) {
        TerminalSession* session = makeStartedSession();
        REQUIRE(session != nullptr);
        Ubiquitty::Preferences::Values configuration;
        configuration.cursorBlink = leg.preference;
        session->applyTerminalPreferences(configuration);
        CountingFactory factory;
        UbiquittyTest::HostScreen host(7, 17);
        REQUIRE(attachAndAcknowledge(session, factory, host));
        receiveWorkspaceStructure(factory, host);
        Ubiquitty::processBytes(&host.terminal(), leg.hostBytes);
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.publish());
        pumpPostedWork();
        session->setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);

        std::vector<float> weights;
        const bool isAlwaysAnimating = walkTheBlink(*session, weights);
        const bool didFade = std::ranges::any_of(weights, [](float weight) { return weight < 0.5f; });
        CHECK_MSG(didFade == leg.shouldBlink, leg.shouldBlink ? "the cursor never dimmed on the frame clock" : "a stated blink = false still blinked");
        CHECK_MSG(isAlwaysAnimating == leg.shouldBlink, "the animation schedule disagreed with the cursor's weight");
        CHECK_MSG(!session->isFrameHeld(), "the coherence rule held a model only the host writes");
        releaseTerminalSession(session);
        discardPostedWork();
    }
}

// A thin client draws only the pictures the wire gives it: the host decodes an inline image into its
// own store, and the typed content identity, pixels and placement record cross on
// the pane's handle. The client's passive terminal is what its renderer reads, so the image has
// to land there, placed, with the session told to draw again — an image arrives with no cell change
// behind it, and a session that repaints on cells alone holds the frame it had.
TEST(aThinClientHoldsTheImagesItsHostPlaced)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, host));
    auto& connection = host.plane().connection();
    const auto connectionBudget = std::make_shared<Sync::BulkSourceBudget>(Sync::maxCatalogMessageBytes);
    const auto aggregateBudget = std::make_shared<Sync::BulkSourceBudget>(4 * Sync::maxCatalogMessageBytes);
    connection.setImageSourceAdmission([&connection, connectionBudget, aggregateBudget] {
        const auto retained = connection.retainedOutboundPayloadBytes();
        return Sync::BulkSourceAdmission { .connection = connectionBudget, .aggregate = aggregateBudget, .otherConnectionBytes = retained, .otherAggregateBytes = retained };
    });
    receiveWorkspaceStructure(factory, host);
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.publish());
    pumpPostedWork();
    REQUIRE(host.plane().handle() != 0);
    (void)session->takeEventFlags();

    // The quadrants the snapshot tier feeds, through the host's own OSC 1337 decode.
    Ubiquitty::processBytes(&host.terminal(),
        "\033]1337;File=inline=1;width=24px;height=24px;size=104:"
        "iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAIAAABvFaqvAAAAL0lEQVR42mO4Y6RPELXOZCCIGEYNGjVo1CC8Bql1XyKItnYyEESjBo0aNGoQXgQAtVDf3/jjW+IAAAAASUVORK5CYII=\007");
    REQUIRE(Ubiquitty::Image::stored(&host.terminal(), 0) != nullptr);
    factory.lastChannel->receive(host.publish());
    for (int step = 0; step < 8; ++step) {
        pumpPostedWork();
        host.receive(factory.lastChannel->sent);
        factory.lastChannel->receive(host.flush());
    }
    pumpPostedWork();
    CHECK(connection.counters().imageTransfersSent == 1);

    Ubiquitty::Terminal* mirror = session->terminal();
    REQUIRE(mirror != nullptr);
    bool isPlaced = false;
    for (int index = 0; index < Ubiquitty::Image::maxTerminalImageCount; ++index) {
        const Ubiquitty::Image::TerminalImage* image = Ubiquitty::Image::stored(mirror, index);
        if (!image || image->width != 24 || image->height != 24)
            continue;
        for (const Ubiquitty::Image::TerminalImagePlacement& placement : mirror->images.placements)
            isPlaced = isPlaced || (placement.used && placement.imageID == image->imageID);
    }
    CHECK_MSG(isPlaced, "the client's terminal holds no placed copy of the image the host decoded");
    const std::uint32_t screenDirty = 1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::ScreenDirty);
    CHECK_MSG((session->takeEventFlags() & screenDirty) != 0, "an image that arrived alone did not ask the session for a frame");
    releaseTerminalSession(session);
    discardPostedWork();
}

// A program on the host sizes its pictures from the winsize's pixel fields, and the host has no
// window to measure: the pixels a thin client's frontend publishes are the only ones its session
// gets. The frontend reports a pixel-only change through `setWindowSize`, which for a thin client
// has no PTY to resize and has to cross the wire instead.
TEST(aThinClientsPixelsCrossToTheHost)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    TerminalSession* session = makeStartedSession();
    REQUIRE(session != nullptr);
    CountingFactory factory;
    UbiquittyTest::HostScreen host(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, host));
    receiveWorkspaceStructure(factory, host);
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.publish());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    (void)host.plane().connection().takeViewportReports();

    session->publishCellPixelSize(9, 18, 720, 432, 740, 450);
    session->setWindowSize(80, 24, 720, 432);
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    std::optional<Ubiquitty::Protocol::Sync::Viewport> viewport;
    for (const Ubiquitty::Protocol::Sync::ViewportReport& report : host.plane().connection().takeViewportReports()) {
        if (report.handle == host.plane().handle())
            viewport = report.viewport;
    }
    CHECK_MSG(viewport.has_value(), "a pixel change on a thin client sent no Viewport");
    CHECK_MSG(viewport && viewport->columns == 80 && viewport->rows == 24, "the pixels' Viewport moved the grid");
    CHECK_MSG(viewport && viewport->cellPixelWidth == 9 && viewport->textAreaPixelWidth == 720 && viewport->drawablePixelHeight == 450,
        "the Viewport did not carry the pixels the frontend published");
    releaseTerminalSession(session);
    discardPostedWork();
}

namespace {

/// A local pane's transport whose process subtree is scripted: the walk a
/// title is read from, with nothing to read or write.
class ScriptedProcessStream final : public Ubiquitty::PAL::Stream, private Ubiquitty::PAL::ProcessFacts {
public:
    std::vector<Ubiquitty::PAL::Process::Record> records;
    std::string scriptedForegroundName;
    std::optional<Ubiquitty::PAL::ProcessID> foregroundGroup;

    int read(std::span<char>) override { return static_cast<int>(Ubiquitty::PAL::StreamStatus::Again); }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char>) override { return 0; }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int fileDescriptor() const override { return -1; }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return scriptedForegroundName; }
    bool foregroundIsShell() override { return true; }
    std::optional<Ubiquitty::PAL::ProcessID> foregroundProcessGroup() override { return foregroundGroup; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return true; }
    int gatherProcesses(std::span<Ubiquitty::PAL::Process::Record> out) override
    {
        const std::size_t count = std::min(records.size(), out.size());
        std::copy_n(records.begin(), count, out.begin());
        return static_cast<int>(count);
    }
};

struct ModelLockHost final : Ubiquitty::Runtime::DrainHost {
    void lockModel() override { }
    void unlockModel() override { }
    void onSyncFrameReadyLocked(Ubiquitty::Runtime::SyncFrameReason) override { }
};

/// The parts of a title that come from the pane's facts, joined as a tab
/// joins them.
std::string titleFromFacts(const std::string& directory, const std::string& chain, const std::string& shellCommand, const std::string& ttyName)
{
    Ubiquitty::Title::TitleShowFlags show {};
    show.showsDirectory = true;
    show.showsProcess = true;
    show.showsShellCommand = true;
    show.showsTTYName = true;
    show.showsDimensions = true;
    const std::string lead = Ubiquitty::Title::titleDirectorySegment(directory, "/Users/someone", Ubiquitty::Title::PathFormat::BasenameOnly);
    Ubiquitty::Title::TitleSegments segments {};
    segments.lead = lead;
    segments.chain = chain;
    segments.shellCommand = shellCommand;
    segments.ttyName = ttyName;
    return Ubiquitty::Title::buildRichTitle(show, segments, 148, 36, Ubiquitty::PAL::Process::TitleSeparator::Spaced);
}

} // namespace

// A pane hosted elsewhere titles exactly as the same shell would in a local
// window. The host reads its pane's facts with the reading a local runtime
// uses and sends them whole, so the login shell keeps its dash, and the shell
// and tty segments a local pane shows are there for a host pane too.
TEST(aHostPaneTitlesItsProcessExactlyAsALocalPaneDoes)
{
    namespace Process = Ubiquitty::PAL::Process;
    namespace Sync = Ubiquitty::Protocol::Sync;
    const std::string directory = "/Users/someone/project";

    // The walk of a window's shell on macOS: login(1) leads the session and
    // execs the shell with a dashed argv[0].
    Process::Record login;
    login.processID = 4100;
    login.processGroupID = 4100;
    login.name = "login";
    login.commandLine = "login -pf someone";
    login.isSessionLeader = true;
    login.user = "root";
    login.tty = "ttys012";
    Process::Record shell;
    shell.processID = 4101;
    shell.parentProcessID = 4100;
    shell.processGroupID = 4101;
    shell.name = "-zsh";
    shell.commandLine = "-zsh";
    shell.isForeground = true;
    shell.user = "someone";
    shell.tty = "ttys012";

    // Host: the daemon reads the same walk the same way and sends the record.
    const std::vector<Process::Record> walked { login, shell };
    const Ubiquitty::Session::WalkedProcessFacts facts = Ubiquitty::Session::walkedProcessFacts(walked);
    Ubiquitty::Protocol::Sync::PaneFacts record;
    record.revision = 1;
    record.currentDirectory = directory;
    record.processChain = facts.processChain;
    record.processChainWithArguments = facts.processChainWithArguments;
    record.foreground.name = "zsh";
    record.shellCommandName = facts.shellCommandName;
    record.terminalName = facts.leaderTTYName;

    discardPostedWork();
    TerminalSession* session = makeStartedSession();
    if (session == nullptr)
        return;
    CountingFactory factory;
    UbiquittyTest::HostScreen hostScreen(7, 17);
    REQUIRE(attachAndAcknowledge(session, factory, hostScreen));
    receiveWorkspaceStructure(factory, hostScreen);
    hostScreen.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(hostScreen.publish());
    pumpPostedWork();
    REQUIRE(hostScreen.plane().handle() != 0);
    // As the daemon sends it: the pane source's typed record on its handle.
    hostScreen.plane().setFacts(record);
    factory.lastChannel->receive(hostScreen.flush());
    pumpPostedWork();
    session->lockForRead();
    const std::string hostDirectory = session->currentDirectory();
    session->unlockForRead();
    const std::string hostTitle = titleFromFacts(hostDirectory, session->processChain(), session->shellCommandName(), session->ttyName());

    // Local: the runtime walks the pane's own subtree.
    Ubiquitty::Runtime::TerminalRuntime runtime;
    Ubiquitty::Runtime::RuntimeInitParameters parameters;
    parameters.rows = 36;
    parameters.columns = 148;
    runtime.init(parameters);
    auto stream = std::make_unique<ScriptedProcessStream>();
    stream->records = { login, shell };
    stream->scriptedForegroundName = "zsh"; // the executable's file name, as proc_pidpath reads it
    stream->foregroundGroup = 4101;
    auto adoption = runtime.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    ModelLockHost drainHost;
    (void)runtime.sampleProcessStateOffDrain(drainHost);
    const std::string localTitle = titleFromFacts(directory, runtime.processChain(), runtime.shellCommandName(), runtime.ttyName());

    CHECK_STR_EQ(runtime.processChain(), "-zsh");
    CHECK_STR_EQ(session->processChain(), "-zsh");
    CHECK_STR_EQ(session->processChainWithArguments(), runtime.processChainWithArguments());
    CHECK_STR_EQ(session->foregroundProcessName(), runtime.foregroundProcessName());
    CHECK_MSG(localTitle.find("-zsh") != std::string::npos, "the local title names the login shell");
    CHECK_STR_EQ(hostTitle, localTitle);
    releaseTerminalSession(session);
    discardPostedWork();
}

UBIQUITTY_TEST_MAIN()
