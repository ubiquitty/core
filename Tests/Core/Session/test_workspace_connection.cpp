// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Session/WorkspaceConnection.h"
#include "Core/Session/WorkspaceConnectionRegistry.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/ResumeRecord.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "WorkspaceStructureFixture.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>

using namespace Ubiquitty;
namespace Sync = Protocol::Sync;

struct Queue final : Session::Dispatcher {
    bool shouldRunInline { false };
    std::deque<Session::PostedWork> work;
    std::deque<Session::PostedWork> delayed;
    void post(Session::PostedWork callback) override
    {
        if (shouldRunInline) {
            std::move(callback)();
        } else {
            work.push_back(std::move(callback));
        }
    }
    void postDelayed(double, Session::PostedWork callback) override { delayed.push_back(std::move(callback)); }
    void drain()
    {
        unsigned limit = 10000;
        while (!work.empty()) {
            REQUIRE(limit--);
            auto callback = std::move(work.front());
            work.pop_front();
            std::move(callback)();
        }
    }
    void advance()
    {
        auto callbacks = std::move(delayed);
        delayed.clear();
        for (auto& callback : callbacks) {
            std::move(callback)();
        }
        drain();
    }
};
struct Wire {
    Transport::ChannelObserver* observer { nullptr };
    std::vector<std::vector<std::uint8_t>> sent;
    bool stopped { false };
    bool shouldFailNextSend { false };
    std::int64_t pending { 0 };
    std::int64_t capacity { 1024 * 1024 };
};
struct Carrier final : Transport::Channel {
    std::shared_ptr<Wire> wire;
    explicit Carrier(std::shared_ptr<Wire> wire)
        : wire(std::move(wire))
    {
    }
    bool start(Transport::ChannelObserver* observer) override
    {
        wire->observer = observer;
        observer->onConnect();
        return true;
    }
    void send(std::span<const std::uint8_t> data) override
    {
        wire->sent.emplace_back(data.begin(), data.end());
        if (std::exchange(wire->shouldFailNextSend, false)) {
            wire->observer->onClose("send failed");
        }
    }
    void stop() override
    {
        wire->stopped = true;
        wire->observer = nullptr;
    }
    Transport::Origin origin() const override { return {}; }
    Transport::Health health() const override { return wire->stopped ? Transport::Health::Closed : Transport::Health::Connected; }
    std::string description() const override { return "fake physical carrier"; }
    std::int64_t pendingOutboundBytes() const override { return wire->pending; }
    std::int64_t outboundCapacityBytes() const override { return wire->capacity; }
};
struct Factory final : Session::ChannelFactory {
    std::vector<std::shared_ptr<Wire>> wires;
    std::vector<std::string> daemonPins;
    /// The socket the carrier is pinned to, which a production factory
    /// keeps until a successor's answer clears it.
    std::string pinnedSocket;
    /// Whether a pin to another socket than `pinnedSocket` is refused, as a
    /// production factory refuses it.
    bool isPinHeld { false };
    int handoverClears { 0 };
    bool pinDaemonSocket(std::string_view name) override
    {
        daemonPins.emplace_back(name);
        if (isPinHeld && !pinnedSocket.empty() && pinnedSocket != name)
            return false;
        pinnedSocket = name;
        return true;
    }
    void clearHandoverReach() override
    {
        pinnedSocket.clear();
        ++handoverClears;
    }
    void createChannel(Session::TransportMode, Done done) override
    {
        auto wire = std::make_shared<Wire>();
        wires.push_back(wire);
        done(std::make_unique<Carrier>(wire), false);
    }
};

/// All physical writes use complete direct envelopes in the client direction.
static bool canonicalWrites(const Wire& wire)
{
    Sync::FrameDecoder decoder;
    for (const auto& bytes : wire.sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (Sync::directionOf(frame->kind) == Sync::Direction::HostToClient)
                return false;
        }
        if (decoder.error() != Sync::EnvelopeError::None)
            return false;
    }
    return decoder.bufferedBytes() == 0;
}

struct Observer final : Session::RemoteSessionObserver {
    unsigned screens { 0 };
    void onScreenChanged() override { ++screens; }
    std::vector<Protocol::Sync::PaneInputStatus> inputs;
    void onInDoubt(const Protocol::Sync::PaneInputStatus& status) override { inputs.push_back(status); }
    int workspaceChanges { 0 };
    void onWorkspaceChanged() override { ++workspaceChanges; }
    unsigned sharedWorkspaceChanges { 0 };
    void onSharedWorkspaceChanged() override { ++sharedWorkspaceChanges; }
    unsigned established { 0 };
    unsigned exits { 0 };
    unsigned reclaims { 0 };
    unsigned retirings { 0 };
    void onConnectionRetiring() override { ++retirings; }
    std::function<void()> reclaimCallback;
    void onReclaimed() override
    {
        ++reclaims;
        if (reclaimCallback)
            reclaimCallback();
    }
    unsigned titles { 0 };
    std::string lastTitle;
    std::vector<Session::Event> screenAccesses;
    void onScreenAccess(const Session::Event& event) override { screenAccesses.push_back(event); }
    unsigned presences { 0 };
    Session::PanePresenceSnapshot lastPresence;
    std::function<void()> presenceCallback;
    void onPresence(const Session::PanePresenceSnapshot& presence) override
    {
        ++presences;
        lastPresence = presence;
        if (presenceCallback)
            presenceCallback();
    }
    std::vector<Protocol::Control::PastePushReply> pastes;
    void onPastePushAck(const Protocol::Control::PastePushReply& reply) override { pastes.push_back(reply); }
    Protocol::Sync::PaneFacts lastTitleParts;
    std::function<void()> titlePartsCallback;
    void onPaneFacts(const Protocol::Sync::PaneFacts& parts) override
    {
        lastTitleParts = parts;
        if (titlePartsCallback)
            titlePartsCallback();
    }
    unsigned clipboardRequests { 0 };
    std::vector<Ubiquitty::Session::WorkspaceIntentResult> workspaceResults;
    void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& result) override { workspaceResults.push_back(result); }
    std::vector<uint32_t> consentRequests;
    void onConsentRequest(const Sync::ConsentQuestion& request) override { consentRequests.push_back(request.requestID); }
    void onClipboardRequest(const Sync::ClipboardRequestMessage&) override { ++clipboardRequests; }
    std::vector<Protocol::Control::FileReadReply> files;
    std::function<void()> fileCallback;
    void onFileData(Protocol::Control::FileReadReply&& reply) override
    {
        files.push_back(std::move(reply));
        if (fileCallback)
            fileCallback();
    }
    std::function<void()> establishedCallback;
    std::function<void()> exitCallback;
    void onSessionEstablished(uint32_t) override
    {
        ++established;
        if (establishedCallback) {
            establishedCallback();
        }
    }
    void onExit() override
    {
        ++exits;
        if (exitCallback) {
            exitCallback();
        }
    }
    std::function<void()> titleCallback;
    void onTitle(std::string_view title) override
    {
        ++titles;
        lastTitle = title;
        if (titleCallback) {
            titleCallback();
        }
    }
    std::string lastDetail;
    std::function<void(Session::State)> stateCallback;
    void onStateChanged(Session::State state, const std::string& detail) override
    {
        lastDetail = detail;
        if (stateCallback)
            stateCallback(state);
    }
};
enum class FirstPanePresence {
    Included,
    Absent
};

// The daemon's end of the physical connection's screen plane: one pane per
// session it binds, each publishing a terminal of its own, under the daemon
// instance and explicit workspace selected by this fixture.
class WorkspaceHost {
public:
    explicit WorkspaceHost(std::string socketName = "workspace-test.sock", uint32_t workspaceID = 91)
        : m_connection(Protocol::Sync::measuredHostPolicy(0), identity(std::move(socketName), workspaceID))
    {
    }

    /// The terminal a session's pane publishes, binding the pane first if
    /// the host has not.
    Terminal& pane(uint32_t sessionID)
    {
        for (auto& pane : m_panes) {
            if (pane->sessionID == sessionID)
                return pane->terminal;
        }
        return addPane(sessionID, 24, 80);
    }

    /// The handle this in-memory host bound to one session's pane.
    std::uint32_t paneHandle(std::uint32_t sessionID) const
    {
        for (const auto& pane : m_panes) {
            if (pane->sessionID == sessionID)
                return pane->handle;
        }
        return 0;
    }

    /// The bytes a session's pane's program was written.
    std::string typed(uint32_t sessionID) const
    {
        for (const auto& pane : m_panes) {
            if (pane->sessionID == sessionID)
                return pane->typed.bytes();
        }
        return {};
    }

    /// Unbinds a session's pane, as the daemon does when the session ends,
    /// and returns the frames that say so.
    std::vector<uint8_t> retirePane(uint32_t sessionID, const Wire& wire)
    {
        for (auto& pane : m_panes) {
            if (pane->sessionID == sessionID && pane->handle) {
                m_connection.unbindPane(pane->handle, 0);
                pane->handle = 0;
            }
        }
        std::erase_if(m_panes, [&](const std::unique_ptr<Pane>& pane) { return pane->sessionID == sessionID; });
        return publish(wire);
    }

    /// Binds a pane for a session and returns the terminal it publishes.
    Terminal& addPane(uint32_t sessionID, int rows, int columns)
    {
        auto pane = std::make_unique<Pane>();
        pane->sessionID = sessionID;
        resize(pane->terminal, rows, columns);
        pane->source = std::make_unique<Protocol::Sync::PaneSource>(pane->terminal, Protocol::Sync::ID128 { .low = sessionID, .high = 17 });
        pane->source->setInputWriter(pane->typed);
        m_panes.push_back(std::move(pane));
        bindWaiting();
        return m_panes.back()->terminal;
    }

    /// Reads what the client wrote on the carrier since the last call, then
    /// publishes every pane and returns what the host sends.
    std::vector<uint8_t> publish(const Wire& wire)
    {
        for (; m_consumed < wire.sent.size(); ++m_consumed)
            m_connection.receiveStream(wire.sent[m_consumed], advance());
        bindWaiting();
        // A pane publishes once on its own; after that only when a case says
        // its terminal moved, so a frame reaches the pane it is about.
        for (auto& pane : m_panes) {
            if (pane->source->generation() == 0)
                pane->source->publish(Protocol::Sync::FrameClass::Settled, m_now);
        }
        return flush();
    }

    /// Publishes a session's pane as its terminal now stands, then reads and
    /// writes as `publish` does.
    std::vector<uint8_t> publishPane(uint32_t sessionID, const Wire& wire)
    {
        for (auto& pane : m_panes) {
            if (pane->sessionID == sessionID)
                pane->source->publish(Protocol::Sync::FrameClass::Settled, m_now);
        }
        return publish(wire);
    }

    /// Resizes a session's pane as its program's window changing size does,
    /// and takes the Structural frame without sending it.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - rows: The new rows.
    ///   - columns: The new columns.
    /// - Returns: The pane's structural generation after the frame.
    std::uint64_t resizePane(uint32_t sessionID, int rows, int columns)
    {
        (void)pane(sessionID);
        for (auto& pane : m_panes) {
            if (pane->sessionID != sessionID)
                continue;
            resize(pane->terminal, rows, columns);
            pane->source->publish(Protocol::Sync::FrameClass::Structural, m_now);
            return pane->source->structuralGeneration();
        }
        return 0;
    }

    /// Sends a pane's records without publishing a screen generation.
    std::vector<uint8_t> records(uint32_t sessionID, Protocol::Sync::PaneFacts facts, const Wire& wire)
    {
        (void)pane(sessionID);
        for (auto& pane : m_panes) {
            if (pane->sessionID == sessionID)
                pane->source->setFacts(std::move(facts));
        }
        return publish(wire);
    }

    /// Borrows the fixture's real call and response owner.
    /// - Returns: The host connection.
    Protocol::Sync::HostConnection& connection() { return m_connection; }

    /// Publishes the real arbiter's owner through the normal records path.
    std::vector<uint8_t> geometryOwner(uint32_t sessionID, Protocol::Sync::ID128 attachment, const Wire& wire)
    {
        for (auto& pane : m_panes) {
            if (pane->sessionID == sessionID) {
                pane->source->geometry().noteViewport(attachment, Protocol::Sync::GeometryStanding::Driver, { .rows = 24, .columns = 80 }, advance());
                pane->source->geometry().noteTyping(attachment, Protocol::Sync::GeometryStanding::Driver, advance());
                pane->source->setFacts({});
            }
        }
        return publish(wire);
    }

    /// Asks the exact pane's frontend for clipboard types.
    /// - Parameters:
    ///   - sessionID: The session whose pane receives the request.
    ///   - requestID: A distinct pending request identity.
    ///   - wire: The carrier the client wrote to.
    /// - Returns: The host's pending frames, including the typed request.
    std::vector<uint8_t> requestClipboard(uint32_t sessionID, uint32_t requestID, const Wire& wire)
    {
        auto out = publish(wire);
        const auto pane = std::ranges::find_if(m_panes, [&](const auto& item) { return item->sessionID == sessionID; });
        CHECK(pane != m_panes.end());
        if (pane == m_panes.end())
            return out;
        CHECK(m_connection.requestClipboard((*pane)->handle, { .requestID = requestID }));
        const auto sent = flush();
        out.insert(out.end(), sent.begin(), sent.end());
        return out;
    }

    /// Services the connection and returns its direct canonical envelopes.
    std::vector<uint8_t> flush()
    {
        const uint64_t now = advance();
        m_connection.service(now);
        std::vector<uint8_t> out;
        std::vector<uint8_t> envelope;
        while (m_connection.popStreamFrame(envelope, now)) {
            out.insert(out.end(), envelope.begin(), envelope.end());
            envelope.clear();
        }
        return out;
    }

    /// Reads what the client wrote, offers a workspace's structure and
    /// returns the frames that carry it.
    ///
    /// - Parameters:
    ///   - workspace: The workspace, its revision set.
    ///   - wire: The carrier the client wrote to.
    /// - Returns: What the host sends.
    std::vector<uint8_t> structure(const Ubiquitty::Session::TreeWorkspace& workspace, const Wire& wire)
    {
        std::vector<uint8_t> out = publish(wire);
        CHECK(UbiquittyTest::offerStructure(m_connection, workspace));
        const std::vector<uint8_t> sent = flush();
        out.insert(out.end(), sent.begin(), sent.end());
        return out;
    }

    /// Reads what the client wrote, then takes the newest size its driver
    /// reported for a session's pane since the last take.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - wire: The carrier the client wrote to.
    /// - Returns: The viewport, or nothing when the client reported none.
    std::optional<Protocol::Sync::Viewport> takePaneViewport(uint32_t sessionID, const Wire& wire)
    {
        (void)pane(sessionID);
        (void)publish(wire);
        (void)publish(wire);
        std::optional<Protocol::Sync::Viewport> viewport;
        for (const Protocol::Sync::ViewportReport& report : m_connection.takeViewportReports()) {
            const bool isForSession = std::ranges::any_of(m_panes, [&](const std::unique_ptr<Pane>& pane) {
                return pane->handle == report.handle && pane->sessionID == sessionID;
            });
            if (isForSession)
                viewport = report.viewport;
        }
        return viewport;
    }

private:
    struct Pane {
        uint32_t sessionID { 0 };
        UbiquittyTest::TypedInput typed;
        Terminal terminal;
        std::unique_ptr<Protocol::Sync::PaneSource> source;
        uint32_t handle { 0 };
    };

    static Protocol::Sync::HostIdentity identity(std::string socketName, uint32_t workspaceID)
    {
        Protocol::Sync::HostIdentity identity;
        identity.hostInstanceID = { .low = 17, .high = 0 };
        identity.attachmentID = { .low = 1, .high = 17 };
        // A person who may type into the workspace's panes.
        identity.mayDrive = true;
        identity.verifiedKind = Protocol::Sync::ClientKind::Human;
        identity.selection = { .target = Sync::AdmissionTarget::Workspace,
            .sessionID = 11,
            .workspaceID = workspaceID,
            .daemonInstanceID = 17,
            .socketName = std::move(socketName),
            .hostPlatform = "fixture" };
        return identity;
    }

    void bindWaiting()
    {
        if (m_connection.state() != Protocol::Sync::ConnectionState::Attached)
            return;
        for (auto& pane : m_panes) {
            if (pane->handle)
                continue;
            pane->handle = m_connection.bindPane(*pane->source);
            if (pane->handle != 0)
                UbiquittyTest::showPane(m_connection, pane->handle, advance());
        }
    }

    uint64_t advance() { return m_now += 20'000; }

    Protocol::Sync::HostConnection m_connection;
    std::vector<std::unique_ptr<Pane>> m_panes;
    std::size_t m_consumed { 0 };
    uint64_t m_now { 1'000'000 };
};

struct Fixture {
    Queue* queue;
    Factory* factory;
    Session::WorkspaceConnection connection;
    std::unique_ptr<Terminal> firstTerm { std::make_unique<Terminal>() };
    std::unique_ptr<Terminal> secondTerm { std::make_unique<Terminal>() };
    Observer firstObserver;
    Observer secondObserver;
    Session::RemoteSessionCore first;
    Session::RemoteSessionCore second;
    // The daemon's end of the current carrier's screen plane; a reconnect
    // greets a fresh one.
    std::unique_ptr<WorkspaceHost> host;
    Fixture()
        : Fixture(std::make_unique<Queue>(), std::make_unique<Factory>())
    {
    }
    Fixture(std::unique_ptr<Queue> dispatch, std::unique_ptr<Factory> transport, std::unique_ptr<Sync::SyncClientCore> prepared = nullptr)
        : queue(dispatch.get())
        , factory(transport.get())
        , connection(std::move(dispatch), std::move(transport))
        , first(firstTerm.get(), connection, &firstObserver)
        , second(secondTerm.get(), connection, &secondObserver)
    {
        // A protected owner a platform prepared joins the physical connection
        // before its first carrier, as TerminalSession::startRemote installs it.
        if (prepared)
            CHECK(connection.connection().adoptPreparedResume(std::move(prepared)));
        first.start(0, 80, 24);
        queue->drain();
        CHECK(factory->wires.size() == 1);
        handshake();
    }
    void feed(const std::vector<uint8_t>& frame)
    {
        factory->wires.back()->observer->onData(frame);
        queue->drain();
    }
    void feedDatagram(const std::vector<uint8_t>& frame)
    {
        factory->wires.back()->observer->onDatagram(frame);
        queue->drain();
    }
    void handshake()
    {
        // The host greets, says what the workspace holds, and then binds the
        // panes it named: a structure goes ahead of the panes it first names.
        host = std::make_unique<WorkspaceHost>();
        const auto welcome = host->publish(*factory->wires.back());
        Sync::FrameDecoder decoder;
        decoder.append(welcome);
        unsigned welcomes = 0;
        while (const auto frame = decoder.next()) {
            welcomes += frame->kind == Sync::FrameKind::Welcome;
            CHECK(frame->kind != Sync::FrameKind::PaneBind);
        }
        REQUIRE(welcomes == 1);
        REQUIRE(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
        feed(welcome);
        layout(FirstPanePresence::Included);
        (void)host->pane(11);
        (void)host->pane(22);
        feed(host->publish(*factory->wires.back()));
    }
    /// Sends the workspace's structure, as the host does when its panes
    /// change: the second pane, and beside it the first unless it is absent.
    void layout(FirstPanePresence firstPanePresence, uint32_t workspaceID = 91, uint32_t firstSessionID = 11, const std::string& name = {})
    {
        std::vector<UbiquittyTest::StructurePane> panes;
        if (firstPanePresence == FirstPanePresence::Included)
            panes.push_back({ .pane = firstSessionID, .sessionID = firstSessionID, .columns = 39, .rows = 24 });
        panes.push_back({ .pane = 22, .sessionID = 22, .columns = firstPanePresence == FirstPanePresence::Included ? 40 : 80, .rows = 24 });
        feed(host->structure(UbiquittyTest::rowWorkspace(workspaceID, name, panes, 17, ++structureRevision), *factory->wires.back()));
    }
    /// Sends one canonical current roster, not a pane Catalog reply.
    void attachments(const Sync::AttachmentsList& list)
    {
        // A real host consumes client ACK/Echo frames between publications.
        // Advancing only its send side would instead test an idle timeout.
        feed(host->publish(*factory->wires.back()));
        const auto notification = Sync::attachmentsNotify(list);
        REQUIRE(notification);
        REQUIRE(host->connection().sendNotify(*notification));
        feed(host->flush());
    }
    /// The revision of the structure last sent.
    std::uint64_t structureRevision = 0;
};

static Sync::AttachmentsList rosterForSecondPane(std::uint64_t revision, const std::string& label)
{
    return { .revision = revision,
        .attachments = { { .attachmentID = { 88, 17 }, .role = Sync::AttachmentRole::Observer, .clientKind = Sync::ClientKind::Agent, .state = Sync::AttachmentState::Live, .isGeometryOwner = false, .isPrimary = false, .panes = { { 22, 17 } }, .isSelf = false, .principal = std::nullopt, .presence = Sync::AttachmentPresence { .clientID = 72, .processID = 0, .process = {}, .claimedLabel = label, .grantIdentity = "token:exact-peer", .isQUIC = true, .sharedPanes = { { { 22, 17 }, Sync::PresenceGrant::Always } } } } } };
}

TEST(aWorkspaceReclaimResultPrecedesTheCoalescedUnbindAndStructure)
{
    for (const bool isExact : { false, true }) {
        Fixture fixture;
        fixture.second.start(22, 80, 24);
        fixture.queue->drain();
        REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
        auto& wire = *fixture.factory->wires.back();
        REQUIRE(fixture.first.requestReclaim());
        fixture.queue->drain();
        (void)fixture.host->publish(wire);
        const auto calls = fixture.host->connection().takeCalls();
        REQUIRE(calls.size() == 1);
        const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
        REQUIRE(request.method == "pane.reclaim");
        const auto intent = Protocol::Control::readReclaimRequest(request.params);
        REQUIRE(intent);
        Protocol::Control::ReclaimTarget target { .handle = fixture.host->paneHandle(11), .sessionID = 11, .daemonInstanceID = 17, .hostInstanceID = { 17, 0 }, .attachmentID = { 1, 17 }, .paneID = { 11, 17 } };
        CHECK_EQ(intent->handle, target.handle);
        if (!isExact)
            ++target.attachmentID.low;
        REQUIRE(fixture.host->connection().answerCall(calls.front().callIndex,
                    Codec::JSON::serialize(Protocol::Control::reclaimReleasedResult(request.idOrNull(), target)))
            == Sync::CallAnswer::Sent);
        bool didSeeOriginalStructure = false;
        fixture.firstObserver.reclaimCallback = [&] { didSeeOriginalStructure = fixture.connection.containsSession(11); };
        // One physical read contains the exact Result, unbind, then changed
        // topology. Waiting until takeCallResults would instead report Exit.
        auto coalesced = fixture.host->retirePane(11, wire);
        const UbiquittyTest::StructurePane remaining { .pane = 22, .sessionID = 22, .columns = 80, .rows = 24 };
        const auto structure = fixture.host->structure(UbiquittyTest::rowWorkspace(91, {}, { &remaining, 1 }, 17, ++fixture.structureRevision), wire);
        coalesced.insert(coalesced.end(), structure.begin(), structure.end());
        fixture.feed(coalesced);
        CHECK_EQ(fixture.firstObserver.reclaims, isExact ? 1u : 0u);
        CHECK_EQ(fixture.firstObserver.exits, isExact ? 0u : 1u);
        CHECK(didSeeOriginalStructure == isExact);
        CHECK(!fixture.connection.containsSession(11));
        CHECK(fixture.connection.containsSession(22));
        CHECK_EQ(fixture.secondObserver.reclaims, 0u);
        CHECK_EQ(fixture.secondObserver.exits, 0u);
        CHECK_EQ(fixture.factory->wires.size(), 1u);
    }
}

// The order the iOS client depends on at its background deadline: a
// suspended workspace writes its protected record while the physical carrier
// is still open, and only then sends Bye{Suspending} and lets the carrier go.
// The store is the prepared owner's, installed through the seam the platform
// bridges use, and observes the carrier at the moment of each write.
TEST(aSuspendedPreparedWorkspaceWritesItsRecordBeforeTheByeLeaves)
{
    struct OrderingStore final : Sync::ResumeRecordStore {
        std::shared_ptr<Wire>* wire = nullptr;
        std::size_t writes = 0;
        /// The carrier's write count and openness at each record write.
        std::vector<std::pair<std::size_t, bool>> moments;
        std::optional<Sync::ResumeRecord> record;
        Sync::ResumeBindingOutcome bindWelcome(const Sync::Welcome&, Sync::ID128, const std::array<std::uint8_t, 32>&) override { return Sync::ResumeBindingOutcome::Bound; }
        Sync::ResumeWriteOutcome write(const Sync::ResumeRecord& value) override
        {
            ++writes;
            record = value;
            if (wire && *wire)
                moments.emplace_back((*wire)->sent.size(), !(*wire)->stopped);
            return Sync::ResumeWriteOutcome::Committed;
        }
        std::optional<Sync::ResumeRecord> read() override { return record; }
        Sync::ResumePurgeOutcome purge() override
        {
            record.reset();
            return Sync::ResumePurgeOutcome::Purged;
        }
    };
    auto store = std::make_shared<OrderingStore>();
    auto prepared = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
    REQUIRE(prepared->prepareResumeStore(store, { 30, 31 }, {}));
    std::shared_ptr<Wire> current;
    store->wire = &current;
    auto queue = std::make_unique<Queue>();
    auto transport = std::make_unique<Factory>();
    Factory& factory = *transport;
    Fixture fixture(std::move(queue), std::move(transport), std::move(prepared));
    REQUIRE(factory.wires.size() == 1);
    current = factory.wires.back();
    REQUIRE(fixture.first.acknowledged());
    REQUIRE(store->record.has_value());
    CHECK((store->record->clientInstance == Sync::ID128 { 30, 31 }));

    const std::size_t momentsBefore = store->moments.size();
    fixture.first.suspend();
    fixture.queue->drain();
    REQUIRE(store->moments.size() > momentsBefore);
    CHECK(current->stopped);
    // The suspension's own write finds the carrier open and nothing sent yet
    // that says goodbye; a later write, when the transport has gone, is the
    // attachment keeping the newest picture and changes nothing here.
    const auto [sentAtWrite, wasOpen] = store->moments[momentsBefore];
    CHECK(wasOpen);
    Sync::FrameDecoder before;
    for (std::size_t index = 0; index < sentAtWrite; ++index)
        before.append(current->sent[index]);
    while (const auto frame = before.next())
        CHECK(frame->kind != Sync::FrameKind::Bye);
    REQUIRE(current->sent.size() > sentAtWrite);
    Sync::FrameDecoder after;
    for (std::size_t index = sentAtWrite; index < current->sent.size(); ++index)
        after.append(current->sent[index]);
    std::optional<Sync::FrameKind> lastKind;
    std::optional<Sync::Bye> bye;
    while (const auto frame = after.next()) {
        lastKind = frame->kind;
        if (frame->kind == Sync::FrameKind::Bye)
            bye = Sync::decodeBye(frame->body);
    }
    CHECK((lastKind == std::optional { Sync::FrameKind::Bye }));
    REQUIRE(bye.has_value());
    CHECK(bye->reason == Sync::ByeReason::Suspending);
}

TEST(aGoneWorkspaceRetiresItsPreparedResumeRecord)
{
    auto store = std::make_shared<Sync::MemoryResumeRecordStore>();
    auto prepared = std::make_unique<Sync::SyncClientCore>(nullptr, nullptr);
    REQUIRE(prepared->prepareResumeStore(store, { 30, 31 }, {}));
    Fixture fixture(std::make_unique<Queue>(), std::make_unique<Factory>(), std::move(prepared));
    REQUIRE(fixture.first.acknowledged());
    REQUIRE(store->read().has_value());
    const auto original = fixture.factory->wires.back();
    original->observer->onClose("carrier lost");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    const auto pinned = fixture.factory->wires.back();
    const std::string gone = "\nUBIQUITTY-INSTANCE-GONE workspace-test.sock\n";
    for (unsigned char byte : gone) {
        REQUIRE(pinned->observer);
        pinned->observer->onData({ &byte, 1 });
        fixture.queue->drain();
    }
    REQUIRE(fixture.factory->wires.size() == 3);
    CHECK(fixture.factory->pinnedSocket.empty());
    CHECK_EQ(fixture.factory->handoverClears, 1);
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::SessionGone, .detail = "The requested host instance is no longer serving this connection." });
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
    fixture.feed(frame);
    CHECK(fixture.first.state() == Session::State::Failed);
    CHECK_STR_EQ(fixture.firstObserver.lastDetail, "Ubiquitty restarted on the host, and the saved workspace is no longer available.");
    CHECK(!store->read().has_value());
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        fixture.first.networkPathChanged();
        fixture.first.resumeAfterSuspension(60);
        fixture.queue->advance();
    }
    CHECK_EQ(fixture.factory->wires.size(), 3u);
    CHECK(fixture.factory->pinnedSocket.empty());
    CHECK(!store->read().has_value());
}

TEST(workspaceConnectionSharesCarrierAcrossPaneDetachment)
{
    Fixture fixture;
    REQUIRE(fixture.first.acknowledged());
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.second.acknowledged());
    REQUIRE(fixture.factory->wires.size() == 1);
    REQUIRE(fixture.first.workspace() == fixture.second.workspace());
    fixture.first.stop();
    fixture.second.sendInput("hello");
    fixture.queue->drain();
    REQUIRE(!fixture.factory->wires.back()->stopped);
    REQUIRE(fixture.secondObserver.established == 1);
}

TEST(workspaceConnectionKeepsStableIdentityAfterRootExit)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.layout(FirstPanePresence::Absent);
    REQUIRE(fixture.firstObserver.exits == 1);
    REQUIRE(fixture.secondObserver.exits == 0);
    REQUIRE(fixture.second.workspaceID() == 91);
    REQUIRE(!fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionRejectsForeignPane)
{
    Fixture fixture;
    fixture.second.start(666, 80, 24);
    REQUIRE(!fixture.second.acknowledged());
    REQUIRE(fixture.factory->wires.size() == 1);
}

TEST(workspaceConnectionReconnectsOnceForAllPanes)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.handshake();
    REQUIRE(fixture.first.acknowledged());
    REQUIRE(fixture.second.acknowledged());
    REQUIRE(fixture.firstObserver.established == 2);
    REQUIRE(fixture.secondObserver.established == 2);
}

TEST(workspacePanesFollowThePhysicalConnectionsAuthenticatedSuccessor)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.factory->wires.back()->observer->onClose("handover");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    std::vector<std::uint8_t> envelope;
    const auto append = [&](Protocol::Sync::FrameKind kind, const auto& message) {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, message);
        REQUIRE(Protocol::Sync::appendFrame(envelope, kind, 0, body));
    };
    Protocol::Sync::Welcome welcome;
    welcome.wireVersion = Protocol::Sync::wireVersion;
    welcome.hostInstanceID = { 23, 0 };
    welcome.attachmentID = { 2, 23 };
    welcome.resumeSecret = { 3, 23 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = 11,
        .workspaceID = 91,
        .daemonInstanceID = 23,
        .socketName = "successor.sock",
        .hostPlatform = "fixture" };
    welcome.predecessorHostInstanceID = Protocol::Sync::ID128 { 17, 0 };
    welcome.resumed = {
        { .paneID = { 11, 17 }, .outcome = Protocol::Sync::ResumeOutcome::Diffed, .inputWritten = 0 },
        { .paneID = { 22, 17 }, .outcome = Protocol::Sync::ResumeOutcome::Diffed, .inputWritten = 0 },
    };
    append(Protocol::Sync::FrameKind::Welcome, welcome);
    REQUIRE(envelope.size() > 1);
    fixture.feed({ envelope.begin(), envelope.end() - 1 });
    CHECK(!fixture.first.acknowledged());
    CHECK(!fixture.second.acknowledged());
    CHECK(fixture.connection.connection().daemonInstanceID() == 17);
    std::vector<std::uint8_t> frame { envelope.back() };
    envelope.clear();
    const std::array panes {
        UbiquittyTest::StructurePane { .pane = 11, .sessionID = 11, .columns = 39, .rows = 24 },
        UbiquittyTest::StructurePane { .pane = 22, .sessionID = 22, .columns = 40, .rows = 24 },
    };
    Protocol::Sync::WorkspaceStructureHead structure { .workspaceID = 91, .revision = ++fixture.structureRevision };
    REQUIRE(Session::encodeWorkspaceStructure(structure.record, UbiquittyTest::rowWorkspace(91, {}, panes, 17, structure.revision)));
    append(Protocol::Sync::FrameKind::WorkspaceStructure, structure);
    append(Protocol::Sync::FrameKind::PaneBind, Protocol::Sync::PaneBind { .handle = 31, .paneID = { 11, 17 } });
    append(Protocol::Sync::FrameKind::PaneBind, Protocol::Sync::PaneBind { .handle = 32, .paneID = { 22, 17 } });
    frame.insert(frame.end(), envelope.begin(), envelope.end());
    fixture.feed(frame);
    CHECK(fixture.first.acknowledged());
    CHECK(fixture.second.acknowledged());
    CHECK(fixture.first.state() == Session::State::Connected);
    CHECK(fixture.second.state() == Session::State::Connected);
    CHECK_EQ(fixture.first.daemonInstanceID(), 23u);
    CHECK_EQ(fixture.second.daemonInstanceID(), 23u);
    CHECK_EQ(fixture.firstObserver.established, 2u);
    CHECK_EQ(fixture.secondObserver.established, 2u);
    CHECK(fixture.firstObserver.lastDetail.empty());
    CHECK(fixture.secondObserver.lastDetail.empty());
    CHECK(fixture.first.requestFile("/srv/first") != 0);
    CHECK(fixture.second.requestFile("/srv/second") != 0);
}

namespace {

/// Drops the fixture's carrier as a daemon that stopped without handing over
/// does, and answers its reconnect as the host's current daemon does when it
/// restored the workspace from the stopped daemon's record: the same
/// workspace, a new daemon instance, and fresh sessions and panes laid out
/// in the places the record kept.
///
/// - Parameters:
///   - fixture: A connected fixture whose panes are 11 and 22, in that order.
///   - restored: The restored workspace's panes, in the record's order.
///   - workspaceID: The id the restoring host lists the workspace under.
void reconnectToRestoredWorkspace(Fixture& fixture, std::span<const UbiquittyTest::StructurePane> restored, std::uint32_t workspaceID = 91)
{
    fixture.factory->wires.back()->observer->onClose("daemon stopped");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    std::vector<std::uint8_t> envelope;
    const auto append = [&](Protocol::Sync::FrameKind kind, const auto& message) {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, message);
        REQUIRE(Protocol::Sync::appendFrame(envelope, kind, 0, body));
    };
    Protocol::Sync::Welcome welcome;
    welcome.wireVersion = Protocol::Sync::wireVersion;
    welcome.hostInstanceID = { 29, 0 };
    welcome.attachmentID = { 2, 29 };
    welcome.resumeSecret = { 3, 29 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = restored.front().sessionID,
        .workspaceID = workspaceID,
        .daemonInstanceID = 29,
        .socketName = "current.sock",
        .hostPlatform = "fixture",
        .restoredFromDaemonInstanceID = 17 };
    append(Protocol::Sync::FrameKind::Welcome, welcome);
    auto workspace = UbiquittyTest::rowWorkspace(workspaceID, {}, restored, 29, ++fixture.structureRevision);
    workspace.state = Session::TreeWorkspaceState::RestoredAfterHostRestart;
    Protocol::Sync::WorkspaceStructureHead structure { .workspaceID = workspaceID, .revision = fixture.structureRevision };
    REQUIRE(Session::encodeWorkspaceStructure(structure.record, workspace));
    append(Protocol::Sync::FrameKind::WorkspaceStructure, structure);
    std::uint32_t handle = 40;
    for (const UbiquittyTest::StructurePane& pane : restored)
        append(Protocol::Sync::FrameKind::PaneBind, Protocol::Sync::PaneBind { .handle = ++handle, .paneID = { pane.sessionID, 29 } });
    fixture.feed(envelope);
}

} // namespace

/// A daemon that stopped without handing over is replaced by the host's
/// current one, which restores the workspace from its record under ids of
/// its own. Every pane of the connection stays the pane it was, attached to
/// the session restored in its place, whatever numbers the two hosts gave:
/// here the first pane's restored session has the second pane's old number.
TEST(workspacePanesStayThePanesTheyWereWhenTheHostRestoresTheirWorkspace)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    REQUIRE(fixture.first.sessionID() == 11 && fixture.second.sessionID() == 22);
    const std::array restored {
        UbiquittyTest::StructurePane { .pane = 5, .sessionID = 22, .columns = 39, .rows = 24 },
        UbiquittyTest::StructurePane { .pane = 6, .sessionID = 33, .columns = 40, .rows = 24 },
    };
    reconnectToRestoredWorkspace(fixture, restored);

    CHECK_EQ(fixture.connection.connection().daemonInstanceID(), 29u);
    CHECK_EQ(fixture.firstObserver.exits, 0u);
    CHECK_EQ(fixture.secondObserver.exits, 0u);
    CHECK(fixture.first.acknowledged());
    CHECK(fixture.second.acknowledged());
    CHECK(fixture.first.state() == Session::State::Connected);
    CHECK(fixture.second.state() == Session::State::Connected);
    CHECK_EQ(fixture.first.sessionID(), 22u);
    CHECK_EQ(fixture.second.sessionID(), 33u);
    CHECK_EQ(fixture.firstObserver.established, 2u);
    CHECK_EQ(fixture.secondObserver.established, 2u);
    CHECK_EQ(fixture.first.workspaceID(), 91u);
    CHECK(fixture.connection.connection().paneForSession(22) == Session::Layout::PaneID { 5 });
    CHECK(fixture.connection.connection().paneForSession(33) == Session::Layout::PaneID { 6 });
    CHECK(fixture.first.requestFile("/srv/first") != 0);
    CHECK(fixture.second.requestFile("/srv/second") != 0);

    // A frontend keeping views by the mirror's ids is told where each went, once.
    const Session::Layout::Restoration restoration = fixture.first.takeRestoration();
    CHECK(restoration.isRestored);
    REQUIRE(restoration.panes.size() == 2);
    CHECK(restoration.panes[0].formerPane == Session::Layout::PaneID { 11 });
    CHECK(restoration.panes[0].pane == Session::Layout::PaneID { 5 });
    CHECK_EQ(restoration.panes[0].formerSessionID, 11u);
    CHECK_EQ(restoration.panes[0].sessionID, 22u);
    CHECK(restoration.panes[1].formerPane == Session::Layout::PaneID { 22 });
    CHECK(restoration.panes[1].pane == Session::Layout::PaneID { 6 });
    CHECK(!fixture.second.takeRestoration().isRestored);
}

/// The host that restored the workspace already held a workspace under its
/// id, as two hosts that each number their first workspace 1 do, and lists
/// the restored one under another. The connection keeps its panes, holds the
/// workspace by the new id, and asks the restoring instance for that id when
/// it reconnects: the id the reference held is another workspace's there.
TEST(aWorkspaceRestoredUnderAnotherIDKeepsItsPanesAndIsAskedForByThatID)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    REQUIRE(fixture.first.workspaceID() == 91);
    const std::array restored {
        UbiquittyTest::StructurePane { .pane = 5, .sessionID = 33, .columns = 39, .rows = 24 },
        UbiquittyTest::StructurePane { .pane = 6, .sessionID = 34, .columns = 40, .rows = 24 },
    };
    reconnectToRestoredWorkspace(fixture, restored, 92);

    CHECK_EQ(fixture.connection.connection().daemonInstanceID(), 29u);
    CHECK(fixture.first.state() == Session::State::Connected);
    CHECK(fixture.second.state() == Session::State::Connected);
    CHECK_EQ(fixture.firstObserver.exits, 0u);
    CHECK_EQ(fixture.secondObserver.exits, 0u);
    CHECK_EQ(fixture.first.sessionID(), 33u);
    CHECK_EQ(fixture.second.sessionID(), 34u);
    CHECK_EQ(fixture.first.workspaceID(), 92u);
    CHECK_EQ(fixture.second.workspaceID(), 92u);
    CHECK(fixture.connection.connection().paneForSession(34) == Session::Layout::PaneID { 6 });
    CHECK_EQ(fixture.first.takeRestoration().panes.size(), 2u);

    fixture.factory->wires.back()->observer->onClose("dropped");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 3);
    Sync::FrameDecoder decoder;
    std::optional<Sync::Greet> hello;
    for (const auto& bytes : fixture.factory->wires.back()->sent) {
        decoder.append(bytes);
        while (const auto outgoing = decoder.next()) {
            if (outgoing->kind == Sync::FrameKind::Greet)
                hello = Sync::decodeGreet(outgoing->body);
        }
    }
    REQUIRE(hello);
    CHECK_EQ(hello->request.expectedDaemonInstanceID, 29u);
    CHECK_EQ(hello->request.workspaceID, 92u);
    CHECK(hello->request.target == Sync::AdmissionTarget::Workspace);
}

/// A restored workspace whose arrangement is not the one the connection held
/// pairs no pane, and every pane of the stopped daemon ends, including one
/// whose old session number the restoring host gave a pane of its own.
TEST(workspacePanesTheRestorationDoesNotPairEndEvenWhereTheirNumbersReturn)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    const std::array restored {
        UbiquittyTest::StructurePane { .pane = 5, .sessionID = 11, .columns = 80, .rows = 24 },
    };
    reconnectToRestoredWorkspace(fixture, restored);

    CHECK_EQ(fixture.firstObserver.exits, 1u);
    CHECK_EQ(fixture.secondObserver.exits, 1u);
    CHECK_EQ(fixture.firstObserver.established, 1u);
    const Session::Layout::Restoration restoration = fixture.first.takeRestoration();
    CHECK(restoration.isRestored);
    CHECK(restoration.panes.empty());
}

// A reference to the holder's instance, answered on the holder's socket by
// the host it handed the workspace to: the window takes the answer as its
// workspace, and its carrier's pin moves to the socket that answered rather
// than refusing it as another host's.
TEST(aWorkspaceHeldNowByAnotherHostMovesTheCarrierPinToItsSocket)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    REQUIRE(fixture.factory->pinnedSocket == "workspace-test.sock");
    fixture.factory->isPinHeld = true;
    fixture.factory->wires.back()->observer->onClose("the holder handed over");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    std::vector<std::uint8_t> envelope;
    const auto append = [&](Protocol::Sync::FrameKind kind, const auto& message) {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, message);
        REQUIRE(Protocol::Sync::appendFrame(envelope, kind, 0, body));
    };
    Protocol::Sync::Welcome welcome;
    welcome.wireVersion = Protocol::Sync::wireVersion;
    welcome.hostInstanceID = { 23, 0 };
    welcome.attachmentID = { 2, 23 };
    welcome.resumeSecret = { 3, 23 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = 11,
        .workspaceID = 91,
        .daemonInstanceID = 23,
        .socketName = "successor.sock",
        .hostPlatform = "fixture",
        .restoredFromDaemonInstanceID = 17 };
    append(Protocol::Sync::FrameKind::Welcome, welcome);
    const std::array panes {
        UbiquittyTest::StructurePane { .pane = 11, .sessionID = 11, .columns = 39, .rows = 24 },
        UbiquittyTest::StructurePane { .pane = 22, .sessionID = 22, .columns = 40, .rows = 24 },
    };
    Protocol::Sync::WorkspaceStructureHead structure { .workspaceID = 91, .revision = ++fixture.structureRevision };
    REQUIRE(Session::encodeWorkspaceStructure(structure.record, UbiquittyTest::rowWorkspace(91, {}, panes, 17, structure.revision)));
    append(Protocol::Sync::FrameKind::WorkspaceStructure, structure);
    append(Protocol::Sync::FrameKind::PaneBind, Protocol::Sync::PaneBind { .handle = 31, .paneID = { 11, 17 } });
    append(Protocol::Sync::FrameKind::PaneBind, Protocol::Sync::PaneBind { .handle = 32, .paneID = { 22, 17 } });
    fixture.feed(envelope);
    CHECK(fixture.first.state() == Session::State::Connected);
    CHECK(fixture.second.state() == Session::State::Connected);
    CHECK_EQ(fixture.first.daemonInstanceID(), 23u);
    CHECK_EQ(fixture.second.daemonInstanceID(), 23u);
    // The workspace's sessions run on: each pane keeps its own.
    CHECK_EQ(fixture.first.sessionID(), 11u);
    CHECK_EQ(fixture.second.sessionID(), 22u);
    CHECK_EQ(fixture.firstObserver.exits, 0u);
    CHECK_EQ(fixture.secondObserver.exits, 0u);
    CHECK(fixture.firstObserver.lastDetail.empty());
    CHECK(fixture.secondObserver.lastDetail.empty());
    CHECK(fixture.factory->pinnedSocket == "successor.sock");
    CHECK(fixture.factory->handoverClears >= 1);
}

TEST(fileDownloadConsentStaysOnItsPaneAndExpiresWithTheWorkspaceCarrier)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    using Decision = Session::FileDownloadDecision;
    auto& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    REQUIRE(fixture.host->connection().takeCalls().empty());
    CHECK(fixture.second.prepareFileDownload(1, "host.example", "/srv/second.txt"));
    fixture.feed(fixture.host->publish(wire));
    CHECK(fixture.host->connection().takeCalls().empty());
    CHECK(fixture.first.answerFileDownload(1, Decision::Download) == 0);
    fixture.feed(fixture.host->publish(wire));
    CHECK(fixture.host->connection().takeCalls().empty());
    CHECK(fixture.second.answerFileDownload(1, Decision::Download) != 0);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool valid = false;
    const auto request = Codec::JSON::parse(calls.front().text, valid);
    REQUIRE(valid && request.find("params"));
    CHECK(request.stringOr("method") == "file.read");
    CHECK(request.find("params")->numberOr("pane") == fixture.host->paneHandle(22));
    CHECK(request.find("params")->numberOr("pane") != fixture.host->paneHandle(11));
    CHECK(request.find("params")->stringOr("path") == "/srv/second.txt");
    CHECK(fixture.second.answerFileDownload(1, Decision::Download) == 0);
    fixture.feed(fixture.host->publish(wire));
    CHECK(fixture.host->connection().takeCalls().empty());

    CHECK(fixture.second.prepareFileDownload(2, "host.example", "/srv/stale.txt"));
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    CHECK(!fixture.second.pendingFileDownload());
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    REQUIRE(fixture.factory->wires.back()->observer);
    fixture.handshake();
    CHECK(fixture.second.answerFileDownload(2, Decision::Download) == 0);
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(fixture.host->connection().takeCalls().empty());
    CHECK(fixture.second.prepareFileDownload(3, "host.example", "/srv/closed.txt"));
    fixture.second.stop();
    CHECK(fixture.second.answerFileDownload(3, Decision::Download) == 0);
}

TEST(fileDownloadDoesNotSurviveASynchronousCarrierClose)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    auto& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    REQUIRE(fixture.host->connection().takeCalls().empty());
    using Decision = Session::FileDownloadDecision;
    REQUIRE(fixture.first.prepareFileDownload(1, "host.example", "/srv/refused.txt"));
    unsigned closures = 0;
    fixture.firstObserver.stateCallback = [&](Session::State state) {
        if (state != Session::State::Reconnecting)
            return;
        ++closures;
        fixture.first.stop();
    };
    fixture.queue->shouldRunInline = true;
    wire.shouldFailNextSend = true;
    CHECK(fixture.first.answerFileDownload(1, Decision::Download) == 0);
    CHECK(closures == 1);
    CHECK(!fixture.first.pendingFileDownload());
    CHECK(fixture.first.answerFileDownload(1, Decision::Download) == 0);
    CHECK(fixture.firstObserver.files.empty());
    fixture.firstObserver.stateCallback = {};
}

TEST(fileReadRepliesStayOnTheirOriginalWorkspaceEndpoint)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    const auto first = fixture.first.requestFile("/srv/first");
    const auto second = fixture.second.requestFile("/srv/second");
    REQUIRE(first && second);
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 2);
    for (unsigned index : { 1u, 0u }) {
        bool valid = false;
        const auto request = Codec::JSON::parse(calls[index].text, valid);
        REQUIRE(valid && request.find("params"));
        const auto* parameters = request.find("params");
        auto result = Codec::JSON::Value::object();
        result.set("pane", *parameters->find("pane"));
        result.set("path", *parameters->find("path"));
        result.set("status", Codec::JSON::Value::string("ok"));
        result.set("bytes", Codec::JSON::Value::number(1));
        result.set("data", Codec::JSON::Value::string(index ? "Yg==" : "YQ=="));
        auto answer = Codec::JSON::Value::object();
        answer.set("jsonrpc", Codec::JSON::Value::string("2.0"));
        answer.set("id", *request.find("id"));
        answer.set("result", std::move(result));
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
        if (!index)
            fixture.first.stop();
        fixture.feed(fixture.host->flush());
    }
    CHECK(fixture.firstObserver.files.empty());
    REQUIRE(fixture.secondObserver.files.size() == 1);
    const auto& reply = fixture.secondObserver.files.front();
    CHECK(reply.requestID == second && reply.path == "/srv/second");
    REQUIRE(reply.data().size() == 1);
    CHECK(reply.data()[0] == 'b');
}

/// A wake reported to every window of a workspace redials its connection once.
///
/// The Mac and iOS tell each window, and a workspace's windows share one
/// connection. Acting on each repeat would replace the carrier the first had
/// just started, spawning a bootstrap login per window and killing all but the
/// last — enough stacked logins on port 22 to trip sshd's MaxStartups.
TEST(workspaceConnectionActsOnceOnAReportEveryPaneRelays)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.factory->wires.size() == 1);
    fixture.first.resumeAfterSuspension(-1);
    fixture.second.resumeAfterSuspension(-1);
    // The stream connection is asked to answer once, and redialled once when
    // the host stays silent past the deadline.
    CHECK_MSG(fixture.factory->wires.size() == 1, "a wake redialled a stream connection before asking it to answer");
    fixture.queue->advance();
    CHECK_MSG(fixture.factory->wires.size() == 2, "a wake relayed by two panes redialled more than once");
    fixture.first.networkPathChanged();
    fixture.second.networkPathChanged();
    CHECK_MSG(fixture.factory->wires.size() == 2, "a path change cancelled the unanswered bootstrap");
    fixture.queue->drain();
    fixture.first.resumeAfterSuspension(-1);
    CHECK_MSG(fixture.factory->wires.size() == 2, "a wake in a later turn cancelled the unanswered bootstrap");
    fixture.handshake();
    fixture.first.resumeAfterSuspension(-1);
    fixture.queue->advance();
    CHECK_MSG(fixture.factory->wires.size() == 3, "a Welcome did not permit the next lifecycle recovery");
}

TEST(screenAccessRoutesToItsPaneAndWaitsForItsEndpoint)
{
    Fixture fixture;
    const auto publish = [&](std::uint32_t sessionID, std::uint64_t cursor) {
        Session::Event event;
        event.kind = Session::EventKind::ScreenRead;
        event.sessionID = sessionID;
        event.cursor = cursor;
        event.count = 1700000000000;
        event.text = "claimed reader";
        const auto notification = Protocol::Control::eventsNotify(std::span(&event, 1));
        REQUIRE(notification);
        REQUIRE(fixture.host->connection().sendNotify(*notification));
        fixture.feed(fixture.host->flush());
    };
    for (std::uint64_t cursor = 1; cursor <= 40; ++cursor)
        publish(22, cursor);
    CHECK(fixture.firstObserver.screenAccesses.empty());
    CHECK(fixture.secondObserver.screenAccesses.empty());
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.secondObserver.screenAccesses.size() == 32);
    CHECK(fixture.secondObserver.screenAccesses.front().cursor == 9);
    CHECK(fixture.secondObserver.screenAccesses.back().cursor == 40);
    publish(11, 41);
    REQUIRE(fixture.firstObserver.screenAccesses.size() == 1);
    CHECK(fixture.firstObserver.screenAccesses.front().sessionID == 11);
    publish(22, 42);
    CHECK(fixture.secondObserver.screenAccesses.back().cursor == 42);
    publish(999, 43);
    CHECK(fixture.firstObserver.screenAccesses.size() == 1);
    CHECK(fixture.secondObserver.screenAccesses.size() == 33);
}

TEST(workspaceConnectionDeliversPendingPaneFrames)
{
    Fixture fixture;
    auto roster = rosterForSecondPane(7, "exact peer");
    fixture.attachments(roster);
    REQUIRE(!fixture.secondObserver.presences);
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.secondObserver.presences == 1);
    REQUIRE(fixture.secondObserver.lastPresence.attendees.size() == 1);
    const auto& peer = fixture.secondObserver.lastPresence.attendees.front();
    CHECK(peer.attachmentID == (Sync::ID128 { 88, 17 }));
    CHECK(peer.label == "exact peer" && peer.grantIdentity == "token:exact-peer");
    CHECK(peer.grantScope == Session::grantAlways);
    fixture.attachments(roster);
    CHECK(fixture.secondObserver.presences == 1); // one current revision, not a duplicate presentation
    ++roster.revision;
    roster.attachments.front().presence->claimedLabel = "new label";
    fixture.attachments(roster);
    REQUIRE(fixture.secondObserver.presences == 2);
    CHECK(fixture.secondObserver.lastPresence.attendees.front().label == "new label");
    REQUIRE(fixture.firstObserver.presences == 0);
}

TEST(anEmptyCanonicalRosterClearsAlreadyPresentedPanePresence)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.attachments(rosterForSecondPane(7, "exact peer"));
    REQUIRE(fixture.secondObserver.presences == 1);
    REQUIRE(fixture.secondObserver.lastPresence.attendees.size() == 1);
    fixture.attachments({ .revision = 8, .attachments = {} });
    CHECK(fixture.secondObserver.presences == 2);
    CHECK(fixture.secondObserver.lastPresence.attendees.empty());
}

TEST(aPresenceReplayCanDestroyItsWorkspaceEndpoint)
{
    Fixture fixture;
    fixture.attachments(rosterForSecondPane(7, "exact peer"));
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(fixture.secondTerm.get(), fixture.connection, &observer);
    observer.presenceCallback = [&] { endpoint.reset(); };
    endpoint->start(22, 80, 24);
    CHECK(!endpoint);
    CHECK(observer.presences == 1);
    CHECK(!fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionRoutesWindowCreationToItsExactRequester)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    std::array<std::uint32_t, 2> requests {};
    for (unsigned index = 0; index < requests.size(); ++index) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddSurface;
        verb.surfaceID = 1;
        verb.paneID = index ? 22 : 11;
        requests[index] = (index ? fixture.second : fixture.first).sendWorkspaceVerb(verb);
        REQUIRE(requests[index]);
    }
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 2);
    for (unsigned index : { 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        CHECK(request.stringOr("method") == "workspace.addWindow");
        CHECK(request.find("params")->numberOr("pane") == (index ? 22 : 11));
        CHECK(request.find("params")->stringOr("expectedRevision") == std::to_string(fixture.structureRevision));
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id")
                      + R"(","result":{"sourceWindow":"window:1","sourcePane":"pane:)" + (index ? "22" : "11")
                      + R"(","window":"window:2","tab":"tab:2","pane":"pane:33","session":"session:33"}})")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        const auto& replies = (index ? fixture.secondObserver : fixture.firstObserver).workspaceResults;
        REQUIRE(replies.size() == 1);
        CHECK(replies.front().requestID == requests[index] && replies.front().wasPerformed);
        CHECK(replies.front().surfaceID == 2 && replies.front().tabID == 2 && replies.front().paneID == 33 && replies.front().sessionID == 33);
    }
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionRoutesSizingRepliesToTheirExactRequesters)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    std::array<std::uint32_t, 3> requests {};
    const std::array operations { Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab, Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio, Ubiquitty::Session::WorkspaceIntentKind::SetDividerRatio };
    REQUIRE(fixture.first.workspace());
    const auto* tree = fixture.first.workspace()->paneTree(Session::Layout::SurfaceID { 1 }, Session::Layout::TabID { 1 });
    REQUIRE(tree);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operations[index];
        verb.surfaceID = verb.tabID = 1;
        verb.paneID = 22;
        verb.direction = std::to_underlying(Panes::Direction::Right);
        verb.splitIndex = static_cast<std::uint16_t>(tree->root());
        verb.ratio = 0.25f;
        requests[index] = (index == 1 ? fixture.second : fixture.first).sendWorkspaceVerb(verb);
        REQUIRE(requests[index]);
    }
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 3);
    for (std::size_t index : { 2u, 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        const auto& parameters = *request.find("params");
        CHECK(parameters.numberOr("window") == 1 && parameters.numberOr("tab") == 1);
        CHECK(parameters.stringOr("expectedRevision") == std::to_string(fixture.structureRevision));
        CHECK(request.stringOr("method") == (index == 0 ? "tab.balance" : index == 1 ? "pane.adjustRatio"
                                                                                     : "divider.setRatio"));
        if (index == 1)
            CHECK(parameters.numberOr("pane") == 22 && parameters.stringOr("direction") == "right" && parameters.numberOr("step") == 0.25);
        if (index == 2) {
            REQUIRE(parameters.find("path"));
            CHECK(parameters.find("path")->isArray() && parameters.find("path")->items().empty() && parameters.numberOr("ratio") == 0.25);
        }
        const std::string result = index == 0 ? R"({"window":"window:1","tab":"tab:1"})"
            : index == 1                      ? R"({"window":"window:1","tab":"tab:1","pane":"pane:22","direction":"right","step":0.25})"
                                              : R"({"window":"window:1","tab":"tab:1","path":[],"ratio":0.25})";
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        const auto& replies = (index == 1 ? fixture.secondObserver : fixture.firstObserver).workspaceResults;
        REQUIRE(!replies.empty());
        CHECK(replies.back().requestID == requests[index] && replies.back().wasPerformed);
        CHECK(replies.back().surfaceID == 0 && replies.back().tabID == 0 && replies.back().paneID == 0 && replies.back().sessionID == 0);
    }
    CHECK(fixture.firstObserver.workspaceResults.size() == 2 && fixture.secondObserver.workspaceResults.size() == 1);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionLabelsOnlyItsSharedPhysicalClient)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    std::array<std::uint32_t, 2> identities {};
    for (unsigned index = 0; index < identities.size(); ++index) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::LabelClient;
        verb.title = index ? "second request" : "first request";
        identities[index] = (index ? fixture.second : fixture.first).sendWorkspaceVerb(verb);
        REQUIRE(identities[index]);
    }
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 2);
    for (unsigned index : { 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        CHECK(request.stringOr("method") == "client.label" && request.find("params")->members().size() == 1);
        const std::string label = index ? "second request" : "first request";
        CHECK(request.find("params")->stringOr("label") == label);
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":{"client":"client:17","label":")" + label + R"("}})")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        const auto& replies = (index ? fixture.secondObserver : fixture.firstObserver).workspaceResults;
        REQUIRE(replies.size() == 1);
        CHECK(replies.front().requestID == identities[index] && replies.front().wasPerformed);
    }
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionRoutesTypedTransfersToTheirRequesters)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    const std::array sourcePanes { UbiquittyTest::StructurePane { 11, 11 }, UbiquittyTest::StructurePane { 22, 22 } };
    const std::array targetPanes { UbiquittyTest::StructurePane { 33, 33 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", sourcePanes, 17, ++fixture.structureRevision);
    auto target = UbiquittyTest::rowWorkspace(91, {}, targetPanes, 17).windows.front().tabs.front();
    target.id = Session::Layout::TabID { 2 };
    workspace.windows.front().tabs.push_back(std::move(target));
    fixture.feed(fixture.host->structure(workspace, wire));
    std::array<std::uint32_t, 3> requests {};
    const std::array operations { Ubiquitty::Session::WorkspaceIntentKind::MovePaneToTab, Ubiquitty::Session::WorkspaceIntentKind::MoveTabToPane, Ubiquitty::Session::WorkspaceIntentKind::MovePaneToNewTab };
    for (std::size_t index = 0; index < operations.size(); ++index) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operations[index];
        verb.surfaceID = verb.tabID = 1;
        verb.paneID = 22;
        verb.targetPaneID = 33;
        verb.targetTabID = 2;
        verb.orientation = std::to_underlying(Panes::Orientation::Vertical);
        verb.side = std::to_underlying(Panes::InsertSide::First);
        verb.insertTarget = std::to_underlying(Panes::InsertTarget::Root);
        requests[index] = (index == 1 ? fixture.second : fixture.first).sendWorkspaceVerb(verb);
        REQUIRE(requests[index]);
    }
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 3);
    for (std::size_t index : { 2u, 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        CHECK(request.find("params")->stringOr("expectedRevision") == std::to_string(fixture.structureRevision));
        CHECK(request.stringOr("method") == (index == 1 ? "tab.moveToPane" : index == 2 ? "pane.moveToNewTab"
                                                                                        : "pane.moveToTab"));
        const std::string result = index == 2 ? R"({"sourceWindow":"window:1","sourceTab":"tab:1","pane":"pane:22","tab":"tab:3","window":"window:1"})"
                                              : std::string(R"({"sourceWindow":"window:1","sourceTab":"tab:1","tab":"tab:2","window":"window:1","target":"pane:33","orientation":"vertical","side":"before","insertTarget":"root")")
                + (index == 1 ? "}" : R"(,"pane":"pane:22"})");
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        const auto& replies = (index == 1 ? fixture.secondObserver : fixture.firstObserver).workspaceResults;
        REQUIRE(!replies.empty());
        CHECK(replies.back().requestID == requests[index] && replies.back().wasPerformed);
        CHECK(replies.back().tabID == 0 && replies.back().surfaceID == 0 && replies.back().paneID == 0 && replies.back().sessionID == 0);
    }
    CHECK(fixture.firstObserver.workspaceResults.size() == 2 && fixture.secondObserver.workspaceResults.size() == 1);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionRoutesTypedExtractionsToTheirRequesters)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    std::array<std::uint32_t, 3> requests {};
    const std::array operations { Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToTab, Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToSurface, Ubiquitty::Session::WorkspaceIntentKind::ExtractTabToSurface };
    for (std::size_t index = 0; index < operations.size(); ++index) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operations[index];
        verb.surfaceID = verb.tabID = 1;
        verb.paneID = 22;
        requests[index] = (index == 1 ? fixture.second : fixture.first).sendWorkspaceVerb(verb);
        REQUIRE(requests[index]);
    }
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 3);
    for (std::size_t index : { 2u, 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        const auto& params = *request.find("params");
        CHECK(params.numberOr("window") == 1 && params.numberOr(index == 2 ? "tab" : "pane") == (index == 2 ? 1 : 22));
        CHECK(params.stringOr("expectedRevision") == std::to_string(fixture.structureRevision));
        CHECK(request.stringOr("method") == (index == 2 ? "tab.extractWindow" : index == 1 ? "pane.extractWindow"
                                                                                           : "pane.extractTab"));
        const std::string result = index == 2 ? R"({"sourceWindow":"window:1","tab":"tab:1","window":"window:5"})"
            : index == 1                      ? R"({"sourceWindow":"window:1","sourceTab":"tab:1","pane":"pane:22","tab":"tab:6","window":"window:5"})"
                                              : R"({"sourceWindow":"window:1","sourceTab":"tab:1","pane":"pane:22","tab":"tab:6","window":"window:1"})";
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        const auto& returned = (index == 1 ? fixture.secondObserver : fixture.firstObserver).workspaceResults.back();
        CHECK(returned.requestID == requests[index] && returned.wasPerformed);
        CHECK(returned.tabID == (index == 0 ? 6u : 0u));
        CHECK(returned.surfaceID == (index == 0 ? 0u : 5u));
        CHECK(returned.paneID == 0 && returned.sessionID == 0);
    }
    CHECK(fixture.firstObserver.workspaceResults.size() == 2 && fixture.secondObserver.workspaceResults.size() == 1);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionRoutesTypedPaneRelocationsToTheirRequesters)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::SwapPanes;
    verb.surfaceID = 1;
    verb.paneID = 22;
    verb.targetPaneID = 11;
    const auto secondRequest = fixture.second.sendWorkspaceVerb(verb);
    REQUIRE(secondRequest);
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::MovePane;
    verb.orientation = std::to_underlying(Panes::Orientation::Vertical);
    verb.side = std::to_underlying(Panes::InsertSide::Second);
    const auto firstRequest = fixture.first.sendWorkspaceVerb(verb);
    REQUIRE(firstRequest && firstRequest != secondRequest);
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 2);
    for (std::size_t index : { 1u, 0u }) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        const auto& params = *request.find("params");
        CHECK(params.numberOr("window") == 1 && params.numberOr("pane") == 22 && params.numberOr("target") == 11);
        CHECK(params.stringOr("expectedRevision") == std::to_string(fixture.structureRevision));
        CHECK(request.stringOr("method") == (index ? "pane.move" : "pane.swap"));
        const std::string extra = index ? R"(,"orientation":"vertical","side":"after")" : "";
        CHECK(fixture.host->connection().answerCall(calls[index].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":{"window":"window:1","pane":"pane:22","target":"pane:11")" + extra + "}}")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
        if (index)
            CHECK(fixture.secondObserver.workspaceResults.empty());
    }
    REQUIRE(fixture.firstObserver.workspaceResults.size() == 1 && fixture.secondObserver.workspaceResults.size() == 1);
    CHECK(fixture.firstObserver.workspaceResults[0].requestID == firstRequest && fixture.firstObserver.workspaceResults[0].wasPerformed);
    CHECK(fixture.secondObserver.workspaceResults[0].requestID == secondRequest && fixture.secondObserver.workspaceResults[0].wasPerformed);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame[0] != 0x17);
}

TEST(workspaceConnectionRoutesClipboardToOriginatingPane)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.first.sendInput("foreground elsewhere");
    fixture.queue->drain();
    fixture.feed(fixture.host->requestClipboard(22, 123, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.clipboardRequests == 1);
    REQUIRE(fixture.firstObserver.clipboardRequests == 0);
    fixture.second.stop();
    fixture.feed(fixture.host->requestClipboard(22, 124, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.clipboardRequests == 1);
    REQUIRE(fixture.firstObserver.clipboardRequests == 0);
    REQUIRE(!fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionRoutesConsentWithinItsAuthorityToTheRequestingPane)
{
    namespace Control = Protocol::Control;
    Fixture fixture;
    Fixture otherHost;
    fixture.second.start(22, 80, 24);
    otherHost.second.start(22, 80, 24);
    fixture.first.sendInput("focus elsewhere");
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    (void)fixture.host->connection().takeCalls();
    Sync::ConsentQuestion request;
    request.requestID = 123;
    request.sessionID = 22;
    const auto sendQuestion = [&] {
        const auto notify = Sync::consentQuestionNotify(request);
        REQUIRE(notify);
        REQUIRE(fixture.host->connection().sendNotify(*notify));
        fixture.feed(fixture.host->flush());
    };
    const auto takeRefusal = [&] {
        fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
        const auto calls = fixture.host->connection().takeCalls();
        REQUIRE(calls.size() == 1);
        const auto call = Control::decodeRequest(calls.front().text, {});
        REQUIRE(call.outcome == Control::RequestOutcome::Dispatch);
        REQUIRE(call.method == "consent.respond");
        REQUIRE(call.hasID && call.id.isString() && !call.id.asString().empty());
        const auto response = Control::readConsentResponse(call.params);
        REQUIRE(response);
        CHECK(response->requestID == request.requestID);
        CHECK(response->kind == Sync::ConsentKind::Attach);
        // Nobody here is shown it, which is not an answer for the person.
        CHECK(response->decision == Sync::ConsentChoice::Unshown);
    };
    sendQuestion();
    REQUIRE(fixture.secondObserver.consentRequests == std::vector<uint32_t> { 123 });
    REQUIRE(fixture.firstObserver.consentRequests.empty());
    REQUIRE(otherHost.firstObserver.consentRequests.empty());
    REQUIRE(otherHost.secondObserver.consentRequests.empty());

    fixture.second.stop();
    sendQuestion();
    REQUIRE(fixture.secondObserver.consentRequests.size() == 1);
    REQUIRE(fixture.firstObserver.consentRequests.empty());
    takeRefusal();

    request.requestID = 124;
    request.sessionID = 666;
    sendQuestion();
    REQUIRE(fixture.firstObserver.consentRequests.empty());
    takeRefusal();

    // A tree change for a session this carrier has no live pane for is not
    // shown here either, so it is declined as unshown, never denied.
    Sync::StructureConsent change {
        .requestID = 125,
        .sessionID = 666,
        .workspaceID = 3,
        .revision = 1,
        .workspaceName = "work",
        .method = "workspace.rename",
        .parameters = R"({"workspace":3,"name":"work"})",
        .agentLabel = "helper",
        .peerProcess = "/usr/bin/tool",
        .peerProcessID = 7,
    };
    const auto structureNotify = Sync::structureConsentNotify(change);
    REQUIRE(structureNotify);
    REQUIRE(fixture.host->connection().sendNotify(*structureNotify));
    fixture.feed(fixture.host->flush());
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto call = Control::decodeRequest(calls.front().text, {});
    REQUIRE(call.outcome == Control::RequestOutcome::Dispatch && call.method == "consent.respond");
    const auto response = Control::readConsentResponse(call.params);
    REQUIRE(response);
    CHECK(response->requestID == change.requestID && response->kind == Sync::ConsentKind::Structure);
    CHECK_MSG(response->decision == Sync::ConsentChoice::Unshown, "a tree change nobody here was shown was denied for the person");
}

// A host can ask about a pane in the same read as the structure placing it,
// and ahead of it, so the carrier routes the question before an endpoint
// joining in that update is established. Held while endpoints join, the
// question reaches the pane that joined; one naming a pane nobody joined is
// declined as unshown when the hold ends; and one the host retired while it
// waited is never shown or answered.
TEST(aQuestionForAPaneStillJoiningWaitsForTheJoin)
{
    namespace Control = Protocol::Control;
    const auto ask = [](Fixture& fixture, std::uint32_t requestID, std::uint32_t sessionID) {
        Sync::ConsentQuestion request;
        request.requestID = requestID;
        request.sessionID = sessionID;
        const auto notify = Sync::consentQuestionNotify(request);
        REQUIRE(notify);
        REQUIRE(fixture.host->connection().sendNotify(*notify));
        fixture.feed(fixture.host->flush());
    };
    const auto answers = [](Fixture& fixture) {
        fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
        std::vector<Sync::ConsentChoice> decisions;
        for (const auto& call : fixture.host->connection().takeCalls()) {
            const auto request = Control::decodeRequest(call.text, {});
            if (request.outcome != Control::RequestOutcome::Dispatch || request.method != "consent.respond")
                continue;
            if (const auto response = Control::readConsentResponse(request.params))
                decisions.push_back(response->decision);
        }
        return decisions;
    };

    {
        Fixture fixture;
        (void)answers(fixture);
        {
            const auto hold = fixture.connection.holdQuestionsWhileJoining();
            ask(fixture, 31, 22);
            CHECK(fixture.secondObserver.consentRequests.empty());
            fixture.second.start(22, 80, 24);
            fixture.queue->drain();
            REQUIRE(fixture.second.acknowledged());
            CHECK_MSG(fixture.secondObserver.consentRequests.empty(), "a held question was routed before the hold ended");
        }
        CHECK(fixture.secondObserver.consentRequests == std::vector<uint32_t> { 31 });
        CHECK(fixture.firstObserver.consentRequests.empty());
        CHECK_MSG(answers(fixture).empty(), "a question its joining pane was shown was answered for it");
    }
    {
        Fixture fixture;
        (void)answers(fixture);
        {
            const auto hold = fixture.connection.holdQuestionsWhileJoining();
            ask(fixture, 32, 22);
            CHECK_MSG(answers(fixture).empty(), "a held question was answered before the hold ended");
        }
        CHECK(fixture.secondObserver.consentRequests.empty());
        const auto decisions = answers(fixture);
        CHECK_MSG(decisions.size() == 1 && decisions.front() == Sync::ConsentChoice::Unshown,
            "a held question for a pane nobody joined was not declined as unshown");
    }
    {
        Fixture fixture;
        (void)answers(fixture);
        {
            const auto hold = fixture.connection.holdQuestionsWhileJoining();
            ask(fixture, 33, 22);
            const auto resolved = Sync::consentResolvedNotify(Sync::ConsentResolution { .kind = Sync::ConsentKind::Attach, .requestID = 33, .sessionID = 22 });
            REQUIRE(resolved);
            REQUIRE(fixture.host->connection().sendNotify(*resolved));
            fixture.feed(fixture.host->flush());
            fixture.second.start(22, 80, 24);
            fixture.queue->drain();
        }
        CHECK_MSG(fixture.secondObserver.consentRequests.empty(), "a question the host retired while it waited was shown");
        CHECK(answers(fixture).empty());
    }
}

// After a reconnect every pane is established again from the first
// structure, one at a time, and a pane established first flushes the
// carrier. A question the host queued ahead of that structure, for a pane
// established after it, waits for the whole update and then reaches that
// pane rather than being declined as unshown for it.
TEST(aQuestionReadAheadOfAReconnectsStructureReachesItsReestablishedPane)
{
    namespace Control = Protocol::Control;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    // A pane its frontend has sized states its viewport as it is
    // established, which is what flushes the carrier mid-update.
    fixture.first.setSize(80, 24);
    fixture.queue->drain();
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    REQUIRE(!fixture.first.acknowledged() && !fixture.second.acknowledged());

    fixture.host = std::make_unique<WorkspaceHost>();
    auto& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    Sync::ConsentQuestion request;
    request.requestID = 41;
    request.sessionID = 22;
    const auto notify = Sync::consentQuestionNotify(request);
    REQUIRE(notify);
    REQUIRE(fixture.host->connection().sendNotify(*notify));
    // The structure's read carries the question ahead of it.
    fixture.layout(FirstPanePresence::Included);
    (void)fixture.host->pane(11);
    (void)fixture.host->pane(22);
    fixture.feed(fixture.host->publish(wire));
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    CHECK(fixture.secondObserver.consentRequests == std::vector<uint32_t> { 41 });
    CHECK(fixture.firstObserver.consentRequests.empty());
    fixture.feed(fixture.host->publish(wire));
    for (const auto& call : fixture.host->connection().takeCalls()) {
        const auto decoded = Control::decodeRequest(call.text, {});
        CHECK_MSG(decoded.method != "consent.respond", "a question a re-established pane was shown was answered for it");
    }
}

TEST(workspaceConnectionHandlesLayoutBeforeRootExit)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.layout(FirstPanePresence::Absent);
    const std::vector<uint8_t> frame = fixture.host->retirePane(11, *fixture.factory->wires.back());
    fixture.feed(frame);
    REQUIRE(fixture.firstObserver.exits == 1);
    REQUIRE(!fixture.factory->wires.back()->stopped);
    fixture.second.sendInput("survives root exit");
    fixture.queue->drain();
}

TEST(workspacePasteOwnsOnePhysicalUploadAndAnswersOnlyItsOriginalEndpoint)
{
    using namespace Protocol::Control;
    using namespace Protocol::Sync;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    const std::array<std::uint8_t, 4> payload { 'a', 0, 'b', 0xFF };
    const auto firstID = fixture.first.pushPaste("text/plain", payload);
    REQUIRE(firstID);
    CHECK(!fixture.second.pushPaste("text/plain", payload));
    fixture.feed(fixture.host->publish(wire));
    auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false;
    const auto first = Codec::JSON::parse(calls.front().text, isJSON);
    REQUIRE(isJSON && first.find("params"));
    const auto manifest = pastePushParameters(*first.find("params"));
    REQUIRE(manifest);
    CHECK(first.stringOr("method") == "paste.push");
    CHECK(fixture.first.pastePushProgress() == 0);
    CHECK(fixture.second.pastePushProgress() < 0);
    std::vector<std::uint8_t> received;
    unsigned chunks = 0;
    fixture.host->connection().setControlUploadReceiver([&](const BulkChunk& chunk) {
        ++chunks;
        CHECK(chunk.transfer == 9 && chunk.index == 0 && chunk.isLast);
        CHECK(chunk.sha256 == manifest->sha256);
        received.assign(chunk.bytes.begin(), chunk.bytes.end());
        return ClientBulkDisposition::Accepted; }, [](std::uint32_t transfer) { return transfer == 9; });
    CHECK(fixture.host->connection().sendNotify(*pasteReadyNotify({ "unrelated", manifest->pane, 9 })));
    fixture.feed(fixture.host->flush());
    fixture.feed(fixture.host->publish(wire));
    CHECK(chunks == 0);
    wire.pending = wire.capacity;
    CHECK(fixture.host->connection().sendNotify(*pasteReadyNotify({ first.stringOr("id"), manifest->pane, 9 })));
    fixture.feed(fixture.host->flush());
    fixture.feed(fixture.host->publish(wire));
    CHECK(chunks == 0);
    wire.pending = 0;
    CHECK(fixture.host->connection().sendNotify(*pasteReadyNotify({ first.stringOr("id"), manifest->pane, 9 })));
    fixture.feed(fixture.host->flush());
    fixture.feed(fixture.host->publish(wire));
    CHECK(chunks == 1);
    CHECK(std::ranges::equal(received, payload));

    // Stopping the original pane retires its source. The neighbor can own a
    // new upload, but a late Result for the old pane cannot settle that owner.
    fixture.first.stop();
    const auto secondID = fixture.second.pushPaste("text/plain", payload);
    REQUIRE(secondID);
    fixture.feed(fixture.host->publish(wire));
    const auto next = fixture.host->connection().takeCalls();
    REQUIRE(next.size() == 1);
    const auto second = Codec::JSON::parse(next.front().text, isJSON);
    REQUIRE(isJSON && second.find("params"));
    const auto secondManifest = pastePushParameters(*second.find("params"));
    REQUIRE(secondManifest);
    CHECK(secondManifest->pane != manifest->pane);
    const auto answer = [&](std::size_t index, const Codec::JSON::Value& request, std::uint32_t pane, std::string status) {
        auto result = Codec::JSON::Value::object();
        result.set("pane", Codec::JSON::Value::number(pane));
        result.set("status", Codec::JSON::Value::string(std::move(status)));
        result.set("path", Codec::JSON::Value::string(""));
        auto response = Codec::JSON::Value::object();
        response.set("jsonrpc", Codec::JSON::Value::string("2.0"));
        response.set("id", *request.find("id"));
        response.set("result", std::move(result));
        CHECK(fixture.host->connection().answerCall(index, Codec::JSON::serialize(response)) == CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
    };
    answer(calls.front().callIndex, first, manifest->pane, "busy");
    CHECK(fixture.firstObserver.pastes.empty() && fixture.secondObserver.pastes.empty());
    CHECK(!fixture.first.pushPaste("text/plain", payload));
    CHECK(!fixture.second.pushPaste("text/plain", payload));
    // Unsupported may be answered before credit when the current target has
    // no input stream; it still belongs only to the second endpoint and Call.
    answer(next.front().callIndex, second, secondManifest->pane, "unsupported");
    REQUIRE(fixture.secondObserver.pastes.size() == 1);
    CHECK(fixture.secondObserver.pastes.front().pushID == secondID);
    CHECK(fixture.secondObserver.pastes.front().status == PastePushStatus::Unsupported);
    CHECK(fixture.firstObserver.pastes.empty());
    CHECK(fixture.second.pushPaste("text/plain", payload));
}

TEST(workspaceThemesUseThePhysicalAttachmentsFactsWithoutMutatingEitherMirror)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(wire));
    const auto firstPalette = fixture.firstTerm->palette;
    const auto secondPalette = fixture.secondTerm->palette;
    TermPalette palette {};
    palette.seedDefaults();
    palette.defaultBackground = { 0x1234, 0x5678, 0x9abc };
    auto baseline = palette;
    baseline.defaultBackground = { 0xabcd, 0xef01, 0x2345 };
    REQUIRE(fixture.second.sendPalette(palette, baseline));
    fixture.feed(fixture.host->publish(wire));
    const auto& facts = fixture.host->connection().facts();
    REQUIRE(facts.themePalette.has_value());
    CHECK(facts.themePalette->live == palette);
    CHECK(facts.themePalette->baseline == baseline);
    CHECK(fixture.firstTerm->palette == firstPalette);
    CHECK(fixture.secondTerm->palette == secondPalette);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame.front() != 0x14);

    wire.observer->onClose("palette reconnect");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.handshake();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const Protocol::Sync::ThemePalette expectedTheme { .live = palette, .baseline = baseline };
    CHECK(fixture.host->connection().facts().themePalette == std::optional { expectedTheme });
}

TEST(sharedWindowPublicationUsesThePanesPhysicalWorkspaceConnection)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(wire));
    Session::SharedWorkspace shared;
    Session::WorkspaceWindow window;
    window.tabs.push_back({ "shell", Session::PaneNode::makeLeaf({}), 0 });
    shared.arrangement.windows.push_back(std::move(window));
    shared.sessions = { 22 };
    CHECK(!fixture.first.publishSharedWorkspace(shared));
    REQUIRE(fixture.second.publishSharedWorkspace(shared));
    CHECK(fixture.first.sharedWorkspaceResult().isPending);
    CHECK(fixture.second.sharedWorkspaceResult().isPending);
    CHECK(!fixture.second.publishSharedWorkspace(shared));
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
    CHECK(request.method == "workspace.share");
    CHECK(request.id.asString() == "ubiquitty.share.22");
    CHECK(request.params.stringOr("daemonInstanceID") == "17");
    CHECK(fixture.host->connection().answerCall(calls.front().callIndex,
              R"({"jsonrpc":"2.0","id":"ubiquitty.share.22","result":{"workspace":19}})")
        == Protocol::Sync::CallAnswer::Sent);
    fixture.feed(fixture.host->publish(wire));
    CHECK(!fixture.second.sharedWorkspaceResult().isPending);
    CHECK_EQ(fixture.second.sharedWorkspaceResult().workspaceID, 19u);
    CHECK_EQ(fixture.secondObserver.sharedWorkspaceChanges, 1u);
    CHECK_EQ(fixture.firstObserver.sharedWorkspaceChanges, 1u);
    CHECK_EQ(fixture.factory->wires.size(), 1u);
    CHECK(canonicalWrites(wire));
}

TEST(adoptedSessionFollowsItsPublishedWorkspaceAndReconnectsBySession)
{
    Fixture fixture;
    auto& physical = fixture.connection.connection();
    physical.followAdoptedSessionWorkspace();
    fixture.layout(FirstPanePresence::Absent, 92);
    CHECK_EQ(physical.workspaceID(), 91u);
    fixture.layout(FirstPanePresence::Included, 92);
    CHECK_EQ(physical.workspaceID(), 92u);
    CHECK_EQ(fixture.first.workspaceID(), 92u);
    fixture.first.suspend();
    fixture.first.resumeAfterSuspension(120);
    fixture.queue->drain();
    REQUIRE(fixture.factory->wires.size() == 2);
    auto& wire = *fixture.factory->wires.back();
    Sync::FrameDecoder decoder;
    bool didSeeSessionAttach = false;
    for (const auto& bytes : wire.sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Sync::FrameKind::Greet)
                continue;
            const auto greet = Sync::decodeGreet(frame->body);
            didSeeSessionAttach = greet && greet->request.sessionID == 11 && !greet->request.workspaceID
                && greet->request.expectedDaemonInstanceID == 17 && greet->request.isAttachOnly;
        }
    }
    REQUIRE(didSeeSessionAttach);
    fixture.host = std::make_unique<WorkspaceHost>("workspace-test.sock", 93);
    fixture.feed(fixture.host->publish(wire));
    fixture.layout(FirstPanePresence::Included, 93);
    (void)fixture.host->pane(11);
    fixture.feed(fixture.host->publish(wire));
    CHECK(fixture.first.state() == Session::State::Connected);
    CHECK_EQ(fixture.first.sessionID(), 11u);
    CHECK_EQ(physical.workspaceID(), 93u);
    CHECK_EQ(physical.daemonInstanceID(), 17u);
}

TEST(workspaceReferencesDoNotFollowUnrequestedWorkspaceMoves)
{
    for (const bool shouldFollowSession : { false, true }) {
        Fixture fixture;
        auto& physical = fixture.connection.connection();
        if (shouldFollowSession) {
            physical.followAdoptedSessionWorkspace();
            physical.setAttachWorkspaceID(91);
        }
        fixture.layout(FirstPanePresence::Included, 92);
        CHECK_EQ(physical.workspaceID(), 91u);
        fixture.first.suspend();
        fixture.first.resumeAfterSuspension(120);
        fixture.queue->drain();
        REQUIRE(fixture.factory->wires.size() == 2);
        fixture.host = std::make_unique<WorkspaceHost>("workspace-test.sock", 92);
        fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
        CHECK(physical.state() == Session::State::Failed);
        CHECK(!fixture.first.acknowledged());
    }
}

TEST(workspaceClipboardRoutingBelongsToTheSharedPhysicalConnection)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(wire));
    REQUIRE(fixture.second.setClipboardServing(Clipboard::Serving::Enabled));
    CHECK(!fixture.first.didClaimClipboardServe());
    CHECK(!fixture.second.didClaimClipboardServe());
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
    CHECK(request.method == "client.clipboardServing");
    CHECK(request.params.boolOr("enabled"));
    CHECK(fixture.host->connection().answerCall(calls.front().callIndex,
              R"({"jsonrpc":"2.0","id":)" + std::to_string(static_cast<std::uint64_t>(request.id.asNumber())) + R"(,"result":{"enabled":true}})")
        == Protocol::Sync::CallAnswer::Sent);
    fixture.feed(fixture.host->publish(wire));
    CHECK(fixture.first.didClaimClipboardServe());
    CHECK(fixture.second.didClaimClipboardServe());
    REQUIRE(fixture.first.setClipboardServing(Clipboard::Serving::Disabled));
    CHECK(!fixture.first.didClaimClipboardServe());
    CHECK(!fixture.second.didClaimClipboardServe());
    CHECK(fixture.factory->wires.size() == 1);
    for (const auto& frame : wire.sent)
        CHECK(frame.empty() || frame.front() != 0x18);
}

TEST(workspaceConnectionClearsTheExactEndpointThroughItsSharedCarrier)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto& wire = *fixture.factory->wires.back();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(wire));
    REQUIRE(fixture.first.sendClearBuffer(Screen::ClearCommand::Buffer));
    REQUIRE(fixture.second.sendClearBuffer(Screen::ClearCommand::Scrollback));
    fixture.feed(fixture.host->publish(wire));
    const auto calls = fixture.host->connection().takeCalls();
    REQUIRE(calls.size() == 2);
    std::array<double, 2> identifiers {};
    for (std::size_t index = 0; index < calls.size(); ++index) {
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[index].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        CHECK(request.stringOr("method") == "pane.clear");
        CHECK(request.find("params")->numberOr("pane") == index + 1);
        CHECK(request.find("params")->stringOr("command") == (index ? "scrollback" : "buffer"));
        identifiers[index] = request.numberOr("id");
    }
    CHECK(identifiers[0] != identifiers[1]);
    CHECK(canonicalWrites(wire));
}

TEST(workspaceConnectionAppliesScreenOnlyToAddressedPane)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    // The checkpoint the second endpoint asked for when it attached.
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const unsigned firstScreens = fixture.firstObserver.screens;
    const unsigned secondScreens = fixture.secondObserver.screens;
    Terminal& pane = fixture.host->pane(22);
    resize(pane, 4, 12);
    processBytes(&pane, "pane two");
    fixture.feed(fixture.host->publishPane(22, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.screens == secondScreens + 1);
    REQUIRE(fixture.firstObserver.screens == firstScreens);
    const auto* cell = Screen::visibleCell(fixture.secondTerm.get(), 0, 0, 0);
    REQUIRE(cell && cell->codePoint == 'p');
    REQUIRE(fixture.secondTerm->columns == 12);
}

namespace {

/// A workspace whose second pane's leaf names a size and the structural
/// generation that brought it.
///
/// - Parameters:
///   - revision: The structure's revision.
///   - rows: The second pane's rows.
///   - columns: Its columns.
///   - generation: The generation of its latest Structural frame.
/// - Returns: The structure.
Session::TreeWorkspace resizedWorkspace(std::uint64_t revision, int rows, int columns, std::uint64_t generation)
{
    const std::array<UbiquittyTest::StructurePane, 2> panes { { { .pane = 11, .sessionID = 11, .columns = 39, .rows = 24 }, { .pane = 22, .sessionID = 22, .columns = columns, .rows = rows } } };
    Session::TreeWorkspace workspace = UbiquittyTest::rowWorkspace(91, {}, panes, 17, revision);
    const auto stamp = [&](auto& self, Sync::TopologyNode& node) -> void {
        if (node.isLeaf()) {
            if (node.paneID == Sync::ID128 { .low = 22, .high = 17 })
                node.structuralGeneration = generation;
            return;
        }
        for (Sync::TopologyNode& child : node.children)
            self(self, child);
    };
    stamp(stamp, workspace.windows[0].tabs[0].split);
    return workspace;
}

} // namespace

// The host resizes the second pane and describes the new size in a structure
// that overtakes the pane's checkpoint, as separate streams may deliver it.
// The structure reaches the client attachment from the structure path; until
// the checkpoint lands, the rectangle keeps the old picture's size,
// letterboxed, and at every frame it is either letterboxed or the size of the
// grid drawn in it (Docs/sync.md §6.10 "Topology never overtakes content").
TEST(aStructureAheadOfItsCheckpointNeverResizesTheOldPicture)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    fixture.feed(fixture.host->structure(resizedWorkspace(++fixture.structureRevision, 24, 80, 0), *fixture.factory->wires.back()));
    REQUIRE(fixture.second.paneRectangle().has_value());
    CHECK((*fixture.second.paneRectangle() == Sync::PaneRectangle { .rows = 24, .columns = 80, .isLetterboxed = false }));

    const std::uint64_t generation = fixture.host->resizePane(22, 30, 100);
    const std::vector<uint8_t> sent = fixture.host->structure(resizedWorkspace(++fixture.structureRevision, 30, 100, generation), *fixture.factory->wires.back());
    std::vector<std::vector<uint8_t>> frames;
    Sync::FrameDecoder decoder;
    decoder.append(sent);
    std::size_t structureIndex = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind == Sync::FrameKind::WorkspaceStructure)
            structureIndex = frames.size();
        std::vector<uint8_t> one;
        REQUIRE(Sync::appendFrame(one, frame->kind, frame->channel, frame->body));
        frames.push_back(std::move(one));
    }
    REQUIRE(decoder.error() == Sync::EnvelopeError::None);
    REQUIRE(frames.size() > 1);
    REQUIRE(structureIndex > 0);
    // Channel 0 overtakes the pane's channel.
    std::ranges::rotate(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(structureIndex), frames.begin() + static_cast<std::ptrdiff_t>(structureIndex) + 1);
    fixture.feed(frames.front());
    REQUIRE(fixture.second.paneRectangle().has_value());
    CHECK((*fixture.second.paneRectangle() == Sync::PaneRectangle { .rows = 24, .columns = 80, .isLetterboxed = true }));
    CHECK_EQ(fixture.secondTerm->rows, 24);
    CHECK_EQ(fixture.secondTerm->columns, 80);
    for (std::size_t index = 1; index < frames.size(); ++index) {
        fixture.feed(frames[index]);
        const std::optional<Sync::PaneRectangle> rectangle = fixture.second.paneRectangle();
        REQUIRE(rectangle.has_value());
        CHECK(rectangle->isLetterboxed || (rectangle->rows == fixture.secondTerm->rows && rectangle->columns == fixture.secondTerm->columns));
    }
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK((*fixture.second.paneRectangle() == Sync::PaneRectangle { .rows = 30, .columns = 100, .isLetterboxed = false }));
}

TEST(workspaceConnectionReportsRecordsWithoutAScreenUpdateOnlyToAddressedPane)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const unsigned firstScreens = fixture.firstObserver.screens;
    const unsigned secondScreens = fixture.secondObserver.screens;
    Protocol::Sync::PaneFacts facts;
    facts.progress = { .state = OSC::ProgressIndeterminate, .value = 0 };
    fixture.feed(fixture.host->records(22, facts, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondTerm->progressState == OSC::ProgressIndeterminate);
    REQUIRE(fixture.firstTerm->progressState == OSC::ProgressNone);
    REQUIRE(fixture.secondObserver.screens == secondScreens + 1);
    REQUIRE(fixture.firstObserver.screens == firstScreens);

    // Clearing the record is news too; no checkpoint, Update or trailing
    // terminal output is needed to take the chrome down.
    facts.progress = {};
    fixture.feed(fixture.host->records(22, facts, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondTerm->progressState == OSC::ProgressNone);
    REQUIRE(fixture.secondObserver.screens == secondScreens + 2);
    REQUIRE(fixture.firstObserver.screens == firstScreens);
}

TEST(remoteNonOwnerPresentationUsesAuthenticatedPerPaneGeometry)
{
    Fixture fixture;
    CHECK(!fixture.first.isGeometryOwnedElsewhere());
    CHECK(!fixture.second.isGeometryOwnedElsewhere());
    fixture.feed(fixture.host->geometryOwner(22, { 99, 17 }, *fixture.factory->wires.back()));
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(fixture.second.isGeometryOwnedElsewhere());
    CHECK(!fixture.first.isGeometryOwnedElsewhere());
    // The client's dimensions do not establish authority: only the matching
    // attachment ID from its Welcome can make it the size owner.
    fixture.feed(fixture.host->geometryOwner(22, { 1, 17 }, *fixture.factory->wires.back()));
    CHECK(!fixture.second.isGeometryOwnedElsewhere());
    fixture.feed(fixture.host->geometryOwner(22, { 99, 17 }, *fixture.factory->wires.back()));
    REQUIRE(fixture.second.isGeometryOwnedElsewhere());
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    CHECK(!fixture.second.isGeometryOwnedElsewhere());
    fixture.queue->advance();
    fixture.handshake();
    CHECK(!fixture.second.isGeometryOwnedElsewhere());
    fixture.feed(fixture.host->geometryOwner(22, { 99, 17 }, *fixture.factory->wires.back()));
    REQUIRE(fixture.second.isGeometryOwnedElsewhere());
    fixture.second.stop();
    CHECK(!fixture.second.isGeometryOwnedElsewhere());
}

TEST(workspacePaneFactsReachTitleAndForegroundReadersIncludingLateAttachments)
{
    Fixture fixture;
    Protocol::Sync::PaneFacts facts;
    facts.programTitle = "recorded title";
    facts.currentDirectory = "/srv/project";
    facts.processChain = "less ◂ -zsh";
    facts.processChainWithArguments = "less README ◂ -zsh";
    facts.foreground.name = "less";
    facts.shellCommandName = "zsh";
    facts.terminalName = "ttys004";
    facts.isAtPrompt = false;
    facts.progress = { .state = OSC::ProgressIndeterminate, .value = 0 };
    fixture.feed(fixture.host->records(22, facts, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.titles == 0);

    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.secondObserver.lastTitle == facts.programTitle);
    REQUIRE(fixture.secondObserver.lastTitleParts.currentDirectory == facts.currentDirectory);
    REQUIRE(fixture.secondObserver.lastTitleParts.processChain == facts.processChain);
    REQUIRE(fixture.secondObserver.lastTitleParts.processChainWithArguments == facts.processChainWithArguments);
    REQUIRE(fixture.secondObserver.lastTitleParts.foreground.name == facts.foreground.name);
    REQUIRE(fixture.secondObserver.lastTitleParts.shellCommandName == facts.shellCommandName);
    REQUIRE(fixture.secondObserver.lastTitleParts.terminalName == facts.terminalName);
    REQUIRE(!fixture.second.foregroundIsShell());
    REQUIRE(fixture.secondTerm->progressState == OSC::ProgressIndeterminate);
    REQUIRE(fixture.firstObserver.lastTitle.empty());

    facts.programTitle.clear();
    facts.currentDirectory.clear();
    facts.isAtPrompt = true;
    fixture.feed(fixture.host->records(22, facts, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.lastTitle.empty());
    REQUIRE(fixture.secondObserver.lastTitleParts.currentDirectory.empty());
    REQUIRE(fixture.second.foregroundIsShell());

    // The removed byte is fatal before it can overwrite the typed record.
    const std::vector<uint8_t> oldTitle { 0x83, 0, 0, 0, 0 };
    fixture.feed(oldTitle);
    REQUIRE(fixture.secondObserver.lastTitle.empty());
    REQUIRE(fixture.connection.connection().state() != Session::State::Connected);
}

TEST(aLateThirdSessionsProcessFactsStayWithItsTab)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    auto thirdTerminal = std::make_unique<Terminal>();
    Observer thirdObserver;
    Session::RemoteSessionCore third(thirdTerminal.get(), fixture.connection, &thirdObserver);
    const auto layout = [&](unsigned count, unsigned selected) {
        Session::TreeWorkspace workspace;
        workspace.id = 91;
        workspace.revision = ++fixture.structureRevision;
        Session::TreeWindow window;
        window.id = Session::Layout::SurfaceID { 1 };
        window.isHostActive = true;
        for (unsigned index = 0; index < count; ++index) {
            const std::array panes { UbiquittyTest::StructurePane { .pane = index + 1, .sessionID = (index + 1) * 11, .columns = 80, .rows = 24 } };
            auto row = UbiquittyTest::rowWorkspace(91, {}, panes, 17);
            auto tab = std::move(row.windows.front().tabs.front());
            tab.id = Session::Layout::TabID { index + 1 };
            tab.isHostActive = index == selected;
            window.tabs.push_back(std::move(tab));
        }
        workspace.windows.push_back(std::move(window));
        fixture.feed(fixture.host->structure(workspace, *fixture.factory->wires.back()));
    };
    const auto publish = [&](std::uint32_t sessionID, const char* name) {
        Sync::PaneFacts facts;
        facts.programTitle = name;
        facts.foreground = { .processID = sessionID * 100, .name = name, .generation = 1 };
        facts.processChain = std::string(name) + " ◂ -zsh";
        facts.processChainWithArguments = facts.processChain;
        facts.terminalName = "pts/" + std::to_string(sessionID);
        fixture.feed(fixture.host->records(sessionID, facts, *fixture.factory->wires.back()));
    };
    const auto verify = [&] {
        for (const auto& [observer, sessionID, name] : {
                 std::tuple { &fixture.firstObserver, 11u, "first-job" },
                 std::tuple { &fixture.secondObserver, 22u, "idle-shell" },
                 std::tuple { &thirdObserver, 33u, "third-job" } }) {
            CHECK_STR_EQ(observer->lastTitle, name);
            CHECK_STR_EQ(observer->lastTitleParts.foreground.name, name);
            CHECK_EQ(observer->lastTitleParts.foreground.processID, sessionID * 100);
            CHECK_STR_EQ(observer->lastTitleParts.processChain, std::string(name) + " ◂ -zsh");
            CHECK_STR_EQ(observer->lastTitleParts.terminalName, "pts/" + std::to_string(sessionID));
        }
    };
    layout(2, 1);
    publish(11, "first-job");
    publish(22, "idle-shell");
    layout(3, 2);
    publish(33, "third-job");
    third.start(33, 80, 24);
    REQUIRE(third.acknowledged());
    verify();
    (void)fixture.host->resizePane(33, 30, 100);
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    verify();
    layout(3, 0);
    publish(33, "third-job");
    verify();
    fixture.factory->wires.back()->observer->onClose("reconnect");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.host = std::make_unique<WorkspaceHost>();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    layout(3, 2);
    publish(33, "third-job");
    publish(22, "idle-shell");
    publish(11, "first-job");
    REQUIRE(third.acknowledged());
    verify();
}

TEST(workspacePaneFactsDoNotContinueAfterTheirObserverStopsThePane)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.secondObserver.titlePartsCallback = [&] { fixture.second.stop(); };
    Protocol::Sync::PaneFacts facts;
    facts.programTitle = "must not arrive after stopping";
    facts.currentDirectory = "/srv/stopped";
    fixture.feed(fixture.host->records(22, facts, *fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.lastTitleParts.currentDirectory == facts.currentDirectory);
    REQUIRE(!fixture.second.acknowledged());
    REQUIRE(fixture.secondObserver.lastTitle.empty());
}

TEST(workspacePaneFactsDoNotRetitleAReconnectedPaneFromAnOldCallback)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    bool shouldReconnect = true;
    fixture.secondObserver.titlePartsCallback = [&] {
        if (!shouldReconnect)
            return;
        shouldReconnect = false;
        fixture.factory->wires.back()->observer->onClose("drop during title-parts callback");
        fixture.queue->drain();
        fixture.queue->advance();
        REQUIRE(fixture.factory->wires.size() == 2);
        REQUIRE(fixture.factory->wires.back()->observer);
        fixture.handshake();
        Protocol::Sync::PaneFacts current;
        current.programTitle = "current connection";
        fixture.feed(fixture.host->records(22, current, *fixture.factory->wires.back()));
    };
    Protocol::Sync::PaneFacts previous;
    previous.programTitle = "superseded connection";
    fixture.feed(fixture.host->records(22, previous, *fixture.factory->wires.back()));
    REQUIRE(!shouldReconnect);
    REQUIRE(fixture.second.acknowledged());
    REQUIRE(fixture.secondObserver.lastTitle == "current connection");
}

TEST(workspaceConnectionRegistryReservesPendingIdentities)
{
    Session::WorkspaceConnectionRegistry registry;
    Session::WorkspaceAttachRequest request;
    request.authority = { "ssh user@host", "user", "daemon-one.sock" };
    request.workspaceID = 91;
    unsigned creations = 0;
    auto create = [&] {
        ++creations;
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };
    auto first = registry.acquire(request, create);
    auto again = registry.acquire(request, create);
    REQUIRE(first && again && creations == 1);
    REQUIRE(!first->wasReused && again->wasReused);
    REQUIRE(first->connection == again->connection);
    request.workspaceID = 92;
    auto otherWorkspace = registry.acquire(request, create);
    REQUIRE(otherWorkspace && !otherWorkspace->wasReused && creations == 2);
    request.workspaceID = 91;
    request.authority.daemonSocket = "daemon-two.sock";
    auto otherDaemon = registry.acquire(request, create);
    REQUIRE(otherDaemon && !otherDaemon->wasReused && creations == 3);
    request.workspaceID = 0;
    request.sessionID = 77;
    auto pendingSession = registry.acquire(request, create);
    auto repeatedPendingSession = registry.acquire(request, create);
    REQUIRE(pendingSession && repeatedPendingSession && repeatedPendingSession->wasReused && creations == 4);
    request.sessionID = 0;
    request.workspaceName = "build";
    auto pendingName = registry.acquire(request, create);
    auto repeatedPendingName = registry.acquire(request, create);
    REQUIRE(pendingName && repeatedPendingName && repeatedPendingName->wasReused && creations == 5);
    request.workspaceName = "editor";
    auto differentName = registry.acquire(request, create);
    REQUIRE(differentName && !differentName->wasReused && creations == 6);
    request.authority.daemonSocket = "bad;socket";
    REQUIRE(!registry.acquire(request, create));
    REQUIRE(creations == 6);
}

// The workspace a carrier mirrors is the one its handshake named. Another
// workspace's structure, which a host sends a connection that lists it, is
// not the carrier's and moves nothing: the mirror, its panes and its identity
// stay what they were.
TEST(workspaceConnectionIsNotRetargetedByAnotherWorkspacesStructure)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    const int changesBefore = fixture.secondObserver.workspaceChanges;
    fixture.layout(FirstPanePresence::Absent, 92);
    REQUIRE(fixture.connection.connection().state() != Session::State::Failed);
    REQUIRE(!fixture.factory->wires.back()->stopped);
    REQUIRE(fixture.second.workspaceID() == 91);
    REQUIRE(fixture.firstObserver.exits == 0);
    REQUIRE(fixture.secondObserver.workspaceChanges == changesBefore);
    // Nor does a later revision of its own workspace that shows the same
    // windows, tabs and panes: who is attached revises a structure, and no
    // mirror holds it.
    fixture.layout(FirstPanePresence::Included);
    REQUIRE(fixture.secondObserver.workspaceChanges == changesBefore);
    fixture.layout(FirstPanePresence::Included, 91, 11, "renamed");
    REQUIRE(fixture.secondObserver.workspaceChanges == changesBefore + 1);
}

TEST(workspaceConnectionHandlesExitBeforeLayoutWithoutRetirementLeak)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    for (uint32_t identity = 100; identity < 400; ++identity) {
        fixture.layout(FirstPanePresence::Included, 91, identity);
        (void)fixture.host->pane(identity);
        fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
        fixture.feed(fixture.host->retirePane(identity, *fixture.factory->wires.back()));
        fixture.layout(FirstPanePresence::Absent);
        REQUIRE(!fixture.factory->wires.back()->stopped);
        REQUIRE(fixture.second.acknowledged());
    }
    REQUIRE(fixture.factory->wires.size() == 1);
}

TEST(aRepeatedCanonicalUnbindRefusesTheCarrierWithoutRepeatingTheEndpointExit)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    const auto handle = fixture.host->paneHandle(11);
    REQUIRE(handle);
    const auto retirement = fixture.host->retirePane(11, *fixture.factory->wires.back());
    Sync::FrameDecoder decoder;
    decoder.append(retirement);
    std::vector<std::uint8_t> replay;
    while (const auto frame = decoder.next()) {
        if (frame->kind != Sync::FrameKind::PaneUnbind)
            continue;
        const auto unbind = Sync::decodePaneUnbind(frame->body);
        REQUIRE(unbind && unbind->handle == handle);
        REQUIRE(Sync::appendFrame(replay, frame->kind, frame->channel, frame->body));
    }
    REQUIRE(!replay.empty());
    fixture.feed(retirement);
    REQUIRE(fixture.firstObserver.exits == 1);
    REQUIRE(fixture.secondObserver.exits == 0);
    REQUIRE(!fixture.factory->wires.back()->stopped);
    fixture.feed(replay);
    CHECK(fixture.factory->wires.back()->stopped);
    CHECK(fixture.connection.connection().state() == Session::State::Reconnecting);
    CHECK(fixture.firstObserver.exits == 1);
    CHECK(fixture.secondObserver.exits == 0);
    CHECK(fixture.factory->wires.size() == 1);
}

TEST(workspaceConnectionAllowsEndpointDestructionDuringExit)
{
    Fixture fixture;
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(fixture.secondTerm.get(), fixture.connection, &observer);
    observer.exitCallback = [&] { endpoint.reset(); };
    endpoint->start(22, 80, 24);
    const std::vector<uint8_t> frame = fixture.host->retirePane(22, *fixture.factory->wires.back());
    fixture.feed(frame);
    REQUIRE(!endpoint);
    REQUIRE(observer.exits == 1);
    REQUIRE(!fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionAllowsEndpointDestructionDuringEstablishment)
{
    Fixture fixture;
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(fixture.secondTerm.get(), fixture.connection, &observer);
    observer.establishedCallback = [&] { endpoint.reset(); };
    endpoint->start(22, 80, 24);
    REQUIRE(!endpoint);
    REQUIRE(observer.established == 1);
    REQUIRE(!fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionRejectsDetachedEndpointControls)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.first.stop();
    auto before = fixture.factory->wires.back()->sent.size();
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddSurface;
    REQUIRE(!fixture.first.sendWorkspaceVerb(verb));
    REQUIRE(!fixture.first.requestSessionList());
    fixture.queue->drain();
    REQUIRE(fixture.factory->wires.back()->sent.size() == before);
}

TEST(workspaceConnectionBoundsRepliesForUnrepresentedPane)
{
    Fixture fixture;
    // More than the former per-pane reply queue limit: a roster replaces the
    // one canonical snapshot instead of accumulating deferred pane messages.
    for (std::uint64_t revision = 1; revision <= 4500; ++revision)
        fixture.attachments(rosterForSecondPane(revision, std::to_string(revision)));
    CHECK(!fixture.factory->wires.back()->stopped);
    CHECK(fixture.secondObserver.presences == 0);
    fixture.second.start(22, 80, 24);
    CHECK(fixture.secondObserver.presences == 1);
    REQUIRE(fixture.secondObserver.lastPresence.attendees.size() == 1);
    CHECK(fixture.secondObserver.lastPresence.attendees.front().label == "4500");
    CHECK(fixture.factory->wires.size() == 1);
}

TEST(workspaceConnectionRegistryReleasesFailedOwnerReservation)
{
    Session::WorkspaceConnectionRegistry registry;
    Session::WorkspaceAttachRequest request;
    request.authority.endpointIdentity = "local host";
    request.workspaceID = 91;
    unsigned creations = 0;
    auto create = [&] {
        ++creations;
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };
    auto first = registry.acquire(request, create);
    REQUIRE(first);
    first->connection->refuseDuplicateAttachment();
    auto replacement = registry.acquire(request, create);
    REQUIRE(replacement);
    REQUIRE(!replacement->wasReused);
    REQUIRE(creations == 2);
    REQUIRE(first->connection != replacement->connection);
}

TEST(workspaceConnectionRetainsCarrierDuringFinalFrontendRelease)
{
    auto dispatch = std::make_unique<Queue>();
    auto* queue = dispatch.get();
    auto transport = std::make_unique<Factory>();
    auto* factory = transport.get();
    auto owner = std::make_unique<Session::WorkspaceConnection>(std::move(dispatch), std::move(transport));
    auto terminal = std::make_unique<Terminal>();
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(terminal.get(), *owner, &observer);
    endpoint->start(0, 80, 24);
    queue->drain();
    auto wire = factory->wires.back();
    WorkspaceHost host;
    wire->observer->onData(host.publish(*wire));
    queue->drain();
    const UbiquittyTest::StructurePane only { .pane = 11, .sessionID = 11 };
    wire->observer->onData(host.structure(UbiquittyTest::rowWorkspace(91, {}, { &only, 1 }, 17), *wire));
    queue->drain();
    REQUIRE(endpoint->acknowledged());
    observer.titleCallback = [&] { endpoint.reset(); owner.reset(); };
    Protocol::Sync::PaneFacts facts;
    facts.programTitle = "release frontend";
    const auto frame = host.records(11, facts, *wire);
    wire->observer->onData(frame);
    REQUIRE(!queue->work.empty());
    auto task = std::move(queue->work.front());
    queue->work.pop_front();
    std::move(task)();
    REQUIRE(!endpoint);
    REQUIRE(!owner);
    REQUIRE(wire->stopped);
}

TEST(workspaceConnectionHandlesSynchronousCarrierFailureDuringDrain)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->shouldRunInline = true;
    auto wire = fixture.factory->wires.back();
    wire->shouldFailNextSend = true;
    fixture.first.sendInput("disconnect during send");
    REQUIRE(wire->stopped);
    REQUIRE(!fixture.first.acknowledged());
    REQUIRE(!fixture.second.acknowledged());
    REQUIRE(fixture.factory->wires.size() == 1);
}

TEST(workspaceConnectionRegistryUsesAuthoritativeRenamedWorkspace)
{
    Fixture fixture;
    Session::WorkspaceConnectionRegistry registry;
    Session::WorkspaceAttachRequest request;
    request.authority.endpointIdentity = "local host";
    request.workspaceName = "build";
    unsigned creations = 0;
    auto create = [&]() -> std::shared_ptr<Session::WorkspaceConnection> {
        if (++creations == 1) {
            return { &fixture.connection, [](auto*) { } };
        }
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };
    auto first = registry.acquire(request, create);
    REQUIRE(first);
    fixture.layout(FirstPanePresence::Included, 91, 11, "build");
    auto same = registry.acquire(request, create);
    REQUIRE(same && same->wasReused && creations == 1);
    fixture.layout(FirstPanePresence::Included, 91, 11, "editor");
    auto oldName = registry.acquire(request, create);
    REQUIRE(oldName && !oldName->wasReused && creations == 2);
    request.workspaceName = "editor";
    auto renamed = registry.acquire(request, create);
    REQUIRE(renamed && renamed->wasReused && renamed->connection == first->connection);
}

TEST(workspaceConnectionRegistryRejectsOtherEventLoopBeforePruning)
{
    Session::WorkspaceConnectionRegistry registry;
    Session::WorkspaceAttachRequest request;
    request.authority.endpointIdentity = "local host";
    request.workspaceID = 91;
    unsigned creations = 0;
    auto create = [&] {
        ++creations;
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };
    auto first = registry.acquire(request, create);
    REQUIRE(first);
    first->connection->refuseDuplicateAttachment();
    bool didRejectSameIdentity = false;
    bool didRejectOtherIdentity = false;
    std::thread otherEventLoop([&] {
        auto same = registry.acquire(request, create);
        didRejectSameIdentity = !same && same.error() == Session::WorkspaceConnectionRegistry::Error::DifferentEventLoop;
        auto otherRequest = request;
        otherRequest.workspaceID = 92;
        auto other = registry.acquire(otherRequest, create);
        didRejectOtherIdentity = !other && other.error() == Session::WorkspaceConnectionRegistry::Error::DifferentEventLoop;
    });
    otherEventLoop.join();
    REQUIRE(didRejectSameIdentity);
    REQUIRE(didRejectOtherIdentity);
    REQUIRE(creations == 1);
    auto replacement = registry.acquire(request, create);
    REQUIRE(replacement && !replacement->wasReused && creations == 2);
}

TEST(workspaceConnectionRegistryAliasesOnlyAcknowledgedDaemonAuthority)
{
    Fixture fixture;
    Session::WorkspaceConnectionRegistry registry;
    Session::WorkspaceAttachRequest request;
    request.authority.endpointIdentity = "ssh actual-user@actual-target";
    request.workspaceID = 91;
    unsigned creations = 0;
    auto create = [&]() -> std::shared_ptr<Session::WorkspaceConnection> {
        if (++creations == 1) {
            return { &fixture.connection, [](auto*) { } };
        }
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };
    auto first = registry.acquire(request, create);
    REQUIRE(first);
    // A structure that changes what the mirror shows is what the registry
    // hears of; the name is what changes here.
    fixture.layout(FirstPanePresence::Included, 91, 11, "resolved");
    request.authority.daemonSocket = "workspace-test.sock";
    auto resolved = registry.acquire(request, create);
    REQUIRE(resolved && resolved->wasReused && resolved->connection == first->connection);
    request.authority.daemonInstanceID = 17;
    auto sameInstance = registry.acquire(request, create);
    REQUIRE(sameInstance && sameInstance->wasReused);
    request.authority.daemonInstanceID = 18;
    auto differentInstance = registry.acquire(request, create);
    REQUIRE(differentInstance && !differentInstance->wasReused);
    request.authority.daemonInstanceID = 0;
    request.authority.daemonSocket = "another-daemon.sock";
    auto differentDaemon = registry.acquire(request, create);
    REQUIRE(differentDaemon && !differentDaemon->wasReused);
    request.authority.daemonSocket = "workspace-test.sock";
    request.authority.endpointIdentity = "ssh another-user@actual-target";
    auto differentPrincipal = registry.acquire(request, create);
    REQUIRE(differentPrincipal && !differentPrincipal->wasReused);
    REQUIRE(creations == 4);
}

// An owner that learned its workspace after it attached, as a shared window's
// does from its publication, is indexed once the host admitted it there, and
// is what the same authority's attach of that workspace reuses. A workspace
// another live owner of the same authority already carries keeps that owner,
// and neither is refused; any other authority is apart from both.
TEST(workspaceConnectionRegistryAdoptsAResolvedOwnerAndKeepsOtherAuthoritiesApart)
{
    using Registry = Session::WorkspaceConnectionRegistry;
    Registry registry;
    const Session::WorkspaceAuthority bridge { .endpointIdentity = "local bridge", .principal = {}, .daemonSocket = {}, .daemonInstanceID = 17 };
    unsigned creations = 0;
    auto create = [&] {
        ++creations;
        return std::make_shared<Session::WorkspaceConnection>(std::make_unique<Queue>(), std::make_unique<Factory>());
    };

    const auto unstarted = create();
    const auto unresolved = registry.adopt(bridge, unstarted);
    CHECK(!unresolved && unresolved.error() == Registry::Error::Unresolved);
    Session::WorkspaceAuthority unnamed = bridge;
    unnamed.endpointIdentity.clear();

    Fixture fixture;
    const std::shared_ptr<Session::WorkspaceConnection> owner { &fixture.connection, [](auto*) { } };
    const auto invalid = registry.adopt(unnamed, owner);
    CHECK(!invalid && invalid.error() == Registry::Error::InvalidAuthority);
    bool didRefuseOtherLoop = false;
    std::thread otherEventLoop([&] {
        const auto attempt = registry.adopt(bridge, owner);
        didRefuseOtherLoop = !attempt && attempt.error() == Registry::Error::DifferentEventLoop;
    });
    otherEventLoop.join();
    CHECK(didRefuseOtherLoop);
    REQUIRE(registry.adopt(bridge, owner).has_value());
    CHECK_MSG(registry.adopt(bridge, owner).has_value(), "an owner already indexed was refused as its own duplicate");

    Session::WorkspaceAttachRequest request;
    request.authority = bridge;
    request.workspaceID = 91;
    auto exact = registry.acquire(request, create);
    REQUIRE(exact && exact->wasReused && exact->connection == owner);
    request.authority.daemonInstanceID = 0;
    request.authority.daemonSocket = "workspace-test.sock";
    auto resolved = registry.acquire(request, create);
    REQUIRE(resolved && resolved->wasReused && resolved->connection == owner);
    request.workspaceID = 0;
    request.sessionID = 22;
    auto bySession = registry.acquire(request, create);
    REQUIRE(bySession && bySession->wasReused && bySession->connection == owner);
    CHECK_EQ(creations, 1u);

    request = {};
    request.workspaceID = 91;
    request.authority = bridge;
    request.authority.daemonInstanceID = 18;
    auto otherDaemon = registry.acquire(request, create);
    REQUIRE(otherDaemon && !otherDaemon->wasReused);
    request.authority = bridge;
    request.authority.endpointIdentity = "another bridge";
    auto otherBridge = registry.acquire(request, create);
    REQUIRE(otherBridge && !otherBridge->wasReused);
    request.authority = bridge;
    request.authority.principal = "someone else";
    auto otherPrincipal = registry.acquire(request, create);
    REQUIRE(otherPrincipal && !otherPrincipal->wasReused);
    CHECK_EQ(creations, 4u);

    Fixture second;
    const std::shared_ptr<Session::WorkspaceConnection> duplicate { &second.connection, [](auto*) { } };
    const auto refused = registry.adopt(bridge, duplicate);
    CHECK(!refused && refused.error() == Registry::Error::Duplicate);
    CHECK(fixture.connection.connection().state() == Session::State::Connected);
    CHECK(second.connection.connection().state() == Session::State::Connected);
    request.authority = bridge;
    auto stillFirst = registry.acquire(request, create);
    REQUIRE(stillFirst && stillFirst->wasReused && stillFirst->connection == owner);
    Session::WorkspaceAuthority otherAuthority = bridge;
    otherAuthority.endpointIdentity = "second bridge";
    CHECK(registry.adopt(otherAuthority, duplicate).has_value());
    CHECK(second.connection.connection().state() == Session::State::Connected);
    CHECK_EQ(fixture.factory->wires.size(), 1u);
    CHECK_EQ(second.factory->wires.size(), 1u);
}

// Two connections in one process carry one workspace when its panes attached
// before the workspace was known to be one. One retires into the other only
// while both are connected to the same workspace of the same daemon process,
// and never into one that already shows one of its panes, since a second view
// of a pane is not a move. Each of its panes is told once, as a pane that
// joins it while it retires is, and nothing passes back. The other's carrier
// holds a verified picture of a pane no endpoint there shows.
TEST(aConnectionRetiresIntoAnotherOfItsWorkspaceOnlyWhereNoPaneIsShownTwice)
{
    Fixture survivor;
    Fixture leaving;
    const std::shared_ptr<Session::WorkspaceConnection> survivorOwner { &survivor.connection, [](auto*) { } };
    const std::shared_ptr<Session::WorkspaceConnection> leavingOwner { &leaving.connection, [](auto*) { } };

    CHECK(!leaving.connection.retireInto(nullptr));
    CHECK(!leaving.connection.retireInto(leavingOwner));
    CHECK_MSG(!leaving.connection.retireInto(survivorOwner), "a connection retired into one that already shows its pane");
    CHECK_EQ(leaving.firstObserver.retirings, 0u);
    CHECK(!leaving.connection.successor());

    // Another workspace of the same daemon process.
    Fixture elsewhere;
    const std::shared_ptr<Session::WorkspaceConnection> elsewhereOwner { &elsewhere.connection, [](auto*) { } };
    elsewhere.connection.connection().followAdoptedSessionWorkspace();
    elsewhere.layout(FirstPanePresence::Included, 92);
    REQUIRE(elsewhere.connection.connection().workspaceID() == 92u);
    CHECK(!leaving.connection.retireInto(elsewhereOwner));
    CHECK_EQ(leaving.firstObserver.retirings, 0u);

    // The survivor's carrier pictures a pane it has no endpoint for.
    CHECK(survivor.connection.holdsPicture(22));
    CHECK(!survivor.connection.holdsPicture(33));
    survivor.first.stop();
    REQUIRE(survivor.connection.canAttachSession(11));
    REQUIRE(leaving.connection.retireInto(survivorOwner));
    CHECK(leaving.connection.successor() == survivorOwner);
    CHECK_EQ(leaving.firstObserver.retirings, 1u);
    CHECK(leaving.connection.retireInto(survivorOwner));
    CHECK_MSG(leaving.firstObserver.retirings == 1u, "a pane was told again that its connection retires");
    CHECK(!leaving.connection.retireInto(elsewhereOwner));
    CHECK_MSG(!survivor.connection.retireInto(leavingOwner), "a connection retired into one on its own way out");
    CHECK(!survivor.connection.successor());

    // A pane that joins the retiring connection is told too.
    leaving.second.start(22, 80, 24);
    leaving.queue->drain();
    REQUIRE(leaving.second.acknowledged());
    CHECK_EQ(leaving.secondObserver.retirings, 1u);
    CHECK_EQ(leaving.firstObserver.retirings, 1u);

    // Retiring is a promise about the panes, not a change to either carrier.
    CHECK(leaving.first.state() == Session::State::Connected);
    CHECK(survivor.connection.connection().state() == Session::State::Connected);
    CHECK_EQ(leaving.factory->wires.size(), 1u);
    CHECK_EQ(survivor.factory->wires.size(), 1u);
    CHECK(canonicalWrites(*leaving.factory->wires.back()));
}

// Two windows of a partial restore attach by session before the host names
// their workspace, and each opens a connection. When the later resolves to the
// workspace the earlier carries, it retires into the earlier one, whose
// topology then holds its pane; it is refused as a duplicate attachment only
// when the earlier one already shows its pane.
TEST(aLaterOwnerOfACarriedWorkspaceRetiresIntoTheEarlierUnlessItShowsTheSamePane)
{
    for (const bool isShownByEarlier : { true, false }) {
        Fixture earlier;
        Fixture later;
        Session::WorkspaceConnectionRegistry registry;
        Session::WorkspaceAttachRequest request;
        request.authority.endpointIdentity = "local host";
        request.sessionID = 11;
        unsigned creations = 0;
        auto create = [&]() -> std::shared_ptr<Session::WorkspaceConnection> {
            return { ++creations == 1 ? &earlier.connection : &later.connection, [](auto*) { } };
        };
        auto earlierLease = registry.acquire(request, create);
        REQUIRE(earlierLease && !earlierLease->wasReused);
        earlier.layout(FirstPanePresence::Included, 91, 11, "restored");
        if (!isShownByEarlier)
            earlier.first.stop();
        // A session the earlier topology does not hold, so nothing is reused.
        request.sessionID = 33;
        auto laterLease = registry.acquire(request, create);
        REQUIRE(laterLease && !laterLease->wasReused);
        REQUIRE(laterLease->connection.get() == &later.connection);
        later.layout(FirstPanePresence::Included, 91, 11, "restored");

        if (isShownByEarlier) {
            CHECK_MSG(later.first.state() == Session::State::Failed, "a second view of a pane was kept as a move");
            CHECK(later.firstObserver.lastDetail.find("already attached") != std::string::npos);
            CHECK_EQ(later.firstObserver.retirings, 0u);
            CHECK(!later.connection.successor());
        } else {
            CHECK_MSG(later.first.state() == Session::State::Connected, "a movable owner was refused as a duplicate");
            CHECK_EQ(later.firstObserver.retirings, 1u);
            CHECK(later.connection.successor() == earlierLease->connection);
        }
        CHECK(earlier.connection.connection().state() == Session::State::Connected);
        // The earlier owner stays the one an attach of the workspace reuses.
        Session::WorkspaceAttachRequest byWorkspace;
        byWorkspace.authority.endpointIdentity = "local host";
        byWorkspace.workspaceID = 91;
        auto reused = registry.acquire(byWorkspace, create);
        REQUIRE(reused && reused->wasReused);
        CHECK(reused->connection == earlierLease->connection);
        CHECK_EQ(creations, 2u);
    }
}

/// The last greeting a carrier carried.
static std::optional<Sync::Greet> lastGreet(const Wire& wire)
{
    Sync::FrameDecoder decoder;
    std::optional<Sync::Greet> greet;
    for (const auto& bytes : wire.sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (frame->kind == Sync::FrameKind::Greet)
                greet = Sync::decodeGreet(frame->body);
        }
    }
    return greet;
}

// A shared window's carrier keeps to the workspace it published and greets
// again by a session that workspace still holds: the pane it was opened
// through can be reclaimed while the others go on. A carrier that follows its
// adopted session and is not kept to a workspace greets by that session.
TEST(aSharedWindowsCarrierGreetsAgainByASessionItsWorkspaceStillHolds)
{
    for (const bool isKeptToWorkspace : { true, false }) {
        Fixture fixture;
        auto& physical = fixture.connection.connection();
        physical.followAdoptedSessionWorkspace();
        if (isKeptToWorkspace)
            physical.setAttachWorkspaceID(91);
        fixture.second.start(22, 80, 24);
        fixture.queue->drain();
        REQUIRE(fixture.second.acknowledged());
        fixture.layout(FirstPanePresence::Absent);
        REQUIRE(fixture.firstObserver.exits == 1);
        REQUIRE(fixture.second.acknowledged());
        CHECK_EQ(physical.sessionID(), isKeptToWorkspace ? 22u : 11u);
        fixture.factory->wires.back()->observer->onClose("drop");
        fixture.queue->drain();
        fixture.queue->advance();
        REQUIRE(fixture.factory->wires.size() == 2);
        const auto greet = lastGreet(*fixture.factory->wires.back());
        REQUIRE(greet.has_value());
        CHECK_EQ(greet->request.sessionID, isKeptToWorkspace ? 22u : 11u);
        CHECK_EQ(greet->request.workspaceID, isKeptToWorkspace ? 91u : 0u);
        CHECK(fixture.secondObserver.exits == 0);
    }
}

// Every pane of one physical owner asks for its own reclaim, as stopping a
// shared window does. The host settles one reclaim per attachment and refuses
// another while one awaits release, so the asks reach it one at a time: each
// answer, a refusal included, lets the next go, and each exact answer hands
// back only the pane it names.
TEST(everyPaneOfOneOwnerIsReclaimedByItsOwnAnswer)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    REQUIRE(fixture.first.acknowledged() && fixture.second.acknowledged());
    auto& wire = *fixture.factory->wires.back();
    // The one reclaim the host holds, and the session it names.
    const auto pendingReclaim = [&](std::uint32_t& sessionID) -> std::optional<Sync::ReceivedCall> {
        fixture.feed(fixture.host->publish(wire));
        auto calls = fixture.host->connection().takeCalls();
        CHECK_MSG(calls.size() <= 1, "a second reclaim reached the host while one awaited release");
        if (calls.empty())
            return std::nullopt;
        const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
        if (request.method != "pane.reclaim")
            return std::nullopt;
        const auto intent = Protocol::Control::readReclaimRequest(request.params);
        if (!intent)
            return std::nullopt;
        sessionID = intent->handle == fixture.host->paneHandle(11) ? 11u : 22u;
        return calls.front();
    };
    const auto release = [&](const Sync::ReceivedCall& call, std::uint32_t sessionID) {
        const auto request = Protocol::Control::decodeRequest(call.text, {});
        const Protocol::Control::ReclaimTarget target { .handle = fixture.host->paneHandle(sessionID), .sessionID = sessionID, .daemonInstanceID = 17, .hostInstanceID = { 17, 0 }, .attachmentID = { 1, 17 }, .paneID = { sessionID, 17 } };
        CHECK(fixture.host->connection().answerCall(call.callIndex, Codec::JSON::serialize(Protocol::Control::reclaimReleasedResult(request.idOrNull(), target)))
            == Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->publish(wire));
    };
    const auto refuse = [&](const Sync::ReceivedCall& call) {
        const auto request = Protocol::Control::decodeRequest(call.text, {});
        CHECK(fixture.host->connection().answerCall(call.callIndex,
                  Codec::JSON::serialize(Protocol::Control::rpcError(request.idOrNull(), Protocol::Control::ErrorCode::Busy, "a reclaim is awaiting release")))
            == Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->publish(wire));
    };

    REQUIRE(fixture.first.requestReclaim());
    REQUIRE(fixture.second.requestReclaim());
    CHECK_MSG(!fixture.second.requestReclaim(), "one pane asked for its reclaim twice");
    fixture.queue->drain();
    std::uint32_t asked = 0;
    auto call = pendingReclaim(asked);
    REQUIRE(call && asked == 11);
    // A refusal leaves its pane shared, and lets the next pane's ask go.
    refuse(*call);
    CHECK_EQ(fixture.firstObserver.reclaims, 0u);
    call = pendingReclaim(asked);
    REQUIRE(call && asked == 22);
    release(*call, 22);
    CHECK_EQ(fixture.secondObserver.reclaims, 1u);
    CHECK_EQ(fixture.firstObserver.reclaims, 0u);
    // The refused pane may ask again, and its own answer hands it back.
    REQUIRE(fixture.first.requestReclaim());
    fixture.queue->drain();
    call = pendingReclaim(asked);
    REQUIRE(call && asked == 11);
    release(*call, 11);
    CHECK_EQ(fixture.firstObserver.reclaims, 1u);
    CHECK_EQ(fixture.firstObserver.exits, 0u);
    CHECK_EQ(fixture.secondObserver.exits, 0u);
    CHECK_EQ(fixture.factory->wires.size(), 1u);
}

// A valid typed record cannot invent a binding for a handle the host never sent.
static std::vector<uint8_t> unboundPaneRecord()
{
    std::vector<std::uint8_t> body, frame;
    Sync::encodePaneFacts(body, Sync::PaneFacts { .revision = 1, .programTitle = "unbound" });
    const auto written = Sync::appendFrame(frame, Sync::FrameKind::PaneFacts, UINT32_MAX, body);
    CHECK(written);
    return frame;
}

// A pane that left the layout may still have its exit in flight, and the host
// may retire many; neither ends the carrier. A frame for a pane the workspace
// never held does.
TEST(workspaceConnectionToleratesRetiredExitsAndRefusesForeignPanes)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.layout(FirstPanePresence::Absent);
    for (uint32_t identity = 100; identity < 400; ++identity) {
        fixture.layout(FirstPanePresence::Included, 91, identity);
        (void)fixture.host->pane(identity);
        fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
        fixture.layout(FirstPanePresence::Absent);
        // The session is gone, so the daemon unbinds its pane: a connection
        // holds at most `maxPanesPerConnection` bound at once.
        fixture.feed(fixture.host->retirePane(identity, *fixture.factory->wires.back()));
    }
    REQUIRE(!fixture.factory->wires.back()->stopped);
    REQUIRE(fixture.second.acknowledged());
    fixture.feed(unboundPaneRecord());
    REQUIRE(fixture.factory->wires.back()->stopped);
}

TEST(workspaceConnectionPinsFreshDaemonBeforeReconnect)
{
    Fixture fixture;
    REQUIRE(fixture.factory->daemonPins == std::vector<std::string> { "workspace-test.sock" });
    fixture.factory->wires.back()->observer->onClose("retry");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.handshake();
    REQUIRE(fixture.factory->daemonPins == (std::vector<std::string> { "workspace-test.sock", "workspace-test.sock" }));
    fixture.factory->wires.back()->observer->onClose("retry again");
    fixture.queue->drain();
    fixture.queue->advance();
    fixture.host = std::make_unique<WorkspaceHost>("different-daemon.sock");
    const auto welcome = fixture.host->publish(*fixture.factory->wires.back());
    REQUIRE(welcome.size() > 1);
    fixture.feed({ welcome.begin(), welcome.end() - 1 });
    CHECK(!fixture.factory->wires.back()->stopped);
    CHECK(!fixture.first.acknowledged());
    fixture.feed({ welcome.end() - 1, welcome.end() });
    REQUIRE(fixture.factory->wires.back()->stopped);
    REQUIRE(fixture.connection.connection().state() == Session::State::Failed);
    REQUIRE(fixture.factory->daemonPins.size() == 2);
}

// Input is the pane's own: each pane core's goes to its pane's program on the
// one carrier, and after a reconnect to a host that cannot say what it wrote,
// the pane whose input is in doubt is the one whose observer hears of it.
TEST(workspacePaneInputIsItsOwnAndItsDoubtIsToldToItsObserver)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    REQUIRE(fixture.first.send(Protocol::Sync::RawInput { .bytes = { 'a' } }));
    REQUIRE(fixture.second.send(Protocol::Sync::RawInput { .bytes = { 'b' } }));
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(fixture.host->typed(11) == "a");
    CHECK(fixture.host->typed(22) == "b");

    // The carrier dies with the second pane's next key unread by the host.
    REQUIRE(fixture.second.send(Protocol::Sync::RawInput { .bytes = { 'c' } }));
    fixture.factory->wires.back()->observer->onClose("reconnect");
    fixture.queue->drain();
    fixture.queue->advance();
    fixture.handshake();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    REQUIRE(fixture.secondObserver.inputs.size() == 1);
    CHECK(fixture.secondObserver.inputs.back().inDoubt == 1);
    CHECK(fixture.second.inputStatus().isAwaitingDecision());
    CHECK(fixture.firstObserver.inputs.empty());
    CHECK(!fixture.first.inputStatus().isAwaitingDecision());

    fixture.second.resendInDoubt();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(fixture.host->typed(22) == "c");
    CHECK(fixture.host->typed(11).empty());
    REQUIRE(fixture.secondObserver.inputs.size() == 2);
    CHECK(fixture.secondObserver.inputs.back().inDoubt == 0);
}

TEST(workspaceConnectionRejectsNewSiblingDuringReconnect)
{
    Fixture fixture;
    fixture.factory->wires.back()->observer->onClose("reconnect");
    fixture.queue->drain();
    REQUIRE(!fixture.connection.canAttachSession(22));
    fixture.second.start(22, 80, 24);
    REQUIRE(!fixture.second.acknowledged());
    REQUIRE(fixture.secondObserver.established == 0);
    fixture.queue->advance();
    fixture.handshake();
    REQUIRE(fixture.first.acknowledged());
    REQUIRE(fixture.connection.canAttachSession(22));
    REQUIRE(fixture.secondObserver.established == 0);
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.second.acknowledged());
    REQUIRE(fixture.secondObserver.established == 1);
}

static unsigned resyncRequests(const Wire& wire)
{
    unsigned count = 0;
    Sync::FrameDecoder decoder;
    for (const auto& bytes : wire.sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (frame->kind == Sync::FrameKind::Resync)
                ++count;
        }
    }
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    return count;
}

// The host may bind a pane and send its checkpoint before any endpoint on
// this client attached to the session. The connection holds that screen in a
// terminal of its own; an endpoint that attaches later swaps its mirror in and
// asks for a checkpoint, since the mirror holds none of what was applied.
TEST(workspaceConnectionCatchesUpAPaneBoundBeforeItsEndpoint)
{
    Fixture fixture;
    fixture.layout(FirstPanePresence::Included, 91, 33);
    processBytes(&fixture.host->pane(33), "caught up");
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    REQUIRE(resyncRequests(*fixture.factory->wires.back()) == 0);

    auto mirror = std::make_unique<Terminal>();
    Observer observer;
    Session::RemoteSessionCore endpoint(mirror.get(), fixture.connection, &observer);
    endpoint.start(33, 80, 24);
    fixture.queue->drain();
    REQUIRE(resyncRequests(*fixture.factory->wires.back()) == 1);
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const auto* cell = Screen::visibleCell(mirror.get(), 0, 0, 0);
    REQUIRE(cell && cell->codePoint == 'c');
    REQUIRE(observer.screens == 1);
}

namespace Ubiquitty::Session {

/// Reads the picture a workspace carrier holds for a pane, to prove which
/// terminal it is in.
struct RemoteSessionCarrierPictureAccess {
    [[nodiscard]] static const Terminal* picture(const RemoteSessionCore& carrier, Protocol::Sync::ID128 paneID)
    {
        const auto* attachment = carrier.m_core->attachment();
        return attachment ? attachment->paneTerminal(paneID) : nullptr;
    }
};

} // namespace Ubiquitty::Session

/// Binds pane 33 into an endpoint's own terminal and returns the pane's ID.
static Protocol::Sync::ID128 lendPaneThirtyThree(Fixture& fixture, Session::RemoteSessionCore& endpoint)
{
    endpoint.start(33, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    return { .low = 33, .high = 17 };
}

// A pane's endpoint lends its terminal to the carrier, which a workspace
// shares with other panes and keeps after the endpoint. One destroyed without
// a stop takes the loan back as one stopped does: the carrier binds the pane
// to a terminal of its own and asks for its checkpoint.
TEST(workspaceConnectionTakesBackATerminalLentByAnEndpointDestroyedWithoutStop)
{
    Fixture fixture;
    fixture.layout(FirstPanePresence::Included, 91, 33);
    processBytes(&fixture.host->pane(33), "lent");
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));

    auto mirror = std::make_unique<Terminal>();
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(mirror.get(), fixture.connection, &observer);
    const auto paneID = lendPaneThirtyThree(fixture, *endpoint);
    REQUIRE(Session::RemoteSessionCarrierPictureAccess::picture(fixture.connection.connection(), paneID) == mirror.get());
    const auto resyncsBefore = resyncRequests(*fixture.factory->wires.back());
    const auto* cell = Screen::visibleCell(mirror.get(), 0, 0, 0);
    REQUIRE(cell && cell->codePoint == 'l');

    const auto lent = reinterpret_cast<std::uintptr_t>(mirror.get());
    endpoint.reset();
    mirror.reset();
    fixture.queue->drain();
    const auto* held = Session::RemoteSessionCarrierPictureAccess::picture(fixture.connection.connection(), paneID);
    CHECK(held != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(held) != lent);
    CHECK(resyncRequests(*fixture.factory->wires.back()) == resyncsBefore + 1);
}

// A pane the host unbinds stays held for a rebind, in the picture it had: the
// endpoint's terminal. That endpoint going away takes the picture with it,
// so a held replica never names a terminal its lender freed.
TEST(workspaceConnectionHoldsNoUnboundPaneInATerminalItsEndpointTookBack)
{
    Fixture fixture;
    fixture.layout(FirstPanePresence::Included, 91, 33);
    processBytes(&fixture.host->pane(33), "lent");
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));

    auto mirror = std::make_unique<Terminal>();
    Observer observer;
    auto endpoint = std::make_unique<Session::RemoteSessionCore>(mirror.get(), fixture.connection, &observer);
    const auto paneID = lendPaneThirtyThree(fixture, *endpoint);
    REQUIRE(Session::RemoteSessionCarrierPictureAccess::picture(fixture.connection.connection(), paneID) == mirror.get());
    fixture.feed(fixture.host->retirePane(33, *fixture.factory->wires.back()));
    fixture.queue->drain();

    const auto lent = reinterpret_cast<std::uintptr_t>(mirror.get());
    endpoint.reset();
    mirror.reset();
    fixture.queue->drain();
    CHECK(reinterpret_cast<std::uintptr_t>(Session::RemoteSessionCarrierPictureAccess::picture(fixture.connection.connection(), paneID)) != lent);
}

TEST(workspaceConnectionRejectsReusedIDsFromAnotherDaemonInstance)
{
    Fixture fixture;
    REQUIRE(fixture.connection.connection().daemonInstanceID() == 17);
    fixture.factory->wires.back()->observer->onClose("daemon stopped");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    const auto& sent = fixture.factory->wires.back()->sent;
    REQUIRE(!sent.empty());
    Sync::FrameDecoder decoder;
    bool sawPinnedHello = false;
    for (const auto& bytes : sent) {
        decoder.append(bytes);
        while (const auto outgoing = decoder.next()) {
            if (outgoing->kind != Sync::FrameKind::Greet)
                continue;
            const auto hello = Sync::decodeGreet(outgoing->body);
            sawPinnedHello = hello && hello->request.expectedDaemonInstanceID == 17
                && hello->request.target == Sync::AdmissionTarget::Workspace
                && hello->request.sessionID == 11 && hello->request.workspaceID == 91 && hello->request.isAttachOnly;
        }
    }
    REQUIRE(sawPinnedHello);
    Protocol::Sync::Welcome welcome;
    welcome.wireVersion = Protocol::Sync::wireVersion;
    welcome.hostInstanceID = { 18, 0 };
    welcome.attachmentID = { 1, 18 };
    welcome.resumeSecret = { 2, 18 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = 11,
        .workspaceID = 91,
        .daemonInstanceID = 18,
        .socketName = "workspace-test.sock",
        .hostPlatform = "fixture" };
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, welcome);
    std::vector<std::uint8_t> envelope;
    REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Welcome, 0, body));
    REQUIRE(envelope.size() > 1);
    fixture.feed({ envelope.begin(), envelope.end() - 1 });
    CHECK(fixture.connection.connection().state() == Session::State::Connecting);
    CHECK(!fixture.first.acknowledged());
    CHECK(fixture.connection.connection().daemonInstanceID() == 17);
    fixture.feed({ envelope.end() - 1, envelope.end() });
    REQUIRE(fixture.connection.connection().state() == Session::State::Failed);
    REQUIRE(fixture.factory->wires.back()->stopped);
}

/// A pane resized while its connection is down asks for that grid once its
/// workspace is admitted again.
///
/// A pane sends no HELLO of its own, so nothing else carries the grid the
/// frontend asked for, and the first snapshot after the reconnect would
/// otherwise settle the pane back at the grid it had before.
TEST(workspaceConnectionAsksForTheGridAPaneWasResizedToWhileReconnecting)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.second.acknowledged());
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    REQUIRE(!fixture.second.acknowledged());

    fixture.second.setSize(46, 28);
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.handshake();
    REQUIRE(fixture.second.acknowledged());

    // The size is kept for the pane's bind: the host binds it, the client
    // reads the bind, and what it then writes is the pane's viewport.
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.queue->advance();
    const std::optional<Protocol::Sync::Viewport> viewport = fixture.host->takePaneViewport(22, wire);
    REQUIRE(viewport.has_value());
    CHECK(viewport->columns == 46);
    CHECK(viewport->rows == 28);
}

/// A pane joins a workspace before its view is laid out, holding the grid it
/// was launched with. That grid is nobody's drawable, so it is not stated:
/// stated, it would size the host's pane to the launch grid until the layout
/// arrived, a resize the program is told about for a size no one has.
TEST(workspaceConnectionStatesNoSizeForAPaneNotLaidOut)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    REQUIRE(fixture.second.acknowledged());
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.queue->advance();
    CHECK(!fixture.host->takePaneViewport(22, wire).has_value());
    CHECK(fixture.second.trace().totals().viewportsStated == 0);

    // The layout arrives: the grid the drawable holds is stated, once.
    fixture.second.setSize(132, 43);
    fixture.queue->advance();
    const std::optional<Protocol::Sync::Viewport> viewport = fixture.host->takePaneViewport(22, wire);
    REQUIRE(viewport.has_value());
    CHECK(viewport->columns == 132);
    CHECK(viewport->rows == 43);
    CHECK(fixture.second.trace().totals().viewportsStated == 1);
    fixture.second.setSize(132, 43);
    fixture.queue->advance();
    CHECK(!fixture.host->takePaneViewport(22, wire).has_value());
    CHECK(fixture.second.trace().totals().viewportsStated == 1);
}

namespace {

/// The fixture's workspace as one window of two tabs, a pane in each.
///
/// - Parameters:
///   - activeTab: The index of the tab the host has in front.
///   - revision: The structure's revision.
///   - name: The workspace's name; a different one makes a structure the
///     mirror adopts when nothing else in it moved.
Session::TreeWorkspace twoTabWorkspace(std::size_t activeTab, std::uint64_t revision, const std::string& name = {})
{
    const std::array<UbiquittyTest::StructurePane, 1> first { { { .pane = 11, .sessionID = 11 } } };
    const std::array<UbiquittyTest::StructurePane, 1> second { { { .pane = 22, .sessionID = 22 } } };
    Session::TreeWorkspace workspace = UbiquittyTest::rowWorkspace(91, name, first, 17, revision);
    Session::TreeTab other = UbiquittyTest::rowWorkspace(91, name, second, 17, revision).windows.front().tabs.front();
    other.id = Session::Layout::TabID { 2 };
    Session::TreeWindow& window = workspace.windows.front();
    window.tabs.push_back(std::move(other));
    window.hostActiveTab = activeTab;
    for (std::size_t index = 0; index < window.tabs.size(); ++index)
        window.tabs[index].isHostActive = index == activeTab;
    return workspace;
}

} // namespace

/// The tab a person brings to the front is asked of the host once and stays
/// in front here until the host has answered.
///
/// Which tab a window shows is the host's to say. A client that selected the
/// tab locally and said nothing would be put back on the host's tab by the
/// next structure; one that asked and then showed whatever the mirror holds
/// would be put back for a round trip by a structure the host sent before it
/// read the ask. So the ask is held: `frontTab` answers the tab asked for
/// until a structure shows it, the host refuses it, or the host performs it
/// and names another tab since.
TEST(aTabBroughtToFrontIsAskedOfTheHostAndHeldUntilItAnswers)
{
    namespace Layout = Session::Layout;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    const Layout::SurfaceID window { 1 };
    const Layout::TabID one { 1 };
    const Layout::TabID two { 2 };
    const auto structure = [&](std::size_t activeTab, const std::string& name = {}) {
        fixture.feed(fixture.host->structure(twoTabWorkspace(activeTab, ++fixture.structureRevision, name), wire));
    };
    const auto hostTab = [&] { return fixture.first.workspace()->activeTab(window); };
    std::vector<Protocol::Sync::ReceivedCall> calls;
    const auto collectCalls = [&] {
        fixture.feed(fixture.host->publish(wire));
        for (auto& call : fixture.host->connection().takeCalls())
            calls.push_back(std::move(call));
        CHECK(canonicalWrites(wire));
    };
    const auto requestAt = [&](std::size_t index) {
        bool isJSON = false; // bool-ok: parser output
        auto request = Codec::JSON::parse(calls.at(index).text, isJSON);
        CHECK(isJSON && request.stringOr("method") == "tab.activate");
        return request;
    };
    const auto answerAt = [&](std::size_t index, bool wasPerformed) { // bool-ok: the fixture's existing result decision
        const auto request = requestAt(index);
        const auto tab = static_cast<std::uint32_t>(request.find("params")->numberOr("tab"));
        const auto body = wasPerformed ? R"("result":{"window":"window:1","tab":"tab:)" + std::to_string(tab) + R"("})"
                                       : std::string(R"("error":{"code":-32001,"message":"refused"})");
        CHECK(fixture.host->connection().answerCall(calls.at(index).callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(",)" + body + "}")
            == Protocol::Sync::CallAnswer::Sent);
        fixture.feed(fixture.host->flush());
    };

    structure(1);
    REQUIRE(fixture.first.workspace() != nullptr);
    CHECK(hostTab() == two);
    CHECK(fixture.first.frontTab(window) == two);
    CHECK(fixture.second.frontTab(window) == two);

    // The tab already in front: nothing to ask.
    CHECK(!fixture.second.bringTabToFront(window, two));
    CHECK(canonicalWrites(wire));
    CHECK(!fixture.first.bringTabToFront(Layout::invalidSurface, one));
    CHECK(!fixture.first.bringTabToFront(window, Layout::invalidTab));

    // The person chooses the first tab: one typed call names that tab.
    CHECK(fixture.first.bringTabToFront(window, one));
    collectCalls();
    REQUIRE(calls.size() == 1);
    CHECK(requestAt(0).find("params")->numberOr("tab") == 1);
    // Every pane of the workspace reads the one answer; the mirror is still the host's.
    CHECK(fixture.first.frontTab(window) == one);
    CHECK(fixture.second.frontTab(window) == one);
    CHECK(hostTab() == two);
    // Asked already: a second report of the same choice sends nothing.
    CHECK(!fixture.first.bringTabToFront(window, one));
    CHECK(!fixture.second.bringTabToFront(window, one));
    collectCalls();
    CHECK(calls.size() == 1);

    // A structure the host sent before it read the ask still names its own
    // tab, and does not put that tab back in front here.
    structure(1, "sent before the ask was read");
    CHECK(hostTab() == two);
    CHECK(fixture.first.frontTab(window) == one);

    // The answer, then the structure that shows it.
    answerAt(0, true);
    CHECK(fixture.first.frontTab(window) == one);
    structure(0, "sent before the ask was read");
    CHECK(hostTab() == one);
    CHECK(fixture.first.frontTab(window) == one);

    // Another client chooses the second tab: the host's word, shown here.
    structure(1, "sent before the ask was read");
    CHECK(fixture.first.frontTab(window) == two);

    // A refusal leaves the host's tab in front.
    CHECK(fixture.first.bringTabToFront(window, one));
    collectCalls();
    REQUIRE(calls.size() == 2);
    CHECK(fixture.first.frontTab(window) == one);
    answerAt(1, false);
    CHECK(fixture.first.frontTab(window) == two);

    // An ask the host performed yields to whatever its next structure names:
    // a tab another client chose since is the later word.
    CHECK(fixture.first.bringTabToFront(window, one));
    collectCalls();
    REQUIRE(calls.size() == 3);
    answerAt(2, true);
    CHECK(fixture.first.frontTab(window) == one);
    structure(1, "chosen elsewhere since");
    CHECK(fixture.first.frontTab(window) == two);

    // The person changes their mind before the host answers: the later ask
    // replaces the earlier, and each is sent.
    CHECK(fixture.first.bringTabToFront(window, one));
    CHECK(fixture.second.bringTabToFront(window, two));
    collectCalls();
    REQUIRE(calls.size() == 5);
    CHECK(requestAt(4).find("params")->numberOr("tab") == 2);
    CHECK(fixture.first.frontTab(window) == two);
    answerAt(3, true);
    CHECK(fixture.first.frontTab(window) == two);

    // A connection that drops takes its unanswered asks with it.
    CHECK(fixture.first.bringTabToFront(window, one));
    CHECK(fixture.first.frontTab(window) == one);
    fixture.factory->wires.back()->observer->onClose("drop");
    fixture.queue->drain();
    fixture.queue->advance();
    REQUIRE(fixture.factory->wires.size() == 2);
    fixture.handshake();
    CHECK(fixture.first.frontTab(window) == fixture.first.workspace()->activeTab(window));
}

TEST(retiredTopologyReplyHeadersCannotCompleteATypedWorkspaceCall)
{
    namespace Layout = Session::Layout;
    for (const std::uint8_t raw : { 0x9E, 0xA5 }) {
        Fixture fixture;
        fixture.second.start(22, 80, 24);
        Wire& wire = *fixture.factory->wires.back();
        fixture.feed(fixture.host->structure(twoTabWorkspace(1, ++fixture.structureRevision), wire));
        REQUIRE(fixture.first.bringTabToFront(Layout::SurfaceID { 1 }, Layout::TabID { 1 }));
        fixture.feed(fixture.host->publish(wire));
        REQUIRE(fixture.host->connection().takeCalls().size() == 1);
        CHECK(canonicalWrites(wire));
        REQUIRE(fixture.connection.connection().state() == Session::State::Connected);
        // Numeric retired outer headers are negative input, not an active codec.
        const std::vector<std::uint8_t> retired { raw, 0, 0, 0, 0 };
        fixture.feed(retired);
        CHECK(fixture.firstObserver.workspaceResults.empty());
        CHECK(fixture.secondObserver.workspaceResults.empty());
        CHECK(wire.stopped);
        CHECK(fixture.connection.connection().state() == Session::State::Reconnecting);
        CHECK(fixture.firstObserver.lastDetail == "The connection ended: an unknown Sync frame kind.");
        CHECK(fixture.factory->wires.size() == 1);
    }
}

TEST(paneLinksReadOnlyTheirOwnWorkspaceReplicaAndHistory)
{
    namespace Sync = Protocol::Sync;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.feed(fixture.host->publish(wire));
    Session::PaneLink& first = fixture.first;
    Session::PaneLink& second = fixture.second;
    REQUIRE(first.view().history);
    REQUIRE(second.view().history);
    CHECK(first.view().terminal == fixture.firstTerm.get());
    CHECK(second.view().terminal == fixture.secondTerm.get());
    CHECK(first.view().history != second.view().history);
    CHECK(first.view().history->key().paneID.low == 11);
    CHECK(second.view().history->key().paneID.low == 22);

    Sync::PaneFacts facts;
    facts.programTitle = "second pane only";
    fixture.feed(fixture.host->records(22, facts, wire));
    REQUIRE(second.view().facts);
    CHECK(second.view().facts->programTitle == facts.programTitle);
    CHECK(!first.view().facts || first.view().facts->programTitle.empty());

    const auto before = wire.sent.size();
    const std::array<Sync::LineRange, 1> wanted { Sync::LineRange { 0, 1 } };
    REQUIRE(second.wantHistory(Sync::HistoryPriority::Viewport, wanted));
    bool foundWant = false; // bool-ok: records whether the decoder saw this request
    Sync::FrameDecoder decoder;
    for (std::size_t index = before; index < wire.sent.size(); ++index) {
        decoder.append(wire.sent[index]);
        while (const auto envelope = decoder.next()) {
            if (envelope->kind != Sync::FrameKind::HistoryWant)
                continue;
            const auto decoded = Sync::decodeHistoryWant(envelope->body);
            REQUIRE(decoded);
            CHECK(decoded->ranges == std::vector<Sync::LineRange>(wanted.begin(), wanted.end()));
            CHECK(decoded->historyEpoch == second.view().history->key().epoch);
            CHECK(envelope->channel == fixture.host->paneHandle(22));
            CHECK(fixture.host->connection().paneView(envelope->channel).has_value());
            foundWant = true;
        }
    }
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    CHECK(foundWant);
}

TEST(paneLinksRefuseInvalidAndDetachedHistoryWithoutSending)
{
    namespace Sync = Protocol::Sync;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.feed(fixture.host->publish(wire));
    Session::PaneLink& link = fixture.second;
    const std::array<Sync::LineRange, 1> valid { Sync::LineRange { 0, 1 } };
    const std::array<Sync::LineRange, 1> empty { Sync::LineRange { 9, 9 } };
    const auto before = wire.sent.size();
    CHECK(!link.wantHistory(Sync::HistoryPriority::Viewport, empty));
    CHECK(!link.wantHistory(static_cast<Sync::HistoryPriority>(255), valid));
    CHECK(wire.sent.size() == before);
    fixture.second.stop();
    const auto afterStop = wire.sent.size();
    CHECK(!link.wantHistory(Sync::HistoryPriority::Viewport, valid));
    CHECK(wire.sent.size() == afterStop);
    CHECK(link.linkState(60'000'000).phase == Session::LinkPhase::Ended);
    CHECK(link.view().terminal == fixture.secondTerm.get());
    CHECK(!link.view().history);
    CHECK(!link.view().facts);
}

TEST(paneLinksKeepLossSeparateFromTheTwoStallReasons)
{
    Fixture fixture;
    Session::PaneLink& link = fixture.first;
    CHECK(link.linkState(1'000'000).phase == Session::LinkPhase::Live);
    fixture.factory->wires.back()->observer->onClose("mock carrier dropped");
    fixture.queue->drain();
    const auto lost = link.linkState(60'000'000);
    CHECK(lost.phase == Session::LinkPhase::Lost);
    CHECK(!lost.isStalled());
    CHECK(link.view().terminal == fixture.firstTerm.get());
    fixture.first.stop();
    CHECK(link.linkState(60'000'000).phase == Session::LinkPhase::Ended);
}

TEST(workspacePaneLinksUseThePhysicalOwnersFullWidthClock)
{
    Fixture fixture;
    const std::uint64_t now = 12'000'000'000;
    fixture.connection.connection().setClock([sample = now] { return sample; });
    fixture.second.start(22, 80, 24);
    Session::PaneLink& first = fixture.first;
    Session::PaneLink& second = fixture.second;
    CHECK_EQ(first.clockMicroseconds(), now);
    CHECK_EQ(second.clockMicroseconds(), now);
}

TEST(workspacePaneViewportFactsUseOnlyTheirPhysicalPane)
{
    namespace Sync = Protocol::Sync;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.feed(fixture.host->publish(wire));
    Session::PaneLink& second = fixture.second;
    const int firstRows = fixture.firstTerm->rows;
    const int firstColumns = fixture.firstTerm->columns;
    const int secondRows = fixture.secondTerm->rows;
    const int secondColumns = fixture.secondTerm->columns;
    const auto before = wire.sent.size();
    second.setViewport({ .rows = 31, .columns = 96, .widthInPixels = 999, .heightInPixels = 998 });
    fixture.queue->drain();
    struct Report {
        std::uint32_t pane;
        Sync::Viewport viewport;
    };
    std::vector<Report> reports;
    const auto collectReports = [&](std::size_t start) {
        Sync::FrameDecoder decoder;
        for (std::size_t index = start; index < wire.sent.size(); ++index) {
            decoder.append(wire.sent[index]);
            while (const auto envelope = decoder.next()) {
                if (envelope->kind != Sync::FrameKind::Viewport)
                    continue;
                const auto decoded = Sync::decodeViewport(envelope->body);
                REQUIRE(decoded);
                reports.push_back({ envelope->channel, *decoded });
            }
        }
        CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    };
    collectReports(before);
    REQUIRE(reports.size() == 1);
    const auto paneHandle = reports.front().pane;
    CHECK(paneHandle != 0);
    CHECK_EQ(paneHandle, fixture.host->paneHandle(22));
    CHECK(paneHandle != fixture.host->paneHandle(11));
    CHECK_EQ(reports.front().viewport.rows, 31);
    CHECK_EQ(reports.front().viewport.columns, 96);
    CHECK_EQ(reports.front().viewport.textAreaPixelWidth, 0);
    // The report is sent through the physical attachment for this endpoint,
    // without modifying either verified model while the host has not replied.
    CHECK_EQ(fixture.firstTerm->rows, firstRows);
    CHECK_EQ(fixture.firstTerm->columns, firstColumns);
    CHECK_EQ(fixture.secondTerm->rows, secondRows);
    CHECK_EQ(fixture.secondTerm->columns, secondColumns);

    const auto beforePixels = wire.sent.size();
    second.setPixelGeometry({ { 17, 29 }, { 1632, 899 }, { 1644, 911 } });
    second.setWindowSize(42, 43, 777, 778);
    fixture.queue->drain();
    collectReports(beforePixels);
    REQUIRE(reports.size() == 2);
    CHECK_EQ(reports.back().pane, paneHandle);
    CHECK((reports.back().viewport == Sync::Viewport { .rows = 31, .columns = 96, .cellPixelWidth = 17, .cellPixelHeight = 29, .textAreaPixelWidth = 1632, .textAreaPixelHeight = 899, .drawablePixelWidth = 1644, .drawablePixelHeight = 911 }));
    CHECK_EQ(fixture.secondTerm->rows, secondRows);
    CHECK_EQ(fixture.secondTerm->columns, secondColumns);
}

TEST(workspaceTypographyFactsReachThePhysicalAttachmentFromEitherPane)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 9 * 64;
    facts.cellHeight = 18 * 64;
    facts.faceCount = 1;
    facts.faces[0].lineHeight = 2048;
    facts.faces[0].averageAdvance = 1024;
    facts.faces[0].advances.fill(1024);
    fixture.second.publishTypography(facts);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const auto secondHandle = fixture.host->paneHandle(22);
    const auto firstHandle = fixture.host->paneHandle(11);
    REQUIRE(fixture.host->connection().typography(secondHandle));
    CHECK(*fixture.host->connection().typography(secondHandle) == facts);
    CHECK(!fixture.host->connection().typography(firstHandle));
    const auto secondPublication = fixture.host->connection().typography(secondHandle);
    fixture.second.publishTypography(facts);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(fixture.host->connection().typography(secondHandle) == secondPublication);
    facts.cellWidth = 10 * 64;
    fixture.first.publishTypography(facts);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    REQUIRE(fixture.host->connection().typography(firstHandle));
    CHECK(*fixture.host->connection().typography(firstHandle) == facts);
    CHECK(fixture.host->connection().typography(secondHandle) == secondPublication);
    CHECK(fixture.host->connection().typography(secondHandle)->cellWidth != facts.cellWidth);
}

TEST(aDetachedWorkspaceSurfaceWithdrawsItsRendererUntilItPublishesAgain)
{
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 9 * 64;
    facts.cellHeight = 18 * 64;
    facts.rendersBlocks = 1;
    facts.shaping = 1;
    facts.faceCount = 1;
    facts.faces[0].lineHeight = 2048;
    facts.faces[0].averageAdvance = 1024;
    facts.faces[0].advances.fill(1024);
    fixture.second.publishTypography(facts);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    const auto handle = fixture.host->paneHandle(22);
    REQUIRE(fixture.host->connection().typography(handle));
    REQUIRE(fixture.host->connection().typography(handle)->rendersBlocks == 1);
    fixture.second.stop();
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    REQUIRE(fixture.host->connection().typography(handle));
    CHECK_EQ(fixture.host->connection().typography(handle)->rendersBlocks, 0u);
    CHECK_EQ(fixture.host->connection().typography(handle)->shaping, 0u);
    REQUIRE(fixture.first.acknowledged());
    fixture.second.start(22, 80, 24);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK_EQ(fixture.host->connection().typography(handle)->rendersBlocks, 0u);
    ++facts.rendererRevision;
    fixture.second.publishTypography(facts);
    fixture.queue->drain();
    fixture.feed(fixture.host->publish(*fixture.factory->wires.back()));
    CHECK(*fixture.host->connection().typography(handle) == facts);
}

TEST(aRestoredPaneLinkRetiresItsPreviewOnTheFirstVerifiedLiveFrame)
{
    struct LinkObserver final : Session::PaneLinkObserver {
        std::vector<Session::FrameReady> frames;
        void frameReady(const Session::FrameReady& frame) override { frames.push_back(frame); }
    } observer;
    Fixture fixture;
    fixture.second.adoptRestoredPreview({ .frameClass = Protocol::Sync::FrameClass::Structural,
        .generation = 12,
        .origin = Session::FrameOrigin::Restored });
    fixture.second.observe(observer);
    fixture.second.setView(Protocol::Sync::ViewState::Dormant);
    fixture.second.pollEvents(fixture.second.clockMicroseconds());
    CHECK(observer.frames.empty());
    fixture.second.setView(Protocol::Sync::ViewState::Live);
    fixture.second.pollEvents(fixture.second.clockMicroseconds());
    REQUIRE(observer.frames.size() == 1);
    CHECK(observer.frames.front().origin == Session::FrameOrigin::Restored);
    CHECK_EQ(observer.frames.front().generation, 12u);
    CHECK(fixture.second.linkState(fixture.second.clockMicroseconds()).phase == Session::LinkPhase::Connecting);
    fixture.second.start(22, 80, 24);
    fixture.second.pollEvents(fixture.second.clockMicroseconds());
    REQUIRE(observer.frames.size() == 2);
    CHECK(observer.frames.back().origin == Session::FrameOrigin::Live);
    CHECK(observer.frames.back().generation > 0);
    fixture.second.pollEvents(fixture.second.clockMicroseconds());
    CHECK_EQ(observer.frames.size(), 2u);
}

TEST(workspacePaneLinkObserversFollowOnlyTheirOwnVerifiedFrames)
{
    struct LinkObserver final : Session::PaneLinkObserver {
        std::vector<Session::FrameReady> frames;
        unsigned facts = 0;
        void frameReady(const Session::FrameReady& frame) override { frames.push_back(frame); }
        void factsChanged() override { ++facts; }
    } firstObserver, secondObserver;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.feed(fixture.host->publish(wire));
    fixture.first.observe(firstObserver);
    fixture.second.observe(secondObserver);
    fixture.first.pollEvents(1'000'000);
    fixture.second.pollEvents(1'000'000);
    REQUIRE(firstObserver.frames.size() == 1);
    REQUIRE(secondObserver.frames.size() == 1);
    CHECK(firstObserver.frames.front().generation > 0);
    fixture.feed(fixture.host->publishPane(11, wire));
    CHECK_EQ(firstObserver.frames.size(), 1u);
    fixture.first.pollEvents(2'000'000);
    fixture.second.pollEvents(2'000'000);
    REQUIRE(firstObserver.frames.size() == 2);
    CHECK(firstObserver.frames.back().frameClass == Protocol::Sync::FrameClass::Settled);
    CHECK_EQ(secondObserver.frames.size(), 1u);
    Protocol::Sync::PaneFacts facts;
    facts.programTitle = "second only";
    fixture.feed(fixture.host->records(22, facts, wire));
    fixture.first.pollEvents(3'000'000);
    fixture.second.pollEvents(3'000'000);
    CHECK_EQ(firstObserver.facts, 0u);
    CHECK_EQ(secondObserver.facts, 1u);
}

TEST(aDormantWorkspacePaneLinkCoalescesFramesWithoutHidingItsSibling)
{
    struct LinkObserver final : Session::PaneLinkObserver {
        std::vector<Session::FrameReady> frames;
        void frameReady(const Session::FrameReady& frame) override { frames.push_back(frame); }
    } firstObserver, secondObserver;
    Fixture fixture;
    fixture.second.start(22, 80, 24);
    Wire& wire = *fixture.factory->wires.back();
    fixture.feed(fixture.host->publish(wire));
    fixture.feed(fixture.host->publish(wire));
    fixture.first.observe(firstObserver);
    fixture.second.observe(secondObserver);
    fixture.first.setView(Protocol::Sync::ViewState::Dormant);
    fixture.first.pollEvents(1'000'000);
    fixture.second.pollEvents(1'000'000);
    CHECK(firstObserver.frames.empty());
    CHECK_EQ(secondObserver.frames.size(), 1u);
    fixture.feed(fixture.host->publishPane(11, wire));
    fixture.first.pollEvents(2'000'000);
    CHECK(firstObserver.frames.empty());
    fixture.first.setView(Protocol::Sync::ViewState::Live);
    fixture.feed(fixture.host->publish(wire));
    fixture.first.pollEvents(3'000'000);
    REQUIRE(firstObserver.frames.size() == 1);
    fixture.first.pollEvents(4'000'000);
    CHECK_EQ(firstObserver.frames.size(), 1u);
    CHECK_EQ(secondObserver.frames.size(), 1u);
}

UBIQUITTY_TEST_MAIN()
